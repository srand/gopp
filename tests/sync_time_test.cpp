#include <gopp/chan.hpp>
#include <gopp/context.hpp>
#include <gopp/runtime.hpp>
#include <gopp/sync.hpp>
#include <gopp/time.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <mutex>

using namespace gopp;
using namespace std::chrono_literals;

TEST(Sync, MutexExcludes) {
    sync::Mutex mu;
    long counter = 0;
    sync::WaitGroup wg;
    for (int i = 0; i < 16; i++) {
        wg.Go([&] {
            for (int j = 0; j < 10000; j++) {
                std::lock_guard l(mu);
                counter++;
            }
        });
    }
    wg.Wait();
    EXPECT_EQ(counter, 160000);
}

TEST(Sync, MutexParksWhileHeld) {
    // Holding a lock across a sleep must not block other fibers that don't
    // need it.
    sync::Mutex mu;
    mu.lock();
    auto got = make_chan<int>();
    go([&] {
        std::lock_guard l(mu);
        got.send(1);
    });
    auto other = make_chan<int>();
    go([other] { other.send(2); });
    EXPECT_EQ(other.recv(), 2);
    time::Sleep(5ms);
    mu.unlock();
    EXPECT_EQ(got.recv(), 1);
}

TEST(Sync, RWMutex) {
    sync::RWMutex mu;
    int value = 0;
    std::atomic<int> readers{0}, maxReaders{0};
    sync::WaitGroup wg;
    for (int i = 0; i < 8; i++) {
        wg.Go([&] {
            for (int j = 0; j < 200; j++) {
                mu.RLock();
                int r = readers.fetch_add(1) + 1;
                int m = maxReaders.load();
                while (r > m && !maxReaders.compare_exchange_weak(m, r)) {
                }
                runtime::Gosched();
                readers.fetch_sub(1);
                mu.RUnlock();
            }
        });
    }
    for (int i = 0; i < 2; i++) {
        wg.Go([&] {
            for (int j = 0; j < 200; j++) {
                mu.lock();
                EXPECT_EQ(readers.load(), 0);
                value++;
                mu.unlock();
            }
        });
    }
    wg.Wait();
    EXPECT_EQ(value, 400);
}

TEST(Sync, Once) {
    sync::Once once;
    std::atomic<int> calls{0};
    sync::WaitGroup wg;
    for (int i = 0; i < 10; i++) wg.Go([&] { once.Do([&] { calls.fetch_add(1); }); });
    wg.Wait();
    EXPECT_EQ(calls.load(), 1);
}

// After Go's TestCondSignal: each signal wakes exactly one waiter.
TEST(Sync, CondSignal) {
    sync::Mutex m;
    sync::Cond c(&m);
    constexpr int n = 2;
    auto running = make_chan<bool>(n);
    auto awake = make_chan<bool>(n);
    for (int i = 0; i < n; i++) {
        go([&] {
            m.lock();
            running.send(true);
            c.Wait();
            awake.send(true);
            m.unlock();
        });
    }
    for (int i = 0; i < n; i++) running.recv();  // wait for everyone to run
    for (int left = n; left > 0; left--) {
        bool b;
        EXPECT_FALSE(awake.tryRecv(b)) << "fiber not asleep";
        m.lock();
        c.Signal();
        m.unlock();
        awake.recv();  // will deadlock if no fiber wakes up
        EXPECT_FALSE(awake.tryRecv(b)) << "too many fibers awake";
    }
    c.Signal();
}

// After Go's TestCondBroadcast: a broadcast wakes all waiters.
TEST(Sync, CondBroadcast) {
    sync::Mutex m;
    sync::Cond c(&m);
    constexpr int n = 100;
    int generation = 0;
    sync::WaitGroup wg;
    auto running = make_chan<bool>(n);
    for (int i = 0; i < n; i++) {
        wg.Go([&] {
            m.lock();
            int gen = generation;
            running.send(true);
            while (generation == gen) c.Wait();
            m.unlock();
        });
    }
    for (int i = 0; i < n; i++) running.recv();
    m.lock();
    generation++;
    c.Broadcast();
    m.unlock();
    wg.Wait();
}

TEST(Time, SleepInFiber) {
    auto done = make_chan<time::Duration>();
    go([done] {
        auto start = time::Now();
        time::Sleep(20ms);
        done.send(time::Since(start));
    });
    auto d = done.recv();
    EXPECT_GE(d.Milliseconds(), 19);
    EXPECT_LT(d.Milliseconds(), 500);
}

TEST(Time, SleepOnForeignThread) {
    auto start = time::Now();
    time::Sleep(10ms);
    EXPECT_GE(time::Since(start).Milliseconds(), 9);
}

TEST(Time, ManySleepers) {
    sync::WaitGroup wg;
    auto start = time::Now();
    for (int i = 0; i < 1000; i++) wg.Go([i] { time::Sleep(time::Millisecond * (i % 20)); });
    wg.Wait();
    EXPECT_LT(time::Since(start).Milliseconds(), 1000);
}

TEST(Time, TimerStop) {
    auto t = time::NewTimer(50ms);
    EXPECT_TRUE(t->Stop());
    EXPECT_FALSE(t->Stop());
    time::Time v;
    EXPECT_FALSE(t->C.tryRecv(v));
}

TEST(Time, AfterFunc) {
    auto done = make_chan<int>();
    auto t = time::AfterFunc(5ms, [done] { done.send(1); });
    EXPECT_EQ(done.recv(), 1);
}

TEST(Time, Ticker) {
    auto t = time::NewTicker(5ms);
    for (int i = 0; i < 3; i++) t->C.recv();
    t->Stop();
}

TEST(Time, DurationString) {
    EXPECT_EQ(time::Duration(0).String(), "0s");
    EXPECT_EQ((1500 * time::Millisecond).String(), "1.5s");
    EXPECT_EQ((2 * time::Hour + 3 * time::Minute).String(), "2h3m0s");
    EXPECT_EQ((250 * time::Microsecond).String(), "250\xc2\xb5s");
    EXPECT_EQ(time::Duration(100ms).String(), "100ms");
}

TEST(Context, Cancel) {
    auto [ctx, cancel] = context::WithCancel(context::Background());
    EXPECT_FALSE(ctx->Err());
    auto [child, cancelChild] = context::WithCancel(ctx);
    auto woke = make_chan<bool>();
    go([child, woke] {
        child->Done().recv();
        woke.send(true);
    });
    cancel();
    EXPECT_TRUE(woke.recv());
    EXPECT_EQ(ctx->Err(), context::Canceled);
    EXPECT_EQ(child->Err(), context::Canceled);
    cancelChild();
}

TEST(Context, Timeout) {
    auto [ctx, cancel] = context::WithTimeout(context::Background(), 10ms);
    auto start = time::Now();
    ctx->Done().recv();
    EXPECT_GE(time::Since(start).Milliseconds(), 9);
    EXPECT_EQ(ctx->Err(), context::DeadlineExceeded);
    EXPECT_TRUE(ctx->Err()->Timeout());
    cancel();
}

TEST(Context, Value) {
    static int key;
    auto ctx = context::WithValue(context::Background(), &key, context::Background());
    EXPECT_TRUE(ctx->Value(&key));
    EXPECT_FALSE(ctx->Value(nullptr));
}
