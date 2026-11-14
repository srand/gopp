// Ported from Go 1.27.3's regexp/find_test.go and all_test.go, with Go's
// test names. Left out: the tests of Copy (deprecated), of UnmarshalText,
// of unexported functions, and the benchmarks.
#include <gopp/bytes.hpp>
#include <gopp/regexp.hpp>
#include <gopp/regexp/syntax.hpp>
#include <gopp/strings.hpp>
#include <gopp/sync.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <functional>
#include <optional>
#include <string>
#include <vector>

using namespace gopp;

namespace {

using matchList = std::optional<std::vector<std::vector<int>>>;

// build is a helper to construct a [][]int by extracting n sequences from
// x. This represents n matches with len(x)/n submatches each.
matchList build(std::size_t n, std::vector<int> x) {
    std::vector<std::vector<int>> ret(n);
    std::size_t runLength = x.size() / n;
    std::size_t j = 0;
    for (auto& r : ret) {
        r.assign(x.begin() + isize(j), x.begin() + isize(j + runLength));
        j += runLength;
    }
    return ret;
}

// For each pattern/text pair, what is the expected output of each
// function? We can derive the textual results from the indexed results,
// the non-submatch results from the submatched results, the single results
// from the 'all' results, and the byte results from the string results.
// Therefore the table includes only the FindAllStringSubmatchIndex result.
struct FindTest {
    std::string pat;
    std::string text;
    int max;
    matchList matches;
};

std::ostream& operator<<(std::ostream& os, const FindTest& t) {
    return os << "pat: " << strconv::Quote(t.pat) << " text: " << strconv::Quote(t.text);
}

const std::vector<FindTest>& findTests() {
    static const std::vector<FindTest> tests = {

	{std::string("", 0), std::string("", 0), -1, build(1, {0, 0})},
	{std::string("^abcdefg", 8), std::string("abcdefg", 7), -1, build(1, {0, 7})},
	{std::string("a+", 2), std::string("baaab", 5), -1, build(1, {1, 4})},
	{std::string("a", 1), std::string("bababaab", 8), -1, build(4, {1, 2, 3, 4, 5, 6, 6, 7})},
	{std::string("a", 1), std::string("bababaab", 8), 0, std::nullopt},
	{std::string("a", 1), std::string("bababaab", 8), 1, build(1, {1, 2})},
	{std::string("a", 1), std::string("bababaab", 8), 2, build(2, {1, 2, 3, 4})},
	{std::string("a", 1), std::string("bababaab", 8), 3, build(3, {1, 2, 3, 4, 5, 6})},
	{std::string("a", 1), std::string("bababaab", 8), 4, build(4, {1, 2, 3, 4, 5, 6, 6, 7})},
	{std::string("a", 1), std::string("bababaab", 8), 5, build(4, {1, 2, 3, 4, 5, 6, 6, 7})},
	{std::string("abcd..", 6), std::string("abcdef", 6), -1, build(1, {0, 6})},
	{std::string("a", 1), std::string("a", 1), -1, build(1, {0, 1})},
	{std::string("x", 1), std::string("y", 1), -1, std::nullopt},
	{std::string("b", 1), std::string("abc", 3), -1, build(1, {1, 2})},
	{std::string(".", 1), std::string("a", 1), -1, build(1, {0, 1})},
	{std::string(".*", 2), std::string("abcdef", 6), -1, build(1, {0, 6})},
	{std::string("^", 1), std::string("abcde", 5), -1, build(1, {0, 0})},
	{std::string("$", 1), std::string("abcde", 5), -1, build(1, {5, 5})},
	{std::string("^abcd$", 6), std::string("abcd", 4), -1, build(1, {0, 4})},
	{std::string("^bcd'", 5), std::string("abcdef", 6), -1, std::nullopt},
	{std::string("^abcd$", 6), std::string("abcde", 5), -1, std::nullopt},
	{std::string("a+", 2), std::string("baaab", 5), -1, build(1, {1, 4})},
	{std::string("a*", 2), std::string("baaab", 5), -1, build(3, {0, 0, 1, 4, 5, 5})},
	{std::string("[a-z]+", 6), std::string("abcd", 4), -1, build(1, {0, 4})},
	{std::string("[^a-z]+", 7), std::string("ab1234cd", 8), -1, build(1, {2, 6})},
	{std::string("[a\134-\134]z]+", 9), std::string("az]-bcz", 7), -1, build(2, {0, 4, 6, 7})},
	{std::string("[^\134n]+", 6), std::string("abcd\012", 5), -1, build(1, {0, 4})},
	{std::string("[\346\227\245\346\234\254\350\252\236]+", 12), std::string("\346\227\245\346\234\254\350\252\236\346\227\245\346\234\254\350\252\236", 18), -1, build(1, {0, 18})},
	{std::string("\346\227\245\346\234\254\350\252\236+", 10), std::string("\346\227\245\346\234\254\350\252\236", 9), -1, build(1, {0, 9})},
	{std::string("\346\227\245\346\234\254\350\252\236+", 10), std::string("\346\227\245\346\234\254\350\252\236\350\252\236\350\252\236\350\252\236", 18), -1, build(1, {0, 18})},
	{std::string("()", 2), std::string("", 0), -1, build(1, {0, 0, 0, 0})},
	{std::string("(a)", 3), std::string("a", 1), -1, build(1, {0, 1, 0, 1})},
	{std::string("(.)(.)", 6), std::string("\346\227\245a", 4), -1, build(1, {0, 4, 0, 3, 3, 4})},
	{std::string("(.*)", 4), std::string("", 0), -1, build(1, {0, 0, 0, 0})},
	{std::string("(.*)", 4), std::string("abcd", 4), -1, build(1, {0, 4, 0, 4})},
	{std::string("(..)(..)", 8), std::string("abcd", 4), -1, build(1, {0, 4, 0, 2, 2, 4})},
	{std::string("(([^xyz]*)(d))", 14), std::string("abcd", 4), -1, build(1, {0, 4, 0, 4, 0, 3, 3, 4})},
	{std::string("((a|b|c)*(d))", 13), std::string("abcd", 4), -1, build(1, {0, 4, 0, 4, 2, 3, 3, 4})},
	{std::string("(((a|b|c)*)(d))", 15), std::string("abcd", 4), -1, build(1, {0, 4, 0, 4, 0, 3, 2, 3, 3, 4})},
	{std::string("\134a\134f\134n\134r\134t\134v", 12), std::string("\007\014\012\015\011\013", 6), -1, build(1, {0, 6})},
	{std::string("[\134a\134f\134n\134r\134t\134v]+", 15), std::string("\007\014\012\015\011\013", 6), -1, build(1, {0, 6})},

	{std::string("a*(|(b))c*", 10), std::string("aacc", 4), -1, build(1, {0, 4, 2, 2, -1, -1})},
	{std::string("(.*).*", 6), std::string("ab", 2), -1, build(1, {0, 2, 0, 2})},
	{std::string("[.]", 3), std::string(".", 1), -1, build(1, {0, 1})},
	{std::string("/$", 2), std::string("/abc/", 5), -1, build(1, {4, 5})},
	{std::string("/$", 2), std::string("/abc", 4), -1, std::nullopt},

	// multiple matches
	{std::string(".", 1), std::string("abc", 3), -1, build(3, {0, 1, 1, 2, 2, 3})},
	{std::string("(.)", 3), std::string("abc", 3), -1, build(3, {0, 1, 0, 1, 1, 2, 1, 2, 2, 3, 2, 3})},
	{std::string(".(.)", 4), std::string("abcd", 4), -1, build(2, {0, 2, 1, 2, 2, 4, 3, 4})},
	{std::string("ab*", 3), std::string("abbaab", 6), -1, build(3, {0, 3, 3, 4, 4, 6})},
	{std::string("a(b*)", 5), std::string("abbaab", 6), -1, build(3, {0, 3, 1, 3, 3, 4, 4, 4, 4, 6, 5, 6})},

	// fixed bugs
	{std::string("ab$", 3), std::string("cab", 3), -1, build(1, {1, 3})},
	{std::string("axxb$", 5), std::string("axxcb", 5), -1, std::nullopt},
	{std::string("data", 4), std::string("daXY data", 9), -1, build(1, {5, 9})},
	{std::string("da(.)a$", 7), std::string("daXY data", 9), -1, build(1, {5, 9, 7, 8})},
	{std::string("zx+", 3), std::string("zzx", 3), -1, build(1, {1, 3})},
	{std::string("ab$", 3), std::string("abcab", 5), -1, build(1, {3, 5})},
	{std::string("(aa)*$", 6), std::string("a", 1), -1, build(1, {1, 1, -1, -1})},
	{std::string("(\077:.|(\077:.a))", 12), std::string("", 0), -1, std::nullopt},
	{std::string("(\077:A(\077:A|a))", 12), std::string("Aa", 2), -1, build(1, {0, 2})},
	{std::string("(\077:A|(\077:A|a))", 13), std::string("a", 1), -1, build(1, {0, 1})},
	{std::string("(a){0}", 6), std::string("", 0), -1, build(1, {0, 0, -1, -1})},
	{std::string("(\077-s)(\077:(\077:^).)", 15), std::string("\012", 1), -1, std::nullopt},
	{std::string("(\077s)(\077:(\077:^).)", 14), std::string("\012", 1), -1, build(1, {0, 1})},
	{std::string("(\077:(\077:^).)", 10), std::string("\012", 1), -1, std::nullopt},
	{std::string("\134b", 2), std::string("x", 1), -1, build(2, {0, 0, 1, 1})},
	{std::string("\134b", 2), std::string("xx", 2), -1, build(2, {0, 0, 2, 2})},
	{std::string("\134b", 2), std::string("x y", 3), -1, build(4, {0, 0, 1, 1, 2, 2, 3, 3})},
	{std::string("\134b", 2), std::string("xx yy", 5), -1, build(4, {0, 0, 2, 2, 3, 3, 5, 5})},
	{std::string("\134B", 2), std::string("x", 1), -1, std::nullopt},
	{std::string("\134B", 2), std::string("xx", 2), -1, build(1, {1, 1})},
	{std::string("\134B", 2), std::string("x y", 3), -1, std::nullopt},
	{std::string("\134B", 2), std::string("xx yy", 5), -1, build(2, {1, 1, 4, 4})},
	{std::string("(|a)*", 5), std::string("aa", 2), -1, build(3, {0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2})},
	{std::string("0A|0[aA]", 8), std::string("0a", 2), -1, build(1, {0, 2})},
	{std::string("0[aA]|0A", 8), std::string("0a", 2), -1, build(1, {0, 2})},

	// RE2 tests
	{std::string("[^\134S\134s]", 7), std::string("abcd", 4), -1, std::nullopt},
	{std::string("[^\134S[:space:]]", 14), std::string("abcd", 4), -1, std::nullopt},
	{std::string("[^\134D\134d]", 7), std::string("abcd", 4), -1, std::nullopt},
	{std::string("[^\134D[:digit:]]", 14), std::string("abcd", 4), -1, std::nullopt},
	{std::string("(\077i)\134W", 6), std::string("x", 1), -1, std::nullopt},
	{std::string("(\077i)\134W", 6), std::string("k", 1), -1, std::nullopt},
	{std::string("(\077i)\134W", 6), std::string("s", 1), -1, std::nullopt},

	// can backslash-escape any punctuation
	{std::string("\134!\134\042\134#\134$\134%\134&\134'\134(\134)\134*\134+\134,\134-\134.\134/\134:\134;\134<\134=\134>\134\077\134@\134[\134\134\134]\134^\134_\134{\134|\134}\134~", 62),
		std::string("!\042#$%&'()*+,-./:;<=>\077@[\134]^_{|}~", 31), -1, build(1, {0, 31})},
	{std::string("[\134!\134\042\134#\134$\134%\134&\134'\134(\134)\134*\134+\134,\134-\134.\134/\134:\134;\134<\134=\134>\134\077\134@\134[\134\134\134]\134^\134_\134{\134|\134}\134~]+", 65),
		std::string("!\042#$%&'()*+,-./:;<=>\077@[\134]^_{|}~", 31), -1, build(1, {0, 31})},
	{std::string("\134`", 2), std::string("`", 1), -1, build(1, {0, 1})},
	{std::string("[\134`]+", 5), std::string("`", 1), -1, build(1, {0, 1})},

	{std::string("\357\277\275", 3), std::string("\377", 1), -1, build(1, {0, 1})},
	{std::string("\357\277\275", 3), std::string("hello\377world", 11), -1, build(1, {5, 6})},
	{std::string(".*", 2), std::string("hello\377world", 11), -1, build(1, {0, 11})},
	{std::string("\134x{fffd}", 8), std::string("\302\000", 2), -1, build(1, {0, 1})},
	{std::string("[\357\277\275]", 5), std::string("\377", 1), -1, build(1, {0, 1})},
	{std::string("[\134x{fffd}]", 10), std::string("\302\000", 2), -1, build(1, {0, 1})},

	// long set of matches (longer than startSize)
	{
		std::string(".", 1),
		std::string("qwertyuiopasdfghjklzxcvbnm1234567890", 36),
		-1,
		build(36, {0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10,
			10, 11, 11, 12, 12, 13, 13, 14, 14, 15, 15, 16, 16, 17, 17, 18, 18, 19, 19, 20,
			20, 21, 21, 22, 22, 23, 23, 24, 24, 25, 25, 26, 26, 27, 27, 28, 28, 29, 29, 30,
			30, 31, 31, 32, 32, 33, 33, 34, 34, 35, 35, 36}),
	},
    };
    return tests;
}

std::string str(std::span<const byte> b) { return std::string(asStringView(b)); }
slice<byte> bytesOf(const std::string& s) { return toBytes(s); }
bool hasMatches(const FindTest& t) { return t.matches && !t.matches->empty(); }

TEST(GoRegexp, Find) {
    for (const auto& test : findTests()) {
        auto re = regexp::MustCompile(test.pat);
        EXPECT_EQ(re->String(), test.pat);
        auto result = re->Find(bytesOf(test.text));
        if (test.max == 0) continue;
        if (!test.matches && result.isNil()) continue;
        if (!test.matches && !result.isNil()) {
            ADD_FAILURE() << "got match " << str(result) << ", want none: " << test;
        } else if (test.matches && result.isNil()) {
            ADD_FAILURE() << "got no match, want one: " << test;
        } else {
            auto& m = (*test.matches)[0];
            EXPECT_EQ(result.len(), result.cap()) << test;
            EXPECT_EQ(str(result), test.text.substr(std::size_t(m[0]), std::size_t(m[1] - m[0]))) << test;
        }
    }
}

TEST(GoRegexp, FindString) {
    for (const auto& test : findTests()) {
        auto result = regexp::MustCompile(test.pat)->FindString(string(test.text));
        if (test.max == 0) continue;
        if (!hasMatches(test)) {
            EXPECT_EQ(result, "") << test;
        } else {
            auto& m = (*test.matches)[0];
            EXPECT_EQ(result, test.text.substr(std::size_t(m[0]), std::size_t(m[1] - m[0]))) << test;
        }
    }
}

void testFindIndex(const FindTest& test, const std::vector<int>& result) {
    if (test.max == 0) return;
    if (!test.matches) {
        EXPECT_TRUE(result.empty()) << "got match, want none: " << test;
    } else if (result.empty()) {
        ADD_FAILURE() << "got no match, want one: " << test;
    } else {
        auto& want = (*test.matches)[0];
        EXPECT_EQ(result[0], want[0]) << test;
        EXPECT_EQ(result[1], want[1]) << test;
    }
}

TEST(GoRegexp, FindIndex) {
    for (const auto& test : findTests()) testFindIndex(test, regexp::MustCompile(test.pat)->FindIndex(bytesOf(test.text)));
}

TEST(GoRegexp, FindStringIndex) {
    for (const auto& test : findTests()) testFindIndex(test, regexp::MustCompile(test.pat)->FindStringIndex(string(test.text)));
}

TEST(GoRegexp, FindReaderIndex) {
    for (const auto& test : findTests()) {
        auto r = strings::NewReader(string(test.text));
        testFindIndex(test, regexp::MustCompile(test.pat)->FindReaderIndex(*r));
    }
}

TEST(GoRegexp, FindAll) {
    for (const auto& test : findTests()) {
        auto result = regexp::MustCompile(test.pat)->FindAll(bytesOf(test.text), test.max);
        if (!test.matches) {
            EXPECT_TRUE(result.empty()) << "got match, want none: " << test;
            continue;
        }
        ASSERT_EQ(result.size(), test.matches->size()) << test;
        for (std::size_t k = 0; k < result.size(); k++) {
            auto& e = (*test.matches)[k];
            EXPECT_EQ(result[k].len(), result[k].cap()) << "match " << k << ": " << test;
            EXPECT_EQ(str(result[k]), test.text.substr(std::size_t(e[0]), std::size_t(e[1] - e[0]))) << test;
        }
    }
}

TEST(GoRegexp, FindAllString) {
    for (const auto& test : findTests()) {
        auto result = regexp::MustCompile(test.pat)->FindAllString(string(test.text), test.max);
        if (!test.matches) {
            EXPECT_TRUE(result.empty()) << "got match, want none: " << test;
            continue;
        }
        ASSERT_EQ(result.size(), test.matches->size()) << test;
        for (std::size_t k = 0; k < result.size(); k++) {
            auto& e = (*test.matches)[k];
            EXPECT_EQ(result[k], test.text.substr(std::size_t(e[0]), std::size_t(e[1] - e[0]))) << test;
        }
    }
}

void testFindAllIndex(const FindTest& test, const std::vector<std::vector<int>>& result) {
    if (!test.matches) {
        EXPECT_TRUE(result.empty()) << "got match, want none: " << test;
        return;
    }
    ASSERT_EQ(result.size(), test.matches->size()) << test;
    for (std::size_t k = 0; k < result.size(); k++) {
        auto& e = (*test.matches)[k];
        EXPECT_EQ(result[k][0], e[0]) << "match " << k << ": " << test;
        EXPECT_EQ(result[k][1], e[1]) << "match " << k << ": " << test;
    }
}

TEST(GoRegexp, FindAllIndex) {
    for (const auto& test : findTests()) {
        testFindAllIndex(test, regexp::MustCompile(test.pat)->FindAllIndex(bytesOf(test.text), test.max));
    }
}

TEST(GoRegexp, FindAllStringIndex) {
    for (const auto& test : findTests()) {
        testFindAllIndex(test, regexp::MustCompile(test.pat)->FindAllStringIndex(string(test.text), test.max));
    }
}

void testSubmatchBytes(const FindTest& test, std::size_t n, const std::vector<int>& submatches,
                       const std::vector<slice<byte>>& result) {
    ASSERT_EQ(submatches.size(), result.size() * 2) << "match " << n << ": " << test;
    for (std::size_t k = 0; k < submatches.size(); k += 2) {
        if (submatches[k] == -1) {
            EXPECT_TRUE(result[k / 2].isNil()) << "match " << n << ": want nil: " << test;
            continue;
        }
        const auto& got = result[k / 2];
        EXPECT_EQ(got.len(), got.cap()) << "match " << n << ": " << test;
        EXPECT_EQ(str(got), test.text.substr(std::size_t(submatches[k]), std::size_t(submatches[k + 1] - submatches[k])))
            << "match " << n << ": " << test;
    }
}

TEST(GoRegexp, FindSubmatch) {
    for (const auto& test : findTests()) {
        auto result = regexp::MustCompile(test.pat)->FindSubmatch(bytesOf(test.text));
        if (test.max == 0) continue;
        if (!test.matches) {
            EXPECT_TRUE(result.empty()) << "got match, want none: " << test;
        } else if (result.empty()) {
            ADD_FAILURE() << "got no match, want one: " << test;
        } else {
            testSubmatchBytes(test, 0, (*test.matches)[0], result);
        }
    }
}

void testSubmatchString(const FindTest& test, std::size_t n, const std::vector<int>& submatches,
                        const std::vector<string>& result) {
    ASSERT_EQ(submatches.size(), result.size() * 2) << "match " << n << ": " << test;
    for (std::size_t k = 0; k < submatches.size(); k += 2) {
        if (submatches[k] == -1) {
            EXPECT_EQ(result[k / 2], "") << "match " << n << ": want empty string: " << test;
            continue;
        }
        EXPECT_EQ(result[k / 2], test.text.substr(std::size_t(submatches[k]), std::size_t(submatches[k + 1] - submatches[k])))
            << "match " << n << ": " << test;
    }
}

TEST(GoRegexp, FindStringSubmatch) {
    for (const auto& test : findTests()) {
        auto result = regexp::MustCompile(test.pat)->FindStringSubmatch(string(test.text));
        if (test.max == 0) continue;
        if (!test.matches) {
            EXPECT_TRUE(result.empty()) << "got match, want none: " << test;
        } else if (result.empty()) {
            ADD_FAILURE() << "got no match, want one: " << test;
        } else {
            testSubmatchString(test, 0, (*test.matches)[0], result);
        }
    }
}

void testFindSubmatchIndex(const FindTest& test, const std::vector<int>& result) {
    if (test.max == 0) return;
    if (!test.matches) {
        EXPECT_TRUE(result.empty()) << "got match, want none: " << test;
    } else if (result.empty()) {
        ADD_FAILURE() << "got no match, want one: " << test;
    } else {
        EXPECT_EQ(result, (*test.matches)[0]) << test;
    }
}

TEST(GoRegexp, FindSubmatchIndex) {
    for (const auto& test : findTests()) {
        testFindSubmatchIndex(test, regexp::MustCompile(test.pat)->FindSubmatchIndex(bytesOf(test.text)));
    }
}

TEST(GoRegexp, FindStringSubmatchIndex) {
    for (const auto& test : findTests()) {
        testFindSubmatchIndex(test, regexp::MustCompile(test.pat)->FindStringSubmatchIndex(string(test.text)));
    }
}

TEST(GoRegexp, FindReaderSubmatchIndex) {
    for (const auto& test : findTests()) {
        auto r = strings::NewReader(string(test.text));
        testFindSubmatchIndex(test, regexp::MustCompile(test.pat)->FindReaderSubmatchIndex(*r));
    }
}

TEST(GoRegexp, FindAllSubmatch) {
    for (const auto& test : findTests()) {
        auto result = regexp::MustCompile(test.pat)->FindAllSubmatch(bytesOf(test.text), test.max);
        if (!test.matches) {
            EXPECT_TRUE(result.empty()) << "got match, want none: " << test;
            continue;
        }
        ASSERT_EQ(result.size(), test.matches->size()) << test;
        for (std::size_t k = 0; k < result.size(); k++) testSubmatchBytes(test, k, (*test.matches)[k], result[k]);
    }
}

TEST(GoRegexp, FindAllStringSubmatch) {
    for (const auto& test : findTests()) {
        auto result = regexp::MustCompile(test.pat)->FindAllStringSubmatch(string(test.text), test.max);
        if (!test.matches) {
            EXPECT_TRUE(result.empty()) << "got match, want none: " << test;
            continue;
        }
        ASSERT_EQ(result.size(), test.matches->size()) << test;
        for (std::size_t k = 0; k < result.size(); k++) testSubmatchString(test, k, (*test.matches)[k], result[k]);
    }
}

void testFindAllSubmatchIndex(const FindTest& test, const std::vector<std::vector<int>>& result) {
    if (!test.matches) {
        EXPECT_TRUE(result.empty()) << "got match, want none: " << test;
        return;
    }
    ASSERT_EQ(result.size(), test.matches->size()) << test;
    for (std::size_t k = 0; k < result.size(); k++) EXPECT_EQ(result[k], (*test.matches)[k]) << "match " << k << ": " << test;
}

TEST(GoRegexp, FindAllSubmatchIndex) {
    for (const auto& test : findTests()) {
        testFindAllSubmatchIndex(test, regexp::MustCompile(test.pat)->FindAllSubmatchIndex(bytesOf(test.text), test.max));
    }
}

TEST(GoRegexp, FindAllStringSubmatchIndex) {
    for (const auto& test : findTests()) {
        testFindAllSubmatchIndex(test, regexp::MustCompile(test.pat)->FindAllStringSubmatchIndex(string(test.text), test.max));
    }
}

// ---------------------------------------------------------------------------
// all_test.go

std::string repeat(std::string_view s, int n) {
    std::string r;
    for (int i = 0; i < n; i++) r += s;
    return r;
}

ptr<regexp::Regexp> compileTest(const std::string& expr, const std::string& error) {
    auto [re, err] = regexp::Compile(expr);
    if (error.empty() && err) ADD_FAILURE() << "compiling `" << expr << "`; unexpected error: " << err;
    if (!error.empty() && !err) {
        ADD_FAILURE() << "compiling `" << expr << "`; missing error";
    } else if (!error.empty() && err.Error().view().find(error) == std::string_view::npos) {
        ADD_FAILURE() << "compiling `" << expr << "`; wrong error: " << err << "; want " << error;
    }
    return re;
}

TEST(GoRegexp, GoodCompile) {
    for (std::string re : {R"()", R"(.)", R"(^.$)", R"(a)", R"(a*)", R"(a+)", R"(a?)", R"(a|b)", R"(a*|b*)",
                           R"((a*|b)(c*|d))", R"([a-z])", R"([a-abc-c\-\]\[])", R"([a-z]+)", R"([abc])", R"([^1234])",
                           R"([^\n])", R"(\!\\)"}) {
        compileTest(re, "");
    }
}

TEST(GoRegexp, BadCompile) {
    std::vector<std::pair<std::string, std::string>> badRe = {
        {R"(*)", "missing argument to repetition operator: `*`"},
        {R"(+)", "missing argument to repetition operator: `+`"},
        {R"(?)", "missing argument to repetition operator: `?`"},
        {R"((abc)", "missing closing ): `(abc`"},
        {R"(abc))", "unexpected ): `abc)`"},
        {R"(x[a-z)", "missing closing ]: `[a-z`"},
        {R"([z-a])", "invalid character class range: `z-a`"},
        {R"(abc\)", "trailing backslash at end of expression"},
        {R"(a**)", "invalid nested repetition operator: `**`"},
        {R"(a*+)", "invalid nested repetition operator: `*+`"},
        {R"(\x)", "invalid escape sequence: `\\x`"},
        {repeat(R"(\pL)", 27000), "expression too large"},
    };
    for (const auto& [re, err] : badRe) compileTest(re, err);
}

TEST(GoRegexp, Match) {
    for (const auto& test : findTests()) {
        if (test.max == 0) continue;
        auto re = compileTest(test.pat, "");
        if (!re) continue;
        EXPECT_EQ(re->MatchString(string(test.text)), hasMatches(test)) << "MatchString failure on " << test;
        // now try bytes
        EXPECT_EQ(re->Match(bytesOf(test.text)), hasMatches(test)) << "Match failure on " << test;
    }
}

TEST(GoRegexp, MatchFunction) {
    for (const auto& test : findTests()) {
        auto [m, err] = regexp::MatchString(test.pat, string(test.text));
        if (!err) continue;
        EXPECT_EQ(m, hasMatches(test)) << "Match failure on " << test;
    }
}

struct ReplaceTest {
    std::string pattern, replacement, input, output;
};

const std::vector<ReplaceTest> replaceTests = {
	// Test empty input and/or replacement, with pattern that matches the empty string.
	{std::string("", 0), std::string("", 0), std::string("", 0), std::string("", 0)},
	{std::string("", 0), std::string("x", 1), std::string("", 0), std::string("x", 1)},
	{std::string("", 0), std::string("", 0), std::string("abc", 3), std::string("abc", 3)},
	{std::string("", 0), std::string("x", 1), std::string("abc", 3), std::string("xaxbxcx", 7)},

	// Test empty input and/or replacement, with pattern that does not match the empty string.
	{std::string("b", 1), std::string("", 0), std::string("", 0), std::string("", 0)},
	{std::string("b", 1), std::string("x", 1), std::string("", 0), std::string("", 0)},
	{std::string("b", 1), std::string("", 0), std::string("abc", 3), std::string("ac", 2)},
	{std::string("b", 1), std::string("x", 1), std::string("abc", 3), std::string("axc", 3)},
	{std::string("y", 1), std::string("", 0), std::string("", 0), std::string("", 0)},
	{std::string("y", 1), std::string("x", 1), std::string("", 0), std::string("", 0)},
	{std::string("y", 1), std::string("", 0), std::string("abc", 3), std::string("abc", 3)},
	{std::string("y", 1), std::string("x", 1), std::string("abc", 3), std::string("abc", 3)},

	// Multibyte characters -- verify that we don't try to match in the middle
	// of a character.
	{std::string("[a-c]*", 6), std::string("x", 1), std::string("\346\227\245", 3), std::string("x\346\227\245x", 5)},
	{std::string("[^\346\227\245]", 6), std::string("x", 1), std::string("abc\346\227\245def", 9), std::string("xxx\346\227\245xxx", 9)},

	// Start and end of a string.
	{std::string("^[a-c]*", 7), std::string("x", 1), std::string("abcdabc", 7), std::string("xdabc", 5)},
	{std::string("[a-c]*$", 7), std::string("x", 1), std::string("abcdabc", 7), std::string("abcdx", 5)},
	{std::string("^[a-c]*$", 8), std::string("x", 1), std::string("abcdabc", 7), std::string("abcdabc", 7)},
	{std::string("^[a-c]*", 7), std::string("x", 1), std::string("abc", 3), std::string("x", 1)},
	{std::string("[a-c]*$", 7), std::string("x", 1), std::string("abc", 3), std::string("x", 1)},
	{std::string("^[a-c]*$", 8), std::string("x", 1), std::string("abc", 3), std::string("x", 1)},
	{std::string("^[a-c]*", 7), std::string("x", 1), std::string("dabce", 5), std::string("xdabce", 6)},
	{std::string("[a-c]*$", 7), std::string("x", 1), std::string("dabce", 5), std::string("dabcex", 6)},
	{std::string("^[a-c]*$", 8), std::string("x", 1), std::string("dabce", 5), std::string("dabce", 5)},
	{std::string("^[a-c]*", 7), std::string("x", 1), std::string("", 0), std::string("x", 1)},
	{std::string("[a-c]*$", 7), std::string("x", 1), std::string("", 0), std::string("x", 1)},
	{std::string("^[a-c]*$", 8), std::string("x", 1), std::string("", 0), std::string("x", 1)},

	{std::string("^[a-c]+", 7), std::string("x", 1), std::string("abcdabc", 7), std::string("xdabc", 5)},
	{std::string("[a-c]+$", 7), std::string("x", 1), std::string("abcdabc", 7), std::string("abcdx", 5)},
	{std::string("^[a-c]+$", 8), std::string("x", 1), std::string("abcdabc", 7), std::string("abcdabc", 7)},
	{std::string("^[a-c]+", 7), std::string("x", 1), std::string("abc", 3), std::string("x", 1)},
	{std::string("[a-c]+$", 7), std::string("x", 1), std::string("abc", 3), std::string("x", 1)},
	{std::string("^[a-c]+$", 8), std::string("x", 1), std::string("abc", 3), std::string("x", 1)},
	{std::string("^[a-c]+", 7), std::string("x", 1), std::string("dabce", 5), std::string("dabce", 5)},
	{std::string("[a-c]+$", 7), std::string("x", 1), std::string("dabce", 5), std::string("dabce", 5)},
	{std::string("^[a-c]+$", 8), std::string("x", 1), std::string("dabce", 5), std::string("dabce", 5)},
	{std::string("^[a-c]+", 7), std::string("x", 1), std::string("", 0), std::string("", 0)},
	{std::string("[a-c]+$", 7), std::string("x", 1), std::string("", 0), std::string("", 0)},
	{std::string("^[a-c]+$", 8), std::string("x", 1), std::string("", 0), std::string("", 0)},

	// Other cases.
	{std::string("abc", 3), std::string("def", 3), std::string("abcdefg", 7), std::string("defdefg", 7)},
	{std::string("bc", 2), std::string("BC", 2), std::string("abcbcdcdedef", 12), std::string("aBCBCdcdedef", 12)},
	{std::string("abc", 3), std::string("", 0), std::string("abcdabc", 7), std::string("d", 1)},
	{std::string("x", 1), std::string("xXx", 3), std::string("xxxXxxx", 7), std::string("xXxxXxxXxXxXxxXxxXx", 19)},
	{std::string("abc", 3), std::string("d", 1), std::string("", 0), std::string("", 0)},
	{std::string("abc", 3), std::string("d", 1), std::string("abc", 3), std::string("d", 1)},
	{std::string(".+", 2), std::string("x", 1), std::string("abc", 3), std::string("x", 1)},
	{std::string("[a-c]*", 6), std::string("x", 1), std::string("def", 3), std::string("xdxexfx", 7)},
	{std::string("[a-c]+", 6), std::string("x", 1), std::string("abcbcdcdedef", 12), std::string("xdxdedef", 8)},
	{std::string("[a-c]*", 6), std::string("x", 1), std::string("abcbcdcdedef", 12), std::string("xdxdxexdxexfx", 13)},

	// Substitutions
	{std::string("a+", 2), std::string("($0)", 4), std::string("banana", 6), std::string("b(a)n(a)n(a)", 12)},
	{std::string("a+", 2), std::string("(${0})", 6), std::string("banana", 6), std::string("b(a)n(a)n(a)", 12)},
	{std::string("a+", 2), std::string("(${0})$0", 8), std::string("banana", 6), std::string("b(a)an(a)an(a)a", 15)},
	{std::string("a+", 2), std::string("(${0})$0", 8), std::string("banana", 6), std::string("b(a)an(a)an(a)a", 15)},
	{std::string("hello, (.+)", 11), std::string("goodbye, ${1}", 13), std::string("hello, world", 12), std::string("goodbye, world", 14)},
	{std::string("hello, (.+)", 11), std::string("goodbye, $1x", 12), std::string("hello, world", 12), std::string("goodbye, ", 9)},
	{std::string("hello, (.+)", 11), std::string("goodbye, ${1}x", 14), std::string("hello, world", 12), std::string("goodbye, worldx", 15)},
	{std::string("hello, (.+)", 11), std::string("<$0><$1><$2><$3>", 16), std::string("hello, world", 12), std::string("<hello, world><world><><>", 25)},
	{std::string("hello, (\077P<noun>.+)", 19), std::string("goodbye, $noun!", 15), std::string("hello, world", 12), std::string("goodbye, world!", 15)},
	{std::string("hello, (\077P<noun>.+)", 19), std::string("goodbye, ${noun}", 16), std::string("hello, world", 12), std::string("goodbye, world", 14)},
	{std::string("(\077P<x>hi)|(\077P<x>bye)", 20), std::string("$x$x$x", 6), std::string("hi", 2), std::string("hihihi", 6)},
	{std::string("(\077P<x>hi)|(\077P<x>bye)", 20), std::string("$x$x$x", 6), std::string("bye", 3), std::string("byebyebye", 9)},
	{std::string("(\077P<x>hi)|(\077P<x>bye)", 20), std::string("$xyz", 4), std::string("hi", 2), std::string("", 0)},
	{std::string("(\077P<x>hi)|(\077P<x>bye)", 20), std::string("${x}yz", 6), std::string("hi", 2), std::string("hiyz", 4)},
	{std::string("(\077P<x>hi)|(\077P<x>bye)", 20), std::string("hello $$x", 9), std::string("hi", 2), std::string("hello $x", 8)},
	{std::string("a+", 2), std::string("${oops", 6), std::string("aaa", 3), std::string("${oops", 6)},
	{std::string("a+", 2), std::string("$$", 2), std::string("aaa", 3), std::string("$", 1)},
	{std::string("a+", 2), std::string("$", 1), std::string("aaa", 3), std::string("$", 1)},

	// Substitution when subexpression isn't found
	{std::string("(x)\077", 4), std::string("$1", 2), std::string("123", 3), std::string("123", 3)},
	{std::string("abc", 3), std::string("$1", 2), std::string("123", 3), std::string("123", 3)},

	// Substitutions involving a (x){0}
	{std::string("(a)(b){0}(c)", 12), std::string(".$1|$3.", 7), std::string("xacxacx", 7), std::string("x.a|c.x.a|c.x", 13)},
	{std::string("(a)(((b))){0}c", 14), std::string(".$1.", 4), std::string("xacxacx", 7), std::string("x.a.x.a.x", 9)},
	{std::string("((a(b){0}){3}){5}(h)", 20), std::string("y caramb$2", 10), std::string("say aaaaaaaaaaaaaaaah", 21), std::string("say ay caramba", 14)},
	{std::string("((a(b){0}){3}){5}h", 18), std::string("y caramb$2", 10), std::string("say aaaaaaaaaaaaaaaah", 21), std::string("say ay caramba", 14)},
};

const std::vector<ReplaceTest> replaceLiteralTests = {
	// Substitutions
	{std::string("a+", 2), std::string("($0)", 4), std::string("banana", 6), std::string("b($0)n($0)n($0)", 15)},
	{std::string("a+", 2), std::string("(${0})", 6), std::string("banana", 6), std::string("b(${0})n(${0})n(${0})", 21)},
	{std::string("a+", 2), std::string("(${0})$0", 8), std::string("banana", 6), std::string("b(${0})$0n(${0})$0n(${0})$0", 27)},
	{std::string("a+", 2), std::string("(${0})$0", 8), std::string("banana", 6), std::string("b(${0})$0n(${0})$0n(${0})$0", 27)},
	{std::string("hello, (.+)", 11), std::string("goodbye, ${1}", 13), std::string("hello, world", 12), std::string("goodbye, ${1}", 13)},
	{std::string("hello, (\077P<noun>.+)", 19), std::string("goodbye, $noun!", 15), std::string("hello, world", 12), std::string("goodbye, $noun!", 15)},
	{std::string("hello, (\077P<noun>.+)", 19), std::string("goodbye, ${noun}", 16), std::string("hello, world", 12), std::string("goodbye, ${noun}", 16)},
	{std::string("(\077P<x>hi)|(\077P<x>bye)", 20), std::string("$x$x$x", 6), std::string("hi", 2), std::string("$x$x$x", 6)},
	{std::string("(\077P<x>hi)|(\077P<x>bye)", 20), std::string("$x$x$x", 6), std::string("bye", 3), std::string("$x$x$x", 6)},
	{std::string("(\077P<x>hi)|(\077P<x>bye)", 20), std::string("$xyz", 4), std::string("hi", 2), std::string("$xyz", 4)},
	{std::string("(\077P<x>hi)|(\077P<x>bye)", 20), std::string("${x}yz", 6), std::string("hi", 2), std::string("${x}yz", 6)},
	{std::string("(\077P<x>hi)|(\077P<x>bye)", 20), std::string("hello $$x", 9), std::string("hi", 2), std::string("hello $$x", 9)},
	{std::string("a+", 2), std::string("${oops", 6), std::string("aaa", 3), std::string("${oops", 6)},
	{std::string("a+", 2), std::string("$$", 2), std::string("aaa", 3), std::string("$$", 2)},
	{std::string("a+", 2), std::string("$", 1), std::string("aaa", 3), std::string("$", 1)},
};

TEST(GoRegexp, ReplaceAll) {
    for (const auto& tc : replaceTests) {
        auto [re, err] = regexp::Compile(tc.pattern);
        if (err) {
            ADD_FAILURE() << "Unexpected error compiling " << tc.pattern << ": " << err;
            continue;
        }
        EXPECT_EQ(re->ReplaceAllString(string(tc.input), tc.replacement), tc.output)
            << strconv::Quote(tc.pattern) << ".ReplaceAllString(" << strconv::Quote(tc.input) << ","
            << strconv::Quote(tc.replacement) << ")";
        // now try bytes
        EXPECT_EQ(str(re->ReplaceAll(bytesOf(tc.input), as_bytes(tc.replacement))), tc.output)
            << strconv::Quote(tc.pattern) << ".ReplaceAll";
    }
}

TEST(GoRegexp, ReplaceAllLiteral) {
    // Run ReplaceAll tests that do not have $ expansions.
    auto check = [](const ReplaceTest& tc) {
        auto [re, err] = regexp::Compile(tc.pattern);
        if (err) {
            ADD_FAILURE() << "Unexpected error compiling " << tc.pattern << ": " << err;
            return;
        }
        EXPECT_EQ(re->ReplaceAllLiteralString(string(tc.input), tc.replacement), tc.output)
            << strconv::Quote(tc.pattern) << ".ReplaceAllLiteralString(" << strconv::Quote(tc.input) << ","
            << strconv::Quote(tc.replacement) << ")";
        EXPECT_EQ(str(re->ReplaceAllLiteral(bytesOf(tc.input), as_bytes(tc.replacement))), tc.output)
            << strconv::Quote(tc.pattern) << ".ReplaceAllLiteral";
    };
    for (const auto& tc : replaceTests) {
        if (tc.replacement.find('$') != std::string::npos) continue;
        check(tc);
    }
    // Run literal-specific tests.
    for (const auto& tc : replaceLiteralTests) check(tc);
}

TEST(GoRegexp, ReplaceAllFunc) {
    struct ReplaceFuncTest {
        std::string pattern;
        std::function<string(const string&)> replacement;
        std::string input, output;
    };
    auto xy = [](const string& s) { return "x" + s + "y"; };
    std::vector<ReplaceFuncTest> replaceFuncTests = {
        {"[a-c]", xy, "defabcdef", "defxayxbyxcydef"},
        {"[a-c]+", xy, "defabcdef", "defxabcydef"},
        {"[a-c]*", xy, "defabcdef", "xydxyexyfxabcydxyexyfxy"},
    };
    for (const auto& tc : replaceFuncTests) {
        auto re = regexp::MustCompile(tc.pattern);
        EXPECT_EQ(re->ReplaceAllStringFunc(string(tc.input), tc.replacement), tc.output) << tc.pattern;
        // now try bytes
        auto out = re->ReplaceAllFunc(bytesOf(tc.input), [&](const slice<byte>& s) {
            return toBytes(tc.replacement(String(s)).view());
        });
        EXPECT_EQ(str(out), tc.output) << tc.pattern;
    }
}

struct MetaTest {
    std::string pattern, output, literal;
    bool isLiteral;
};

const std::vector<MetaTest> metaTests = {
    {R"()", R"()", R"()", true},
    {R"(foo)", R"(foo)", R"(foo)", true},
    {R"(日本語+)", R"(日本語\+)", R"(日本語)", false},
    {R"(foo\.\$)", R"(foo\\\.\\\$)", R"(foo.$)", true},  // has meta but no operator
    {R"(foo.\$)", R"(foo\.\\\$)", R"(foo)", false},      // has escaped operators and real operators
    {R"(!@#$%^&*()_+-=[{]}\|,<.>/?~)", R"(!@#\$%\^&\*\(\)_\+-=\[\{\]\}\\\|,<\.>/\?~)", R"(!@#)", false},
};

TEST(GoRegexp, QuoteMeta) {
    for (const auto& tc : metaTests) {
        // Verify that QuoteMeta returns the expected string.
        string quoted = regexp::QuoteMeta(string(tc.pattern));
        if (quoted != tc.output) {
            ADD_FAILURE() << "QuoteMeta(`" << tc.pattern << "`) = `" << quoted << "`; want `" << tc.output << "`";
            continue;
        }
        // Verify that the quoted string is in fact treated as expected by
        // Compile -- i.e. that it matches the original, unquoted string.
        if (!tc.pattern.empty()) {
            auto [re, err] = regexp::Compile(quoted);
            if (err) {
                ADD_FAILURE() << "Unexpected error compiling QuoteMeta(`" << tc.pattern << "`): " << err;
                continue;
            }
            std::string src = "abc" + tc.pattern + "def";
            EXPECT_EQ(re->ReplaceAllString(string(src), "xyz"), "abcxyzdef") << tc.pattern;
        }
    }
}

TEST(GoRegexp, LiteralPrefix) {
    std::vector<MetaTest> tests = metaTests;
    // See golang.org/issue/11175. output is unused.
    std::vector<MetaTest> literalPrefixTests = {
        {R"(^0^0$)", "", R"(0)", false},  {R"(^0^)", "", "", false},  {R"(^0$)", "", R"(0)", true},
        {R"($0^)", "", "", false},        {R"($0$)", "", "", false},  {R"(^^0$$)", "", "", false},
        {R"(^$^$)", "", "", false},       {R"($$0^^)", "", "", false}, {R"(a\x{fffd}b)", "", R"(a)", false},
        {R"(\x{fffd}b)", "", "", false},  {"�", "", "", false},
    };
    tests.insert(tests.end(), literalPrefixTests.begin(), literalPrefixTests.end());
    for (const auto& tc : tests) {
        // Literal method needs to scan the pattern.
        auto re = regexp::MustCompile(tc.pattern);
        auto [str, complete] = re->LiteralPrefix();
        EXPECT_EQ(complete, tc.isLiteral) << "LiteralPrefix(`" << tc.pattern << "`)";
        EXPECT_EQ(str, tc.literal) << "LiteralPrefix(`" << tc.pattern << "`)";
    }
}

TEST(GoRegexp, Subexp) {
    struct subexpIndex {
        std::string name;
        int index;
    };
    struct subexpCase {
        std::string input;
        int num;
        std::vector<std::string> names;
        std::vector<subexpIndex> indices;
    };
    std::vector<subexpIndex> emptySubexpIndices = {{"", -1}, {"missing", -1}};
    std::vector<subexpCase> subexpCases = {
        {R"()", 0, {}, emptySubexpIndices},
        {R"(.*)", 0, {}, emptySubexpIndices},
        {R"(abba)", 0, {}, emptySubexpIndices},
        {R"(ab(b)a)", 1, {"", ""}, emptySubexpIndices},
        {R"(ab(.*)a)", 1, {"", ""}, emptySubexpIndices},
        {R"((.*)ab(.*)a)", 2, {"", "", ""}, emptySubexpIndices},
        {R"((.*)(ab)(.*)a)", 3, {"", "", "", ""}, emptySubexpIndices},
        {R"((.*)((a)b)(.*)a)", 4, {"", "", "", "", ""}, emptySubexpIndices},
        {R"((.*)(\(ab)(.*)a)", 3, {"", "", "", ""}, emptySubexpIndices},
        {R"((.*)(\(a\)b)(.*)a)", 3, {"", "", "", ""}, emptySubexpIndices},
        {R"((?P<foo>.*)(?P<bar>(a)b)(?P<foo>.*)a)", 4, {"", "foo", "bar", "", "foo"},
         {{"", -1}, {"missing", -1}, {"foo", 1}, {"bar", 2}}},
    };
    for (const auto& c : subexpCases) {
        auto re = regexp::MustCompile(c.input);
        int n = re->NumSubexp();
        if (n != c.num) {
            ADD_FAILURE() << c.input << ": NumSubexp = " << n << ", want " << c.num;
            continue;
        }
        const auto& names = re->SubexpNames();
        if (int(names.size()) != 1 + n) {
            ADD_FAILURE() << c.input << ": len(SubexpNames) = " << names.size() << ", want " << n;
            continue;
        }
        if (!c.names.empty()) {
            for (int i = 0; i < 1 + n; i++) EXPECT_EQ(names[std::size_t(i)], c.names[std::size_t(i)]) << c.input;
        }
        for (const auto& subexp : c.indices) EXPECT_EQ(re->SubexpIndex(subexp.name), subexp.index) << c.input;
    }
}

TEST(GoRegexp, Split) {
    struct test {
        std::string s, r;
        int n;
        std::vector<std::string> out;
    };
    std::vector<test> splitTests = {
	{std::string("foo:and:bar", 11), std::string(":", 1), -1, std::vector<std::string>{std::string("foo", 3), std::string("and", 3), std::string("bar", 3)}},
	{std::string("foo:and:bar", 11), std::string(":", 1), 1, std::vector<std::string>{std::string("foo:and:bar", 11)}},
	{std::string("foo:and:bar", 11), std::string(":", 1), 2, std::vector<std::string>{std::string("foo", 3), std::string("and:bar", 7)}},
	{std::string("foo:and:bar", 11), std::string("foo", 3), -1, std::vector<std::string>{std::string("", 0), std::string(":and:bar", 8)}},
	{std::string("foo:and:bar", 11), std::string("bar", 3), -1, std::vector<std::string>{std::string("foo:and:", 8), std::string("", 0)}},
	{std::string("foo:and:bar", 11), std::string("baz", 3), -1, std::vector<std::string>{std::string("foo:and:bar", 11)}},
	{std::string("baabaab", 7), std::string("a", 1), -1, std::vector<std::string>{std::string("b", 1), std::string("", 0), std::string("b", 1), std::string("", 0), std::string("b", 1)}},
	{std::string("baabaab", 7), std::string("a*", 2), -1, std::vector<std::string>{std::string("b", 1), std::string("b", 1), std::string("b", 1)}},
	{std::string("baabaab", 7), std::string("ba*", 3), -1, std::vector<std::string>{std::string("", 0), std::string("", 0), std::string("", 0), std::string("", 0)}},
	{std::string("foobar", 6), std::string("f*b*", 4), -1, std::vector<std::string>{std::string("", 0), std::string("o", 1), std::string("o", 1), std::string("a", 1), std::string("r", 1)}},
	{std::string("foobar", 6), std::string("f+.*b+", 6), -1, std::vector<std::string>{std::string("", 0), std::string("ar", 2)}},
	{std::string("foobooboar", 10), std::string("o{2}", 4), -1, std::vector<std::string>{std::string("f", 1), std::string("b", 1), std::string("boar", 4)}},
	{std::string("a,b,c,d,e,f", 11), std::string(",", 1), 3, std::vector<std::string>{std::string("a", 1), std::string("b", 1), std::string("c,d,e,f", 7)}},
	{std::string("a,b,c,d,e,f", 11), std::string(",", 1), 0, std::vector<std::string>{}},
	{std::string(",", 1), std::string(",", 1), -1, std::vector<std::string>{std::string("", 0), std::string("", 0)}},
	{std::string(",,,", 3), std::string(",", 1), -1, std::vector<std::string>{std::string("", 0), std::string("", 0), std::string("", 0), std::string("", 0)}},
	{std::string("", 0), std::string(",", 1), -1, std::vector<std::string>{std::string("", 0)}},
	{std::string("", 0), std::string(".*", 2), -1, std::vector<std::string>{std::string("", 0)}},
	{std::string("", 0), std::string(".+", 2), -1, std::vector<std::string>{std::string("", 0)}},
	{std::string("", 0), std::string("", 0), -1, std::vector<std::string>{}},
	{std::string("foobar", 6), std::string("", 0), -1, std::vector<std::string>{std::string("f", 1), std::string("o", 1), std::string("o", 1), std::string("b", 1), std::string("a", 1), std::string("r", 1)}},
	{std::string("abaabaccadaaae", 14), std::string("a*", 2), 5, std::vector<std::string>{std::string("", 0), std::string("b", 1), std::string("b", 1), std::string("c", 1), std::string("cadaaae", 7)}},
	{std::string(":x:y:z:", 7), std::string(":", 1), -1, std::vector<std::string>{std::string("", 0), std::string("x", 1), std::string("y", 1), std::string("z", 1), std::string("", 0)}},
};
    int i = 0;
    for (const auto& tc : splitTests) {
        auto [re, err] = regexp::Compile(tc.r);
        if (err) {
            ADD_FAILURE() << "#" << i << ": " << tc.r << ": compile error: " << err;
            continue;
        }
        auto split = re->Split(string(tc.s), tc.n);
        std::vector<std::string> got;
        for (const auto& s : split) got.push_back(s.str());
        EXPECT_EQ(got, tc.out) << "#" << i << ": " << strconv::Quote(tc.r);
        i++;
    }
}

// The following sequence of Match calls used to panic. See issue #12980.
TEST(GoRegexp, ParseAndCompile) {
    std::string expr = "a$";
    string s = "a\nb";
    struct test {
        syntax::Flags reFlags;
        bool expMatch;
    };
    int i = 0;
    for (auto tc : std::vector<test>{{syntax::Perl | syntax::OneLine, false}, {syntax::Perl & ~syntax::OneLine, true}}) {
        auto [parsed, err] = syntax::Parse(expr, tc.reFlags);
        ASSERT_FALSE(err) << i << ": parse: " << err;
        auto [re, cerr] = regexp::Compile(parsed->String());
        ASSERT_FALSE(cerr) << i << ": compile: " << cerr;
        EXPECT_EQ(re->MatchString(s), tc.expMatch) << i << ": " << re->String();
        i++;
    }
}

// Check that the same machine can be used with the standard matcher and
// then the backtracker when there are no captures.
TEST(GoRegexp, SwitchBacktrack) {
    auto re = regexp::MustCompile(R"(a|b)");
    auto longb = slice<byte>::make(256 * 1024 + 1);
    // The following sequence of Match calls used to panic. See issue #10319.
    re->Match(longb);           // triggers standard matcher
    re->Match(longb.Sub(0, 1));  // triggers backtracker
}

// go++: a Regexp is safe for concurrent use.
TEST(Regexp, Concurrent) {
    auto re = regexp::MustCompile(R"((\w+)@(\w+)\.com)");
    sync::WaitGroup wg;
    std::atomic<int> bad{0};
    for (int i = 0; i < 8; i++) {
        wg.Go([&] {
            for (int k = 0; k < 2000; k++) {
                auto m = re->FindStringSubmatch("mail gopher@golang.com now");
                if (m.size() != 3 || m[1] != "gopher" || m[2] != "golang") bad++;
            }
        });
    }
    wg.Wait();
    EXPECT_EQ(bad.load(), 0);
}

// go++: Expand with names and numbers, and the POSIX leftmost-longest
// semantics.
TEST(Regexp, ExpandAndPOSIX) {
    auto re = regexp::MustCompile(R"((?P<key>\w+)=(?P<value>\w+))");
    std::string src = "a=1 b=2";
    slice<byte> out;
    for (const auto& m : re->FindAllStringSubmatchIndex(string(src), -1)) {
        out = re->ExpandString(out, "$value:${key};", src, m);
    }
    EXPECT_EQ(str(out), "1:a;2:b;");
    EXPECT_EQ(regexp::MustCompile("a|ab")->FindString("ab"), "a");
    EXPECT_EQ(regexp::MustCompilePOSIX("a|ab")->FindString("ab"), "ab");
}

}  // namespace
