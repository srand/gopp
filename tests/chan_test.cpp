#include <gopp/chan.hpp>
#include <gopp/runtime.hpp>
#include <gopp/string.hpp>
#include <gopp/sync.hpp>
#include <gopp/time.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <memory>

using namespace gopp;
using namespace std::chrono_literals;

TEST(Chan, Buffered) {
    auto c = makeChan<int>(3);
    c.send(1);
    c.send(2);
    c.send(3);
    EXPECT_EQ(c.len(), 3u);
    EXPECT_EQ(c.cap(), 3u);
    EXPECT_EQ(c.recv(), 1);
    EXPECT_EQ(c.recv(), 2);
    EXPECT_EQ(c.recv(), 3);
}

TEST(Chan, CloseDrainsThenZero) {
    auto c = makeChan<int>(2);
    c.send(7);
    c.close();
    auto [v, ok] = c.recv2();
    EXPECT_EQ(v, 7);
    EXPECT_TRUE(ok);
    auto [w, ok2] = c.recv2();
    EXPECT_EQ(w, 0);
    EXPECT_FALSE(ok2);
}

TEST(Chan, CloseWakesReceivers) {
    auto c = makeChan<int>();
    auto results = makeChan<bool>(3);
    for (int i = 0; i < 3; i++) {
        go([c, results] { results.send(c.recv2().second); });
    }
    time::Sleep(10ms);
    c.close();
    for (int i = 0; i < 3; i++) EXPECT_FALSE(results.recv());
}

TEST(Chan, MoveOnlyValues) {
    auto c = makeChan<std::unique_ptr<int>>(1);
    c.send(std::make_unique<int>(5));
    auto p = c.recv();
    ASSERT_TRUE(p);
    EXPECT_EQ(*p, 5);
}

TEST(Chan, StringValues) {
    auto c = makeChan<string>();
    go([c] {
        c.send("hello");
        c.send("world"_s);
        c.close();
    });
    std::string all;
    for (auto& s : c) all += s.view();
    EXPECT_EQ(all, "helloworld");
}

TEST(Chan, TrySendRecv) {
    auto c = makeChan<int>(1);
    EXPECT_TRUE(c.trySend(1));
    EXPECT_FALSE(c.trySend(2));
    int v = 0;
    EXPECT_TRUE(c.tryRecv(v));
    EXPECT_EQ(v, 1);
    bool closed = true;
    EXPECT_FALSE(c.tryRecv(v, &closed));
    EXPECT_FALSE(closed);
    c.close();
    EXPECT_FALSE(c.tryRecv(v, &closed));
    EXPECT_TRUE(closed);
}

TEST(Chan, ManyProducersConsumers) {
    auto c = makeChan<int>(16);
    constexpr int producers = 8, per = 5000;
    std::atomic<long> sum{0};
    sync::WaitGroup prod, cons;
    for (int p = 0; p < producers; p++) {
        prod.Go([c] {
            for (int i = 1; i <= per; i++) c.send(i);
        });
    }
    for (int k = 0; k < 4; k++) {
        cons.Go([c, &sum] {
            for (int v : c) sum.fetch_add(v);
        });
    }
    prod.Wait();
    c.close();
    cons.Wait();
    EXPECT_EQ(sum.load(), long(producers) * per * (per + 1) / 2);
}

TEST(Select, DefaultWhenNothingReady) {
    auto c = makeChan<int>();
    bool def = false;
    select(recv(c, [](int) { FAIL(); }), defaultCase([&] { def = true; }));
    EXPECT_TRUE(def);
}

TEST(Select, PicksReadyCase) {
    auto a = makeChan<int>(1);
    auto b = makeChan<int>(1);
    b.send(2);
    int got = 0;
    select(recv(a, [&](int v) { got = v; }), recv(b, [&](int v) { got = v * 10; }));
    EXPECT_EQ(got, 20);
}

TEST(Select, SendCase) {
    auto a = makeChan<int>(1);
    bool sent = false;
    select(send(a, 9, [&] { sent = true; }), defaultCase([] {}));
    EXPECT_TRUE(sent);
    EXPECT_EQ(a.recv(), 9);
}

TEST(Select, BlocksUntilReady) {
    auto a = makeChan<int>();
    auto b = makeChan<string>();
    go([b] {
        time::Sleep(5ms);
        b.send("x");
    });
    string got;
    select(recv(a, [](int) {}), recv(b, [&](string s) { got = s; }));
    EXPECT_EQ(got, "x");
}

TEST(Select, ClosedChannelReceivesZero) {
    auto a = makeChan<int>();
    a.close();
    bool ok = true;
    select(recv(a, [&](int v, bool k) {
        EXPECT_EQ(v, 0);
        ok = k;
    }));
    EXPECT_FALSE(ok);
}

TEST(Select, Timeout) {
    auto never = makeChan<int>();
    bool timed_out = false;
    auto start = time::Now();
    select(recv(never, [](int) {}), recv(time::After(20ms), [&](time::Time) { timed_out = true; }));
    EXPECT_TRUE(timed_out);
    EXPECT_GE(time::Since(start).Milliseconds(), 19);
}

TEST(Select, FairBetweenReadyCases) {
    auto a = makeChan<int>(1000);
    auto b = makeChan<int>(1000);
    for (int i = 0; i < 1000; i++) {
        a.send(1);
        b.send(1);
    }
    int na = 0, nb = 0;
    for (int i = 0; i < 1000; i++) {
        select(recv(a, [&](int) { na++; }), recv(b, [&](int) { nb++; }));
    }
    EXPECT_GT(na, 300);
    EXPECT_GT(nb, 300);
}

TEST(Select, ConcurrentSelectsOnSharedChannels) {
    // Fibers select across the same channels in different orders; every
    // value must be delivered exactly once.
    auto a = makeChan<int>();
    auto b = makeChan<int>();
    std::atomic<int> received{0};
    sync::WaitGroup wg;
    constexpr int n = 4000;
    for (int k = 0; k < 4; k++) {
        wg.Go([&, a, b] {
            for (;;) {
                bool done = false;
                select(recv(a, [&](int, bool ok) {
                           if (ok) received.fetch_add(1);
                           else done = true;
                       }),
                       recv(b, [&](int, bool ok) {
                           if (ok) received.fetch_add(1);
                           else done = true;
                       }));
                if (done) return;
            }
        });
    }
    sync::WaitGroup senders;
    senders.Go([&, a] {
        for (int i = 0; i < n; i++) a.send(i);
    });
    senders.Go([&, b] {
        for (int i = 0; i < n; i++) b.send(i);
    });
    senders.Wait();
    a.close();
    b.close();
    wg.Wait();
    EXPECT_EQ(received.load(), 2 * n);
}
