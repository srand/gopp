// The scheduler, after Go's runtime/proc.Go.
#include "runtime.hpp"
#include "stack.hpp"

#include <gopp/base.hpp>

#include <sys/mman.h>
#include <signal.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <mutex>
#include <new>
#include <chrono>
#include <string>
#include <thread>
#include <string_view>
#include <vector>

namespace gopp {

void panic(std::string_view msg) noexcept { runtime::dopanic(msg); }

}  // namespace gopp

namespace gopp::runtime {

void fatal(const char* msg) noexcept { dofatal(msg); }

void lock_t::lockSlow() noexcept {
    for (int i = 0; i < 64; i++) {
        uint32_t c = 0;
        if (s_.load(std::memory_order_relaxed) == 0 && s_.compare_exchange_weak(c, 1, std::memory_order_acquire)) return;
        cpuRelax();
    }
    uint32_t c = s_.exchange(2, std::memory_order_acquire);
    while (c != 0) {
        s_.wait(2, std::memory_order_relaxed);
        c = s_.exchange(2, std::memory_order_acquire);
    }
}

namespace {

struct sched_t {
    lock_t lock;

    // Global run queue.
    glist runq;
    std::atomic<int32_t> runqsize{0};

    // Idle Ps and Ms.
    P* pidle = nullptr;
    std::atomic<int32_t> npidle{0};
    M* midle = nullptr;
    int32_t nmidle = 0;
    int64_t mnext = 0;
    M* allm = nullptr;

    std::atomic<int32_t> nmspinning{0};
    // Set when a spinning M found work but no idle P to run it on: Ms about
    // to drop their P become spinning instead (Go's "delicate dance").
    std::atomic<uint32_t> needspinning{0};

    // Network poller: lastpoll is 0 while an M blocks in netpoll.
    std::atomic<int64_t> lastpoll{1};
    std::atomic<int64_t> poll_until{0};

    // Free Gs, shared between Ps.
    lock_t gflock;
    G* gfree = nullptr;
    int32_t ngfree = 0;

    std::atomic<int64_t> ngoroutines{0};
    // Ms running a fiber whose P was taken (M::preempted). sysmon takes no
    // more Ps while gomaxprocs of them run, which bounds the threads that
    // preemption adds.
    std::atomic<int32_t> npreempted{0};
    std::atomic<uint64_t> goidgen{0};

    int32_t gomaxprocs = 1;
    P* allp = nullptr;
};

// Never destroyed: Ms keep running while the process exits.
alignas(sched_t) unsigned char sched_storage[sizeof(sched_t)];
sched_t& sched = *reinterpret_cast<sched_t*>(sched_storage);

std::once_flag init_once;
std::atomic<bool> inited{false};

thread_local M* tls_m = nullptr;
thread_local G* tls_thread_g = nullptr;
thread_local uint64_t tls_rand = 0;

// All Gs, for tracebacks: pushed at the head, never removed, so that a
// crash can walk the list while other threads add to it.
constinit std::atomic<G*> allgs_head{nullptr};
constinit std::atomic<pid_t> main_tid{0};

void allgadd(G* gp) noexcept {
    G* head = allgs_head.load(std::memory_order_relaxed);
    do gp->alllink = head;
    while (!allgs_head.compare_exchange_weak(head, gp, std::memory_order_release, std::memory_order_relaxed));
}

// Marks the pseudo-G of a thread dead when the thread exits, so that
// tracebacks don't signal its thread id, which may be reused.
struct thread_g_reaper {
    G* gp = nullptr;
    ~thread_g_reaper() {
        if (!gp) return;
        gp->tid = 0;
        gp->status.store(gstatus::dead, std::memory_order_release);
        releaseSigaltstack();
    }
};

int ncpu = 1;

// GODEBUG=asyncpreemptoff=1: sysmon doesn't take Ps from long-running
// fibers, and scheduling is purely cooperative.
bool preempt_off = false;

void schedule(M* m) noexcept;

// Marks runtime code that uses the P from a fiber (see G::nopreempt): the
// SIGURG handler doesn't take the P while it runs. The signal fences keep
// the compiler from moving P accesses out of it; the handler runs on the
// same thread, so nothing stronger is needed. gp is nullptr outside fibers.
struct nopreemptGuard {
    G* gp;
    explicit nopreemptGuard(G* g) noexcept : gp(g) {
        if (gp) gp->nopreempt++;
        std::atomic_signal_fence(std::memory_order_seq_cst);
    }
    ~nopreemptGuard() {
        std::atomic_signal_fence(std::memory_order_seq_cst);
        if (gp) gp->nopreempt--;
    }
    nopreemptGuard(const nopreemptGuard&) = delete;
    nopreemptGuard& operator=(const nopreemptGuard&) = delete;
};

// The fiber m runs, for nopreemptGuard.
G* fiberOf(M* m) noexcept { return m ? m->curg : nullptr; }

}  // namespace

// ---------------------------------------------------------------------------
// Thread-local state

[[gnu::noinline]] M* getm() noexcept {
    asm volatile("" ::: "memory");
    return tls_m;
}

[[gnu::noinline]] G* getg() noexcept {
    asm volatile("" ::: "memory");
    if (M* m = tls_m) return m->curg;
    G* gp = tls_thread_g;
    if (!gp) {
        gp = new G();
        gp->is_thread = true;
        gp->tid = gettid();
        gp->status.store(gstatus::running, std::memory_order_relaxed);
        tls_thread_g = gp;
        thread_local thread_g_reaper reaper;
        reaper.gp = gp;
        ensureSigaltstack();
        allgadd(gp);
    }
    return gp;
}

G* allgs() noexcept { return allgs_head.load(std::memory_order_acquire); }

G* getgIfAny() noexcept {
    if (M* m = tls_m) return m->curg;
    return tls_thread_g;
}

bool isMainThread(pid_t t) noexcept { return t != 0 && t == main_tid.load(std::memory_order_relaxed); }

uint32_t fastrand() noexcept {
    // wyrand
    uint64_t& s = tls_rand;
    if (s == 0) s = reinterpret_cast<uintptr_t>(&s) ^ static_cast<uint64_t>(nanotime());
    s += 0xa0761d6478bd642fULL;
    __uint128_t r = static_cast<__uint128_t>(s) * (s ^ 0xe7037ed1a0b428dbULL);
    return static_cast<uint32_t>(static_cast<uint64_t>(r >> 64) ^ static_cast<uint64_t>(r));
}

int64_t nanotime() noexcept {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000000 + ts.tv_nsec;
}

// ---------------------------------------------------------------------------
// Ps and run queues

namespace {

void acquirep(M* m, P* pp) noexcept {
    if (m->p || pp->m.load(std::memory_order_relaxed)) fatal("acquirep: invalid p state");
    m->p = pp;
    pp->schedtick.store(pp->schedtick.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    pp->m.store(m, std::memory_order_release);  // sysmon then sees m->tid
    pp->running.store(true, std::memory_order_relaxed);
}

P* releasep(M* m) noexcept {
    P* pp = m->p;
    if (!pp || pp->m.load(std::memory_order_relaxed) != m) fatal("releasep: invalid p state");
    m->p = nullptr;
    pp->m.store(nullptr, std::memory_order_relaxed);
    pp->running.store(false, std::memory_order_relaxed);
    return pp;
}

// sched.lock must be held.
void pidleput(P* pp) noexcept {
    pp->link = sched.pidle;
    sched.pidle = pp;
    sched.npidle.fetch_add(1, std::memory_order_relaxed);
}

// sched.lock must be held.
P* pidleget() noexcept {
    P* pp = sched.pidle;
    if (pp) {
        sched.pidle = pp->link;
        sched.npidle.fetch_sub(1, std::memory_order_relaxed);
    }
    return pp;
}

// pidleget for an M about to spin. sched.lock must be held.
P* pidlegetSpinning() noexcept {
    P* pp = pidleget();
    if (!pp) sched.needspinning.store(1);
    return pp;
}

void becomeSpinning(M* m) noexcept {
    m->spinning = true;
    sched.nmspinning.fetch_add(1);
    sched.needspinning.store(0);
}

// sched.lock must be held.
void globrunqput(G* gp) noexcept {
    sched.runq.push_back(gp);
    sched.runqsize.fetch_add(1, std::memory_order_relaxed);
}

void globrunqputbatch(glist& list) noexcept {
    int32_t n = list.size;
    while (G* gp = list.pop()) sched.runq.push_back(gp);
    sched.runqsize.fetch_add(n, std::memory_order_relaxed);
}

bool runqputslow(P* pp, G* gp, uint32_t h, uint32_t t) noexcept;

// Puts gp on pp's local queue. With next, gp goes to runnext and runs
// next, ahead of the queue.
void runqput(P* pp, G* gp, bool next) noexcept {
    if (next) {
        G* old = pp->runnext.load(std::memory_order_relaxed);
        while (!pp->runnext.compare_exchange_weak(old, gp, std::memory_order_release, std::memory_order_relaxed)) {
        }
        if (!old) return;
        gp = old;  // kick the old runnext out to the queue
    }
    for (;;) {
        uint32_t h = pp->runqhead.load(std::memory_order_acquire);
        uint32_t t = pp->runqtail.load(std::memory_order_relaxed);
        if (t - h < runq_size) {
            pp->runq[t % runq_size].store(gp, std::memory_order_relaxed);
            pp->runqtail.store(t + 1, std::memory_order_release);
            return;
        }
        if (runqputslow(pp, gp, h, t)) return;
    }
}

// Moves half of the full local queue, and gp, to the global queue.
bool runqputslow(P* pp, G* gp, uint32_t h, uint32_t t) noexcept {
    G* batch[runq_size / 2 + 1];
    uint32_t n = (t - h) / 2;
    if (n != runq_size / 2) fatal("runqputslow: queue is not full");
    for (uint32_t i = 0; i < n; i++) batch[i] = pp->runq[(h + i) % runq_size].load(std::memory_order_relaxed);
    if (!pp->runqhead.compare_exchange_strong(h, h + n, std::memory_order_release, std::memory_order_relaxed)) return false;
    batch[n] = gp;
    glist list;
    for (uint32_t i = 0; i <= n; i++) list.push_back(batch[i]);
    sched.lock.lock();
    globrunqputbatch(list);
    sched.lock.unlock();
    return true;
}

G* runqget(P* pp) noexcept {
    G* next = pp->runnext.load(std::memory_order_relaxed);
    if (next && pp->runnext.compare_exchange_strong(next, nullptr, std::memory_order_acquire)) return next;
    for (;;) {
        uint32_t h = pp->runqhead.load(std::memory_order_acquire);
        uint32_t t = pp->runqtail.load(std::memory_order_relaxed);
        if (t == h) return nullptr;
        G* gp = pp->runq[h % runq_size].load(std::memory_order_relaxed);
        if (pp->runqhead.compare_exchange_weak(h, h + 1, std::memory_order_release, std::memory_order_relaxed)) return gp;
    }
}

bool runqempty(P* pp) noexcept {
    for (;;) {
        uint32_t head = pp->runqhead.load(std::memory_order_acquire);
        uint32_t tail = pp->runqtail.load(std::memory_order_acquire);
        G* next = pp->runnext.load(std::memory_order_acquire);
        if (tail == pp->runqtail.load(std::memory_order_acquire)) return head == tail && next == nullptr;
    }
}

// Grabs half of pp's queue into batch, starting at batch_head.
uint32_t runqgrab(P* pp, std::atomic<G*>* batch, uint32_t batch_head, bool steal_runnext) noexcept {
    for (;;) {
        uint32_t h = pp->runqhead.load(std::memory_order_acquire);
        uint32_t t = pp->runqtail.load(std::memory_order_acquire);
        uint32_t n = t - h;
        n = n - n / 2;
        if (n == 0) {
            if (steal_runnext) {
                if (G* next = pp->runnext.load(std::memory_order_acquire)) {
                    if (pp->running.load(std::memory_order_relaxed)) {
                        // pp is running and likely about to schedule its
                        // runnext; give it a moment rather than thrash.
                        for (int i = 0; i < 50; i++) cpuRelax();
                    }
                    if (!pp->runnext.compare_exchange_strong(next, nullptr, std::memory_order_acq_rel)) continue;
                    batch[batch_head % runq_size].store(next, std::memory_order_relaxed);
                    return 1;
                }
            }
            return 0;
        }
        if (n > runq_size / 2) continue;  // read inconsistent h and t
        for (uint32_t i = 0; i < n; i++) {
            G* gp = pp->runq[(h + i) % runq_size].load(std::memory_order_relaxed);
            batch[(batch_head + i) % runq_size].store(gp, std::memory_order_relaxed);
        }
        if (pp->runqhead.compare_exchange_strong(h, h + n, std::memory_order_acq_rel)) return n;
    }
}

// Steals half of p2's queue into pp's and returns one of the stolen Gs.
G* runqsteal(P* pp, P* p2, bool steal_runnext) noexcept {
    uint32_t t = pp->runqtail.load(std::memory_order_relaxed);
    uint32_t n = runqgrab(p2, pp->runq, t, steal_runnext);
    if (n == 0) return nullptr;
    n--;
    G* gp = pp->runq[(t + n) % runq_size].load(std::memory_order_relaxed);
    if (n == 0) return gp;
    uint32_t h = pp->runqhead.load(std::memory_order_acquire);
    if (t - h + n >= runq_size) fatal("runqsteal: runq overflow");
    pp->runqtail.store(t + n, std::memory_order_release);
    return gp;
}

// Takes a batch from the global queue. sched.lock must be held.
G* globrunqget(P* pp, int32_t max) noexcept {
    int32_t size = sched.runqsize.load(std::memory_order_relaxed);
    if (size == 0) return nullptr;
    int32_t n = size / sched.gomaxprocs + 1;
    if (n > size) n = size;
    if (max > 0 && n > max) n = max;
    if (n > static_cast<int32_t>(runq_size / 2)) n = runq_size / 2;
    sched.runqsize.fetch_sub(n, std::memory_order_relaxed);
    G* gp = sched.runq.pop();
    n--;
    for (; n > 0; n--) runqput(pp, sched.runq.pop(), false);
    return gp;
}

// ---------------------------------------------------------------------------
// Ms

void mstart(M* m) noexcept;

// sched.lock must be held.
void mput(M* m) noexcept {
    m->schedlink = sched.midle;
    sched.midle = m;
    sched.nmidle++;
}

// sched.lock must be held.
M* mget() noexcept {
    M* m = sched.midle;
    if (m) {
        sched.midle = m->schedlink;
        sched.nmidle--;
    }
    return m;
}

void newm(P* pp, bool spinning) noexcept {
    M* m = new M();
    m->nextp = pp;
    m->spinning = spinning;
    sched.lock.lock();
    m->id = sched.mnext++;
    m->alllink = sched.allm;
    sched.allm = m;
    sched.lock.unlock();

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 256 * 1024);
    pthread_t tid;
    int err = pthread_create(
        &tid, &attr,
        [](void* arg) -> void* {
            mstart(static_cast<M*>(arg));
            return nullptr;
        },
        m);
    pthread_attr_destroy(&attr);
    if (err != 0) fatal("newm: pthread_create failed");
}

// Parks the calling M until startm hands it a P.
void stopm(M* m) noexcept {
    if (m->p) fatal("stopm holding p");
    if (m->spinning) fatal("stopm spinning");
    statInc(m->stats.stopm);
    sched.lock.lock();
    mput(m);
    sched.lock.unlock();
    m->park.sleep();
    m->park.clear();
    acquirep(m, m->nextp);
    m->nextp = nullptr;
}

// Runs pp (an idle P if nullptr) on an idle or new M.
void startm(P* pp, bool spinning) noexcept {
    sched.lock.lock();
    if (!pp) {
        pp = pidleget();
        if (!pp) {
            sched.lock.unlock();
            if (spinning && sched.nmspinning.fetch_sub(1) < 1) fatal("startm: negative nmspinning");
            return;
        }
    }
    M* nmp = mget();
    sched.lock.unlock();
    if (!nmp) {
        newm(pp, spinning);
        return;
    }
    if (nmp->spinning || nmp->nextp) fatal("startm: m in bad state");
    nmp->spinning = spinning;
    nmp->nextp = pp;
    GOPP_STAT(startm);
    nmp->park.wakeup();
}

// Wakes an M to run fibers, if there is an idle P and no M is spinning.
void wakep() noexcept {
    int32_t z = 0;
    if (sched.nmspinning.load(std::memory_order_relaxed) != 0 || !sched.nmspinning.compare_exchange_strong(z, 1)) return;
    sched.lock.lock();
    P* pp = pidlegetSpinning();
    sched.lock.unlock();
    if (!pp) {
        if (sched.nmspinning.fetch_sub(1) < 1) fatal("wakep: negative nmspinning");
        return;
    }
    GOPP_STAT(wakep);
    startm(pp, true);
}

// Hands off pp from an M entering a blocking call.
void handoffp(P* pp) noexcept {
    if (!runqempty(pp) || sched.runqsize.load(std::memory_order_relaxed) != 0) {
        startm(pp, false);
        return;
    }
    int32_t z = 0;
    if (sched.nmspinning.load() + sched.npidle.load() == 0 && sched.nmspinning.compare_exchange_strong(z, 1)) {
        startm(pp, true);
        return;
    }
    sched.lock.lock();
    if (sched.runqsize.load(std::memory_order_relaxed) != 0) {
        sched.lock.unlock();
        startm(pp, false);
        return;
    }
    // The last running P, with nobody polling the network: keep an M.
    if (sched.npidle.load() == sched.gomaxprocs - 1 && sched.lastpoll.load() != 0) {
        sched.lock.unlock();
        startm(pp, false);
        return;
    }
    pidleput(pp);
    sched.lock.unlock();
}

void resetspinning(M* m) noexcept {
    if (!m->spinning) fatal("resetspinning: not a spinning m");
    m->spinning = false;
    if (sched.nmspinning.fetch_sub(1) < 1) fatal("findrunnable: negative nmspinning");
    // We were the spinning M and found work; another M may find more.
    wakep();
}

void startIdle(int n) noexcept {
    for (; n > 0 && sched.npidle.load() != 0; n--) startm(nullptr, false);
}

// ---------------------------------------------------------------------------
// Finding work

G* stealWork(M* m, P* pp) noexcept {
    int32_t n = sched.gomaxprocs;
    for (int i = 0; i < 4; i++) {
        bool last = i == 3;
        uint32_t start = fastrand() % n;
        for (int32_t j = 0; j < n; j++) {
            P* p2 = &sched.allp[(start + j) % n];
            if (p2 == pp) continue;
            if (!runqempty(p2)) {
                if (G* gp = runqsteal(pp, p2, last)) return gp;
            }
        }
    }
    (void)m;
    return nullptr;
}

G* findRunnable(M* m) noexcept {
top:
    P* pp = m->p;

    if (int64_t nt = nextTimer(); nt != INT64_MAX) {
        int64_t now = nanotime();
        if (nt <= now) runTimers(now);
    }

    // Check the global queue once in a while for fairness.
    if (pp->schedtick.load(std::memory_order_relaxed) % 61 == 0 && sched.runqsize.load(std::memory_order_relaxed) > 0) {
        sched.lock.lock();
        G* gp = globrunqget(pp, 1);
        sched.lock.unlock();
        if (gp) return gp;
    }

    if (G* gp = runqget(pp)) return gp;

    if (sched.runqsize.load(std::memory_order_relaxed) != 0) {
        sched.lock.lock();
        G* gp = globrunqget(pp, 0);
        sched.lock.unlock();
        if (gp) return gp;
    }

    // Poll the network without blocking.
    if (netpollAnyWaiters() && sched.lastpoll.load() != 0) {
        glist list = netpoll(0);
        statInc(m->stats.poll_nb);
        if (!list.empty()) {
            statInc(m->stats.poll_nb_ready);
            G* gp = list.pop();
            injectglist(list);
            gp->status.store(gstatus::runnable, std::memory_order_relaxed);
            return gp;
        }
    }

    // Steal, unless too many Ms are spinning already.
    int32_t procs = sched.gomaxprocs;
    if (m->spinning || 2 * sched.nmspinning.load() < procs - sched.npidle.load()) {
        if (!m->spinning) {
            becomeSpinning(m);
            statInc(m->stats.spin);
        }
        if (G* gp = stealWork(m, pp)) {
            statInc(m->stats.steal_ok);
            return gp;
        }
        statInc(m->stats.steal_fail);
    }

    // Nothing to do: give up the P.
    sched.lock.lock();
    if (sched.runqsize.load(std::memory_order_relaxed) != 0) {
        G* gp = globrunqget(pp, 0);
        sched.lock.unlock();
        return gp;
    }
    if (!m->spinning && sched.needspinning.load() == 1) {
        // A spinning M found work it couldn't take: spin on its behalf.
        becomeSpinning(m);
        sched.lock.unlock();
        goto top;
    }
    releasep(m);
    pidleput(pp);
    sched.lock.unlock();

    // The delicate dance: drop nmspinning first, then check all sources of
    // work again. In the other order, work submitted between the checks and
    // the decrement would see a spinning M and wake nobody.
    bool was_spinning = m->spinning;
    if (m->spinning) {
        m->spinning = false;
        if (sched.nmspinning.fetch_sub(1) < 1) fatal("findrunnable: negative nmspinning");

        sched.lock.lock();
        if (sched.runqsize.load(std::memory_order_relaxed) != 0) {
            if (P* p2 = pidlegetSpinning()) {
                G* gp = globrunqget(p2, 0);
                if (!gp) fatal("global runq empty with non-zero runqsize");
                sched.lock.unlock();
                acquirep(m, p2);
                becomeSpinning(m);
                return gp;
            }
        }
        sched.lock.unlock();

        for (int32_t i = 0; i < procs; i++) {
            if (!runqempty(&sched.allp[i])) {
                sched.lock.lock();
                P* p2 = pidlegetSpinning();
                sched.lock.unlock();
                if (p2) {
                    acquirep(m, p2);
                    becomeSpinning(m);
                    goto top;
                }
                break;
            }
        }
        if (int64_t nt = nextTimer(); nt != INT64_MAX && nt <= nanotime()) {
            sched.lock.lock();
            P* p2 = pidlegetSpinning();
            sched.lock.unlock();
            if (p2) {
                acquirep(m, p2);
                becomeSpinning(m);
                goto top;
            }
        }
    }

    // Block in the network poller, unless another M already does.
    int64_t poll_until = nextTimer();
    if ((netpollAnyWaiters() || poll_until != INT64_MAX) && sched.lastpoll.exchange(0) != 0) {
        sched.poll_until.store(poll_until == INT64_MAX ? 0 : poll_until);
        int64_t delay = -1;
        if (poll_until != INT64_MAX) {
            delay = poll_until - nanotime();
            if (delay < 0) delay = 0;
        }
        glist list = netpoll(delay);
        statInc(m->stats.poll_block);
        if (!list.empty()) statInc(m->stats.poll_block_ready);
        sched.poll_until.store(0);
        sched.lastpoll.store(nanotime());

        sched.lock.lock();
        P* p2 = pidleget();
        sched.lock.unlock();
        if (!p2) {
            injectglist(list);
        } else {
            acquirep(m, p2);
            if (!list.empty()) {
                G* gp = list.pop();
                injectglist(list);
                if (was_spinning) becomeSpinning(m);
                gp->status.store(gstatus::runnable, std::memory_order_relaxed);
                return gp;
            }
            if (was_spinning) becomeSpinning(m);
            goto top;
        }
    } else if (poll_until != INT64_MAX) {
        // The poller may sleep past our timer: wake it to recompute.
        int64_t pu = sched.poll_until.load();
        if (pu == 0 || pu > poll_until) netpollBreak();
    }

    stopm(m);
    goto top;
}

// ---------------------------------------------------------------------------
// Running fibers

void gfput(P* pp, G* gp) noexcept {
    gp->fn = nullptr;
    gp->schedlink = pp->gfree;
    pp->gfree = gp;
    pp->gfreecnt++;
    if (pp->gfreecnt >= 64) {
        sched.gflock.lock();
        while (pp->gfreecnt >= 32) {
            G* g1 = pp->gfree;
            pp->gfree = g1->schedlink;
            pp->gfreecnt--;
            g1->schedlink = sched.gfree;
            sched.gfree = g1;
            sched.ngfree++;
        }
        sched.gflock.unlock();
    }
}

G* gfget(P* pp) noexcept {
    if (!pp->gfree && sched.gfree) {
        sched.gflock.lock();
        while (pp->gfreecnt < 32 && sched.gfree) {
            G* g1 = sched.gfree;
            sched.gfree = g1->schedlink;
            sched.ngfree--;
            g1->schedlink = pp->gfree;
            pp->gfree = g1;
            pp->gfreecnt++;
        }
        sched.gflock.unlock();
    }
    G* gp = pp->gfree;
    if (gp) {
        pp->gfree = gp->schedlink;
        pp->gfreecnt--;
    }
    return gp;
}

// Not inlined: tracebacks stop at its frame (gentry_pc).
[[gnu::noinline]] boost::context::fiber gentry(G* gp, boost::context::fiber&& caller) {
    gp->caller = std::move(caller);
    // The switch here held nopreempt (see mcall); newproc set it to 1.
    std::atomic_signal_fence(std::memory_order_seq_cst);
    gp->nopreempt--;
    // An exception that escapes is reported where it was thrown.
    runFiberFn(gp);
    gp->fn = nullptr;  // destroy captured state while still on the fiber
    gp->nopreempt++;   // held across the switch, as in mcall
    std::atomic_signal_fence(std::memory_order_seq_cst);
    // execute frees the G to its M's P: get one back if it was taken.
    if (!getm()->p) exitsyscall();
    gp->action = gaction::exit;
    return std::move(gp->caller);
}

// Switches to gp until it parks, yields or exits. Runs on the M's own stack.
void execute(M* m, G* gp) noexcept {
    for (;;) {
        m->curg = gp;
        gp->m = m;
        gp->action = gaction::none;
        gp->status.store(gstatus::running, std::memory_order_relaxed);
        m->p->schedtick.store(m->p->schedtick.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);

        gp->ctx = std::move(gp->ctx).resume();

        m->curg = nullptr;
        // The P is used below, and nopreempt, still held from the switch,
        // keeps the handler off it; the fence keeps curg's clearing first.
        std::atomic_signal_fence(std::memory_order_seq_cst);
        gp->m = nullptr;
        switch (gp->action) {
        case gaction::park:
            if (gp->unlockf && !gp->unlockf(gp, gp->unlockarg)) {
                gp->status.store(gstatus::running, std::memory_order_relaxed);
                continue;  // resume it at once
            }
            return;
        case gaction::yield:
            gp->status.store(gstatus::runnable, std::memory_order_relaxed);
            sched.lock.lock();
            globrunqput(gp);
            sched.lock.unlock();
            return;
        case gaction::exit:
            gp->status.store(gstatus::dead, std::memory_order_relaxed);
            sched.ngoroutines.fetch_sub(1, std::memory_order_relaxed);
            gfput(m->p, gp);
            return;
        case gaction::exitsyscall: {
            // No P was free after a blocking call. One may have become free
            // since, its M having seen an empty global queue: try again
            // under sched.lock, which that M held to check the queue and
            // give up its P (Go's exitsyscall0). Otherwise queue gp for
            // the next M that looks.
            sched.lock.lock();
            P* pp = pidleget();
            if (!pp) {
                gp->status.store(gstatus::runnable, std::memory_order_relaxed);
                globrunqput(gp);
            }
            sched.lock.unlock();
            if (pp) {
                acquirep(m, pp);
                continue;  // resume it at once
            }
            stopm(m);
            return;
        }
        case gaction::none:
            fatal("execute: fiber returned without an action");
        }
    }
}

void schedule(M* m) noexcept {
    for (;;) {
        G* gp = findRunnable(m);
        statInc(m->stats.schedules);
        if (m->spinning) resetspinning(m);
        execute(m, gp);
    }
}

void mstart(M* m) noexcept {
    tls_m = m;
    m->tid = gettid();
    ensureSigaltstack();
    // The thread inherits its creator's signal mask: a program that blocks
    // signals in main would otherwise block preemption (Go's minit).
    sigset_t urg;
    sigemptyset(&urg);
    sigaddset(&urg, SIGURG);
    pthread_sigmask(SIG_UNBLOCK, &urg, nullptr);
    acquirep(m, m->nextp);
    m->nextp = nullptr;
    schedule(m);
}

// Switches from the running fiber to its M's scheduler context.
// The switch holds nopreempt: from here until the scheduler context has
// cleared m->curg, and from when execute sets m->curg until the fiber runs
// again, the code that runs is the scheduler's, which uses the P.
void mcall(G* gp, gaction action) noexcept {
    gp->nopreempt++;
    std::atomic_signal_fence(std::memory_order_seq_cst);
    gp->action = action;
    // Where a traceback of the switched-out fiber starts.
    gp->sched_fp = reinterpret_cast<uintptr_t>(__builtin_frame_address(0));
    gp->caller = std::move(gp->caller).resume();
    std::atomic_signal_fence(std::memory_order_seq_cst);
    gp->nopreempt--;
}

// Preemption: sysmon takes the P from a fiber that has run for
// force_preempt while other work waits, and hands it to another M. The
// fiber keeps running on its thread without a P, and gets one back when it
// next parks, yields or returns from a blocking call. It is never switched
// out at an arbitrary instruction, so errno, thread_local values and locks
// stay valid.
constexpr int64_t force_preempt = 10'000'000;  // ns
// When to ask again after the handler declined because the fiber was
// inside the runtime, which it leaves soon.
constexpr int64_t preempt_retry = 1'000'000;  // ns

std::atomic<uint64_t> sysmon_retakes{0};

// Whether fibers wait that pp's M doesn't get to: pp's own queue, or the
// global one while no P is idle to take them.
bool workWaiting(P* pp) noexcept {
    return !runqempty(pp) ||
           (sched.runqsize.load(std::memory_order_relaxed) > 0 && sched.npidle.load(std::memory_order_relaxed) == 0);
}

// sysmon's view of a P: the schedtick it last saw and when.
struct sysmon_pdesc {
    uint32_t schedtick = 0;
    int64_t schedwhen = 0;
};

// Asks the M of each P that has run one fiber for force_preempt, while
// work waits, to give the P up. Returns whether it asked any.
bool retake(std::vector<sysmon_pdesc>& pd, int64_t now) noexcept {
    bool asked = false;
    // Each request answered adds a thread that runs without a P; count the
    // ones whose handlers haven't run yet (a thread waiting for a CPU) too.
    int32_t budget = sched.gomaxprocs - sched.npreempted.load(std::memory_order_relaxed);
    for (int32_t i = 0; i < sched.gomaxprocs; i++) {
        M* mp = sched.allp[i].m.load(std::memory_order_acquire);
        if (mp && mp->preempt_p.load(std::memory_order_relaxed)) budget--;
    }
    for (int32_t i = 0; i < sched.gomaxprocs; i++) {
        P* pp = &sched.allp[i];
        uint32_t t = pp->schedtick.load(std::memory_order_relaxed);
        M* mp = pp->m.load(std::memory_order_acquire);
        if (!pp->running.load(std::memory_order_relaxed) || !mp || t != pd[i].schedtick) {
            pd[i] = {t, now};
            continue;
        }
        if (budget <= 0) continue;
        bool declined = mp->preempt_declined.load(std::memory_order_relaxed) == (uint64_t{1} << 32 | t);
        if (now - pd[i].schedwhen < (declined ? preempt_retry : force_preempt) || !workWaiting(pp)) continue;
        mp->preempt_tick.store(t, std::memory_order_relaxed);
        mp->preempt_p.store(pp, std::memory_order_release);
        ::tgkill(::getpid(), mp->tid, SIGURG);
        pd[i].schedwhen = now;  // at most one request per force_preempt
        budget--;
        asked = true;
    }
    return asked;
}

// Hands off the Ps that SIGURG handlers gave up. Returns how many.
int handoffRetaken(std::vector<P*>& ps) noexcept {
    ps.clear();
    sched.lock.lock();
    for (M* m = sched.allm; m; m = m->alllink) {
        if (P* pp = m->retaken.exchange(nullptr, std::memory_order_acquire)) ps.push_back(pp);
    }
    sched.lock.unlock();
    for (P* pp : ps) handoffp(pp);
    sysmon_retakes.fetch_add(ps.size(), std::memory_order_relaxed);
    return static_cast<int>(ps.size());
}

// Times sysmon found the network unpolled with fibers or threads waiting
// on it, and polled it (schedtrace's sysmon_poll).
std::atomic<uint64_t> sysmon_polls{0};
std::atomic<uint64_t> sysmon_timers{0};

// sysmon (after Go's): a thread outside the scheduler that polls the
// network when nobody has for 10ms while something waits on it. The
// scheduler keeps an M polling whenever it idles with waiters, so this is a
// safety net; schedtrace counts each time it was needed.
void sysmon() {
    constexpr int64_t force_poll = 10'000'000;  // ns
    int64_t delay = 20'000;                     // ns; backs off to 10ms
    int idle = 0;
    std::vector<sysmon_pdesc> pd(sched.gomaxprocs);
    std::vector<P*> retaken;
    retaken.reserve(sched.gomaxprocs);
    for (;;) {
        std::this_thread::sleep_for(std::chrono::nanoseconds(delay));
        int64_t now = nanotime();
        int64_t last = sched.lastpoll.load();
        bool polled = false;
        if (netpollAnyWaiters() && last != 0 && last + force_poll < now &&
            sched.lastpoll.compare_exchange_strong(last, now)) {
            glist list = netpoll(0);
            if (!list.empty()) {
                sysmon_polls.fetch_add(1, std::memory_order_relaxed);
                injectglist(list);
                polled = true;
            }
        }
        // Timers are run by Ms that schedule. When every P runs a fiber
        // that doesn't park, as a loop of calls that complete at once does
        // (fibers aren't preempted), they'd starve: a sleeping thread
        // wouldn't wake. Run timers that are late here; their functions
        // don't block.
        constexpr int64_t timer_slack = 1'000'000;  // ns
        if (int64_t nt = nextTimer(); nt != INT64_MAX && nt + timer_slack < now) {
            runTimers(now);
            sysmon_timers.fetch_add(1, std::memory_order_relaxed);
            polled = true;
        }
        // Ps given up since the last round, then new requests: a request
        // keeps the delay short, so that its answer is handed off soon.
        if (!preempt_off) {
            if (handoffRetaken(retaken) > 0) polled = true;
            if (retake(pd, now)) polled = true;
        }
        if (polled) {
            idle = 0;
            delay = 20'000;
        } else if (++idle > 50) {
            delay = std::min<int64_t>(delay * 2, force_poll);
        }
    }
}

// GOPP_SCHEDTRACE=<ms>: prints the scheduler counters per second every ms.
void schedtrace(int ms) {
    constexpr int nfields = 0
#define GOPP_COUNT(name) +1
        GOPP_SCHED_STATS(GOPP_COUNT)
#undef GOPP_COUNT
        ;
    static const char* names[] = {
#define GOPP_NAME(name) #name,
        GOPP_SCHED_STATS(GOPP_NAME)
#undef GOPP_NAME
    };
    uint64_t prev[nfields] = {};
    int64_t last = nanotime();
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        uint64_t cur[nfields] = {};
        int nm = 0;
        sched.lock.lock();
        for (M* m = sched.allm; m; m = m->alllink) {
            int i = 0;
            nm++;
#define GOPP_SUM(name) cur[i++] += m->stats.name.load(std::memory_order_relaxed);
            GOPP_SCHED_STATS(GOPP_SUM)
#undef GOPP_SUM
        }
        int32_t idle = sched.npidle.load(), spinning = sched.nmspinning.load(), nmidle = sched.nmidle;
        sched.lock.unlock();
        int64_t now = nanotime();
        double secs = static_cast<double>(now - last) / 1e9;
        last = now;
        std::string line = "SCHED threads=" + std::to_string(nm) + " idlep=" + std::to_string(idle) +
                           " spinning=" + std::to_string(spinning) + " idlem=" + std::to_string(nmidle) +
                           " runq=" + std::to_string(sched.runqsize.load()) +
                           " pollwaiters=" + std::to_string(netpollWaiters()) +
                           " polling=" + std::to_string(sched.lastpoll.load() == 0) +
                           " sysmon_poll=" + std::to_string(sysmon_polls.load(std::memory_order_relaxed)) +
                           " sysmon_timers=" + std::to_string(sysmon_timers.load(std::memory_order_relaxed)) +
                           " retake=" + std::to_string(sysmon_retakes.load(std::memory_order_relaxed));
        for (int i = 0; i < nfields; i++) {
            line += " " + std::string(names[i]) + "=" + std::to_string(static_cast<uint64_t>((cur[i] - prev[i]) / secs));
            prev[i] = cur[i];
        }
        std::fprintf(stderr, "%s\n", line.c_str());
    }
}

void init() noexcept {
    new (&sched) sched_t();
    ncpu = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
    cpu_set_t set;
    if (sched_getaffinity(0, sizeof(set), &set) == 0) ncpu = std::max(1, CPU_COUNT(&set));
    int procs = ncpu;
    if (const char* s = std::getenv("GOMAXPROCS")) {
        int n = std::atoi(s);
        if (n > 0) procs = n;
    }
    sched.gomaxprocs = procs;
    sched.allp = new P[procs];
    for (int i = procs - 1; i >= 0; i--) {
        sched.allp[i].id = i;
        pidleput(&sched.allp[i]);
    }
    if (const char* s = std::getenv("GODEBUG")) {
        // Comma-separated name=value settings; the last one wins, as in Go.
        std::string_view v(s);
        while (!v.empty()) {
            std::string_view kv = v.substr(0, v.find(','));
            v.remove_prefix(std::min(v.size(), kv.size() + 1));
            if (kv.starts_with("asyncpreemptoff=")) preempt_off = kv == "asyncpreemptoff=1";
        }
    }
    netpollInit();
    inited.store(true, std::memory_order_release);
    std::thread(sysmon).detach();
    if (const char* s = std::getenv("GOPP_SCHEDTRACE")) {
        int ms = std::atoi(s);
        if (ms > 0) std::thread(schedtrace, ms).detach();
    }
}

}  // namespace

void wakeNetpoller(int64_t when) noexcept {
    if (sched.lastpoll.load() == 0) {
        // An M is blocked in netpoll; wake it if it would sleep past when.
        int64_t pu = sched.poll_until.load();
        if (pu == 0 || pu > when) netpollBreak();
    } else {
        wakep();
    }
}

void ensurePoller() noexcept {
    // Nobody blocks in netpoll: wake an M, which polls once it finds no
    // other work.
    if (sched.lastpoll.load() != 0) wakep();
}

void ensureInit() noexcept {
    if (!inited.load(std::memory_order_acquire)) std::call_once(init_once, init);
}

bool hasRunnable() noexcept {
    M* m = getm();
    nopreemptGuard g{fiberOf(m)};
    if (!m || !m->p) return false;
    return !runqempty(m->p) || sched.runqsize.load(std::memory_order_relaxed) != 0;
}

namespace {

// The signal handlers are installed before main, as Go's runtime does
// before main.main: in a program that links the runtime, a write to a
// broken pipe fails with EPIPE from the start, and crashes print
// tracebacks. The main thread gets its pseudo-G here, so that tracebacks
// list it.
[[gnu::constructor]] void installSignalHandlers() {
    main_tid.store(gettid(), std::memory_order_relaxed);
    initsig();
    getg();
}

}  // namespace

// ---------------------------------------------------------------------------
// Public and runtime-internal entry points

uintptr_t gentryPc() noexcept { return reinterpret_cast<uintptr_t>(&gentry); }

// Not inlined, so that its return address is in the function that
// started the fiber (gopp::go is always inlined).
[[gnu::noinline]] void newproc(std::move_only_function<void()> fn) {
    ensureInit();
    uintptr_t gopc = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
    M* m = getm();
    nopreemptGuard g{fiberOf(m)};
    P* pp = m ? m->p : nullptr;
    G* gp = pp ? gfget(pp) : nullptr;
    bool fresh = !gp;
    if (fresh) gp = new G();
    gp->nopreempt = 1;  // gentry drops it once the fiber runs
    gp->fn = std::move(fn);
    gp->goid = sched.goidgen.fetch_add(1, std::memory_order_relaxed) + 1;
    gp->sched_fp = 0;
    gp->gopc = gopc;
    gp->parent_goid = m && m->curg ? m->curg->goid : 0;
    gp->wait = waitreason::none;
    gp->ctx = boost::context::fiber(std::allocator_arg, stack_allocator{gp},
                                    [gp](boost::context::fiber&& caller) { return gentry(gp, std::move(caller)); });
    gp->status.store(gstatus::runnable, std::memory_order_relaxed);
    sched.ngoroutines.fetch_add(1, std::memory_order_relaxed);
    if (fresh) allgadd(gp);
    if (pp) {
        runqput(pp, gp, true);
    } else {
        sched.lock.lock();
        globrunqput(gp);
        sched.lock.unlock();
    }
    wakep();
}

void park(unlock_fn unlockf, void* arg, waitreason reason) noexcept {
    G* gp = getg();
    if (!gp) fatal("park on scheduler stack");
    nopreemptGuard g{gp->is_thread ? nullptr : gp};
    if (!gp->is_thread && !gp->m->p) exitsyscall();  // parking inside a blocking call
    gp->wait = reason;
    gp->status.store(gstatus::waiting, std::memory_order_relaxed);
    if (gp->is_thread) {
        if (unlockf && !unlockf(gp, arg)) {
            gp->status.store(gstatus::running, std::memory_order_relaxed);
            return;
        }
        while (gp->thread_sema.exchange(0, std::memory_order_acquire) == 0) gp->thread_sema.wait(0, std::memory_order_acquire);
        gp->status.store(gstatus::running, std::memory_order_relaxed);
        return;
    }
    gp->unlockf = unlockf;
    gp->unlockarg = arg;
    mcall(gp, gaction::park);
}

void parkUnlock(lock_t* l, waitreason reason) noexcept {
    park(
        [](G*, void* arg) {
            static_cast<lock_t*>(arg)->unlock();
            return true;
        },
        l, reason);
}

void ready(G* gp) noexcept {
    if (gp->is_thread) {
        gp->thread_sema.store(1, std::memory_order_release);
        gp->thread_sema.notify_one();
        return;
    }
    gp->status.store(gstatus::runnable, std::memory_order_relaxed);
    M* m = getm();
    nopreemptGuard g{fiberOf(m)};
    if (m && m->p) {
        runqput(m->p, gp, true);
    } else {
        sched.lock.lock();
        globrunqput(gp);
        sched.lock.unlock();
    }
    wakep();
}

void injectglist(glist& list) noexcept {
    if (list.empty()) return;
    for (G* gp = list.head; gp; gp = gp->schedlink) gp->status.store(gstatus::runnable, std::memory_order_relaxed);
    M* m = getm();
    nopreemptGuard g{fiberOf(m)};
    if (!m || !m->p) {
        int n = list.size;
        sched.lock.lock();
        globrunqputbatch(list);
        sched.lock.unlock();
        startIdle(n);
        return;
    }
    // Give idle Ps a share through the global queue, keep the rest.
    int32_t npidle = sched.npidle.load();
    glist globq;
    while (globq.size < npidle && !list.empty()) globq.push_back(list.pop());
    if (!globq.empty()) {
        int n = globq.size;
        sched.lock.lock();
        globrunqputbatch(globq);
        sched.lock.unlock();
        startIdle(n);
    }
    while (G* gp = list.pop()) runqput(m->p, gp, false);
    wakep();
}

void Gosched() {
    G* gp = getg();
    if (!gp || gp->is_thread) {
        sched_yield();
        return;
    }
    nopreemptGuard g{gp};
    if (!gp->m->p) exitsyscall();
    mcall(gp, gaction::yield);
}

void entersyscall() noexcept {
    M* m = getm();
    nopreemptGuard g{fiberOf(m)};
    if (!m || !m->p || !m->curg) return;
    P* pp = releasep(m);
    handoffp(pp);
}

void exitsyscall() noexcept {
    M* m = getm();
    nopreemptGuard g{fiberOf(m)};
    if (!m || m->p || !m->curg) return;
    if (m->preempted) {
        m->preempted = false;
        sched.npreempted.fetch_sub(1, std::memory_order_relaxed);
    }
    sched.lock.lock();
    P* pp = pidleget();
    sched.lock.unlock();
    if (pp) {
        acquirep(m, pp);
        return;
    }
    mcall(m->curg, gaction::exitsyscall);
}

// The SIGURG handler's part, on the thread of the M sysmon asked: gives up
// the P if the M still runs the same fiber on it and the fiber is outside
// the runtime. It only moves the P to m->retaken; sysmon hands it off.
void preemptSignal() noexcept {
    M* m = tls_m;
    if (!m) return;
    P* pp = m->preempt_p.exchange(nullptr, std::memory_order_acquire);
    if (!pp || pp != m->p) return;
    if (pp->schedtick.load(std::memory_order_relaxed) != m->preempt_tick.load(std::memory_order_relaxed)) return;
    G* gp = m->curg;
    if (!gp) return;
    if (gp->nopreempt != 0) {
        m->preempt_declined.store(uint64_t{1} << 32 | pp->schedtick.load(std::memory_order_relaxed),
                                  std::memory_order_relaxed);
        return;
    }
    // A P given up earlier that sysmon hasn't collected yet: storing
    // another would lose that one.
    if (m->retaken.load(std::memory_order_relaxed)) return;
    m->p = nullptr;
    pp->m.store(nullptr, std::memory_order_relaxed);
    pp->running.store(false, std::memory_order_relaxed);
    m->preempted = true;
    sched.npreempted.fetch_add(1, std::memory_order_relaxed);
    m->retaken.store(pp, std::memory_order_release);
}

int GOMAXPROCS() noexcept {
    ensureInit();
    return sched.gomaxprocs;
}

int NumCPU() noexcept {
    ensureInit();
    return ncpu;
}

int64 NumGoroutine() noexcept { return sched.ngoroutines.load(std::memory_order_relaxed); }

uint64 goid() noexcept {
    M* m = getm();
    return m && m->curg ? m->curg->goid : 0;
}

}  // namespace gopp::runtime
