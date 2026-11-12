// pollDesc state machine (after Go's runtime/netpoll.Go) on epoll
// (runtime/netpoll_epoll.Go).
#include "netpoll.hpp"
#include "runtime.hpp"

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace gopp::runtime {

namespace {

// States of poll_desc::rg and ::wg, besides a parked G*.
constexpr uintptr_t pd_nil = 0;
constexpr uintptr_t pd_ready = 1;
constexpr uintptr_t pd_wait = 2;

}  // namespace

struct poll_desc {
    poll_desc* link = nullptr;  // in the free list
    lock_t lock;                // protects the fields below the atomics
    int fd = -1;

    std::atomic<uintptr_t> rg{pd_nil};
    std::atomic<uintptr_t> wg{pd_nil};

    // Read without the lock by poll_wait.
    std::atomic<bool> closing{false};
    std::atomic<bool> rd_expired{false};
    std::atomic<bool> wd_expired{false};

    int64_t rd = 0;  // read deadline, -1 if expired
    int64_t wd = 0;
    uint64_t rseq = 0;  // guards against stale deadline timers
    uint64_t wseq = 0;
    timer rt;
    timer wt;
};

namespace {

int epfd = -1;
int wakefd = -1;
std::atomic<uint32_t> wake_sig{0};
std::atomic<int32_t> waiters{0};

// pollDescs are never freed: epoll may still report a closed one.
lock_t cache_lock;
poll_desc* cache_first = nullptr;

poll_desc* alloc_pd() noexcept {
    cache_lock.lock();
    poll_desc* pd = cache_first;
    if (pd) cache_first = pd->link;
    cache_lock.unlock();
    if (!pd) pd = new poll_desc();
    return pd;
}

void free_pd(poll_desc* pd) noexcept {
    cache_lock.lock();
    pd->link = cache_first;
    cache_first = pd;
    cache_lock.unlock();
}

int checkerr(poll_desc* pd, int mode) noexcept {
    if (pd->closing.load(std::memory_order_acquire)) return poll_err_closing;
    if ((mode == 'r' && pd->rd_expired.load(std::memory_order_acquire)) ||
        (mode == 'w' && pd->wd_expired.load(std::memory_order_acquire)))
        return poll_err_timeout;
    return poll_no_error;
}

bool blockcommit(G* gp, void* arg) noexcept {
    auto* gpp = static_cast<std::atomic<uintptr_t>*>(arg);
    uintptr_t expected = pd_wait;
    bool ok = gpp->compare_exchange_strong(expected, reinterpret_cast<uintptr_t>(gp), std::memory_order_acq_rel);
    if (ok) {
        waiters.fetch_add(1, std::memory_order_relaxed);
        // A fiber's M goes on to poll when idle; a thread has no M.
        if (gp->is_thread) ensure_poller();
    }
    return ok;
}

// Returns true if I/O is ready, false on timeout or close.
bool netpollblock(poll_desc* pd, int mode, bool waitio) noexcept {
    std::atomic<uintptr_t>* gpp = mode == 'w' ? &pd->wg : &pd->rg;
    for (;;) {
        uintptr_t v = pd_ready;
        if (gpp->compare_exchange_strong(v, pd_nil, std::memory_order_acq_rel)) return true;
        v = pd_nil;
        if (gpp->compare_exchange_strong(v, pd_wait, std::memory_order_acq_rel)) break;
        if (v != pd_ready && v != pd_nil) fatal("netpollblock: double wait");
    }
    // Recheck errors after setting pd_wait: a deadline or close may have
    // come in between and must not be missed.
    if (waitio || checkerr(pd, mode) == poll_no_error) park(blockcommit, gpp);
    uintptr_t old = gpp->exchange(pd_nil, std::memory_order_acq_rel);
    if (old > pd_wait) fatal("netpollblock: corrupted state");
    return old == pd_ready;
}

// Returns the G parked on pd for mode, if any, and records readiness when
// ioready is set.
G* netpollunblock(poll_desc* pd, int mode, bool ioready) noexcept {
    std::atomic<uintptr_t>* gpp = mode == 'w' ? &pd->wg : &pd->rg;
    for (;;) {
        uintptr_t old = gpp->load(std::memory_order_acquire);
        if (old == pd_ready) return nullptr;
        if (old == pd_nil && !ioready) return nullptr;
        uintptr_t nv = ioready ? pd_ready : pd_nil;
        if (gpp->compare_exchange_weak(old, nv, std::memory_order_acq_rel)) {
            if (old == pd_wait || old == pd_nil) return nullptr;
            waiters.fetch_sub(1, std::memory_order_relaxed);
            return reinterpret_cast<G*>(old);
        }
    }
}

void deadline_fired(poll_desc* pd, uint64_t seq, bool read) noexcept {
    pd->lock.lock();
    if (read) {
        if (seq != pd->rseq || pd->rd <= 0) {
            pd->lock.unlock();
            return;
        }
        pd->rd = -1;
        pd->rd_expired.store(true, std::memory_order_release);
    } else {
        if (seq != pd->wseq || pd->wd <= 0) {
            pd->lock.unlock();
            return;
        }
        pd->wd = -1;
        pd->wd_expired.store(true, std::memory_order_release);
    }
    G* gp = netpollunblock(pd, read ? 'r' : 'w', false);
    pd->lock.unlock();
    if (gp) ready(gp);
}

}  // namespace

void netpoll_init() noexcept {
    epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0) fatal("netpoll_init: epoll_create1 failed");
    wakefd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (wakefd < 0) fatal("netpoll_init: eventfd failed");
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.ptr = &wakefd;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, wakefd, &ev) < 0) fatal("netpoll_init: epoll_ctl failed");
}

bool netpoll_any_waiters() noexcept { return waiters.load(std::memory_order_relaxed) > 0; }

int32_t netpoll_waiters() noexcept { return waiters.load(std::memory_order_relaxed); }

bool netpoll_add_uring(int fd, void* ring) noexcept {
    // Tagged with the low bit, to tell rings from poll_descs. Edge
    // triggered: uring_reap drains the ring.
    epoll_event ev{};
    ev.events = EPOLLIN | EPOLLET;
    ev.data.ptr = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(ring) | 1);
    return epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) == 0;
}

void netpoll_waiter_add(int32_t delta) noexcept { waiters.fetch_add(delta, std::memory_order_relaxed); }

void netpoll_break() noexcept {
    uint32_t z = 0;
    if (!wake_sig.compare_exchange_strong(z, 1)) return;
    uint64_t one = 1;
    for (;;) {
        ssize_t n = write(wakefd, &one, sizeof(one));
        if (n == sizeof(one) || errno == EAGAIN) return;
        if (errno != EINTR) fatal("netpoll_break: write failed");
    }
}

glist netpoll(int64_t delay) noexcept {
    glist list;
    if (epfd < 0) return list;
    int waitms;
    if (delay < 0) waitms = -1;
    else if (delay == 0) waitms = 0;
    else if (delay < 1000000) waitms = 1;
    else if (delay < 1000000000000LL) waitms = static_cast<int>(delay / 1000000);
    else waitms = 1000000000;

    epoll_event events[128];
    int n;
    for (;;) {
        n = epoll_wait(epfd, events, 128, waitms);
        if (n >= 0) break;
        if (errno != EINTR) fatal("netpoll: epoll_wait failed");
        if (waitms > 0) return list;  // let the caller recompute the delay
    }
    for (int i = 0; i < n; i++) {
        epoll_event& ev = events[i];
        if (ev.events == 0) continue;
        if (ev.data.ptr == &wakefd) {
            if (ev.events != EPOLLIN) fatal("netpoll: eventfd error");
            if (delay != 0) {
                uint64_t buf;
                ssize_t r = read(wakefd, &buf, sizeof(buf));
                (void)r;
                wake_sig.store(0);
            }
            continue;
        }
        if (auto tag = reinterpret_cast<uintptr_t>(ev.data.ptr); tag & 1) {
            uring_reap(reinterpret_cast<void*>(tag & ~uintptr_t(1)), list);
            continue;
        }
        auto* pd = static_cast<poll_desc*>(ev.data.ptr);
        bool r = ev.events & (EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR);
        bool w = ev.events & (EPOLLOUT | EPOLLHUP | EPOLLERR);
        G* rg = r ? netpollunblock(pd, 'r', true) : nullptr;
        G* wg = w ? netpollunblock(pd, 'w', true) : nullptr;
        for (G* gp : {rg, wg}) {
            if (!gp) continue;
            // Threads that aren't Ms wait on a semaphore, not a run queue.
            if (gp->is_thread) ready(gp);
            else list.push_back(gp);
        }
    }
    return list;
}

poll_desc* poll_open(int fd, int* err) noexcept {
    ensure_init();
    poll_desc* pd = alloc_pd();
    pd->lock.lock();
    pd->fd = fd;
    pd->closing.store(false);
    pd->rd_expired.store(false);
    pd->wd_expired.store(false);
    pd->rg.store(pd_nil);
    pd->wg.store(pd_nil);
    pd->rd = pd->wd = 0;
    pd->rseq++;
    pd->wseq++;
    pd->lock.unlock();

    epoll_event ev{};
    ev.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLET;
    ev.data.ptr = pd;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) < 0) {
        *err = errno;
        free_pd(pd);
        return nullptr;
    }
    *err = 0;
    return pd;
}

void poll_close(poll_desc* pd) noexcept {
    if (!pd->closing.load()) fatal("poll_close: close without unblock");
    uintptr_t wg = pd->wg.load(), rg = pd->rg.load();
    if ((wg != pd_nil && wg != pd_ready) || (rg != pd_nil && rg != pd_ready)) fatal("poll_close: blocked read or write");
    epoll_event ev{};
    epoll_ctl(epfd, EPOLL_CTL_DEL, pd->fd, &ev);
    free_pd(pd);
}

int poll_reset(poll_desc* pd, int mode) noexcept {
    int err = checkerr(pd, mode);
    if (err != poll_no_error) return err;
    if (mode == 'r') pd->rg.store(pd_nil);
    else if (mode == 'w') pd->wg.store(pd_nil);
    return poll_no_error;
}

int poll_wait(poll_desc* pd, int mode) noexcept {
    int err = checkerr(pd, mode);
    if (err != poll_no_error) return err;
    while (!netpollblock(pd, mode, false)) {
        err = checkerr(pd, mode);
        if (err != poll_no_error) return err;
        // A timeout that was reset, or a spurious wakeup: wait again.
    }
    return poll_no_error;
}

void poll_set_deadline(poll_desc* pd, int64_t d, int mode) noexcept {
    pd->lock.lock();
    if (pd->closing.load()) {
        pd->lock.unlock();
        return;
    }
    auto arm = [pd](timer& t, uint64_t seq, int64_t when, bool read) {
        del_timer(&t);
        if (when > 0) {
            t.when = when;
            t.period = 0;
            t.arg = pd;
            t.seq = seq;
            t.f = read ? +[](void* arg, uint64_t s) { deadline_fired(static_cast<poll_desc*>(arg), s, true); }
                       : +[](void* arg, uint64_t s) { deadline_fired(static_cast<poll_desc*>(arg), s, false); };
            add_timer(&t);
        }
    };
    if (mode == 'r' || mode == 'r' + 'w') {
        pd->rd = d;
        pd->rseq++;
        pd->rd_expired.store(d < 0, std::memory_order_release);
        arm(pd->rt, pd->rseq, d, true);
    }
    if (mode == 'w' || mode == 'r' + 'w') {
        pd->wd = d;
        pd->wseq++;
        pd->wd_expired.store(d < 0, std::memory_order_release);
        arm(pd->wt, pd->wseq, d, false);
    }
    G* rg = pd->rd < 0 ? netpollunblock(pd, 'r', false) : nullptr;
    G* wg = pd->wd < 0 ? netpollunblock(pd, 'w', false) : nullptr;
    pd->lock.unlock();
    if (rg) ready(rg);
    if (wg) ready(wg);
}

void poll_unblock(poll_desc* pd) noexcept {
    pd->lock.lock();
    if (pd->closing.load()) fatal("poll_unblock: already closing");
    pd->closing.store(true, std::memory_order_release);
    pd->rseq++;
    pd->wseq++;
    G* rg = netpollunblock(pd, 'r', false);
    G* wg = netpollunblock(pd, 'w', false);
    del_timer(&pd->rt);
    del_timer(&pd->wt);
    pd->lock.unlock();
    if (rg) ready(rg);
    if (wg) ready(wg);
}

}  // namespace gopp::runtime
