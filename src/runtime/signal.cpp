// Signal handling, after Go's runtime/signal_unix.go, for SIGPIPE.
//
// Go's runtime handles SIGPIPE so that writes to a broken pipe or socket
// return EPIPE, except on standard output and error, where the program
// dies of the signal as a C program would (os.File.Write calls sigpipe).
// The handler is a function, not SIG_IGN, so that programs started with
// exec get the default action back.
#include "runtime.hpp"

#include <signal.h>

#include <mutex>

namespace gopp::runtime {

namespace {

std::once_flag sig_once;

void sigpipe_handler(int) {}

}  // namespace

void initsig() noexcept {
    std::call_once(sig_once, [] {
        struct sigaction old{};
        if (sigaction(SIGPIPE, nullptr, &old) < 0) return;
        if (old.sa_handler == SIG_IGN && !(old.sa_flags & SA_SIGINFO)) {
            // Started with SIGPIPE ignored (a parent's choice): keep it.
            return;
        }
        if (old.sa_handler != SIG_DFL || (old.sa_flags & SA_SIGINFO)) return;  // someone else's handler
        struct sigaction sa{};
        sa.sa_handler = sigpipe_handler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_RESTART;
        sigaction(SIGPIPE, &sa, nullptr);
    });
}

void sigpipe() noexcept {
    struct sigaction cur{};
    if (sigaction(SIGPIPE, nullptr, &cur) < 0) return;
    bool siginfo = cur.sa_flags & SA_SIGINFO;
    if (!siginfo && cur.sa_handler == SIG_IGN) return;  // ignored, now or from the start
    if (siginfo || (cur.sa_handler != SIG_DFL && cur.sa_handler != sigpipe_handler)) {
        // The program's own handler: let it have the signal, as Go sends it
        // to os/signal's channel instead of dying.
        raise(SIGPIPE);
        return;
    }
    // Go's dieFromSignal: the default action, unblocked, raised.
    struct sigaction sa{};
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGPIPE, &sa, nullptr);
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGPIPE);
    pthread_sigmask(SIG_UNBLOCK, &set, nullptr);
    raise(SIGPIPE);
}

}  // namespace gopp::runtime
