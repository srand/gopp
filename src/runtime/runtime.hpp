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

#include <signal.h>
#include <sys/types.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <span>
#include <string_view>
#include <vector>

namespace gopp::runtime {

// ---------------------------------------------------------------------------
// Architecture

inline void cpuRelax() noexcept {
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

class lockT {
public:
    void lock() noexcept {
        uint32_t c = 0;
        if (s_.compare_exchange_strong(c, 1, std::memory_order_acquire)) return;
        lockSlow();
    }

    void unlock() noexcept {
        if (s_.exchange(0, std::memory_order_release) == 2) s_.notify_one();
    }

private:
    void lockSlow() noexcept;
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

// Why a fiber waits, for tracebacks (Go's waitReason).
enum class waitreason : uint8_t {
    none,
    chanReceive,
    chanSend,
    chanReceiveNil,
    chanSendNil,
    select,
    selectNoCases,
    sleep,
    ioWait,
    semacquire,
    syncMutexLock,
    syncRwmutexRlock,
    syncRwmutexLock,
    syncWaitgroupWait,
    syncCondWait,
};
enum class gaction : uint8_t { none, park, yield, exit, exitsyscall };

using unlockFn = bool (*)(struct G* gp, void* arg);

struct G {
    boost::context::fiber ctx;     // the suspended fiber, while not running
    boost::context::fiber caller;  // the M's scheduler context, while running
    std::move_only_function<void()> fn;

    std::atomic<gstatus> status{gstatus::idle};
    gaction action = gaction::none;
    unlockFn unlockf = nullptr;
    void* unlockarg = nullptr;

    M* m = nullptr;
    G* schedlink = nullptr;
    uint64_t goid = 0;

    // Channels and select (Go's g.param, g.selectDone, g.waiting).
    void* param = nullptr;
    std::atomic<uint32_t> selectDone{0};
    sudog* waiting = nullptr;

    // A pseudo-G that stands for a thread the runtime didn't start. Parking
    // it blocks the thread on threadSema.
    bool isThread = false;
    std::atomic<uint32_t> threadSema{0};

    // Nonzero while the fiber runs runtime code that uses its M's P: the
    // SIGURG handler then leaves the P alone. Only the thread running the
    // fiber touches it; see nopreemptGuard in proc.cpp. It is kept on the
    // G, not the M, because a fiber parks inside it and finishes it on
    // whichever M resumes it.
    uint32_t nopreempt = 0;

    // Tracebacks (traceback.cpp). Gs are never freed: all of them, pseudo-
    // Gs included, are on the allgs list through alllink. The fiber's stack
    // is [stackLo, stackHi). schedFp is the frame address of mcall while
    // the fiber is switched out, 0 before it first runs. gopc is the return
    // address of newproc's caller, in the function that started the fiber,
    // and parentGoid its goroutine. tid is the thread of a pseudo-G, 0
    // once that thread has exited.
    G* alllink = nullptr;
    uintptr_t stackLo = 0;
    uintptr_t stackHi = 0;
    uintptr_t schedFp = 0;
    uintptr_t gopc = 0;
    uint64_t parentGoid = 0;
    waitreason wait = waitreason::none;
    pid_t tid = 0;
};

// Timers (timer.cpp): each P has a heap of them, like Go's.
struct timer {
    int64_t when = 0;
    int64_t period = 0;
    void (*f)(void* arg, uint64_t seq) = nullptr;
    void* arg = nullptr;
    uint64_t seq = 0;
    int32_t idx = -1;                // position in its P's heap, -1 if not scheduled
    std::atomic<P*> pp{nullptr};     // the P whose heap holds it; written under that heap's lock
};

// A P's timers: a heap by when, and the earliest when, for Ms that look
// for due timers without the lock.
struct ptimers {
    lockT lock;
    std::vector<timer*> heap;
    std::atomic<int64_t> next{INT64_MAX};
};

constexpr uint32_t runqSize = 256;

struct P {
    int32_t id = 0;
    // running, m and schedtick are written by the M that holds the P (or
    // the one acquiring it) and read by sysmon: relaxed atomics, one
    // writer at a time.
    std::atomic<bool> running{false};
    std::atomic<M*> m{nullptr};
    P* link = nullptr;
    // Fibers started on this P, and times an M acquired it: sysmon sees a
    // P that changed hands as busy for a new run.
    std::atomic<uint32_t> schedtick{0};

    // Local run queue, lock-free: the owner puts at the tail, anyone takes
    // from the head.
    std::atomic<uint32_t> runqhead{0};
    std::atomic<uint32_t> runqtail{0};
    std::atomic<G*> runq[runqSize] = {};
    std::atomic<G*> runnext{nullptr};

    // Free Gs, for reuse.
    G* gfree = nullptr;
    int32_t gfreecnt = 0;

    // Timers added by fibers running on this P.
    ptimers timers;
};

// Scheduler counters, per M so that counting doesn't share cache lines.
// Printed by GOPP_SCHEDTRACE=<ms>.
struct schedStats {
#define GOPP_SCHED_STATS(X) \
    X(schedules) X(stealOk) X(stealFail) X(pollNb) X(pollNbReady) X(pollBlock) X(pollBlockReady) \
    X(stopm) X(startm) X(wakep) X(spin) X(read) X(readEagain) X(write) X(writeEagain) \
    X(uringDone) X(uringPark)
#define GOPP_FIELD(name) std::atomic<uint64_t> name{0};
    GOPP_SCHED_STATS(GOPP_FIELD)
#undef GOPP_FIELD
};

inline void statInc(std::atomic<uint64_t>& c, uint64_t n = 1) noexcept {
    c.store(c.load(std::memory_order_relaxed) + n, std::memory_order_relaxed);  // single writer
}

struct M {
    int64_t id = 0;
    schedStats stats;
    M* alllink = nullptr;
    P* p = nullptr;
    P* nextp = nullptr;
    G* curg = nullptr;
    bool spinning = false;
    note park;
    M* schedlink = nullptr;
    uint64_t fastrand = 0;

    // Preemption. tid is the M's thread, for tgkill. sysmon asks for the
    // P with preemptTick and then preemptP; the SIGURG handler, on this
    // M's thread, answers with the P it gave up in retaken, or, if the
    // fiber was inside the runtime, with the tick it declined in
    // preemptDeclined (1 << 32 | tick), so that sysmon asks again soon.
    // preempted is set while the M runs its fiber without the P it lost,
    // and only this M's thread touches it.
    pid_t tid = 0;
    std::atomic<P*> preemptP{nullptr};
    std::atomic<uint32_t> preemptTick{0};
    std::atomic<P*> retaken{nullptr};
    std::atomic<uint64_t> preemptDeclined{0};
    bool preempted = false;
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
#define GOPP_STAT(field) do { if (::gopp::runtime::M* m_ = ::gopp::runtime::getm()) ::gopp::runtime::statInc(m_->stats.field); } while (0)

// The calling fiber; a pseudo-G on foreign threads; nullptr on an M's
// scheduler context.
G* getg() noexcept;

// Parks the calling fiber. After the switch to the scheduler, unlockf(gp,
// arg) runs there; if it returns false the fiber resumes at once. This is
// how a fiber releases the lock of a wait queue only once it can no longer
// run, so that a concurrent ready() can't resume it while it's running.
// reason shows in tracebacks.
void park(unlockFn unlockf, void* arg, waitreason reason) noexcept;

// Parks the calling fiber after unlocking l.
void parkUnlock(lockT* l, waitreason reason) noexcept;

// Makes a parked fiber runnable.
void ready(G* gp) noexcept;

// Makes a list of fibers runnable, from a scheduler context.
void injectglist(glist& list) noexcept;

uint32_t fastrand() noexcept;

void ensureInit() noexcept;

// Whether the calling fiber's P has other fibers to run: in its run queue,
// or the global one. False outside fibers.
bool hasRunnable() noexcept;

// ---------------------------------------------------------------------------
// Timers (timer.cpp)



// Schedules t to fire at t->when (nanotime), on the calling fiber's P
// (any P on other threads). t must not be scheduled.
void addTimer(timer* t) noexcept;
// Unschedules t; returns whether it was scheduled.
bool delTimer(timer* t) noexcept;
// Reschedules t at when, on the P it's on (or as addTimer does); returns
// whether it was scheduled before.
bool modTimer(timer* t, int64_t when) noexcept;
// Runs pp's timers that are due; returns the time of its next, or
// INT64_MAX. Any M may run any P's timers.
int64_t runTimers(P* pp, int64_t now) noexcept;
// Runs the due timers of all Ps.
void runDueTimers(int64_t now) noexcept;
// The earliest timer of all Ps, or INT64_MAX.
int64_t nextTimer() noexcept;
// The P a new timer joins: the calling fiber's, or any (proc.cpp).
P* timerP() noexcept;
// All Ps (proc.cpp).
std::span<P> allPs() noexcept;
// Makes sure an M notices a timer that fires at when (proc.cpp).
void wakeNetpoller(int64_t when) noexcept;
// Makes sure an M runs the network poller, for threads that aren't Ms
// waiting on it (proc.cpp).
void ensurePoller() noexcept;

// ---------------------------------------------------------------------------
// Network poller (netpoll.cpp)

void netpollInit() noexcept;
// Waits up to delay ns (-1: forever, 0: don't block) for I/O readiness and
// returns the fibers that became runnable.
glist netpoll(int64_t delay) noexcept;
// Interrupts a blocked netpoll.
void netpollBreak() noexcept;
// Adds an io_uring ring's descriptor to the poller: when it has
// completions, netpoll takes them with uring_reap(ring). Returns false if
// epoll refuses.
bool netpollAddUring(int fd, void* ring) noexcept;
// Counts fibers and threads parked for I/O, besides those on poll_descs:
// while there are any, an M polls.
void netpollWaiterAdd(int32_t delta) noexcept;
// Takes a ring's completions; the fibers to run go to list (uring.cpp).
void uringReap(void* ring, glist& list) noexcept;
bool netpollAnyWaiters() noexcept;
// The number of fibers and threads parked in the poller, for schedtrace.
int32_t netpollWaiters() noexcept;

// ---------------------------------------------------------------------------
// Signals (signal.cpp)

// Installs the runtime's signal handlers, once. SIGPIPE gets a handler
// that does nothing, as in Go: writes to a broken pipe or socket fail with
// EPIPE instead of killing the process. A program started with SIGPIPE
// ignored keeps it ignored. Runs before main.
void initsig() noexcept;

// The runtime's part of the SIGURG handler: preemption (proc.cpp).
// Async-signal-safe.
void preemptSignal() noexcept;

// Go's sigpipe: after a write to standard output or error failed with
// EPIPE, dies of SIGPIPE, unless SIGPIPE is ignored or os/signal's Notify
// asked for it, which then gets it.
void sigpipe() noexcept;

// The runtime side of os/signal (Go's signal_enable and friends). Enable
// installs the runtime's handler, which queues the signal for
// signal_recv; disable restores the signal's earlier disposition; ignore
// sets SIG_IGN.
void signalEnable(uint32_t sig) noexcept;
void signalDisable(uint32_t sig) noexcept;
void signalIgnore(uint32_t sig) noexcept;
bool signalIgnored(uint32_t sig) noexcept;
// Parks until a signal enabled by signal_enable arrives, and returns it.
// Only os/signal's loop fiber calls it.
uint32_t signalRecv() noexcept;
// Returns once the signals that arrived so far have been delivered by
// os/signal's loop (Go's signalWaitUntilIdle).
void signalWaitUntilIdle() noexcept;

// ---------------------------------------------------------------------------
// Tracebacks (traceback.cpp), after Go's traceback.go and panic.go.

// The head of the list of all Gs, linked through G::alllink (proc.cpp).
G* allgs() noexcept;
// The calling fiber, or the pseudo-G of a foreign thread if it has one;
// unlike getg it never creates one, so signal handlers can call it.
G* getgIfAny() noexcept;
// The entry function of fibers, where their stacks start (proc.cpp).
uintptr_t gentryPc() noexcept;
// Whether t is the main thread (proc.cpp).
bool isMainThread(pid_t t) noexcept;

// Prints "panic: msg" and the calling goroutine's stack, or all of them,
// as GOTRACEBACK says, and exits.
[[noreturn]] void dopanic(std::string_view msg) noexcept;
// The same for "fatal error: msg", which prints all goroutines.
[[noreturn]] void dofatal(const char* msg) noexcept;
// The handler of the crash signals: SIGSEGV, SIGBUS, SIGFPE, SIGILL,
// SIGTRAP, SIGABRT, SIGSYS and SIGQUIT.
[[noreturn]] void crashSignal(int sig, siginfo_t* info, void* uc) noexcept;
// The traceback's part of the SIGURG handler: answers a request to print
// the stack of this thread. Async-signal-safe.
void tracebackSignal(void* uc) noexcept;
// Runs gp's function. An exception that escapes it is reported as a panic
// from where it was thrown, before any unwinding.
void runFiberFn(G* gp);
// The terminate handler: reports an uncaught exception, on threads other
// than fibers, as a panic.
[[noreturn]] void terminateHandler() noexcept;
// Gives the calling thread an alternate signal stack, if it has none, so
// that the crash handler can run after a stack overflow.
void ensureSigaltstack() noexcept;
// Frees the calling thread's alternate signal stack, at its exit.
void releaseSigaltstack() noexcept;

// ---------------------------------------------------------------------------
// Semaphores (sema.cpp), after Go's sema.Go.

void semacquire(std::atomic<uint32_t>* addr, bool lifo = false,
                waitreason reason = waitreason::semacquire) noexcept;
void semrelease(std::atomic<uint32_t>* addr, bool handoff = false) noexcept;

}  // namespace gopp::runtime
