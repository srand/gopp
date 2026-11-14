// io_uring rings for blocking system calls; see uring.hpp.
#include "uring.hpp"

#include "runtime.hpp"

#include "liburing.hpp"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace gopp::runtime {

namespace {

// A submitted request. g is 0 while its submitter hasn't parked, the G
// once it has, and req_done after the completion: whoever comes second
// wakes or doesn't park.
struct request {
    std::atomic<uintptr_t> g{0};
    int res = 0;
};
constexpr uintptr_t req_done = 1;

// A thread's ring. Only its thread submits to it; completions are taken
// under cq_lock, by the thread or by the poller. Rings are never freed:
// epoll may still report one after its thread exits. They go back to a
// pool for the next thread.
struct ring {
    io_uring r;
    lock_t cq_lock;
    ring* link = nullptr;
};

// 0: not initialized; 1: initializing; 2: in use; 3: unavailable.
std::atomic<int> state{0};
bool supported[IORING_OP_LAST] = {};

lock_t pool_lock;
ring* pool = nullptr;

constexpr unsigned ring_entries = 64;

void init() noexcept {
    if (const char* s = std::getenv("GOPP_IOURING"); s && std::strcmp(s, "0") == 0) return;
    ensureInit();  // the poller, which takes the completions
    io_uring probe_ring;
    io_uring_params p{};
    if (io_uring_queue_init_params(1, &probe_ring, &p) < 0) return;
    // Without these, completions could be dropped, and reads couldn't use
    // the file offset.
    unsigned need = IORING_FEAT_NODROP | IORING_FEAT_RW_CUR_POS;
    if ((p.features & need) == need) {
        if (io_uring_probe* probe = io_uring_get_probe_ring(&probe_ring)) {
            for (int op = 0; op < IORING_OP_LAST; op++) supported[op] = io_uring_opcode_supported(probe, op);
            io_uring_free_probe(probe);
        }
    }
    io_uring_queue_exit(&probe_ring);
    if ((p.features & need) != need) return;
    state.store(2, std::memory_order_release);
}

ring* newRing() noexcept {
    pool_lock.lock();
    ring* rg = pool;
    if (rg) pool = rg->link;
    pool_lock.unlock();
    if (rg) return rg;
    rg = new ring();
    if (io_uring_queue_init(ring_entries, &rg->r, 0) < 0 || !netpollAddUring(rg->r.ring_fd, rg)) {
        // Out of locked memory, or descriptors: this thread does without.
        delete rg;
        return nullptr;
    }
    return rg;
}

// The calling thread's ring, made on first use.
struct thread_ring {
    ring* rg = nullptr;
    bool failed = false;
    ~thread_ring() {
        if (!rg) return;
        pool_lock.lock();
        rg->link = pool;
        pool = rg;
        pool_lock.unlock();
    }
};
thread_local thread_ring tl_ring;

ring* getRing() noexcept {
    if (!tl_ring.rg && !tl_ring.failed) {
        tl_ring.rg = newRing();
        tl_ring.failed = !tl_ring.rg;
    }
    return tl_ring.rg;
}

// Marks req done with res; returns the G to wake, if it had parked.
G* complete(request* req, int res) noexcept {
    req->res = res;
    uintptr_t old = req->g.exchange(req_done, std::memory_order_acq_rel);
    // req may be gone now: its submitter may have seen req_done.
    if (old <= req_done) return nullptr;
    netpollWaiterAdd(-1);
    return reinterpret_cast<G*>(old);
}

// Takes rg's completions. The fibers to wake go to list if there's one;
// otherwise, and for threads, they're readied here.
void reap(ring* rg, glist* list) noexcept {
    rg->cq_lock.lock();
    unsigned head, n = 0;
    io_uring_cqe* cqe;
    G* wake[64];
    int nwake = 0;
    io_uring_for_each_cqe(&rg->r, head, cqe) {
        n++;
        G* gp = complete(static_cast<request*>(io_uring_cqe_get_data(cqe)), cqe->res);
        if (!gp) continue;
        if (list && !gp->is_thread) {
            list->push_back(gp);
        } else {
            wake[nwake++] = gp;
            if (nwake == 64) {
                io_uring_cq_advance(&rg->r, n);
                n = 0;
                for (int i = 0; i < nwake; i++) ready(wake[i]);
                nwake = 0;
            }
        }
    }
    io_uring_cq_advance(&rg->r, n);
    rg->cq_lock.unlock();
    for (int i = 0; i < nwake; i++) ready(wake[i]);
}

bool commit(G* gp, void* arg) noexcept {
    auto* req = static_cast<request*>(arg);
    uintptr_t expected = 0;
    if (!req->g.compare_exchange_strong(expected, reinterpret_cast<uintptr_t>(gp), std::memory_order_acq_rel)) {
        return false;  // completed meanwhile
    }
    netpollWaiterAdd(1);
    // A fiber's M goes on to poll when idle; a thread has no M.
    if (gp->is_thread) ensurePoller();
    return true;
}

}  // namespace

bool uringHas(int opcode) noexcept {
    int s = state.load(std::memory_order_acquire);
    if (s == 0) {
        static std::once_flag once;
        std::call_once(once, [] {
            init();
            if (state.load(std::memory_order_relaxed) != 2) state.store(3, std::memory_order_release);
        });
        s = state.load(std::memory_order_acquire);
    }
    return s == 2 && opcode >= 0 && opcode < IORING_OP_LAST && supported[opcode] && getRing();
}

bool uringWorth(int opcode) noexcept { return hasRunnable() && uringHas(opcode); }

int uringCall(void (*prep)(io_uring_sqe*, void*), void* arg) noexcept {
    ring* rg = getRing();
    request req;
    io_uring_sqe* sqe;
    while (!(sqe = io_uring_get_sqe(&rg->r))) {
        // The submission queue is full of unsubmitted entries: submit them.
        io_uring_submit(&rg->r);
    }
    prep(sqe, arg);
    io_uring_sqe_set_data(sqe, &req);
    for (;;) {
        int r = io_uring_submit(&rg->r);
        if (r >= 0) break;
        if (r == -EINTR) continue;
        if (r == -EBUSY || r == -EAGAIN) {
            // The completion queue overflowed: make room.
            reap(rg, nullptr);
            continue;
        }
        fatal("io_uring_submit failed");
    }
    // A call that didn't have to wait, such as a read from the page cache,
    // has completed already.
    reap(rg, nullptr);
    // Parks unless the completion came in meanwhile; either way req_done
    // has published res when this returns.
    if (req.g.load(std::memory_order_acquire) != req_done) {
        GOPP_STAT(uring_park);
        park(commit, &req, waitreason::io_wait);
    } else {
        GOPP_STAT(uring_done);
    }
    return req.res;
}

void uringReap(void* rg, glist& list) noexcept { reap(static_cast<ring*>(rg), &list); }

}  // namespace gopp::runtime
