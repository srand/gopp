// Ported from Go 1.27.3's os/error_test.go and the ownership tests of
// os_unix_test.go, with Go's test names. See go_os_test.cpp.
#include "go_os_test_util.hpp"

#include <grp.h>

using namespace os_test;

namespace {

std::string checkErrorPredicate(const char* predName, bool (*pred)(const error&) noexcept, const error& err,
                                  const error& target) {
    if (!pred(err)) return std::string(predName) + " does not work as expected for " + str(err.Error());
    if (!errors::Is(err, target)) return "errors.Is(" + str(err.Error()) + ", " + str(target.Error()) + ") = false, want true";
    return "";
}

std::string testErrNotExist(const std::string& name) {
    auto [originalWd, werr] = os::Getwd();
    if (werr) return "Getwd: " + str(werr.Error());
    auto [f, err] = os::Open(name);
    if (!err) {
        f->Close();
        return "Open should have failed";
    }
    if (auto s = checkErrorPredicate("os.IsNotExist", os::IsNotExist, err, fs::ErrNotExist); !s.empty()) return s;

    error cerr = os::Chdir(name);
    if (!cerr) {
        if (error e = os::Chdir(originalWd.view())) {
            return "Chdir should have failed, failed to restore original working directory: " + str(e.Error());
        }
        return "Chdir should have failed, restored original working directory";
    }
    return checkErrorPredicate("os.IsNotExist", os::IsNotExist, cerr, fs::ErrNotExist);
}

}  // namespace

TEST(GoOs, ErrIsExist) {
    auto [f, err] = os::CreateTemp("", "_Go_ErrIsExist");
    ASSERT_FALSE(err) << "open ErrIsExist tempfile: " << err;
    auto [f2, err2] = os::OpenFile(f->Name().view(), os::O_RDWR_ | os::O_CREATE_ | os::O_EXCL_, 0600);
    if (!err2) {
        f2->Close();
        ADD_FAILURE() << "Open should have failed";
    } else {
        EXPECT_EQ(checkErrorPredicate("os.IsExist", os::IsExist, err2, fs::ErrExist), "");
    }
    f->Close();
    os::Remove(f->Name().view());
}

TEST(GoOs, ErrIsNotExist) {
    TempDir tmp;
    std::string name = join(tmp, "NotExists");
    EXPECT_EQ(testErrNotExist(name), "");
    name = join(name, "NotExists2");
    EXPECT_EQ(testErrNotExist(name), "");
}

TEST(GoOs, IsExist) {
    struct isExistTest {
        error err;
        bool is;
        bool isnot;
    };
    auto pathErr = [](error e) { return error(make_ptr<fs::PathError>(string(), string(), e)); };
    auto linkErr = [](error e) { return error(make_ptr<os::LinkError>(string(), string(), string(), e)); };
    auto syscallErr = [](error e) { return error(make_ptr<os::SyscallError>(string(), e)); };
    isExistTest tests[] = {
        {pathErr(fs::ErrInvalid), false, false},
        {pathErr(fs::ErrPermission), false, false},
        {pathErr(fs::ErrExist), true, false},
        {pathErr(fs::ErrNotExist), false, true},
        {pathErr(fs::ErrClosed), false, false},
        {linkErr(fs::ErrInvalid), false, false},
        {linkErr(fs::ErrPermission), false, false},
        {linkErr(fs::ErrExist), true, false},
        {linkErr(fs::ErrNotExist), false, true},
        {linkErr(fs::ErrClosed), false, false},
        {syscallErr(fs::ErrNotExist), false, true},
        {syscallErr(fs::ErrExist), true, false},
        {nullptr, false, false},
    };
    for (auto& tt : tests) {
        EXPECT_EQ(os::IsExist(tt.err), tt.is) << "os.IsExist(" << tt.err << ")";
        EXPECT_EQ(errors::Is(tt.err, fs::ErrExist), tt.is) << "errors.Is(" << tt.err << ", fs.ErrExist)";
        EXPECT_EQ(os::IsNotExist(tt.err), tt.isnot) << "os.IsNotExist(" << tt.err << ")";
        EXPECT_EQ(errors::Is(tt.err, fs::ErrNotExist), tt.isnot) << "errors.Is(" << tt.err << ", fs.ErrNotExist)";
    }
}

TEST(GoOs, IsPermission) {
    struct isPermissionTest {
        error err;
        bool want;
    } tests[] = {
        {nullptr, false},
        {error(make_ptr<fs::PathError>(string(), string(), fs::ErrPermission)), true},
        {error(make_ptr<os::SyscallError>(string(), fs::ErrPermission)), true},
    };
    for (auto& tt : tests) {
        EXPECT_EQ(os::IsPermission(tt.err), tt.want) << "os.IsPermission(" << tt.err << ")";
        EXPECT_EQ(errors::Is(tt.err, fs::ErrPermission), tt.want) << "errors.Is(" << tt.err << ", fs.ErrPermission)";
    }
}

TEST(GoOs, ErrPathNUL) {
    auto [f, err] = os::CreateTemp("", std::string_view("_Go_ErrPathNUL\0", 15));
    if (!err) {
        f->Close();
        FAIL() << "TempFile should have failed";
    }
    auto [f1, err1] = os::CreateTemp("", "_Go_ErrPathNUL");
    ASSERT_FALSE(err1) << "open ErrPathNUL tempfile: " << err1;
    std::string name = str(f1->Name());
    auto [f2, err2] = os::OpenFile(name, os::O_RDWR_, 0600);
    ASSERT_FALSE(err2) << "open ErrPathNUL: " << err2;
    f2->Close();
    auto [f3, err3] = os::OpenFile(name + std::string(1, '\0'), os::O_RDWR_, 0600);
    if (!err3) {
        f3->Close();
        ADD_FAILURE() << "Open should have failed";
    }
    f1->Close();
    os::Remove(name);
}

TEST(GoOs, PathErrorUnwrap) {
    error pe = make_ptr<fs::PathError>(string(), string(), fs::ErrInvalid);
    EXPECT_TRUE(errors::Is(pe, fs::ErrInvalid)) << "errors.Is failed, wanted success";
}

namespace {

struct myErrorIs final : errorInterface {
    explicit myErrorIs(error e) : e(std::move(e)) {}
    error e;
    string Error() const override { return e.Error(); }
    bool Is(const errorInterface* target) const noexcept override { return target == e.get(); }
};

}  // namespace

TEST(GoOs, ErrorIsMethods) {
    EXPECT_FALSE(os::IsPermission(make_ptr<myErrorIs>(fs::ErrPermission)))
        << "os.IsPermission(err) = true when err.Is(fs.ErrPermission), wanted false";
}

// os_unix_test.go

namespace {

void checkUidGid(std::string_view path, int uid, int gid) {
    auto [dir, err] = os::Lstat(path);
    ASSERT_FALSE(err) << "Lstat " << path << " (looking for uid/gid " << uid << "/" << gid << "): " << err;
    auto* sys = static_cast<const struct stat*>(dir->Sys());
    EXPECT_EQ(static_cast<int>(sys->st_uid), uid) << "Lstat " << path;
    EXPECT_EQ(static_cast<int>(sys->st_gid), gid) << "Lstat " << path;
}

std::vector<int> getgroups() {
    int n = ::getgroups(0, nullptr);
    std::vector<gid_t> g(static_cast<std::size_t>(std::max(n, 0)));
    n = ::getgroups(n, g.data());
    return std::vector<int>(g.begin(), g.begin() + std::max(n, 0));
}

}  // namespace

TEST(GoOs, Chown) {
    // Use TempDir() to make sure we're on a local file system, so that
    // the group ids returned by Getgroups will be allowed on the file. On
    // NFS, the Getgroups groups are almost certainly not okay for the
    // file.
    NewFile f;
    auto [dir, err] = f->Stat();
    ASSERT_FALSE(err) << err;
    // Can't change uid unless root, but can try changing the group id.
    // First try our current group.
    int gid = static_cast<int>(::getgid());
    ASSERT_FALSE(os::Chown(f->Name().view(), -1, gid));
    auto* sys = static_cast<const struct stat*>(dir->Sys());
    int uid = static_cast<int>(sys->st_uid);
    checkUidGid(f->Name().view(), uid, gid);

    // Then try all the auxiliary groups.
    for (int g : getgroups()) {
        ASSERT_FALSE(os::Chown(f->Name().view(), -1, g)) << "chown -1 " << g;
        checkUidGid(f->Name().view(), uid, g);
        // change back to gid to test fd.Chown
        ASSERT_FALSE(f->Chown(-1, gid)) << "fchown -1 " << gid;
        checkUidGid(f->Name().view(), uid, gid);
    }
}

TEST(GoOs, FileChown) {
    NewFile f;
    auto [dir, err] = f->Stat();
    ASSERT_FALSE(err) << err;
    int gid = static_cast<int>(::getgid());
    ASSERT_FALSE(f->Chown(-1, gid));
    auto* sys = static_cast<const struct stat*>(dir->Sys());
    int uid = static_cast<int>(sys->st_uid);
    checkUidGid(f->Name().view(), uid, gid);
    for (int g : getgroups()) {
        ASSERT_FALSE(f->Chown(-1, g)) << "fchown -1 " << g;
        checkUidGid(f->Name().view(), uid, g);
        ASSERT_FALSE(f->Chown(-1, gid)) << "fchown -1 " << gid;
        checkUidGid(f->Name().view(), uid, gid);
    }
}

TEST(GoOs, Lchown) {
    NewFile f;
    auto [dir, err] = f->Stat();
    ASSERT_FALSE(err) << err;
    std::string linkname = str(f->Name()) + "2";
    ASSERT_FALSE(os::Symlink(f->Name().view(), linkname));
    int gid = static_cast<int>(::getgid());
    error lerr = os::Lchown(linkname, -1, gid);
    if (lerr) {
        os::Remove(linkname);
        FAIL() << "lchown " << linkname << " -1 " << gid << ": " << lerr;
    }
    auto* sys = static_cast<const struct stat*>(dir->Sys());
    int uid = static_cast<int>(sys->st_uid);
    checkUidGid(linkname, uid, gid);
    for (int g : getgroups()) {
        EXPECT_FALSE(os::Lchown(linkname, -1, g)) << "lchown -1 " << g;
        checkUidGid(linkname, uid, g);
        // Check that link target's gid is unchanged.
        checkUidGid(f->Name().view(), uid, static_cast<int>(sys->st_gid));
        EXPECT_FALSE(os::Lchown(linkname, -1, gid));
    }
    os::Remove(linkname);
}
