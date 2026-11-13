// Ported from Go 1.27.3's io/fs tests: format_test.go, fs_test.go,
// glob_test.go, readdir_test.go, readfile_test.go, readlink_test.go,
// stat_test.go, sub_test.go and walk_test.go, with Go's test names.
//
// Adapted: Go's glob tests glob the package's own directory (os.DirFS(".")
// and its glob.go); these glob a temporary directory that has one.
#include "go_os_test_util.hpp"

#include <gopp/io/fs.hpp>
#include <gopp/path.hpp>
#include <gopp/testing/fstest.hpp>

#include <gtest/gtest.h>

#include <limits>
#include <map>

using namespace gopp;

namespace {

// formatTest implements FileInfo to test FormatFileInfo, and implements
// DirEntry to test FormatDirEntry.
struct format_test final : fs::FileInfo, fs::DirEntry {
    format_test(string name, int64 size, fs::FileMode mode, time::Time mod_time, bool is_dir)
        : name_(std::move(name)), size_(size), mode_(mode), mod_time_(mod_time), is_dir_(is_dir) {}

    string Name() const override { return name_; }
    int64 Size() const override { return size_; }
    fs::FileMode Mode() const override { return mode_; }
    time::Time ModTime() const override { return mod_time_; }
    bool IsDir() const override { return is_dir_; }
    fs::FileMode Type() const override { return mode_.Type(); }
    std::pair<ptr<fs::FileInfo>, error> Info() const override { return {nullptr, nullptr}; }

    string name_;
    int64 size_;
    fs::FileMode mode_;
    time::Time mod_time_;
    bool is_dir_;
};

struct format_case {
    format_test input;
    std::string want_file_info;
    std::string want_dir_entry;
};

std::vector<format_case> format_tests() {
    time::Time noon = time::Time::Unix(12 * 3600, 0);  // 1970-01-01 12:00:00 UTC
    return {
        {{"hello.go", 100, 0644, noon, false}, "-rw-r--r-- 100 1970-01-01 12:00:00 hello.go", "- hello.go"},
        {{"home/gopher", 0, fs::ModeDir | 0755, noon, true},
         "drwxr-xr-x 0 1970-01-01 12:00:00 home/gopher/",
         "d home/gopher/"},
        {{"big", std::numeric_limits<int64>::max(), fs::ModeIrregular | 0644, noon, false},
         "?rw-r--r-- 9223372036854775807 1970-01-01 12:00:00 big",
         "? big"},
        {{"small", std::numeric_limits<int64>::min(), fs::ModeSocket | fs::ModeSetuid | 0644, noon, false},
         "Surw-r--r-- -9223372036854775808 1970-01-01 12:00:00 small",
         "S small"},
    };
}

}  // namespace

TEST(GoIoFs, FormatFileInfo) {
    int i = 0;
    for (auto& test : format_tests()) {
        EXPECT_EQ(fs::FormatFileInfo(test.input), test.want_file_info) << i;
        i++;
    }
}

TEST(GoIoFs, FormatDirEntry) {
    int i = 0;
    for (auto& test : format_tests()) {
        EXPECT_EQ(fs::FormatDirEntry(test.input), test.want_dir_entry) << i;
        i++;
    }
}

TEST(GoIoFs, ValidPath) {
    struct test {
        std::string name;
        bool ok;
    } tests[] = {
        {".", true},       {"x", true},       {"x/y", true},

        {"", false},       {"..", false},     {"/", false},       {"x/", false},     {"/x", false},
        {"x/y/", false},   {"/x/y", false},   {"./", false},      {"./x", false},    {"x/.", false},
        {"x/./y", false},  {"../", false},    {"../x", false},    {"x/..", false},   {"x/../y", false},
        {"x//y", false},   {"x\\", true},     {"x\\y", true},     {"x:y", true},     {"\\x", true},
    };
    for (auto& tt : tests) EXPECT_EQ(fs::ValidPath(tt.name), tt.ok) << "ValidPath(" << tt.name << ")";
}

TEST(GoIoFs, FileModeString) {
    // Not a Go test: FileMode.String of the examples in io/fs's docs.
    EXPECT_EQ(fs::FileMode(0644).String(), "-rw-r--r--");
    EXPECT_EQ((fs::ModeDir | 0755).String(), "drwxr-xr-x");
    EXPECT_EQ(fs::FileMode(fs::ModeSymlink | 0777).String(), "Lrwxrwxrwx");
    EXPECT_EQ(std::format("{}", fs::FileMode(0600)), "-rw-------");
}

// ---------------------------------------------------------------------------
// The functions over any FS.

namespace {

using os_test::TempDir;

int sysValue;

// readfile_test.go's testFsys.
ptr<fstest::MapFS> testFsys() {
    return fstest::MakeMapFS({
        {"hello.txt", {.Data = to_bytes("hello, world"), .Mode = 0456, .ModTime = time::Now(), .Sys = &sysValue}},
        {"sub/goodbye.txt",
         {.Data = to_bytes("goodbye, world"), .Mode = 0456, .ModTime = time::Now(), .Sys = &sysValue}},
    });
}

// Go's openOnly: an FS with only Open.
struct openOnly final : fs::FS {
    explicit openOnly(ptr<fs::FS> f_) : f(std::move(f_)) {}
    std::pair<ptr<fs::File>, error> Open(std::string_view name) override { return f->Open(name); }
    ptr<fs::FS> f;
};

// Go's readFileOnly, readDirOnly, statOnly, subOnly and globOnly: an FS
// with the one method, and an Open that fails.
struct readFileOnly final : fs::ReadFileFS {
    explicit readFileOnly(ptr<fstest::MapFS> f_) : f(std::move(f_)) {}
    std::pair<ptr<fs::File>, error> Open(std::string_view) override { return {nullptr, fs::ErrNotExist}; }
    std::pair<bytes, error> ReadFile(std::string_view name) override { return f->ReadFile(name); }
    ptr<fstest::MapFS> f;
};

struct readDirOnly final : fs::ReadDirFS {
    explicit readDirOnly(ptr<fstest::MapFS> f_) : f(std::move(f_)) {}
    std::pair<ptr<fs::File>, error> Open(std::string_view) override { return {nullptr, fs::ErrNotExist}; }
    std::pair<std::vector<ptr<fs::DirEntry>>, error> ReadDir(std::string_view name) override {
        return f->ReadDir(name);
    }
    ptr<fstest::MapFS> f;
};

struct statOnly final : fs::StatFS {
    explicit statOnly(ptr<fstest::MapFS> f_) : f(std::move(f_)) {}
    std::pair<ptr<fs::File>, error> Open(std::string_view) override { return {nullptr, fs::ErrNotExist}; }
    std::pair<ptr<fs::FileInfo>, error> Stat(std::string_view name) override { return f->Stat(name); }
    ptr<fstest::MapFS> f;
};

struct subOnly final : fs::SubFS {
    explicit subOnly(ptr<fstest::MapFS> f_) : f(std::move(f_)) {}
    std::pair<ptr<fs::File>, error> Open(std::string_view) override { return {nullptr, fs::ErrNotExist}; }
    std::pair<ptr<fs::FS>, error> Sub(std::string_view dir) override { return f->Sub(dir); }
    ptr<fstest::MapFS> f;
};

struct globOnly final : fs::GlobFS {
    explicit globOnly(ptr<fstest::MapFS> f_) : f(std::move(f_)) {}
    std::pair<ptr<fs::File>, error> Open(std::string_view) override { return {nullptr, fs::ErrNotExist}; }
    std::pair<std::vector<string>, error> Glob(std::string_view pattern) override { return f->Glob(pattern); }
    ptr<fstest::MapFS> f;
};

std::vector<std::string> names(const std::vector<ptr<fs::DirEntry>>& dirs) {
    std::vector<std::string> v;
    for (auto& d : dirs) v.emplace_back(d->Name().view());
    return v;
}

std::vector<std::string> strs(const std::vector<string>& in) {
    std::vector<std::string> v;
    for (auto& s : in) v.emplace_back(s.view());
    return v;
}

std::string errorPath(const error& err) {
    auto* perr = errors::As<fs::PathError>(err);
    return perr ? std::string(perr->Path.view()) : "";
}

// A directory like Go's io/fs, with a glob.go, in a temporary directory.
struct globDir {
    TempDir tmp;
    std::string fsdir = tmp.path() + "/fs";
    globDir() {
        EXPECT_FALSE(os::Mkdir(fsdir, 0755));
        EXPECT_FALSE(os::WriteFile(fsdir + "/glob.go", "package fs\n", 0644));
        EXPECT_FALSE(os::WriteFile(fsdir + "/glob_test.go", "package fs_test\n", 0644));
    }
};

}  // namespace

TEST(GoIoFs, Glob) {
    globDir d;
    struct test {
        ptr<fs::FS> fs;
        std::string pattern, result;
    } globTests[] = {
        {os::DirFS(d.fsdir), "glob.go", "glob.go"},
        {os::DirFS(d.fsdir), "gl?b.go", "glob.go"},
        {os::DirFS(d.fsdir), "gl\\ob.go", "glob.go"},
        {os::DirFS(d.fsdir), "*", "glob.go"},
        {os::DirFS(d.tmp.path()), "*/glob.go", "fs/glob.go"},
    };
    for (auto& tt : globTests) {
        auto [matches, err] = fs::Glob(*tt.fs, tt.pattern);
        if (err) {
            ADD_FAILURE() << "Glob error for " << tt.pattern << ": " << err;
            continue;
        }
        auto m = strs(matches);
        EXPECT_NE(std::ranges::find(m, tt.result), m.end()) << "Glob(" << tt.pattern << ") want " << tt.result;
    }
    for (std::string pattern : {"no_match", "../*/no_match", "\\*"}) {
        auto [matches, err] = fs::Glob(*os::DirFS(d.fsdir), pattern);
        if (err) {
            ADD_FAILURE() << "Glob error for " << pattern << ": " << err;
            continue;
        }
        EXPECT_TRUE(matches.empty()) << "Glob(" << pattern << ") want []";
    }
}

TEST(GoIoFs, GlobError) {
    globDir d;
    for (std::string pattern : {"[]", "nonexist/[]"}) {
        auto [m, err] = fs::Glob(*os::DirFS(d.fsdir), pattern);
        EXPECT_EQ(err, path::ErrBadPattern) << "Glob(fs, " << pattern << ") returned err=" << err;
    }
}

TEST(GoIoFs, CVE202230630) {
    // Prior to CVE-2022-30630, a stack exhaustion would occur given a large
    // number of separators. There is now a limit of 10,000.
    globDir d;
    auto [m, err] = fs::Glob(*os::DirFS(d.fsdir), "/*" + std::string(10001, '/'));
    EXPECT_EQ(err, path::ErrBadPattern) << "Glob returned err=" << err;
}

TEST(GoIoFs, GlobMethod) {
    auto check = [](std::string_view desc, const std::vector<string>& names, const error& err) {
        if (err || names.size() != 1 || names[0].view() != "hello.txt") {
            ADD_FAILURE() << "Glob(" << desc << ") = " << names.size() << " names, " << err << ", want [hello.txt], nil";
        }
    };

    // Test that ReadDir uses the method when present.
    globOnly g(testFsys());
    auto [names1, err1] = fs::Glob(g, "*.txt");
    check("readDirOnly", names1, err1);

    // Test that ReadDir uses Open when the method is not present.
    openOnly o(testFsys());
    auto [names2, err2] = fs::Glob(o, "*.txt");
    check("openOnly", names2, err2);
}

TEST(GoIoFs, ReadDir) {
    auto check = [](std::string_view desc, const std::vector<ptr<fs::DirEntry>>& dirs, const error& err) {
        if (err || dirs.size() != 2 || dirs[0]->Name().view() != "hello.txt" || dirs[1]->Name().view() != "sub") {
            ADD_FAILURE() << "ReadDir(" << desc << ") = " << testing::PrintToString(names(dirs)) << ", " << err
                          << ", want [hello.txt sub], nil";
        }
    };

    // Test that ReadDir uses the method when present.
    readDirOnly rd(testFsys());
    auto [dirs, err] = fs::ReadDir(rd, ".");
    check("readDirOnly", dirs, err);

    // Test that ReadDir uses Open when the method is not present.
    openOnly o(testFsys());
    std::tie(dirs, err) = fs::ReadDir(o, ".");
    check("openOnly", dirs, err);

    // Test that ReadDir on Sub of . works (sub_test checks non-trivial subs).
    auto [sub, serr] = fs::Sub(testFsys(), ".");
    ASSERT_FALSE(serr) << serr;
    std::tie(dirs, err) = fs::ReadDir(*sub, ".");
    check("sub(.)", dirs, err);
}

TEST(GoIoFs, FileInfoToDirEntry) {
    auto testFs = fstest::MakeMapFS({
        {"notadir.txt", {.Data = to_bytes("hello, world"), .Mode = 0, .ModTime = time::Now(), .Sys = &sysValue}},
        {"adir", {.Data = {}, .Mode = os::ModeDir, .ModTime = time::Now(), .Sys = &sysValue}},
    });

    struct test {
        std::string path;
        fs::FileMode wantMode;
        bool wantDir;
    } tests[] = {
        {.path = "notadir.txt", .wantMode = 0, .wantDir = false},
        {.path = "adir", .wantMode = os::ModeDir, .wantDir = true},
    };

    for (auto& test : tests) {
        SCOPED_TRACE(test.path);
        auto [fi, err] = fs::Stat(*testFs, test.path);
        ASSERT_FALSE(err) << err;

        auto dirEntry = fs::FileInfoToDirEntry(fi);
        EXPECT_EQ(dirEntry->Type(), test.wantMode) << "FileMode mismatch";
        EXPECT_EQ(dirEntry->Name().view(), test.path) << "Name mismatch";
        EXPECT_EQ(dirEntry->IsDir(), test.wantDir) << "IsDir mismatch";
    }
}

TEST(GoIoFs, ReadDirPath) {
    TempDir tmp;
    auto fsys = os::DirFS(tmp.path());
    auto [d1, err1] = fs::ReadDir(*fsys, "non-existent");
    openOnly o(fsys);
    auto [d2, err2] = fs::ReadDir(o, "non-existent");
    EXPECT_EQ(errorPath(err1), errorPath(err2));
}

TEST(GoIoFs, ReadFile) {
    // Test that ReadFile uses the method when present.
    readFileOnly rf(testFsys());
    auto [data, err] = fs::ReadFile(rf, "hello.txt");
    ASSERT_TRUE(as_string_view(data) == "hello, world" && !err)
        << "ReadFile(readFileOnly, \"hello.txt\") = " << as_string_view(data) << ", " << err;

    // Test that ReadFile uses Open when the method is not present.
    openOnly o(testFsys());
    std::tie(data, err) = fs::ReadFile(o, "hello.txt");
    ASSERT_TRUE(as_string_view(data) == "hello, world" && !err)
        << "ReadFile(openOnly, \"hello.txt\") = " << as_string_view(data) << ", " << err;

    // Test that ReadFile on Sub of . works (sub_test checks non-trivial subs).
    auto [sub, serr] = fs::Sub(testFsys(), ".");
    ASSERT_FALSE(serr) << serr;
    std::tie(data, err) = fs::ReadFile(*sub, "hello.txt");
    ASSERT_TRUE(as_string_view(data) == "hello, world" && !err)
        << "ReadFile(sub(.), \"hello.txt\") = " << as_string_view(data) << ", " << err;
}

TEST(GoIoFs, ReadFilePath) {
    TempDir tmp;
    auto fsys = os::DirFS(tmp.path());
    auto [d1, err1] = fs::ReadFile(*fsys, "non-existent");
    openOnly o(fsys);
    auto [d2, err2] = fs::ReadFile(o, "non-existent");
    EXPECT_EQ(errorPath(err1), errorPath(err2));
}

namespace {

ptr<fstest::MapFS> linkFS() {
    return fstest::MakeMapFS({
        {"foo", {.Data = to_bytes("bar"), .Mode = fs::ModeSymlink | 0777}},
        {"bar", {.Data = to_bytes("Hello, World!\n"), .Mode = 0644}},
        {"dir/parentlink", {.Data = to_bytes("../bar"), .Mode = fs::ModeSymlink | 0777}},
        {"dir/link", {.Data = to_bytes("file"), .Mode = fs::ModeSymlink | 0777}},
        {"dir/file", {.Data = to_bytes("Hello, World!\n"), .Mode = 0644}},
    });
}

}  // namespace

TEST(GoIoFs, ReadLink) {
    auto testFS = linkFS();
    auto check = [](fs::FS& fsys, std::string_view name, std::string_view want) {
        auto [got, err] = fs::ReadLink(fsys, name);
        if (got.view() != want || err) {
            ADD_FAILURE() << "ReadLink(" << name << ") = " << got.view() << ", " << err << "; want " << want << ", <nil>";
        }
    };

    check(*testFS, "foo", "bar");
    check(*testFS, "dir/parentlink", "../bar");
    check(*testFS, "dir/link", "file");

    // Test that ReadLink on Sub works.
    auto [sub, err] = fs::Sub(testFS, "dir");
    ASSERT_FALSE(err) << err;

    check(*sub, "link", "file");
    check(*sub, "parentlink", "../bar");
}

TEST(GoIoFs, Lstat) {
    auto testFS = linkFS();
    auto check = [](fs::FS& fsys, std::string_view name, fs::FileMode want) {
        auto [info, err] = fs::Lstat(fsys, name);
        fs::FileMode got;
        if (!err) got = info->Mode();
        if (got != want || err) {
            ADD_FAILURE() << "Lstat(" << name << ") = " << got << ", " << err << "; want " << want << ", <nil>";
        }
    };

    check(*testFS, "foo", fs::ModeSymlink | 0777);
    check(*testFS, "bar", 0644);

    // Test that Lstat on Sub works.
    auto [sub, err] = fs::Sub(testFS, "dir");
    ASSERT_FALSE(err) << err;
    check(*sub, "link", fs::ModeSymlink | 0777);
}

TEST(GoIoFs, Stat) {
    auto check = [](std::string_view desc, const ptr<fs::FileInfo>& info, const error& err) {
        ASSERT_TRUE(!err && info && info->Mode() == fs::FileMode(0456))
            << "Stat(" << desc << ") = " << (info ? info->Mode().String().view() : "<nil>") << ", " << err
            << ", want Mode:0456, nil";
    };

    // Test that Stat uses the method when present.
    statOnly so(testFsys());
    auto [info, err] = fs::Stat(so, "hello.txt");
    check("statOnly", info, err);

    // Test that Stat uses Open when the method is not present.
    openOnly o(testFsys());
    std::tie(info, err) = fs::Stat(o, "hello.txt");
    check("openOnly", info, err);
}

TEST(GoIoFs, Sub) {
    auto check = [](std::string_view desc, const ptr<fs::FS>& sub, const error& err) {
        if (err) {
            ADD_FAILURE() << "Sub(sub): " << err;
            return;
        }
        auto [data, rerr] = fs::ReadFile(*sub, "goodbye.txt");
        if (as_string_view(data) != "goodbye, world" || rerr) {
            ADD_FAILURE() << "ReadFile(" << desc << ", \"goodbye.txt\") = " << as_string_view(data) << ", " << rerr
                          << ", want goodbye, world, nil";
        }

        auto [dirs, derr] = fs::ReadDir(*sub, ".");
        if (derr || dirs.size() != 1 || dirs[0]->Name().view() != "goodbye.txt") {
            ADD_FAILURE() << "ReadDir(" << desc << ", \".\") = " << testing::PrintToString(names(dirs)) << ", " << derr
                          << ", want [goodbye.txt], nil";
        }
    };

    // Test that Sub uses the method when present.
    auto [sub, err] = fs::Sub(make_ptr<subOnly>(testFsys()), "sub");
    check("subOnly", sub, err);

    // Test that Sub uses Open when the method is not present.
    std::tie(sub, err) = fs::Sub(make_ptr<openOnly>(testFsys()), "sub");
    check("openOnly", sub, err);

    auto [f, oerr] = sub->Open("nonexist");
    ASSERT_TRUE(oerr) << "Open(nonexist): succeeded";
    auto* pe = dynamic_cast<const fs::PathError*>(oerr.get());
    ASSERT_TRUE(pe) << "Open(nonexist): error is " << typeid(*oerr.get()).name() << ", want *PathError";
    ASSERT_EQ(pe->Path.view(), "nonexist") << "Open(nonexist): err.Path";

    std::tie(f, oerr) = sub->Open("./");
    ASSERT_TRUE(errors::Is(oerr, fs::ErrInvalid)) << "Open(./): error is " << oerr << ", want " << fs::ErrInvalid;
}

namespace {

// walk_test.go's Node: entries is nullopt if the entry is a file.
struct Node {
    std::string name;
    std::optional<std::vector<Node>> entries;
    int mark = 0;
};

Node makeNodeTree() {
    using V = std::vector<Node>;
    return {"testdata",
            V{
                {"a", std::nullopt},
                {"b", V{}},
                {"c", std::nullopt},
                {"d",
                 V{
                     {"x", std::nullopt},
                     {"y", V{}},
                     {"z", V{{"u", std::nullopt}, {"v", std::nullopt}}},
                 }},
            }};
}

void walkTree(Node& n, const std::string& path, const std::function<void(const std::string&, Node&)>& f) {
    f(path, n);
    if (n.entries) {
        for (auto& e : *n.entries) walkTree(e, std::string(path::Join(path, e.name).view()), f);
    }
}

ptr<fs::FS> makeTree(Node& tree) {
    auto fsys = make_ptr<fstest::MapFS>();
    walkTree(tree, tree.name, [&](const std::string& path, Node& n) {
        if (!n.entries) (*fsys)[path] = fstest::MapFile{};
        else (*fsys)[path] = fstest::MapFile{.Mode = fs::ModeDir};
    });
    return fsys;
}

// Assumes that each node name is unique. Good enough for a test. If clear
// is true, any incoming error is cleared before return. The errors are
// always accumulated, though.
error mark(Node& tree, const ptr<fs::DirEntry>& entry, error err, std::vector<error>& errors, bool clear) {
    string name = entry->Name();
    walkTree(tree, tree.name, [&](const std::string&, Node& n) {
        if (n.name == name.view()) n.mark++;
    });
    if (err) {
        errors.push_back(err);
        if (clear) return nullptr;
        return err;
    }
    return nullptr;
}

}  // namespace

TEST(GoIoFs, WalkDir) {
    os_test::InTempDir in;
    Node tree = makeNodeTree();
    auto fsys = makeTree(tree);
    std::vector<error> errors;
    bool clear = true;
    auto markFn = [&](std::string_view, const ptr<fs::DirEntry>& entry, error err) -> error {
        return mark(tree, entry, err, errors, clear);
    };
    // Expect no errors.
    error err = fs::WalkDir(*fsys, ".", markFn);
    ASSERT_FALSE(err) << "no error expected, found: " << err;
    ASSERT_TRUE(errors.empty()) << "unexpected errors: " << errors.size();
    walkTree(tree, tree.name, [](const std::string& path, Node& n) {
        EXPECT_EQ(n.mark, 1) << "node " << path << " mark";
        n.mark = 0;
    });
}

TEST(GoIoFs, WalkDirSymlink) {
    auto fsys = fstest::MakeMapFS({
        {"link", {.Data = to_bytes("dir"), .Mode = fs::ModeSymlink}},
        {"dir/a", {}},
        {"dir/b/c", {}},
        {"dir/d", {.Data = to_bytes("b"), .Mode = fs::ModeSymlink}},
    });

    std::map<std::string, fs::FileMode> wantTypes = {
        {"link", fs::ModeDir}, {"link/a", 0}, {"link/b", fs::ModeDir}, {"link/b/c", 0}, {"link/d", fs::ModeSymlink},
    };
    std::map<std::string, int> marks;
    auto walkFn = [&](std::string_view path, const ptr<fs::DirEntry>& entry, error err) -> error {
        std::string p(path);
        marks[p]++;
        if (auto it = wantTypes.find(p); it == wantTypes.end()) {
            ADD_FAILURE() << "Unexpected path " << p << " in walk";
        } else if (auto got = entry->Type(); got != it->second) {
            ADD_FAILURE() << p << " entry type = " << got << "; want " << it->second;
        }
        if (err) ADD_FAILURE() << "Walking " << p << ": " << err;
        return nullptr;
    };

    // Expect no errors.
    error err = fs::WalkDir(*fsys, "link", walkFn);
    ASSERT_FALSE(err) << "no error expected, found: " << err;
    for (auto& [path, _] : wantTypes) EXPECT_EQ(marks[path], 1) << path << " visited";
}

TEST(GoIoFs, Issue51617) {
    if (os::Geteuid() == 0) GTEST_SKIP() << "root can read a directory with mode 0";
    TempDir dir;
    for (std::string sub : {"a", "a/bad", "a/next"}) ASSERT_FALSE(os::Mkdir(dir.path() + "/" + sub, 0755));
    std::string bad = dir.path() + "/a/bad";
    ASSERT_FALSE(os::Chmod(bad, 0));
    std::vector<std::string> saw;
    error err = fs::WalkDir(*os::DirFS(dir.path()), ".",
                            [&](std::string_view path, const ptr<fs::DirEntry>& d, error err) -> error {
                                if (err) return fs::SkipDir;
                                if (d->IsDir()) saw.emplace_back(path);
                                return nullptr;
                            });
    os::Chmod(bad, 0700);  // avoid errors on cleanup
    ASSERT_FALSE(err) << err;
    std::vector<std::string> want = {".", "a", "a/bad", "a/next"};
    EXPECT_EQ(saw, want);
}
