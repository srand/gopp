// Internal runtime structures, after Go's runtime2.Go.
//
//   G - a fiber ("goroutine"): its context, stack and wait state.
//   M - an OS thread. Its native stack acts as Go's g0: the scheduler loop
//       runs there, and fibers switch to it to park, yield or exit.
//   P - a processor: the right to run fibers, with a local run queue.
//       There are GOMAXPROCS Ps; an M must hold a P to run fibers.
#pragma once

#include <gopp/runtime.hpp>

#include <boost/context/fiber.hpp>

#include <atomic>
#include <cstdint>
#include <functional>

namespace gopp::runtime {

// ---------------------------------------------------------------------------
// Architecture

inline void cpu_relax() noexcept {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
    asm volatile("yield" ::: "memory");
#elif defined(__riscv)
    asm volatile(".insn i 0x0F, 0, x0, x0, 0x010" ::: "memory");  // pause
#else
    asm volatile("" ::: "memory");
#endif
}

[[noreturn]] void fatal(const char* msg) noexcept;

// ---------------------------------------------------------------------------
// Locks for runtime internals. They block the thread, are held only for
// short sections and never across a fiber switch.

class lock_t {
public:
    void lock() noexcept {
        uint32_t c = 0;
        if (s_.compare_exchange_strong(c, 1, std::memory_order_acquire)) return;
        lock_slow();
    }

    void unlock() noexcept {
        if (s_.exchange(0, std::memory_order_release) == 2) s_.notify_one();
    }

private:
    void lock_slow() noexcept;
    std::atomic<uint32_t> s_{0};  // 0 unlocked, 1 locked, 2 locked with waiters
};

// One-shot wakeup of a sleeping thread (Go's note).
class note {
public:
    void sleep() noexcept {
        while (key_.load(std::memory_order_acquire) == 0) key_.wait(0, std::memory_order_acquire);
    }
    void wakeup() noexcept {
        key_.store(1, std::memory_order_release);
        key_.notify_one();
    }
    void clear() noexcept { key_.store(0, std::memory_order_relaxed); }

private:
    std::atomic<uint32_t> key_{0};
};

// ---------------------------------------------------------------------------
// G, M, P

struct M;
struct P;
struct sudog;

enum class gstatus : uint32_t { idle, runnable, running, waiting, dead };
enum class gaction : uint8_t { none, park, yield, exit, exitsyscall };

using unlock_fn = bool (*)(struct G* gp, void* arg);

struct G {
    boost::context::fiber ctx;     // the suspended fiber, while not running
    boost::context::fiber caller;  // the M's scheduler context, while running
    std::move_only_function<void()> fn;

    std::atomic<gstatus> status{gstatus::idle};
    gaction action = gaction::none;
    unlock_fn unlockf = nullptr;
    void* unlockarg = nullptr;

    M* m = nullptr;
    G* schedlink = nullptr;
    uint64_t goid = 0;

    // Channels and select (Go's g.param, g.selectDone, g.waiting).
    void* param = nullptr;
    std::atomic<uint32_t> select_done{0};
    sudog* waiting = nullptr;

    // A pseudo-G that stands for a thread the runtime didn't start. Parking
    // it blocks the thread on thread_sema.
    bool is_thread = false;
    std::atomic<uint32_t> thread_sema{0};
};

constexpr uint32_t runq_size = 256;

struct P {
    int32_t id = 0;
    bool running = false;
    M* m = nullptr;
    P* link = nullptr;
    uint32_t schedtick = 0;

    // Local run queue, lock-free: the owner puts at the tail, anyone takes
    // from the head.
    std::atomic<uint32_t> runqhead{0};
    std::atomic<uint32_t> runqtail{0};
    std::atomic<G*> runq[runq_size] = {};
    std::atomic<G*> runnext{nullptr};

    // Free Gs, for reuse.
    G* gfree = nullptr;
    int32_t gfreecnt = 0;
};

// Scheduler counters, per M so that counting doesn't share cache lines.
// Printed by GOPP_SCHEDTRACE=<ms>.
struct sched_stats {
#define GOPP_SCHED_STATS(X) \
    X(schedules) X(steal_ok) X(steal_fail) X(poll_nb) X(poll_nb_ready) X(poll_block) X(poll_block_ready) \
    X(stopm) X(startm) X(wakep) X(spin) X(read) X(read_eagain) X(write) X(write_eagain) \
    X(uring_done) X(uring_park)
#define GOPP_FIELD(name) std::atomic<uint64_t> name{0};
    GOPP_SCHED_STATS(GOPP_FIELD)
#undef GOPP_FIELD
};

inline void stat_inc(std::atomic<uint64_t>& c, uint64_t n = 1) noexcept {
    c.store(c.load(std::memory_order_relaxed) + n, std::memory_order_relaxed);  // single writer
}

struct M {
    int64_t id = 0;
    sched_stats stats;
    M* alllink = nullptr;
    P* p = nullptr;
    P* nextp = nullptr;
    G* curg = nullptr;
    bool spinning = false;
    note park;
    M* schedlink = nullptr;
    uint64_t fastrand = 0;
};

// A list of Gs linked through schedlink.
struct glist {
    G* head = nullptr;
    G* tail = nullptr;
    int32_t size = 0;

    bool empty() const noexcept { return head == nullptr; }
    void push_back(G* gp) noexcept {
        gp->schedlink = nullptr;
        if (tail) tail->schedlink = gp;
        else head = gp;
        tail = gp;
        size++;
    }
    G* pop() noexcept {
        G* gp = head;
        if (gp) {
            head = gp->schedlink;
            if (!head) tail = nullptr;
            size--;
        }
        return gp;
    }
};

// ---------------------------------------------------------------------------
// Scheduler interface used by the rest of the runtime.

// The M of the calling thread, or nullptr on threads the runtime didn't
// start. Never inlined, so that compilers don't cache it across fiber
// switches, which can move the caller to another thread.
M* getm() noexcept;

// Counts on the calling M's stats, if it is an M.
#define GOPP_STAT(field) do { if (::gopp::runtime::M* m_ = ::gopp::runtime::getm()) ::gopp::runtime::stat_inc(m_->stats.field); } while (0)

// The calling fiber; a pseudo-G on foreign threads; nullptr on an M's
// scheduler context.
G* getg() noexcept;

// Parks the calling fiber. After the switch to the scheduler, unlockf(gp,
// arg) runs there; if it returns false the fiber resumes at once. This is
// how a fiber releases the lock of a wait queue only once it can no longer
// run, so that a concurrent ready() can't resume it while it's running.
void park(unlock_fn unlockf, void* arg) noexcept;

// Parks the calling fiber after unlocking l.
void park_unlock(lock_t* l) noexcept;

// Makes a parked fiber runnable.
void ready(G* gp) noexcept;

// Makes a list of fibers runnable, from a scheduler context.
void injectglist(glist& list) noexcept;

uint32_t fastrand() noexcept;

void ensure_init() noexcept;

// Whether the calling fiber's P has other fibers to run: in its run queue,
// or the global one. False outside fibers.
bool has_runnable() noexcept;

// ---------------------------------------------------------------------------
// Timers (timer.cpp)

struct timer {
    int64_t when = 0;
    int64_t period = 0;
    void (*f)(void* arg, uint64_t seq) = nullptr;
    void* arg = nullptr;
    uint64_t seq = 0;
    int32_t idx = -1;  // position in the heap, -1 if not scheduled
};

// Schedules t to fire at t->when (nanotime). t must not be scheduled.
void add_timer(timer* t) noexcept;
// Unschedules t; returns whether it was scheduled.
bool del_timer(timer* t) noexcept;
// Reschedules t at when; returns whether it was scheduled before.
bool mod_timer(timer* t, int64_t when) noexcept;
// Runs the timers that are due; returns the time of the next, or INT64_MAX.
int64_t run_timers(int64_t now) noexcept;
int64_t next_timer() noexcept;
// Makes sure an M notices a timer that fires at when (proc.cpp).
void wake_netpoller(int64_t when) noexcept;
// Makes sure an M runs the network poller, for threads that aren't Ms
// waiting on it (proc.cpp).
void ensure_poller() noexcept;

// ---------------------------------------------------------------------------
// Network poller (netpoll.cpp)

void netpoll_init() noexcept;
// Waits up to delay ns (-1: forever, 0: don't block) for I/O readiness and
// returns the fibers that became runnable.
glist netpoll(int64_t delay) noexcept;
// Interrupts a blocked netpoll.
void netpoll_break() noexcept;
// Adds an io_uring ring's descriptor to the poller: when it has
// completions, netpoll takes them with uring_reap(ring). Returns false if
// epoll refuses.
bool netpoll_add_uring(int fd, void* ring) noexcept;
// Counts fibers and threads parked for I/O, besides those on poll_descs:
// while there are any, an M polls.
void netpoll_waiter_add(int32_t delta) noexcept;
// Takes a ring's completions; the fibers to run go to list (uring.cpp).
void uring_reap(void* ring, glist& list) noexcept;
bool netpoll_any_waiters() noexcept;
// The number of fibers and threads parked in the poller, for schedtrace.
int32_t netpoll_waiters() noexcept;

// ---------------------------------------------------------------------------
// Signals (signal.cpp)

// Installs the runtime's signal handlers, once. SIGPIPE gets a handler
// that does nothing, as in Go: writes to a broken pipe or socket fail with
// EPIPE instead of killing the process. A program started with SIGPIPE
// ignored keeps it ignored. Runs before main.
void initsig() noexcept;

// The runtime's part of the SIGURG handler: preemption (proc.cpp).
// Async-signal-safe.
void preempt_signal() noexcept;

// Go's sigpipe: after a write to standard output or error failed with
// EPIPE, dies of SIGPIPE, unless SIGPIPE is ignored or os/signal's Notify
// asked for it, which then gets it.
void sigpipe() noexcept;

// The runtime side of os/signal (Go's signal_enable and friends). Enable
// installs the runtime's handler, which queues the signal for
// signal_recv; disable restores the signal's earlier disposition; ignore
// sets SIG_IGN.
void signal_enable(uint32_t sig) noexcept;
void signal_disable(uint32_t sig) noexcept;
void signal_ignore(uint32_t sig) noexcept;
bool signal_ignored(uint32_t sig) noexcept;
// Parks until a signal enabled by signal_enable arrives, and returns it.
// Only os/signal's loop fiber calls it.
uint32_t signal_recv() noexcept;
// Returns once the signals that arrived so far have been delivered by
// os/signal's loop (Go's signalWaitUntilIdle).
void signal_wait_until_idle() noexcept;

// ---------------------------------------------------------------------------
// Semaphores (sema.cpp), after Go's sema.Go.

void semacquire(std::atomic<uint32_t>* addr, bool lifo = false) noexcept;
void semrelease(std::atomic<uint32_t>* addr, bool handoff = false) noexcept;

}  // namespace gopp::runtime
