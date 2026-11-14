// Ported from Go 1.27.3's crypto/sha256, crypto/sha512, crypto/sha1,
// crypto/md5 and crypto/hmac tests, with Go's test names; the vectors are
// in go_hash_testdata.hpp. Left out: the tests of marshaling hash states
// (TestGoldenMarshal, TestLargeHashes and the mismatch tests), the
// allocation, cgo and Cloner tests, the tests of Go's own block functions,
// and cryptotest.TestHash.
#include <gopp/crypto/hmac.hpp>
#include <gopp/crypto/md5.hpp>
#include <gopp/crypto/sha1.hpp>
#include <gopp/crypto/sha256.hpp>
#include <gopp/crypto/sha512.hpp>
#include <gopp/encoding/hex.hpp>
#include <gopp/io.hpp>

#include "go_hash_testdata.hpp"

#include <gtest/gtest.h>

#include <functional>
#include <string>
#include <vector>

using namespace gopp;
using namespace go_hash_testdata;

namespace {

std::string x(std::span<const byte> b) { return std::string(hex::EncodeToString(b).view()); }

// Go's testGolden: the one-shot sum, then the running hash written whole
// twice and in two halves with a Sum between them, with Reset after each.
template <std::size_t N>
void testGolden(const char* name, std::span<const golden_test> golden,
                std::function<std::array<byte, N>(std::span<const byte>)> sum, ptr<hash::Hash> c) {
    for (const auto& g : golden) {
        ASSERT_EQ(x(sum(as_bytes(g.in))), g.out) << "Sum function: " << name << "(" << g.in << ")";
        for (int j = 0; j < 3; j++) {
            if (j < 2) {
                io::WriteString(*c, g.in);
            } else {
                io::WriteString(*c, g.in.substr(0, g.in.size() / 2));
                c->Sum(bytes());
                io::WriteString(*c, g.in.substr(g.in.size() / 2));
            }
            ASSERT_EQ(x(c->Sum(bytes())), g.out) << name << "[" << j << "](" << g.in << ")";
            c->Reset();
        }
    }
}

// Go's TestLarge and TestExtraLarge: "0123456789" repeated, written in
// blocks of several sizes at several offsets.
void testLarge(ptr<hash::Hash> c, int N, const std::string& ok) {
    constexpr int offsets = 4;
    std::vector<byte> block(static_cast<std::size_t>(N + offsets));
    for (int offset = 0; offset < offsets; offset++) {
        for (int i = 0; i < N; i++) block[static_cast<std::size_t>(offset + i)] = static_cast<byte>('0' + i % 10);
        for (int blockSize = 10; blockSize <= N; blockSize *= 10) {
            int blocks = N / blockSize;
            std::span<const byte> b(block.data() + offset, static_cast<std::size_t>(blockSize));
            c->Reset();
            for (int i = 0; i < blocks; i++) c->Write(b);
            ASSERT_EQ(x(c->Sum(bytes())), ok) << "offset=" << offset << ", blockSize=" << blockSize;
        }
    }
}

// ---------------------------------------------------------------------------
// crypto/sha256

TEST(GoSha256, Golden) {
    testGolden<32>("sha256", sha256_golden, [](std::span<const byte> b) { return sha256::Sum256(b); }, sha256::New());
    testGolden<28>("sha224", sha256_golden224, [](std::span<const byte> b) { return sha256::Sum224(b); },
                   sha256::New224());
}

TEST(GoSha256, Large) {
    // sha256sum of "0123456789" * 1000
    testLarge(sha256::New(), 10000, "4c207598af7a20db0e3334dd044399a40e467cb81b37f7ba05a4f76dcbd8fd59");
}

TEST(GoSha256, ExtraLarge) {
    // sha256sum of "0123456789" * 10000
    testLarge(sha256::New(), 100000, "aca9e593cc629cbaa94cd5a07dc029424aad93e5129e5d11f8dcd2f139c16cc0");
}

TEST(GoSha256, Size) {
    EXPECT_EQ(sha256::New()->Size(), sha256::Size);
    EXPECT_EQ(sha256::New224()->Size(), sha256::Size224);
}

TEST(GoSha256, BlockSize) { EXPECT_EQ(sha256::New()->BlockSize(), sha256::BlockSize); }

// ---------------------------------------------------------------------------
// crypto/sha512

TEST(GoSha512, Golden) {
    testGolden<28>("SHA512/224", sha512_golden224, [](std::span<const byte> b) { return sha512::Sum512_224(b); },
                   sha512::New512_224());
    testGolden<32>("SHA512/256", sha512_golden256, [](std::span<const byte> b) { return sha512::Sum512_256(b); },
                   sha512::New512_256());
    testGolden<48>("SHA384", sha512_golden384, [](std::span<const byte> b) { return sha512::Sum384(b); },
                   sha512::New384());
    testGolden<64>("SHA512", sha512_golden512, [](std::span<const byte> b) { return sha512::Sum512(b); },
                   sha512::New());
}

TEST(GoSha512, Size) {
    EXPECT_EQ(sha512::New()->Size(), sha512::Size);
    EXPECT_EQ(sha512::New384()->Size(), sha512::Size384);
    EXPECT_EQ(sha512::New512_224()->Size(), sha512::Size224);
    EXPECT_EQ(sha512::New512_256()->Size(), sha512::Size256);
}

TEST(GoSha512, BlockSize) { EXPECT_EQ(sha512::New()->BlockSize(), sha512::BlockSize); }

// ---------------------------------------------------------------------------
// crypto/sha1

TEST(GoSha1, Golden) {
    testGolden<20>("sha1", sha1_golden, [](std::span<const byte> b) { return sha1::Sum(b); }, sha1::New());
}

TEST(GoSha1, Size) { EXPECT_EQ(sha1::New()->Size(), sha1::Size); }

TEST(GoSha1, BlockSize) { EXPECT_EQ(sha1::New()->BlockSize(), sha1::BlockSize); }

// ---------------------------------------------------------------------------
// crypto/md5

TEST(GoMd5, Golden) {
    for (const auto& g : md5_golden) {
        ASSERT_EQ(x(md5::Sum(as_bytes(g.in))), g.out) << "Sum function: md5(" << g.in << ")";
        auto c = md5::New();
        std::vector<byte> buf(g.in.size() + 4);
        std::size_t start = 0;
        for (int j = 0; j < 3 + 4; j++) {
            if (j < 2) {
                io::WriteString(*c, g.in);
            } else if (j == 2) {
                io::WriteString(*c, g.in.substr(0, g.in.size() / 2));
                c->Sum(bytes());
                io::WriteString(*c, g.in.substr(g.in.size() / 2));
            } else {
                // test unaligned write
                start++;
                std::copy(g.in.begin(), g.in.end(), buf.begin() + static_cast<std::ptrdiff_t>(start));
                c->Write(std::span<const byte>(buf.data() + start, g.in.size()));
            }
            ASSERT_EQ(x(c->Sum(bytes())), g.out) << "md5[" << j << "](" << g.in << ")";
            c->Reset();
        }
    }
}

TEST(GoMd5, Large) {
    // md5sum of "0123456789" * 1000
    testLarge(md5::New(), 10000, "2bb571599a4180e1d542f76904adc3df");
}

TEST(GoMd5, ExtraLarge) {
    // md5sum of "0123456789" * 10000
    testLarge(md5::New(), 100000, "13572e9e296cff52b79c52148313c3a5");
}

// ---------------------------------------------------------------------------
// crypto/hmac

// justHash has just the hash::Hash methods of the hash it wraps.
struct justHash final : hash::Hash {
    explicit justHash(ptr<hash::Hash> h) : h(std::move(h)) {}
    ptr<hash::Hash> h;
    std::pair<isize, error> Write(std::span<const byte> p) override { return h->Write(p); }
    bytes Sum(bytes b) const override { return h->Sum(std::move(b)); }
    void Reset() override { h->Reset(); }
    isize Size() const override { return h->Size(); }
    isize BlockSize() const override { return h->BlockSize(); }
};

TEST(GoHmac, HMAC) {
    int i = 0;
    for (const auto& tt : hmacTests) {
        auto h = hmac::New(tt.hash, tt.key);
        EXPECT_EQ(h->Size(), tt.size) << "Size";
        EXPECT_EQ(h->BlockSize(), tt.blocksize) << "BlockSize";
        for (int j = 0; j < 4; j++) {
            auto [n, err] = h->Write(tt.in);
            ASSERT_TRUE(n == static_cast<isize>(tt.in.size()) && !err) << "test " << i << "." << j;

            // Repetitive Sum() calls should return the same value
            for (int k = 0; k < 2; k++) {
                EXPECT_EQ(x(h->Sum(bytes())), tt.out) << "test " << i << "." << j << "." << k;
            }

            // Second iteration: make sure reset works.
            h->Reset();

            // Third and fourth iteration: make sure hmac works on hashes
            // that are only hash::Hash.
            if (j == 1) h = hmac::New([&] { return ptr<hash::Hash>(make_ptr<justHash>(tt.hash())); }, tt.key);
        }
        i++;
    }
}

TEST(GoHmacDeathTest, NonUniqueHash) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    auto sha = sha256::New();
    EXPECT_DEATH(hmac::New([&] { return sha; }, as_bytes("bytes")), "does not produce unique values");
}

TEST(GoHmac, Equal) {
    auto a = as_bytes("test");
    auto b = as_bytes("test1");
    auto c = as_bytes("test2");
    EXPECT_TRUE(hmac::Equal(b, b)) << "Equal failed with equal arguments";
    EXPECT_FALSE(hmac::Equal(a, b)) << "Equal accepted a prefix of the second argument";
    EXPECT_FALSE(hmac::Equal(b, a)) << "Equal accepted a prefix of the first argument";
    EXPECT_FALSE(hmac::Equal(b, c)) << "Equal accepted unequal slices";
}

}  // namespace
