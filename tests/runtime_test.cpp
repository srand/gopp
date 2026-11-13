#include <gopp/chan.hpp>
#include <gopp/runtime.hpp>
#include <gopp/sync.hpp>
#include <gopp/time.hpp>

#include <gtest/gtest.h>

#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace gopp;
using namespace std::chrono_literals;

TEST(Runtime, GoRunsFiber) {
    auto done = make_chan<int>();
    go([done] { done.send(42); });
    EXPECT_EQ(done.recv(), 42);
}

TEST(Runtime, GoWithArguments) {
    auto done = make_chan<int>();
    go([](chan<int> c, int a, int b) { c.send(a + b); }, done, 40, 2);
    EXPECT_EQ(done.recv(), 42);
}

TEST(Runtime, ManyFibers) {
    constexpr int n = 10000;
    std::atomic<int> count{0};
    sync::WaitGroup wg;
    for (int i = 0; i < n; i++) {
        wg.Go([&] { count.fetch_add(1); });
    }
    wg.Wait();
    EXPECT_EQ(count.load(), n);
}

TEST(Runtime, FibersRunOnSeveralThreads) {
    if (runtime::GOMAXPROCS() < 2) GTEST_SKIP() << "needs GOMAXPROCS >= 2";
    std::mutex mu;
    std::set<std::thread::id> threads;
    sync::WaitGroup wg;
    for (int i = 0; i < 64; i++) {
        wg.Go([&] {
            // Busy for a moment so that other Ms steal work.
            auto end = std::chrono::steady_clock::now() + 2ms;
            while (std::chrono::steady_clock::now() < end) {
            }
            std::lock_guard l(mu);
            threads.insert(std::this_thread::get_id());
        });
    }
    wg.Wait();
    EXPECT_GT(threads.size(), 1u);
}

TEST(Runtime, GoschedYields) {
    std::atomic<int> turns{0};
    sync::WaitGroup wg;
    for (int i = 0; i < 4; i++) {
        wg.Go([&] {
            for (int j = 0; j < 100; j++) {
                turns.fetch_add(1);
                runtime::Gosched();
            }
        });
    }
    wg.Wait();
    EXPECT_EQ(turns.load(), 400);
}

TEST(Runtime, FiberIdsAreDistinct) {
    auto ids = make_chan<uint64>(2);
    go([ids] { ids.send(runtime::goid()); });
    go([ids] { ids.send(runtime::goid()); });
    uint64 a = ids.recv(), b = ids.recv();
    EXPECT_NE(a, 0u);
    EXPECT_NE(b, 0u);
    EXPECT_NE(a, b);
    EXPECT_EQ(runtime::goid(), 0u);  // the test's thread isn't a fiber
}

TEST(Runtime, BlockingCallHandsOffProcessor) {
    // With GOMAXPROCS fibers stuck in blocking calls, others still run.
    int procs = runtime::GOMAXPROCS();
    auto release = make_chan<empty>();
    sync::WaitGroup wg;
    for (int i = 0; i < procs; i++) {
        wg.Go([&] {
            runtime::blocking([] { std::this_thread::sleep_for(50ms); });
        });
    }
    auto ran = make_chan<int>(1);
    go([ran] { ran.send(1); });
    EXPECT_EQ(ran.recv(), 1);
    wg.Wait();
}

TEST(Runtime, PingPong) {
    auto ping = make_chan<int>();
    auto pong = make_chan<int>();
    go([ping, pong] {
        for (int v : ping) pong.send(v + 1);
        pong.close();
    });
    int v = 0;
    for (int i = 0; i < 10000; i++) {
        ping.send(v);
        v = pong.recv();
    }
    ping.close();
    EXPECT_EQ(v, 10000);
    auto [x, ok] = pong.recv2();
    EXPECT_FALSE(ok);
}

// ---------------------------------------------------------------------------
// Preemption: a P that runs one fiber for 10ms while other work waits is
// handed to another thread.

namespace {

// Busy for d, without a call into the runtime.
void spin_for(std::chrono::milliseconds d) {
    auto end = std::chrono::steady_clock::now() + d;
    while (std::chrono::steady_clock::now() < end) {
    }
}

// Receives from c within d; false on timeout.
template <typename T>
bool recv_within(const chan<T>& c, time::Duration d, T* out = nullptr) {
    bool got = false;
    select(recv(c,
                [&](T v) {
                    got = true;
                    if (out) *out = v;
                }),
           recv(time::After(d), [] {}));
    return got;
}

// Runs this test program's test `name` as a child with the extra
// environment variables (ahead of the inherited ones, so they win) and
// returns its wait status. After run_child in go_os_pipe_test.cpp.
int run_child(const std::string& name, const std::vector<std::string>& env) {
    std::string filter = "--gtest_filter=" + name;
    std::vector<std::string> envs = env;
    // Not the parent's GTEST_ settings: a shard or output file of the
    // parent's would make the child run nothing, or overwrite its report.
    for (char** e = environ; *e; e++) {
        if (!std::string_view(*e).starts_with("GTEST_")) envs.emplace_back(*e);
    }
    std::vector<char*> envp;
    for (auto& e : envs) envp.push_back(e.data());
    envp.push_back(nullptr);
    char self[] = "/proc/self/exe";
    char* argv[] = {self, filter.data(), nullptr};
    pid_t pid = ::fork();
    if (pid == 0) {
        ::execve(self, argv, envp.data());
        ::_exit(127);
    }
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    return status;
}

}  // namespace

TEST(Runtime, PreemptSpinnersDontStarveSleeper) {
    sync::WaitGroup wg;
    for (int i = 0; i < runtime::GOMAXPROCS(); i++) wg.Go([] { spin_for(500ms); });
    auto start = time::Now();
    auto took = make_chan<time::Duration>(1);
    go([took, start] {
        time::Sleep(10 * time::Millisecond);
        took.send(time::Since(start));
    });
    time::Duration elapsed = took.recv();
    EXPECT_LT(elapsed.ns, (100 * time::Millisecond).ns) << elapsed.String();
    wg.Wait();
}

TEST(Runtime, PreemptSpinnersDontStarveNewFiber) {
    sync::WaitGroup wg;
    for (int i = 0; i < runtime::GOMAXPROCS(); i++) wg.Go([] { spin_for(500ms); });
    std::this_thread::sleep_for(20ms);  // let the spinners take every P
    auto ran = make_chan<int>(1);
    go([ran] {
        auto ping = make_chan<int>();
        auto pong = make_chan<int>();
        go([ping, pong] {
            for (int v : ping) pong.send(v + 1);
            pong.close();
        });
        int v = 0;
        for (int i = 0; i < 100; i++) {
            ping.send(v);
            v = pong.recv();
        }
        ping.close();
        ran.send(v);
    });
    int v = 0;
    EXPECT_TRUE(recv_within(ran, 100 * time::Millisecond, &v));
    EXPECT_EQ(v, 100);
    wg.Wait();
}

TEST(Runtime, PreemptedFiberParksAndResumes) {
    // Each spinner loses its P while another fiber waits, then parks, is
    // woken and spins again on whatever P it gets back.
    int procs = runtime::GOMAXPROCS();
    std::vector<chan<int>> cs;
    for (int i = 0; i < procs; i++) cs.push_back(make_chan<int>(1));
    auto done = make_chan<int>(procs);
    for (int i = 0; i < procs; i++) {
        go([c = cs[i], done, i] {
            spin_for(300ms);
            int v = c.recv();
            spin_for(50ms);
            done.send(i + v);
        });
    }
    std::this_thread::sleep_for(20ms);
    auto waiter = make_chan<int>(1);
    go([waiter] { waiter.send(1); });
    EXPECT_TRUE(recv_within(waiter, 100 * time::Millisecond));
    std::this_thread::sleep_for(300ms);
    for (auto& c : cs) c.send(1000);
    std::set<int> got;
    for (int k = 0; k < procs; k++) {
        int v = 0;
        if (recv_within(done, 2 * time::Second, &v)) got.insert(v);
    }
    EXPECT_EQ(got.size(), static_cast<std::size_t>(procs));
}

TEST(Runtime, PreemptUnderChurn) {
    // Fibers that start fibers for 15ms at a time without parking, so
    // that SIGURG lands while they're in newproc and runqput; then they
    // sleep, get a P back and do it again. Every fiber must still run.
    std::atomic<int64_t> spawned{0}, ran{0};
    sync::WaitGroup children, workers;
    for (int w = 0; w < 2 * runtime::GOMAXPROCS(); w++) {
        workers.Go([&] {
            for (int round = 0; round < 20; round++) {
                auto end = std::chrono::steady_clock::now() + 15ms;
                while (std::chrono::steady_clock::now() < end) {
                    spawned.fetch_add(1);
                    children.Go([&] { ran.fetch_add(1); });
                }
                time::Sleep(time::Millisecond);
            }
        });
    }
    workers.Wait();
    children.Wait();
    EXPECT_GT(spawned.load(), 0);
    EXPECT_EQ(ran.load(), spawned.load());
}

TEST(Runtime, PreemptBoundsThreads) {
    // Many fibers that each compute for 100ms: sysmon takes Ps from at
    // most GOMAXPROCS of them at a time, so no more than about twice
    // GOMAXPROCS run at once, where one thread per fiber would run them all.
    int procs = runtime::GOMAXPROCS();
    std::atomic<int> running{0}, most{0};
    sync::WaitGroup wg;
    for (int i = 0; i < 8 * procs; i++) {
        wg.Go([&] {
            int n = running.fetch_add(1) + 1;
            for (int m = most.load(); n > m && !most.compare_exchange_weak(m, n);) {
            }
            spin_for(100ms);
            running.fetch_sub(1);
        });
    }
    wg.Wait();
    EXPECT_GT(most.load(), procs);           // some were preempted
    EXPECT_LE(most.load(), 2 * procs + 1);  // a request may be in flight
}

TEST(Runtime, PreemptWithSignalsBlocked) {
    if (std::getenv("GOPP_TEST_PREEMPT_CHILD")) {
        // SIGURG blocked before the runtime starts its threads, as a
        // program that waits for signals with sigwait does: the threads
        // unblock it for themselves.
        sigset_t urg;
        sigemptyset(&urg);
        sigaddset(&urg, SIGURG);
        pthread_sigmask(SIG_BLOCK, &urg, nullptr);
        sync::WaitGroup wg;
        wg.Go([] { spin_for(300ms); });
        auto start = time::Now();
        auto took = make_chan<time::Duration>(1);
        go([took, start] {
            time::Sleep(10 * time::Millisecond);
            took.send(time::Since(start));
        });
        time::Duration elapsed = took.recv();
        wg.Wait();
        std::_Exit(elapsed < 150 * time::Millisecond ? 0 : 1);
    }
    int status = run_child("Runtime.PreemptWithSignalsBlocked", {"GOPP_TEST_PREEMPT_CHILD=1", "GOMAXPROCS=1"});
    ASSERT_TRUE(WIFEXITED(status)) << status;
    EXPECT_EQ(WEXITSTATUS(status), 0) << "the spinner wasn't preempted with SIGURG blocked in main";
}

TEST(Runtime, RawBlockingSyscallLosesP) {
    // Fibers blocked in a system call made without runtime::blocking keep
    // their thread, but not their P.
    sync::WaitGroup wg;
    for (int i = 0; i < runtime::GOMAXPROCS(); i++) {
        wg.Go([] {
            auto end = std::chrono::steady_clock::now() + 300ms;
            for (;;) {
                auto left = std::chrono::duration_cast<std::chrono::milliseconds>(end - std::chrono::steady_clock::now());
                if (left.count() <= 0) break;
                ::poll(nullptr, 0, static_cast<int>(left.count()));  // EINTR from SIGURG: go on
            }
        });
    }
    std::this_thread::sleep_for(20ms);
    auto ran = make_chan<int>(1);
    go([ran] { ran.send(1); });
    EXPECT_TRUE(recv_within(ran, 100 * time::Millisecond));
    wg.Wait();
}

TEST(Runtime, PreemptOffSwitch) {
    if (std::getenv("GOPP_TEST_PREEMPT_CHILD")) {
        // GOMAXPROCS=1 and GODEBUG=asyncpreemptoff=1: the sleeper waits
        // for the spinner.
        sync::WaitGroup wg;
        wg.Go([] { spin_for(300ms); });
        auto start = time::Now();
        auto took = make_chan<time::Duration>(1);
        go([took, start] {
            time::Sleep(10 * time::Millisecond);
            took.send(time::Since(start));
        });
        time::Duration elapsed = took.recv();
        wg.Wait();
        std::_Exit(elapsed >= 250 * time::Millisecond ? 0 : 1);
    }
    int status = run_child("Runtime.PreemptOffSwitch",
                           {"GOPP_TEST_PREEMPT_CHILD=1", "GODEBUG=asyncpreemptoff=1", "GOMAXPROCS=1"});
    ASSERT_TRUE(WIFEXITED(status)) << status;
    EXPECT_EQ(WEXITSTATUS(status), 0) << "the fiber was preempted with asyncpreemptoff=1";
}
