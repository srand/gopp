// Ported from Go 1.27.3's testing/fstest tests: mapfs_test.go and
// testfs_test.go, with Go's test names.
#include "go_os_test_util.hpp"

#include <gopp/fmt.hpp>
#include <gopp/io/fs.hpp>
#include <gopp/testing/fstest.hpp>

#include <algorithm>

using namespace gopp;

TEST(GoTestingFstest, MapFS) {
    auto m = fstest::MakeMapFS({
        {"hello", {.Data = to_bytes("hello, world\n")}},
        {"fortune/k/ken.txt", {.Data = to_bytes("If a program is too slow, it must have a loop.\n")}},
    });
    if (error err = fstest::TestFS(m, {"hello", "fortune", "fortune/k", "fortune/k/ken.txt"})) FAIL() << err;
}

TEST(GoTestingFstest, MapFSChmodDot) {
    auto m = fstest::MakeMapFS({
        {"a/b.txt", {.Mode = 0666}},
        {".", {.Mode = 0777 | fs::ModeDir}},
    });
    std::string buf;
    fs::WalkDir(*m, ".", [&](std::string_view path, const ptr<fs::DirEntry>& d, error) -> error {
        auto [fi, err] = d->Info();
        if (err) return err;
        buf += std::format("{}: {}\n", path, fi->Mode());
        return nullptr;
    });
    std::string want = ".: drwxrwxrwx\na: dr-xr-xr-x\na/b.txt: -rw-rw-rw-\n";
    EXPECT_EQ(buf, want) << "MapFS modes";
}

TEST(GoTestingFstest, MapFSFileInfoName) {
    auto m = fstest::MakeMapFS({{"path/to/b.txt", {}}});
    auto [info, err] = m->Stat("path/to/b.txt");
    ASSERT_FALSE(err) << err;
    EXPECT_EQ(info->Name().view(), "b.txt") << "MapFS FileInfo.Name";
}

TEST(GoTestingFstest, MapFSSymlink) {
    const std::string fileContent = "If a program is too slow, it must have a loop.\n";
    auto m = fstest::MakeMapFS({
        {"fortune/k/ken.txt", {.Data = to_bytes(fileContent)}},
        {"dirlink", {.Data = to_bytes("fortune/k"), .Mode = fs::ModeSymlink}},
        {"linklink", {.Data = to_bytes("dirlink"), .Mode = fs::ModeSymlink}},
        {"ken.txt", {.Data = to_bytes("dirlink/ken.txt"), .Mode = fs::ModeSymlink}},
    });
    if (error err = fstest::TestFS(m, {"fortune/k/ken.txt", "dirlink", "ken.txt", "linklink"})) ADD_FAILURE() << err;

    auto [gotData, err] = fs::ReadFile(*m, "ken.txt");
    EXPECT_TRUE(as_string_view(gotData) == fileContent && !err)
        << "fs.ReadFile(m, \"ken.txt\") = " << as_string_view(gotData) << ", " << err;
    auto [gotLink, lerr] = fs::ReadLink(*m, "dirlink");
    EXPECT_TRUE(gotLink.view() == "fortune/k" && !lerr)
        << "fs.ReadLink(m, \"dirlink\") = " << gotLink.view() << ", " << lerr;

    auto check = [](const ptr<fs::FileInfo>& info, const error& err, std::string_view desc, std::string_view name,
                    fs::FileMode mode) {
        if (err) {
            ADD_FAILURE() << desc << " = _, " << err << "; want _, <nil>";
            return;
        }
        EXPECT_EQ(info->Name().view(), name) << desc << ".Name()";
        EXPECT_EQ(info->Mode(), mode) << desc << ".Mode()";
    };
    auto [i1, e1] = fs::Lstat(*m, "dirlink");
    check(i1, e1, "fs.Lstat(m, \"dirlink\")", "dirlink", fs::ModeSymlink);
    auto [i2, e2] = fs::Stat(*m, "dirlink");
    check(i2, e2, "fs.Stat(m, \"dirlink\")", "dirlink", fs::ModeDir | 0555);
    auto [i3, e3] = fs::Lstat(*m, "linklink");
    check(i3, e3, "fs.Lstat(m, \"linklink\")", "linklink", fs::ModeSymlink);
    auto [i4, e4] = fs::Stat(*m, "linklink");
    check(i4, e4, "fs.Stat(m, \"linklink\")", "linklink", fs::ModeDir | 0555);
}

TEST(GoTestingFstest, Symlink) {
    os_test::TempDir tmp;
    auto tmpfs = os::DirFS(tmp.path());

    ASSERT_FALSE(os::WriteFile(tmp.path() + "/hello", "hello, world\n", 0644));
    ASSERT_FALSE(os::Symlink(tmp.path() + "/hello", tmp.path() + "/hello.link"));
    ASSERT_FALSE(os::Symlink("hello", tmp.path() + "/hello_rel.link"));

    if (error err = fstest::TestFS(tmpfs, {"hello", "hello.link"})) FAIL() << err;
}

TEST(GoTestingFstest, Dash) {
    auto m = fstest::MakeMapFS({{"a-b/a", {.Data = to_bytes("a-b/a")}}});
    if (error err = fstest::TestFS(m, {"a-b/a"})) ADD_FAILURE() << err;
}

namespace {

// Go's shuffledFile: reads directories in reverse order.
struct shuffledFile final : fs::ReadDirFile {
    explicit shuffledFile(ptr<fs::File> f_) : f(std::move(f_)) {}
    std::pair<isize, error> Read(std::span<byte> p) override { return f->Read(p); }
    error Close() override { return f->Close(); }
    std::pair<ptr<fs::FileInfo>, error> Stat() override { return f->Stat(); }
    std::pair<std::vector<ptr<fs::DirEntry>>, error> ReadDir(int n) override {
        auto [dirents, err] = dynamic_cast<fs::ReadDirFile&>(*f).ReadDir(n);
        // Shuffle in a deterministic way, all we care about is making sure
        // that the list of directory entries is not is the lexicographic
        // order.
        //
        // We do this to make sure that the TestFS test suite is not affected
        // by the order of directory entries.
        std::ranges::sort(dirents, [](const ptr<fs::DirEntry>& a, const ptr<fs::DirEntry>& b) {
            return b->Name().view() < a->Name().view();
        });
        return {dirents, err};
    }
    ptr<fs::File> f;
};

// Go's shuffledFS: a MapFS with only Open, whose directories are shuffled.
struct shuffledFS final : fs::FS {
    explicit shuffledFS(ptr<fstest::MapFS> m_) : m(std::move(m_)) {}
    std::pair<ptr<fs::File>, error> Open(std::string_view name) override {
        auto [f, err] = m->Open(name);
        if (err) return {nullptr, err};
        return {make_ptr<shuffledFile>(f), nullptr};
    }
    ptr<fstest::MapFS> m;
};

struct failPermFS final : fs::FS {
    std::pair<ptr<fs::File>, error> Open(std::string_view name) override {
        if (!fs::ValidPath(name)) return {nullptr, make_ptr<fs::PathError>(string("open"), string(name), fs::ErrInvalid)};
        return {nullptr, make_ptr<fs::PathError>(string("open"), string(name), fs::ErrPermission)};
    }
};

}  // namespace

TEST(GoTestingFstest, ShuffledFS) {
    auto fsys = make_ptr<shuffledFS>(fstest::MakeMapFS({
        {"tmp/one", {.Data = to_bytes("1")}},
        {"tmp/two", {.Data = to_bytes("2")}},
        {"tmp/three", {.Data = to_bytes("3")}},
    }));
    if (error err = fstest::TestFS(fsys, {"tmp/one", "tmp/two", "tmp/three"})) ADD_FAILURE() << err;
}

TEST(GoTestingFstest, TestFSWrappedErrors) {
    error err = fstest::TestFS(make_ptr<failPermFS>());
    ASSERT_TRUE(err) << "error expected";

    EXPECT_TRUE(errors::Is(err, fs::ErrPermission)) << "error should be a wrapped ErrPermission: " << err;

    // TestFS is expected to return a list of errors. Enforce that the list
    // can be extracted for browsing.
    const error_interface* list = nullptr;
    for (const error_interface* e = err.get(); e && !list; e = e->Unwrap()) {
        if (!e->UnwrapList().empty()) list = e;
    }
    ASSERT_TRUE(list) << "caller should be able to extract the errors as a list: " << err;
    for (const error& e : list->UnwrapList()) {
        // ErrPermission is expected but any other error must be reported.
        EXPECT_TRUE(errors::Is(e, fs::ErrPermission)) << "unexpected error: " << e;
    }
}
