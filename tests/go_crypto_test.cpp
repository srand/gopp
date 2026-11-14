// Ported from Go 1.27.3's crypto/subtle and crypto/rand tests, with Go's
// test names.
//
// Where Go uses testing/quick, these check random inputs from a seeded
// generator. TestXORBytes takes Go's short-mode steps and fills its
// buffers from that generator rather than crypto/rand, which would make
// it read the kernel's randomness millions of times. TestRead checks the
// byte distribution where Go checks that compress/flate can't shrink the
// output. Left out: the allocation tests, TestXORBytesBoundary (Go's
// guard-page buffers), the tests that replace rand.Reader, and
// TestReadError and TestSynctest, which need Go's runtime.
#include <gopp/crypto/rand.hpp>
#include <gopp/crypto/subtle.hpp>
#include <gopp/runtime.hpp>
#include <gopp/sync.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

using namespace gopp;

namespace {

// ---------------------------------------------------------------------------
// crypto/subtle

TEST(GoSubtle, ConstantTimeCompare) {
    struct {
        std::vector<byte> a, b;
        int out;
    } tests[] = {
        {{}, {}, 1},
        {{0x11}, {0x11}, 1},
        {{0x12}, {0x11}, 0},
        {{0x11}, {0x11, 0x12}, 0},
        {{0x11, 0x12}, {0x11}, 0},
    };
    int i = 0;
    for (const auto& test : tests) EXPECT_EQ(subtle::ConstantTimeCompare(test.a, test.b), test.out) << "#" << i++;
}

TEST(GoSubtle, ConstantTimeByteEq) {
    struct {
        uint8 a, b;
        int out;
    } tests[] = {{0, 0, 1}, {0, 1, 0}, {1, 0, 0}, {0xff, 0xff, 1}, {0xff, 0xfe, 0}};
    int i = 0;
    for (const auto& test : tests) EXPECT_EQ(subtle::ConstantTimeByteEq(test.a, test.b), test.out) << "#" << i++;
    // All pairs, where Go checks random ones.
    for (int a = 0; a < 256; a++) {
        for (int b = 0; b < 256; b++) {
            ASSERT_EQ(subtle::ConstantTimeByteEq(uint8(a), uint8(b)), a == b ? 1 : 0) << a << ", " << b;
        }
    }
}

TEST(GoSubtle, ConstantTimeEq) {
    std::mt19937 rng(1);
    for (int i = 0; i < 10000; i++) {
        auto a = static_cast<int32>(rng());
        auto b = i % 2 ? a : static_cast<int32>(rng());
        ASSERT_EQ(subtle::ConstantTimeEq(a, b), a == b ? 1 : 0) << a << ", " << b;
    }
}

TEST(GoSubtle, ConstantTimeCopy) {
    std::mt19937 rng(1);
    for (int i = 0; i < 1000; i++) {
        std::size_t n = rng() % 64;
        std::vector<byte> x(n), y(n);
        for (auto& b : x) b = static_cast<byte>(rng());
        for (auto& b : y) b = static_cast<byte>(rng());
        int v = static_cast<int>(rng() & 1);
        auto want = v == 1 ? y : x;
        subtle::ConstantTimeCopy(v, x, y);
        ASSERT_EQ(x, want);
    }
}

TEST(GoSubtle, ConstantTimeLessOrEq) {
    struct {
        isize x, y;
        int result;
    } tests[] = {{0, 0, 1}, {1, 0, 0}, {0, 1, 1}, {10, 20, 1}, {20, 10, 0}, {10, 10, 1}};
    int i = 0;
    for (const auto& test : tests) {
        EXPECT_EQ(subtle::ConstantTimeLessOrEq(test.x, test.y), test.result) << "#" << i++;
    }
    // And at the ends of the range, which Go 1.27 supports.
    constexpr isize mx = std::numeric_limits<isize>::max(), mn = std::numeric_limits<isize>::min();
    EXPECT_EQ(subtle::ConstantTimeLessOrEq(mn, mx), 1);
    EXPECT_EQ(subtle::ConstantTimeLessOrEq(mx, mn), 0);
    EXPECT_EQ(subtle::ConstantTimeLessOrEq(-1, 0), 1);
    EXPECT_EQ(subtle::ConstantTimeLessOrEq(0, -1), 0);
}

TEST(GoSubtle, ConstantTimeSelect) {
    EXPECT_EQ(subtle::ConstantTimeSelect(1, 5, 7), 5);
    EXPECT_EQ(subtle::ConstantTimeSelect(0, 5, 7), 7);
    EXPECT_EQ(subtle::ConstantTimeSelect(1, -3, 9), -3);
}

TEST(GoSubtle, XORBytes) {
    std::mt19937 rng(1);
    auto fill = [&](std::span<byte> b) {
        for (auto& c : b) c = static_cast<byte>(rng());
    };
    for (std::size_t n = 1; n <= 1024; n++) {
        if (n > 16) n += n >> 3;  // Go's short mode
        for (std::size_t alignP = 0; alignP < 8; alignP++) {
            for (std::size_t alignQ = 0; alignQ < 8; alignQ++) {
                for (std::size_t alignD = 0; alignD < 8; alignD++) {
                    std::vector<byte> pb(alignP + n + 100), qb(alignQ + n + 100);
                    std::span<byte> p(pb.data() + alignP, n), q(qb.data() + alignQ, n);
                    fill(p);
                    fill(q);

                    std::vector<byte> d(alignD + n + 100);
                    fill(d);

                    auto want = d;
                    for (std::size_t i = 0; i < n; i++) want[alignD + i] = p[i] ^ q[i];
                    std::vector<byte> wantN(want.begin() + static_cast<std::ptrdiff_t>(alignD),
                                            want.begin() + static_cast<std::ptrdiff_t>(alignD + n));
                    std::vector<byte> zero(n);
                    auto eq = [](std::span<const byte> a, std::span<const byte> b) { return std::ranges::equal(a, b); };

                    ASSERT_EQ(subtle::XORBytes(std::span(d).subspan(alignD), p, q), static_cast<isize>(n));
                    ASSERT_EQ(d, want) << "n=" << n << " alignP=" << alignP << " alignQ=" << alignQ << " alignD=" << alignD;
                    std::vector<byte> p1(p.begin(), p.end());
                    ASSERT_EQ(subtle::XORBytes(p, p, q), static_cast<isize>(n));
                    ASSERT_TRUE(eq(p, wantN));
                    ASSERT_EQ(subtle::XORBytes(q, p1, q), static_cast<isize>(n));
                    ASSERT_TRUE(eq(q, wantN));
                    ASSERT_EQ(subtle::XORBytes(p, p, p), static_cast<isize>(n));
                    ASSERT_TRUE(eq(p, zero));
                    ASSERT_EQ(subtle::XORBytes(p1, q, q), static_cast<isize>(n));
                    ASSERT_TRUE(eq(p1, zero));
                }
            }
        }
    }
}

TEST(GoSubtleDeathTest, XorBytesPanic) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    std::vector<byte> one(1), two(2), three(3);
    EXPECT_DEATH(subtle::XORBytes({}, one, one), "subtle.XORBytes: dst too short");
    EXPECT_DEATH(subtle::XORBytes(one, two, three), "subtle.XORBytes: dst too short");
    EXPECT_DEATH(
        {
            std::vector<byte> x(3);
            subtle::XORBytes(x, std::span(x).subspan(1), two);
        },
        "subtle.XORBytes: invalid overlap");
    EXPECT_DEATH(
        {
            std::vector<byte> x(3);
            subtle::XORBytes(x, two, std::span(x).subspan(1));
        },
        "subtle.XORBytes: invalid overlap");
}

// ---------------------------------------------------------------------------
// crypto/rand

// Go's testReadAndReader: each test runs with Read and with Reader.Read.
template <typename F>
void testReadAndReader(F f) {
    f([](std::span<byte> b) { return rand::Read(b); });
    f([](std::span<byte> b) { return rand::Reader.Read(b); });
}

TEST(GoRand, Read) {
    testReadAndReader([](auto Read) {
        std::vector<byte> b(4'000'000);
        auto [n, err] = Read(b);
        ASSERT_TRUE(n == static_cast<isize>(b.size()) && !err) << "Read(buf) = " << n << ", " << err;
        // Every byte value about as often as the others.
        std::array<std::size_t, 256> count{};
        for (byte c : b) count[c]++;
        double want = b.size() / 256.0;
        for (std::size_t c : count) ASSERT_NEAR(c, want, want * 0.1);
    });
}

TEST(GoRand, ReadByteValues) {
    testReadAndReader([](auto Read) {
        byte b[1];
        std::set<byte> v;
        while (v.size() < 256) {
            auto [n, err] = Read(b);
            ASSERT_TRUE(n == 1 && !err);
            v.insert(b[0]);
        }
    });
}

TEST(GoRand, LargeRead) {
    testReadAndReader([](auto Read) {
        // 40MiB, more than the documented maximum of 32Mi-1 on Linux 32-bit.
        std::vector<byte> b(40 << 20);
        auto [n, err] = Read(b);
        ASSERT_FALSE(err);
        EXPECT_EQ(n, static_cast<isize>(b.size()));
    });
}

TEST(GoRand, ReadEmpty) {
    testReadAndReader([](auto Read) {
        auto [n, err] = Read(std::span<byte>());
        EXPECT_TRUE(n == 0 && !err);
    });
}

TEST(GoRand, ConcurrentRead) {
    testReadAndReader([](auto Read) {
        constexpr int N = 100, M = 1000;
        sync::WaitGroup wg;
        wg.Add(N);
        std::atomic<int> failures{0};
        for (int i = 0; i < N; i++) {
            go([&] {
                for (int j = 0; j < M; j++) {
                    byte b[32];
                    auto [n, err] = Read(b);
                    if (n != 32 || err) failures++;
                }
                wg.Done();
            });
        }
        wg.Wait();
        EXPECT_EQ(failures.load(), 0);
    });
}

TEST(GoRand, Text) {
    std::set<std::string> set;  // hold every string produced
    std::array<std::map<char, int>, 26> indexSet;  // hold every char produced at every position

    // not getting a char in a position: (31/32)¹⁰⁰⁰ = 1.6e-14
    // test completion within 1000 rounds: (1-(31/32)¹⁰⁰⁰)²⁶ = 0.9999999999996
    // empirically, this should complete within 400 rounds = 0.999921
    constexpr int rounds = 1000;
    bool done = false;
    for (int round = 0; round < rounds; round++) {
        std::string s(rand::Text().view());
        EXPECT_EQ(s.size(), 26u) << "len(Text())";
        for (std::size_t i = 0; i < s.size(); i++) {
            char r = s[i];
            EXPECT_FALSE(('A' > r || r > 'Z') && ('2' > r || r > '7')) << "Text()[" << i << "] = " << r;
        }
        EXPECT_TRUE(set.insert(s).second) << "Text() = " << s << ", duplicate of previously produced string";

        done = true;
        for (std::size_t i = 0; i < s.size(); i++) {
            indexSet[i][s[i]]++;
            if (indexSet[i].size() != 32) done = false;
        }
        if (done) break;
    }
    EXPECT_TRUE(done) << "failed to produce every char at every index after " << rounds << " rounds";
}

}  // namespace
