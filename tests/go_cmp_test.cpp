// Ported from Go 1.27.3's cmp/cmpTest.go, with Go's test names and table
// (the examples as tests).
#include <gopp/cmp.hpp>
#include <gopp/slices.hpp>
#include <gopp/sort.hpp>
#include <gopp/string.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <format>
#include <limits>
#include <string>
#include <variant>
#include <vector>

using namespace gopp;

namespace {

const double inf = std::numeric_limits<double>::infinity();
const double nan_ = std::numeric_limits<double>::quiet_NaN();
const double negzero = std::copysign(0.0, -1.0);
const uintptr nonnilptr = reinterpret_cast<uintptr>(&negzero);
const uintptr nilptr = 0;

using value = std::variant<isize, string, double, uintptr>;

struct test {
    value x, y;
    int compare;
};

std::vector<test> tests() {
    return {
        {isize(1), isize(2), -1},
        {isize(1), isize(1), 0},
        {isize(2), isize(1), +1},
        {string("a"), string("aa"), -1},
        {string("a"), string("a"), 0},
        {string("aa"), string("a"), +1},
        {1.0, 1.1, -1},
        {1.1, 1.1, 0},
        {1.1, 1.0, +1},
        {inf, inf, 0},
        {-inf, -inf, 0},
        {-inf, 1.0, -1},
        {1.0, -inf, +1},
        {inf, 1.0, +1},
        {1.0, inf, -1},
        {nan_, nan_, 0},
        {0.0, nan_, +1},
        {nan_, 0.0, -1},
        {nan_, -inf, -1},
        {-inf, nan_, +1},
        {0.0, 0.0, 0},
        {negzero, negzero, 0},
        {negzero, 0.0, 0},
        {0.0, negzero, 0},
        {negzero, 1.0, -1},
        {negzero, -1.0, +1},
        {nilptr, nonnilptr, -1},
        {nonnilptr, nilptr, 1},
        {nonnilptr, nonnilptr, 0},
    };
}

}  // namespace

TEST(GoCmp, Less) {
    for (auto& t : tests()) {
        bool b = std::visit(
            [&](const auto& x) {
                using T = std::decay_t<decltype(x)>;
                return cmp::Less(x, std::get<T>(t.y));
            },
            t.x);
        EXPECT_EQ(b, t.compare < 0) << "case " << t.compare;
    }
}

TEST(GoCmp, Compare) {
    for (auto& t : tests()) {
        int c = std::visit(
            [&](const auto& x) {
                using T = std::decay_t<decltype(x)>;
                return cmp::Compare(x, std::get<T>(t.y));
            },
            t.x);
        EXPECT_EQ(c, t.compare);
    }
}

TEST(GoCmp, Sort) {
    // Our comparison functions are consistent with sort.Float64s.
    std::vector<double> input{1.0, 0.0, negzero, inf, -inf, nan_};
    sort::Float64s(input);
    for (std::size_t i = 0; i + 1 < input.size(); i++) {
        EXPECT_FALSE(cmp::Less(input[i + 1], input[i])) << "Less sort mismatch at " << i;
        EXPECT_LE(cmp::Compare(input[i], input[i + 1]), 0) << "Compare sort mismatch at " << i;
    }
}

TEST(GoCmp, Or) {
    EXPECT_EQ(cmp::Or(0), 0);
    EXPECT_EQ(cmp::Or(1), 1);
    EXPECT_EQ(cmp::Or(0, 2), 2);
    EXPECT_EQ(cmp::Or(3, 0), 3);
    EXPECT_EQ(cmp::Or(4, 5), 4);
    EXPECT_EQ(cmp::Or(0, 6, 7), 6);
}

TEST(GoCmp, ExampleOr) {
    string userInput1 = "";
    string userInput2 = "some text";
    EXPECT_EQ(cmp::Or(userInput1, "default"_s), "default");
    EXPECT_EQ(cmp::Or(userInput2, "default"_s), "some text");
    EXPECT_EQ(cmp::Or(userInput1, userInput2, "default"_s), "some text");
}

TEST(GoCmp, ExampleOr_sort) {
    struct Order {
        string Product;
        string Customer;
        double Price;
    };
    std::vector<Order> orders{
        {"foo", "alice", 1.00}, {"bar", "bob", 3.00}, {"baz", "carol", 4.00},
        {"foo", "alice", 2.00}, {"bar", "carol", 1.00}, {"foo", "bob", 4.00},
    };
    // Sort by customer first, product second, and last by higher price.
    slices::SortFunc(orders, [](const Order& a, const Order& b) {
        return cmp::Or(cmp::Compare(a.Customer, b.Customer), cmp::Compare(a.Product, b.Product),
                       cmp::Compare(b.Price, a.Price));
    });
    std::string out;
    for (auto& o : orders) out += std::format("{} {} {:.2f}\n", o.Product, o.Customer, o.Price);
    EXPECT_EQ(out,
              "foo alice 2.00\n"
              "foo alice 1.00\n"
              "bar bob 3.00\n"
              "foo bob 4.00\n"
              "bar carol 1.00\n"
              "baz carol 4.00\n");
}

TEST(GoCmp, ExampleLess) {
    EXPECT_TRUE(cmp::Less(1, 2));
    EXPECT_TRUE(cmp::Less("a"_s, "aa"_s));
    EXPECT_FALSE(cmp::Less(1.0, nan_));
    EXPECT_TRUE(cmp::Less(nan_, 1.0));
}

TEST(GoCmp, ExampleCompare) {
    EXPECT_EQ(cmp::Compare(1, 2), -1);
    EXPECT_EQ(cmp::Compare("a"_s, "aa"_s), -1);
    EXPECT_EQ(cmp::Compare(1.5, 1.5), 0);
    EXPECT_EQ(cmp::Compare(nan_, 1.0), -1);
}
