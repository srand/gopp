// Ported from Go 1.27.3's strings/strings_test.go and builder_test.go,
// with Go's test names. Left out: the benchmarks, the allocation counts,
// the randomized tests, TestTitle (Title is deprecated and not ported),
// the Reader and Replacer tests (strings::NewReader is io::StringReader;
// Replacer isn't ported) and the Builder tests of Go's copy checks and
// growth.
#include <gopp/strings.hpp>
#include <gopp/unicode.hpp>
#include <gopp/unicode/utf8.hpp>

#include <gtest/gtest.h>

#include <functional>
#include <limits>
#include <string>
#include <vector>

using namespace gopp;
using namespace std::literals;

namespace {

int len(const std::string& s) { return int(s.size()); }
constexpr isize maxInt = std::numeric_limits<isize>::max();

std::vector<std::string> strs(const slice<string>& s) {
    std::vector<std::string> out;
    for (const auto& v : s) out.push_back(v.str());
    return out;
}

template <typename G>
std::vector<std::string> collect(G&& seq) {
    std::vector<std::string> out;
    for (auto&& v : seq) out.push_back(v.str());
    return out;
}

struct LinesTest {
    std::string a;
    std::vector<std::string> b;
};

const std::string abcd = "abcd";
const std::string faces = "☺☻☹";
const std::string commas = "1,2,3,4";
const std::string dots = "1....2....3....4";

struct IndexTest {
    std::string s;
    std::string sep;
    int out;
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

// Test case for any function which accepts and returns a single string.
struct StringTest {
    std::string in, out;
};

const std::string space = "\t\v\r\f\n\u0085  　";

struct ToValidUTF8Test {
    std::string in, repl, out;
};

struct TrimTest {
    std::string f;
    std::string in, arg, out;
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
    std::string trimOut;
    std::string leftOut;
    std::string rightOut;
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

struct ContainsTest {
    std::string str, substr;
    bool expected;
};

struct ContainsRuneTest {
    std::string str;
    rune r;
    bool expected;
};

struct EqualFoldTest {
    std::string s, t;
    bool out;
};

struct CountTest {
    std::string s, sep;
    int num;
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

const std::vector<LinesTest> linesTests = {
    {.a = "abc\nabc\n"s, .b = {"abc\n"s, "abc\n"s}},
    {.a = "abc\r\nabc"s, .b = {"abc\r\n"s, "abc"s}},
    {.a = "abc\r\n"s, .b = {"abc\r\n"s}},
    {.a = "\nabc"s, .b = {"\n"s, "abc"s}},
    {.a = "\nabc\n\n"s, .b = {"\n"s, "abc\n"s, "\n"s}},
};

const std::vector<IndexTest> indexTests = {
    {""s, ""s, 0},
    {""s, "a"s, -1},
    {""s, "foo"s, -1},
    {"fo"s, "foo"s, -1},
    {"foo"s, "foo"s, 0},
    {"oofofoofooo"s, "f"s, 2},
    {"oofofoofooo"s, "foo"s, 4},
    {"barfoobarfoo"s, "foo"s, 3},
    {"foo"s, ""s, 0},
    {"foo"s, "o"s, 1},
    {"abcABCabc"s, "A"s, 3},
    {"jrzm6jjhorimglljrea4w3rlgosts0w2gia17hno2td4qd1jz"s, "jz"s, 47},
    {"ekkuk5oft4eq0ocpacknhwouic1uua46unx12l37nioq9wbpnocqks6"s, "ks6"s, 52},
    {"999f2xmimunbuyew5vrkla9cpwhmxan8o98ec"s, "98ec"s, 33},
    {"9lpt9r98i04k8bz6c6dsrthb96bhi"s, "96bhi"s, 24},
    {"55u558eqfaod2r2gu42xxsu631xf0zobs5840vl"s, "5840vl"s, 33},
    // cases with one byte strings - test special case in Index()
    {""s, "a"s, -1},
    {"x"s, "a"s, -1},
    {"x"s, "x"s, 0},
    {"abc"s, "a"s, 0},
    {"abc"s, "b"s, 1},
    {"abc"s, "c"s, 2},
    {"abc"s, "x"s, -1},
    // test special cases in Index() for short strings
    {""s, "ab"s, -1},
    {"bc"s, "ab"s, -1},
    {"ab"s, "ab"s, 0},
    {"xab"s, "ab"s, 1},
    {"xab"s.substr(0, 2), "ab"s, -1},
    {""s, "abc"s, -1},
    {"xbc"s, "abc"s, -1},
    {"abc"s, "abc"s, 0},
    {"xabc"s, "abc"s, 1},
    {"xabc"s.substr(0, 3), "abc"s, -1},
    {"xabxc"s, "abc"s, -1},
    {""s, "abcd"s, -1},
    {"xbcd"s, "abcd"s, -1},
    {"abcd"s, "abcd"s, 0},
    {"xabcd"s, "abcd"s, 1},
    {"xyabcd"s.substr(0, 5), "abcd"s, -1},
    {"xbcqq"s, "abcqq"s, -1},
    {"abcqq"s, "abcqq"s, 0},
    {"xabcqq"s, "abcqq"s, 1},
    {"xyabcqq"s.substr(0, 6), "abcqq"s, -1},
    {"xabxcqq"s, "abcqq"s, -1},
    {"xabcqxq"s, "abcqq"s, -1},
    {""s, "01234567"s, -1},
    {"32145678"s, "01234567"s, -1},
    {"01234567"s, "01234567"s, 0},
    {"x01234567"s, "01234567"s, 1},
    {"x0123456x01234567"s, "01234567"s, 9},
    {"xx01234567"s.substr(0, 9), "01234567"s, -1},
    {""s, "0123456789"s, -1},
    {"3214567844"s, "0123456789"s, -1},
    {"0123456789"s, "0123456789"s, 0},
    {"x0123456789"s, "0123456789"s, 1},
    {"x012345678x0123456789"s, "0123456789"s, 11},
    {"xyz0123456789"s.substr(0, 12), "0123456789"s, -1},
    {"x01234567x89"s, "0123456789"s, -1},
    {""s, "0123456789012345"s, -1},
    {"3214567889012345"s, "0123456789012345"s, -1},
    {"0123456789012345"s, "0123456789012345"s, 0},
    {"x0123456789012345"s, "0123456789012345"s, 1},
    {"x012345678901234x0123456789012345"s, "0123456789012345"s, 17},
    {""s, "01234567890123456789"s, -1},
    {"32145678890123456789"s, "01234567890123456789"s, -1},
    {"01234567890123456789"s, "01234567890123456789"s, 0},
    {"x01234567890123456789"s, "01234567890123456789"s, 1},
    {"x0123456789012345678x01234567890123456789"s, "01234567890123456789"s, 21},
    {"xyz01234567890123456789"s.substr(0, 22), "01234567890123456789"s, -1},
    {""s, "0123456789012345678901234567890"s, -1},
    {"321456788901234567890123456789012345678911"s, "0123456789012345678901234567890"s, -1},
    {"0123456789012345678901234567890"s, "0123456789012345678901234567890"s, 0},
    {"x0123456789012345678901234567890"s, "0123456789012345678901234567890"s, 1},
    {"x012345678901234567890123456789x0123456789012345678901234567890"s, "0123456789012345678901234567890"s, 32},
    {"xyz0123456789012345678901234567890"s.substr(0, 33), "0123456789012345678901234567890"s, -1},
    {""s, "01234567890123456789012345678901"s, -1},
    {"32145678890123456789012345678901234567890211"s, "01234567890123456789012345678901"s, -1},
    {"01234567890123456789012345678901"s, "01234567890123456789012345678901"s, 0},
    {"x01234567890123456789012345678901"s, "01234567890123456789012345678901"s, 1},
    {"x0123456789012345678901234567890x01234567890123456789012345678901"s, "01234567890123456789012345678901"s, 33},
    {"xyz01234567890123456789012345678901"s.substr(0, 34), "01234567890123456789012345678901"s, -1},
    {"xxxxxx012345678901234567890123456789012345678901234567890123456789012"s, "012345678901234567890123456789012345678901234567890123456789012"s, 6},
    {""s, "0123456789012345678901234567890123456789"s, -1},
    {"xx012345678901234567890123456789012345678901234567890123456789012"s, "0123456789012345678901234567890123456789"s, 2},
    {"xx012345678901234567890123456789012345678901234567890123456789012"s.substr(0, 41), "0123456789012345678901234567890123456789"s, -1},
    {"xx012345678901234567890123456789012345678901234567890123456789012"s, "0123456789012345678901234567890123456xxx"s, -1},
    {"xx0123456789012345678901234567890123456789012345678901234567890120123456789012345678901234567890123456xxx"s, "0123456789012345678901234567890123456xxx"s, 65},
    // test fallback to Rabin-Karp.
    {"oxoxoxoxoxoxoxoxoxoxoxoy"s, "oy"s, 22},
    {"oxoxoxoxoxoxoxoxoxoxoxox"s, "oy"s, -1},
    // test fallback to IndexRune
    {"oxoxoxoxoxoxoxoxoxoxox☺"s, "☺"s, 22},
    // invalid UTF-8 byte sequence (must be longer than bytealg.MaxBruteForce to
    // test that we don't use IndexRune)
    {"xx0123456789012345678901234567890123456789012345678901234567890120123456789012345678901234567890123456xxx\xed\x9f\xc0"s, "\xed\x9f\xc0"s, 105},
};

const std::vector<IndexTest> lastIndexTests = {
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

const std::vector<IndexTest> indexAnyTests = {
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

const std::vector<IndexTest> lastIndexAnyTests = {
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
    {abcd, ""s, 2, {"a"s, "bcd"s}},
    {abcd, ""s, 4, {"a"s, "b"s, "c"s, "d"s}},
    {abcd, ""s, -1, {"a"s, "b"s, "c"s, "d"s}},
    {faces, ""s, -1, {"☺"s, "☻"s, "☹"s}},
    {faces, ""s, 3, {"☺"s, "☻"s, "☹"s}},
    {faces, ""s, 17, {"☺"s, "☻"s, "☹"s}},
    {"☺�☹"s, ""s, -1, {"☺"s, "�"s, "☹"s}},
    {abcd, "a"s, 0, {}},
    {abcd, "a"s, -1, {""s, "bcd"s}},
    {abcd, "z"s, -1, {"abcd"s}},
    {commas, ","s, -1, {"1"s, "2"s, "3"s, "4"s}},
    {dots, "..."s, -1, {"1"s, ".2"s, ".3"s, ".4"s}},
    {faces, "☹"s, -1, {"☺☻"s, ""s}},
    {faces, "~"s, -1, {faces}},
    {"1 2 3 4"s, " "s, 3, {"1"s, "2"s, "3 4"s}},
    {"1 2"s, " "s, 3, {"1"s, "2"s}},
    {""s, "T"s, maxInt / 4, {""s}},
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
    {"\u2000"s, {}},
    {"  abc  "s, {"abc"s}},
    {"1 2 3 4"s, {"1"s, "2"s, "3"s, "4"s}},
    {"1  2  3  4"s, {"1"s, "2"s, "3"s, "4"s}},
    {"1\t\t2\t\t3\t4"s, {"1"s, "2"s, "3"s, "4"s}},
    {"1\u20002\u20013\u20024"s, {"1"s, "2"s, "3"s, "4"s}},
    {"\u2000\u2001\u2002"s, {}},
    {"\n™\t™\n"s, {"™"s, "™"s}},
    {"\n\u20001™2\u2000 \u2001 ™"s, {"1™2"s, "™"s}},
    {"\n1\uFFFD \uFFFD2\u20003\uFFFD4"s, {"1\uFFFD"s, "\uFFFD2"s, "3\uFFFD4"s}},
    {"1\xFF\u2000\xFF" "2\xFF \xFF"s, {"1\xFF"s, "\xFF" "2\xFF"s, "\xFF"s}},
    {faces, {faces}},
};

const std::vector<FieldsTest> FieldsFuncTests = {
    {""s, {}},
    {"XX"s, {}},
    {"XXhiXXX"s, {"hi"s}},
    {"aXXbXXXcX"s, {"a"s, "b"s, "c"s}},
};

const std::vector<StringTest> upperTests = {
    {""s, ""s},
    {"ONLYUPPER"s, "ONLYUPPER"s},
    {"abc"s, "ABC"s},
    {"AbC123"s, "ABC123"s},
    {"azAZ09_"s, "AZAZ09_"s},
    {"longStrinGwitHmixofsmaLLandcAps"s, "LONGSTRINGWITHMIXOFSMALLANDCAPS"s},
    {"RENAN BASTOS 93 AOSDAJDJAIDJAIDAJIaidsjjaidijadsjiadjiOOKKO"s, "RENAN BASTOS 93 AOSDAJDJAIDJAIDAJIAIDSJJAIDIJADSJIADJIOOKKO"s},
    {"long\u0250string\u0250with\u0250nonascii\u2C6Fchars"s, "LONG\u2C6FSTRING\u2C6FWITH\u2C6FNONASCII\u2C6FCHARS"s},
    {"\u0250\u0250\u0250\u0250\u0250"s, "\u2C6F\u2C6F\u2C6F\u2C6F\u2C6F"s}, // grows one byte per char
    {"a\u0080\U0010FFFF"s, "A\u0080\U0010FFFF"s},                           // test utf8.RuneSelf and utf8.MaxRune
};

const std::vector<StringTest> lowerTests = {
    {""s, ""s},
    {"abc"s, "abc"s},
    {"AbC123"s, "abc123"s},
    {"azAZ09_"s, "azaz09_"s},
    {"longStrinGwitHmixofsmaLLandcAps"s, "longstringwithmixofsmallandcaps"s},
    {"renan bastos 93 AOSDAJDJAIDJAIDAJIaidsjjaidijadsjiadjiOOKKO"s, "renan bastos 93 aosdajdjaidjaidajiaidsjjaidijadsjiadjiookko"s},
    {"LONG\u2C6FSTRING\u2C6FWITH\u2C6FNONASCII\u2C6FCHARS"s, "long\u0250string\u0250with\u0250nonascii\u0250chars"s},
    {"\u2C6D\u2C6D\u2C6D\u2C6D\u2C6D"s, "\u0251\u0251\u0251\u0251\u0251"s}, // shrinks one byte per char
    {"A\u0080\U0010FFFF"s, "a\u0080\U0010FFFF"s},                           // test utf8.RuneSelf and utf8.MaxRune
};

const std::vector<StringTest> trimSpaceTests = {
    {""s, ""s},
    {"abc"s, "abc"s},
    {space + "abc"s + space, "abc"s},
    {" "s, ""s},
    {" \t\r\n \t\t\r\r\n\n "s, ""s},
    {" \t\r\n x\t\t\r\r\n\n "s, "x"s},
    {" \u2000\t\r\n x\t\t\r\r\ny\n \u3000"s, "x\t\t\r\r\ny"s},
    {"1 \t\r\n2"s, "1 \t\r\n2"s},
    {" x\x80"s, "x\x80"s},
    {" x\xc0"s, "x\xc0"s},
    {"x \xc0\xc0 "s, "x \xc0\xc0"s},
    {"x \xc0"s, "x \xc0"s},
    {"x \xc0 "s, "x \xc0"s},
    {"x \xc0\xc0 "s, "x \xc0\xc0"s},
    {"x ☺\xc0\xc0 "s, "x ☺\xc0\xc0"s},
    {"x ☺ "s, "x ☺"s},
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

const std::vector<TrimFuncTest> trimFuncTests = {
    {isSpace, space + " hello "s + space,
        "hello"s,
        "hello "s + space,
        space + " hello"s},
    {isDigit, "\u0e50\u0e5212hello34\u0e50\u0e51"s,
        "hello"s,
        "hello34\u0e50\u0e51"s,
        "\u0e50\u0e5212hello"s},
    {isUpper, "\u2C6F\u2C6F\u2C6F\u2C6FABCDhelloEF\u2C6F\u2C6FGH\u2C6F\u2C6F"s,
        "hello"s,
        "helloEF\u2C6F\u2C6FGH\u2C6F\u2C6F"s,
        "\u2C6F\u2C6F\u2C6F\u2C6FABCDhello"s},
    {not_(isSpace), "hello"s + space + "hello"s,
        space,
        space + "hello"s,
        "hello"s + space},
    {not_(isDigit), "hello\u0e50\u0e521234\u0e50\u0e51helo"s,
        "\u0e50\u0e521234\u0e50\u0e51"s,
        "\u0e50\u0e521234\u0e50\u0e51helo"s,
        "hello\u0e50\u0e521234\u0e50\u0e51"s},
    {isValidRune, "ab\xc0" "a\xc0" "cd"s,
        "\xc0" "a\xc0"s,
        "\xc0" "a\xc0" "cd"s,
        "ab\xc0" "a\xc0"s},
    {not_(isValidRune), "\xc0" "a\xc0"s,
        "a"s,
        "a\xc0"s,
        "\xc0" "a"s},
    {isSpace, ""s,
        ""s,
        ""s,
        ""s},
    {isSpace, " "s,
        ""s,
        ""s,
        ""s},
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
    {"\x80\x80\x80\x80"s, not_(isValidRune), 0, 3},
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

const std::vector<ContainsTest> ContainsTests = {
    {"abc"s, "bc"s, true},
    {"abc"s, "bcd"s, false},
    {"abc"s, ""s, true},
    {""s, "a"s, false},

    // cases to cover code in runtime/asm_amd64.s:indexShortStr
    // 2-byte needle
    {"xxxxxx"s, "01"s, false},
    {"01xxxx"s, "01"s, true},
    {"xx01xx"s, "01"s, true},
    {"xxxx01"s, "01"s, true},
    {"01xxxxx"s.substr(1), "01"s, false},
    {"xxxxx01"s.substr(0, 6), "01"s, false},
    // 3-byte needle
    {"xxxxxxx"s, "012"s, false},
    {"012xxxx"s, "012"s, true},
    {"xx012xx"s, "012"s, true},
    {"xxxx012"s, "012"s, true},
    {"012xxxxx"s.substr(1), "012"s, false},
    {"xxxxx012"s.substr(0, 7), "012"s, false},
    // 4-byte needle
    {"xxxxxxxx"s, "0123"s, false},
    {"0123xxxx"s, "0123"s, true},
    {"xx0123xx"s, "0123"s, true},
    {"xxxx0123"s, "0123"s, true},
    {"0123xxxxx"s.substr(1), "0123"s, false},
    {"xxxxx0123"s.substr(0, 8), "0123"s, false},
    // 5-7-byte needle
    {"xxxxxxxxx"s, "01234"s, false},
    {"01234xxxx"s, "01234"s, true},
    {"xx01234xx"s, "01234"s, true},
    {"xxxx01234"s, "01234"s, true},
    {"01234xxxxx"s.substr(1), "01234"s, false},
    {"xxxxx01234"s.substr(0, 9), "01234"s, false},
    // 8-byte needle
    {"xxxxxxxxxxxx"s, "01234567"s, false},
    {"01234567xxxx"s, "01234567"s, true},
    {"xx01234567xx"s, "01234567"s, true},
    {"xxxx01234567"s, "01234567"s, true},
    {"01234567xxxxx"s.substr(1), "01234567"s, false},
    {"xxxxx01234567"s.substr(0, 12), "01234567"s, false},
    // 9-15-byte needle
    {"xxxxxxxxxxxxx"s, "012345678"s, false},
    {"012345678xxxx"s, "012345678"s, true},
    {"xx012345678xx"s, "012345678"s, true},
    {"xxxx012345678"s, "012345678"s, true},
    {"012345678xxxxx"s.substr(1), "012345678"s, false},
    {"xxxxx012345678"s.substr(0, 13), "012345678"s, false},
    // 16-byte needle
    {"xxxxxxxxxxxxxxxxxxxx"s, "0123456789ABCDEF"s, false},
    {"0123456789ABCDEFxxxx"s, "0123456789ABCDEF"s, true},
    {"xx0123456789ABCDEFxx"s, "0123456789ABCDEF"s, true},
    {"xxxx0123456789ABCDEF"s, "0123456789ABCDEF"s, true},
    {"0123456789ABCDEFxxxxx"s.substr(1), "0123456789ABCDEF"s, false},
    {"xxxxx0123456789ABCDEF"s.substr(0, 20), "0123456789ABCDEF"s, false},
    // 17-31-byte needle
    {"xxxxxxxxxxxxxxxxxxxxx"s, "0123456789ABCDEFG"s, false},
    {"0123456789ABCDEFGxxxx"s, "0123456789ABCDEFG"s, true},
    {"xx0123456789ABCDEFGxx"s, "0123456789ABCDEFG"s, true},
    {"xxxx0123456789ABCDEFG"s, "0123456789ABCDEFG"s, true},
    {"0123456789ABCDEFGxxxxx"s.substr(1), "0123456789ABCDEFG"s, false},
    {"xxxxx0123456789ABCDEFG"s.substr(0, 21), "0123456789ABCDEFG"s, false},

    // partial match cases
    {"xx01x"s, "012"s, false},                             // 3
    {"xx0123x"s, "01234"s, false},                         // 5-7
    {"xx01234567x"s, "012345678"s, false},                 // 9-15
    {"xx0123456789ABCDEFx"s, "0123456789ABCDEFG"s, false}, // 17-31, issue 15679
};

const std::vector<ContainsTest> ContainsAnyTests = {
    {""s, ""s, false},
    {""s, "a"s, false},
    {""s, "abc"s, false},
    {"a"s, ""s, false},
    {"a"s, "a"s, true},
    {"aaa"s, "a"s, true},
    {"abc"s, "xyz"s, false},
    {"abc"s, "xcz"s, true},
    {"a☺b☻c☹d"s, "uvw☻xyz"s, true},
    {"aRegExp*"s, ".(|)*+?^$[]"s, true},
    {dots + dots + dots, " "s, false},
};

const std::vector<ContainsRuneTest> ContainsRuneTests = {
    {""s, U'a', false},
    {"a"s, U'a', true},
    {"aaa"s, U'a', true},
    {"abc"s, U'y', false},
    {"abc"s, U'c', true},
    {"a☺b☻c☹d"s, U'x', false},
    {"a☺b☻c☹d"s, U'☻', true},
    {"aRegExp*"s, U'*', true},
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
    {"1"s, "2"s, false},
    {"utf-8"s, "US-ASCII"s, false},
};

const std::vector<CountTest> CountTests = {
    {""s, ""s, 1},
    {""s, "notempty"s, 0},
    {"notempty"s, ""s, 9},
    {"smaller"s, "not smaller"s, 0},
    {"12345678987654321"s, "6"s, 2},
    {"611161116"s, "6"s, 3},
    {"notequal"s, "NotEqual"s, 0},
    {"equal"s, "equal"s, 1},
    {"abc1231231123q"s, "123"s, 3},
    {"11111"s, "11"s, 2},
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

TEST(GoStrings, Lines) {
    for (const auto& s : linesTests) EXPECT_EQ(collect(strings::Lines(s.a)), s.b) << "Lines(" << s.a << ")";
}

// Execute f on each test case. funcName should be the name of f; it's
// used in failure reports.
void runIndexTests(isize (*f)(std::string_view, std::string_view) noexcept, const char* funcName,
                   const std::vector<IndexTest>& testCases) {
    for (const auto& test : testCases) {
        EXPECT_EQ(f(test.s, test.sep), test.out) << funcName << "(" << test.s << "," << test.sep << ")";
    }
}

TEST(GoStrings, Index) { runIndexTests(strings::Index, "Index", indexTests); }
TEST(GoStrings, LastIndex) { runIndexTests(strings::LastIndex, "LastIndex", lastIndexTests); }
TEST(GoStrings, IndexAny) { runIndexTests(strings::IndexAny, "IndexAny", indexAnyTests); }
TEST(GoStrings, LastIndexAny) { runIndexTests(strings::LastIndexAny, "LastIndexAny", lastIndexAnyTests); }

TEST(GoStrings, IndexByte) {
    for (const auto& tt : indexTests) {
        if (tt.sep.size() != 1) continue;
        EXPECT_EQ(strings::IndexByte(tt.s, tt.sep[0]), tt.out) << "IndexByte(" << tt.s << ", '" << tt.sep << "')";
    }
}

TEST(GoStrings, LastIndexByte) {
    const std::vector<IndexTest> testCases = {
        {"", "q", -1},
        {"abcdef", "q", -1},
        {"abcdefabcdef", "a", len("abcdef")},       // something in the middle
        {"abcdefabcdef", "f", len("abcdefabcde")},  // last byte
        {"zabcdefabcdef", "z", 0},                  // first byte
        {"a☺b☻c☹d", "b", len("a☺")},                // non-ascii
    };
    for (const auto& test : testCases) {
        EXPECT_EQ(strings::LastIndexByte(test.s, test.sep[0]), test.out) << "LastIndexByte(" << test.s << "," << test.sep << ")";
    }
}

TEST(GoStrings, Split) {
    for (const auto& tt : splittests) {
        auto a = strings::SplitN(tt.s, tt.sep, tt.n);
        if (strs(a) != tt.a) {
            ADD_FAILURE() << "Split(" << tt.s << ", " << tt.sep << ", " << tt.n << ")";
            continue;
        }
        if (tt.n < 0) {
            EXPECT_EQ(collect(strings::SplitSeq(tt.s, tt.sep)), tt.a) << "collect(SplitSeq(" << tt.s << ", " << tt.sep << "))";
        }
        if (tt.n == 0) continue;
        EXPECT_EQ(strings::Join(a, tt.sep), tt.s) << "Join(Split(" << tt.s << ", " << tt.sep << ", " << tt.n << "))";
        if (tt.n < 0) {
            EXPECT_EQ(strs(strings::Split(tt.s, tt.sep)), strs(a)) << "Split disagrees with SplitN(" << tt.s << ")";
        }
    }
}

TEST(GoStrings, SplitAfter) {
    for (const auto& tt : splitaftertests) {
        auto a = strings::SplitAfterN(tt.s, tt.sep, tt.n);
        if (strs(a) != tt.a) {
            ADD_FAILURE() << "SplitAfter(" << tt.s << ", " << tt.sep << ", " << tt.n << ")";
            continue;
        }
        if (tt.n < 0) {
            EXPECT_EQ(collect(strings::SplitAfterSeq(tt.s, tt.sep)), tt.a) << "collect(SplitAfterSeq(" << tt.s << ", " << tt.sep << "))";
        }
        EXPECT_EQ(strings::Join(a, ""), tt.s) << "Join(SplitAfter(" << tt.s << ", " << tt.sep << ", " << tt.n << "))";
        if (tt.n < 0) {
            EXPECT_EQ(strs(strings::SplitAfter(tt.s, tt.sep)), strs(a)) << "SplitAfter disagrees with SplitAfterN(" << tt.s << ")";
        }
    }
}

TEST(GoStrings, Fields) {
    for (const auto& tt : fieldstests) {
        auto a = strings::Fields(tt.s);
        if (strs(a) != tt.a) {
            ADD_FAILURE() << "Fields(" << tt.s << ")";
            continue;
        }
        EXPECT_EQ(collect(strings::FieldsSeq(tt.s)), tt.a) << "collect(FieldsSeq(" << tt.s << "))";
    }
}

TEST(GoStrings, FieldsFunc) {
    for (const auto& tt : fieldstests) {
        EXPECT_EQ(strs(strings::FieldsFunc(tt.s, unicode::IsSpace)), tt.a) << "FieldsFunc(" << tt.s << ", unicode.IsSpace)";
    }
    auto pred = [](rune c) { return c == 'X'; };
    for (const auto& tt : FieldsFuncTests) {
        EXPECT_EQ(strs(strings::FieldsFunc(tt.s, pred)), tt.a) << "FieldsFunc(" << tt.s << ")";
        EXPECT_EQ(collect(strings::FieldsFuncSeq(tt.s, pred)), tt.a) << "collect(FieldsFuncSeq(" << tt.s << "))";
    }
}

void runStringTests(const std::function<string(const string&)>& f, const char* funcName, const std::vector<StringTest>& testCases) {
    for (const auto& tc : testCases) EXPECT_EQ(f(tc.in), tc.out) << funcName << "(" << tc.in << ")";
}

std::string tenRunes(rune ch) {
    std::string s;
    for (int i = 0; i < 10; i++) s += std::string(asStringView(utf8::AppendRune({}, ch)));
    return s;
}

// User-defined self-inverse mapping function
rune rot13(rune r) {
    rune step = 13;
    if (r >= 'a' && r <= 'z') return ((r - 'a' + step) % 26) + 'a';
    if (r >= 'A' && r <= 'Z') return ((r - 'A' + step) % 26) + 'A';
    return r;
}

std::string enc(rune r) { return std::string(asStringView(utf8::AppendRune({}, r))); }

TEST(GoStrings, Map) {
    // Run a couple of awful growth/shrinkage tests
    auto a = tenRunes('a');
    // 1. Grow. This triggers two reallocations in Map.
    auto maxRune = [](rune) { return unicode::MaxRune; };
    EXPECT_EQ(strings::Map(maxRune, a), tenRunes(unicode::MaxRune)) << "growing";

    // 2. Shrink
    auto minRune = [](rune) { return rune('a'); };
    EXPECT_EQ(strings::Map(minRune, tenRunes(unicode::MaxRune)), a) << "shrinking";

    // 3. Rot13
    EXPECT_EQ(strings::Map(rot13, "a to zed"), "n gb mrq") << "rot13";

    // 4. Rot13^2
    EXPECT_EQ(strings::Map(rot13, strings::Map(rot13, "a to zed")), "a to zed") << "rot13";

    // 5. Drop
    auto dropNotLatin = [](rune r) -> rune {
        if (unicode::Is(unicode::Latin, r)) return r;
        return -1;
    };
    EXPECT_EQ(strings::Map(dropNotLatin, "Hello, 세계"), "Hello") << "drop";

    // 6. Identity
    auto identity = [](rune r) { return r; };
    string orig = "Input string that we expect not to be copied.";
    auto m = strings::Map(identity, orig);
    EXPECT_EQ(orig.data(), m.data()) << "unexpected copy during identity map";

    // 7. Handle invalid UTF-8 sequence
    auto replaceNotLatin = [](rune r) {
        if (unicode::Is(unicode::Latin, r)) return r;
        return utf8::RuneError;
    };
    EXPECT_EQ(strings::Map(replaceNotLatin, "Hello\255World"), "Hello�World") << "replace invalid sequence";

    // 8. Check utf8.RuneSelf and utf8.MaxRune encoding
    auto encode = [](rune r) {
        switch (r) {
        case utf8::RuneSelf:
            return unicode::MaxRune;
        case unicode::MaxRune:
            return utf8::RuneSelf;
        }
        return r;
    };
    auto s = enc(utf8::RuneSelf) + enc(utf8::MaxRune);
    auto r = enc(utf8::MaxRune) + enc(utf8::RuneSelf);  // reverse of s
    EXPECT_EQ(strings::Map(encode, s), r) << "encoding not handled correctly";
    EXPECT_EQ(strings::Map(encode, r), s) << "encoding not handled correctly";

    // 9. Check mapping occurs in the front, middle and back
    auto trimSpaces = [](rune r) -> rune {
        if (unicode::IsSpace(r)) return -1;
        return r;
    };
    EXPECT_EQ(strings::Map(trimSpaces, "   abc    123   "), "abc123") << "trimSpaces";
}

TEST(GoStrings, ToUpper) {
    runStringTests([](const string& s) { return strings::ToUpper(s); }, "ToUpper", upperTests);
}

TEST(GoStrings, ToLower) {
    runStringTests([](const string& s) { return strings::ToLower(s); }, "ToLower", lowerTests);
}

TEST(GoStrings, ToValidUTF8) {
    for (const auto& tc : toValidUTF8Tests) {
        EXPECT_EQ(strings::ToValidUTF8(tc.in, tc.repl), tc.out) << "ToValidUTF8(" << tc.in << ", " << tc.repl << ")";
    }
}

TEST(GoStrings, SpecialCase) {
    string lower = "abcçdefgğhıijklmnoöprsştuüvyz";
    string upper = "ABCÇDEFGĞHIİJKLMNOÖPRSŞTUÜVYZ";
    EXPECT_EQ(strings::ToUpperSpecial(unicode::TurkishCase, upper), upper) << "Upper(upper)";
    EXPECT_EQ(strings::ToUpperSpecial(unicode::TurkishCase, lower), upper) << "Upper(lower)";
    EXPECT_EQ(strings::ToLowerSpecial(unicode::TurkishCase, lower), lower) << "Lower(lower)";
    EXPECT_EQ(strings::ToLowerSpecial(unicode::TurkishCase, upper), lower) << "Lower(upper)";
}

TEST(GoStrings, TrimSpace) {
    runStringTests([](const string& s) { return strings::TrimSpace(s); }, "TrimSpace", trimSpaceTests);
}

TEST(GoStrings, Trim) {
    for (const auto& tc : trimTests) {
        string actual;
        if (tc.f == "Trim") {
            actual = strings::Trim(tc.in, tc.arg);
        } else if (tc.f == "TrimLeft") {
            actual = strings::TrimLeft(tc.in, tc.arg);
        } else if (tc.f == "TrimRight") {
            actual = strings::TrimRight(tc.in, tc.arg);
        } else if (tc.f == "TrimPrefix") {
            actual = strings::TrimPrefix(tc.in, tc.arg);
        } else if (tc.f == "TrimSuffix") {
            actual = strings::TrimSuffix(tc.in, tc.arg);
        } else {
            ADD_FAILURE() << "Undefined trim function " << tc.f;
            continue;
        }
        EXPECT_EQ(actual, tc.out) << tc.f << "(" << tc.in << ", " << tc.arg << ")";
    }
}

TEST(GoStrings, TrimFunc) {
    for (const auto& tc : trimFuncTests) {
        EXPECT_EQ(strings::TrimFunc(tc.in, tc.f.f), tc.trimOut) << "TrimFunc(" << tc.in << ", " << tc.f.name << ")";
        EXPECT_EQ(strings::TrimLeftFunc(tc.in, tc.f.f), tc.leftOut) << "TrimLeftFunc(" << tc.in << ", " << tc.f.name << ")";
        EXPECT_EQ(strings::TrimRightFunc(tc.in, tc.f.f), tc.rightOut) << "TrimRightFunc(" << tc.in << ", " << tc.f.name << ")";
    }
}

TEST(GoStrings, IndexFunc) {
    for (const auto& tc : indexFuncTests) {
        EXPECT_EQ(strings::IndexFunc(tc.in, tc.f.f), tc.first) << "IndexFunc(" << tc.in << ", " << tc.f.name << ")";
        EXPECT_EQ(strings::LastIndexFunc(tc.in, tc.f.f), tc.last) << "LastIndexFunc(" << tc.in << ", " << tc.f.name << ")";
    }
}

TEST(GoStrings, CaseConsistency) {
    // Make a string of all the runes.
    constexpr int numRunes = int(unicode::MaxRune + 1);
    std::string a;
    for (rune i = 0; i < numRunes; i++) a += enc(i);
    string s(a);
    // convert the cases.
    auto upper = strings::ToUpper(s);
    auto lower = strings::ToLower(s);

    // Consistency checks
    EXPECT_EQ(utf8::RuneCountInString(upper), numRunes) << "rune count wrong in upper";
    EXPECT_EQ(utf8::RuneCountInString(lower), numRunes) << "rune count wrong in lower";
    EXPECT_EQ(strings::ToUpper(upper), upper) << "ToUpper(upper) consistency fail";
    EXPECT_EQ(strings::ToLower(lower), lower) << "ToLower(lower) consistency fail";
}

TEST(GoStrings, Repeat) {
    std::string longString = "a" + std::string(1 << 16, '\0') + "z";
    std::string longSpaces(200, ' ');
    struct T {
        std::string in, out;
        isize count;
    };
    const std::vector<T> RepeatTests = {
        {"", "", 0},
        {"", "", 1},
        {"", "", 2},
        {"-", "", 0},
        {"-", "-", 1},
        {"-", "----------", 10},
        {"abc ", "abc abc abc ", 3},
        {" ", " ", 1},
        {"--", "----", 2},
        {"===", "======", 2},
        {"000", "000000000", 3},
        {"\t\t\t\t", "\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t\t", 4},
        {" ", longSpaces, len(longSpaces)},
        // Tests for results over the chunkLimit
        {std::string(1, '\0'), std::string(1 << 16, '\0'), 1 << 16},
        {longString, longString + longString, 2},
    };
    for (const auto& tt : RepeatTests) EXPECT_EQ(strings::Repeat(tt.in, tt.count), tt.out) << "Repeat(" << tt.in << ", " << tt.count << ")";
}

TEST(GoStrings, RepeatCatchesOverflow) {
    // Repeat panics, which go++ can't recover from: death tests.
    EXPECT_DEATH((void)strings::Repeat("--", -2147483647), "strings: negative Repeat count");
    (void)strings::Repeat("", maxInt);
    EXPECT_DEATH((void)strings::Repeat("-", -1), "negative");
    EXPECT_DEATH((void)strings::Repeat(std::string(255, '\0'), isize(~std::size_t(0) / 255 + 1)), "overflow");
}

TEST(GoStrings, Replace) {
    for (const auto& tt : ReplaceTests) {
        EXPECT_EQ(strings::Replace(tt.in, tt.old, tt.new_, tt.n), tt.out) << "Replace(" << tt.in << ", " << tt.old << ", " << tt.new_ << ", " << tt.n << ")";
        if (tt.n == -1) {
            EXPECT_EQ(strings::ReplaceAll(tt.in, tt.old, tt.new_), tt.out) << "ReplaceAll(" << tt.in << ")";
        }
    }
    // Not in Go's test: Replace returns s itself when nothing is replaced.
    string s = "no replacements here";
    EXPECT_EQ(strings::Replace(s, "x", "y", -1).data(), s.data());
}

TEST(GoStrings, Contains) {
    for (const auto& ct : ContainsTests) {
        EXPECT_EQ(strings::Contains(ct.str, ct.substr), ct.expected) << "Contains(" << ct.str << ", " << ct.substr << ")";
    }
}

TEST(GoStrings, ContainsAny) {
    for (const auto& ct : ContainsAnyTests) {
        EXPECT_EQ(strings::ContainsAny(ct.str, ct.substr), ct.expected) << "ContainsAny(" << ct.str << ", " << ct.substr << ")";
    }
}

TEST(GoStrings, ContainsRune) {
    for (const auto& ct : ContainsRuneTests) {
        EXPECT_EQ(strings::ContainsRune(ct.str, ct.r), ct.expected) << "ContainsRune(" << ct.str << ", " << ct.r << ")";
    }
}

TEST(GoStrings, ContainsFunc) {
    for (const auto& ct : ContainsRuneTests) {
        EXPECT_EQ(strings::ContainsFunc(ct.str, [&](rune r) { return ct.r == r; }), ct.expected)
            << "ContainsFunc(" << ct.str << ", func(" << ct.r << "))";
    }
}

TEST(GoStrings, EqualFold) {
    for (const auto& tt : EqualFoldTests) {
        EXPECT_EQ(strings::EqualFold(tt.s, tt.t), tt.out) << "EqualFold(" << tt.s << ", " << tt.t << ")";
        EXPECT_EQ(strings::EqualFold(tt.t, tt.s), tt.out) << "EqualFold(" << tt.t << ", " << tt.s << ")";
    }
}

TEST(GoStrings, Count) {
    for (const auto& tt : CountTests) EXPECT_EQ(strings::Count(tt.s, tt.sep), tt.num) << "Count(" << tt.s << ", " << tt.sep << ")";
}

TEST(GoStrings, Cut) {
    for (const auto& tt : cutTests) {
        auto [before, after, found] = strings::Cut(tt.s, tt.sep);
        EXPECT_TRUE(before == tt.before && after == tt.after && found == tt.found) << "Cut(" << tt.s << ", " << tt.sep << ")";
    }
}

TEST(GoStrings, CutLast) {
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
        auto [before, after, found] = strings::CutLast(tt.s, tt.sep);
        EXPECT_TRUE(before == tt.before && after == tt.after && found == tt.found) << "CutLast(" << tt.s << ", " << tt.sep << ")";
    }
}

TEST(GoStrings, CutPrefix) {
    for (const auto& tt : cutPrefixTests) {
        auto [after, found] = strings::CutPrefix(tt.s, tt.sep);
        EXPECT_TRUE(after == tt.after && found == tt.found) << "CutPrefix(" << tt.s << ", " << tt.sep << ")";
    }
}

TEST(GoStrings, CutSuffix) {
    for (const auto& tt : cutSuffixTests) {
        auto [before, found] = strings::CutSuffix(tt.s, tt.sep);
        EXPECT_TRUE(before == tt.before && found == tt.found) << "CutSuffix(" << tt.s << ", " << tt.sep << ")";
    }
}

TEST(GoStrings, BuilderWriteInvalidRune) {
    // Invalid runes, including negative ones, should be written as
    // utf8.RuneError.
    for (rune r : {rune(-1), utf8::MaxRune + 1}) {
        strings::Builder b;
        b.WriteRune(r);
        EXPECT_EQ(b.String(), "�");
    }
}

// Not in Go's tests: the parts share the input's buffer.
TEST(GoStrings, SubstringsShareBuffer) {
    string s = "  hello world  ";
    auto t = strings::TrimSpace(s);
    EXPECT_EQ(t.data(), s.data() + 2);
    auto f = strings::Fields(s);
    ASSERT_EQ(f.len(), 2u);
    EXPECT_EQ(f[1].data(), s.data() + 8);
    EXPECT_EQ(strings::ToLower(s).data(), s.data());  // already lower case
}

}  // namespace
