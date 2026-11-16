// Ported from Go 1.26's math/big tests (int_test.go and intconv_test.go),
// with Go's test names, for the methods of big::Int that go++ has: Bytes,
// SetBytes, FillBytes, BitLen, Cmp, Sign, Int64, IsInt64, SetString,
// String and Text.
//
// Where Go uses testing/quick, these check random inputs from a seeded
// generator. TestCmp is Go's TestCmpAbs and TestIntCmpSelf with Cmp of
// signed values, as CmpAbs isn't ported; TestSetString keeps the cases of
// the bases 0 to 36 and 62 that stringTests has, and also checks Text of
// each value. SetStringFailureKeepsValue, Text and CopyAndMove are
// additions, for what Go's Int does not have. Left out: all
// arithmetic, formatting with fmt, JSON and text marshaling, Bits, the
// allocation tests and the benchmarks.
#include <gopp/math/big.hpp>

#include <gtest/gtest.h>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

using namespace gopp;

namespace {

std::string hex(std::span<const byte> b) {
    static const char d[] = "0123456789abcdef";
    std::string s;
    for (byte c : b) {
        s += d[c >> 4];
        s += d[c & 15];
    }
    return s;
}

// Random byte strings of up to 50 bytes, as testing/quick makes.
std::vector<std::vector<byte>> randomBytes(unsigned seed) {
    std::mt19937 rng(seed);
    std::vector<std::vector<byte>> r;
    for (int i = 0; i < 100; i++) {
        std::vector<byte> b(rng() % 51);
        for (byte& c : b) c = static_cast<byte>(rng());
        if (i % 7 == 0 && !b.empty()) b[0] = 0;  // leading zeros
        r.push_back(std::move(b));
    }
    return r;
}

TEST(GoBig, SetBytes) {
    for (auto& b : randomBytes(1)) {
        std::string hex1 = hex(big::Int().SetBytes(b).Bytes());
        std::string hex2 = hex(b);
        while (hex1.size() < hex2.size()) hex1 = "0" + hex1;
        while (hex1.size() > hex2.size()) hex2 = "0" + hex2;
        EXPECT_EQ(hex1, hex2);
    }
}

TEST(GoBig, Bytes) {
    for (auto b : randomBytes(2)) {
        // Trim leading zero bytes since Bytes() won't return them.
        size_t i = 0;
        while (i < b.size() && b[i] == 0) i++;
        std::vector<byte> want(b.begin() + static_cast<long>(i), b.end());
        slice<byte> got = big::Int().SetBytes(want).Bytes();
        EXPECT_EQ(hex(got), hex(want));
    }
}

const char* bitLenTests[][2] = {
    {"-1", "1"},
    {"0", "0"},
    {"1", "1"},
    {"2", "2"},
    {"4", "3"},
    {"0xabc", "12"},
    {"0x8000", "16"},
    {"0x80000000", "32"},
    {"0x800000000000", "48"},
    {"0x8000000000000000", "64"},
    {"0x80000000000000000000", "80"},
    {"-0x4000000000000000000000", "87"},
};

TEST(GoBig, BitLen) {
    for (auto& test : bitLenTests) {
        big::Int x;
        auto [p, ok] = x.SetString(test[0], 0);
        ASSERT_TRUE(ok) << test[0];
        EXPECT_EQ(x.BitLen(), std::atoi(test[1])) << test[0];
    }
}

const char* cmpAbsTests[] = {
    "0",
    "1",
    "2",
    "10",
    "10000000",
    "2783678367462374683678456387645876387564783686583485",
    "2783678367462374683678456387645876387564783686583486",
    "32957394867987420967976567076075976570670947609750670956097509670576075067076027578341538",
};

TEST(GoBig, Cmp) {
    std::vector<big::Int> values;
    for (const char* s : cmpAbsTests) {
        big::Int x;
        ASSERT_TRUE(x.SetString(s, 0).second) << s;
        // The self comparison of TestIntCmpSelf.
        EXPECT_EQ(x.Cmp(x), 0) << s;
        if (!values.empty()) {
            ASSERT_LT(values.back().Cmp(x), 0) << "cmpAbsTests entries not sorted";
        }
        values.push_back(x);
    }
    // Every combination of signs. Of the signed values, the one that is
    // negative or smaller in absolute value if both are positive is less.
    for (size_t i = 0; i < values.size(); i++) {
        for (size_t j = 0; j < values.size(); j++) {
            for (int k = 0; k < 4; k++) {
                bool xneg = k & 1, yneg = k & 2;
                big::Int a, b;
                ASSERT_TRUE(a.SetString(std::string(xneg ? "-" : "") + cmpAbsTests[i], 0).second);
                ASSERT_TRUE(b.SetString(std::string(yneg ? "-" : "") + cmpAbsTests[j], 0).second);
                // The sign of x - y, from the order of the absolute values.
                int want;
                int xs = i == 0 ? 0 : xneg ? -1 : 1, ys = j == 0 ? 0 : yneg ? -1 : 1;
                if (xs != ys) {
                    want = xs < ys ? -1 : 1;
                } else if (xs == 0) {
                    want = 0;
                } else {
                    want = i == j ? 0 : (i < j) == (xs > 0) ? -1 : 1;
                }
                EXPECT_EQ(a.Cmp(b), want) << a.String().view() << " " << b.String().view();
                EXPECT_EQ(a.Sign(), xs);
            }
        }
    }
}

const char* int64Tests[] = {
    // int64
    "0",
    "1",
    "-1",
    "4294967295",
    "-4294967295",
    "4294967296",
    "-4294967296",
    "9223372036854775807",
    "-9223372036854775807",
    "-9223372036854775808",

    // not int64
    "0x8000000000000000",
    "-0x8000000000000001",
    "38579843757496759476987459679745",
    "-38579843757496759476987459679745",
};

TEST(GoBig, Int64) {
    for (const char* s : int64Tests) {
        big::Int x;
        ASSERT_TRUE(x.SetString(s, 0).second) << s;

        errno = 0;
        long long want = std::strtoll(s, nullptr, 0);
        if (errno == ERANGE) {
            EXPECT_FALSE(x.IsInt64()) << "IsInt64(" << s << ") succeeded unexpectedly";
            continue;
        }
        EXPECT_TRUE(x.IsInt64()) << s;
        EXPECT_EQ(x.Int64(), want) << s;
    }
}

TEST(GoBig, NewIntMinInt64) {
    int64 want = INT64_MIN;
    EXPECT_EQ(big::NewInt(want)->Int64(), want);
    EXPECT_TRUE(big::NewInt(want)->IsInt64());
    EXPECT_EQ(big::NewInt(want)->String(), "-9223372036854775808");
}

TEST(GoBig, FillBytes) {
    for (const char* n : {
             "0",
             "1000",
             "0xffffffff",
             "-0xffffffff",
             "0xffffffffffffffff",
             "0x10000000000000000",
             "0xabababababababababababababababababababababababababa",
             "0xffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
         }) {
        SCOPED_TRACE(n);
        big::Int x;
        ASSERT_TRUE(x.SetString(n, 0).second);
        // The absolute value, which CmpAbs compares in Go.
        auto abs = x.Bytes();
        auto checkResult = [&](std::span<const byte> buf) {
            EXPECT_EQ(hex(big::Int().SetBytes(buf).Bytes()), hex(abs));
        };

        // Perfectly sized buffer.
        size_t byteLen = static_cast<size_t>((x.BitLen() + 7) / 8);
        std::vector<byte> buf(byteLen);
        checkResult(x.FillBytes(buf));
        checkResult(buf);

        // Way larger, checking all bytes get zeroed.
        buf.assign(100, 0xff);
        checkResult(x.FillBytes(buf));
        checkResult(buf);
    }
}

// FillBytes panics for a buffer that is too small.
TEST(GoBigDeathTest, FillBytes) {
    big::Int x;
    ASSERT_TRUE(x.SetString("0x10000000000000000", 0).second);
    std::vector<byte> buf(8);
    EXPECT_DEATH(x.FillBytes(buf), "buffer too small to fit value");
    buf.resize(9);
    EXPECT_EQ(x.FillBytes(buf).size(), 9u);
}

struct stringTest {
    const char* in;
    const char* out;
    int base;
    int64 val;
    bool ok;
};

const stringTest stringTests[] = {
    // invalid inputs
    {"", "", 0, 0, false},
    {"a", "", 0, 0, false},
    {"z", "", 0, 0, false},
    {"+", "", 0, 0, false},
    {"-", "", 0, 0, false},
    {"0b", "", 0, 0, false},
    {"0o", "", 0, 0, false},
    {"0x", "", 0, 0, false},
    {"0y", "", 0, 0, false},
    {"2", "", 2, 0, false},
    {"0b2", "", 0, 0, false},
    {"08", "", 0, 0, false},
    {"8", "", 8, 0, false},
    {"0xg", "", 0, 0, false},
    {"g", "", 16, 0, false},

    // invalid inputs with separators
    {"_", "", 0, 0, false},
    {"0_", "", 0, 0, false},
    {"_0", "", 0, 0, false},
    {"-1__0", "", 0, 0, false},
    {"0x10_", "", 0, 0, false},
    {"1_000", "", 10, 0, false},  // separators are not permitted for bases != 0
    {"d_e_a_d", "", 16, 0, false},

    // valid inputs
    {"0", "0", 0, 0, true},
    {"0", "0", 10, 0, true},
    {"0", "0", 16, 0, true},
    {"+0", "0", 0, 0, true},
    {"-0", "0", 0, 0, true},
    {"10", "10", 0, 10, true},
    {"10", "10", 10, 10, true},
    {"10", "10", 16, 16, true},
    {"-10", "-10", 16, -16, true},
    {"+10", "10", 16, 16, true},
    {"0b10", "2", 0, 2, true},
    {"0o10", "8", 0, 8, true},
    {"0x10", "16", 0, 16, true},
    {"0x10", "", 16, 0, false},
    {"-0x10", "-16", 0, -16, true},
    {"+0x10", "16", 0, 16, true},
    {"00", "0", 0, 0, true},
    {"0", "0", 8, 0, true},
    {"07", "7", 0, 7, true},
    {"7", "7", 8, 7, true},
    {"023", "19", 0, 19, true},
    {"23", "23", 8, 19, true},
    {"cafebabe", "cafebabe", 16, 0xcafebabe, true},
    {"0b0", "0", 0, 0, true},
    {"-111", "-111", 2, -7, true},
    {"-0b111", "-7", 0, -7, true},
    {"0b1001010111", "599", 0, 0x257, true},
    {"1001010111", "1001010111", 2, 0x257, true},
    {"A", "a", 36, 10, true},
    {"A", "A", 37, 36, true},
    {"ABCXYZ", "abcxyz", 36, 623741435, true},
    {"ABCXYZ", "ABCXYZ", 62, 33536793425, true},

    // valid input with separators
    {"1_000", "1000", 0, 1000, true},
    {"0b_1010", "10", 0, 10, true},
    {"+0o_660", "432", 0, 0660, true},
    {"-0xF00D_1E", "-15731998", 0, -0xf00d1e, true},
};

TEST(GoBig, SetString) {
    big::Int tmp;
    for (const auto& test : stringTests) {
        SCOPED_TRACE(std::string("input '") + test.in + "' base " + std::to_string(test.base));
        // Initialize to a non-zero value so that issues with parsing 0
        // are detected.
        tmp.SetInt64(1234567890);
        big::Int fresh;
        auto [n1, ok1] = fresh.SetString(test.in, test.base);
        auto [n2, ok2] = tmp.SetString(test.in, test.base);
        ASSERT_EQ(ok1, test.ok);
        ASSERT_EQ(ok2, test.ok);
        if (!test.ok) {
            EXPECT_EQ(n1, nullptr);
            EXPECT_EQ(n2, nullptr);
            continue;
        }
        EXPECT_EQ(n1, &fresh);
        EXPECT_EQ(n2, &tmp);
        auto expected = big::NewInt(test.val);
        EXPECT_EQ(fresh.Cmp(*expected), 0) << "got " << fresh.String().view();
        EXPECT_EQ(tmp.Cmp(*expected), 0) << "got " << tmp.String().view();
        // TestIntText: the text of the value in the base.
        if (test.base > 1) {
            EXPECT_EQ(fresh.Text(test.base), test.out);
        }
    }
}

TEST(GoBig, SetStringFailureKeepsValue) {
    big::Int x;
    x.SetInt64(42);
    EXPECT_FALSE(x.SetString("4x2", 10).second);
    EXPECT_EQ(x.Int64(), 42);
}

TEST(GoBig, Text) {
    big::Int x;
    ASSERT_TRUE(x.SetString("-0xdeadbeefcafebabe0123456789", 0).second);
    EXPECT_EQ(x.Text(16), "-deadbeefcafebabe0123456789");
    big::Int y;
    ASSERT_TRUE(y.SetString(x.Text(10).view(), 10).second);
    EXPECT_EQ(y.Cmp(x), 0);
    EXPECT_EQ(big::NewInt(0)->String(), "0");
    EXPECT_EQ(big::NewInt(255)->Text(2), "11111111");
}

TEST(GoBig, CopyAndMove) {
    big::Int a;
    a.SetString("123456789012345678901234567890", 10);
    big::Int b = a;
    a.SetInt64(7);
    EXPECT_EQ(b.String(), "123456789012345678901234567890");
    EXPECT_EQ(a.String(), "7");
    big::Int c = std::move(b);
    EXPECT_EQ(c.String(), "123456789012345678901234567890");
    // A moved-from Int reads as zero and can be set again.
    EXPECT_EQ(b.Sign(), 0);
    b = c;
    EXPECT_EQ(b.Cmp(c), 0);
    big::Int d;
    d = std::move(c);
    EXPECT_EQ(d.Cmp(b), 0);
}

}  // namespace
