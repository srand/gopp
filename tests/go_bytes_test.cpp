// Ported from Go 1.27.3's bytes/bytes_test.go and compare_test.go, with
// Go's test names. Left out: the benchmarks, the allocation counts, the
// exhaustive and randomized tests (TestEqualExhaustive, TestNotEqual,
// TestIndexByteBig, TestCountByte, TestIndexRandom, ...), FuzzReplace and
// TestTitle (Title is deprecated and not ported).
#include <gopp/bytes.hpp>
#include <gopp/unicode.hpp>
#include <gopp/unicode/utf8.hpp>

#include <gtest/gtest.h>

#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <vector>

using namespace gopp;
using namespace std::literals;

namespace {

// A Go []byte in the test tables: nullopt for nil.
using bs = std::optional<std::string>;
bs bytes_of(std::string s) { return s; }
const bs empty_bytes = ""s;

slice<byte> b(const bs& v) { return v ? toBytes(*v) : slice<byte>(); }
slice<byte> b(const std::string& s) { return toBytes(s); }
std::string str(std::span<const byte> s) { return std::string(asStringView(s)); }
std::string q(const bs& v) { return v ? "\"" + *v + "\"" : "nil"; }

std::vector<std::string> sliceOfString(const slice<slice<byte>>& s) {
    std::vector<std::string> result;
    for (const auto& v : s) result.push_back(str(v));
    return result;
}

template <typename G>
std::vector<std::string> collect(G&& seq) {
    std::vector<std::string> out;
    for (auto&& v : seq) out.push_back(str(v));
    return out;
}

int len(const std::string& s) { return int(s.size()); }
std::string repeat(const std::string& s, int n) {
    std::string r;
    for (int i = 0; i < n; i++) r += s;
    return r;
}
constexpr isize max_int = std::numeric_limits<isize>::max();

struct LinesTest {
    std::string a;
    std::vector<std::string> b;
};

// For ease of reading, the test cases use strings that are converted to
// byte slices before invoking the functions.

const std::string abcd = "abcd";
const std::string faces = "☺☻☹";
const std::string commas = "1,2,3,4";
const std::string dots = "1....2....3....4";

struct BinOpTest {
    std::string a;
    std::string b;
    int i;
};

struct SplitTest {
    std::string s;
    std::string sep;
    isize n;
    std::vector<std::string> a;
};

struct FieldsTest {
    std::string s;
    std::vector<std::string> a;
};

// Test case for any function which accepts and returns a byte slice. For
// ease of creation, we write the input byte slice as a string.
struct StringTest {
    std::string in;
    bs out;
};

const std::string space = "\t\v\r\f\n\u0085  　";

struct ToValidUTF8Test {
    std::string in, repl, out;
};

struct RunesTest {
    std::string in;
    std::vector<rune> out;
    bool lossy;
};

struct TrimTest {
    std::string f;
    std::string in, arg, out;
};

struct TrimNilTest {
    std::string f;
    bs in;
    std::string arg;
    bs out;
};

struct predicate {
    std::function<bool(rune)> f;
    std::string name;
};

const predicate isSpace{unicode::IsSpace, "IsSpace"};
const predicate isDigit{unicode::IsDigit, "IsDigit"};
const predicate isUpper{unicode::IsUpper, "IsUpper"};
const predicate isValidRune{[](rune r) { return r != utf8::RuneError; }, "IsValidRune"};

predicate not_(const predicate& p) {
    return {[f = p.f](rune r) { return !f(r); }, "not " + p.name};
}

struct TrimFuncTest {
    predicate f;
    std::string in;
    bs trimOut;
    bs leftOut;
    bs rightOut;
};

struct IndexFuncTest {
    std::string in;
    predicate f;
    int first, last;
};

struct ReplaceTest {
    std::string in;
    std::string old, new_;
    isize n;
    std::string out;
};

struct TitleTest {
    std::string in, out;
};

struct EqualFoldTest {
    std::string s, t;
    bool out;
};

struct CutTest {
    std::string s, sep;
    std::string before, after;
    bool found;
};

struct CutPrefixTest {
    std::string s, sep;
    std::string after;
    bool found;
};

struct CutSuffixTest {
    std::string s, sep;
    std::string before;
    bool found;
};

struct ContainsTest {
    bs b, subslice;
    bool want;
};

struct ContainsAnyTest {
    bs b;
    std::string substr;
    bool expected;
};

struct ContainsRuneTest {
    bs b;
    rune r;
    bool expected;
};

struct CompareTest {
    bs a, b;
    int i;
};

const std::vector<LinesTest> linesTests = {
    {.a = "abc\nabc\n"s, .b = {"abc\n"s, "abc\n"s}},
    {.a = "abc\r\nabc"s, .b = {"abc\r\n"s, "abc"s}},
    {.a = "abc\r\n"s, .b = {"abc\r\n"s}},
    {.a = "\nabc"s, .b = {"\n"s, "abc"s}},
    {.a = "\nabc\n\n"s, .b = {"\n"s, "abc\n"s, "\n"s}},
};

const std::vector<BinOpTest> indexTests = {
    {""s, ""s, 0},
    {""s, "a"s, -1},
    {""s, "foo"s, -1},
    {"fo"s, "foo"s, -1},
    {"foo"s, "baz"s, -1},
    {"foo"s, "foo"s, 0},
    {"oofofoofooo"s, "f"s, 2},
    {"oofofoofooo"s, "foo"s, 4},
    {"barfoobarfoo"s, "foo"s, 3},
    {"foo"s, ""s, 0},
    {"foo"s, "o"s, 1},
    {"abcABCabc"s, "A"s, 3},
    // cases with one byte strings - test IndexByte and special case in Index()
    {""s, "a"s, -1},
    {"x"s, "a"s, -1},
    {"x"s, "x"s, 0},
    {"abc"s, "a"s, 0},
    {"abc"s, "b"s, 1},
    {"abc"s, "c"s, 2},
    {"abc"s, "x"s, -1},
    {"barfoobarfooyyyzzzyyyzzzyyyzzzyyyxxxzzzyyy"s, "x"s, 33},
    {"fofofofooofoboo"s, "oo"s, 7},
    {"fofofofofofoboo"s, "ob"s, 11},
    {"fofofofofofoboo"s, "boo"s, 12},
    {"fofofofofofoboo"s, "oboo"s, 11},
    {"fofofofofoooboo"s, "fooo"s, 8},
    {"fofofofofofoboo"s, "foboo"s, 10},
    {"fofofofofofoboo"s, "fofob"s, 8},
    {"fofofofofofofoffofoobarfoo"s, "foffof"s, 12},
    {"fofofofofoofofoffofoobarfoo"s, "foffof"s, 13},
    {"fofofofofofofoffofoobarfoo"s, "foffofo"s, 12},
    {"fofofofofoofofoffofoobarfoo"s, "foffofo"s, 13},
    {"fofofofofoofofoffofoobarfoo"s, "foffofoo"s, 13},
    {"fofofofofofofoffofoobarfoo"s, "foffofoo"s, 12},
    {"fofofofofoofofoffofoobarfoo"s, "foffofoob"s, 13},
    {"fofofofofofofoffofoobarfoo"s, "foffofoob"s, 12},
    {"fofofofofoofofoffofoobarfoo"s, "foffofooba"s, 13},
    {"fofofofofofofoffofoobarfoo"s, "foffofooba"s, 12},
    {"fofofofofoofofoffofoobarfoo"s, "foffofoobar"s, 13},
    {"fofofofofofofoffofoobarfoo"s, "foffofoobar"s, 12},
    {"fofofofofoofofoffofoobarfoo"s, "foffofoobarf"s, 13},
    {"fofofofofofofoffofoobarfoo"s, "foffofoobarf"s, 12},
    {"fofofofofoofofoffofoobarfoo"s, "foffofoobarfo"s, 13},
    {"fofofofofofofoffofoobarfoo"s, "foffofoobarfo"s, 12},
    {"fofofofofoofofoffofoobarfoo"s, "foffofoobarfoo"s, 13},
    {"fofofofofofofoffofoobarfoo"s, "foffofoobarfoo"s, 12},
    {"fofofofofoofofoffofoobarfoo"s, "ofoffofoobarfoo"s, 12},
    {"fofofofofofofoffofoobarfoo"s, "ofoffofoobarfoo"s, 11},
    {"fofofofofoofofoffofoobarfoo"s, "fofoffofoobarfoo"s, 11},
    {"fofofofofofofoffofoobarfoo"s, "fofoffofoobarfoo"s, 10},
    {"fofofofofoofofoffofoobarfoo"s, "foobars"s, -1},
    {"foofyfoobarfoobar"s, "y"s, 4},
    {"oooooooooooooooooooooo"s, "r"s, -1},
    {"oxoxoxoxoxoxoxoxoxoxoxoy"s, "oy"s, 22},
    {"oxoxoxoxoxoxoxoxoxoxoxox"s, "oy"s, -1},
    // test fallback to Rabin-Karp.
    {"000000000000000000000000000000000000000000000000000000000000000000000001"s, "0000000000000000000000000000000000000000000000000000000000000000001"s, 5},
    // test fallback to IndexRune
    {"oxoxoxoxoxoxoxoxoxoxox☺"s, "☺"s, 22},
    // invalid UTF-8 byte sequence (must be longer than bytealg.MaxBruteForce to
    // test that we don't use IndexRune)
    {"xx0123456789012345678901234567890123456789012345678901234567890120123456789012345678901234567890123456xxx\xed\x9f\xc0"s, "\xed\x9f\xc0"s, 105},
};

const std::vector<BinOpTest> lastIndexTests = {
    {""s, ""s, 0},
    {""s, "a"s, -1},
    {""s, "foo"s, -1},
    {"fo"s, "foo"s, -1},
    {"foo"s, "foo"s, 0},
    {"foo"s, "f"s, 0},
    {"oofofoofooo"s, "f"s, 7},
    {"oofofoofooo"s, "foo"s, 7},
    {"barfoobarfoo"s, "foo"s, 9},
    {"foo"s, ""s, 3},
    {"foo"s, "o"s, 2},
    {"abcABCabc"s, "A"s, 3},
    {"abcABCabc"s, "a"s, 6},
};

const std::vector<BinOpTest> indexAnyTests = {
    {""s, ""s, -1},
    {""s, "a"s, -1},
    {""s, "abc"s, -1},
    {"a"s, ""s, -1},
    {"a"s, "a"s, 0},
    {"\x80"s, "\xff" "b"s, 0},
    {"aaa"s, "a"s, 0},
    {"abc"s, "xyz"s, -1},
    {"abc"s, "xcz"s, 2},
    {"ab☺c"s, "x☺yz"s, 2},
    {"a☺b☻c☹d"s, "cx"s, len("a☺b☻"s)},
    {"a☺b☻c☹d"s, "uvw☻xyz"s, len("a☺b"s)},
    {"aRegExp*"s, ".(|)*+?^$[]"s, 7},
    {dots + dots + dots, " "s, -1},
    {"012abcba210"s, "\xff" "b"s, 4},
    {"012\x80" "bcb\x80" "210"s, "\xff" "b"s, 3},
    {"0123456\xcf\x80" "abc"s, "\xcf" "b\x80"s, 10},
};

const std::vector<BinOpTest> lastIndexAnyTests = {
    {""s, ""s, -1},
    {""s, "a"s, -1},
    {""s, "abc"s, -1},
    {"a"s, ""s, -1},
    {"a"s, "a"s, 0},
    {"\x80"s, "\xff" "b"s, 0},
    {"aaa"s, "a"s, 2},
    {"abc"s, "xyz"s, -1},
    {"abc"s, "ab"s, 1},
    {"ab☺c"s, "x☺yz"s, 2},
    {"a☺b☻c☹d"s, "cx"s, len("a☺b☻"s)},
    {"a☺b☻c☹d"s, "uvw☻xyz"s, len("a☺b"s)},
    {"a.RegExp*"s, ".(|)*+?^$[]"s, 8},
    {dots + dots + dots, " "s, -1},
    {"012abcba210"s, "\xff" "b"s, 6},
    {"012\x80" "bcb\x80" "210"s, "\xff" "b"s, 7},
    {"0123456\xcf\x80" "abc"s, "\xcf" "b\x80"s, 10},
};

const std::vector<SplitTest> splittests = {
    {""s, ""s, -1, {}},
    {abcd, "a"s, 0, {}},
    {abcd, ""s, 2, {"a"s, "bcd"s}},
    {abcd, "a"s, -1, {""s, "bcd"s}},
    {abcd, "z"s, -1, {"abcd"s}},
    {abcd, ""s, -1, {"a"s, "b"s, "c"s, "d"s}},
    {commas, ","s, -1, {"1"s, "2"s, "3"s, "4"s}},
    {dots, "..."s, -1, {"1"s, ".2"s, ".3"s, ".4"s}},
    {faces, "☹"s, -1, {"☺☻"s, ""s}},
    {faces, "~"s, -1, {faces}},
    {faces, ""s, -1, {"☺"s, "☻"s, "☹"s}},
    {"1 2 3 4"s, " "s, 3, {"1"s, "2"s, "3 4"s}},
    {"1 2"s, " "s, 3, {"1"s, "2"s}},
    {"123"s, ""s, 2, {"1"s, "23"s}},
    {"123"s, ""s, 17, {"1"s, "2"s, "3"s}},
    {"bT"s, "T"s, max_int / 4, {"b"s, ""s}},
    {"\xff-\xff"s, ""s, -1, {"\xff"s, "-"s, "\xff"s}},
    {"\xff-\xff"s, "-"s, -1, {"\xff"s, "\xff"s}},
};

const std::vector<SplitTest> splitaftertests = {
    {abcd, "a"s, -1, {"a"s, "bcd"s}},
    {abcd, "z"s, -1, {"abcd"s}},
    {abcd, ""s, -1, {"a"s, "b"s, "c"s, "d"s}},
    {commas, ","s, -1, {"1,"s, "2,"s, "3,"s, "4"s}},
    {dots, "..."s, -1, {"1..."s, ".2..."s, ".3..."s, ".4"s}},
    {faces, "☹"s, -1, {"☺☻☹"s, ""s}},
    {faces, "~"s, -1, {faces}},
    {faces, ""s, -1, {"☺"s, "☻"s, "☹"s}},
    {"1 2 3 4"s, " "s, 3, {"1 "s, "2 "s, "3 4"s}},
    {"1 2 3"s, " "s, 3, {"1 "s, "2 "s, "3"s}},
    {"1 2"s, " "s, 3, {"1 "s, "2"s}},
    {"123"s, ""s, 2, {"1"s, "23"s}},
    {"123"s, ""s, 17, {"1"s, "2"s, "3"s}},
};

const std::vector<FieldsTest> fieldstests = {
    {""s, {}},
    {" "s, {}},
    {" \t "s, {}},
    {"  abc  "s, {"abc"s}},
    {"1 2 3 4"s, {"1"s, "2"s, "3"s, "4"s}},
    {"1  2  3  4"s, {"1"s, "2"s, "3"s, "4"s}},
    {"1\t\t2\t\t3\t4"s, {"1"s, "2"s, "3"s, "4"s}},
    {"1\u20002\u20013\u20024"s, {"1"s, "2"s, "3"s, "4"s}},
    {"\u2000\u2001\u2002"s, {}},
    {"\n™\t™\n"s, {"™"s, "™"s}},
    {faces, {faces}},
};

const std::vector<StringTest> upperTests = {
    {""s, bytes_of(""s)},
    {"ONLYUPPER"s, bytes_of("ONLYUPPER"s)},
    {"abc"s, bytes_of("ABC"s)},
    {"AbC123"s, bytes_of("ABC123"s)},
    {"azAZ09_"s, bytes_of("AZAZ09_"s)},
    {"longStrinGwitHmixofsmaLLandcAps"s, bytes_of("LONGSTRINGWITHMIXOFSMALLANDCAPS"s)},
    {"long\u0250string\u0250with\u0250nonascii\u2C6Fchars"s, bytes_of("LONG\u2C6FSTRING\u2C6FWITH\u2C6FNONASCII\u2C6FCHARS"s)},
    {"\u0250\u0250\u0250\u0250\u0250"s, bytes_of("\u2C6F\u2C6F\u2C6F\u2C6F\u2C6F"s)}, // grows one byte per char
    {"a\u0080\U0010FFFF"s, bytes_of("A\u0080\U0010FFFF"s)},                           // test utf8.RuneSelf and utf8.MaxRune
};

const std::vector<StringTest> lowerTests = {
    {""s, bytes_of(""s)},
    {"abc"s, bytes_of("abc"s)},
    {"AbC123"s, bytes_of("abc123"s)},
    {"azAZ09_"s, bytes_of("azaz09_"s)},
    {"longStrinGwitHmixofsmaLLandcAps"s, bytes_of("longstringwithmixofsmallandcaps"s)},
    {"LONG\u2C6FSTRING\u2C6FWITH\u2C6FNONASCII\u2C6FCHARS"s, bytes_of("long\u0250string\u0250with\u0250nonascii\u0250chars"s)},
    {"\u2C6D\u2C6D\u2C6D\u2C6D\u2C6D"s, bytes_of("\u0251\u0251\u0251\u0251\u0251"s)}, // shrinks one byte per char
    {"A\u0080\U0010FFFF"s, bytes_of("a\u0080\U0010FFFF"s)},                           // test utf8.RuneSelf and utf8.MaxRune
};

const std::vector<StringTest> trimSpaceTests = {
    {""s, {}},
    {"  a"s, bytes_of("a"s)},
    {"b  "s, bytes_of("b"s)},
    {"abc"s, bytes_of("abc"s)},
    {space + "abc"s + space, bytes_of("abc"s)},
    {" "s, {}},
    {"\u3000 "s, {}},
    {" \u3000"s, {}},
    {" \t\r\n \t\t\r\r\n\n "s, {}},
    {" \t\r\n x\t\t\r\r\n\n "s, bytes_of("x"s)},
    {" \u2000\t\r\n x\t\t\r\r\ny\n \u3000"s, bytes_of("x\t\t\r\r\ny"s)},
    {"1 \t\r\n2"s, bytes_of("1 \t\r\n2"s)},
    {" x\x80"s, bytes_of("x\x80"s)},
    {" x\xc0"s, bytes_of("x\xc0"s)},
    {"x \xc0\xc0 "s, bytes_of("x \xc0\xc0"s)},
    {"x \xc0"s, bytes_of("x \xc0"s)},
    {"x \xc0 "s, bytes_of("x \xc0"s)},
    {"x \xc0\xc0 "s, bytes_of("x \xc0\xc0"s)},
    {"x ☺\xc0\xc0 "s, bytes_of("x ☺\xc0\xc0"s)},
    {"x ☺ "s, bytes_of("x ☺"s)},
};

const std::vector<ToValidUTF8Test> toValidUTF8Tests = {
    {""s, "\uFFFD"s, ""s},
    {"abc"s, "\uFFFD"s, "abc"s},
    {"\uFDDD"s, "\uFFFD"s, "\uFDDD"s},
    {"a\xff" "b"s, "\uFFFD"s, "a\uFFFDb"s},
    {"a\xff" "b\uFFFD"s, "X"s, "aXb\uFFFD"s},
    {"a☺\xff" "b☺\xC0\xAF" "c☺\xff"s, ""s, "a☺b☺c☺"s},
    {"a☺\xff" "b☺\xC0\xAF" "c☺\xff"s, "日本語"s, "a☺日本語b☺日本語c☺日本語"s},
    {"\xC0\xAF"s, "\uFFFD"s, "\uFFFD"s},
    {"\xE0\x80\xAF"s, "\uFFFD"s, "\uFFFD"s},
    {"\xed\xa0\x80"s, "abc"s, "abc"s},
    {"\xed\xbf\xbf"s, "\uFFFD"s, "\uFFFD"s},
    {"\xF0\x80\x80\xaf"s, "☺"s, "☺"s},
    {"\xF8\x80\x80\x80\xAF"s, "\uFFFD"s, "\uFFFD"s},
    {"\xFC\x80\x80\x80\x80\xAF"s, "\uFFFD"s, "\uFFFD"s},
};

const std::vector<RunesTest> RunesTests = {
    {""s, {}, false},
    {" "s, {32}, false},
    {"ABC"s, {65, 66, 67}, false},
    {"abc"s, {97, 98, 99}, false},
    {"\u65e5\u672c\u8a9e"s, {26085, 26412, 35486}, false},
    {"ab\x80" "c"s, {97, 98, 0xFFFD, 99}, true},
    {"ab\xc0" "c"s, {97, 98, 0xFFFD, 99}, true},
};

const std::vector<TrimTest> trimTests = {
    {"Trim"s, "abba"s, "a"s, "bb"s},
    {"Trim"s, "abba"s, "ab"s, ""s},
    {"TrimLeft"s, "abba"s, "ab"s, ""s},
    {"TrimRight"s, "abba"s, "ab"s, ""s},
    {"TrimLeft"s, "abba"s, "a"s, "bba"s},
    {"TrimLeft"s, "abba"s, "b"s, "abba"s},
    {"TrimRight"s, "abba"s, "a"s, "abb"s},
    {"TrimRight"s, "abba"s, "b"s, "abba"s},
    {"Trim"s, "<tag>"s, "<>"s, "tag"s},
    {"Trim"s, "* listitem"s, " *"s, "listitem"s},
    {"Trim"s, "\"quote\""s, "\""s, "quote"s},
    {"Trim"s, "\u2C6F\u2C6F\u0250\u0250\u2C6F\u2C6F"s, "\u2C6F"s, "\u0250\u0250"s},
    {"Trim"s, "\x80test\xff"s, "\xff"s, "test"s},
    {"Trim"s, " Ġ "s, " "s, "Ġ"s},
    {"Trim"s, " Ġİ0"s, "0 "s, "Ġİ"s},
    //empty string tests
    {"Trim"s, "abba"s, ""s, "abba"s},
    {"Trim"s, ""s, "123"s, ""s},
    {"Trim"s, ""s, ""s, ""s},
    {"TrimLeft"s, "abba"s, ""s, "abba"s},
    {"TrimLeft"s, ""s, "123"s, ""s},
    {"TrimLeft"s, ""s, ""s, ""s},
    {"TrimRight"s, "abba"s, ""s, "abba"s},
    {"TrimRight"s, ""s, "123"s, ""s},
    {"TrimRight"s, ""s, ""s, ""s},
    {"TrimRight"s, "☺\xc0"s, "☺"s, "☺\xc0"s},
    {"TrimPrefix"s, "aabb"s, "a"s, "abb"s},
    {"TrimPrefix"s, "aabb"s, "b"s, "aabb"s},
    {"TrimSuffix"s, "aabb"s, "a"s, "aabb"s},
    {"TrimSuffix"s, "aabb"s, "b"s, "aab"s},
};

const std::vector<TrimNilTest> trimNilTests = {
    {"Trim"s, {}, ""s, {}},
    {"Trim"s, empty_bytes, ""s, {}},
    {"Trim"s, bytes_of("a"s), "a"s, {}},
    {"Trim"s, bytes_of("aa"s), "a"s, {}},
    {"Trim"s, bytes_of("a"s), "ab"s, {}},
    {"Trim"s, bytes_of("ab"s), "ab"s, {}},
    {"Trim"s, bytes_of("☺"s), "☺"s, {}},
    {"TrimLeft"s, {}, ""s, {}},
    {"TrimLeft"s, empty_bytes, ""s, {}},
    {"TrimLeft"s, bytes_of("a"s), "a"s, {}},
    {"TrimLeft"s, bytes_of("aa"s), "a"s, {}},
    {"TrimLeft"s, bytes_of("a"s), "ab"s, {}},
    {"TrimLeft"s, bytes_of("ab"s), "ab"s, {}},
    {"TrimLeft"s, bytes_of("☺"s), "☺"s, {}},
    {"TrimRight"s, {}, ""s, {}},
    {"TrimRight"s, empty_bytes, ""s, empty_bytes},
    {"TrimRight"s, bytes_of("a"s), "a"s, empty_bytes},
    {"TrimRight"s, bytes_of("aa"s), "a"s, empty_bytes},
    {"TrimRight"s, bytes_of("a"s), "ab"s, empty_bytes},
    {"TrimRight"s, bytes_of("ab"s), "ab"s, empty_bytes},
    {"TrimRight"s, bytes_of("☺"s), "☺"s, empty_bytes},
    {"TrimPrefix"s, {}, ""s, {}},
    {"TrimPrefix"s, empty_bytes, ""s, empty_bytes},
    {"TrimPrefix"s, bytes_of("a"s), "a"s, empty_bytes},
    {"TrimPrefix"s, bytes_of("☺"s), "☺"s, empty_bytes},
    {"TrimSuffix"s, {}, ""s, {}},
    {"TrimSuffix"s, empty_bytes, ""s, empty_bytes},
    {"TrimSuffix"s, bytes_of("a"s), "a"s, empty_bytes},
    {"TrimSuffix"s, bytes_of("☺"s), "☺"s, empty_bytes},
};

const std::vector<TrimFuncTest> trimFuncTests = {
    {isSpace, space + " hello "s + space,
        bytes_of("hello"s),
        bytes_of("hello "s + space),
        bytes_of(space + " hello"s)},
    {isDigit, "\u0e50\u0e5212hello34\u0e50\u0e51"s,
        bytes_of("hello"s),
        bytes_of("hello34\u0e50\u0e51"s),
        bytes_of("\u0e50\u0e5212hello"s)},
    {isUpper, "\u2C6F\u2C6F\u2C6F\u2C6FABCDhelloEF\u2C6F\u2C6FGH\u2C6F\u2C6F"s,
        bytes_of("hello"s),
        bytes_of("helloEF\u2C6F\u2C6FGH\u2C6F\u2C6F"s),
        bytes_of("\u2C6F\u2C6F\u2C6F\u2C6FABCDhello"s)},
    {not_(isSpace), "hello"s + space + "hello"s,
        bytes_of(space),
        bytes_of(space + "hello"s),
        bytes_of("hello"s + space)},
    {not_(isDigit), "hello\u0e50\u0e521234\u0e50\u0e51helo"s,
        bytes_of("\u0e50\u0e521234\u0e50\u0e51"s),
        bytes_of("\u0e50\u0e521234\u0e50\u0e51helo"s),
        bytes_of("hello\u0e50\u0e521234\u0e50\u0e51"s)},
    {isValidRune, "ab\xc0" "a\xc0" "cd"s,
        bytes_of("\xc0" "a\xc0"s),
        bytes_of("\xc0" "a\xc0" "cd"s),
        bytes_of("ab\xc0" "a\xc0"s)},
    {not_(isValidRune), "\xc0" "a\xc0"s,
        bytes_of("a"s),
        bytes_of("a\xc0"s),
        bytes_of("\xc0" "a"s)},
    // The nils returned by TrimLeftFunc are odd behavior, but we need
    // to preserve backwards compatibility.
    {isSpace, ""s,
        {},
        {},
        bytes_of(""s)},
    {isSpace, " "s,
        {},
        {},
        bytes_of(""s)},
};

const std::vector<IndexFuncTest> indexFuncTests = {
    {""s, isValidRune, -1, -1},
    {"abc"s, isDigit, -1, -1},
    {"0123"s, isDigit, 0, 3},
    {"a1b"s, isDigit, 1, 1},
    {space, isSpace, 0, len(space) - 3}, // last rune in space is 3 bytes
    {"\u0e50\u0e5212hello34\u0e50\u0e51"s, isDigit, 0, 18},
    {"\u2C6F\u2C6F\u2C6F\u2C6FABCDhelloEF\u2C6F\u2C6FGH\u2C6F\u2C6F"s, isUpper, 0, 34},
    {"12\u0e50\u0e52hello34\u0e50\u0e51"s, not_(isDigit), 8, 12},

    // tests of invalid UTF-8
    {"\x80" "1"s, isDigit, 1, 1},
    {"\x80" "abc"s, isDigit, -1, -1},
    {"\xc0" "a\xc0"s, isValidRune, 1, 1},
    {"\xc0" "a\xc0"s, not_(isValidRune), 0, 2},
    {"\xc0☺\xc0"s, not_(isValidRune), 0, 4},
    {"\xc0☺\xc0\xc0"s, not_(isValidRune), 0, 5},
    {"ab\xc0" "a\xc0" "cd"s, not_(isValidRune), 2, 4},
    {"a\xe0\x80" "cd"s, not_(isValidRune), 1, 2},
};

const std::vector<ReplaceTest> ReplaceTests = {
    {"hello"s, "l"s, "L"s, 0, "hello"s},
    {"hello"s, "l"s, "L"s, -1, "heLLo"s},
    {"hello"s, "x"s, "X"s, -1, "hello"s},
    {""s, "x"s, "X"s, -1, ""s},
    {"radar"s, "r"s, "<r>"s, -1, "<r>ada<r>"s},
    {""s, ""s, "<>"s, -1, "<>"s},
    {"banana"s, "a"s, "<>"s, -1, "b<>n<>n<>"s},
    {"banana"s, "a"s, "<>"s, 1, "b<>nana"s},
    {"banana"s, "a"s, "<>"s, 1000, "b<>n<>n<>"s},
    {"banana"s, "an"s, "<>"s, -1, "b<><>a"s},
    {"banana"s, "ana"s, "<>"s, -1, "b<>na"s},
    {"banana"s, ""s, "<>"s, -1, "<>b<>a<>n<>a<>n<>a<>"s},
    {"banana"s, ""s, "<>"s, 10, "<>b<>a<>n<>a<>n<>a<>"s},
    {"banana"s, ""s, "<>"s, 6, "<>b<>a<>n<>a<>n<>a"s},
    {"banana"s, ""s, "<>"s, 5, "<>b<>a<>n<>a<>na"s},
    {"banana"s, ""s, "<>"s, 1, "<>banana"s},
    {"banana"s, "a"s, "a"s, -1, "banana"s},
    {"banana"s, "a"s, "a"s, 1, "banana"s},
    {"☺☻☹"s, ""s, "<>"s, -1, "<>☺<>☻<>☹<>"s},
};

const std::vector<TitleTest> ToTitleTests = {
    {""s, ""s},
    {"a"s, "A"s},
    {" aaa aaa aaa "s, " AAA AAA AAA "s},
    {" Aaa Aaa Aaa "s, " AAA AAA AAA "s},
    {"123a456"s, "123A456"s},
    {"double-blind"s, "DOUBLE-BLIND"s},
    {"ÿøû"s, "ŸØÛ"s},
};

const std::vector<EqualFoldTest> EqualFoldTests = {
    {"abc"s, "abc"s, true},
    {"ABcd"s, "ABcd"s, true},
    {"123abc"s, "123ABC"s, true},
    {"αβδ"s, "ΑΒΔ"s, true},
    {"abc"s, "xyz"s, false},
    {"abc"s, "XYZ"s, false},
    {"abcdefghijk"s, "abcdefghijX"s, false},
    {"abcdefghijk"s, "abcdefghij\u212A"s, true},
    {"abcdefghijK"s, "abcdefghij\u212A"s, true},
    {"abcdefghijkz"s, "abcdefghij\u212Ay"s, false},
    {"abcdefghijKz"s, "abcdefghij\u212Ay"s, false},
};

const std::vector<CutTest> cutTests = {
    {"abc"s, "b"s, "a"s, "c"s, true},
    {"abc"s, "a"s, ""s, "bc"s, true},
    {"abc"s, "c"s, "ab"s, ""s, true},
    {"abc"s, "abc"s, ""s, ""s, true},
    {"abc"s, ""s, ""s, "abc"s, true},
    {"abc"s, "d"s, "abc"s, ""s, false},
    {""s, "d"s, ""s, ""s, false},
    {""s, ""s, ""s, ""s, true},
};

const std::vector<CutPrefixTest> cutPrefixTests = {
    {"abc"s, "a"s, "bc"s, true},
    {"abc"s, "abc"s, ""s, true},
    {"abc"s, ""s, "abc"s, true},
    {"abc"s, "d"s, "abc"s, false},
    {""s, "d"s, ""s, false},
    {""s, ""s, ""s, true},
};

const std::vector<CutSuffixTest> cutSuffixTests = {
    {"abc"s, "bc"s, "a"s, true},
    {"abc"s, "abc"s, ""s, true},
    {"abc"s, ""s, "abc"s, true},
    {"abc"s, "d"s, "abc"s, false},
    {""s, "d"s, ""s, false},
    {""s, ""s, ""s, true},
};

const std::vector<ContainsTest> containsTests = {
    {bytes_of("hello"s), bytes_of("hel"s), true},
    {bytes_of("日本語"s), bytes_of("日本"s), true},
    {bytes_of("hello"s), bytes_of("Hello, world"s), false},
    {bytes_of("東京"s), bytes_of("京東"s), false},
};

const std::vector<ContainsAnyTest> ContainsAnyTests = {
    {bytes_of(""s), ""s, false},
    {bytes_of(""s), "a"s, false},
    {bytes_of(""s), "abc"s, false},
    {bytes_of("a"s), ""s, false},
    {bytes_of("a"s), "a"s, true},
    {bytes_of("aaa"s), "a"s, true},
    {bytes_of("abc"s), "xyz"s, false},
    {bytes_of("abc"s), "xcz"s, true},
    {bytes_of("a☺b☻c☹d"s), "uvw☻xyz"s, true},
    {bytes_of("aRegExp*"s), ".(|)*+?^$[]"s, true},
    {bytes_of(dots + dots + dots), " "s, false},
};

const std::vector<ContainsRuneTest> ContainsRuneTests = {
    {bytes_of(""s), U'a', false},
    {bytes_of("a"s), U'a', true},
    {bytes_of("aaa"s), U'a', true},
    {bytes_of("abc"s), U'y', false},
    {bytes_of("abc"s), U'c', true},
    {bytes_of("a☺b☻c☹d"s), U'x', false},
    {bytes_of("a☺b☻c☹d"s), U'☻', true},
    {bytes_of("aRegExp*"s), U'*', true},
};

const std::vector<CompareTest> compareTests = {
    {bytes_of(""s), bytes_of(""s), 0},
    {bytes_of("a"s), bytes_of(""s), 1},
    {bytes_of(""s), bytes_of("a"s), -1},
    {bytes_of("abc"s), bytes_of("abc"s), 0},
    {bytes_of("abd"s), bytes_of("abc"s), 1},
    {bytes_of("abc"s), bytes_of("abd"s), -1},
    {bytes_of("ab"s), bytes_of("abc"s), -1},
    {bytes_of("abc"s), bytes_of("ab"s), 1},
    {bytes_of("x"s), bytes_of("ab"s), 1},
    {bytes_of("ab"s), bytes_of("x"s), -1},
    {bytes_of("x"s), bytes_of("a"s), 1},
    {bytes_of("b"s), bytes_of("x"s), -1},
    // test runtime·memeq's chunked implementation
    {bytes_of("abcdefgh"s), bytes_of("abcdefgh"s), 0},
    {bytes_of("abcdefghi"s), bytes_of("abcdefghi"s), 0},
    {bytes_of("abcdefghi"s), bytes_of("abcdefghj"s), -1},
    {bytes_of("abcdefghj"s), bytes_of("abcdefghi"s), 1},
    // nil tests
    {{}, {}, 0},
    {bytes_of(""s), {}, 0},
    {{}, bytes_of(""s), 0},
    {bytes_of("a"s), {}, 1},
    {{}, bytes_of("a"s), -1},
};

TEST(GoBytes, Lines) {
    for (const auto& s : linesTests) {
        auto result = collect(bytes::Lines(b(s.a)));
        EXPECT_EQ(result, s.b) << "Lines(" << s.a << ")";
    }
}

TEST(GoBytes, Equal) {
    for (const auto& tt : compareTests) {
        bool eql = bytes::Equal(b(tt.a), b(tt.b));
        EXPECT_EQ(eql, tt.i == 0) << "Equal(" << q(tt.a) << ", " << q(tt.b) << ")";
    }
}

TEST(GoBytes, Compare) {
    for (const auto& tt : compareTests) {
        constexpr std::size_t numShifts = 16;
        auto tb = b(tt.b);
        auto buffer = slice<byte>::make(tb.len() + numShifts);
        // vary the input alignment of tt.b
        for (std::size_t offset = 0; offset <= numShifts; offset++) {
            auto shiftedB = buffer.Sub(offset, tb.len() + offset);
            Copy<byte>(shiftedB, tb);
            EXPECT_EQ(bytes::Compare(b(tt.a), shiftedB), tt.i) << "Compare(" << q(tt.a) << ", " << q(tt.b) << "), offset " << offset;
        }
    }
}

TEST(GoBytes, CompareIdenticalSlice) {
    auto bb = toBytes("Hello Gophers!");
    EXPECT_EQ(bytes::Compare(bb, bb), 0) << "b != b";
    EXPECT_EQ(bytes::Compare(bb, bb.Sub(0, 1)), 1) << "b > b[:1] failed";
}

// Execute f on each test case. funcName should be the name of f; it's
// used in failure reports.
void runIndexTests(isize (*f)(std::span<const byte>, std::span<const byte>) noexcept, const char* funcName,
                   const std::vector<BinOpTest>& testCases) {
    for (const auto& test : testCases) {
        EXPECT_EQ(f(as_bytes(test.a), as_bytes(test.b)), test.i) << funcName << "(" << test.a << "," << test.b << ")";
    }
}

void runIndexAnyTests(isize (*f)(std::span<const byte>, std::string_view) noexcept, const char* funcName,
                      const std::vector<BinOpTest>& testCases) {
    for (const auto& test : testCases) {
        EXPECT_EQ(f(as_bytes(test.a), test.b), test.i) << funcName << "(" << test.a << "," << test.b << ")";
    }
}

TEST(GoBytes, Index) { runIndexTests(bytes::Index, "Index", indexTests); }
TEST(GoBytes, LastIndex) { runIndexTests(bytes::LastIndex, "LastIndex", lastIndexTests); }
TEST(GoBytes, IndexAny) { runIndexAnyTests(bytes::IndexAny, "IndexAny", indexAnyTests); }
TEST(GoBytes, LastIndexAny) { runIndexAnyTests(bytes::LastIndexAny, "LastIndexAny", lastIndexAnyTests); }

TEST(GoBytes, IndexByte) {
    for (const auto& tt : indexTests) {
        if (tt.b.size() != 1) continue;
        EXPECT_EQ(bytes::IndexByte(as_bytes(tt.a), byte(tt.b[0])), tt.i) << "IndexByte(" << tt.a << ", '" << tt.b << "')";
    }
}

TEST(GoBytes, LastIndexByte) {
    const std::vector<BinOpTest> testCases = {
        {"", "q", -1},
        {"abcdef", "q", -1},
        {"abcdefabcdef", "a", len("abcdef")},       // something in the middle
        {"abcdefabcdef", "f", len("abcdefabcde")},  // last byte
        {"zabcdefabcdef", "z", 0},                  // first byte
        {"a☺b☻c☹d", "b", len("a☺")},                // non-ascii
    };
    for (const auto& test : testCases) {
        EXPECT_EQ(bytes::LastIndexByte(as_bytes(test.a), byte(test.b[0])), test.i) << "LastIndexByte(" << test.a << "," << test.b << ")";
    }
}

TEST(GoBytes, IndexRune) {
    struct T {
        std::string in;
        rune rune_;
        int want;
    };
    const std::vector<T> tests = {
        {"", U'a', -1},
        {"", U'☺', -1},
        {"foo", U'☹', -1},
        {"foo", U'o', 1},
        {"foo☺bar", U'☺', 3},
        {"foo☺☻☹bar", U'☹', 9},
        {"a A x", U'A', 2},
        {"some_text=some_value", U'=', 9},
        {"☺a", U'a', 3},
        {"a☻☺b", U'☺', 4},
        {"𠀳𠀗𠀾𠁄𠀧𠁆𠁂𠀫𠀖𠀪𠀲𠀴𠁀𠀨𠀿", U'𠀿', 56},

        // 2 bytes
        {"ӆ", U'ӆ', 0},
        {"a", U'ӆ', -1},
        {"  ӆ", U'ӆ', 2},
        {"  a", U'ӆ', -1},
        {repeat("ц", 64) + "ӆ", U'ӆ', 128},  // test cutover
        {repeat("ц", 64), U'ӆ', -1},

        // 3 bytes
        {"Ꚁ", U'Ꚁ', 0},
        {"a", U'Ꚁ', -1},
        {"  Ꚁ", U'Ꚁ', 2},
        {"  a", U'Ꚁ', -1},
        {repeat("Ꙁ", 64) + "Ꚁ", U'Ꚁ', 192},  // test cutover
        {repeat("Ꙁ", 64) + "Ꚁ", U'䚀', -1},   // 'Ꚁ' and '䚀' share the same last two bytes

        // 4 bytes
        {"𡌀", U'𡌀', 0},
        {"a", U'𡌀', -1},
        {"  𡌀", U'𡌀', 2},
        {"  a", U'𡌀', -1},
        {repeat("𡋀", 64) + "𡌀", U'𡌀', 256},  // test cutover
        {repeat("𡋀", 64) + "𡌀", U'𣌀', -1},   // '𡌀' and '𣌀' share the same last two bytes

        // RuneError should match any invalid UTF-8 byte sequence.
        {"�", U'�', 0},
        {"\xff", U'�', 0},
        {"☻x�", U'�', len("☻x")},
        {"☻x\xe2\x98", U'�', len("☻x")},
        {"☻x\xe2\x98�", U'�', len("☻x")},
        {"☻x\xe2\x98x", U'�', len("☻x")},

        // Invalid rune values should never match.
        {"a☺b☻c☹d\xe2\x98�\xff�\xed\xa0\x80", -1, -1},
        {"a☺b☻c☹d\xe2\x98�\xff�\xed\xa0\x80", 0xD800, -1},  // Surrogate pair
        {"a☺b☻c☹d\xe2\x98�\xff�\xed\xa0\x80", utf8::MaxRune + 1, -1},

        // Test the cutover to bytealg.Index when it is triggered in the
        // middle of rune that contains consecutive runs of equal bytes.
        {"aaaaa\u212A\u212A\u212A\u212A\U000bc104", U'\U000bc104', 17},  // cutover: (n + 16) / 8
        {"aaaaa\u212A\u212A\u212A\u212A鄄", U'鄄', 17},
        {"aa\u212A\u212A\u212A\u212A\u212Aa\U000bc104", U'\U000bc104', 18},  // cutover: 4 + n>>4
        {"aa\u212A\u212A\u212A\u212A\u212Aa鄄", U'鄄', 18},
    };
    for (const auto& tt : tests) {
        EXPECT_EQ(bytes::IndexRune(as_bytes(tt.in), tt.rune_), tt.want) << "IndexRune(" << tt.in << ", " << tt.rune_ << ")";
    }
}

TEST(GoBytes, Split) {
    for (const auto& tt : splittests) {
        auto a = bytes::SplitN(b(tt.s), as_bytes(tt.sep), tt.n);

        // Appending to the results should not change future results.
        slice<byte> x;
        for (const auto& v : a) x = append(v, byte('z'));

        auto result = sliceOfString(a);
        if (result != tt.a) {
            ADD_FAILURE() << "Split(" << tt.s << ", " << tt.sep << ", " << tt.n << ")";
            continue;
        }

        if (tt.n < 0) {
            auto b2 = collect(bytes::SplitSeq(b(tt.s), as_bytes(tt.sep)));
            EXPECT_EQ(b2, tt.a) << "collect(SplitSeq(" << tt.s << ", " << tt.sep << "))";
        }

        if (tt.n == 0 || a.len() == 0) continue;

        EXPECT_EQ(str(x), tt.a.back() + "z") << "last appended result";

        auto s = bytes::Join(a, as_bytes(tt.sep));
        EXPECT_EQ(str(s), tt.s) << "Join(Split(" << tt.s << ", " << tt.sep << ", " << tt.n << "))";
        if (tt.n < 0) {
            auto b3 = sliceOfString(bytes::Split(b(tt.s), as_bytes(tt.sep)));
            EXPECT_EQ(result, b3) << "Split disagrees with SplitN(" << tt.s << ", " << tt.sep << ", " << tt.n << ")";
        }
        if (a.len() > 0) {
            auto in = a[0];
            EXPECT_FALSE(in.cap() == s.cap() && in.data() == s.data()) << "Join didn't copy";
        }
    }
}

TEST(GoBytes, SplitAfter) {
    for (const auto& tt : splitaftertests) {
        auto a = bytes::SplitAfterN(b(tt.s), as_bytes(tt.sep), tt.n);

        // Appending to the results should not change future results.
        slice<byte> x;
        for (const auto& v : a) x = append(v, byte('z'));

        auto result = sliceOfString(a);
        if (result != tt.a) {
            ADD_FAILURE() << "SplitAfter(" << tt.s << ", " << tt.sep << ", " << tt.n << ")";
            continue;
        }

        if (tt.n < 0) {
            auto b2 = collect(bytes::SplitAfterSeq(b(tt.s), as_bytes(tt.sep)));
            EXPECT_EQ(b2, tt.a) << "collect(SplitAfterSeq(" << tt.s << ", " << tt.sep << "))";
        }

        EXPECT_EQ(str(x), tt.a.back() + "z") << "last appended result";

        auto s = bytes::Join(a, {});
        EXPECT_EQ(str(s), tt.s) << "Join(Split(" << tt.s << ", " << tt.sep << ", " << tt.n << "))";
        if (tt.n < 0) {
            auto b3 = sliceOfString(bytes::SplitAfter(b(tt.s), as_bytes(tt.sep)));
            EXPECT_EQ(result, b3) << "SplitAfter disagrees with SplitAfterN(" << tt.s << ", " << tt.sep << ", " << tt.n << ")";
        }
    }
}

TEST(GoBytes, Fields) {
    for (const auto& tt : fieldstests) {
        auto bb = b(tt.s);
        auto a = bytes::Fields(bb);

        // Appending to the results should not change future results.
        slice<byte> x;
        for (const auto& v : a) x = append(v, byte('z'));

        auto result = sliceOfString(a);
        if (result != tt.a) {
            ADD_FAILURE() << "Fields(" << tt.s << ")";
            continue;
        }

        EXPECT_EQ(collect(bytes::FieldsSeq(b(tt.s))), tt.a) << "collect(FieldsSeq(" << tt.s << "))";
        EXPECT_EQ(str(bb), tt.s) << "slice changed";
        if (!tt.a.empty()) {
            EXPECT_EQ(str(x), tt.a.back() + "z") << "last appended result";
        }
    }
}

TEST(GoBytes, FieldsFunc) {
    for (const auto& tt : fieldstests) {
        auto a = bytes::FieldsFunc(b(tt.s), unicode::IsSpace);
        EXPECT_EQ(sliceOfString(a), tt.a) << "FieldsFunc(" << tt.s << ", unicode.IsSpace)";
    }
    auto pred = [](rune c) { return c == 'X'; };
    const std::vector<FieldsTest> fieldsFuncTests = {
        {"", {}},
        {"XX", {}},
        {"XXhiXXX", {"hi"}},
        {"aXXbXXXcX", {"a", "b", "c"}},
    };
    for (const auto& tt : fieldsFuncTests) {
        auto bb = b(tt.s);
        auto a = bytes::FieldsFunc(bb, pred);

        // Appending to the results should not change future results.
        slice<byte> x;
        for (const auto& v : a) x = append(v, byte('z'));

        EXPECT_EQ(sliceOfString(a), tt.a) << "FieldsFunc(" << tt.s << ")";
        EXPECT_EQ(collect(bytes::FieldsFuncSeq(b(tt.s), pred)), tt.a) << "collect(FieldsFuncSeq(" << tt.s << "))";
        EXPECT_EQ(str(bb), tt.s) << "slice changed";
        if (!tt.a.empty()) {
            EXPECT_EQ(str(x), tt.a.back() + "z") << "last appended result";
        }
    }
}

void runStringTests(const std::function<slice<byte>(const slice<byte>&)>& f, const char* funcName,
                    const std::vector<StringTest>& testCases) {
    for (const auto& tc : testCases) {
        auto actual = f(b(tc.in));
        EXPECT_FALSE(actual.isNil() && tc.out) << funcName << "(" << tc.in << ") = nil; want " << q(tc.out);
        EXPECT_FALSE(!actual.isNil() && !tc.out) << funcName << "(" << tc.in << ") = " << str(actual) << "; want nil";
        EXPECT_TRUE(bytes::Equal(actual, b(tc.out))) << funcName << "(" << tc.in << ") = " << str(actual) << "; want " << q(tc.out);
    }
}

std::string tenRunes(rune r) {
    std::string s;
    for (int i = 0; i < 10; i++) s += str(utf8::AppendRune({}, r));
    return s;
}

// User-defined self-inverse mapping function
rune rot13(rune r) {
    constexpr rune step = 13;
    if (r >= 'a' && r <= 'z') return ((r - 'a' + step) % 26) + 'a';
    if (r >= 'A' && r <= 'Z') return ((r - 'A' + step) % 26) + 'A';
    return r;
}

TEST(GoBytes, Map) {
    // Run a couple of awful growth/shrinkage tests
    auto a = tenRunes('a');

    // 1. Grow. This triggers two reallocations in Map.
    auto maxRune = [](rune) { return unicode::MaxRune; };
    auto m = bytes::Map(maxRune, as_bytes(a));
    EXPECT_EQ(str(m), tenRunes(unicode::MaxRune)) << "growing";

    // 2. Shrink
    auto minRune = [](rune) { return rune('a'); };
    m = bytes::Map(minRune, as_bytes(tenRunes(unicode::MaxRune)));
    EXPECT_EQ(str(m), a) << "shrinking";

    // 3. Rot13
    m = bytes::Map(rot13, as_bytes("a to zed"));
    EXPECT_EQ(str(m), "n gb mrq") << "rot13";

    // 4. Rot13^2
    m = bytes::Map(rot13, bytes::Map(rot13, as_bytes("a to zed")));
    EXPECT_EQ(str(m), "a to zed") << "rot13";

    // 5. Drop
    auto dropNotLatin = [](rune r) -> rune {
        if (unicode::Is(unicode::Latin, r)) return r;
        return -1;
    };
    m = bytes::Map(dropNotLatin, as_bytes("Hello, 세계"));
    EXPECT_EQ(str(m), "Hello") << "drop";

    // 6. Invalid rune
    auto invalidRune = [](rune) { return utf8::MaxRune + 1; };
    m = bytes::Map(invalidRune, as_bytes("x"));
    EXPECT_EQ(str(m), "�") << "invalidRune";
}

TEST(GoBytes, ToUpper) {
    runStringTests([](const slice<byte>& s) { return bytes::ToUpper(s); }, "ToUpper", upperTests);
}

TEST(GoBytes, ToLower) {
    runStringTests([](const slice<byte>& s) { return bytes::ToLower(s); }, "ToLower", lowerTests);
}

TEST(GoBytes, ToValidUTF8) {
    for (const auto& tc : toValidUTF8Tests) {
        auto got = bytes::ToValidUTF8(as_bytes(tc.in), as_bytes(tc.repl));
        EXPECT_EQ(str(got), tc.out) << "ToValidUTF8(" << tc.in << ", " << tc.repl << ")";
    }
}

TEST(GoBytes, TrimSpace) {
    runStringTests([](const slice<byte>& s) { return bytes::TrimSpace(s); }, "TrimSpace", trimSpaceTests);
}

TEST(GoBytes, Repeat) {
    struct RepeatTest {
        std::string in, out;
        isize count;
    };
    std::string longString = "a" + std::string(1 << 16, '\0') + "z";
    const std::vector<RepeatTest> RepeatTests = {
        {"", "", 0},
        {"", "", 1},
        {"", "", 2},
        {"-", "", 0},
        {"-", "-", 1},
        {"-", "----------", 10},
        {"abc ", "abc abc abc ", 3},
        // Tests for results over the chunkLimit
        {std::string(1, '\0'), std::string(1 << 16, '\0'), 1 << 16},
        {longString, longString + longString, 2},
    };
    for (const auto& tt : RepeatTests) {
        auto a = bytes::Repeat(as_bytes(tt.in), tt.count);
        EXPECT_EQ(str(a), tt.out) << "Repeat(" << tt.in << ", " << tt.count << ")";
    }
}

// See Issue golang.org/issue/16237
TEST(GoBytes, RepeatCatchesOverflow) {
    // Repeat panics, which go++ can't recover from: death tests.
    constexpr isize maxInt = std::numeric_limits<isize>::max();
    EXPECT_DEATH((void)bytes::Repeat(as_bytes("--"), -2147483647), "negative");
    (void)bytes::Repeat(as_bytes(""), maxInt);
    (void)bytes::Repeat(as_bytes("-"), 10);
    (void)bytes::Repeat(as_bytes("gopher"), 0);
    EXPECT_DEATH((void)bytes::Repeat(as_bytes("-"), -1), "negative");
    EXPECT_DEATH((void)bytes::Repeat(as_bytes("--"), -102), "negative");
    EXPECT_DEATH((void)bytes::Repeat(as_bytes(std::string(255, '\0')), isize(~std::size_t(0) / 255 + 1)), "overflow");
    EXPECT_DEATH((void)bytes::Repeat(as_bytes("-"), maxInt), "out of range|overflow|bad_alloc|memory");
}

TEST(GoBytes, Runes) {
    for (const auto& tt : RunesTests) {
        auto a = bytes::Runes(as_bytes(tt.in));
        if (!std::equal(a.begin(), a.end(), tt.out.begin(), tt.out.end())) {
            ADD_FAILURE() << "Runes(" << tt.in << ")";
            continue;
        }
        if (!tt.lossy) {
            // can only test reassembly if we didn't lose information
            std::string s;
            for (rune r : a) s += str(utf8::AppendRune({}, r));
            EXPECT_EQ(s, tt.in) << "string(Runes(" << tt.in << "))";
        }
    }
}

TEST(GoBytes, Trim) {
    using trimFn = slice<byte> (*)(const slice<byte>&, std::string_view);
    using trimBytesFn = slice<byte> (*)(const slice<byte>&, std::span<const byte>);
    auto toFn = [](const std::string& name) -> std::pair<trimFn, trimBytesFn> {
        if (name == "Trim") return {bytes::Trim, nullptr};
        if (name == "TrimLeft") return {bytes::TrimLeft, nullptr};
        if (name == "TrimRight") return {bytes::TrimRight, nullptr};
        if (name == "TrimPrefix") return {nullptr, bytes::TrimPrefix};
        if (name == "TrimSuffix") return {nullptr, bytes::TrimSuffix};
        ADD_FAILURE() << "Undefined trim function " << name;
        return {nullptr, nullptr};
    };

    for (const auto& tc : trimTests) {
        auto [f, fb] = toFn(tc.f);
        if (!f && !fb) continue;
        std::string actual = f ? str(f(b(tc.in), tc.arg)) : str(fb(b(tc.in), as_bytes(tc.arg)));
        EXPECT_EQ(actual, tc.out) << tc.f << "(" << tc.in << ", " << tc.arg << ")";
    }

    for (const auto& tc : trimNilTests) {
        auto [f, fb] = toFn(tc.f);
        if (!f && !fb) continue;
        auto actual = f ? f(b(tc.in), tc.arg) : fb(b(tc.in), as_bytes(tc.arg));
        if (actual.len() != 0) {
            ADD_FAILURE() << tc.f << "(" << q(tc.in) << ", " << tc.arg << ") returned non-empty value";
        } else {
            EXPECT_EQ(actual.isNil(), !tc.out.has_value()) << tc.f << "(" << q(tc.in) << ", " << tc.arg << ") got nil " << actual.isNil();
        }
    }
}

TEST(GoBytes, TrimFunc) {
    for (const auto& tc : trimFuncTests) {
        struct trimmer {
            const char* name;
            slice<byte> (*trim)(const slice<byte>&, std::function_ref<bool(rune)>);
            bs out;
        };
        const trimmer trimmers[] = {
            {"TrimFunc", bytes::TrimFunc, tc.trimOut},
            {"TrimLeftFunc", bytes::TrimLeftFunc, tc.leftOut},
            {"TrimRightFunc", bytes::TrimRightFunc, tc.rightOut},
        };
        for (const auto& t : trimmers) {
            auto actual = t.trim(b(tc.in), tc.f.f);
            EXPECT_FALSE(actual.isNil() && t.out) << t.name << "(" << tc.in << ", " << tc.f.name << ") = nil; want " << q(t.out);
            EXPECT_FALSE(!actual.isNil() && !t.out) << t.name << "(" << tc.in << ", " << tc.f.name << ") = " << str(actual) << "; want nil";
            EXPECT_TRUE(bytes::Equal(actual, b(t.out))) << t.name << "(" << tc.in << ", " << tc.f.name << ") = " << str(actual) << "; want " << q(t.out);
        }
    }
}

TEST(GoBytes, IndexFunc) {
    for (const auto& tc : indexFuncTests) {
        EXPECT_EQ(bytes::IndexFunc(as_bytes(tc.in), tc.f.f), tc.first) << "IndexFunc(" << tc.in << ", " << tc.f.name << ")";
        EXPECT_EQ(bytes::LastIndexFunc(as_bytes(tc.in), tc.f.f), tc.last) << "LastIndexFunc(" << tc.in << ", " << tc.f.name << ")";
    }
}

TEST(GoBytes, Replace) {
    for (const auto& tt : ReplaceTests) {
        auto in = b(tt.in);
        auto old = b(tt.old);
        auto new_ = b(tt.new_);
        in = appendSpan(in, as_bytes("<spare>"));
        in = in.Sub(0, tt.in.size());
        auto out = bytes::Replace(in, old, new_, tt.n);
        EXPECT_EQ(str(out), tt.out) << "Replace(" << tt.in << ", " << tt.old << ", " << tt.new_ << ", " << tt.n << ")";
        EXPECT_FALSE(in.cap() == out.cap() && in.data() == out.data()) << "Replace(" << tt.in << ") didn't copy";
        if (tt.n == -1) {
            auto out2 = bytes::ReplaceAll(in, old, new_);
            EXPECT_EQ(str(out2), tt.out) << "ReplaceAll(" << tt.in << ", " << tt.old << ", " << tt.new_ << ")";
        }
    }
}

TEST(GoBytes, ToTitle) {
    for (const auto& tt : ToTitleTests) EXPECT_EQ(str(bytes::ToTitle(as_bytes(tt.in))), tt.out) << "ToTitle(" << tt.in << ")";
}

TEST(GoBytes, EqualFold) {
    for (const auto& tt : EqualFoldTests) {
        EXPECT_EQ(bytes::EqualFold(as_bytes(tt.s), as_bytes(tt.t)), tt.out) << "EqualFold(" << tt.s << ", " << tt.t << ")";
        EXPECT_EQ(bytes::EqualFold(as_bytes(tt.t), as_bytes(tt.s)), tt.out) << "EqualFold(" << tt.t << ", " << tt.s << ")";
    }
}

TEST(GoBytes, Cut) {
    for (const auto& tt : cutTests) {
        auto [before, after, found] = bytes::Cut(b(tt.s), as_bytes(tt.sep));
        EXPECT_TRUE(str(before) == tt.before && str(after) == tt.after && found == tt.found) << "Cut(" << tt.s << ", " << tt.sep << ")";
    }
}

TEST(GoBytes, CutLast) {
    const std::vector<CutTest> tests = {
        {"a/b/c", "/", "a/b", "c", true},
        {"a//b//c", "//", "a//b", "c", true},
        {"abc", "/", "abc", "", false},
        {"abc", "", "abc", "", true},
        {"", "", "", "", true},
        {"/abc", "/", "", "abc", true},
        {"abc/", "/", "abc", "", true},
    };
    for (const auto& tt : tests) {
        auto [before, after, found] = bytes::CutLast(b(tt.s), as_bytes(tt.sep));
        EXPECT_TRUE(str(before) == tt.before && str(after) == tt.after && found == tt.found) << "CutLast(" << tt.s << ", " << tt.sep << ")";
    }
}

TEST(GoBytes, CutPrefix) {
    for (const auto& tt : cutPrefixTests) {
        auto [after, found] = bytes::CutPrefix(b(tt.s), as_bytes(tt.sep));
        EXPECT_TRUE(str(after) == tt.after && found == tt.found) << "CutPrefix(" << tt.s << ", " << tt.sep << ")";
    }
}

TEST(GoBytes, CutSuffix) {
    for (const auto& tt : cutSuffixTests) {
        auto [before, found] = bytes::CutSuffix(b(tt.s), as_bytes(tt.sep));
        EXPECT_TRUE(str(before) == tt.before && found == tt.found) << "CutSuffix(" << tt.s << ", " << tt.sep << ")";
    }
}

TEST(GoBytes, BufferGrowNegative) {
    EXPECT_DEATH(
        {
            bytes::Buffer bb;
            bb.Grow(-1);
        },
        "negative count");
}

TEST(GoBytes, BufferTruncateNegative) {
    EXPECT_DEATH(
        {
            bytes::Buffer bb;
            bb.Truncate(-1);
        },
        "truncation out of range");
}

TEST(GoBytes, BufferTruncateOutOfRange) {
    EXPECT_DEATH(
        {
            bytes::Buffer bb;
            bb.Write(slice<byte>::make(10));
            bb.Truncate(20);
        },
        "truncation out of range");
}

TEST(GoBytes, Contains) {
    for (const auto& tt : containsTests) {
        EXPECT_EQ(bytes::Contains(b(tt.b), b(tt.subslice)), tt.want) << "Contains(" << q(tt.b) << ", " << q(tt.subslice) << ")";
    }
}

TEST(GoBytes, ContainsAny) {
    for (const auto& ct : ContainsAnyTests) {
        EXPECT_EQ(bytes::ContainsAny(b(ct.b), ct.substr), ct.expected) << "ContainsAny(" << q(ct.b) << ", " << ct.substr << ")";
    }
}

TEST(GoBytes, ContainsRune) {
    for (const auto& ct : ContainsRuneTests) {
        EXPECT_EQ(bytes::ContainsRune(b(ct.b), ct.r), ct.expected) << "ContainsRune(" << q(ct.b) << ", " << ct.r << ")";
    }
}

TEST(GoBytes, ContainsFunc) {
    for (const auto& ct : ContainsRuneTests) {
        EXPECT_EQ(bytes::ContainsFunc(b(ct.b), [&](rune r) { return ct.r == r; }), ct.expected)
            << "ContainsFunc(" << q(ct.b) << ", func(" << ct.r << "))";
    }
}

TEST(GoBytes, Clone) {
    auto a42 = toBytes(repeat("a", 42));
    const std::vector<slice<byte>> cloneTests = {
        slice<byte>(),
        slice<byte>::make(0),
        bytes::Clone(slice<byte>::make(0)),
        a42.Sub(0, 0),
        a42.Sub(0, 0, 0),
        toBytes("short"),
        a42,
    };
    for (const auto& input : cloneTests) {
        auto clone = bytes::Clone(input);
        EXPECT_TRUE(bytes::Equal(clone, input)) << "Clone(" << str(input) << ")";
        EXPECT_FALSE(input.isNil() && !clone.isNil()) << "Clone(nil) return value should be equal to nil slice.";
        EXPECT_FALSE(!input.isNil() && clone.isNil()) << "Clone return value should not be equal to nil slice.";
        EXPECT_FALSE(input.cap() != 0 && input.data() == clone.data()) << "Clone return value should not reference inputs backing memory.";
    }
}

}  // namespace
