// Tracebacks: what crashes print (src/runtime/traceback.cpp). Each test
// runs a crashing program in a death test and matches its report.
#include <gopp/chan.hpp>
#include <gopp/runtime.hpp>
#include <gopp/sync.hpp>
#include <gopp/time.hpp>

#include "runtime/runtime.hpp"

#include <gtest/gtest.h>

#include <signal.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <stdexcept>
#include <thread>

using namespace gopp;
using testing::ExitedWithCode;

namespace {

// The source line of a frame: "\t/path/tests/traceback_test.cpp:12 +0x1f",
// without the offset for a call inlined in the frame.
#define HERE "\n\t[^\n]*tests/traceback_test\\.cpp:[0-9]+( \\+0x[0-9a-f]+)?\n"
// A lambda's frame. Inlined, as with -O2, it has only the name "operator()".
#define LAMBDA "[^\n]*operator\\(\\)[^\n]*" HERE

void block() {
    for (;;) std::this_thread::sleep_for(std::chrono::seconds(10));
}

[[gnu::noinline]] void panic_in_fiber() { gopp::panic("boom"); }

// Exits with 3 if unwinding ran it: the report must come first.
struct exit_if_destroyed {
    ~exit_if_destroyed() { ::_exit(3); }
};

[[gnu::noinline]] void throw_error() { throw std::runtime_error("boom"); }

[[gnu::noinline]] void throw_under_destructor() {
    exit_if_destroyed e;
    throw_error();
}

[[gnu::noinline]] void nil_deref() {
    int* volatile p = nullptr;
    *p = 1;
}

[[gnu::noinline]] int recurse(int n) {
    volatile char buf[256];
    buf[0] = static_cast<char>(n);
    if (n == -1) return 0;
    return recurse(n + 1) + buf[0];
}

// The empty asm statements keep the calls from being tail calls, which
// leave no frame with optimization.
[[gnu::noinline]] void recv_forever(chan<int> c) {
    c.recv();
    asm volatile("");
}

[[gnu::noinline]] void lock_forever(sync::Mutex* mu) {
    mu->lock();
    asm volatile("");
}

[[gnu::noinline]] void sleep_forever() {
    time::Sleep(time::Hour);
    asm volatile("");
}

// Starts fibers that block on a channel, a mutex and a sleep, and waits
// until they have.
void start_blocked_fibers() {
    auto c = make_chan<int>();
    auto* mu = new sync::Mutex;
    mu->lock();
    go([c] { recv_forever(c); });
    go([mu] { lock_forever(mu); });
    go([] { sleep_forever(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
}

}  // namespace

TEST(Traceback, PanicInFiber) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(
        {
            go([] { panic_in_fiber(); });
            block();
        },
        ExitedWithCode(2),
        "panic: boom\n"
        "\n"
        "goroutine [0-9]+ \\[running\\]:\n"
        "gopp::panic\\(\\.\\.\\.\\)\n\t[^\n]*src/runtime/proc\\.cpp:[0-9]+ \\+0x[0-9a-f]+\n"
        "\\(anonymous namespace\\)::panic_in_fiber\\(\\)" HERE
        LAMBDA
        "created by Traceback_PanicInFiber_Test::TestBody" HERE);
}

TEST(Traceback, PanicOnMainThread) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(panic_in_fiber(), ExitedWithCode(2),
                "panic: boom\n"
                "\n"
                "thread [0-9]+ \\(main\\) \\[running\\]:\n"
                "gopp::panic\\(\\.\\.\\.\\)\n\t[^\n]*\n"
                "\\(anonymous namespace\\)::panic_in_fiber\\(\\)" HERE
                "Traceback_PanicOnMainThread_Test::TestBody\\(\\)" HERE);
}

TEST(Traceback, ExceptionInFiberBeforeUnwinding) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(
        {
            go([] { throw_under_destructor(); });
            block();
        },
        ExitedWithCode(2),
        "panic: std::runtime_error: boom\n"
        "\n"
        "goroutine [0-9]+ \\[running\\]:\n"
        "\\(anonymous namespace\\)::throw_error\\(\\)" HERE
        "\\(anonymous namespace\\)::throw_under_destructor\\(\\)" HERE
        LAMBDA
        "created by Traceback_ExceptionInFiberBeforeUnwinding_Test::TestBody" HERE);
}

TEST(Traceback, ExceptionOnThread) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(std::thread([] { throw_under_destructor(); }).join(), ExitedWithCode(2),
                "panic: std::runtime_error: boom\n"
                "\n"
                "thread [0-9]+ \\[running\\]:\n"
                "\\(anonymous namespace\\)::throw_error\\(\\)" HERE
                "\\(anonymous namespace\\)::throw_under_destructor\\(\\)" HERE);
}

TEST(Traceback, NilDereference) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(
        {
            go([] { nil_deref(); });
            block();
        },
        ExitedWithCode(2),
        "panic: runtime error: invalid memory address or nil pointer dereference\n"
        "\\[signal SIGSEGV: segmentation violation code=0x1 addr=0x0 pc=0x[0-9a-f]+\\]\n"
        "\n"
        "goroutine [0-9]+ \\[running\\]:\n"
        "\\(anonymous namespace\\)::nil_deref\\(\\)" HERE
        "(" LAMBDA ")?"  // gone if nil_deref was a tail call
        "created by Traceback_NilDereference_Test::TestBody" HERE);
}

TEST(Traceback, StackOverflow) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(
        {
            go([] { recurse(0); });
            block();
        },
        ExitedWithCode(2),
        "runtime: goroutine stack exceeds [0-9]+-byte limit\n"
        "runtime: sp=0x[0-9a-f]+ stack=\\[0x[0-9a-f]+, 0x[0-9a-f]+\\]\n"
        "fatal error: stack overflow\n"
        "\n"
        "goroutine [0-9]+ \\[running\\]:\n"
        "\\(anonymous namespace\\)::recurse\\(\\.\\.\\.\\)" HERE
        "\\(anonymous namespace\\)::recurse\\(\\.\\.\\.\\)" HERE
        "(.*\n)*\\.\\.\\.additional frames elided\\.\\.\\.\n");
}

TEST(Traceback, FatalErrorPrintsAllGoroutines) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(
        {
            start_blocked_fibers();
            runtime::fatal("test");
        },
        ExitedWithCode(2),
        "fatal error: test\n"
        "\n"
        "thread [0-9]+ \\(main\\) \\[running\\]:\n"
        "(.*\n)*"
        "goroutine [0-9]+ \\[chan receive\\]:\n"
        "(.*\n)*"
        "\\(anonymous namespace\\)::recv_forever\\(\\.\\.\\.\\)" HERE
        "(.*\n)*"
        "created by \\(anonymous namespace\\)::start_blocked_fibers" HERE
        "\n"
        "goroutine [0-9]+ \\[sync.Mutex.Lock\\]:\n"
        "(.*\n)*"
        "\\(anonymous namespace\\)::lock_forever\\(\\.\\.\\.\\)" HERE
        "(.*\n)*"
        "goroutine [0-9]+ \\[sleep\\]:\n"
        "(.*\n)*"
        "\\(anonymous namespace\\)::sleep_forever\\(\\)" HERE);
}

TEST(Traceback, SigquitPrintsAllGoroutinesAndRegisters) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(
        {
            start_blocked_fibers();
            ::raise(SIGQUIT);
        },
        ExitedWithCode(2),
        "SIGQUIT: quit\n"
        "PC=0x[0-9a-f]+ sigcode=[0-9]+\n"
        "\n"
        "thread [0-9]+ \\(main\\) \\[running\\]:\n"
        "(.*\n)*"
        "Traceback_SigquitPrintsAllGoroutinesAndRegisters_Test::TestBody\\(\\)" HERE
        "(.*\n)*"
        "goroutine [0-9]+ \\[chan receive\\]:\n"
        "(.*\n)*"
        "goroutine [0-9]+ \\[sleep\\]:\n"
        "(.*\n)*"
        "\n(rax|r0) +0x[0-9a-f]+\n");
}

TEST(Traceback, RunningFiberOnAnotherThread) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(
        {
            std::atomic<bool> spinning{false};
            go([&spinning] {
                spinning = true;
                for (;;) asm volatile("" ::: "memory");
            });
            while (!spinning) std::this_thread::yield();
            runtime::fatal("test");
        },
        ExitedWithCode(2),
        "fatal error: test\n"
        "(.*\n)*"
        "goroutine [0-9]+ \\[running\\]:\n"
        LAMBDA "created by Traceback_RunningFiberOnAnotherThread_Test::TestBody" HERE);
}

TEST(Traceback, GotracebackNone) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(
        {
            ::setenv("GOTRACEBACK", "none", 1);
            go([] { panic_in_fiber(); });
            block();
        },
        ExitedWithCode(2), "panic: boom\n$");
}

TEST(Traceback, GotracebackSystemShowsRuntimeFrames) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(
        {
            ::setenv("GOTRACEBACK", "system", 1);
            go([] { panic_in_fiber(); });
            block();
        },
        ExitedWithCode(2), "goroutine [0-9]+ \\[running\\]:\n(.*\n)*gopp::runtime::run_fiber_fn\\(\\.\\.\\.\\)\n");
}

TEST(Traceback, GotracebackCrashDiesOfSigabrt) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(
        {
            ::setenv("GOTRACEBACK", "crash", 1);
            panic_in_fiber();
        },
        testing::KilledBySignal(SIGABRT), "panic: boom\n");
}
