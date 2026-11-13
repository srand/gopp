// The os/signal package: incoming signals, delivered to channels.
//
//     auto c = make_chan<os::Signal>(1);
//     signal::Notify(c, os::Interrupt, syscall::SIGTERM_);
//     os::Signal s = c.recv();
//
//     auto [ctx, stop] = signal::NotifyContext(context::Background(), os::Interrupt);
//
// The signal handler only queues a signal; a fiber of the package delivers
// it to the channels, without blocking: a channel whose buffer is full
// misses it, as in Go. Synchronous signals (SIGSEGV, SIGBUS, SIGFPE,
// SIGILL, SIGTRAP, SIGSYS) and SIGKILL and SIGSTOP can't be notified.
//
// A signal that isn't notified keeps the disposition it had: the default
// action, which ends the program for SIGINT or SIGTERM, a handler the
// program installed itself, or ignored if the program started with it
// ignored. Reset and Stop give a signal back its earlier disposition.
// SIGPIPE is the runtime's: see the os package.
#pragma once

// The module interfaces (modules/) import these instead.
#ifndef GOPP_MODULE
#include <gopp/chan.hpp>
#include <gopp/context.hpp>
#include <gopp/os.hpp>

#include <span>
#include <utility>
#endif

namespace gopp::os::signal {

namespace detail {
void notify(const chan<os::Signal>& c, std::span<const os::Signal> sigs);
void ignore(std::span<const os::Signal> sigs);
void reset(std::span<const os::Signal> sigs);
std::pair<ptr<context::Context>, context::CancelFunc> notify_context(ptr<context::Context> parent,
                                                                    std::span<const os::Signal> sigs);
}  // namespace detail

// signal.Notify: relays the signals sigs, or all if none are given, to c.
// It never blocks sending to c: give c enough buffer (1 for a single
// signal). Calls with the same channel add signals to its set.
template <typename... S>
void Notify(const chan<os::Signal>& c, const S&... sigs) {
    const os::Signal a[] = {os::Signal(sigs)..., os::Signal()};
    detail::notify(c, std::span<const os::Signal>(a, sizeof...(S)));
}

// signal.Stop: stops relaying signals to c. Once it returns, c gets no
// more signals.
void Stop(const chan<os::Signal>& c);

// signal.Ignore: ignores the signals, or all if none are given, undoing
// Notify for them.
template <typename... S>
void Ignore(const S&... sigs) {
    const os::Signal a[] = {os::Signal(sigs)..., os::Signal()};
    detail::ignore(std::span<const os::Signal>(a, sizeof...(S)));
}

// signal.Ignored: whether sig is ignored, by Ignore or from the start.
bool Ignored(const os::Signal& sig);

// signal.Reset: undoes Notify for the signals, or all if none are given.
template <typename... S>
void Reset(const S&... sigs) {
    const os::Signal a[] = {os::Signal(sigs)..., os::Signal()};
    detail::reset(std::span<const os::Signal>(a, sizeof...(S)));
}

// signal.NotifyContext: a copy of parent that is canceled when one of the
// signals arrives, stop is called, or parent is done. Its cause
// (context::Cause) is then "interrupt signal received" and the like.
// stop undoes Notify, like Reset: call it once the context isn't needed.
template <typename... S>
std::pair<ptr<context::Context>, context::CancelFunc> NotifyContext(ptr<context::Context> parent, const S&... sigs) {
    const os::Signal a[] = {os::Signal(sigs)..., os::Signal()};
    return detail::notify_context(std::move(parent), std::span<const os::Signal>(a, sizeof...(S)));
}

}  // namespace gopp::os::signal
