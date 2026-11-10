// The go++ runtime: fibers ("goroutines") scheduled M:N over OS threads.
//
//     gopp::go([] { ... });            // go func() { ... }()
//     gopp::go(f, arg1, arg2);         // go f(arg1, arg2)
//
// Code that blocks on channels, locks, timers or the network parks the
// fiber, never the thread. Threads that the runtime didn't start, such as
// the main thread, can use all of these too; they block the thread instead.
#pragma once

#include <gopp/base.hpp>

#include <functional>
#include <tuple>
#include <utility>

namespace gopp {

namespace runtime {

// Starts fn on a new fiber.
void newproc(std::move_only_function<void()> fn);

// runtime.Gosched: yields the processor to other fibers.
void Gosched();

// runtime.GOMAXPROCS: the number of threads running fibers at the same
// time. Set it with the GOMAXPROCS environment variable.
int GOMAXPROCS() noexcept;

// runtime.NumCPU
int NumCPU() noexcept;

// runtime.NumGoroutine
int64 NumGoroutine() noexcept;

// The id of the calling fiber, or 0 outside fibers.
uint64 goid() noexcept;

// Monotonic clock in nanoseconds.
int64 nanotime() noexcept;

// Marks a call that blocks its thread, such as file I/O or a C library
// call. The thread's processor is handed to another thread meanwhile, as Go
// does around blocking system calls.
void entersyscall() noexcept;
void exitsyscall() noexcept;

class blocking_call {
public:
    blocking_call() noexcept { entersyscall(); }
    ~blocking_call() { exitsyscall(); }
    blocking_call(const blocking_call&) = delete;
    blocking_call& operator=(const blocking_call&) = delete;
};

// Run f as a blocking call.
template <typename F>
decltype(auto) blocking(F&& f) {
    blocking_call b;
    return std::forward<F>(f)();
}

}  // namespace runtime

// The go statement: runs f(args...) on a new fiber. Arguments are moved
// or copied into the fiber, like Go evaluates them before starting it.
template <typename F, typename... Args>
void go(F&& f, Args&&... args) {
    runtime::newproc([f = std::forward<F>(f), tup = std::make_tuple(std::forward<Args>(args)...)]() mutable {
        std::apply(std::move(f), std::move(tup));
    });
}

}  // namespace gopp
