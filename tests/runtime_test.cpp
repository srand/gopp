#include <gopp/chan.hpp>
#include <gopp/runtime.hpp>
#include <gopp/sync.hpp>
#include <gopp/time.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <mutex>
#include <set>
#include <thread>

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
