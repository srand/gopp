// Signal handling, after Go's runtime/signal_unix.go and sigqueue.go.
//
// SIGPIPE: Go's runtime handles it so that writes to a broken pipe or
// socket return EPIPE, except on standard output and error, where the
// program dies of the signal as a C program would (os.File.Write calls
// sigpipe). The handler is a function, not SIG_IGN, so that programs
// started with exec get the default action back.
//
// os/signal: Notify installs the handler on a signal (sigenable). The
// handler only queues the signal, as Go's sigsend does: it sets the
// signal's bit in a mask and writes to an eventfd, both async-signal-safe.
// os/signal's loop fiber waits on the eventfd through the network poller
// (signal_recv) and delivers the signals to the channels. Signals the
// program started with ignored stay ignored until Notify asks for them,
// and Reset or Stop gives a signal back its earlier disposition.
//
// Crashes: SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGTRAP, SIGABRT, SIGSYS and
// SIGQUIT get a handler at startup, if their disposition is the default
// one, that prints the goroutines' stacks and exits (traceback.cpp), as
// Go's does. Notify can still take SIGQUIT and SIGABRT; Reset or Stop
// gives them back to the runtime.
#include "netpoll.hpp"
#include "runtime.hpp"

#include <signal.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <exception>
#include <mutex>

namespace gopp::runtime {

namespace {

constexpr uint32_t nsig = 65;
constexpr int nwords = (nsig + 31) / 32;

// Go's sigtable flags for Linux, reduced to what matters here: whether
// os/signal may handle the signal (_SigNotify), and whether its default
// action is to ignore it (_SigIgn).
constexpr bool canNotify(uint32_t s) noexcept {
    switch (s) {
    case SIGILL: case SIGTRAP: case SIGBUS: case SIGFPE: case SIGKILL: case SIGSEGV:
    case SIGSTKFLT: case SIGSTOP: case SIGSYS: case 32: case 33: case 34:
        return false;
    default:
        return s >= 1 && s < nsig;
    }
}

constexpr bool defaultIgnored(int s) noexcept {
    return s == SIGCHLD || s == SIGURG || s == SIGWINCH || s == SIGCONT;
}

// Go's _SigPanic and _SigThrow signals.
constexpr bool isCrash(int s) noexcept {
    switch (s) {
    case SIGSEGV: case SIGBUS: case SIGFPE: case SIGILL: case SIGTRAP: case SIGABRT: case SIGSYS: case SIGQUIT:
        return true;
    default:
        return false;
    }
}

struct sig_state {
    std::atomic<uint32_t> wanted[nwords];   // signals Notify asked for
    std::atomic<uint32_t> ignored[nwords];  // signals ignored, by Ignore or from the start
    std::atomic<uint32_t> mask[nwords];     // signals queued for signal_recv
    std::atomic<uint32_t> handling[nwords]; // signals with the runtime's handler
    std::atomic<int32_t> delivering{0};     // handlers in sigsend
    std::atomic<uint32_t> at_init[nwords];  // handlers installed by initsig, which stay
    // signal_recv's state: true while it waits with nothing queued.
    std::atomic<bool> receiving{false};
    std::atomic<int> wakefd{-1};            // eventfd that wakes signal_recv
    poll_desc* pd = nullptr;
    uint32_t recv[nwords] = {};             // signal_recv's local copy of mask
    lock_t lock;                            // serializes enable, disable and ignore
    // The dispositions before the runtime's handler (Go's fwdSig).
    struct sigaction fwd[nsig] = {};
};

// Constant-initialized: initsig runs before dynamic initialization.
constinit sig_state sig;
constinit std::once_flag init_once;

bool test(const std::atomic<uint32_t>* words, uint32_t s) noexcept {
    return (words[s / 32].load(std::memory_order_acquire) >> (s % 32)) & 1;
}

void set(std::atomic<uint32_t>* words, uint32_t s, bool on) noexcept {
    if (on) words[s / 32].fetch_or(uint32_t(1) << (s % 32), std::memory_order_acq_rel);
    else words[s / 32].fetch_and(~(uint32_t(1) << (s % 32)), std::memory_order_acq_rel);
}

// Go's sigsend: queues s for signal_recv if Notify wants it. Async-signal-
// safe.
bool sigsend(uint32_t s) noexcept {
    if (s >= nsig) return false;
    sig.delivering.fetch_add(1, std::memory_order_acq_rel);
    if (!test(sig.wanted, s)) {
        sig.delivering.fetch_sub(1, std::memory_order_acq_rel);
        return false;
    }
    set(sig.mask, s, true);
    int fd = sig.wakefd.load(std::memory_order_acquire);
    if (fd >= 0) {
        uint64_t one = 1;
        // EAGAIN means the counter is full: signal_recv will wake anyway.
        while (::write(fd, &one, sizeof(one)) < 0 && errno == EINTR) {
        }
    }
    sig.delivering.fetch_sub(1, std::memory_order_acq_rel);
    return true;
}

// Takes the default action of s: dies, stops, or nothing. Then puts the
// runtime's handler back, for a signal that stopped the process.
void raiseDefault(int s) noexcept {
    struct sigaction dfl{}, cur{};
    dfl.sa_handler = SIG_DFL;
    sigemptyset(&dfl.sa_mask);
    ::sigaction(s, &dfl, &cur);
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, s);
    ::sigprocmask(SIG_UNBLOCK, &set, nullptr);
    ::raise(s);
    ::sigaction(s, &cur, nullptr);
}

// A signal nobody asked for reached the handler: SIGPIPE does nothing
// (writes fail with EPIPE); others get the disposition they had before
// the handler was installed, as Go's sigfwd does.
void forward(int s, siginfo_t* info, void* ctx) noexcept {
    if (s == SIGPIPE) return;
    if (s == SIGURG && test(sig.ignored, s)) return;  // Ignore kept the handler
    const struct sigaction& f = sig.fwd[s];
    if (f.sa_flags & SA_SIGINFO) {
        if (f.sa_sigaction) f.sa_sigaction(s, info, ctx);
        return;
    }
    if (f.sa_handler == SIG_IGN) return;
    if (f.sa_handler == SIG_DFL) {
        if (!defaultIgnored(s)) raiseDefault(s);
        return;
    }
    f.sa_handler(s);
}

void sighandler(int s, siginfo_t* info, void* ctx) {
    int saved = errno;
    if (s == SIGURG) {
        tracebackSignal(ctx);
        preemptSignal();
    }
    bool crash = isCrash(s) && test(sig.at_init, static_cast<uint32_t>(s));
    // Faults can't be notified: their handler returning would fault again.
    if (crash && !canNotify(static_cast<uint32_t>(s))) crashSignal(s, info, ctx);
    if (!sigsend(static_cast<uint32_t>(s))) {
        if (crash) crashSignal(s, info, ctx);
        forward(s, info, ctx);
    }
    errno = saved;
}

bool isRuntimeHandler(const struct sigaction& sa) noexcept {
    return (sa.sa_flags & SA_SIGINFO) && sa.sa_sigaction == sighandler;
}

void install(int s) noexcept {
    struct sigaction sa{};
    sa.sa_sigaction = sighandler;
    sigfillset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO | SA_RESTART | SA_ONSTACK;
    if (isCrash(s)) {
        // A fault while the traceback prints comes back to the handler,
        // which then gives up, instead of the kernel killing the process
        // without a word.
        sa.sa_flags |= SA_NODEFER;
        for (int f : {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGTRAP, SIGABRT, SIGSYS}) sigdelset(&sa.sa_mask, f);
    }
    ::sigaction(s, &sa, nullptr);
}

// The eventfd of signal_recv, registered with the poller on first use.
void ensureReceiver() noexcept {
    if (sig.wakefd.load(std::memory_order_acquire) >= 0) return;
    int fd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (fd < 0) fatal("os/signal: eventfd failed");
    int err = 0;
    sig.pd = pollOpen(fd, &err);
    if (!sig.pd) fatal("os/signal: can't poll the eventfd");
    sig.wakefd.store(fd, std::memory_order_release);
}

}  // namespace

void initsig() noexcept {
    std::call_once(init_once, [] {
        for (uint32_t s = 1; s < nsig; s++) {
            struct sigaction& old = sig.fwd[s];
            if (::sigaction(static_cast<int>(s), nullptr, &old) < 0) continue;
            // Signals the program started with ignored stay ignored (Go
            // keeps SIGHUP and SIGINT so; here all of them).
            if (!(old.sa_flags & SA_SIGINFO) && old.sa_handler == SIG_IGN) set(sig.ignored, s, true);
        }
        auto is_default = [](uint32_t s) {
            const struct sigaction& old = sig.fwd[s];
            return !(old.sa_flags & SA_SIGINFO) && old.sa_handler == SIG_DFL;
        };
        auto install_at_init = [](uint32_t s) {
            install(static_cast<int>(s));
            set(sig.handling, s, true);
            set(sig.at_init, s, true);
        };
        // Otherwise SIGPIPE, or a crash signal, was ignored, a parent's
        // choice, or has someone else's handler, such as a sanitizer's:
        // either stays.
        if (is_default(SIGPIPE)) install_at_init(SIGPIPE);
        for (uint32_t s = 1; s < nsig; s++)
            if (isCrash(static_cast<int>(s)) && is_default(s)) install_at_init(s);

        // SIGURG preempts long-running fibers, and asks threads for their
        // stacks in tracebacks, so its handler goes in even if the program
        // started with it ignored, as Go's does. A handler that was there
        // before gets it forwarded.
        install_at_init(SIGURG);

        // An exception that escapes a thread is reported as a panic.
        std::set_terminate(terminateHandler);
        ensureSigaltstack();
    });
}

void sigpipe() noexcept {
    if (signalIgnored(SIGPIPE) || sigsend(SIGPIPE)) return;
    struct sigaction cur{};
    if (::sigaction(SIGPIPE, nullptr, &cur) < 0) return;
    bool siginfo = cur.sa_flags & SA_SIGINFO;
    if (!siginfo && cur.sa_handler == SIG_IGN) return;  // ignored by other means
    if (!isRuntimeHandler(cur) && (siginfo || cur.sa_handler != SIG_DFL)) {
        // The program's own handler: let it have the signal.
        ::raise(SIGPIPE);
        return;
    }
    // Go's dieFromSignal.
    raiseDefault(SIGPIPE);
}

void signalEnable(uint32_t s) noexcept {
    initsig();
    if (s >= nsig) return;
    sig.lock.lock();
    ensureReceiver();
    set(sig.wanted, s, true);
    set(sig.ignored, s, false);
    if (canNotify(s) && !test(sig.handling, s)) {
        ::sigaction(static_cast<int>(s), nullptr, &sig.fwd[s]);
        if (!isRuntimeHandler(sig.fwd[s])) install(static_cast<int>(s));
        set(sig.handling, s, true);
    }
    sig.lock.unlock();
}

void signalDisable(uint32_t s) noexcept {
    initsig();
    if (s >= nsig) return;
    sig.lock.lock();
    if (canNotify(s) && test(sig.handling, s) && !test(sig.at_init, s)) {
        // Back to the disposition before Notify.
        ::sigaction(static_cast<int>(s), &sig.fwd[s], nullptr);
        set(sig.handling, s, false);
        const struct sigaction& f = sig.fwd[s];
        if (!(f.sa_flags & SA_SIGINFO) && f.sa_handler == SIG_IGN) set(sig.ignored, s, true);
    }
    set(sig.wanted, s, false);
    sig.lock.unlock();
}

void signalIgnore(uint32_t s) noexcept {
    initsig();
    if (s >= nsig) return;
    sig.lock.lock();
    if (canNotify(s) && !(s == SIGURG && test(sig.at_init, s))) {
        if (!test(sig.handling, s)) ::sigaction(static_cast<int>(s), nullptr, &sig.fwd[s]);
        struct sigaction ign{};
        ign.sa_handler = SIG_IGN;
        sigemptyset(&ign.sa_mask);
        ::sigaction(static_cast<int>(s), &ign, nullptr);
        set(sig.handling, s, false);
    }
    set(sig.wanted, s, false);
    set(sig.ignored, s, true);
    sig.lock.unlock();
}

bool signalIgnored(uint32_t s) noexcept {
    initsig();
    return s < nsig && test(sig.ignored, s);
}

uint32_t signalRecv() noexcept {
    for (;;) {
        // Serve the signals of the local copy.
        for (uint32_t i = 0; i < nsig; i++) {
            if (sig.recv[i / 32] & (uint32_t(1) << (i % 32))) {
                sig.recv[i / 32] &= ~(uint32_t(1) << (i % 32));
                return i;
            }
        }
        // Wait for sigsend. The eventfd is drained before the mask is
        // looked at, so a signal queued after that wakes poll_wait.
        pollReset(sig.pd, 'r');
        uint64_t n;
        while (::read(sig.wakefd.load(std::memory_order_relaxed), &n, sizeof(n)) < 0 && errno == EINTR) {
        }
        sig.receiving.store(false, std::memory_order_seq_cst);
        bool any = false;
        for (int i = 0; i < nwords; i++) {
            sig.recv[i] = sig.mask[i].exchange(0, std::memory_order_seq_cst);
            any |= sig.recv[i] != 0;
        }
        if (any) continue;
        sig.receiving.store(true, std::memory_order_seq_cst);
        pollWait(sig.pd, 'r');
    }
}

void signal_wait_until_idle() noexcept {
    // A handler may be queuing a signal; then signal_recv may hold some it
    // hasn't delivered. Wait until it waits with nothing queued.
    for (;;) {
        bool idle = sig.delivering.load(std::memory_order_seq_cst) == 0;
        for (int i = 0; idle && i < nwords; i++) idle = sig.mask[i].load(std::memory_order_seq_cst) == 0;
        if (idle && sig.receiving.load(std::memory_order_seq_cst)) return;
        Gosched();
    }
}

}  // namespace gopp::runtime
