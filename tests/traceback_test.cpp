// Tracebacks: what crashes print (src/runtime/traceback.cpp). Each test
// runs a crashing program in a death test and matches its report.
#include <gopp/chan.hpp>
#include <gopp/runtime.hpp>
#include <gopp/sync.hpp>
#include <gopp/os/exec.hpp>
#include <gopp/time.hpp>

#include "go_helper.hpp"
#include "runtime/runtime.hpp"

#include <gtest/gtest.h>

#include <signal.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <regex>
#include <stdexcept>
#include <string>
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

[[gnu::noinline]] void panicInFiber() { gopp::panic("boom"); }

// Exits with 3 if unwinding ran it: the report must come first.
struct exitIfDestroyed {
    ~exitIfDestroyed() { ::_exit(3); }
};

[[gnu::noinline]] void throwError() { throw std::runtime_error("boom"); }

[[gnu::noinline]] void throwUnderDestructor() {
    exitIfDestroyed e;
    throwError();
}

[[gnu::noinline]] void nilDeref() {
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
[[gnu::noinline]] void recvForever(chan<int> c) {
    c.recv();
    asm volatile("");
}

[[gnu::noinline]] void lockForever(sync::Mutex* mu) {
    mu->lock();
    asm volatile("");
}

[[gnu::noinline]] void sleepForever() {
    time::Sleep(time::Hour);
    asm volatile("");
}

// Starts fibers that block on a channel, a mutex and a sleep, and waits
// until they have.
void startBlockedFibers() {
    auto c = make_chan<int>();
    auto* mu = new sync::Mutex;
    mu->lock();
    go([c] { recvForever(c); });
    go([mu] { lockForever(mu); });
    go([] { sleepForever(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
}

// Panics in a fiber, for the stripped copies of this program.
const bool panicHelper = go_helper::registerHelper("traceback-panic", [](const std::vector<std::string>&) {
    go([] { panicInFiber(); });
    block();
});

// Runs the helper in a copy of this program stripped with strip's flags
// (none: of everything), and returns what it printed.
std::string runStripped(const std::vector<string>& flags, std::string& exe) {
    exe = ::testing::TempDir() + "traceback_stripped_" + std::to_string(::getpid());
    std::vector<string> args = flags;
    args.push_back("-o");
    args.push_back(string(exe));
    args.push_back(go_helper::executable());
    auto [stripOut, err] = os::exec::Command("strip", std::span<const string>(args))->CombinedOutput();
    if (err) {
        ADD_FAILURE() << "strip: " << err << ": " << asStringView(stripOut);
        return {};
    }
    // The variable go_helper looks for, which helperCommand sets.
    os::Setenv("GO_EXEC_TEST_PID", std::to_string(os::Getpid()));
    auto [out, runErr] = os::exec::Command(exe, string("traceback-panic"))->CombinedOutput();
    auto* exit = dynamic_cast<const os::exec::ExitError*>(runErr.get());
    EXPECT_TRUE(exit && exit->ExitCode() == 2) << runErr;
    ::unlink(exe.c_str());
    return std::string(asStringView(out));
}

}  // namespace

TEST(Traceback, PanicInFiber) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(
        {
            go([] { panicInFiber(); });
            block();
        },
        ExitedWithCode(2),
        "panic: boom\n"
        "\n"
        "goroutine [0-9]+ \\[running\\]:\n"
        "gopp::panic\\(\\.\\.\\.\\)\n\t[^\n]*src/runtime/proc\\.cpp:[0-9]+ \\+0x[0-9a-f]+\n"
        "\\(anonymous namespace\\)::panicInFiber\\(\\)" HERE
        LAMBDA
        "created by Traceback_PanicInFiber_Test::TestBody" HERE);
}

TEST(Traceback, PanicOnMainThread) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(panicInFiber(), ExitedWithCode(2),
                "panic: boom\n"
                "\n"
                "thread [0-9]+ \\(main\\) \\[running\\]:\n"
                "gopp::panic\\(\\.\\.\\.\\)\n\t[^\n]*\n"
                "\\(anonymous namespace\\)::panicInFiber\\(\\)" HERE
                "Traceback_PanicOnMainThread_Test::TestBody\\(\\)" HERE);
}

TEST(Traceback, ExceptionInFiberBeforeUnwinding) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(
        {
            go([] { throwUnderDestructor(); });
            block();
        },
        ExitedWithCode(2),
        "panic: std::runtime_error: boom\n"
        "\n"
        "goroutine [0-9]+ \\[running\\]:\n"
        "\\(anonymous namespace\\)::throwError\\(\\)" HERE
        "\\(anonymous namespace\\)::throwUnderDestructor\\(\\)" HERE
        LAMBDA
        "created by Traceback_ExceptionInFiberBeforeUnwinding_Test::TestBody" HERE);
}

TEST(Traceback, ExceptionOnThread) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(std::thread([] { throwUnderDestructor(); }).join(), ExitedWithCode(2),
                "panic: std::runtime_error: boom\n"
                "\n"
                "thread [0-9]+ \\[running\\]:\n"
                "\\(anonymous namespace\\)::throwError\\(\\)" HERE
                "\\(anonymous namespace\\)::throwUnderDestructor\\(\\)" HERE);
}

TEST(Traceback, NilDereference) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(
        {
            go([] { nilDeref(); });
            block();
        },
        ExitedWithCode(2),
        "panic: runtime error: invalid memory address or nil pointer dereference\n"
        "\\[signal SIGSEGV: segmentation violation code=0x1 addr=0x0 pc=0x[0-9a-f]+\\]\n"
        "\n"
        "goroutine [0-9]+ \\[running\\]:\n"
        "\\(anonymous namespace\\)::nilDeref\\(\\)" HERE
        "(" LAMBDA ")?"  // gone if nilDeref was a tail call
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
            startBlockedFibers();
            runtime::fatal("test");
        },
        ExitedWithCode(2),
        "fatal error: test\n"
        "\n"
        "thread [0-9]+ \\(main\\) \\[running\\]:\n"
        "(.*\n)*"
        "goroutine [0-9]+ \\[chan receive\\]:\n"
        "(.*\n)*"
        "\\(anonymous namespace\\)::recvForever\\(\\.\\.\\.\\)" HERE
        "(.*\n)*"
        "created by \\(anonymous namespace\\)::startBlockedFibers" HERE
        "\n"
        "goroutine [0-9]+ \\[sync.Mutex.Lock\\]:\n"
        "(.*\n)*"
        "\\(anonymous namespace\\)::lockForever\\(\\.\\.\\.\\)" HERE
        "(.*\n)*"
        "goroutine [0-9]+ \\[sleep\\]:\n"
        "(.*\n)*"
        "\\(anonymous namespace\\)::sleepForever\\(\\)" HERE);
}

TEST(Traceback, SigquitPrintsAllGoroutinesAndRegisters) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(
        {
            startBlockedFibers();
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
            go([] { panicInFiber(); });
            block();
        },
        ExitedWithCode(2), "panic: boom\n$");
}

TEST(Traceback, GotracebackSystemShowsRuntimeFrames) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(
        {
            ::setenv("GOTRACEBACK", "system", 1);
            go([] { panicInFiber(); });
            block();
        },
        ExitedWithCode(2), "goroutine [0-9]+ \\[running\\]:\n(.*\n)*gopp::runtime::runFiberFn\\(\\.\\.\\.\\)\n");
}

TEST(Traceback, GotracebackCrashDiesOfSigabrt) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_EXIT(
        {
            ::setenv("GOTRACEBACK", "crash", 1);
            panicInFiber();
        },
        testing::KilledBySignal(SIGABRT), "panic: boom\n");
}

// Without symbols or debug information, frames print as their addresses:
// the pc, and the address in the program that addr2line takes.
TEST(Traceback, StrippedProgramPrintsAddresses) {
    if (auto [_, err] = os::exec::LookPath("strip"); err) GTEST_SKIP() << "no strip: " << err;
    std::string exe;
    std::string out = runStripped({}, exe);
    const std::string addr = "\n\\? pc=0x[0-9a-f]+ \\(" + exe + "\\+0x[0-9a-f]+\\)\n";
    EXPECT_TRUE(std::regex_search(out, std::regex("panic: boom\n\ngoroutine [0-9]+ \\[running\\]:\n"
                                                  "(\\?\\(\\.\\.\\.\\)\n\t" + addr.substr(1) + ")+"
                                                  "created by \\?\n\t" + addr.substr(1) + "$")))
        << out;
}

// Without debug information, the names come from the symbol table, and the
// addresses stand in for the files and lines.
TEST(Traceback, ProgramWithoutDebugInfoPrintsNamesAndAddresses) {
    if (auto [_, err] = os::exec::LookPath("strip"); err) GTEST_SKIP() << "no strip: " << err;
    std::string exe;
    std::string out = runStripped({string("-g")}, exe);
    const std::string src = "\n\t\\? \\+0x[0-9a-f]+ pc=0x[0-9a-f]+ \\(" + exe + "\\+0x[0-9a-f]+\\)\n";
    EXPECT_TRUE(std::regex_search(out, std::regex("panic: boom\n\ngoroutine [0-9]+ \\[running\\]:\n"
                                                  "gopp::panic\\(\\.\\.\\.\\)" + src +
                                                  "\\(anonymous namespace\\)::panicInFiber\\(\\)" + src +
                                                  "[^\n]*operator\\(\\)[^\n]*" + src +
                                                  "created by [^\n]*" + src + "$")))
        << out;
}
