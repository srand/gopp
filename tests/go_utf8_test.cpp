// Ported from Go 1.27.3's unicode/utf8/utf8_test.go, with Go's test names.
// Left out: the benchmarks, TestRuntimeConversion (of Go's runtime) and
// TestRuneCountNonASCIIAllocation.
#include <gopp/unicode.hpp>
#include <gopp/unicode/utf8.hpp>

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

using namespace gopp;
using namespace gopp::utf8;
using namespace std::literals;

namespace {

struct Utf8Map {
    rune r;
    std::string_view str;
};

struct RuneCountTest {
    std::string_view in;
    int out;
};

struct RuneLenTest {
    rune r;
    int size;
};

struct ValidTest {
    std::string in;
    bool out;
};

struct ValidRuneTest {
    rune r;
    bool ok;
};

std::string q(std::string_view s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        if (c >= 0x20 && c < 0x7f && c != '"' && c != '\\') {
            out += char(c);
        } else {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\x%02x", c);
            out += buf;
        }
    }
    return out + "\"";
}

std::span<const byte> b(std::string_view s) { return as_bytes(s); }

const std::vector<Utf8Map> utf8map = {
    {0x0000, "\x00"sv},
    {0x0001, "\x01"sv},
    {0x007e, "\x7e"sv},
    {0x007f, "\x7f"sv},
    {0x0080, "\xc2\x80"sv},
    {0x0081, "\xc2\x81"sv},
    {0x00bf, "\xc2\xbf"sv},
    {0x00c0, "\xc3\x80"sv},
    {0x00c1, "\xc3\x81"sv},
    {0x00c8, "\xc3\x88"sv},
    {0x00d0, "\xc3\x90"sv},
    {0x00e0, "\xc3\xa0"sv},
    {0x00f0, "\xc3\xb0"sv},
    {0x00f8, "\xc3\xb8"sv},
    {0x00ff, "\xc3\xbf"sv},
    {0x0100, "\xc4\x80"sv},
    {0x07ff, "\xdf\xbf"sv},
    {0x0400, "\xd0\x80"sv},
    {0x0800, "\xe0\xa0\x80"sv},
    {0x0801, "\xe0\xa0\x81"sv},
    {0x1000, "\xe1\x80\x80"sv},
    {0xd000, "\xed\x80\x80"sv},
    {0xd7ff, "\xed\x9f\xbf"sv}, // last code point before surrogate half.
    {0xe000, "\xee\x80\x80"sv}, // first code point after surrogate half.
    {0xfffe, "\xef\xbf\xbe"sv},
    {0xffff, "\xef\xbf\xbf"sv},
    {0x10000, "\xf0\x90\x80\x80"sv},
    {0x10001, "\xf0\x90\x80\x81"sv},
    {0x40000, "\xf1\x80\x80\x80"sv},
    {0x10fffe, "\xf4\x8f\xbf\xbe"sv},
    {0x10ffff, "\xf4\x8f\xbf\xbf"sv},
    {0xFFFD, "\xef\xbf\xbd"sv},
};

const std::vector<Utf8Map> surrogateMap = {
    {0xd800, "\xed\xa0\x80"sv}, // surrogate min decodes to (RuneError, 1)
    {0xdfff, "\xed\xbf\xbf"sv}, // surrogate max decodes to (RuneError, 1)
};

const std::vector<std::string_view> testStrings = {
    ""sv,
    "abcd"sv,
    "☺☻☹"sv,
    "日a本b語ç日ð本Ê語þ日¥本¼語i日©"sv,
    "日a本b語ç日ð本Ê語þ日¥本¼語i日©日a本b語ç日ð本Ê語þ日¥本¼語i日©日a本b語ç日ð本Ê語þ日¥本¼語i日©"sv,
    "\x80\x80\x80\x80"sv,
};

const std::vector<std::string_view> invalidSequenceTests = {
    "\xed\xa0\x80\x80"sv, // surrogate min
    "\xed\xbf\xbf\x80"sv, // surrogate max

    // xx
    "\x91\x80\x80\x80"sv,

    // s1
    "\xC2\x7F\x80\x80"sv,
    "\xC2\xC0\x80\x80"sv,
    "\xDF\x7F\x80\x80"sv,
    "\xDF\xC0\x80\x80"sv,

    // s2
    "\xE0\x9F\xBF\x80"sv,
    "\xE0\xA0\x7F\x80"sv,
    "\xE0\xBF\xC0\x80"sv,
    "\xE0\xC0\x80\x80"sv,

    // s3
    "\xE1\x7F\xBF\x80"sv,
    "\xE1\x80\x7F\x80"sv,
    "\xE1\xBF\xC0\x80"sv,
    "\xE1\xC0\x80\x80"sv,

    //s4
    "\xED\x7F\xBF\x80"sv,
    "\xED\x80\x7F\x80"sv,
    "\xED\x9F\xC0\x80"sv,
    "\xED\xA0\x80\x80"sv,

    // s5
    "\xF0\x8F\xBF\xBF"sv,
    "\xF0\x90\x7F\xBF"sv,
    "\xF0\x90\x80\x7F"sv,
    "\xF0\xBF\xBF\xC0"sv,
    "\xF0\xBF\xC0\x80"sv,
    "\xF0\xC0\x80\x80"sv,

    // s6
    "\xF1\x7F\xBF\xBF"sv,
    "\xF1\x80\x7F\xBF"sv,
    "\xF1\x80\x80\x7F"sv,
    "\xF1\xBF\xBF\xC0"sv,
    "\xF1\xBF\xC0\x80"sv,
    "\xF1\xC0\x80\x80"sv,

    // s7
    "\xF4\x7F\xBF\xBF"sv,
    "\xF4\x80\x7F\xBF"sv,
    "\xF4\x80\x80\x7F"sv,
    "\xF4\x8F\xBF\xC0"sv,
    "\xF4\x8F\xC0\x80"sv,
    "\xF4\x90\x80\x80"sv,
};

const std::vector<RuneCountTest> runecounttests = {
    {"abcd"sv, 4},
    {"☺☻☹"sv, 3},
    {"1,2,3,4"sv, 7},
    {"\xe2\x00"sv, 2},
    {"\xe2\x80"sv, 2},
    {"a\xe2\x80"sv, 3},
};

const std::vector<RuneLenTest> runelentests = {
    {0, 1},
    {U'e', 1},
    {U'é', 2},
    {U'☺', 3},
    {RuneError, 3},
    {MaxRune, 4},
    {0xD800, -1},
    {0xDFFF, -1},
    {MaxRune + 1, -1},
    {-1, -1},
};

const std::vector<ValidTest> validTests = {
    {""s, true},
    {"a"s, true},
    {"abc"s, true},
    {"Ж"s, true},
    {"ЖЖ"s, true},
    {"брэд-ЛГТМ"s, true},
    {"☺☻☹"s, true},
    {"aa\xe2"s, false},
    {"B\xfa"s, false},
    {"B\xfa" "C"s, false},
    {"a\uFFFDb"s, true},
    {"\xF4\x8F\xBF\xBF"s, true},      // U+10FFFF
    {"\xF4\x90\x80\x80"s, false},     // U+10FFFF+1; out of range
    {"\xF7\xBF\xBF\xBF"s, false},     // 0x1FFFFF; out of range
    {"\xFB\xBF\xBF\xBF\xBF"s, false}, // 0x3FFFFFF; out of range
    {"\xc0\x80"s, false},             // U+0000 encoded in two bytes: incorrect
    {"\xed\xa0\x80"s, false},         // U+D800 high surrogate (sic)
    {"\xed\xbf\xbf"s, false},         // U+DFFF low surrogate (sic)
};

const std::vector<ValidRuneTest> validrunetests = {
    {0, true},
    {U'e', true},
    {U'é', true},
    {U'☺', true},
    {RuneError, true},
    {MaxRune, true},
    {0xD7FF, true},
    {0xD800, false},
    {0xDFFF, false},
    {0xE000, true},
    {MaxRune + 1, false},
    {-1, false},
};

TEST(GoUtf8, Constants) {
    EXPECT_EQ(MaxRune, unicode::MaxRune) << "utf8.MaxRune is wrong";
    EXPECT_EQ(RuneError, unicode::ReplacementChar) << "utf8.RuneError is wrong";
}

TEST(GoUtf8, FullRune) {
    for (const auto& m : utf8map) {
        EXPECT_TRUE(FullRune(b(m.str))) << "FullRune(" << q(m.str) << ") = false, want true";
        EXPECT_TRUE(FullRuneInString(m.str)) << "FullRuneInString(" << q(m.str) << ") = false, want true";
        auto s1 = m.str.substr(0, m.str.size() - 1);
        EXPECT_FALSE(FullRune(b(s1))) << "FullRune(" << q(s1) << ") = true, want false";
        EXPECT_FALSE(FullRuneInString(s1)) << "FullRune(" << q(s1) << ") = true, want false";
    }
    for (auto s : {"\xc0"sv, "\xc1"sv}) {
        EXPECT_TRUE(FullRune(b(s))) << "FullRune(" << q(s) << ") = false, want true";
        EXPECT_TRUE(FullRuneInString(s)) << "FullRuneInString(" << q(s) << ") = false, want true";
    }
}

TEST(GoUtf8, EncodeRune) {
    for (const auto& m : utf8map) {
        byte buf[10] = {};
        int n = EncodeRune(buf, m.r);
        EXPECT_EQ(as_string_view(std::span<const byte>(buf, std::size_t(n))), m.str) << "EncodeRune(" << m.r << ")";
    }
}

TEST(GoUtf8, AppendRune) {
    for (const auto& m : utf8map) {
        EXPECT_EQ(as_string_view(AppendRune({}, m.r)), m.str) << "AppendRune(nil, " << m.r << ")";
        EXPECT_EQ(as_string_view(AppendRune(to_bytes("init"), m.r)), "init" + std::string(m.str))
            << "AppendRune(init, " << m.r << ")";
    }
}

TEST(GoUtf8, DecodeRune) {
    for (const auto& m : utf8map) {
        auto bb = to_bytes(m.str);
        auto [r, size] = DecodeRune(bb);
        EXPECT_TRUE(r == m.r && std::size_t(size) == bb.len()) << "DecodeRune(" << q(m.str) << ") = " << r << ", " << size;
        std::tie(r, size) = DecodeRuneInString(m.str);
        EXPECT_TRUE(r == m.r && std::size_t(size) == bb.len()) << "DecodeRuneInString(" << q(m.str) << ")";

        // there's an extra byte that bytes left behind - make sure trailing byte works
        auto extra = append(bb, byte(0));
        std::tie(r, size) = DecodeRune(extra);
        EXPECT_TRUE(r == m.r && std::size_t(size) == bb.len()) << "DecodeRune(" << q(m.str) << ")";
        std::string s = std::string(m.str) + '\0';
        std::tie(r, size) = DecodeRuneInString(s);
        EXPECT_TRUE(r == m.r && std::size_t(size) == bb.len()) << "DecodeRuneInString(" << q(s) << ")";

        // make sure missing bytes fail
        int wantsize = 1;
        if (std::size_t(wantsize) >= bb.len()) wantsize = 0;
        std::tie(r, size) = DecodeRune(bb.Sub(0, bb.len() - 1));
        EXPECT_TRUE(r == RuneError && size == wantsize) << "DecodeRune(" << q(m.str) << "[:len-1]) = " << r << ", " << size;
        auto s2 = m.str.substr(0, m.str.size() - 1);
        std::tie(r, size) = DecodeRuneInString(s2);
        EXPECT_TRUE(r == RuneError && size == wantsize) << "DecodeRuneInString(" << q(s2) << ")";

        // make sure bad sequences fail
        if (bb.len() == 1) {
            bb[0] = 0x80;
        } else {
            bb[bb.len() - 1] = 0x7F;
        }
        std::tie(r, size) = DecodeRune(bb);
        EXPECT_TRUE(r == RuneError && size == 1) << "DecodeRune(" << q(as_string_view(bb)) << ")";
        std::tie(r, size) = DecodeRuneInString(as_string_view(bb));
        EXPECT_TRUE(r == RuneError && size == 1) << "DecodeRuneInString(" << q(as_string_view(bb)) << ")";
    }
}

TEST(GoUtf8, DecodeSurrogateRune) {
    for (const auto& m : surrogateMap) {
        auto [r, size] = DecodeRune(b(m.str));
        EXPECT_TRUE(r == RuneError && size == 1) << "DecodeRune(" << q(m.str) << ")";
        std::tie(r, size) = DecodeRuneInString(m.str);
        EXPECT_TRUE(r == RuneError && size == 1) << "DecodeRuneInString(" << q(m.str) << ")";
    }
}

// The runes of s and their indexes, as Go's `for i, r := range s` visits
// them.
std::vector<std::pair<int, rune>> range_string(std::string_view s) {
    std::vector<std::pair<int, rune>> out;
    for (std::size_t i = 0; i < s.size();) {
        auto [r, n] = DecodeRuneInString(s.substr(i));
        out.push_back({int(i), r});
        i += std::size_t(n);
    }
    return out;
}

void testSequence(std::string_view s) {
    auto index = range_string(s);
    auto bb = b(s);
    int si = 0;
    for (auto [i, r] : index) {
        ASSERT_EQ(si, i) << "Sequence(" << q(s) << ") mismatched index";
        auto [r1, size1] = DecodeRune(bb.subspan(std::size_t(i)));
        ASSERT_EQ(r, r1) << "DecodeRune(" << q(s.substr(std::size_t(i))) << ")";
        auto [r2, size2] = DecodeRuneInString(s.substr(std::size_t(i)));
        ASSERT_EQ(r, r2) << "DecodeRuneInString(" << q(s.substr(std::size_t(i))) << ")";
        ASSERT_EQ(size1, size2) << "DecodeRune/DecodeRuneInString(" << q(s.substr(std::size_t(i))) << ") size mismatch";
        si += size1;
    }
    int j = int(index.size()) - 1;
    for (si = int(s.size()); si > 0;) {
        auto [r1, size1] = DecodeLastRune(bb.subspan(0, std::size_t(si)));
        auto [r2, size2] = DecodeLastRuneInString(s.substr(0, std::size_t(si)));
        ASSERT_EQ(size1, size2) << "DecodeLastRune/DecodeLastRuneInString(" << q(s) << ", " << si << ") size mismatch";
        ASSERT_EQ(r1, index[std::size_t(j)].second) << "DecodeLastRune(" << q(s) << ", " << si << ")";
        ASSERT_EQ(r2, index[std::size_t(j)].second) << "DecodeLastRuneInString(" << q(s) << ", " << si << ")";
        si -= size1;
        ASSERT_EQ(si, index[std::size_t(j)].first) << "DecodeLastRune(" << q(s) << ") index mismatch";
        j--;
    }
    EXPECT_EQ(si, 0) << "DecodeLastRune(" << q(s) << ") finished at " << si << ", not 0";
}

// Check that DecodeRune and DecodeLastRune correspond to the equivalent
// range loop.
TEST(GoUtf8, Sequencing) {
    for (auto ts : testStrings) {
        for (const auto& m : utf8map) {
            for (const auto& s : {std::string(ts) + std::string(m.str), std::string(m.str) + std::string(ts),
                                  std::string(ts) + std::string(m.str) + std::string(ts)}) {
                testSequence(s);
            }
        }
    }
}

TEST(GoUtf8, DecodeInvalidSequence) {
    for (auto s : invalidSequenceTests) {
        auto [r1, n1] = DecodeRune(b(s));
        ASSERT_EQ(r1, RuneError) << "DecodeRune(" << q(s) << ")";
        auto [r2, n2] = DecodeRuneInString(s);
        ASSERT_EQ(r2, RuneError) << "DecodeRuneInString(" << q(s) << ")";
        ASSERT_EQ(r1, r2);
    }
}

// Check that negative runes encode as U+FFFD.
TEST(GoUtf8, NegativeRune) {
    byte errorbuf[UTFMax], buf[UTFMax];
    int n1 = EncodeRune(errorbuf, RuneError);
    int n2 = EncodeRune(buf, -1);
    EXPECT_EQ(as_string_view(std::span<const byte>(buf, std::size_t(n2))),
              as_string_view(std::span<const byte>(errorbuf, std::size_t(n1))));
}

TEST(GoUtf8, RuneCount) {
    for (const auto& tt : runecounttests) {
        EXPECT_EQ(RuneCountInString(tt.in), tt.out) << "RuneCountInString(" << q(tt.in) << ")";
        EXPECT_EQ(RuneCount(b(tt.in)), tt.out) << "RuneCount(" << q(tt.in) << ")";
    }
}

TEST(GoUtf8, RuneLen) {
    for (const auto& tt : runelentests) EXPECT_EQ(RuneLen(tt.r), tt.size) << "RuneLen(" << tt.r << ")";
}

TEST(GoUtf8, Valid) {
    auto tests = validTests;
    for (std::size_t i = 0; i < 100; i++) {
        std::string a(i, 'a'), bs(i, 'b');
        tests.push_back({a, true});
        tests.push_back({a + "Ж", true});
        tests.push_back({a + "\xe2", false});
        tests.push_back({a + "Ж" + bs, true});
        tests.push_back({a + "\xe2" + bs, false});
    }
    for (const auto& tt : tests) {
        EXPECT_EQ(Valid(b(tt.in)), tt.out) << "Valid(" << q(tt.in) << ")";
        EXPECT_EQ(ValidString(tt.in), tt.out) << "ValidString(" << q(tt.in) << ")";
    }
}

TEST(GoUtf8, ValidRune) {
    for (const auto& tt : validrunetests) EXPECT_EQ(ValidRune(tt.r), tt.ok) << "ValidRune(" << tt.r << ")";
}

}  // namespace
