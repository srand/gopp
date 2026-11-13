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
#include "netpoll.hpp"
#include "runtime.hpp"

#include <signal.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <mutex>

namespace gopp::runtime {

namespace {

constexpr uint32_t nsig = 65;
constexpr int nwords = (nsig + 31) / 32;

// Go's sigtable flags for Linux, reduced to what matters here: whether
// os/signal may handle the signal (_SigNotify), and whether its default
// action is to ignore it (_SigIgn).
constexpr bool can_notify(uint32_t s) noexcept {
    switch (s) {
    case SIGILL: case SIGTRAP: case SIGBUS: case SIGFPE: case SIGKILL: case SIGSEGV:
    case SIGSTKFLT: case SIGSTOP: case SIGSYS: case 32: case 33: case 34:
        return false;
    default:
        return s >= 1 && s < nsig;
    }
}

constexpr bool default_ignored(int s) noexcept {
    return s == SIGCHLD || s == SIGURG || s == SIGWINCH || s == SIGCONT;
}

struct sig_state {
    std::atomic<uint32_t> wanted[nwords];   // signals Notify asked for
    std::atomic<uint32_t> ignored[nwords];  // signals ignored, by Ignore or from the start
    std::atomic<uint32_t> mask[nwords];     // signals queued for signal_recv
    std::atomic<uint32_t> handling[nwords]; // signals with the runtime's handler
    std::atomic<int32_t> delivering{0};     // handlers in sigsend
    // signal_recv's state: true while it waits with nothing queued.
    std::atomic<bool> receiving{false};
    std::atomic<int> wakefd{-1};            // eventfd that wakes signal_recv
    poll_desc* pd = nullptr;
    uint32_t recv[nwords] = {};             // signal_recv's local copy of mask
    lock_t lock;                            // serializes enable, disable and ignore
    // The dispositions before the runtime's handler (Go's fwdSig).
    struct sigaction fwd[nsig] = {};
    // SIGPIPE's handler was installed at startup: it stays.
    bool sigpipe_at_init = false;
    // SIGURG's handler was installed at startup, for preemption: it stays
    // through Stop, Reset and Ignore.
    bool sigurg_at_init = false;
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
void raise_default(int s) noexcept {
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
        if (!default_ignored(s)) raise_default(s);
        return;
    }
    f.sa_handler(s);
}

void sighandler(int s, siginfo_t* info, void* ctx) {
    int saved = errno;
    if (s == SIGURG) preempt_signal();
    if (!sigsend(static_cast<uint32_t>(s))) forward(s, info, ctx);
    errno = saved;
}

bool is_runtime_handler(const struct sigaction& sa) noexcept {
    return (sa.sa_flags & SA_SIGINFO) && sa.sa_sigaction == sighandler;
}

void install(int s) noexcept {
    struct sigaction sa{};
    sa.sa_sigaction = sighandler;
    sigfillset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO | SA_RESTART | SA_ONSTACK;
    ::sigaction(s, &sa, nullptr);
}

// The eventfd of signal_recv, registered with the poller on first use.
void ensure_receiver() noexcept {
    if (sig.wakefd.load(std::memory_order_acquire) >= 0) return;
    int fd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (fd < 0) fatal("os/signal: eventfd failed");
    int err = 0;
    sig.pd = poll_open(fd, &err);
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
        const struct sigaction& old = sig.fwd[SIGPIPE];
        if (!(old.sa_flags & SA_SIGINFO) && old.sa_handler == SIG_DFL) {
            install(SIGPIPE);
            set(sig.handling, SIGPIPE, true);
            sig.sigpipe_at_init = true;
        }
        // Otherwise SIGPIPE was ignored, a parent's choice, or has someone
        // else's handler: either stays.

        // SIGURG preempts long-running fibers, so its handler goes in even
        // if the program started with it ignored, as Go's does. A handler
        // that was there before gets it forwarded.
        install(SIGURG);
        set(sig.handling, SIGURG, true);
        sig.sigurg_at_init = true;
    });
}

void sigpipe() noexcept {
    if (signal_ignored(SIGPIPE) || sigsend(SIGPIPE)) return;
    struct sigaction cur{};
    if (::sigaction(SIGPIPE, nullptr, &cur) < 0) return;
    bool siginfo = cur.sa_flags & SA_SIGINFO;
    if (!siginfo && cur.sa_handler == SIG_IGN) return;  // ignored by other means
    if (!is_runtime_handler(cur) && (siginfo || cur.sa_handler != SIG_DFL)) {
        // The program's own handler: let it have the signal.
        ::raise(SIGPIPE);
        return;
    }
    // Go's dieFromSignal.
    raise_default(SIGPIPE);
}

void signal_enable(uint32_t s) noexcept {
    initsig();
    if (s >= nsig) return;
    sig.lock.lock();
    ensure_receiver();
    set(sig.wanted, s, true);
    set(sig.ignored, s, false);
    if (can_notify(s) && !test(sig.handling, s)) {
        ::sigaction(static_cast<int>(s), nullptr, &sig.fwd[s]);
        if (!is_runtime_handler(sig.fwd[s])) install(static_cast<int>(s));
        set(sig.handling, s, true);
    }
    sig.lock.unlock();
}

void signal_disable(uint32_t s) noexcept {
    initsig();
    if (s >= nsig) return;
    sig.lock.lock();
    if (can_notify(s) && test(sig.handling, s) && !(s == SIGPIPE && sig.sigpipe_at_init) &&
        !(s == SIGURG && sig.sigurg_at_init)) {
        // Back to the disposition before Notify.
        ::sigaction(static_cast<int>(s), &sig.fwd[s], nullptr);
        set(sig.handling, s, false);
        const struct sigaction& f = sig.fwd[s];
        if (!(f.sa_flags & SA_SIGINFO) && f.sa_handler == SIG_IGN) set(sig.ignored, s, true);
    }
    set(sig.wanted, s, false);
    sig.lock.unlock();
}

void signal_ignore(uint32_t s) noexcept {
    initsig();
    if (s >= nsig) return;
    sig.lock.lock();
    if (can_notify(s) && !(s == SIGURG && sig.sigurg_at_init)) {
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

bool signal_ignored(uint32_t s) noexcept {
    initsig();
    return s < nsig && test(sig.ignored, s);
}

uint32_t signal_recv() noexcept {
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
        poll_reset(sig.pd, 'r');
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
        poll_wait(sig.pd, 'r');
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
