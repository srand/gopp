// Ported from Go 1.27.3's sort/sort_test.go and sort/search_test.go, with
// Go's test names and tables (the benchmarks, the allocation test and the
// operation counts, which only log, left out).
#include <gopp/cmp.hpp>
#include <gopp/slices.hpp>
#include <gopp/sort.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <format>
#include <functional>
#include <limits>
#include <random>
#include <string>
#include <vector>

using namespace gopp;

namespace {

const double inf = std::numeric_limits<double>::infinity();
const double nan_ = std::numeric_limits<double>::quiet_NaN();

const std::array<isize, 13> ints{74, 59, 238, -784, 9845, 959, 905, 0, 0, 42, 7586, -5467984, 7586};
const std::array<double, 14> float64s{74.3, 59.0, inf, 238.2, -784.0, 2.3, nan_, nan_, -inf, 9845.768, -959.7485, 905, 7.8, 7.8};
std::array<string, 8> strings_data() { return {"", "Hello", "foo", "bar", "foo", "f00", "%*&^*&^&", "***"}; }

std::mt19937_64 rng(1);
isize rand_intn(isize n) { return std::uniform_int_distribution<isize>(0, n - 1)(rng); }

// export_test.go
void Heapsort(sort::Interface& data) {
    sort::detail::interface_data d{data};
    slices::detail::heap_sort(d, 0, data.Len());
}
void ReverseRange(sort::Interface&& data, isize a, isize b) {
    sort::detail::interface_data d{data};
    slices::detail::reverse_range(d, a, b);
}

}  // namespace

TEST(GoSort, SortIntSlice) {
    auto data = ints;
    sort::IntSlice a(data);
    sort::Sort(a);
    EXPECT_TRUE(sort::IsSorted(a));
}

TEST(GoSort, SortFloat64Slice) {
    auto data = float64s;
    sort::Float64Slice a(data);
    sort::Sort(a);
    EXPECT_TRUE(sort::IsSorted(a));
}

// Compare Sort with slices.Sort sorting a float64 slice containing NaNs.
TEST(GoSort, SortFloat64sCompareSlicesSort) {
    auto slice1 = slice<double>::of(float64s);
    auto slice2 = slice<double>::of(float64s);
    sort::Sort(sort::Float64Slice(slice1));
    slices::Sort(slice2);
    EXPECT_TRUE(slices::EqualFunc(slice1, slice2, [](double a, double b) { return cmp::Compare(a, b) == 0; }));
}

TEST(GoSort, SortStringSlice) {
    auto data = strings_data();
    sort::StringSlice a(data);
    sort::Sort(a);
    EXPECT_TRUE(sort::IsSorted(a));
}

TEST(GoSort, Ints) {
    auto data = ints;
    sort::Ints(data);
    EXPECT_TRUE(sort::IntsAreSorted(data));
}

TEST(GoSort, Float64s) {
    auto data = float64s;
    sort::Float64s(data);
    EXPECT_TRUE(sort::Float64sAreSorted(data));
}

TEST(GoSort, Strings) {
    auto data = strings_data();
    sort::Strings(data);
    EXPECT_TRUE(sort::StringsAreSorted(data));
}

TEST(GoSort, Slice) {
    auto data = strings_data();
    sort::Slice(data, [&](isize i, isize j) { return data[i] < data[j]; });
    EXPECT_TRUE(sort::SliceIsSorted(data, [&](isize i, isize j) { return data[i] < data[j]; }));
}

TEST(GoSort, SortLarge_Random) {
    std::size_t n = 1000000;
    std::vector<isize> data(n);
    for (auto& v : data) v = rand_intn(100);
    ASSERT_FALSE(sort::IntsAreSorted(data)) << "terrible rand.rand";
    sort::Ints(data);
    EXPECT_TRUE(sort::IntsAreSorted(data)) << "sort didn't sort - 1M ints";
}

TEST(GoSort, ReverseSortIntSlice) {
    auto data = ints;
    auto data1 = ints;
    sort::IntSlice a(data);
    sort::Sort(a);
    sort::IntSlice r(data1);
    sort::Sort(sort::Reverse(r));
    for (std::size_t i = 0; i < data.size(); i++) {
        EXPECT_EQ(a.s[i], r.s[data.size() - 1 - i]) << "reverse sort didn't sort";
        if (i > data.size() / 2) break;
    }
}

TEST(GoSort, BreakPatterns) {
    // Special slice used to trigger breakPatterns.
    std::vector<isize> data(30, 10);
    data[(data.size() / 4) * 1] = 0;
    data[(data.size() / 4) * 2] = 1;
    data[(data.size() / 4) * 3] = 2;
    sort::Sort(sort::IntSlice(data));
}

TEST(GoSort, ReverseRange) {
    std::vector<isize> data{1, 2, 3, 4, 5, 6, 7};
    ReverseRange(sort::IntSlice(data), 0, std::ssize(data));
    for (std::size_t i = data.size() - 1; i > 0; i--) {
        ASSERT_LE(data[i], data[i - 1]) << "reverseRange didn't work";
    }
    std::vector<isize> data1{1, 2, 3, 4, 5, 6, 7};
    std::vector<isize> data2{1, 2, 5, 4, 3, 6, 7};
    ReverseRange(sort::IntSlice(data1), 2, 5);
    EXPECT_EQ(data1, data2) << "reverseRange didn't work";
}

namespace {
struct nonDeterministicTestingData : sort::Interface {
    std::mt19937 r{0};
    isize Len() override { return 500; }
    bool Less(isize i, isize j) override {
        if (i < 0 || j < 0 || i >= Len() || j >= Len()) ADD_FAILURE() << "nondeterministic comparison out of bounds";
        return std::uniform_real_distribution<float>(0, 1)(r) < 0.5f;
    }
    void Swap(isize i, isize j) override {
        if (i < 0 || j < 0 || i >= Len() || j >= Len()) ADD_FAILURE() << "nondeterministic comparison out of bounds";
    }
};
}  // namespace

TEST(GoSort, NonDeterministicComparison) {
    // Sort must stay in bounds when Less returns inconsistent results.
    nonDeterministicTestingData td;
    for (int i = 0; i < 10; i++) sort::Sort(td);
}

namespace {

enum { Sawtooth, Rand, Stagger, Plateau, Shuffle, NDist };
enum { Copy, Reverse, ReverseFirstHalf, ReverseSecondHalf, Sorted, Dither, NMode };

struct testingData : sort::Interface {
    std::string desc;
    std::span<isize> data;
    isize maxswap = 0;  // number of swaps allowed
    isize ncmp = 0, nswap = 0;
    bool failed = false;

    isize Len() override { return std::ssize(data); }
    bool Less(isize i, isize j) override {
        ncmp++;
        return data[i] < data[j];
    }
    void Swap(isize i, isize j) override {
        if (nswap >= maxswap && !failed) {
            ADD_FAILURE() << std::format("{}: used {} swaps sorting slice of {}", desc, nswap, data.size());
            failed = true;
        }
        nswap++;
        std::swap(data[i], data[j]);
    }
};

isize lg(isize n) {
    isize i = 0;
    while ((isize(1) << i) < n) i++;
    return i;
}

void testBentleyMcIlroy(const std::function<void(sort::Interface&)>& sort_fn, const std::function<isize(isize)>& maxswap) {
    const isize sizes[] = {100, 1023, 1024, 1025};
    const char* dists[] = {"sawtooth", "rand", "stagger", "plateau", "shuffle"};
    const char* modes[] = {"copy", "reverse", "reverse1", "reverse2", "sort", "dither"};
    std::array<isize, 1025> tmp1{}, tmp2{};
    for (isize n : sizes) {
        for (isize m = 1; m < 2 * n; m *= 2) {
            for (int dist = 0; dist < NDist; dist++) {
                isize j = 0, k = 1;
                auto data = std::span(tmp1).first(n);
                for (isize i = 0; i < n; i++) {
                    switch (dist) {
                        case Sawtooth: data[i] = i % m; break;
                        case Rand: data[i] = rand_intn(m); break;
                        case Stagger: data[i] = (i * m + i) % n; break;
                        case Plateau: data[i] = std::min(i, m); break;
                        case Shuffle:
                            if (rand_intn(m) != 0) {
                                j += 2;
                                data[i] = j;
                            } else {
                                k += 2;
                                data[i] = k;
                            }
                            break;
                    }
                }
                auto mdata = std::span(tmp2).first(n);
                for (int mode = 0; mode < NMode; mode++) {
                    switch (mode) {
                        case Copy:
                            for (isize i = 0; i < n; i++) mdata[i] = data[i];
                            break;
                        case Reverse:
                            for (isize i = 0; i < n; i++) mdata[i] = data[n - i - 1];
                            break;
                        case ReverseFirstHalf:
                            for (isize i = 0; i < n / 2; i++) mdata[i] = data[n / 2 - i - 1];
                            for (isize i = n / 2; i < n; i++) mdata[i] = data[i];
                            break;
                        case ReverseSecondHalf:
                            for (isize i = 0; i < n / 2; i++) mdata[i] = data[i];
                            for (isize i = n / 2; i < n; i++) mdata[i] = data[n - (i - n / 2) - 1];
                            break;
                        case Sorted:
                            for (isize i = 0; i < n; i++) mdata[i] = data[i];
                            sort::Ints(mdata);
                            break;
                        case Dither:
                            for (isize i = 0; i < n; i++) mdata[i] = data[i] + i % 5;
                            break;
                    }
                    testingData d;
                    d.desc = std::format("n={} m={} dist={} mode={}", n, m, dists[dist], modes[mode]);
                    d.data = mdata;
                    d.maxswap = maxswap(n);
                    sort_fn(d);
                    ASSERT_FALSE(d.failed);
                    ASSERT_TRUE(sort::IntsAreSorted(mdata)) << d.desc << ": ints not sorted";
                }
            }
        }
    }
}

}  // namespace

TEST(GoSort, SortBM) {
    testBentleyMcIlroy([](sort::Interface& d) { sort::Sort(d); }, [](isize n) { return n * lg(n) * 12 / 10; });
}

TEST(GoSort, HeapsortBM) {
    testBentleyMcIlroy(Heapsort, [](isize n) { return n * lg(n) * 12 / 10; });
}

TEST(GoSort, StableBM) {
    testBentleyMcIlroy([](sort::Interface& d) { sort::Stable(d); }, [](isize n) { return n * lg(n) * lg(n) / 3; });
}

namespace {
// Based on M. Douglas McIlroy's "antiquicksort".
struct adversaryTestingData : sort::Interface {
    std::vector<isize> data;  // item values, initialized to gas and changed by Less
    isize maxcmp;             // number of comparisons allowed
    isize ncmp = 0;           // number of comparisons (calls to Less)
    isize nsolid = 0;         // number of elements that have been set to non-gas values
    isize candidate = 0;      // guess at current pivot
    isize gas;                // special value for unset elements, higher than everything else
    bool failed = false;

    adversaryTestingData(isize size, isize maxcmp) : data(size, size - 1), maxcmp(maxcmp), gas(size - 1) {}

    isize Len() override { return std::ssize(data); }
    bool Less(isize i, isize j) override {
        if (ncmp >= maxcmp && !failed) {
            ADD_FAILURE() << std::format("used {} comparisons sorting adversary data with size {}", ncmp, data.size());
            failed = true;
        }
        ncmp++;
        if (data[i] == gas && data[j] == gas) {
            if (i == candidate) {
                data[i] = nsolid++;  // freeze i
            } else {
                data[j] = nsolid++;  // freeze j
            }
        }
        if (data[i] == gas) {
            candidate = i;
        } else if (data[j] == gas) {
            candidate = j;
        }
        return data[i] < data[j];
    }
    void Swap(isize i, isize j) override { std::swap(data[i], data[j]); }
};
}  // namespace

TEST(GoSort, Adversary) {
    const isize size = 10000;              // large enough to distinguish between O(n^2) and O(n*log(n))
    isize maxcmp = size * lg(size) * 4;    // the factor 4 was found by trial and error
    adversaryTestingData d(size, maxcmp);
    sort::Sort(d);  // This should degenerate to heapsort.
    for (isize i = 0; i < size; i++) {
        ASSERT_EQ(d.data[i], i) << "adversary data not fully sorted";
    }
}

TEST(GoSort, StableInts) {
    auto data = ints;
    sort::Stable(sort::IntSlice(data));
    EXPECT_TRUE(sort::IntsAreSorted(data));
}

namespace {
struct intPair {
    isize a, b;
};

// intPairs compare on a only.
struct intPairs : sort::Interface {
    std::vector<intPair> d;
    explicit intPairs(std::size_t n) : d(n) {}
    isize Len() override { return std::ssize(d); }
    bool Less(isize i, isize j) override { return d[i].a < d[j].a; }
    void Swap(isize i, isize j) override { std::swap(d[i], d[j]); }

    // Record initial order in b.
    void initB() {
        for (std::size_t i = 0; i < d.size(); i++) d[i].b = static_cast<isize>(i);
    }
    // Whether a-equal elements were not reordered.
    bool inOrder() const {
        isize lastA = -1, lastB = 0;
        for (auto& p : d) {
            if (lastA != p.a) {
                lastA = p.a;
                lastB = p.b;
                continue;
            }
            if (p.b <= lastB) return false;
            lastB = p.b;
        }
        return true;
    }
};
}  // namespace

TEST(GoSort, Stability) {
    isize n = 100000, m = 1000;
    intPairs data(n);

    // random distribution
    for (auto& p : data.d) p.a = rand_intn(m);
    ASSERT_FALSE(sort::IsSorted(data)) << "terrible rand.rand";
    data.initB();
    sort::Stable(data);
    EXPECT_TRUE(sort::IsSorted(data)) << "Stable didn't sort";
    EXPECT_TRUE(data.inOrder()) << "Stable wasn't stable";

    // already sorted
    data.initB();
    sort::Stable(data);
    EXPECT_TRUE(sort::IsSorted(data)) << "Stable shuffled sorted ints (order)";
    EXPECT_TRUE(data.inOrder()) << "Stable shuffled sorted ints (stability)";

    // sorted reversed
    for (std::size_t i = 0; i < data.d.size(); i++) data.d[i].a = n - static_cast<isize>(i);
    data.initB();
    sort::Stable(data);
    EXPECT_TRUE(sort::IsSorted(data)) << "Stable didn't sort";
    EXPECT_TRUE(data.inOrder()) << "Stable wasn't stable";
}

// ---------------------------------------------------------------------------
// search_test.go

namespace {

const std::vector<isize> data{-10, -5, 0, 1, 2, 3, 5, 7, 11, 100, 100, 100, 1000, 10000};

std::function<bool(isize)> f(const std::vector<isize>& a, isize x) {
    return [&a, x](isize i) { return a[i] >= x; };
}

}  // namespace

TEST(GoSort, Search) {
    struct {
        std::string name;
        isize n;
        std::function<bool(isize)> f;
        isize i;
    } tests[] = {
        {"empty", 0, nullptr, 0},
        {"1 1", 1, [](isize i) { return i >= 1; }, 1},
        {"1 true", 1, [](isize) { return true; }, 0},
        {"1 false", 1, [](isize) { return false; }, 1},
        {"1e9 991", 1000000000, [](isize i) { return i >= 991; }, 991},
        {"1e9 true", 1000000000, [](isize) { return true; }, 0},
        {"1e9 false", 1000000000, [](isize) { return false; }, 1000000000},
        {"data -20", std::ssize(data), f(data, -20), 0},
        {"data -10", std::ssize(data), f(data, -10), 0},
        {"data -9", std::ssize(data), f(data, -9), 1},
        {"data -6", std::ssize(data), f(data, -6), 1},
        {"data -5", std::ssize(data), f(data, -5), 1},
        {"data 3", std::ssize(data), f(data, 3), 5},
        {"data 11", std::ssize(data), f(data, 11), 8},
        {"data 99", std::ssize(data), f(data, 99), 9},
        {"data 100", std::ssize(data), f(data, 100), 9},
        {"data 101", std::ssize(data), f(data, 101), 12},
        {"data 10000", std::ssize(data), f(data, 10000), 13},
        {"data 10001", std::ssize(data), f(data, 10001), 14},
        {"descending a", 7, [](isize i) { return std::array<isize, 8>{99, 99, 59, 42, 7, 0, -1, -1}[i] <= 7; }, 4},
        {"descending 7", 1000000000, [](isize i) { return 1000000000 - i <= 7; }, 1000000000 - 7},
        {"overflow", 2000000000, [](isize) { return false; }, 2000000000},
    };
    for (auto& e : tests) {
        EXPECT_EQ(sort::Search(e.n, e.f), e.i) << e.name;
    }
}

TEST(GoSort, Find) {
    std::vector<std::string> str1{"foo"};
    std::vector<std::string> str2{"ab", "ca"};
    std::vector<std::string> str3{"mo", "qo", "vo"};
    std::vector<std::string> str4{"ab", "ad", "ca", "xy"};
    // slice with repeating elements
    std::vector<std::string> strRepeats{"ba", "ca", "da", "da", "da", "ka", "ma", "ma", "ta"};
    // slice with all element equal
    std::vector<std::string> strSame{"xx", "xx", "xx"};

    struct {
        std::vector<std::string> data;
        std::string target;
        isize wantPos;
        bool wantFound;
    } tests[] = {
        {{}, "foo", 0, false},
        {{}, "", 0, false},

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
        auto [pos, found] = sort::Find(std::ssize(tt.data), [&](isize i) { return cmp::Compare(tt.target, tt.data[i]); });
        EXPECT_EQ(pos, tt.wantPos) << tt.target;
        EXPECT_EQ(found, tt.wantFound) << tt.target;
    }
}

namespace {
// The binary logarithm of x, rounded up.
isize log2(isize x) {
    if (x < 1) return 0;
    return std::bit_width(static_cast<std::size_t>(x - 1));
}
}  // namespace

TEST(GoSort, SearchEfficiency) {
    isize n = 100, step = 1;
    for (int exp = 2; exp < 10; exp++) {
        // n == 10**exp, step == 10**(exp-2)
        isize max = log2(n);
        for (isize x = 0; x < n; x += step) {
            isize count = 0;
            isize i = sort::Search(n, [&](isize i) {
                count++;
                return i >= x;
            });
            EXPECT_EQ(i, x) << "n = " << n;
            EXPECT_LE(count, max) << "n = " << n << ", x = " << x;
        }
        n *= 10;
        step *= 10;
    }
}

TEST(GoSort, SearchWrappers) {
    std::vector<isize> d = data;
    std::vector<double> fdata{-3.14, 0, 1, 2, 1000.7};
    std::vector<string> sdata{"f", "foo", "foobar", "x"};
    struct {
        std::string name;
        isize result;
        isize i;
    } wrappertests[] = {
        {"SearchInts", sort::SearchInts(d, 11), 8},
        {"SearchFloat64s", sort::SearchFloat64s(fdata, 2.1), 4},
        {"SearchStrings", sort::SearchStrings(sdata, ""), 0},
        {"IntSlice.Search", sort::IntSlice(d).Search(0), 2},
        {"Float64Slice.Search", sort::Float64Slice(fdata).Search(2.0), 3},
        {"StringSlice.Search", sort::StringSlice(sdata).Search("x"), 3},
    };
    for (auto& e : wrappertests) EXPECT_EQ(e.result, e.i) << e.name;
}

// Abstract exhaustive test: all sizes up to 100, all possible return values.
TEST(GoSort, SearchExhaustive) {
    for (isize size = 0; size <= 100; size++) {
        for (isize targ = 0; targ <= size; targ++) {
            EXPECT_EQ(sort::Search(size, [&](isize i) { return i >= targ; }), targ) << size;
        }
    }
}

// Abstract exhaustive test for Find over 2,4...size*2.
TEST(GoSort, FindExhaustive) {
    for (isize size = 0; size <= 100; size++) {
        for (isize x = 1; x <= size * 2 + 1; x++) {
            auto [pos, found] = sort::Find(size, [&](isize i) { return x - (i + 1) * 2; });
            bool wantFound = x % 2 == 0;
            isize wantPos = wantFound ? x / 2 - 1 : x / 2;
            EXPECT_EQ(found, wantFound) << size << " " << x;
            EXPECT_EQ(pos, wantPos) << size << " " << x;
        }
    }
}
