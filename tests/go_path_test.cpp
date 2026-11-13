// Ported from Go 1.27.3's path/path_test.go and path/match_test.go, with
// Go's test names and tables (the allocation tests and benchmarks left
// out).
#include <gopp/path.hpp>

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace gopp;

namespace {
std::string str(const string& s) { return std::string(s.view()); }
}  // namespace

TEST(GoPath, Clean) {
    struct {
        std::string path, result;
    } cleantests[] = {
        // Already clean
        {"", "."}, {"abc", "abc"}, {"abc/def", "abc/def"}, {"a/b/c", "a/b/c"}, {".", "."}, {"..", ".."},
        {"../..", "../.."}, {"../../abc", "../../abc"}, {"/abc", "/abc"}, {"/", "/"},
        // Remove trailing slash
        {"abc/", "abc"}, {"abc/def/", "abc/def"}, {"a/b/c/", "a/b/c"}, {"./", "."}, {"../", ".."},
        {"../../", "../.."}, {"/abc/", "/abc"},
        // Remove doubled slash
        {"abc//def//ghi", "abc/def/ghi"}, {"//abc", "/abc"}, {"///abc", "/abc"}, {"//abc//", "/abc"}, {"abc//", "abc"},
        // Remove . elements
        {"abc/./def", "abc/def"}, {"/./abc/def", "/abc/def"}, {"abc/.", "abc"},
        // Remove .. elements
        {"abc/def/ghi/../jkl", "abc/def/jkl"}, {"abc/def/../ghi/../jkl", "abc/jkl"}, {"abc/def/..", "abc"},
        {"abc/def/../..", "."}, {"/abc/def/../..", "/"}, {"abc/def/../../..", ".."}, {"/abc/def/../../..", "/"},
        {"abc/def/../../../ghi/jkl/../../../mno", "../../mno"},
        // Combinations
        {"abc/./../def", "def"}, {"abc//./../def", "def"}, {"abc/../../././../def", "../../def"},
    };
    for (auto& test : cleantests) {
        EXPECT_EQ(str(path::Clean(test.path)), test.result) << test.path;
        EXPECT_EQ(str(path::Clean(test.result)), test.result) << test.result;
    }
}

TEST(GoPath, Split) {
    struct {
        std::string path, dir, file;
    } splittests[] = {{"a/b", "a/", "b"}, {"a/b/", "a/b/", ""}, {"a/", "a/", ""}, {"a", "", "a"}, {"/", "/", ""}};
    for (auto& test : splittests) {
        auto [d, f] = path::Split(test.path);
        EXPECT_EQ(str(d), test.dir) << test.path;
        EXPECT_EQ(str(f), test.file) << test.path;
    }
}

TEST(GoPath, Join) {
    struct {
        std::vector<std::string_view> elem;
        std::string path;
    } jointests[] = {
        // zero parameters
        {{}, ""},
        // one parameter
        {{""}, ""}, {{"a"}, "a"},
        // two parameters
        {{"a", "b"}, "a/b"}, {{"a", ""}, "a"}, {{"", "b"}, "b"}, {{"/", "a"}, "/a"}, {{"/", ""}, "/"},
        {{"a/", "b"}, "a/b"}, {{"a/", ""}, "a"}, {{"", ""}, ""},
    };
    for (auto& test : jointests) EXPECT_EQ(str(path::Join(test.elem)), test.path);
    EXPECT_EQ(str(path::Join("a", "b/../c")), "a/c");
}

TEST(GoPath, Ext) {
    struct {
        std::string path, ext;
    } exttests[] = {{"path.go", ".go"}, {"path.pb.go", ".go"}, {"a.dir/b", ""}, {"a.dir/b.go", ".go"}, {"a.dir/", ""}};
    for (auto& test : exttests) EXPECT_EQ(str(path::Ext(test.path)), test.ext) << test.path;
}

TEST(GoPath, Base) {
    struct {
        std::string path, result;
    } basetests[] = {{"", "."},       {".", "."},           {"/.", "."},       {"/", "/"},
                     {"////", "/"},   {"x/", "x"},          {"abc", "abc"},    {"abc/def", "def"},
                     {"a/b/.x", ".x"}, {"a/b/c.", "c."},    {"a/b/c.x", "c.x"}};
    for (auto& test : basetests) EXPECT_EQ(str(path::Base(test.path)), test.result) << test.path;
}

TEST(GoPath, Dir) {
    struct {
        std::string path, result;
    } dirtests[] = {{"", "."},         {".", "."},         {"/.", "/"},          {"/", "/"},
                    {"////", "/"},     {"/foo", "/"},      {"x/", "x"},          {"abc", "."},
                    {"abc/def", "abc"}, {"abc////def", "abc"}, {"a/b/.x", "a/b"}, {"a/b/c.", "a/b"},
                    {"a/b/c.x", "a/b"}};
    for (auto& test : dirtests) EXPECT_EQ(str(path::Dir(test.path)), test.result) << test.path;
}

TEST(GoPath, IsAbs) {
    struct {
        std::string path;
        bool isAbs;
    } isAbsTests[] = {{"", false},        {"/", true}, {"/usr/bin/gcc", true}, {"..", false},
                      {"/a/../bb", true}, {".", false}, {"./", false},          {"lala", false}};
    for (auto& test : isAbsTests) EXPECT_EQ(path::IsAbs(test.path), test.isAbs) << test.path;
}

TEST(GoPath, Match) {
    struct {
        std::string pattern, s;
        bool match;
        bool err;
    } matchTests[] = {
        {"abc", "abc", true, false},
        {"*", "abc", true, false},
        {"*c", "abc", true, false},
        {"a*", "a", true, false},
        {"a*", "abc", true, false},
        {"a*", "ab/c", false, false},
        {"a*/b", "abc/b", true, false},
        {"a*/b", "a/c/b", false, false},
        {"a*b*c*d*e*/f", "axbxcxdxe/f", true, false},
        {"a*b*c*d*e*/f", "axbxcxdxexxx/f", true, false},
        {"a*b*c*d*e*/f", "axbxcxdxe/xxx/f", false, false},
        {"a*b*c*d*e*/f", "axbxcxdxexxx/fff", false, false},
        {"a*b?c*x", "abxbbxdbxebxczzx", true, false},
        {"a*b?c*x", "abxbbxdbxebxczzy", false, false},
        {"ab[c]", "abc", true, false},
        {"ab[b-d]", "abc", true, false},
        {"ab[e-g]", "abc", false, false},
        {"ab[^c]", "abc", false, false},
        {"ab[^b-d]", "abc", false, false},
        {"ab[^e-g]", "abc", true, false},
        {"a\\*b", "a*b", true, false},
        {"a\\*b", "ab", false, false},
        {"a?b", "a☺b", true, false},
        {"a[^a]b", "a☺b", true, false},
        {"a???b", "a☺b", false, false},
        {"a[^a][^a][^a]b", "a☺b", false, false},
        {"[a-ζ]*", "α", true, false},
        {"*[a-ζ]", "A", false, false},
        {"a?b", "a/b", false, false},
        {"a*b", "a/b", false, false},
        {"[\\]a]", "]", true, false},
        {"[\\-]", "-", true, false},
        {"[x\\-]", "x", true, false},
        {"[x\\-]", "-", true, false},
        {"[x\\-]", "z", false, false},
        {"[\\-x]", "x", true, false},
        {"[\\-x]", "-", true, false},
        {"[\\-x]", "a", false, false},
        {"[]a]", "]", false, true},
        {"[-]", "-", false, true},
        {"[x-]", "x", false, true},
        {"[x-]", "-", false, true},
        {"[x-]", "z", false, true},
        {"[-x]", "x", false, true},
        {"[-x]", "-", false, true},
        {"[-x]", "a", false, true},
        {"\\", "a", false, true},
        {"[a-b-c]", "a", false, true},
        {"[", "a", false, true},
        {"[^", "a", false, true},
        {"[^bc", "a", false, true},
        {"a[", "a", false, true},
        {"a[", "ab", false, true},
        {"a[", "x", false, true},
        {"a/b[", "x", false, true},
        {"*x", "xxx", true, false},
    };
    for (auto& tt : matchTests) {
        auto [ok, err] = path::Match(tt.pattern, tt.s);
        EXPECT_EQ(ok, tt.match) << tt.pattern << " " << tt.s;
        EXPECT_EQ(err, tt.err ? path::ErrBadPattern : error()) << tt.pattern << " " << tt.s;
    }
}
