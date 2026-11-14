// Ported from Go 1.27.3's path/filepath/path_test.go and match_test.go:
// the tests of what go++'s filepath adds to path, with Go's test names.
// The lexical functions are path's, which go_path_test.cpp tests. Left
// out: the Windows and Plan 9 tests, the benchmarks, TestWalkFileError
// (it replaces filepath's Lstat), TestWalk's PermErr subtest and
// TestGlobUNC.
#include <gopp/os.hpp>
#include <gopp/path/filepath.hpp>
#include <gopp/strings.hpp>

#include "go_os_test_util.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

namespace filepath_test {

using namespace gopp;
using namespace std::literals;
using os_test::Chdir;
using os_test::TempDir;

struct IsLocalTest {
    std::string path;
    bool isLocal;
};

struct LocalizeTest {
    std::string path;
    std::string want;
};

struct EvalSymlinksTest {
    // If dest is empty, the path is created; otherwise the dest is
    // symlinked to the path.
    std::string path, dest;
};

struct RelTests {
    std::string root, path, want;
};

struct MatchTest {
    std::string pattern, s;
    bool match;
    error err;
};

const std::vector<IsLocalTest> islocaltests = {
    {""s, false},
    {"."s, true},
    {".."s, false},
    {"../a"s, false},
    {"/"s, false},
    {"/a"s, false},
    {"/a/../.."s, false},
    {"a"s, true},
    {"a/../a"s, true},
    {"a/"s, true},
    {"a/."s, true},
    {"a/./b/./c"s, true},
    {"a/../b:/../../c"s, false},
};

const std::vector<LocalizeTest> localizetests = {
    {""s, ""s},
    {"."s, "."s},
    {".."s, ""s},
    {"a/.."s, ""s},
    {"/"s, ""s},
    {"/a"s, ""s},
    {"a\xff" "b"s, ""s},
    {"a/"s, ""s},
    {"a/./b"s, ""s},
    {"\x00"s, ""s},
    {"a"s, "a"s},
    {"a/b/c"s, "a/b/c"s},
};

const std::vector<LocalizeTest> unixlocalizetests = {
    {"#a"s, "#a"s},
    {"a\\b:c"s, "a\\b:c"s},
};

const std::vector<EvalSymlinksTest> EvalSymlinksTestDirs = {
    {"test"s, ""s},
    {"test/dir"s, ""s},
    {"test/dir/link3"s, "../../"s},
    {"test/link1"s, "../test"s},
    {"test/link2"s, "dir"s},
    {"test/linkabs"s, "/"s},
    {"test/link4"s, "../test2"s},
    {"test2"s, "test/dir"s},
    // Issue 23444.
    {"src"s, ""s},
    {"src/pool"s, ""s},
    {"src/pool/test"s, ""s},
    {"src/versions"s, ""s},
    {"src/versions/current"s, "../../version"s},
    {"src/versions/v1"s, ""s},
    {"src/versions/v1/modules"s, ""s},
    {"src/versions/v1/modules/test"s, "../../../pool/test"s},
    {"version"s, "src/versions/v1"s},
};

const std::vector<EvalSymlinksTest> EvalSymlinksTests = {
    {"test"s, "test"s},
    {"test/dir"s, "test/dir"s},
    {"test/dir/../.."s, "."s},
    {"test/link1"s, "test"s},
    {"test/link2"s, "test/dir"s},
    {"test/link1/dir"s, "test/dir"s},
    {"test/link2/.."s, "test"s},
    {"test/dir/link3"s, "."s},
    {"test/link2/link3/test"s, "test"s},
    {"test/linkabs"s, "/"s},
    {"test/link4/.."s, "test"s},
    {"src/versions/current/modules/test"s, "src/pool/test"s},
};

const std::vector<std::string> absTestDirs = {
    "a"s,
    "a/b"s,
    "a/b/c"s,
};

const std::vector<std::string> absTests = {
    "."s,
    "b"s,
    "b/"s,
    "../a"s,
    "../a/b"s,
    "../a/b/./c/../../.././a"s,
    "../a/b/./c/../../.././a/"s,
    "$"s,
    "$/."s,
    "$/a/../a/b"s,
    "$/a/b/c/../../.././a"s,
    "$/a/b/c/../../.././a/"s,
};

const std::vector<RelTests> reltests = {
    {"a/b"s, "a/b"s, "."s},
    {"a/b/."s, "a/b"s, "."s},
    {"a/b"s, "a/b/."s, "."s},
    {"./a/b"s, "a/b"s, "."s},
    {"a/b"s, "./a/b"s, "."s},
    {"ab/cd"s, "ab/cde"s, "../cde"s},
    {"ab/cd"s, "ab/c"s, "../c"s},
    {"a/b"s, "a/b/c/d"s, "c/d"s},
    {"a/b"s, "a/b/../c"s, "../c"s},
    {"a/b/../c"s, "a/b"s, "../b"s},
    {"a/b/c"s, "a/c/d"s, "../../c/d"s},
    {"a/b"s, "c/d"s, "../../c/d"s},
    {"a/b/c/d"s, "a/b"s, "../.."s},
    {"a/b/c/d"s, "a/b/"s, "../.."s},
    {"a/b/c/d/"s, "a/b"s, "../.."s},
    {"a/b/c/d/"s, "a/b/"s, "../.."s},
    {"../../a/b"s, "../../a/b/c/d"s, "c/d"s},
    {"/a/b"s, "/a/b"s, "."s},
    {"/a/b/."s, "/a/b"s, "."s},
    {"/a/b"s, "/a/b/."s, "."s},
    {"/ab/cd"s, "/ab/cde"s, "../cde"s},
    {"/ab/cd"s, "/ab/c"s, "../c"s},
    {"/a/b"s, "/a/b/c/d"s, "c/d"s},
    {"/a/b"s, "/a/b/../c"s, "../c"s},
    {"/a/b/../c"s, "/a/b"s, "../b"s},
    {"/a/b/c"s, "/a/c/d"s, "../../c/d"s},
    {"/a/b"s, "/c/d"s, "../../c/d"s},
    {"/a/b/c/d"s, "/a/b"s, "../.."s},
    {"/a/b/c/d"s, "/a/b/"s, "../.."s},
    {"/a/b/c/d/"s, "/a/b"s, "../.."s},
    {"/a/b/c/d/"s, "/a/b/"s, "../.."s},
    {"/../../a/b"s, "/../../a/b/c/d"s, "c/d"s},
    {"."s, "a/b"s, "a/b"s},
    {"."s, ".."s, ".."s},
    {""s, "../../."s, "../.."s},

    // can't do purely lexically
    {".."s, "."s, "err"s},
    {".."s, "a"s, "err"s},
    {"../.."s, ".."s, "err"s},
    {"a"s, "/a"s, "err"s},
    {"/a"s, "a"s, "err"s},
};

// A function: ErrBadPattern is initialized at run time.
const std::vector<MatchTest>& matchTests() {
    static const std::vector<MatchTest> tests = {
    {"abc"s, "abc"s, true, {}},
    {"*"s, "abc"s, true, {}},
    {"*c"s, "abc"s, true, {}},
    {"a*"s, "a"s, true, {}},
    {"a*"s, "abc"s, true, {}},
    {"a*"s, "ab/c"s, false, {}},
    {"a*/b"s, "abc/b"s, true, {}},
    {"a*/b"s, "a/c/b"s, false, {}},
    {"a*b*c*d*e*/f"s, "axbxcxdxe/f"s, true, {}},
    {"a*b*c*d*e*/f"s, "axbxcxdxexxx/f"s, true, {}},
    {"a*b*c*d*e*/f"s, "axbxcxdxe/xxx/f"s, false, {}},
    {"a*b*c*d*e*/f"s, "axbxcxdxexxx/fff"s, false, {}},
    {"a*b?c*x"s, "abxbbxdbxebxczzx"s, true, {}},
    {"a*b?c*x"s, "abxbbxdbxebxczzy"s, false, {}},
    {"ab[c]"s, "abc"s, true, {}},
    {"ab[b-d]"s, "abc"s, true, {}},
    {"ab[e-g]"s, "abc"s, false, {}},
    {"ab[^c]"s, "abc"s, false, {}},
    {"ab[^b-d]"s, "abc"s, false, {}},
    {"ab[^e-g]"s, "abc"s, true, {}},
    {"a\\*b"s, "a*b"s, true, {}},
    {"a\\*b"s, "ab"s, false, {}},
    {"a?b"s, "a☺b"s, true, {}},
    {"a[^a]b"s, "a☺b"s, true, {}},
    {"a???b"s, "a☺b"s, false, {}},
    {"a[^a][^a][^a]b"s, "a☺b"s, false, {}},
    {"[a-ζ]*"s, "α"s, true, {}},
    {"*[a-ζ]"s, "A"s, false, {}},
    {"a?b"s, "a/b"s, false, {}},
    {"a*b"s, "a/b"s, false, {}},
    {"[\\]a]"s, "]"s, true, {}},
    {"[\\-]"s, "-"s, true, {}},
    {"[x\\-]"s, "x"s, true, {}},
    {"[x\\-]"s, "-"s, true, {}},
    {"[x\\-]"s, "z"s, false, {}},
    {"[\\-x]"s, "x"s, true, {}},
    {"[\\-x]"s, "-"s, true, {}},
    {"[\\-x]"s, "a"s, false, {}},
    {"[]a]"s, "]"s, false, filepath::ErrBadPattern},
    {"[-]"s, "-"s, false, filepath::ErrBadPattern},
    {"[x-]"s, "x"s, false, filepath::ErrBadPattern},
    {"[x-]"s, "-"s, false, filepath::ErrBadPattern},
    {"[x-]"s, "z"s, false, filepath::ErrBadPattern},
    {"[-x]"s, "x"s, false, filepath::ErrBadPattern},
    {"[-x]"s, "-"s, false, filepath::ErrBadPattern},
    {"[-x]"s, "a"s, false, filepath::ErrBadPattern},
    {"\\"s, "a"s, false, filepath::ErrBadPattern},
    {"[a-b-c]"s, "a"s, false, filepath::ErrBadPattern},
    {"["s, "a"s, false, filepath::ErrBadPattern},
    {"[^"s, "a"s, false, filepath::ErrBadPattern},
    {"[^bc"s, "a"s, false, filepath::ErrBadPattern},
    {"a["s, "a"s, false, filepath::ErrBadPattern},
    {"a["s, "ab"s, false, filepath::ErrBadPattern},
    {"a["s, "x"s, false, filepath::ErrBadPattern},
    {"a/b["s, "x"s, false, filepath::ErrBadPattern},
    {"*x"s, "xxx"s, true, {}},
};
    return tests;
}

std::string str(const string& s) { return std::string(s.view()); }

TEST(GoFilepath, IsLocal) {
    for (const auto& test : islocaltests) EXPECT_EQ(filepath::IsLocal(test.path), test.isLocal) << "IsLocal(" << test.path << ")";
}

TEST(GoFilepath, Localize) {
    auto tests = localizetests;
    tests.insert(tests.end(), unixlocalizetests.begin(), unixlocalizetests.end());
    for (const auto& test : tests) {
        auto [got, err] = filepath::Localize(test.path);
        EXPECT_TRUE(got == test.want && (!err) == !test.want.empty()) << "Localize(" << test.path << ") = " << got << ", " << err;
    }
}

TEST(GoFilepath, FromAndToSlash) {
    for (auto s : {"", "/", "/a/b", "a//b"}) {
        EXPECT_EQ(filepath::ToSlash(s), s);
        EXPECT_EQ(filepath::FromSlash(s), s);
    }
}

TEST(GoFilepath, SplitList) {
    struct SplitListTest {
        std::string list;
        std::vector<std::string> result;
    };
    const std::vector<SplitListTest> splitlisttests = {
        {"", {}},
        {"a:b", {"a", "b"}},
        {":a:b", {"", "a", "b"}},
    };
    for (const auto& test : splitlisttests) {
        auto l = filepath::SplitList(test.list);
        std::vector<std::string> got;
        for (const auto& e : l) got.push_back(str(e));
        EXPECT_EQ(got, test.result) << "SplitList(" << test.list << ")";
        EXPECT_FALSE(l.is_nil()) << "SplitList(" << test.list << ") is nil";
    }
}

struct Node {
    std::string name;
    std::vector<Node>* entries;  // nullptr if the entry is a file
    int mark = 0;
};

// The tree of TestWalk; each node name is unique.
struct Tree {
    std::vector<Node> zEntries = {{"u", nullptr}, {"v", nullptr}};
    std::vector<Node> bEntries = {};
    std::vector<Node> yEntries = {};
    std::vector<Node> dEntries = {{"x", nullptr}, {"y", &yEntries}, {"z", &zEntries}};
    std::vector<Node> rootEntries = {{"a", nullptr}, {"b", &bEntries}, {"c", nullptr}, {"d", &dEntries}};
    Node root = {"testdata", &rootEntries};
};

void walkTree(Node& n, const std::string& path, const std::function<void(const std::string&, Node&)>& f) {
    f(path, n);
    if (!n.entries) return;
    for (auto& e : *n.entries) walkTree(e, str(filepath::Join(path, e.name)), f);
}

void makeTree(Tree& tree) {
    walkTree(tree.root, tree.root.name, [](const std::string& path, Node& n) {
        if (!n.entries) {
            auto [fd, err] = os::Create(path);
            if (err) {
                ADD_FAILURE() << "makeTree: " << err;
                return;
            }
            fd->Close();
        } else {
            os::Mkdir(path, 0770);
        }
    });
}

void checkMarks(Tree& tree, bool report) {
    walkTree(tree.root, tree.root.name, [&](const std::string& path, Node& n) {
        if (n.mark != 1 && report) ADD_FAILURE() << "node " << path << " mark = " << n.mark << "; expected 1";
        n.mark = 0;
    });
}

// Assumes that each node name is unique. Good enough for a test. If clear
// is true, any incoming error is cleared before return. The errors are
// always accumulated, though.
error mark(Tree& tree, const ptr<fs::DirEntry>& d, error err, std::vector<error>& errors, bool clear) {
    auto name = str(d->Name());
    walkTree(tree.root, tree.root.name, [&](const std::string&, Node& n) {
        if (n.name == name) n.mark++;
    });
    if (err) {
        errors.push_back(err);
        if (clear) return nullptr;
        return err;
    }
    return nullptr;
}

void testWalk(const std::function<error(std::string_view, const fs::WalkDirFunc&)>& walk) {
    TempDir dir;
    Chdir cd(dir.path());

    Tree tree;
    makeTree(tree);
    std::vector<error> errors;
    bool clear = true;
    fs::WalkDirFunc markFn = [&](std::string_view, const ptr<fs::DirEntry>& d, error err) {
        return mark(tree, d, err, errors, clear);
    };
    // Expect no errors.
    error err = walk(tree.root.name, markFn);
    ASSERT_FALSE(err) << "no error expected, found: " << err;
    ASSERT_TRUE(errors.empty()) << "unexpected errors";
    checkMarks(tree, true);
}

TEST(GoFilepath, Walk) {
    testWalk([](std::string_view root, const fs::WalkDirFunc& fn) {
        return filepath::Walk(root, [&](std::string_view path, const ptr<fs::FileInfo>& info, error err) {
            return fn(path, fs::FileInfoToDirEntry(info), err);
        });
    });
}

TEST(GoFilepath, WalkDir) { testWalk([](std::string_view root, const fs::WalkDirFunc& fn) { return filepath::WalkDir(root, fn); }); }

void touch(const std::string& name) {
    auto [f, err] = os::Create(name);
    ASSERT_FALSE(err) << err;
    ASSERT_FALSE(f->Close());
}

TEST(GoFilepath, WalkSkipDirOnFile) {
    TempDir dir;
    std::string td = dir.path();
    ASSERT_FALSE(os::MkdirAll(str(filepath::Join(td, "dir")), 0755));
    touch(str(filepath::Join(td, "dir/foo1")));
    touch(str(filepath::Join(td, "dir/foo2")));

    bool sawFoo2 = false;
    auto walker = [&](std::string_view path) -> error {
        if (strings::HasSuffix(path, "foo2")) sawFoo2 = true;
        if (strings::HasSuffix(path, "foo1")) return filepath::SkipDir;
        return nullptr;
    };
    auto check = [&](const std::function<error()>& walk) {
        sawFoo2 = false;
        ASSERT_FALSE(walk());
        EXPECT_FALSE(sawFoo2) << "SkipDir on file foo1 did not block processing of foo2";
    };
    // Walk
    check([&] { return filepath::Walk(td, [&](std::string_view path, const ptr<fs::FileInfo>&, error) { return walker(path); }); });
    // WalkDir
    check([&] { return filepath::WalkDir(td, [&](std::string_view path, const ptr<fs::DirEntry>&, error) { return walker(path); }); });
}

TEST(GoFilepath, WalkSkipAllOnFile) {
    TempDir dir;
    std::string td = dir.path();
    ASSERT_FALSE(os::MkdirAll(str(filepath::Join(td, "dir", "subdir")), 0755));
    ASSERT_FALSE(os::MkdirAll(str(filepath::Join(td, "dir2")), 0755));
    touch(str(filepath::Join(td, "dir", "foo1")));
    touch(str(filepath::Join(td, "dir", "foo2")));
    touch(str(filepath::Join(td, "dir", "subdir", "foo3")));
    touch(str(filepath::Join(td, "dir", "foo4")));
    touch(str(filepath::Join(td, "dir2", "bar")));
    touch(str(filepath::Join(td, "last")));

    bool remainingWereSkipped = true;
    auto walker = [&](std::string_view path) -> error {
        if (strings::HasSuffix(path, "foo2")) return filepath::SkipAll;
        if (strings::HasSuffix(path, "foo3") || strings::HasSuffix(path, "foo4") || strings::HasSuffix(path, "bar") ||
            strings::HasSuffix(path, "last")) {
            remainingWereSkipped = false;
        }
        return nullptr;
    };
    auto check = [&](const std::function<error()>& walk) {
        remainingWereSkipped = true;
        ASSERT_FALSE(walk());
        EXPECT_TRUE(remainingWereSkipped) << "SkipAll on file foo2 did not block processing of remaining files and directories";
    };
    check([&] { return filepath::Walk(td, [&](std::string_view path, const ptr<fs::FileInfo>&, error) { return walker(path); }); });
    check([&] { return filepath::WalkDir(td, [&](std::string_view path, const ptr<fs::DirEntry>&, error) { return walker(path); }); });
}

TEST(GoFilepath, WalkSymlinkRoot) {
    TempDir dir;
    std::string td = dir.path();
    std::string d = str(filepath::Join(td, "dir"));
    ASSERT_FALSE(os::MkdirAll(d, 0755));
    touch(str(filepath::Join(d, "foo")));

    std::string link = str(filepath::Join(td, "link"));
    ASSERT_FALSE(os::Symlink("dir", link));
    std::string abslink = str(filepath::Join(td, "abslink"));
    ASSERT_FALSE(os::Symlink(d, abslink));
    std::string linklink = str(filepath::Join(td, "linklink"));
    ASSERT_FALSE(os::Symlink("link", linklink));

    // Since Walk does not traverse symlinks itself, its behavior should
    // depend on whether the path passed to Walk ends in a slash: if it does
    // not end in a slash, Walk should report the symlink itself (since it
    // is the last pathname component); but if it does end in a slash, Walk
    // should walk the directory to which the symlink refers (since it must
    // be fully resolved before walking).
    struct T {
        std::string desc, root;
        std::vector<std::string> want;
    };
    const std::vector<T> tests = {
        {"no slash", link, {link}},
        {"slash", link + "/", {link, str(filepath::Join(link, "foo"))}},
        {"abs no slash", abslink, {abslink}},
        {"abs with slash", abslink + "/", {abslink, str(filepath::Join(abslink, "foo"))}},
        {"double link no slash", linklink, {linklink}},
        {"double link with slash", linklink + "/", {linklink, str(filepath::Join(linklink, "foo"))}},
    };
    for (const auto& tt : tests) {
        std::vector<std::string> walked;
        error err = filepath::Walk(tt.root, [&](std::string_view path, const ptr<fs::FileInfo>&, error err) {
            if (err) return err;
            walked.push_back(str(filepath::Clean(path)));
            return error();
        });
        ASSERT_FALSE(err) << tt.desc << ": " << err;
        EXPECT_EQ(walked, tt.want) << tt.desc << ": Walk(" << tt.root << ")";
    }
}

// simpleJoin builds a file name from the directory and path. It does not
// use Join because we don't want ".." to be evaluated.
std::string simpleJoin(const std::string& dir, const std::string& path) { return dir + filepath::Separator + path; }

void testEvalSymlinks(const std::string& path, const std::string& want) {
    auto [have, err] = filepath::EvalSymlinks(path);
    if (err) {
        ADD_FAILURE() << "EvalSymlinks(" << path << ") error: " << err;
        return;
    }
    EXPECT_EQ(filepath::Clean(have), filepath::Clean(want)) << "EvalSymlinks(" << path << ")";
}

void testEvalSymlinksAfterChdir(const std::string& wd, const std::string& path, const std::string& want) {
    Chdir cd(wd);
    auto [have, err] = filepath::EvalSymlinks(path);
    if (err) {
        ADD_FAILURE() << "EvalSymlinks(" << path << ") in " << wd << " directory error: " << err;
        return;
    }
    EXPECT_EQ(filepath::Clean(have), filepath::Clean(want)) << "EvalSymlinks(" << path << ") in " << wd << " directory";
}

TEST(GoFilepath, EvalSymlinks) {
    TempDir dir;

    // /tmp may itself be a symlink! Avoid the confusion, although it means
    // trusting the thing we're testing.
    auto [tmp, terr] = filepath::EvalSymlinks(dir.path());
    ASSERT_FALSE(terr) << "eval symlink for tmp dir: " << terr;
    std::string tmpDir = str(tmp);

    // Create the symlink farm using relative paths.
    for (const auto& d : EvalSymlinksTestDirs) {
        auto path = simpleJoin(tmpDir, d.path);
        error err = d.dest.empty() ? os::Mkdir(path, 0755) : os::Symlink(d.dest, path);
        ASSERT_FALSE(err) << err;
    }

    // Evaluate the symlink farm.
    for (const auto& test : EvalSymlinksTests) {
        auto path = simpleJoin(tmpDir, test.path);

        auto dest = simpleJoin(tmpDir, test.dest);
        if (filepath::IsAbs(test.dest) || test.dest[0] == filepath::Separator) dest = test.dest;
        testEvalSymlinks(path, dest);

        // test EvalSymlinks(".")
        testEvalSymlinksAfterChdir(path, ".", ".");

        // test EvalSymlinks(".."+path)
        auto dotdotPath = simpleJoin("..", test.dest);
        if (filepath::IsAbs(test.dest) || test.dest[0] == filepath::Separator) dotdotPath = test.dest;
        testEvalSymlinksAfterChdir(simpleJoin(tmpDir, "test"), simpleJoin("..", test.path), dotdotPath);

        // test EvalSymlinks(p) where p is relative path
        testEvalSymlinksAfterChdir(tmpDir, test.path, test.dest);
    }
}

TEST(GoFilepath, EvalSymlinksIsNotExist) {
    TempDir dir;
    Chdir cd(dir.path());

    auto [_, err] = filepath::EvalSymlinks("notexist");
    EXPECT_TRUE(os::IsNotExist(err)) << "expected the file is not found, got " << err;

    ASSERT_FALSE(os::Symlink("notexist", "link"));
    std::tie(_, err) = filepath::EvalSymlinks("link");
    EXPECT_TRUE(os::IsNotExist(err)) << "expected the file is not found, got " << err;
    os::Remove("link");
}

TEST(GoFilepath, Issue13582) {
    TempDir dir;
    std::string tmpDir = dir.path();

    auto d = str(filepath::Join(tmpDir, "dir"));
    ASSERT_FALSE(os::Mkdir(d, 0755));
    auto linkToDir = str(filepath::Join(tmpDir, "link_to_dir"));
    ASSERT_FALSE(os::Symlink(d, linkToDir));
    auto file = str(filepath::Join(linkToDir, "file"));
    auto [f, cerr] = os::Create(file);
    ASSERT_FALSE(cerr);
    f->Close();
    auto link1 = str(filepath::Join(linkToDir, "link1"));
    ASSERT_FALSE(os::Symlink(file, link1));
    auto link2 = str(filepath::Join(linkToDir, "link2"));
    ASSERT_FALSE(os::Symlink(link1, link2));

    // /tmp may itself be a symlink!
    auto [realTmpDir, rerr] = filepath::EvalSymlinks(tmpDir);
    ASSERT_FALSE(rerr);
    auto realDir = str(filepath::Join(realTmpDir, "dir"));
    auto realFile = str(filepath::Join(realDir, "file"));

    struct T {
        std::string path, want;
    };
    for (const auto& test : {T{d, realDir}, T{linkToDir, realDir}, T{file, realFile}, T{link1, realFile}, T{link2, realFile}}) {
        auto [have, err] = filepath::EvalSymlinks(test.path);
        ASSERT_FALSE(err) << err;
        EXPECT_EQ(have, test.want) << "EvalSymlinks(" << test.path << ")";
    }
}

TEST(GoFilepath, RelativeSymlinkToAbsolute) {
    TempDir dir;
    std::string tmpDir = dir.path();
    Chdir cd(tmpDir);

    // Create "link" in the current working directory as a symlink to an
    // arbitrary absolute path.
    ASSERT_FALSE(os::Symlink(tmpDir, "link"));
    auto [p, err] = filepath::EvalSymlinks("link");
    ASSERT_FALSE(err) << "EvalSymlinks(\"link\"): " << err;
    auto [want, werr] = filepath::EvalSymlinks(tmpDir);
    ASSERT_FALSE(werr);
    EXPECT_EQ(p, want) << "EvalSymlinks(\"link\")";
}

TEST(GoFilepath, EvalSymlinksTooManyLinks) {
    TempDir dir;
    auto d = str(filepath::Join(dir.path(), "dir"));
    ASSERT_FALSE(os::Symlink(d, d));
    auto [_, err] = filepath::EvalSymlinks(d);
    EXPECT_TRUE(err) << "expected error, got nil";
}

TEST(GoFilepath, Abs) {
    TempDir dir;
    std::string root = dir.path();
    Chdir cd(root);
    for (const auto& d : absTestDirs) ASSERT_FALSE(os::Mkdir(d, 0777)) << "Mkdir failed";
    ASSERT_FALSE(os::Chdir(absTestDirs[0])) << "chdir failed";
    for (const auto& p : absTests) {
        auto path = str(strings::ReplaceAll(p, "$", root));
        auto [info, err] = os::Stat(path);
        if (err) {
            ADD_FAILURE() << path << ": " << err;
            continue;
        }
        auto [abspath, aerr] = filepath::Abs(path);
        if (aerr) {
            ADD_FAILURE() << "Abs(" << path << ") error: " << aerr;
            continue;
        }
        auto [absinfo, serr] = os::Stat(abspath);
        EXPECT_TRUE(!serr && os::SameFile(absinfo, info)) << "Abs(" << path << ")=" << abspath << ", not the same file";
        EXPECT_TRUE(filepath::IsAbs(abspath)) << "Abs(" << path << ")=" << abspath << ", not an absolute path";
        EXPECT_EQ(abspath, filepath::Clean(abspath)) << "Abs(" << path << ")=" << abspath << ", isn't clean";
    }
}

// Empty path needs to be special-cased on Windows. See
// golang.org/issue/24441.
TEST(GoFilepath, AbsEmptyString) {
    TempDir dir;
    std::string root = dir.path();
    Chdir cd(root);
    auto [info, err] = os::Stat(root);
    ASSERT_FALSE(err);
    auto [abspath, aerr] = filepath::Abs("");
    ASSERT_FALSE(aerr) << "Abs(\"\") error";
    auto [absinfo, serr] = os::Stat(abspath);
    EXPECT_TRUE(!serr && os::SameFile(absinfo, info)) << "Abs(\"\")=" << abspath << ", not the same file";
    EXPECT_TRUE(filepath::IsAbs(abspath));
    EXPECT_EQ(abspath, filepath::Clean(abspath));
}

TEST(GoFilepath, Rel) {
    for (const auto& test : reltests) {
        auto [got, err] = filepath::Rel(test.root, test.path);
        if (test.want == "err") {
            EXPECT_TRUE(err) << "Rel(" << test.root << ", " << test.path << ")=" << got << ", want error";
            continue;
        }
        EXPECT_FALSE(err) << "Rel(" << test.root << ", " << test.path << "): want " << test.want << ", got error: " << err;
        EXPECT_EQ(got, test.want) << "Rel(" << test.root << ", " << test.path << ")";
    }
}

TEST(GoFilepath, VolumeName) {
    EXPECT_EQ(filepath::VolumeName("/a/b"), "");
    EXPECT_EQ(filepath::VolumeName("c:/a"), "");
}

TEST(GoFilepath, Match) {
    for (const auto& tt : matchTests()) {
        auto [ok, err] = filepath::Match(tt.pattern, tt.s);
        EXPECT_TRUE(ok == tt.match && err == tt.err) << "Match(" << tt.pattern << ", " << tt.s << ") = " << ok << ", " << err;
    }
}

// Go's TestGlob runs in path/filepath, among Go's sources; this makes the
// same layout in a temporary directory.
TEST(GoFilepath, Glob) {
    TempDir dir;
    ASSERT_FALSE(os::Mkdir(str(filepath::Join(dir.path(), "filepath")), 0755));
    touch(str(filepath::Join(dir.path(), "filepath", "match.go")));
    Chdir cd(str(filepath::Join(dir.path(), "filepath")));

    struct T {
        std::string pattern, result;
    };
    const std::vector<T> globTests = {
        {"match.go", "match.go"},
        {"mat?h.go", "match.go"},
        {"*", "match.go"},
        {"../*/match.go", "../filepath/match.go"},
    };
    for (const auto& tt : globTests) {
        auto [matches, err] = filepath::Glob(tt.pattern);
        if (err) {
            ADD_FAILURE() << "Glob error for " << tt.pattern << ": " << err;
            continue;
        }
        EXPECT_TRUE(std::find(matches.begin(), matches.end(), tt.result) != matches.end()) << "Glob(" << tt.pattern << ")";
    }
    for (auto pattern : {"no_match", "../*/no_match"}) {
        auto [matches, err] = filepath::Glob(pattern);
        EXPECT_FALSE(err) << "Glob error for " << pattern;
        EXPECT_TRUE(matches.empty()) << "Glob(" << pattern << ") want []";
    }

    // TestNonWindowsGlobEscape
    auto [matches, err] = filepath::Glob("\\match.go");
    ASSERT_FALSE(err);
    EXPECT_EQ(matches, std::vector<string>{"match.go"});
}

TEST(GoFilepath, CVE202230632) {
    // Prior to CVE-2022-30632, this would cause a stack exhaustion given a
    // large number of separators (more than 4,000,000). There is now a
    // limit of 10,000.
    auto [_, err] = filepath::Glob("/*" + std::string(10001, '/'));
    EXPECT_EQ(err, filepath::ErrBadPattern);
}

TEST(GoFilepath, GlobError) {
    for (auto pattern : {"[]", "nonexist/[]"}) {
        auto [_, err] = filepath::Glob(pattern);
        EXPECT_EQ(err, filepath::ErrBadPattern) << "Glob(" << pattern << ")";
    }
}

TEST(GoFilepath, GlobSymlink) {
    TempDir dir;
    struct T {
        std::string path, dest;
        bool brokenLink;
    };
    for (const auto& tt : {T{"test1", "link1", false}, T{"test2", "link2", true}}) {
        auto path = str(filepath::Join(dir.path(), tt.path));
        auto dest = str(filepath::Join(dir.path(), tt.dest));
        touch(path);
        ASSERT_FALSE(os::Symlink(path, dest));
        if (tt.brokenLink) os::Remove(path);  // Break the symlink.
        auto [matches, err] = filepath::Glob(dest);
        EXPECT_FALSE(err) << "GlobSymlink error for " << dest;
        EXPECT_TRUE(std::find(matches.begin(), matches.end(), dest) != matches.end()) << "Glob(" << dest << ")";
    }
}

}  // namespace filepath_test
