// Ported from Go 1.27.3's maps/maps_test.go and maps/iter_test.go, with
// Go's test names. Left out: the benchmark, and the tests of Go's map
// internals (cloning mid-grow, nil maps).
#include <gopp/maps.hpp>
#include <gopp/slices.hpp>
#include <gopp/string.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <ranges>
#include <string>
#include <unordered_map>

using namespace gopp;

namespace {

using intmap = std::unordered_map<isize, isize>;

const intmap m1{{1, 2}, {2, 4}, {4, 8}, {8, 16}};
const std::unordered_map<isize, string> m2{{1, "2"}, {2, "4"}, {4, "8"}, {8, "16"}};

template <typename T>
bool equal(const T& v1, const T& v2) {
    return v1 == v2;
}

template <typename T>
bool equalNaN(const T& v1, const T& v2) {
    auto isNaN = [](const T& f) { return f != f; };
    return v1 == v2 || (isNaN(v1) && isNaN(v2));
}

bool equalIntStr(isize v1, const string& v2) { return std::to_string(v1) == v2.view(); }

}  // namespace

TEST(GoMaps, Equal) {
    EXPECT_TRUE(maps::Equal(m1, m1));
    EXPECT_FALSE(maps::Equal(m1, intmap()));
    EXPECT_FALSE(maps::Equal(intmap(), m1));
    EXPECT_TRUE(maps::Equal(intmap(), intmap()));
    EXPECT_FALSE(maps::Equal(m1, intmap{{1, 2}}));

    // Comparing NaN for equality is expected to fail.
    std::unordered_map<isize, double> mf{{1, 0}, {2, std::numeric_limits<double>::quiet_NaN()}};
    EXPECT_FALSE(maps::Equal(mf, mf));
}

TEST(GoMaps, EqualFunc) {
    EXPECT_TRUE(maps::EqualFunc(m1, m1, equal<isize>));
    EXPECT_FALSE(maps::EqualFunc(m1, intmap(), equal<isize>));
    EXPECT_FALSE(maps::EqualFunc(intmap(), m1, equal<isize>));
    EXPECT_TRUE(maps::EqualFunc(intmap(), intmap(), equal<isize>));
    EXPECT_FALSE(maps::EqualFunc(m1, intmap{{1, 2}}, equal<isize>));

    // Comparing NaN for equality is expected to fail.
    std::unordered_map<isize, double> mf{{1, 0}, {2, std::numeric_limits<double>::quiet_NaN()}};
    EXPECT_FALSE(maps::EqualFunc(mf, mf, equal<double>));
    // But it should succeed using equalNaN.
    EXPECT_TRUE(maps::EqualFunc(mf, mf, equalNaN<double>));

    EXPECT_TRUE(maps::EqualFunc(m1, m2, equalIntStr));
}

TEST(GoMaps, Clone) {
    auto mc = maps::Clone(m1);
    EXPECT_TRUE(maps::Equal(mc, m1));
    mc[16] = 32;
    EXPECT_FALSE(maps::Equal(mc, m1));
}

TEST(GoMaps, Copy) {
    auto mc = maps::Clone(m1);
    maps::Copy(mc, mc);
    EXPECT_TRUE(maps::Equal(mc, m1));
    maps::Copy(mc, intmap{{16, 32}});
    intmap want{{1, 2}, {2, 4}, {4, 8}, {8, 16}, {16, 32}};
    EXPECT_TRUE(maps::Equal(mc, want));

    // Different map types with the same keys and values.
    std::map<isize, bool> a;
    std::unordered_map<isize, bool> b{{1, true}};
    maps::Copy(a, b);
    EXPECT_TRUE(a.at(1));
}

TEST(GoMaps, DeleteFunc) {
    auto mc = maps::Clone(m1);
    maps::DeleteFunc(mc, [](isize, isize) { return false; });
    EXPECT_TRUE(maps::Equal(mc, m1));
    maps::DeleteFunc(mc, [](isize k, isize) { return k > 3; });
    EXPECT_TRUE(maps::Equal(mc, intmap{{1, 2}, {2, 4}}));
}

TEST(GoMaps, CloneWithDelete) {
    intmap m;
    for (isize i = 0; i < 32; i++) m[i] = i;
    for (isize i = 8; i < 32; i++) m.erase(i);
    auto m2 = maps::Clone(m);
    EXPECT_EQ(m2.size(), 8u);
    for (isize i = 0; i < 8; i++) EXPECT_EQ(m2[i], m[i]);
}

TEST(GoMaps, CloneWithMapAssign) {
    intmap m;
    const isize N = 25;
    for (isize i = 0; i < N; i++) m[i] = i;
    auto m2 = maps::Clone(m);
    EXPECT_EQ(m2.size(), static_cast<std::size_t>(N));
    for (isize i = 0; i < N; i++) EXPECT_EQ(m2[i], m[i]);
}

TEST(GoMaps, All) {
    for (isize size = 0; size < 10; size++) {
        intmap m;
        for (isize i = 0; i < size; i++) m[i] = i;
        isize cnt = 0;
        for (auto [i, v] : maps::All(m)) {
            auto it = m.find(i);
            EXPECT_TRUE(it != m.end() && v == it->second) << "at iteration " << cnt;
            cnt++;
        }
        EXPECT_EQ(cnt, size);
    }
}

TEST(GoMaps, Keys) {
    for (isize size = 0; size < 10; size++) {
        slice<isize> want;
        intmap m;
        for (isize i = 0; i < size; i++) {
            m[i] = i;
            want = append(want, i);
        }
        slice<isize> got;
        for (isize k : maps::Keys(m)) got = append(got, k);
        slices::Sort(got);
        EXPECT_TRUE(slices::Equal(got, want));
    }
}

TEST(GoMaps, Values) {
    for (isize size = 0; size < 10; size++) {
        slice<isize> want;
        intmap m;
        for (isize i = 0; i < size; i++) {
            m[i] = i;
            want = append(want, i);
        }
        slice<isize> got;
        for (isize v : maps::Values(m)) got = append(got, v);
        slices::Sort(got);
        EXPECT_TRUE(slices::Equal(got, want));
    }
}

TEST(GoMaps, Insert) {
    intmap got{{1, 1}, {2, 1}};
    maps::Insert(got, std::views::iota(isize(0), isize(5)) | std::views::transform([](isize i) {
                          return std::pair(2 * i, 2 * i + 1);
                      }));
    intmap want{{1, 1}, {2, 1}};
    for (auto [i, v] : intmap{{0, 1}, {2, 3}, {4, 5}, {6, 7}, {8, 9}}) want[i] = v;
    EXPECT_TRUE(maps::Equal(got, want));
}

TEST(GoMaps, Collect) {
    intmap m{{0, 1}, {2, 3}, {4, 5}, {6, 7}, {8, 9}};
    auto got = maps::Collect(maps::All(m));
    EXPECT_TRUE(maps::Equal(got, m));
}

TEST(GoMaps, SortedKeys) {
    std::unordered_map<string, isize> m{{"b", 2}, {"a", 1}, {"c", 3}};
    EXPECT_TRUE(slices::Equal(slices::Sorted(maps::Keys(m)), slice<string>{"a", "b", "c"}));
    auto byIndex = maps::Collect(slices::All(slice<string>{"x", "y"}));
    EXPECT_EQ(byIndex.at(1), "y");
}
