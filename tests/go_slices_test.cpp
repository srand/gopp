// Ported from Go 1.27.3's slices/slices_test.go, slices/sort_test.go and
// slices/iter_test.go, with Go's test names and tables. Left out: the
// benchmarks, the allocation counts, and the cases with zero-size elements
// of length math.MaxInt, which C++ can't make. Go's panics are death tests.
#include <gopp/cmp.hpp>
#include <gopp/slices.hpp>
#include <gopp/strings.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <functional>
#include <limits>
#include <random>
#include <ranges>
#include <string>
#include <vector>

using namespace gopp;

namespace {

using ints = slice<isize>;
using floats = slice<double>;
using strs_t = slice<string>;

const double nan_ = std::numeric_limits<double>::quiet_NaN();
const double inf = std::numeric_limits<double>::infinity();

ints emptyInts() { return ints::make(0); }

std::mt19937_64 rng(2);
isize rand_intn(isize n) { return std::uniform_int_distribution<isize>(0, n - 1)(rng); }

template <typename T>
bool equal(const T& v1, const T& v2) {
    return v1 == v2;
}

// equalNaN is like == except that all NaNs are equal.
template <typename T>
bool equalNaN(const T& v1, const T& v2) {
    auto isNaN = [](const T& f) { return f != f; };
    return v1 == v2 || (isNaN(v1) && isNaN(v2));
}

// offByOne returns true if integers v1 and v2 differ by 1.
bool offByOne(isize v1, isize v2) { return v1 == v2 + 1 || v1 == v2 - 1; }

std::string cmpIntStringStr(isize v1) { return std::string(1, static_cast<char>(v1 - 1 + 'a')); }

struct equalIntTest {
    ints s1, s2;
    bool want;
};

std::vector<equalIntTest> equalIntTests() {
    return {
        {{1}, {}, false},
        {emptyInts(), {}, true},
        {{1, 2, 3}, {1, 2, 3}, true},
        {{1, 2, 3}, {1, 2, 3, 4}, false},
    };
}

struct equalFloatTest {
    floats s1, s2;
    bool wantEqual, wantEqualNaN;
};

std::vector<equalFloatTest> equalFloatTests() {
    return {
        {{1, 2}, {1, 2}, true, true},
        {{1, 2, nan_}, {1, 2, nan_}, false, true},
    };
}

}  // namespace

TEST(GoSlices, Equal) {
    for (auto& test : equalIntTests()) EXPECT_EQ(slices::Equal(test.s1, test.s2), test.want);
    for (auto& test : equalFloatTests()) EXPECT_EQ(slices::Equal(test.s1, test.s2), test.wantEqual);
}

TEST(GoSlices, EqualFunc) {
    for (auto& test : equalIntTests()) EXPECT_EQ(slices::EqualFunc(test.s1, test.s2, equal<isize>), test.want);
    for (auto& test : equalFloatTests()) {
        EXPECT_EQ(slices::EqualFunc(test.s1, test.s2, equal<double>), test.wantEqual);
        EXPECT_EQ(slices::EqualFunc(test.s1, test.s2, equalNaN<double>), test.wantEqualNaN);
    }

    ints s1{1, 2, 3};
    ints s2{2, 3, 4};
    EXPECT_FALSE(slices::EqualFunc(s1, s1, offByOne));
    EXPECT_TRUE(slices::EqualFunc(s1, s2, offByOne));

    strs_t s3{"a", "b", "c"};
    strs_t s4{"A", "B", "C"};
    EXPECT_TRUE(slices::EqualFunc(s3, s4, [](const string& a, const string& b) { return strings::EqualFold(a, b); }));

    auto cmpIntString = [](isize v1, const string& v2) { return cmpIntStringStr(v1) == v2.view(); };
    EXPECT_TRUE(slices::EqualFunc(s1, s3, cmpIntString));
}

namespace {

struct compareIntTest {
    ints s1, s2;
    int want;
};

std::vector<compareIntTest> compareIntTests() {
    return {
        {{1}, {1}, 0},
        {{1}, emptyInts(), 1},
        {emptyInts(), {1}, -1},
        {emptyInts(), emptyInts(), 0},
        {{1, 2, 3}, {1, 2, 3}, 0},
        {{1, 2, 3}, {1, 2, 3, 4}, -1},
        {{1, 2, 3, 4}, {1, 2, 3}, +1},
        {{1, 2, 3}, {1, 4, 3}, -1},
        {{1, 4, 3}, {1, 2, 3}, +1},
        {{1, 4, 3}, {1, 2, 3, 8, 9}, +1},
    };
}

struct compareFloatTest {
    floats s1, s2;
    int want;
};

std::vector<compareFloatTest> compareFloatTests() {
    return {
        {floats::make(0), floats::make(0), 0},
        {{1}, {1}, 0},
        {{nan_}, {nan_}, 0},
        {{1, 2, nan_}, {1, 2, nan_}, 0},
        {{1, nan_, 3}, {1, nan_, 4}, -1},
        {{1, nan_, 3}, {1, 2, 4}, -1},
        {{1, nan_, 3}, {1, 2, nan_}, -1},
        {{1, 2, 3}, {1, 2, nan_}, +1},
        {{1, 2, 3}, {1, nan_, 3}, +1},
        {{1, nan_, 3, 4}, {1, 2, nan_}, -1},
    };
}

template <typename F>
auto equalToCmp(F eq) {
    return [eq](const auto& v1, const auto& v2) { return eq(v1, v2) ? 0 : 1; };
}

}  // namespace

TEST(GoSlices, Compare) {
    for (auto& test : equalIntTests()) EXPECT_EQ(slices::Compare(test.s1, test.s2) == 0, test.want);
    for (auto& test : equalFloatTests()) EXPECT_EQ(slices::Compare(test.s1, test.s2) == 0, test.wantEqualNaN);
    for (auto& test : compareIntTests()) EXPECT_EQ(slices::Compare(test.s1, test.s2), test.want);
    for (auto& test : compareFloatTests()) EXPECT_EQ(slices::Compare(test.s1, test.s2), test.want);
}

TEST(GoSlices, CompareFunc) {
    for (auto& test : equalIntTests()) {
        EXPECT_EQ(slices::CompareFunc(test.s1, test.s2, equalToCmp(equal<isize>)) == 0, test.want);
    }
    for (auto& test : equalFloatTests()) {
        EXPECT_EQ(slices::CompareFunc(test.s1, test.s2, equalToCmp(equal<double>)) == 0, test.wantEqual);
    }
    for (auto& test : compareIntTests()) {
        EXPECT_EQ(slices::CompareFunc(test.s1, test.s2, cmp::Compare<isize>), test.want);
    }
    for (auto& test : compareFloatTests()) {
        EXPECT_EQ(slices::CompareFunc(test.s1, test.s2, cmp::Compare<double>), test.want);
    }

    ints s1{1, 2, 3};
    ints s2{2, 3, 4};
    EXPECT_EQ(slices::CompareFunc(s1, s2, equalToCmp(offByOne)), 0);

    strs_t s3{"a", "b", "c"};
    strs_t s4{"A", "B", "C"};
    EXPECT_EQ(slices::CompareFunc(s3, s4, cmp::Compare<string>), 1);

    auto compareLower = [](const string& v1, const string& v2) {
        return cmp::Compare(strings::ToLower(v1), strings::ToLower(v2));
    };
    EXPECT_EQ(slices::CompareFunc(s3, s4, compareLower), 0);

    auto cmpIntString = [](isize v1, const string& v2) { return cmp::Compare(string(cmpIntStringStr(v1)), v2); };
    EXPECT_EQ(slices::CompareFunc(s1, s3, cmpIntString), 0);
}

namespace {

struct indexTest {
    ints s;
    isize v;
    isize want;
};

std::vector<indexTest> indexTests() {
    return {
        {{}, 0, -1},
        {emptyInts(), 0, -1},
        {{1, 2, 3}, 2, 1},
        {{1, 2, 2, 3}, 2, 1},
        {{1, 2, 3, 2}, 2, 1},
    };
}

template <typename F, typename T>
auto equalToIndex(F f, T v1) {
    return [f, v1](const T& v2) { return f(v1, v2); };
}

bool equalFold(const string& a, const string& b) { return strings::EqualFold(a, b); }

}  // namespace

TEST(GoSlices, Index) {
    for (auto& test : indexTests()) EXPECT_EQ(slices::Index(test.s, test.v), test.want);
}

TEST(GoSlices, IndexFunc) {
    for (auto& test : indexTests()) {
        EXPECT_EQ(slices::IndexFunc(test.s, equalToIndex(equal<isize>, test.v)), test.want);
    }
    strs_t s1{"hi", "HI"};
    EXPECT_EQ(slices::IndexFunc(s1, equalToIndex(equal<string>, string("HI"))), 1);
    EXPECT_EQ(slices::IndexFunc(s1, equalToIndex(equalFold, string("HI"))), 0);
}

TEST(GoSlices, Contains) {
    for (auto& test : indexTests()) EXPECT_EQ(slices::Contains(test.s, test.v), test.want != -1);
}

TEST(GoSlices, ContainsFunc) {
    for (auto& test : indexTests()) {
        EXPECT_EQ(slices::ContainsFunc(test.s, equalToIndex(equal<isize>, test.v)), test.want != -1);
    }
    strs_t s1{"hi", "HI"};
    EXPECT_TRUE(slices::ContainsFunc(s1, equalToIndex(equal<string>, string("HI"))));
    EXPECT_FALSE(slices::ContainsFunc(s1, equalToIndex(equal<string>, string("hI"))));
    EXPECT_TRUE(slices::ContainsFunc(s1, equalToIndex(equalFold, string("hI"))));
}

TEST(GoSlices, Insert) {
    ints s{1, 2, 3};
    EXPECT_TRUE(slices::Equal(slices::Insert(s, 0), s));
    struct {
        ints s;
        isize i;
        ints add, want;
    } insertTests[] = {
        {{1, 2, 3}, 0, {4}, {4, 1, 2, 3}},
        {{1, 2, 3}, 1, {4}, {1, 4, 2, 3}},
        {{1, 2, 3}, 3, {4}, {1, 2, 3, 4}},
        {{1, 2, 3}, 2, {4, 5}, {1, 2, 4, 5, 3}},
    };
    for (auto& test : insertTests) {
        auto copy = slices::Clone(test.s);
        EXPECT_TRUE(slices::Equal(slices::Insert(copy, test.i, test.add), test.want));
    }
}

TEST(GoSlices, InsertOverlap) {
    const isize N = 10;
    auto a = ints::make(N);
    for (isize n = 0; n <= N; n++) {                  // length
        for (isize i = 0; i <= n; i++) {              // insertion point
            for (isize x = 0; x <= N; x++) {          // start of inserted data
                for (isize y = x; y <= N; y++) {      // end of inserted data
                    for (isize k = 0; k < N; k++) a[k] = k;
                    ints want;
                    want = appendSpan(want, std::span<const isize>(a.Sub(0, i)));
                    want = appendSpan(want, std::span<const isize>(a.Sub(x, y)));
                    want = appendSpan(want, std::span<const isize>(a.Sub(i, n)));
                    auto got = slices::Insert(a.Sub(0, n), i, a.Sub(x, y));
                    EXPECT_TRUE(slices::Equal(got, want))
                        << "Insert with overlap failed n=" << n << " i=" << i << " x=" << x << " y=" << y;
                }
            }
        }
    }
}

TEST(GoSlicesDeathTest, InsertPanics) {
    auto a = ints::make(3);
    ints b = ints::make(1);
    struct {
        std::string name;
        ints s;
        isize i;
        ints v;
    } tests[] = {
        // There are no values.
        {"with negative index", a.Sub(0, 1, 1), -1, {}},
        {"with out-of-bounds index and > cap", a.Sub(0, 1, 1), 2, {}},
        {"with out-of-bounds index and = cap", a.Sub(0, 1, 2), 2, {}},
        {"with out-of-bounds index and < cap", a.Sub(0, 1, 3), 2, {}},
        // There are values.
        {"with negative index", a.Sub(0, 1, 1), -1, b},
        {"with out-of-bounds index and > cap", a.Sub(0, 1, 1), 2, b},
        {"with out-of-bounds index and = cap", a.Sub(0, 1, 2), 2, b},
        {"with out-of-bounds index and < cap", a.Sub(0, 1, 3), 2, b},
    };
    for (auto& test : tests) {
        EXPECT_DEATH((void)slices::Insert(test.s, test.i, test.v), "out of range") << test.name;
    }
}

TEST(GoSlices, Delete) {
    struct {
        ints s;
        isize i, j;
        ints want;
    } deleteTests[] = {
        {{1, 2, 3}, 0, 0, {1, 2, 3}},
        {{1, 2, 3}, 0, 1, {2, 3}},
        {{1, 2, 3}, 3, 3, {1, 2, 3}},
        {{1, 2, 3}, 0, 2, {3}},
        {{1, 2, 3}, 0, 3, emptyInts()},
    };
    for (auto& test : deleteTests) {
        auto copy = slices::Clone(test.s);
        EXPECT_TRUE(slices::Equal(slices::Delete(copy, test.i, test.j), test.want));
    }
}

TEST(GoSlices, DeleteFunc) {
    struct {
        ints s;
        std::function<bool(isize)> fn;
        ints want;
    } deleteFuncTests[] = {
        {{}, [](isize) { return true; }, {}},
        {{1, 2, 3}, [](isize) { return true; }, {}},
        {{1, 2, 3}, [](isize) { return false; }, {1, 2, 3}},
        {{1, 2, 3}, [](isize i) { return i > 2; }, {1, 2}},
        {{1, 2, 3}, [](isize i) { return i < 2; }, {2, 3}},
        {{10, 2, 30}, [](isize i) { return i >= 10; }, {2}},
    };
    int i = 0;
    for (auto& test : deleteFuncTests) {
        auto copy = slices::Clone(test.s);
        EXPECT_TRUE(slices::Equal(slices::DeleteFunc(copy, test.fn), test.want)) << "DeleteFunc case " << i;
        i++;
    }
}

TEST(GoSlicesDeathTest, DeletePanics) {
    ints s{0, 1, 2, 3, 4};
    s = s.Sub(0, 2);
    (void)s.Sub(0, 4);  // this is a valid slice of s
    struct {
        std::string name;
        ints s;
        isize i, j;
    } tests[] = {
        {"with negative first index", {42}, -2, 1},
        {"with negative second index", {42}, 1, -1},
        {"with out-of-bounds first index", {42}, 2, 3},
        {"with out-of-bounds second index", {42}, 0, 2},
        {"with out-of-bounds both indexes", {42}, 2, 2},
        {"with invalid i>j", {42}, 1, 0},
        {"s[i:j] is valid and j > len(s)", s, 0, 4},
        {"s[i:j] is valid and i == j > len(s)", s, 3, 3},
    };
    for (auto& test : tests) {
        EXPECT_DEATH((void)slices::Delete(test.s, test.i, test.j), "out of range") << test.name;
    }
}

TEST(GoSlices, DeleteClearTail) {
    int v[6] = {};
    slice<int*> mem{&v[0], &v[1], &v[2], &v[3], &v[4], &v[5]};
    auto s = mem.Sub(0, 5);  // there is 1 element beyond len(s), within cap(s)
    s = slices::Delete(s, 2, 4);
    EXPECT_EQ(mem[3], nullptr);
    EXPECT_EQ(mem[4], nullptr);
    EXPECT_NE(mem[5], nullptr) << "Delete: want unchanged elements beyond original len, got nil";
}

TEST(GoSlices, DeleteFuncClearTail) {
    int v[6] = {};
    slice<int*> mem{&v[0], &v[1], &v[2], &v[3], &v[4], &v[5]};
    *mem[2] = 42;
    *mem[3] = 42;
    auto s = mem.Sub(0, 5);  // there is 1 element beyond len(s), within cap(s)
    s = slices::DeleteFunc(s, [](int* i) { return i != nullptr && *i == 42; });
    EXPECT_EQ(mem[3], nullptr);
    EXPECT_EQ(mem[4], nullptr);
    EXPECT_NE(mem[5], nullptr) << "DeleteFunc: want unchanged elements beyond original len, got nil";
}

TEST(GoSlices, Clone) {
    ints s1{1, 2, 3};
    auto s2 = slices::Clone(s1);
    EXPECT_TRUE(slices::Equal(s1, s2));
    s1[0] = 4;
    EXPECT_TRUE(slices::Equal(s2, ints{1, 2, 3})) << "Clone changed unexpectedly";
    EXPECT_TRUE(slices::Clone(ints()).isNil()) << "Clone(nil) != nil";
    auto got = slices::Clone(s1.Sub(0, 0));
    EXPECT_TRUE(!got.isNil() && got.len() == 0);
}

namespace {
struct compactTest {
    std::string name;
    ints s, want;
};
std::vector<compactTest> compactTests() {
    return {
        {"nil", {}, {}},
        {"one", {1}, {1}},
        {"sorted", {1, 2, 3}, {1, 2, 3}},
        {"2 items", {1, 1, 2}, {1, 2}},
        {"unsorted", {1, 2, 1}, {1, 2, 1}},
        {"many", {1, 2, 2, 3, 3, 4}, {1, 2, 3, 4}},
    };
}
}  // namespace

TEST(GoSlices, Compact) {
    for (auto& test : compactTests()) {
        auto copy = slices::Clone(test.s);
        EXPECT_TRUE(slices::Equal(slices::Compact(copy), test.want)) << test.name;
    }
}

TEST(GoSlices, CompactFunc) {
    for (auto& test : compactTests()) {
        auto copy = slices::Clone(test.s);
        EXPECT_TRUE(slices::Equal(slices::CompactFunc(copy, equal<isize>), test.want)) << test.name;
    }
    strs_t s1{"a", "a", "A", "B", "b"};
    auto copy = slices::Clone(s1);
    EXPECT_TRUE(slices::Equal(slices::CompactFunc(copy, equalFold), strs_t{"a", "B"}));
}

TEST(GoSlices, CompactClearTail) {
    int one = 1, two = 2, three = 3, four = 4;
    slice<int*> mem{&one, &one, &two, &two, &three, &four};
    auto s = mem.Sub(0, 5);  // there is 1 element beyond len(s), within cap(s)
    s = slices::Compact(s);
    EXPECT_TRUE(slices::Equal(s, slice<int*>{&one, &two, &three}));
    EXPECT_EQ(mem[3], nullptr);
    EXPECT_EQ(mem[4], nullptr);
    EXPECT_EQ(mem[5], &four) << "Compact: want unchanged element beyond original len";
}

TEST(GoSlices, CompactFuncClearTail) {
    int a = 1, b = 1, c = 2, d = 2, e = 3, f = 4;
    slice<int*> mem{&a, &b, &c, &d, &e, &f};
    auto s = mem.Sub(0, 5);  // there is 1 element beyond len(s), within cap(s)
    s = slices::CompactFunc(s, [](int* x, int* y) {
        if (x == nullptr || y == nullptr) return x == y;
        return *x == *y;
    });
    EXPECT_TRUE(slices::Equal(s, slice<int*>{&a, &c, &e}));
    EXPECT_EQ(mem[3], nullptr);
    EXPECT_EQ(mem[4], nullptr);
    EXPECT_EQ(mem[5], &f) << "CompactFunc: want unchanged elements beyond original len";
}

TEST(GoSlices, Grow) {
    ints s1{1, 2, 3};

    auto copy = slices::Clone(s1);
    auto s2 = slices::Grow(copy, 1000);
    EXPECT_TRUE(slices::Equal(s1, s2));
    EXPECT_GE(s2.cap(), 1000 + s1.len());

    // Test mutation of elements between length and capacity.
    copy = slices::Clone(s1);
    auto s3 = slices::Grow(copy.Sub(0, 1), 2).Sub(0, 3);
    EXPECT_TRUE(slices::Equal(s1, s3)) << "Grow should not mutate elements between length and capacity";
    s3 = slices::Grow(copy.Sub(0, 1), 1000).Sub(0, 3);
    EXPECT_TRUE(slices::Equal(s1, s3)) << "Grow should not mutate elements between length and capacity";

    // Enough capacity: the same array.
    auto s4 = slices::Grow(s2, static_cast<isize>(s2.cap() - s2.len()));
    EXPECT_EQ(s4.data(), s2.data());
}

TEST(GoSlicesDeathTest, GrowNegative) {
    ints s1{1, 2, 3};
    EXPECT_DEATH((void)slices::Grow(s1, -1), "cannot be negative");
}

TEST(GoSlices, Clip) {
    ints s1 = ints{1, 2, 3, 4, 5, 6}.Sub(0, 3);
    EXPECT_EQ(s1.len(), 3u);
    EXPECT_GE(s1.cap(), 6u);
    auto s2 = slices::Clip(s1);
    EXPECT_TRUE(slices::Equal(s1, s2));
    EXPECT_EQ(s2.cap(), 3u);
}

TEST(GoSlices, Reverse) {
    ints even{3, 1, 4, 1, 5, 9};  // len = 6
    slices::Reverse(even);
    EXPECT_TRUE(slices::Equal(even, ints{9, 5, 1, 4, 1, 3}));

    ints odd{3, 1, 4, 1, 5, 9, 2};  // len = 7
    slices::Reverse(odd);
    EXPECT_TRUE(slices::Equal(odd, ints{2, 9, 5, 1, 4, 1, 3}));

    strs_t words{"one", "two", "three"};
    slices::Reverse(words);
    EXPECT_TRUE(slices::Equal(words, strs_t{"three", "two", "one"}));

    strs_t singleton{"one"};
    slices::Reverse(singleton);
    EXPECT_TRUE(slices::Equal(singleton, strs_t{"one"}));

    slices::Reverse(strs_t());
}

namespace {
// naiveReplace is a baseline implementation of Replace.
ints naiveReplace(ints s, isize i, isize j, std::span<const isize> v) {
    s = slices::Delete(s, i, j);
    s = slices::Insert(s, i, v);
    return s;
}
}  // namespace

TEST(GoSlices, Replace) {
    ints s3 = ints::make(3, 20);
    s3[0] = 0;
    s3[1] = 1;
    s3[2] = 2;
    struct {
        ints s, v;
        isize i, j;
    } tests[] = {
        {},  // all zero value
        {{1, 2, 3, 4}, {5}, 1, 2},
        {{1, 2, 3, 4}, {5, 6, 7, 8}, 1, 2},
        {s3, {3, 4, 5, 6, 7}, 0, 1},
    };
    for (auto& test : tests) {
        auto ss = slices::Clone(test.s), vv = slices::Clone(test.v);
        auto want = naiveReplace(ss, test.i, test.j, vv);
        auto got = slices::Replace(test.s, test.i, test.j, test.v);
        EXPECT_TRUE(slices::Equal(got, want));
    }
}

TEST(GoSlicesDeathTest, ReplacePanics) {
    ints s{0, 1, 2, 3, 4};
    s = s.Sub(0, 2);
    (void)s.Sub(0, 4);  // this is a valid slice of s
    struct {
        std::string name;
        ints s, v;
        isize i, j;
    } tests[] = {
        {"indexes out of order", {1, 2}, {3}, 2, 1},
        {"large index", {1, 2}, {3}, 1, 10},
        {"negative index", {1, 2}, {3}, -1, 2},
        {"s[i:j] is valid and j > len(s)", s, {}, 0, 4},
    };
    for (auto& test : tests) {
        auto ss = slices::Clone(test.s), vv = slices::Clone(test.v);
        EXPECT_DEATH((void)slices::Replace(ss, test.i, test.j, vv), "out of range") << test.name;
    }
}

TEST(GoSlices, ReplaceGrow) {
    // When Replace needs to allocate a new slice, the original slice must
    // not change.
    int a = 1, b = 2, c = 3, d = 4, e = 5, f = 6;
    slice<int*> mem{&a, &b, &c, &d, &e, &f};
    auto memcopy = slices::Clone(mem);
    auto s = mem.Sub(0, 5);  // there is 1 element beyond len(s), within cap(s)
    auto copy = slices::Clone(s);
    auto original = s;

    // The new elements don't fit within cap(s), so Replace will allocate.
    int z = 99;
    s = slices::Replace(s, 1, 3, &z, &z, &z, &z);
    EXPECT_TRUE(slices::Equal(s, slice<int*>{&a, &z, &z, &z, &z, &d, &e}));
    EXPECT_TRUE(slices::Equal(original, copy)) << "original slice has changed";
    EXPECT_TRUE(slices::Equal(mem, memcopy)) << "original backing memory has changed";
}

TEST(GoSlices, ReplaceClearTail) {
    int a = 1, b = 2, c = 3, d = 4, e = 5, f = 6;
    slice<int*> mem{&a, &b, &c, &d, &e, &f};
    auto s = mem.Sub(0, 5);  // there is 1 element beyond len(s), within cap(s)
    int y = 8, z = 9;
    s = slices::Replace(s, 1, 4, &y, &z);
    EXPECT_TRUE(slices::Equal(s, slice<int*>{&a, &y, &z, &e}));
    EXPECT_EQ(mem[4], nullptr) << "Replace: want nil discarded element";
    EXPECT_EQ(mem[5], &f) << "Replace: want unchanged elements beyond original len";
}

TEST(GoSlices, ReplaceOverlap) {
    const isize N = 10;
    auto a = ints::make(N);
    for (isize n = 0; n <= N; n++) {                      // length
        for (isize i = 0; i <= n; i++) {                  // insertion point 1
            for (isize j = i; j <= n; j++) {              // insertion point 2
                for (isize x = 0; x <= N; x++) {          // start of inserted data
                    for (isize y = x; y <= N; y++) {      // end of inserted data
                        for (isize k = 0; k < N; k++) a[k] = k;
                        ints want;
                        want = appendSpan(want, std::span<const isize>(a.Sub(0, i)));
                        want = appendSpan(want, std::span<const isize>(a.Sub(x, y)));
                        want = appendSpan(want, std::span<const isize>(a.Sub(j, n)));
                        auto got = slices::Replace(a.Sub(0, n), i, j, a.Sub(x, y));
                        ASSERT_TRUE(slices::Equal(got, want)) << "Insert with overlap failed n=" << n << " i=" << i
                                                              << " j=" << j << " x=" << x << " y=" << y;
                    }
                }
            }
        }
    }
}

TEST(GoSlices, ReplaceEndClearTail) {
    ints s{11, 22, 33};
    ints v{99};
    // case when j == len(s)
    s = slices::Replace(s, 1, 3, v);
    EXPECT_EQ(s.Sub(0, 3)[2], 0) << "obsolete element";
}

TEST(GoSlices, InsertGrowthRate) {
    auto b = slice<byte>::make(1);
    std::size_t maxCap = b.cap();
    int nGrow = 0;
    const int N = 1000000;
    for (int i = 0; i < N; i++) {
        b = slices::Insert(b, static_cast<isize>(b.len()) - 1, byte(0));
        if (b.cap() > maxCap) {
            maxCap = b.cap();
            nGrow++;
        }
    }
    int want = static_cast<int>(std::log(N) / std::log(1.25));  // 1.25 == growth rate for large slices
    EXPECT_LE(nGrow, want) << "too many grows";
}

TEST(GoSlices, ReplaceGrowthRate) {
    auto b = slice<byte>::make(2);
    std::size_t maxCap = b.cap();
    int nGrow = 0;
    const int N = 1000000;
    for (int i = 0; i < N; i++) {
        b = slices::Replace(b, static_cast<isize>(b.len()) - 2, static_cast<isize>(b.len()) - 1, byte(0), byte(0));
        if (b.cap() > maxCap) {
            maxCap = b.cap();
            nGrow++;
        }
    }
    int want = static_cast<int>(std::log(N) / std::log(1.25));  // 1.25 == growth rate for large slices
    EXPECT_LE(nGrow, want) << "too many grows";
}

TEST(GoSlices, Concat) {
    struct {
        std::vector<ints> s;
        ints want;
    } cases[] = {
        {{{}}, {}},
        {{{1}}, {1}},
        {{{1}, {2}}, {1, 2}},
        {{{1}, {}, {2}}, {1, 2}},
    };
    for (auto& tc : cases) {
        ints got;
        switch (tc.s.size()) {
            case 1: got = slices::Concat(tc.s[0]); break;
            case 2: got = slices::Concat(tc.s[0], tc.s[1]); break;
            case 3: got = slices::Concat(tc.s[0], tc.s[1], tc.s[2]); break;
        }
        EXPECT_TRUE(slices::Equal(tc.want, got));
    }
}

TEST(GoSlices, Repeat) {
    struct {
        ints x;
        isize count;
        ints want;
    } cases[] = {
        {{}, 0, emptyInts()},
        {{}, 1, emptyInts()},
        {{}, std::numeric_limits<isize>::max(), emptyInts()},
        {emptyInts(), 0, emptyInts()},
        {emptyInts(), 1, emptyInts()},
        {emptyInts(), std::numeric_limits<isize>::max(), emptyInts()},
        {{0}, 0, emptyInts()},
        {{0}, 1, {0}},
        {{0}, 2, {0, 0}},
        {{0}, 3, {0, 0, 0}},
        {{0}, 4, {0, 0, 0, 0}},
        {{0, 1}, 0, emptyInts()},
        {{0, 1}, 1, {0, 1}},
        {{0, 1}, 2, {0, 1, 0, 1}},
        {{0, 1}, 3, {0, 1, 0, 1, 0, 1}},
        {{0, 1}, 4, {0, 1, 0, 1, 0, 1, 0, 1}},
        {{0, 1, 2}, 0, emptyInts()},
        {{0, 1, 2}, 1, {0, 1, 2}},
        {{0, 1, 2}, 2, {0, 1, 2, 0, 1, 2}},
        {{0, 1, 2}, 3, {0, 1, 2, 0, 1, 2, 0, 1, 2}},
        {{0, 1, 2}, 4, {0, 1, 2, 0, 1, 2, 0, 1, 2, 0, 1, 2}},
    };
    for (auto& tc : cases) {
        auto got = slices::Repeat(tc.x, tc.count);
        EXPECT_TRUE(!got.isNil() && got.cap() == tc.want.cap() && slices::Equal(got, tc.want))
            << "Repeat(" << tc.x.len() << ", " << tc.count << ")";
    }
}

TEST(GoSlicesDeathTest, RepeatPanics) {
    EXPECT_DEATH((void)slices::Repeat(slice<empty>::make(0), -1), "cannot be negative");
    EXPECT_DEATH((void)slices::Repeat(slice<empty>::make(3), std::numeric_limits<isize>::max()), "overflows");
    EXPECT_DEATH((void)slices::Repeat(slice<empty>::make(2), 1 + std::numeric_limits<isize>::max() / 2), "overflows");
}

TEST(GoSlices, Issue68488) {
    auto s = ints::make(3);
    auto clone = slices::Clone(s.Sub(1, 1));
    EXPECT_TRUE(clone.data() != &s[0] && clone.data() != &s[1] && clone.data() != &s[2])
        << "clone keeps alive s due to array overlap";
}

// The behavior when the primary slice operand is nil.
TEST(GoSlices, Nilness) {
    ints emptySlice = emptyInts();
    ints nilSlice;
    auto emptySeq = std::views::empty<isize>;
    auto truth = [](isize) { return true; };
    auto equal = [](isize, isize) -> bool { ADD_FAILURE() << "unreachable"; return false; };

    // The update functions preserve nilness, like s[i:j].
    EXPECT_TRUE(slices::AppendSeq(nilSlice, emptySeq).isNil()) << "AppendSeq(nil, empty)";
    EXPECT_FALSE(slices::AppendSeq(emptySlice, emptySeq).isNil()) << "AppendSeq(nil, empty)";

    EXPECT_TRUE(slices::Insert(nilSlice, 0).isNil()) << "Insert(nil, 0)";
    EXPECT_FALSE(slices::Insert(emptySlice, 0).isNil()) << "Insert(empty, 0)";

    EXPECT_TRUE(slices::Delete(nilSlice, 0, 0).isNil()) << "Delete(nil, 0, 0)";
    EXPECT_FALSE(slices::Delete(emptySlice, 0, 0).isNil()) << "Delete(empty, 0, 0)";
    EXPECT_FALSE(slices::Delete(ints{1}, 0, 1).isNil()) << "Delete([]int{1}, 0, 1)";

    EXPECT_TRUE(slices::DeleteFunc(nilSlice, truth).isNil()) << "DeleteFunc(nil, f)";
    EXPECT_FALSE(slices::DeleteFunc(emptySlice, truth).isNil()) << "DeleteFunc(empty, f)";
    EXPECT_FALSE(slices::DeleteFunc(ints{1}, truth).isNil()) << "DeleteFunc([]int{1}, truth)";

    EXPECT_TRUE(slices::Replace(nilSlice, 0, 0).isNil()) << "Replace(nil, 0, 0)";
    EXPECT_FALSE(slices::Replace(emptySlice, 0, 0).isNil()) << "Replace(empty, 0, 0)";
    EXPECT_FALSE(slices::Replace(ints{1}, 0, 1).isNil()) << "Replace([]int{1}, 0, 1)";

    EXPECT_TRUE(slices::Clone(nilSlice).isNil()) << "Clone(nil)";
    EXPECT_FALSE(slices::Clone(emptySlice).isNil()) << "Clone(empty)";

    EXPECT_TRUE(slices::Compact(nilSlice).isNil()) << "Compact(nil)";
    EXPECT_FALSE(slices::Compact(emptySlice).isNil()) << "Compact(empty)";

    EXPECT_TRUE(slices::CompactFunc(nilSlice, equal).isNil()) << "CompactFunc(nil)";
    EXPECT_FALSE(slices::CompactFunc(emptySlice, equal).isNil()) << "CompactFunc(empty)";

    EXPECT_TRUE(slices::Grow(nilSlice, 0).isNil()) << "Grow(nil, 0)";
    EXPECT_FALSE(slices::Grow(emptySlice, 0).isNil()) << "Grow(empty, 0)";

    EXPECT_TRUE(slices::Clip(nilSlice).isNil()) << "Clip(nil)";
    EXPECT_FALSE(slices::Clip(emptySlice).isNil()) << "Clip(empty)";
    EXPECT_FALSE(slices::Clip(ints{1}.Sub(0, 0, 0)).isNil()) << "Clip([]int{1}[:0:0])";

    // Concat returns nil iff the result is empty.
    EXPECT_TRUE(slices::Concat(nilSlice, emptySlice, nilSlice, emptySlice).isNil()) << "Concat(nil, ...empty...)";
    EXPECT_TRUE(slices::Concat(emptySlice, emptySlice, nilSlice, emptySlice).isNil()) << "Concat(empty, ...empty...)";

    // Repeat never returns nil.
    EXPECT_FALSE(slices::Repeat(nilSlice, 0).isNil()) << "Repeat(nil, 0)";
    EXPECT_FALSE(slices::Repeat(emptySlice, 0).isNil()) << "Repeat(empty, 0)";
    EXPECT_FALSE(slices::Repeat(nilSlice, 2).isNil()) << "Repeat(nil, 2)";
    EXPECT_FALSE(slices::Repeat(emptySlice, 2).isNil()) << "Repeat(empty, 2)";

    // The collection functions return nil given an empty sequence.
    EXPECT_TRUE(slices::Collect(emptySeq).isNil()) << "Collect(empty)";
    EXPECT_TRUE(slices::Sorted(emptySeq).isNil()) << "Sorted(empty)";
    EXPECT_TRUE(slices::SortedFunc(emptySeq, cmp::Compare<isize>).isNil()) << "SortedFunc(empty)";
    EXPECT_TRUE(slices::SortedStableFunc(emptySeq, cmp::Compare<isize>).isNil()) << "SortedStableFunc(empty)";
}

// ---------------------------------------------------------------------------
// sort_test.go

namespace {

ints sortInts() { return {74, 59, 238, -784, 9845, 959, 905, 0, 0, 42, 7586, -5467984, 7586}; }
floats sortFloat64s() {
    return {74.3, 59.0, inf, 238.2, -784.0, 2.3, -inf, 9845.768, -959.7485, 905, 7.8, 7.8,
            74.3, 59.0, inf, 238.2, -784.0, 2.3};
}
strs_t sortStrs() { return {"", "Hello", "foo", "bar", "foo", "f00", "%*&^*&^&", "***"}; }

struct intPair {
    isize a, b;
};
using intPairs = slice<intPair>;

// Pairs compare on a only.
int intPairCmp(const intPair& x, const intPair& y) { return static_cast<int>(x.a - y.a); }

// Record initial order in b.
void initB(intPairs& d) {
    for (std::size_t i = 0; i < d.len(); i++) d[i].b = static_cast<isize>(i);
}

// Whether a-equal elements were not reordered; reversed expects reverse
// ordering.
bool inOrder(const intPairs& d, bool reversed) {
    isize lastA = -1, lastB = 0;
    for (auto& p : d) {
        if (lastA != p.a) {
            lastA = p.a;
            lastB = p.b;
            continue;
        }
        if (!reversed ? p.b <= lastB : p.b >= lastB) return false;
        lastB = p.b;
    }
    return true;
}

}  // namespace

TEST(GoSlices, SortIntSlice) {
    auto data = slices::Clone(sortInts());
    slices::Sort(data);
    EXPECT_TRUE(slices::IsSorted(data));
}

TEST(GoSlices, SortFuncIntSlice) {
    auto data = slices::Clone(sortInts());
    slices::SortFunc(data, [](isize a, isize b) { return a < b ? -1 : a > b ? 1 : 0; });
    EXPECT_TRUE(slices::IsSorted(data));
}

TEST(GoSlices, SortFloat64Slice) {
    auto data = slices::Clone(sortFloat64s());
    slices::Sort(data);
    EXPECT_TRUE(slices::IsSorted(data));
}

TEST(GoSlices, SortStringSlice) {
    auto data = slices::Clone(sortStrs());
    slices::Sort(data);
    EXPECT_TRUE(slices::IsSorted(data));
}

TEST(GoSlices, SortLarge_Random) {
    auto data = ints::make(1000000);
    for (auto& v : data) v = rand_intn(100);
    ASSERT_FALSE(slices::IsSorted(data)) << "terrible rand.rand";
    slices::Sort(data);
    EXPECT_TRUE(slices::IsSorted(data)) << "sort didn't sort - 1M ints";
}

TEST(GoSlices, Stability) {
    isize n = 100000, m = 1000;
    auto data = intPairs::make(n);

    // random distribution
    for (auto& p : data) p.a = rand_intn(m);
    ASSERT_FALSE(slices::IsSortedFunc(data, intPairCmp)) << "terrible rand.rand";
    initB(data);
    slices::SortStableFunc(data, intPairCmp);
    EXPECT_TRUE(slices::IsSortedFunc(data, intPairCmp)) << "Stable didn't sort";
    EXPECT_TRUE(inOrder(data, false)) << "Stable wasn't stable";

    // already sorted
    initB(data);
    slices::SortStableFunc(data, intPairCmp);
    EXPECT_TRUE(slices::IsSortedFunc(data, intPairCmp)) << "Stable shuffled sorted ints (order)";
    EXPECT_TRUE(inOrder(data, false)) << "Stable shuffled sorted ints (stability)";

    // sorted reversed
    for (std::size_t i = 0; i < data.len(); i++) data[i].a = n - static_cast<isize>(i);
    initB(data);
    slices::SortStableFunc(data, intPairCmp);
    EXPECT_TRUE(slices::IsSortedFunc(data, intPairCmp)) << "Stable didn't sort";
    EXPECT_TRUE(inOrder(data, false)) << "Stable wasn't stable";
}

TEST(GoSlices, MinMax) {
    auto intCmp = [](isize a, isize b) { return static_cast<int>(a - b); };
    struct {
        ints data;
        isize wantMin, wantMax;
    } tests[] = {
        {{7}, 7, 7},
        {{1, 2}, 1, 2},
        {{2, 1}, 1, 2},
        {{1, 2, 3}, 1, 3},
        {{3, 2, 1}, 1, 3},
        {{2, 1, 3}, 1, 3},
        {{2, 2, 3}, 2, 3},
        {{3, 2, 3}, 2, 3},
        {{0, 2, -9}, -9, 2},
    };
    for (auto& tt : tests) {
        EXPECT_EQ(slices::Min(tt.data), tt.wantMin);
        EXPECT_EQ(slices::MinFunc(tt.data, intCmp), tt.wantMin);
        EXPECT_EQ(slices::Max(tt.data), tt.wantMax);
        EXPECT_EQ(slices::MaxFunc(tt.data, intCmp), tt.wantMax);
    }

    struct S {
        isize a;
        string b;
    };
    auto cmpS = [](const S& s1, const S& s2) { return cmp::Compare(s1.a, s2.a); };
    std::vector<S> svals{{1, "a"}, {2, "a"}, {1, "b"}, {2, "b"}};

    auto gotMin = slices::MinFunc(svals, cmpS);
    EXPECT_TRUE(gotMin.a == 1 && gotMin.b == "a");
    auto gotMax = slices::MaxFunc(svals, cmpS);
    EXPECT_TRUE(gotMax.a == 2 && gotMax.b == "a");
}

TEST(GoSlices, MinMaxNaNs) {
    floats fs{1.0, 999.9, 3.14, -400.4, -5.14};
    EXPECT_EQ(slices::Min(fs), -400.4);
    EXPECT_EQ(slices::Max(fs), 999.9);

    // No matter which element of fs is replaced with a NaN, both Min and Max
    // should propagate the NaN to their output.
    for (std::size_t i = 0; i < fs.len(); i++) {
        auto testfs = slices::Clone(fs);
        testfs[i] = nan_;
        EXPECT_TRUE(std::isnan(slices::Min(testfs))) << i;
        EXPECT_TRUE(std::isnan(slices::Max(testfs))) << i;
    }
}

TEST(GoSlicesDeathTest, MinMaxPanics) {
    auto intCmp = [](isize a, isize b) { return static_cast<int>(a - b); };
    auto emptySlice = emptyInts();
    EXPECT_DEATH((void)slices::Min(emptySlice), "empty list");
    EXPECT_DEATH((void)slices::Max(emptySlice), "empty list");
    EXPECT_DEATH((void)slices::MinFunc(emptySlice, intCmp), "empty list");
    EXPECT_DEATH((void)slices::MaxFunc(emptySlice, intCmp), "empty list");
}

TEST(GoSlices, BinarySearch) {
    strs_t str1{"foo"};
    strs_t str2{"ab", "ca"};
    strs_t str3{"mo", "qo", "vo"};
    strs_t str4{"ab", "ad", "ca", "xy"};
    // slice with repeating elements
    strs_t strRepeats{"ba", "ca", "da", "da", "da", "ka", "ma", "ma", "ta"};
    // slice with all element equal
    strs_t strSame{"xx", "xx", "xx"};

    struct {
        strs_t data;
        string target;
        isize wantPos;
        bool wantFound;
    } tests[] = {
        {strs_t::make(0), "foo", 0, false},
        {strs_t::make(0), "", 0, false},

        {str1, "foo", 0, true},
        {str1, "bar", 0, false},
        {str1, "zx", 1, false},

        {str2, "aa", 0, false},
        {str2, "ab", 0, true},
        {str2, "ad", 1, false},
        {str2, "ca", 1, true},
        {str2, "ra", 2, false},

        {str3, "bb", 0, false},
        {str3, "mo", 0, true},
        {str3, "nb", 1, false},
        {str3, "qo", 1, true},
        {str3, "tr", 2, false},
        {str3, "vo", 2, true},
        {str3, "xr", 3, false},

        {str4, "aa", 0, false},
        {str4, "ab", 0, true},
        {str4, "ac", 1, false},
        {str4, "ad", 1, true},
        {str4, "ax", 2, false},
        {str4, "ca", 2, true},
        {str4, "cc", 3, false},
        {str4, "dd", 3, false},
        {str4, "xy", 3, true},
        {str4, "zz", 4, false},

        {strRepeats, "da", 2, true},
        {strRepeats, "db", 5, false},
        {strRepeats, "ma", 6, true},
        {strRepeats, "mb", 8, false},

        {strSame, "xx", 0, true},
        {strSame, "ab", 0, false},
        {strSame, "zz", 3, false},
    };
    for (auto& tt : tests) {
        {
            auto [pos, found] = slices::BinarySearch(tt.data, tt.target);
            EXPECT_EQ(pos, tt.wantPos) << tt.target;
            EXPECT_EQ(found, tt.wantFound) << tt.target;
        }
        {
            auto [pos, found] = slices::BinarySearchFunc(tt.data, tt.target, cmp::Compare<string>);
            EXPECT_EQ(pos, tt.wantPos) << tt.target;
            EXPECT_EQ(found, tt.wantFound) << tt.target;
        }
    }
}

TEST(GoSlices, BinarySearchInts) {
    ints data{20, 30, 40, 50, 60, 70, 80, 90};
    struct {
        isize target, wantPos;
        bool wantFound;
    } tests[] = {
        {20, 0, true},
        {23, 1, false},
        {43, 3, false},
        {80, 6, true},
    };
    for (auto& tt : tests) {
        {
            auto [pos, found] = slices::BinarySearch(data, tt.target);
            EXPECT_EQ(pos, tt.wantPos);
            EXPECT_EQ(found, tt.wantFound);
        }
        {
            auto cmp = [](isize a, isize b) { return static_cast<int>(a - b); };
            auto [pos, found] = slices::BinarySearchFunc(data, tt.target, cmp);
            EXPECT_EQ(pos, tt.wantPos);
            EXPECT_EQ(found, tt.wantFound);
        }
    }
}

TEST(GoSlices, BinarySearchFloats) {
    floats data{nan_, -0.25, 0.0, 1.4};
    struct {
        double target;
        isize wantPos;
        bool wantFound;
    } tests[] = {
        {nan_, 0, true},
        {-inf, 1, false},
        {-0.25, 1, true},
        {0.0, 2, true},
        {1.4, 3, true},
        {1.5, 4, false},
    };
    for (auto& tt : tests) {
        auto [pos, found] = slices::BinarySearch(data, tt.target);
        EXPECT_EQ(pos, tt.wantPos) << tt.target;
        EXPECT_EQ(found, tt.wantFound) << tt.target;
    }
}

TEST(GoSlices, BinarySearchFunc) {
    ints data{1, 10, 11, 2};  // sorted lexicographically
    auto cmp = [](isize a, std::string_view b) { return cmp::Compare(std::to_string(a), std::string(b)); };
    auto [pos, found] = slices::BinarySearchFunc(data, std::string_view("2"), cmp);
    EXPECT_EQ(pos, 3);
    EXPECT_TRUE(found);
}

// ---------------------------------------------------------------------------
// iter_test.go

TEST(GoSlices, All) {
    for (isize size = 0; size < 10; size++) {
        ints s;
        for (isize i = 0; i < size; i++) s = append(s, i);
        isize ei = 0, ev = 0, cnt = 0;
        for (auto [i, v] : slices::All(s)) {
            EXPECT_TRUE(i == ei && v == ev) << "at iteration " << cnt;
            ei++;
            ev++;
            cnt++;
        }
        EXPECT_EQ(cnt, size);
    }
}

TEST(GoSlices, Backward) {
    for (isize size = 0; size < 10; size++) {
        ints s;
        for (isize i = 0; i < size; i++) s = append(s, i);
        isize ei = size - 1, ev = size - 1, cnt = 0;
        for (auto [i, v] : slices::Backward(s)) {
            EXPECT_TRUE(i == ei && v == ev) << "at iteration " << cnt;
            ei--;
            ev--;
            cnt++;
        }
        EXPECT_EQ(cnt, size);
    }
}

TEST(GoSlices, Values) {
    for (isize size = 0; size < 10; size++) {
        ints s;
        for (isize i = 0; i < size; i++) s = append(s, i);
        isize ev = 0, cnt = 0;
        for (isize v : slices::Values(s)) {
            EXPECT_EQ(v, ev) << "at iteration " << cnt;
            ev++;
            cnt++;
        }
        EXPECT_EQ(cnt, size);
    }
}

namespace {
auto testSeq() { return std::views::iota(isize(0), isize(10)) | std::views::stride(2); }
const ints testSeqResult{0, 2, 4, 6, 8};

std::vector<strs_t> iterTests() { return {{}, {"a"}, {"a", "b"}, {"b", "a"}, sortStrs()}; }
}  // namespace

TEST(GoSlices, AppendSeq) {
    auto s = slices::AppendSeq(ints{1, 2}, testSeq());
    auto want = appendSpan(ints{1, 2}, std::span<const isize>(testSeqResult));
    EXPECT_TRUE(slices::Equal(s, want));
}

TEST(GoSlices, Collect) {
    EXPECT_TRUE(slices::Equal(slices::Collect(testSeq()), testSeqResult));
}

TEST(GoSlices, ValuesAppendSeq) {
    for (auto& prefix : iterTests()) {
        for (auto& s : iterTests()) {
            auto got = slices::AppendSeq(slices::Clone(prefix), slices::Values(s));
            auto want = appendSpan(slices::Clone(prefix), std::span<const string>(s));
            EXPECT_TRUE(slices::Equal(got, want));
        }
    }
}

TEST(GoSlices, ValuesCollect) {
    for (auto& s : iterTests()) EXPECT_TRUE(slices::Equal(slices::Collect(slices::Values(s)), s));
}

TEST(GoSlices, Sorted) {
    auto s = slices::Sorted(slices::Values(sortInts()));
    EXPECT_TRUE(slices::IsSorted(s));
}

TEST(GoSlices, SortedFunc) {
    auto s = slices::SortedFunc(slices::Values(sortInts()), [](isize a, isize b) { return static_cast<int>(a - b); });
    EXPECT_TRUE(slices::IsSorted(s));
}

TEST(GoSlices, SortedStableFunc) {
    isize n = 1000, m = 100;
    auto data = intPairs::make(n);
    for (auto& p : data) p.a = rand_intn(m);
    initB(data);

    auto s = slices::SortedStableFunc(slices::Values(data), intPairCmp);
    EXPECT_TRUE(slices::IsSortedFunc(s, intPairCmp)) << "SortedStableFunc didn't sort";
    EXPECT_TRUE(inOrder(s, false)) << "SortedStableFunc wasn't stable";

    // iterVal converts a Seq2 to a Seq.
    auto iterVal = std::views::transform([](auto iv) { return std::get<1>(iv); });
    s = slices::SortedStableFunc(slices::Backward(data) | iterVal, intPairCmp);
    EXPECT_TRUE(slices::IsSortedFunc(s, intPairCmp)) << "SortedStableFunc didn't sort reverse ints";
    EXPECT_TRUE(inOrder(s, true)) << "SortedStableFunc wasn't stable on reverse ints";
}

namespace {
bool chunkEqual(const std::vector<ints>& s1, const std::vector<ints>& s2) {
    return slices::EqualFunc(s1, s2, [](const ints& a, const ints& b) { return slices::Equal(a, b); });
}
}  // namespace

TEST(GoSlices, Chunk) {
    struct {
        std::string name;
        ints s;
        isize n;
        std::vector<ints> chunks;
    } cases[] = {
        {"nil", {}, 1, {}},
        {"empty", emptyInts(), 1, {}},
        {"short", {1, 2}, 3, {{1, 2}}},
        {"one", {1, 2}, 2, {{1, 2}}},
        {"even", {1, 2, 3, 4}, 2, {{1, 2}, {3, 4}}},
        {"odd", {1, 2, 3, 4, 5}, 2, {{1, 2}, {3, 4}, {5}}},
    };
    for (auto& tc : cases) {
        std::vector<ints> chunks;
        for (ints c : slices::Chunk(tc.s, tc.n)) chunks.push_back(c);
        EXPECT_TRUE(chunkEqual(chunks, tc.chunks)) << tc.name;
        if (chunks.empty()) continue;

        // Appending to the end of the first chunk must not clobber the
        // beginning of the next chunk.
        auto s = slices::Clone(tc.s);
        chunks[0] = append(chunks[0], isize(-1));
        EXPECT_TRUE(slices::Equal(s, tc.s)) << "slice was clobbered";
    }
}

TEST(GoSlicesDeathTest, ChunkPanics) {
    EXPECT_DEATH((void)slices::Chunk(slice<empty>::make(0), 0), "cannot be less than 1");
}

TEST(GoSlices, ChunkRange) {
    // Chunk iteration can be stopped.
    std::vector<ints> got;
    for (ints c : slices::Chunk(ints{1, 2, 3, 4, -100}, 2)) {
        if (got.size() == 2) break;  // Found enough values, break early.
        got.push_back(c);
    }
    EXPECT_TRUE(chunkEqual(got, {{1, 2}, {3, 4}}));
}
