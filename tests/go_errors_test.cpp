// Ported from Go 1.27.3's errors/join_test.go, and the %w cases of
// fmt/errors_test.go (TestErrorf), with Go's test names.
#include <gopp/errors.hpp>
#include <gopp/fmt.hpp>

#include <gtest/gtest.h>

#include <vector>

using namespace gopp;

namespace {

std::vector<const error_interface*> unwrapList(const error& err) {
    std::vector<const error_interface*> v;
    for (const error& e : err->UnwrapList()) v.push_back(e.get());
    return v;
}

// Go's multiErr: an error with Unwrap() []error.
struct multiErr final : error_interface {
    explicit multiErr(std::vector<error> e) : errs(std::move(e)) {}
    string Error() const override { return string::from_static("multiError"); }
    std::span<const error> UnwrapList() const noexcept override { return errs; }
    std::vector<error> errs;
};

}  // namespace

TEST(GoErrors, JoinReturnsNil) {
    EXPECT_FALSE(errors::Join({})) << "errors.Join() want nil";
    EXPECT_FALSE(errors::Join({nullptr})) << "errors.Join(nil) want nil";
    EXPECT_FALSE(errors::Join({nullptr, nullptr})) << "errors.Join(nil, nil) want nil";
}

TEST(GoErrors, Join) {
    error err1 = errors::New("err1");
    error err2 = errors::New("err2");
    error merr = make_ptr<multiErr>(std::vector<error>{errors::New("err3")});
    struct test {
        std::vector<error> errs;
        std::vector<const error_interface*> want;
    } tests[] = {
        {{err1}, {err1.get()}},
        {{err1, err2}, {err1.get(), err2.get()}},
        {{err1, nullptr, err2}, {err1.get(), err2.get()}},
        {{merr}, {merr.get()}},
    };
    for (auto& test : tests) {
        auto got = unwrapList(errors::Join(test.errs));
        EXPECT_EQ(got, test.want);
    }
}

TEST(GoErrors, JoinErrorMethod) {
    error err1 = errors::New("err1");
    error err2 = errors::New("err2");
    struct test {
        std::vector<error> errs;
        std::string want;
    } tests[] = {
        {{err1}, "err1"},
        {{err1, err2}, "err1\nerr2"},
        {{err1, nullptr, err2}, "err1\nerr2"},
    };
    for (auto& test : tests) EXPECT_EQ(errors::Join(test.errs).Error().view(), test.want);
}

TEST(GoErrors, IsAsThroughJoin) {
    // Not a Go test of its own: errors.Is and As search the lists of
    // Unwrap() []error, depth first (errors/wrap_test.go's multi cases).
    error err1 = errors::New("1");
    error err2 = errors::New("2");
    error poser = errors::New("poser");
    error joined = errors::Join({err1, errors::Join({poser, err2})});
    EXPECT_TRUE(errors::Is(joined, err1));
    EXPECT_TRUE(errors::Is(joined, err2));
    EXPECT_TRUE(errors::Is(joined, poser));
    EXPECT_FALSE(errors::Is(joined, errors::New("1")));
    EXPECT_TRUE(errors::As<multiErr>(errors::Join({err1, make_ptr<multiErr>(std::vector<error>{err2})})));
}

TEST(GoFmt, Errorf) {
    // fmt/errors_test.go's TestErrorf: the %w cases.
    error wrapped = errors::New("inner error");
    struct test {
        error err;
        std::string wantText;
        std::vector<error> wantSplit;
        error wantUnwrap;
    } tests[] = {
        {fmt::Errorf("{}", fmt::W(wrapped)), "inner error", {}, wrapped},
        {fmt::Errorf("added context: {}", fmt::W(wrapped)), "added context: inner error", {}, wrapped},
        {fmt::Errorf("{} with added context", fmt::W(wrapped)), "inner error with added context", {}, wrapped},
        {fmt::Errorf("{} {}", fmt::W(wrapped), fmt::W(wrapped)), "inner error inner error", {wrapped, wrapped}, nullptr},
        {fmt::Errorf("{}", fmt::W(nullptr)), "<nil>", {}, nullptr},
        {fmt::Errorf("{}", 1), "1", {}, nullptr},
    };
    for (auto& tt : tests) {
        EXPECT_EQ(tt.err.Error().view(), tt.wantText);
        EXPECT_EQ(errors::Unwrap(tt.err), tt.wantUnwrap) << tt.wantText;
        std::vector<error> split(tt.err->UnwrapList().begin(), tt.err->UnwrapList().end());
        EXPECT_EQ(split, tt.wantSplit) << tt.wantText;
    }
    EXPECT_TRUE(errors::Is(fmt::Errorf("a: {}", fmt::W(fmt::Errorf("b: {}", fmt::W(wrapped)))), wrapped));
}
