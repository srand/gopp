// Ported from Go 1.27.3's os/root_test.go and os/root_unix_test.go, with
// Go's test names. Go's subtests are
// SCOPED_TRACEs; a t.Fatal ends the case.
//
// Adapted: TestRootNoLstat checks what the directory reads return; go++
// has no stat hook to check that they don't Lstat by name. Go's
// TestRootLinkTo runs the rename test (testRootMoveTo(t, true)), and its
// consistency check compares err1 with itself; both are kept as in Go.
#include "go_os_test_util.hpp"

#include <gopp/chan.hpp>
#include <gopp/fmt.hpp>
#include <gopp/net.hpp>
#include <gopp/path.hpp>

#include <sys/statvfs.h>

#include <algorithm>
#include <memory>
#include <optional>

using namespace os_test;

namespace {

// makefs creates a test filesystem layout in a new directory named ROOT
// in tmp, and returns its path. Each entry is a file, directory, or
// symbolic link to create:
//
//   - "d/": directory d
//   - "f": file f with contents f
//   - "a => b": symlink a with target b
//
// $ABS is replaced with the absolute path of the directory containing the
// filesystem. Parent directories are automatically created as needed.
std::string makefs(const TempDir& tmp, const std::vector<std::string>& fs) {
    std::string root = join(tmp, "ROOT");
    EXPECT_FALSE(os::Mkdir(root, 0777));
    for (std::string ent : fs) {
        for (std::size_t p; (p = ent.find("$ABS")) != ent.npos;) ent.replace(p, 4, root);
        std::string base = ent, link;
        bool isLink = false;
        if (auto i = ent.find(" => "); i != ent.npos) {
            base = ent.substr(0, i);
            link = ent.substr(i + 4);
            isLink = true;
            ent = base;
        }
        EXPECT_FALSE(os::MkdirAll(str(path::Join(root, path::Dir(base).view())), 0777));
        if (isLink) {
            EXPECT_FALSE(os::Symlink(link, str(path::Join(root, base))));
        } else if (ent.ends_with("/")) {
            EXPECT_FALSE(os::MkdirAll(str(path::Join(root, ent)), 0777));
        } else {
            EXPECT_FALSE(os::WriteFile(str(path::Join(root, ent)), ent, 0666));
        }
    }
    return root;
}

// A rootTest is a test case for os.Root.
struct rootTest {
    std::string name = {};
    // fs is the test filesystem layout. See makefs above.
    std::vector<std::string> fs = {};
    // open is the filename to access in the test.
    std::string open = {};
    // target is the filename that we expect to be accessed, after resolving
    // all symlinks. For test cases where the operation fails due to an
    // escaping path such as ../ROOT/x, the target is the filename that
    // should not have been opened.
    std::string target = {};
    // ltarget is the filename that we expect to accessed, after resolving
    // all symlinks except the last one. This is the file we expect to be
    // removed by Remove or statted by Lstat.
    //
    // If the last path component in open is not a symlink, ltarget should
    // be "".
    std::string ltarget = {};
    // wantError is true if accessing the file should fail.
    bool wantError = false;
    // alwaysFails is true if the open operation is expected to fail even
    // when using non-openat operations.
    bool alwaysFails = false;

    // run sets up the test filesystem layout, opens the root, and calls f.
    template <typename F>
    void run(F&& f) const {
        SCOPED_TRACE(name);
        TempDir tmp;
        std::string root = makefs(tmp, fs);
        auto [d, err] = os::OpenRoot(root);
        ASSERT_FALSE(err) << err;
        // The target is a file that will be accessed, or a file that should
        // not be accessed (because doing so escapes the root).
        std::string t = target.empty() ? "" : join(root, target);
        f(t, *d);
        d->Close();
    }
};

const std::vector<rootTest> rootTestCases = {
    {.name = "plain path", .fs = {}, .open = "target", .target = "target"},
    {.name = "path in directory", .fs = {"a/b/c/"}, .open = "a/b/c/target", .target = "a/b/c/target"},
    {.name = "symlink", .fs = {"link => target"}, .open = "link", .target = "target", .ltarget = "link"},
    {.name = "symlink dotdot slash", .fs = {"link => ../"}, .open = "link", .ltarget = "link", .wantError = true},
    {.name = "symlink ending in slash", .fs = {"dir/", "link => dir/"}, .open = "link/target", .target = "dir/target"},
    {.name = "slash after symlink to file",
     .fs = {"link => ../ROOT/target"},
     .open = "link/",
     .target = "target",
     .wantError = true},
    {.name = "slash after symlink to dir", .fs = {"link => ../ROOT/target", "target/"}, .open = "link/", .wantError = true},
    {.name = "symlink dotdot dotdot slash",
     .fs = {"dir/link => ../../"},
     .open = "dir/link",
     .ltarget = "dir/link",
     .wantError = true},
    {.name = "symlink chain",
     .fs = {"link => a/b/c/target", "a/b => e", "a/e => ../f", "f => g/h/i", "g/h/i => ..", "g/c/"},
     .open = "link",
     .target = "g/c/target",
     .ltarget = "link"},
    {.name = "path with dot", .fs = {"a/b/"}, .open = "./a/./b/./target", .target = "a/b/target"},
    {.name = "path with dotdot", .fs = {"a/b/"}, .open = "a/../a/b/../../a/b/../b/target", .target = "a/b/target"},
    {.name = "path with dotdot slash", .fs = {}, .open = "../", .wantError = true},
    {.name = "path with dotdot dotdot slash", .fs = {"a/"}, .open = "a/../../", .wantError = true},
    {.name = "dotdot no symlink", .fs = {"a/"}, .open = "a/../target", .target = "target"},
    {.name = "dotdot after symlink", .fs = {"a => b/c", "b/c/"}, .open = "a/../target", .target = "b/target"},
    {.name = "dotdot before symlink", .fs = {"a => b/c", "b/c/"}, .open = "b/../a/target", .target = "b/c/target"},
    {.name = "symlink ends in dot", .fs = {"a => b/.", "b/"}, .open = "a/target", .target = "b/target"},
    {.name = "directory does not exist", .fs = {}, .open = "a/file", .wantError = true, .alwaysFails = true},
    {.name = "empty path", .fs = {}, .open = "", .wantError = true, .alwaysFails = true},
    {.name = "symlink cycle", .fs = {"a => a"}, .open = "a", .ltarget = "a", .wantError = true, .alwaysFails = true},
    {.name = "path escapes", .fs = {}, .open = "../ROOT/target", .target = "target", .wantError = true},
    {.name = "long path escapes", .fs = {"a/"}, .open = "a/../../ROOT/target", .target = "target", .wantError = true},
    {.name = "absolute symlink",
     .fs = {"link => $ABS/target"},
     .open = "link",
     .target = "target",
     .ltarget = "link",
     .wantError = true},
    {.name = "relative symlink",
     .fs = {"link => ../ROOT/target"},
     .open = "link",
     .target = "target",
     .ltarget = "link",
     .wantError = true},
    {.name = "symlink chain escapes",
     .fs = {"link => a/b/c/target", "a/b => e", "a/e => ../../ROOT", "c/"},
     .open = "link",
     .target = "c/target",
     .ltarget = "link",
     .wantError = true},
};

// errEndsTest checks the error result of a test, verifying that it
// succeeded or failed as expected. It returns true if the test is done:
// due to encountering an expected error, or a failure.
bool errEndsTest(const error& err, bool wantError, const std::string& op) {
    if (wantError) {
        if (!err) ADD_FAILURE() << op << " = nil; want error";
        return true;
    }
    if (err) {
        ADD_FAILURE() << op << " = " << err << "; want success";
        return true;
    }
    return false;
}

std::string q(std::string_view s) { return str(strconv::Quote(s)); }

}  // namespace

TEST(GoOsRoot, RootOpen_File) {
    const std::string want = "target";
    for (auto& test : rootTestCases) {
        test.run([&](const std::string& target, os::Root& root) {
            if (!target.empty()) { ASSERT_FALSE(os::WriteFile(target, want, 0666)); }
            auto [f, err] = root.Open(test.open);
            if (errEndsTest(err, test.wantError, "root.Open(" + q(test.open) + ")")) return;
            auto [got, rerr] = io::ReadAll(*f);
            EXPECT_TRUE(!rerr && asStringView(got) == want)
                << "Dir.Open(" << q(test.open) << "): read content " << asStringView(got) << ", " << rerr;
            f->Close();
        });
    }
}

TEST(GoOsRoot, RootOpen_Directory) {
    for (auto& test : rootTestCases) {
        test.run([&](const std::string& target, os::Root& root) {
            if (!target.empty()) {
                ASSERT_FALSE(os::Mkdir(target, 0777));
                ASSERT_FALSE(os::WriteFile(target + "/found", "", 0666));
            }
            auto [f, err] = root.Open(test.open);
            if (errEndsTest(err, test.wantError, "root.Open(" + q(test.open) + ")")) return;
            auto [got, rerr] = f->Readdirnames(-1);
            EXPECT_FALSE(rerr) << "Dir.Open(" << q(test.open) << ").Readdirnames: " << rerr;
            EXPECT_TRUE(got.size() == 1 && got[0].view() == "found") << "Dir.Open(" << q(test.open) << ").Readdirnames";
            f->Close();
        });
    }
}

TEST(GoOsRoot, RootCreate) {
    const std::string want = "target";
    for (auto& test : rootTestCases) {
        test.run([&](const std::string& target, os::Root& root) {
            auto [f, err] = root.Create(test.open);
            if (errEndsTest(err, test.wantError, "root.Create(" + q(test.open) + ")")) return;
            ASSERT_FALSE(f->Write(b(want)).second);
            f->Close();
            auto [got, rerr] = os::ReadFile(target);
            ASSERT_FALSE(rerr) << "reading file created with root.Create(" << q(test.open) << "): " << rerr;
            ASSERT_EQ(asStringView(got), want) << "reading file created with root.Create(" << q(test.open) << ")";
        });
    }
}

TEST(GoOsRoot, RootChmod) {
    for (auto& test : rootTestCases) {
        test.run([&](const std::string& target, os::Root& root) {
            if (!target.empty()) {
                // Create a file with no read/write permissions, to ensure
                // we can use Chmod on an inaccessible file.
                ASSERT_FALSE(os::WriteFile(target, "", 0));
            }
            FileMode want = 0666;
            error err = root.Chmod(test.open, want);
            if (errEndsTest(err, test.wantError, "root.Chmod(" + q(test.open) + ")")) return;
            auto [st, serr] = os::Stat(target);
            ASSERT_FALSE(serr) << "os.Stat(" << q(target) << ") = " << serr;
            EXPECT_EQ(st->Mode(), want) << "after root.Chmod(" << q(test.open) << ", " << want << "): file mode";
        });
    }
}

namespace {

// Go's hasNoatime: whether the temporary directory's file system is
// mounted noatime.
bool hasNoatime() {
    struct statvfs st{};
    std::string dir = str(os::TempDir());
    if (::statvfs(dir.c_str(), &st) != 0) return false;
    return st.f_flag & ST_NOATIME;
}

// Go's export_test Atime.
time::Time atime(const ptr<FileInfo>& fi) {
    auto* st = static_cast<const struct stat*>(fi->Sys());
    return time::Time::Unix(st->st_atim.tv_sec, st->st_atim.tv_nsec);
}

}  // namespace

TEST(GoOsRoot, RootChtimes) {
    // Don't check atimes if the fs is mounted noatime.
    bool checkAtimes = !hasNoatime();
    for (auto& test : rootTestCases) {
        test.run([&](const std::string& target, os::Root& root) {
            if (!target.empty()) { ASSERT_FALSE(os::WriteFile(target, "", 0666)); }
            struct timesT {
                time::Time atime, mtime;
            } timesList[] = {
                {time::Now().Add(-1 * time::Minute), time::Now().Add(-1 * time::Minute)},
                {time::Now().Add(1 * time::Minute), time::Now().Add(1 * time::Minute)},
                {time::Time{}, time::Now()},
                {time::Now(), time::Time{}},
            };
            for (auto& times : timesList) {
                error err = root.Chtimes(test.open, times.atime, times.mtime);
                if (errEndsTest(err, test.wantError, "root.Chtimes(" + q(test.open) + ")")) return;
                auto [st, serr] = os::Stat(target);
                ASSERT_FALSE(serr) << "os.Stat(" << q(target) << ") = " << serr;
                if (auto got = st->ModTime(); !times.mtime.IsZero() && !got.Equal(times.mtime)) {
                    ADD_FAILURE() << "after root.Chtimes(" << q(test.open) << "): got mtime=" << got.UnixNano()
                                  << ", want " << times.mtime.UnixNano();
                }
                if (checkAtimes) {
                    if (auto got = atime(st); !times.atime.IsZero() && !got.Equal(times.atime)) {
                        ADD_FAILURE() << "after root.Chtimes(" << q(test.open) << "): got atime=" << got.UnixNano()
                                      << ", want " << times.atime.UnixNano();
                    }
                }
            }
        });
    }
}

TEST(GoOsRoot, RootMkdir) {
    for (auto& test : rootTestCases) {
        test.run([&](const std::string& target, os::Root& root) {
            bool wantError = test.wantError;
            if (!test.ltarget.empty()) {
                // This case is trying to mkdir("some symlink"), which is an
                // error (but not an escape).
                wantError = true;
            }
            error err = root.Mkdir(test.open, 0777);
            if (errEndsTest(err, wantError, "root.Create(" + q(test.open) + ")")) return;
            auto [fi, serr] = os::Lstat(target);
            ASSERT_FALSE(serr) << "stat file created with Root.Mkdir(" << q(test.open) << "): " << serr;
            ASSERT_TRUE(fi->IsDir()) << "stat file created with Root.Mkdir(" << q(test.open) << "): not a directory";
            // Issue #73559: there should be mode bits.
            ASSERT_TRUE(fi->Mode() & FileMode(0777))
                << "stat file created with Root.Mkdir(" << q(test.open) << "): mode=" << fi->Mode() << ", want non-zero";
        });
    }
}

TEST(GoOsRoot, RootMkdirAll) {
    for (auto& test : rootTestCases) {
        if (test.name == "directory does not exist") {
            // Test expects error, mkdirall creates the missing directory.
            continue;
        }
        test.run([&](const std::string& target, os::Root& root) {
            bool wantError = test.wantError;
            if (!test.ltarget.empty()) {
                // This case is trying to mkdir("some symlink"), which is an
                // error (but not an escape).
                wantError = true;
            }
            error err = root.MkdirAll(test.open, 0777);
            if (errEndsTest(err, wantError, "root.MkdirAll(" + q(test.open) + ")")) return;
            auto [fi, serr] = os::Lstat(target);
            ASSERT_FALSE(serr) << "stat file created with Root.MkdirAll(" << q(test.open) << "): " << serr;
            ASSERT_TRUE(fi->IsDir()) << "stat file created with Root.MkdirAll(" << q(test.open) << "): not a directory";
            ASSERT_TRUE(fi->Mode() & FileMode(0777))
                << "stat file created with Root.MkdirAll(" << q(test.open) << "): mode=" << fi->Mode()
                << ", want non-zero";
        });
    }
}

TEST(GoOsRoot, RootOpenRoot) {
    for (auto& test : rootTestCases) {
        test.run([&](const std::string& target, os::Root& root) {
            if (!target.empty()) {
                ASSERT_FALSE(os::Mkdir(target, 0777));
                ASSERT_FALSE(os::WriteFile(target + "/f", "", 0666));
            }
            auto [rr, err] = root.OpenRoot(test.open);
            if (errEndsTest(err, test.wantError, "root.OpenRoot(" + q(test.open) + ")")) return;
            auto [f, oerr] = rr->Open("f");
            rr->Close();
            ASSERT_FALSE(oerr) << "root.OpenRoot(" << q(test.open) << ").Open(\"f\") = " << oerr;
            f->Close();
        });
    }
}

TEST(GoOsRoot, RootRemoveFile) {
    for (auto& test : rootTestCases) {
        test.run([&](std::string target, os::Root& root) {
            bool wantError = test.wantError;
            if (!test.ltarget.empty()) {
                // Remove doesn't follow symlinks in the final path
                // component, so it will successfully remove ltarget.
                wantError = false;
                target = join(root.Name().view(), test.ltarget);
            } else if (!target.empty()) {
                ASSERT_FALSE(os::WriteFile(target, "", 0666));
            }
            error err = root.Remove(test.open);
            if (errEndsTest(err, wantError, "root.Remove(" + q(test.open) + ")")) return;
            auto [fi, serr] = os::Lstat(target);
            ASSERT_TRUE(errors::Is(serr, os::ErrNotExist))
                << "stat file removed with Root.Remove(" << q(test.open) << "): " << serr << ", want ErrNotExist";
        });
    }
}

TEST(GoOsRoot, RootRemoveDirectory) {
    for (auto& test : rootTestCases) {
        test.run([&](std::string target, os::Root& root) {
            bool wantError = test.wantError;
            if (!test.ltarget.empty()) {
                // Remove doesn't follow symlinks in the final path
                // component, so it will successfully remove ltarget.
                wantError = false;
                target = join(root.Name().view(), test.ltarget);
            } else if (!target.empty()) {
                ASSERT_FALSE(os::Mkdir(target, 0777));
            }
            error err = root.Remove(test.open);
            if (errEndsTest(err, wantError, "root.Remove(" + q(test.open) + ")")) return;
            auto [fi, serr] = os::Lstat(target);
            ASSERT_TRUE(errors::Is(serr, os::ErrNotExist))
                << "stat file removed with Root.Remove(" << q(test.open) << "): " << serr << ", want ErrNotExist";
        });
    }
}

TEST(GoOsRoot, RootRemoveAll) {
    for (auto test : rootTestCases) {
        test.run([&](std::string target, os::Root& root) {
            if (test.open.ends_with("/")) {
                // The test is removing a file with a trailing /. RemoveAll
                // ignores trailing /s. If the file is a symlink, it will
                // remove the symlink.
                std::string fullname = str(path::Join(root.Name().view(), test.open));  // cleaned, as filepath.Join
                if (auto [st, err] = os::Lstat(fullname); !err && st->Mode().Type() == fs::ModeSymlink) {
                    test.ltarget = test.open;
                }
            }
            bool wantError = test.wantError;
            if (!test.ltarget.empty()) {
                // Remove doesn't follow symlinks in the final path
                // component, so it will successfully remove ltarget.
                wantError = false;
                target = join(root.Name().view(), test.ltarget);
            } else if (!target.empty()) {
                ASSERT_FALSE(os::Mkdir(target, 0777));
                ASSERT_FALSE(os::WriteFile(join(target, "file"), "", 0666));
            }
            bool targetExists = true;
            if (auto [fi, err] = root.Lstat(test.open); errors::Is(err, os::ErrNotExist)) {
                // If the target doesn't exist, RemoveAll succeeds rather
                // than returning ErrNotExist.
                targetExists = false;
                wantError = false;
            }
            error err = root.RemoveAll(test.open);
            if (errEndsTest(err, wantError, "root.RemoveAll(" + q(test.open) + ")")) return;
            if (!targetExists) return;
            auto [fi, serr] = os::Lstat(target);
            ASSERT_TRUE(errors::Is(serr, os::ErrNotExist))
                << "stat file removed with Root.Remove(" << q(test.open) << "): " << serr << ", want ErrNotExist";
        });
    }
}

TEST(GoOsRoot, RootOpenFileAsRoot) {
    TempDir dir;
    std::string target = join(dir, "target");
    ASSERT_FALSE(os::WriteFile(target, "", 0666));
    auto [r, err] = os::OpenRoot(target);
    if (!err) {
        r->Close();
        FAIL() << "os.OpenRoot(file) succeeded; want failure";
    }
    std::tie(r, err) = os::OpenRoot(dir.path());
    ASSERT_FALSE(err) << err;
    auto [rr, rerr] = r->OpenRoot("target");
    r->Close();
    if (!rerr) {
        rr->Close();
        FAIL() << "Root.OpenRoot(file) succeeded; want failure";
    }
}

TEST(GoOsRoot, RootStat) {
    for (auto& test : rootTestCases) {
        test.run([&](const std::string& target, os::Root& root) {
            const std::string content = "content";
            if (!target.empty()) { ASSERT_FALSE(os::WriteFile(target, content, 0666)); }
            auto [fi, err] = root.Stat(test.open);
            if (errEndsTest(err, test.wantError, "root.Stat(" + q(test.open) + ")")) return;
            EXPECT_EQ(fi->Name(), path::Base(test.open)) << "root.Stat(" << q(test.open) << ").Name()";
            EXPECT_EQ(fi->Size(), int64(content.size())) << "root.Stat(" << q(test.open) << ").Size()";
        });
    }
}

TEST(GoOsRoot, RootLstat) {
    for (auto& test : rootTestCases) {
        test.run([&](const std::string& target, os::Root& root) {
            const std::string content = "content";
            bool wantError = test.wantError;
            if (!test.ltarget.empty()) {
                // Lstat will stat the final link, rather than following it.
                wantError = false;
            } else if (!target.empty()) {
                ASSERT_FALSE(os::WriteFile(target, content, 0666));
            }
            auto [fi, err] = root.Lstat(test.open);
            if (errEndsTest(err, wantError, "root.Stat(" + q(test.open) + ")")) return;
            EXPECT_EQ(fi->Name(), path::Base(test.open)) << "root.Stat(" << q(test.open) << ").Name()";
            if (test.ltarget.empty()) {
                EXPECT_FALSE(fi->Mode() & os::ModeSymlink) << "root.Stat(" << q(test.open) << ").Mode() = "
                                                           << fi->Mode() << ", want non-symlink";
                EXPECT_EQ(fi->Size(), int64(content.size())) << "root.Stat(" << q(test.open) << ").Size()";
            } else {
                EXPECT_TRUE(fi->Mode() & os::ModeSymlink)
                    << "root.Stat(" << q(test.open) << ").Mode() = " << fi->Mode() << ", want symlink";
            }
        });
    }
}

TEST(GoOsRoot, RootReadlink) {
    for (auto& test : rootTestCases) {
        test.run([&](const std::string&, os::Root& root) {
            // Readlink will read the final link, rather than following it;
            // it fails on non-link targets.
            bool wantError = test.ltarget.empty();
            auto [got, err] = root.Readlink(test.open);
            if (errEndsTest(err, wantError, "root.Readlink(" + q(test.open) + ")")) return;
            auto [want, rerr] = os::Readlink(join(root.Name().view(), test.ltarget));
            ASSERT_FALSE(rerr) << "os.Readlink(" << q(test.ltarget) << ") = " << rerr << ", want success";
            EXPECT_EQ(got, want) << "root.Readlink(" << q(test.open) << ")";
        });
    }
}

namespace {

void testRootMoveFrom(bool rename) {
    const std::string want = "target";
    for (auto& test : rootTestCases) {
        test.run([&](const std::string& target, os::Root& root) {
            if (!target.empty()) { ASSERT_FALSE(os::WriteFile(target, want, 0666)); }
            bool wantError = test.wantError;
            std::string linkTarget;
            if (!test.ltarget.empty()) {
                // Rename will rename the link, not the file linked to.
                wantError = false;
                auto [lt, err] = root.Readlink(test.ltarget);
                ASSERT_FALSE(err) << "root.Readlink(" << q(test.ltarget) << ") = " << err << ", want success";
                linkTarget = str(lt);
            }

            const std::string dstPath = "destination";
            std::string op;
            error err;
            if (rename) {
                op = "Rename";
                err = root.Rename(test.open, dstPath);
            } else {
                op = "Link";
                err = root.Link(test.open, dstPath);
            }
            if (errEndsTest(err, wantError, "root." + op + "(" + q(test.open) + ", " + q(dstPath) + ")")) return;

            std::string origPath = target;
            if (!test.ltarget.empty()) origPath = join(root.Name().view(), test.ltarget);
            auto [ofi, lerr] = os::Lstat(origPath);
            if (rename) {
                EXPECT_TRUE(errors::Is(lerr, os::ErrNotExist))
                    << "after renaming file, Lstat(" << q(origPath) << ") = " << lerr << ", want ErrNotExist";
            } else {
                EXPECT_FALSE(lerr) << "after linking file, error accessing original: " << lerr;
            }

            std::string dstFullPath = join(root.Name().view(), dstPath);
            if (!test.ltarget.empty()) {
                auto [got, rerr] = os::Readlink(dstFullPath);
                EXPECT_TRUE(!rerr && got.view() == linkTarget)
                    << "os.Readlink(" << q(dstFullPath) << ") = " << got.view() << ", " << rerr << ", want "
                    << linkTarget;
            } else {
                auto [got, rerr] = os::ReadFile(dstFullPath);
                EXPECT_TRUE(!rerr && asStringView(got) == want)
                    << "os.ReadFile(" << q(dstFullPath) << "): read content " << asStringView(got) << ", " << rerr;
                auto [st, serr] = os::Lstat(dstFullPath);
                EXPECT_TRUE(!serr && !(st->Mode() & fs::ModeSymlink))
                    << "os.Lstat(" << q(dstFullPath) << ") = " << serr << "; want non-symlink";
            }
        });
    }
}

void testRootMoveTo(bool rename) {
    const std::string want = "target";
    for (auto& test : rootTestCases) {
        test.run([&](const std::string&, os::Root& root) {
            const std::string srcPath = "source";
            ASSERT_FALSE(os::WriteFile(join(root.Name().view(), srcPath), want, 0666));

            std::string target = test.target;
            bool wantError = test.wantError;
            if (!test.ltarget.empty()) {
                // Rename will overwrite the final link rather than follow it.
                target = test.ltarget;
                wantError = false;
            }

            error err;
            std::string op;
            if (rename) {
                op = "Rename";
                err = root.Rename(srcPath, test.open);
            } else {
                op = "Link";
                err = root.Link(srcPath, test.open);
            }
            if (errEndsTest(err, wantError, "root." + op + "(" + q(srcPath) + ", " + q(test.open) + ")")) return;

            auto [sfi, lerr] = os::Lstat(join(root.Name().view(), srcPath));
            if (rename) {
                EXPECT_TRUE(errors::Is(lerr, os::ErrNotExist))
                    << "after renaming file, Lstat(" << q(srcPath) << ") = " << lerr << ", want ErrNotExist";
            } else {
                EXPECT_FALSE(lerr) << "after linking file, error accessing original: " << lerr;
            }

            auto [got, rerr] = os::ReadFile(join(root.Name().view(), target));
            EXPECT_TRUE(!rerr && asStringView(got) == want)
                << "os.ReadFile(" << q(target) << "): read content " << asStringView(got) << ", " << rerr;
        });
    }
}

}  // namespace

// TestRootRenameFrom tests renaming the test case target to a known-good
// path.
TEST(GoOsRoot, RootRenameFrom) { testRootMoveFrom(true); }

// TestRootLinkFrom tests linking the test case target to a known-good path.
TEST(GoOsRoot, RootLinkFrom) { testRootMoveFrom(false); }

// TestRootRenameTo tests renaming a known-good path to the test case
// target.
TEST(GoOsRoot, RootRenameTo) { testRootMoveTo(true); }

// TestRootLinkTo: as in Go, it runs the rename test.
TEST(GoOsRoot, RootLinkTo) { testRootMoveTo(true); }

TEST(GoOsRoot, RootSymlink) {
    for (auto& test : rootTestCases) {
        test.run([&](const std::string& target, os::Root& root) {
            // We can't create a symlink over an existing symlink.
            bool wantError = test.wantError || !test.ltarget.empty();
            const std::string wantTarget = "linktarget";
            error err = root.Symlink(wantTarget, test.open);
            if (errEndsTest(err, wantError, "root.Symlink(" + q(test.open) + ")")) return;
            auto [got, rerr] = os::Readlink(target);
            ASSERT_TRUE(!rerr && got.view() == wantTarget)
                << "ReadLink(" << q(target) << ") = " << got.view() << ", " << rerr << "; want " << wantTarget;
        });
    }
}

namespace {

// Per-case state of a consistency test: temporary directories and
// cleanups (Go's t.TempDir and t.Cleanup), run at the end of the case.
struct caseState {
    std::vector<std::unique_ptr<TempDir>> dirs;
    std::vector<std::function<void()>> cleanups;
    const TempDir& tempDir() { return *dirs.emplace_back(std::make_unique<TempDir>()); }
    ~caseState() {
        for (auto it = cleanups.rbegin(); it != cleanups.rend(); ++it) (*it)();
    }
};

// A rootConsistencyTest is a test case comparing os.Root behavior with the
// corresponding non-Root function.
//
// These tests verify that, for example, Root.Open("file/./") and
// os.Open("file/./") have the same result, although the specific result
// may vary by platform.
struct rootConsistencyTest {
    std::string name = {};
    // fs is the test filesystem layout. See makefs above. fsFunc is called
    // to modify the test filesystem, or replace it.
    std::vector<std::string> fs = {};
    std::function<std::string(caseState&, const std::string& dir)> fsFunc = {};
    // open is the filename to access in the test.
    std::string open = {};
    // check is called with the test's name before the test starts, and
    // returns a reason to skip it, or "".
    std::function<std::string(const std::string& testName)> check = {};

    using Func = std::function<std::pair<std::string, error>(const std::string& path, os::Root* r)>;

    void run(const std::string& testName, const Func& f) const {
        SCOPED_TRACE(name);
        if (check) {
            if (std::string skip = check(testName + "/" + name); !skip.empty()) return;  // t.Skip
        }
        caseState st;
        std::string dir1 = makefs(st.tempDir(), fs);
        std::string dir2 = makefs(st.tempDir(), fs);
        if (fsFunc) {
            dir1 = fsFunc(st, dir1);
            dir2 = fsFunc(st, dir2);
        }

        auto [r, err] = os::OpenRoot(dir1);
        ASSERT_FALSE(err) << err;

        auto [res1, err1] = f(open, r.get());
        auto [res2, err2] = f(dir2 + "/" + open, nullptr);
        r->Close();

        if (res1 != res2 || (!err1 != !err2)) {
            ADD_FAILURE() << "with root:    res=" << res1 << "\n              err=" << err1
                          << "\nwithout root: res=" << res2 << "\n              err=" << err2
                          << "\nwant consistent results, got mismatch";
        }

        if (err1 || err2) {
            // As in Go, both are of err1.
            auto underlying = [&](std::string_view how, const error&) -> std::optional<error> {
                if (auto* e = dynamic_cast<const os::PathError*>(err1.get())) return e->Err;
                if (auto* e = dynamic_cast<const os::LinkError*>(err1.get())) return e->Err;
                ADD_FAILURE() << how << ", expected PathError or LinkError; got: " << err1;
                return std::nullopt;
            };
            auto e1 = underlying("with root", err1);
            if (!e1) return;
            auto e2 = underlying("without root", err1);
            if (!e2) return;
            if (*e1 != *e2) {
                ADD_FAILURE() << "with root:    err=" << *e1 << "\nwithout root: err=" << *e2
                              << "\nwant consistent results, got mismatch";
            }
        }
    }
};

std::string tempDirWithUnixSocket(caseState& st, const std::string& name) {
    const TempDir& dir = st.tempDir();
    auto [addr, err] = net::ResolveUnixAddr("unix", join(dir, name));
    if (err) return dir.path();
    auto [conn, lerr] = net::ListenUnix("unix", addr);
    if (lerr) return dir.path();
    st.cleanups.push_back([conn] { conn->Close(); });
    return dir.path();
}

const std::vector<rootConsistencyTest>& rootConsistencyTestCases() {
    static const std::vector<rootConsistencyTest> cases = {
        {.name = "file", .fs = {"target"}, .open = "target"},
        {.name = "dir slash dot", .fs = {"target/file"}, .open = "target/."},
        {.name = "dot", .fs = {"file"}, .open = "."},
        {.name = "file slash dot", .fs = {"target"}, .open = "target/."},
        {.name = "dir slash", .fs = {"target/file"}, .open = "target/"},
        {.name = "dot slash", .fs = {"file"}, .open = "./"},
        {.name = "file slash", .fs = {"target"}, .open = "target/"},
        {.name = "file in path", .fs = {"file"}, .open = "file/target"},
        {.name = "directory in path missing", .open = "dir/target"},
        {.name = "target does not exist", .open = "target"},
        {.name = "symlink slash",
         .fs = {"target/file", "link => target"},
         .open = "link/",
         .check = [](const std::string& t) -> std::string {
             // Linux does not resolve "symlink" in rename("symlink/",
             // "target").
             if (t.starts_with("TestRootConsistencyRename/")) return "known inconsistency on linux";
             // Root.RemoveAll and os.RemoveAll are not always consistent
             // here.
             if (t.starts_with("TestRootConsistencyRemoveAll/")) return "known inconsistency in RemoveAll";
             return "";
         }},
        {.name = "symlink slash dot", .fs = {"target/file", "link => target"}, .open = "link/."},
        {.name = "unresolved symlink", .fs = {"link => target"}, .open = "link"},
        {.name = "resolved symlink", .fs = {"link => target", "target"}, .open = "link"},
        {.name = "dotdot in path after symlink", .fs = {"a => b/c", "b/c/", "b/target"}, .open = "a/../target"},
        {.name = "symlink to dir ends in slash", .fs = {"dir/", "link => dir/"}, .open = "link"},
        {.name = "symlink to file ends in slash", .fs = {"file", "link => file/"}, .open = "link"},
        {.name = "long file name", .open = std::string(500, 'a')},
        {.name = "unreadable directory",
         .fs = {"dir/target"},
         .fsFunc =
             [](caseState& st, const std::string& dir) {
                 os::Chmod(join(dir, "dir"), 0);
                 st.cleanups.push_back([dir] { os::Chmod(join(dir, "dir"), 0700); });
                 return dir;
             },
         .open = "dir/target"},
        {.name = "unix domain socket target",
         .fsFunc = [](caseState& st, const std::string&) { return tempDirWithUnixSocket(st, "a"); },
         .open = "a"},
        {.name = "unix domain socket in path",
         .fsFunc = [](caseState& st, const std::string&) { return tempDirWithUnixSocket(st, "a"); },
         .open = "a/b"},
        {.name = "question mark", .open = "?"},
        {.name = "nul byte", .open = std::string(1, '\0')},
    };
    return cases;
}

std::string statString(const ptr<FileInfo>& fi) {
    return std::format("name:{} size:{} mode:{} isdir:{}", q(fi->Name().view()), fi->Size(), fi->Mode(), fi->IsDir());
}

}  // namespace

TEST(GoOsRoot, RootConsistencyOpen) {
    for (auto& test : rootConsistencyTestCases()) {
        test.run("TestRootConsistencyOpen", [](const std::string& path, os::Root* r) -> std::pair<std::string, error> {
            auto [f, err] = r ? r->Open(path) : os::Open(path);
            if (err) return {"", err};
            auto [fi, serr] = f->Stat();
            std::pair<std::string, error> res;
            if (!serr && !fi->IsDir()) {
                auto [b, rerr] = io::ReadAll(*f);
                res = {std::string(asStringView(b)), rerr};
            } else {
                auto [names, rerr] = f->Readdirnames(-1);
                std::vector<std::string> v;
                for (auto& n : names) v.push_back(q(n.view()));
                std::ranges::sort(v);
                std::string s = "[";
                for (std::size_t i = 0; i < v.size(); i++) s += (i ? " " : "") + v[i];
                res = {s + "]", rerr};
            }
            f->Close();
            return res;
        });
    }
}

TEST(GoOsRoot, RootConsistencyCreate) {
    for (auto& test : rootConsistencyTestCases()) {
        test.run("TestRootConsistencyCreate", [](const std::string& path, os::Root* r) -> std::pair<std::string, error> {
            auto [f, err] = r ? r->Create(path) : os::Create(path);
            if (!err) {
                f->Write(b("file contents"));
                f->Close();
            }
            return {"", err};
        });
    }
}

TEST(GoOsRoot, RootConsistencyChmod) {
    for (auto& test : rootConsistencyTestCases()) {
        test.run("TestRootConsistencyChmod", [](const std::string& path, os::Root* r) -> std::pair<std::string, error> {
            auto chmod = [&](FileMode m) { return r ? r->Chmod(path, m) : os::Chmod(path, m); };
            auto lstat = [&] { return r ? r->Lstat(path) : os::Lstat(path); };
            FileMode m1, m2;
            if (error err = chmod(0555)) return {"chmod 0o555", err};
            auto [fi, err] = lstat();
            if (!err) m1 = fi->Mode();
            if (error cerr = chmod(0777)) return {"chmod 0o777", cerr};
            std::tie(fi, err) = lstat();
            if (!err) m2 = fi->Mode();
            return {std::format("{} {}", m1, m2), err};
        });
    }
}

TEST(GoOsRoot, RootConsistencyMkdir) {
    for (auto& test : rootConsistencyTestCases()) {
        test.run("TestRootConsistencyMkdir", [](const std::string& path, os::Root* r) -> std::pair<std::string, error> {
            return {"", r ? r->Mkdir(path, 0777) : os::Mkdir(path, 0777)};
        });
    }
}

TEST(GoOsRoot, RootConsistencyMkdirAll) {
    for (auto& test : rootConsistencyTestCases()) {
        test.run("TestRootConsistencyMkdirAll", [](const std::string& path, os::Root* r) -> std::pair<std::string, error> {
            return {"", r ? r->MkdirAll(path, 0777) : os::MkdirAll(path, 0777)};
        });
    }
}

TEST(GoOsRoot, RootConsistencyRemove) {
    for (auto& test : rootConsistencyTestCases()) {
        if (test.open == "." || test.open == "./") continue;  // can't remove the root itself
        test.run("TestRootConsistencyRemove", [](const std::string& path, os::Root* r) -> std::pair<std::string, error> {
            return {"", r ? r->Remove(path) : os::Remove(path)};
        });
    }
}

TEST(GoOsRoot, RootConsistencyRemoveAll) {
    for (auto& test : rootConsistencyTestCases()) {
        if (test.open == "." || test.open == "./") continue;  // can't remove the root itself
        test.run("TestRootConsistencyRemoveAll", [](const std::string& path, os::Root* r) -> std::pair<std::string, error> {
            return {"", r ? r->RemoveAll(path) : os::RemoveAll(path)};
        });
    }
}

TEST(GoOsRoot, RootConsistencyStat) {
    for (auto& test : rootConsistencyTestCases()) {
        test.run("TestRootConsistencyStat", [](const std::string& path, os::Root* r) -> std::pair<std::string, error> {
            auto [fi, err] = r ? r->Stat(path) : os::Stat(path);
            if (err) return {"", err};
            return {statString(fi), nullptr};
        });
    }
}

TEST(GoOsRoot, RootConsistencyLstat) {
    for (auto& test : rootConsistencyTestCases()) {
        test.run("TestRootConsistencyLstat", [](const std::string& path, os::Root* r) -> std::pair<std::string, error> {
            auto [fi, err] = r ? r->Lstat(path) : os::Lstat(path);
            if (err) return {"", err};
            return {statString(fi), nullptr};
        });
    }
}

TEST(GoOsRoot, RootConsistencyReadlink) {
    for (auto& test : rootConsistencyTestCases()) {
        test.run("TestRootConsistencyReadlink", [](const std::string& path, os::Root* r) -> std::pair<std::string, error> {
            auto [s, err] = r ? r->Readlink(path) : os::Readlink(path);
            return {str(s), err};
        });
    }
}

namespace {

void testRootConsistencyMove(const std::string& testName, bool rename) {
    // Run this test in two directions: Renaming the test path to a
    // known-good path (from), and renaming a known-good path to the test
    // path (to).
    for (std::string name : {"from", "to"}) {
        SCOPED_TRACE(name);
        for (auto& test : rootConsistencyTestCases()) {
            test.run(testName + "/" + name, [&](const std::string& path, os::Root* r) -> std::pair<std::string, error> {
                auto move = [&](const std::string& oldname, const std::string& newname) {
                    if (rename) return r ? r->Rename(oldname, newname) : os::Rename(oldname, newname);
                    return r ? r->Link(oldname, newname) : os::Link(oldname, newname);
                };
                auto lstat = [&](const std::string& p) { return r ? r->Lstat(p) : os::Lstat(p); };

                std::optional<TempDir> tmp;
                std::string otherPath = "other";
                if (!r) {
                    tmp.emplace();
                    otherPath = join(*tmp, otherPath);
                }

                std::string srcPath, dstPath;
                if (name == "from") {
                    srcPath = path;
                    dstPath = otherPath;
                } else {
                    srcPath = otherPath;
                    dstPath = path;
                }

                if (!rename) {
                    // When the source is a symlink, Root.Link creates a hard
                    // link to the symlink. os.Link does whatever the link
                    // syscall does, which varies between operating systems
                    // and their versions. Skip running the consistency test
                    // when the source is a symlink.
                    if (auto [fi, err] = lstat(srcPath); !err && (fi->Mode() & os::ModeSymlink)) return {"", nullptr};
                }

                if (error err = move(srcPath, dstPath)) return {"", err};
                auto [fi, err] = lstat(dstPath);
                if (err) {
                    ADD_FAILURE() << "stat(" << q(dstPath) << ") after successful copy: " << err;
                    return {"stat error", err};
                }
                return {statString(fi), nullptr};
            });
        }
    }
}

}  // namespace

TEST(GoOsRoot, RootConsistencyRename) { testRootConsistencyMove("TestRootConsistencyRename", true); }

TEST(GoOsRoot, RootConsistencyLink) { testRootConsistencyMove("TestRootConsistencyLink", false); }

TEST(GoOsRoot, RootConsistencySymlink) {
    for (auto& test : rootConsistencyTestCases()) {
        test.run("TestRootConsistencySymlink", [](const std::string& path, os::Root* r) -> std::pair<std::string, error> {
            const std::string target = "linktarget";
            error err;
            string got;
            if (!r) {
                err = os::Symlink(target, path);
                got = os::Readlink(target).first;
            } else {
                err = r->Symlink(target, path);
                got = r->Readlink(target).first;
            }
            return {str(got), err};
        });
    }
}

TEST(GoOsRoot, RootRenameAfterOpen) {
    TempDir dir;

    // Create directory "a" and open it.
    ASSERT_FALSE(os::Mkdir(join(dir, "a"), 0777));
    auto [dirf, err] = os::OpenRoot(join(dir, "a"));
    ASSERT_FALSE(err) << err;

    // Rename "a" => "b", and create "b/f".
    ASSERT_FALSE(os::Rename(join(dir, "a"), join(dir, "b")));
    ASSERT_FALSE(os::WriteFile(join(dir, "b/f"), "hello", 0666));

    // Open "f", and confirm that we see it.
    auto [f, oerr] = dirf->OpenFile("f", os::O_RDONLY_, 0);
    ASSERT_FALSE(oerr) << "reading file after renaming parent: " << oerr;
    EXPECT_EQ(readAll(*f), "hello") << "file contents";

    // f.Name reflects the original path we opened the directory under
    // (".../a"), not "b".
    EXPECT_EQ(str(f->Name()), str(dirf->Name()) + os::PathSeparator + "f") << "f.Name()";
    f->Close();
    dirf->Close();
}

TEST(GoOsRoot, RootNonPermissionMode) {
    TempDir dir;
    auto [r, err] = os::OpenRoot(dir.path());
    ASSERT_FALSE(err) << err;
    EXPECT_TRUE(r->OpenFile("file", os::O_RDWR_ | os::O_CREATE_, 01777).second)
        << "r.OpenFile(file, O_RDWR|O_CREATE, 0o1777) succeeded; want error";
    EXPECT_TRUE(r->Mkdir("file", 01777)) << "r.Mkdir(file, 0o1777) succeeded; want error";
    r->Close();
}

TEST(GoOsRoot, RootUseAfterClose) {
    TempDir dir;
    auto [r, err] = os::OpenRoot(dir.path());
    ASSERT_FALSE(err) << err;
    r->Close();
    struct test {
        std::string name;
        std::function<error(os::Root&, std::string_view)> f;
    } tests[] = {
        {"Open", [](os::Root& r, std::string_view n) { return r.Open(n).second; }},
        {"Create", [](os::Root& r, std::string_view n) { return r.Create(n).second; }},
        {"OpenFile", [](os::Root& r, std::string_view n) { return r.OpenFile(n, os::O_RDWR_, 0666).second; }},
        {"OpenRoot", [](os::Root& r, std::string_view n) { return r.OpenRoot(n).second; }},
        {"Mkdir", [](os::Root& r, std::string_view n) { return r.Mkdir(n, 0777); }},
    };
    for (auto& test : tests) {
        error err = test.f(*r, "target");
        auto* pe = dynamic_cast<const os::PathError*>(err.get());
        EXPECT_TRUE(pe && pe->Path.view() == "target" && pe->Err == os::ErrClosed)
            << "r." << test.name << " = " << err << "; want &PathError{Path: \"target\", Err: ErrClosed}";
    }
}

TEST(GoOsRoot, RootConcurrentClose) {
    TempDir dir;
    auto [r, err] = os::OpenRoot(dir.path());
    ASSERT_FALSE(err) << err;
    auto ch = make_chan<error>(1);
    auto done = make_chan<int>(1);
    go([r, ch, done] {
        bool first = true;
        for (;;) {
            auto [f, err] = r->OpenFile("file", os::O_RDWR_ | os::O_CREATE_, 0666);
            if (err) {
                ch.send(err);
                break;
            }
            if (first) {
                ch.send(nullptr);
                first = false;
            }
            f->Close();
        }
        done.send(0);
    });
    error e1 = ch.recv();
    EXPECT_FALSE(e1) << "OpenFile: " << e1 << ", want success";
    r->Close();
    error e2 = ch.recv();
    EXPECT_TRUE(errors::Is(e2, os::ErrClosed)) << "OpenFile: " << e2 << ", want ErrClosed";
    done.recv();
}

// TestRootRaceRenameDir attempts to escape a Root by renaming a path
// component mid-parse.
//
// We create a deeply nested directory:
//
//	base/a/a/a/a/ [...] /a
//
// And a path that descends into the tree, then returns to the top using
// ..:
//
//	base/a/a/a/a/ [...] /a/../../../ [..] /../a/f
//
// While opening this file, we rename base/a/a to base/b. A naive lookup
// operation will resolve the path to base/f.
TEST(GoOsRoot, RootRaceRenameDir) {
    TempDir tmp;
    std::string dir = tmp.path();
    auto [r, err] = os::OpenRoot(dir);
    ASSERT_FALSE(err) << err;

    constexpr int depth = 4;
    std::string as, ups;
    for (int i = 0; i < depth; i++) {
        as += "a/";
        ups += "../";
    }
    os::MkdirAll(dir + "/base/" + as, 0777);

    std::string path = "base/" + as + ups + "a/f";
    os::WriteFile(dir + "/f", "secret", 0666);
    os::WriteFile(dir + "/base/a/f", "public", 0666);

    // Compute how long it takes to open the path in the common case.
    constexpr int tries = 10;
    time::Duration total{};
    for (int i = 0; i < tries; i++) {
        time::Time start = time::Now();
        auto [f, oerr] = r->Open(path);
        ASSERT_FALSE(oerr) << oerr;
        ASSERT_EQ(readAll(*f), "public");
        f->Close();
        total += time::Since(start);
    }
    time::Duration avg = total / tries;

    // We're trying to exploit a race, so try this a number of times.
    for (int i = 0; i < 100; i++) {
        // Start a goroutine to open the file.
        auto gotc = make_chan<std::string>();
        go([r = r, path, gotc] {
            auto [f, err] = r->Open(path);
            if (err) {
                gotc.send("");
                return;
            }
            auto [b, rerr] = io::ReadAll(*f);
            f->Close();
            gotc.send(std::string(asStringView(b)));
        });

        // Wait for the open operation to partially complete, and then
        // rename a directory near the root.
        time::Sleep(avg / 4);
        ASSERT_FALSE(os::Rename(dir + "/base/a", dir + "/b"));

        std::string got = gotc.recv();
        os::Rename(dir + "/b", dir + "/base/a");
        EXPECT_TRUE(got.empty() || got == "public") << "read file: " << q(got) << "; want error or 'public'";
    }
    r->Close();
}

TEST(GoOsRoot, RootSymlinkToRoot) {
    TempDir tmp;
    std::string dir = makefs(tmp, {"d/d => .."});
    auto [root, err] = os::OpenRoot(dir);
    ASSERT_FALSE(err) << err;
    ASSERT_FALSE(root->Mkdir("d/d/new", 0777));
    auto [f, oerr] = root->Open("d/d");
    ASSERT_FALSE(oerr) << oerr;
    auto [names, rerr] = f->Readdirnames(-1);
    ASSERT_FALSE(rerr) << rerr;
    std::vector<std::string> got;
    for (auto& n : names) got.push_back(str(n));
    std::ranges::sort(got);
    EXPECT_EQ(got, (std::vector<std::string>{"d", "new"})) << "root contains";
    f->Close();
    root->Close();
}

TEST(GoOsRoot, OpenInRoot) {
    TempDir tmp;
    std::string dir = makefs(tmp, {"file", "link => ../ROOT/file"});
    auto [f, err] = os::OpenInRoot(dir, "file");
    ASSERT_FALSE(err) << "OpenInRoot(`file`) = " << err << ", want success";
    f->Close();
    for (std::string name : {std::string("link"), std::string("../ROOT/file"), dir + "/file"}) {
        auto [f2, err2] = os::OpenInRoot(dir, name);
        if (!err2) {
            f2->Close();
            FAIL() << "OpenInRoot(" << q(name) << ") = nil, want error";
        }
    }
}

TEST(GoOsRoot, RootRemoveDot) {
    TempDir dir;
    auto [root, err] = os::OpenRoot(dir.path());
    ASSERT_FALSE(err) << err;
    EXPECT_TRUE(root->Remove(".")) << "root.Remove(\".\") = nil, want error";
    EXPECT_TRUE(root->RemoveAll(".")) << "root.RemoveAll(\".\") = nil, want error";
    EXPECT_FALSE(os::Stat(dir.path()).second) << "root.Remove(All)?(\".\") removed the root";
    root->Close();
}

TEST(GoOsRoot, RootWriteReadFile) {
    TempDir dir;
    auto [root, err] = os::OpenRoot(dir.path());
    ASSERT_FALSE(err) << err;
    std::string name = "filename";
    std::string want = "file contents";
    ASSERT_FALSE(root->WriteFile(name, want, 0666)) << "root.WriteFile(" << q(name) << ")";
    auto [got, rerr] = root->ReadFile(name);
    ASSERT_FALSE(rerr) << "root.ReadFile(" << q(name) << ") = " << rerr;
    EXPECT_EQ(asStringView(got), want);
    root->Close();
}

TEST(GoOsRoot, RootName) {
    TempDir tmp;
    std::string dir = tmp.path();
    auto [root, err] = os::OpenRoot(dir);
    ASSERT_FALSE(err) << err;
    EXPECT_EQ(str(root->Name()), dir) << "root.Name()";

    auto [f, cerr] = root->Create("file");
    ASSERT_FALSE(cerr) << cerr;
    EXPECT_EQ(str(f->Name()), join(dir, "file")) << "root.Create(\"file\").Name()";
    f->Close();

    ASSERT_FALSE(root->Mkdir("dir", 0777));
    auto [subroot, serr] = root->OpenRoot("dir");
    ASSERT_FALSE(serr) << serr;
    EXPECT_EQ(str(subroot->Name()), join(dir, "dir")) << "root.OpenRoot(\"dir\").Name()";
    subroot->Close();
    root->Close();
}

// TestRootNoLstat verifies that reading directories in a Root gives the
// right results (Go also checks, with a stat hook, that they don't Lstat by
// name, which could escape the root).
TEST(GoOsRoot, RootNoLstat) {
    TempDir tmp;
    std::string dir = makefs(tmp, {"subdir/"});
    constexpr int size = 42;
    ASSERT_FALSE(os::WriteFile(dir + "/subdir/file", std::string(size, 'x'), 0666));
    auto [root, err] = os::OpenRoot(dir);
    ASSERT_FALSE(err) << err;

    auto test = [&](const std::string& name, const std::function<void(File&)>& fn) {
        SCOPED_TRACE(name);
        auto [f, oerr] = root->Open("subdir");
        ASSERT_FALSE(oerr) << oerr;
        fn(*f);
        f->Close();
    };

    auto checkFileInfo = [](const ptr<FileInfo>& fi) {
        EXPECT_EQ(fi->Name().view(), "file") << "FileInfo.Name()";
        EXPECT_EQ(fi->Size(), int64(size)) << "FileInfo.Size()";
    };
    auto checkDirEntry = [&](const ptr<os::DirEntry>& d) {
        EXPECT_EQ(d->Name().view(), "file") << "DirEntry.Name()";
        EXPECT_FALSE(d->IsDir()) << "DirEntry.IsDir()";
        auto [fi, err] = d->Info();
        ASSERT_FALSE(err) << "DirEntry.Info() = _, " << err;
        checkFileInfo(fi);
    };

    test("Stat", [](File& subdir) {
        auto [fi, err] = subdir.Stat();
        ASSERT_FALSE(err) << err;
        ASSERT_TRUE(fi->IsDir()) << "Open(\"subdir\").Stat().IsDir() = false, want true";
    });
    // File.ReadDir, returning []DirEntry
    test("ReadDirEntry", [&](File& subdir) {
        auto [dirents, err] = subdir.ReadDir(-1);
        ASSERT_FALSE(err) << err;
        ASSERT_EQ(dirents.size(), 1u) << "Open(\"subdir\").ReadDir(-1), want {file}";
        checkDirEntry(dirents[0]);
    });
    // File.Readdir, returning []FileInfo
    test("ReadFileInfo", [&](File& subdir) {
        auto [fileinfos, err] = subdir.Readdir(-1);
        ASSERT_FALSE(err) << err;
        ASSERT_EQ(fileinfos.size(), 1u) << "Open(\"subdir\").Readdir(-1), want {file}";
        checkFileInfo(fileinfos[0]);
    });
    // File.Readdirnames, returning []string
    test("Readdirnames", [](File& subdir) {
        auto [names, err] = subdir.Readdirnames(-1);
        ASSERT_FALSE(err) << err;
        ASSERT_TRUE(names.size() == 1 && names[0].view() == "file") << "Open(\"subdir\").Readdirnames(-1), want [file]";
    });
    root->Close();
}

// ---------------------------------------------------------------------------
// root_unix_test.go

namespace {

void checkUidGid(std::string_view path, int uid, int gid) {
    auto [dir, err] = os::Lstat(path);
    ASSERT_FALSE(err) << "Lstat " << path << " (looking for uid/gid " << uid << "/" << gid << "): " << err;
    auto* sys = static_cast<const struct stat*>(dir->Sys());
    EXPECT_EQ(int(sys->st_uid), uid) << "Lstat " << path;
    EXPECT_EQ(int(sys->st_gid), gid) << "Lstat " << path;
}

std::vector<int> groupsAndGid() {
    auto [groups, err] = os::Getgroups();
    EXPECT_FALSE(err) << err;
    groups.push_back(os::Getgid());
    return groups;
}

int defaultUid() {
    NewFile f;
    auto [dir, err] = f->Stat();
    EXPECT_FALSE(err) << err;
    return int(static_cast<const struct stat*>(dir->Sys())->st_uid);
}

int firstGroup() {
    auto [groups, err] = os::Getgroups();
    EXPECT_FALSE(err) << err;
    return groups.empty() ? os::Getgid() : groups[0];
}

std::string uidGid(const ptr<FileInfo>& fi) {
    auto* sys = static_cast<const struct stat*>(fi->Sys());
    return std::format("{} {}", sys->st_uid, sys->st_gid);
}

}  // namespace

TEST(GoOsRoot, RootChown) {
    // Look up the current default uid/gid.
    int uid = defaultUid();
    auto groups = groupsAndGid();
    for (auto& test : rootTestCases) {
        test.run([&](const std::string& target, os::Root& root) {
            if (!target.empty()) { ASSERT_FALSE(os::WriteFile(target, "", 0666)); }
            for (int gid : groups) {
                error err = root.Chown(test.open, -1, gid);
                if (errEndsTest(err, test.wantError, std::format("root.Chown({}, -1, {})", q(test.open), gid))) return;
                checkUidGid(target, uid, gid);
            }
        });
    }
}

TEST(GoOsRoot, RootLchown) {
    // Look up the current default uid/gid.
    int uid = defaultUid();
    auto groups = groupsAndGid();
    for (auto test : rootTestCases) {
        test.run([&](std::string target, os::Root& root) {
            if (!test.ltarget.empty()) {
                test.wantError = false;
                target = join(root.Name().view(), test.ltarget);
            } else if (!target.empty()) {
                ASSERT_FALSE(os::WriteFile(target, "", 0666));
            }
            for (int gid : groups) {
                error err = root.Lchown(test.open, -1, gid);
                if (errEndsTest(err, test.wantError, std::format("root.Lchown({}, -1, {})", q(test.open), gid))) return;
                checkUidGid(target, uid, gid);
            }
        });
    }
}

TEST(GoOsRoot, RootConsistencyChown) {
    int gid = firstGroup();
    for (auto& test : rootConsistencyTestCases()) {
        test.run("TestRootConsistencyChown", [&](const std::string& path, os::Root* r) -> std::pair<std::string, error> {
            if (error err = r ? r->Chown(path, -1, gid) : os::Chown(path, -1, gid)) return {"", err};
            auto [fi, err] = r ? r->Lstat(path) : os::Lstat(path);
            if (err) return {"", err};
            return {uidGid(fi), nullptr};
        });
    }
}

TEST(GoOsRoot, RootConsistencyLchown) {
    int gid = firstGroup();
    for (auto& test : rootConsistencyTestCases()) {
        test.run("TestRootConsistencyLchown", [&](const std::string& path, os::Root* r) -> std::pair<std::string, error> {
            if (error err = r ? r->Lchown(path, -1, gid) : os::Lchown(path, -1, gid)) return {"", err};
            auto [fi, err] = r ? r->Lstat(path) : os::Lstat(path);
            if (err) return {"", err};
            return {uidGid(fi), nullptr};
        });
    }
}

// ---------------------------------------------------------------------------
// The rootMultiTest tests of root_test.go: each operation in a matrix of
// file descriptions, in and out of a Root, checked against expectations
// and for consistency. As Go does without -root_comprehensive. Go's
// t.Fatal inside a case ends it (here, by throwing rootFatal).

namespace {

struct rootFatal {};

[[noreturn]] void fatal(const std::string& msg) {
    ADD_FAILURE() << msg;
    throw rootFatal{};
}

// testFileKind is a kind of file.
enum testFileKind {
    testFileUnused,
    testFileAbsent,    // file does not exist
    testFileFile,      // regular file
    testFileDir,       // directory
    testFileSymlink,   // symlink
    testFileJunction,  // Windows directory junction
    testFileMax,
    // testFileError represents a path which fails during resolution, such
    // as "a/b" where "a" does not exist.
    testFileError,
};

std::string kindString(testFileKind kind) {
    switch (kind) {
    case testFileUnused: return "unused";
    case testFileAbsent: return "absent";
    case testFileFile: return "file";
    case testFileDir: return "dir";
    case testFileSymlink: return "symlink";
    case testFileJunction: return "junction";
    case testFileError: return "error";
    default: return std::format("testFileKind({})", int(kind));
    }
}

bool isLink(testFileKind kind) { return kind == testFileSymlink || kind == testFileJunction; }

// testFileRef is a kind of reference to a file: f, ./f, /abs/path/to/f,
// somedir/../f, etc.
struct testFileRef {
    // name is the name of the reference (not the file name): s (/ slash),
    // p (.. parent), b (base), d (directory), r (root)
    std::string name;
    // template is a template path. Templates assume that the file is
    // contained in a directory named "ROOT", and that "ROOT/DIR" exists and
    // is a directory. BASE is replaced with the file's basename. Absolute
    // path templates start with /ROOT.
    std::string tmpl;
    // escapes indicates whether the path escapes the current directory.
    bool escapes = false;

    // path creates a path using the template. dir is the absolute path to
    // the root directory (named "ROOT"), base the name of the target file
    // within it.
    std::string path(const std::string& dir, const std::string& base) const {
        std::string p = tmpl;
        for (std::size_t i; (i = p.find("BASE")) != p.npos;) p.replace(i, 4, base);
        if (p.starts_with("/ROOT")) p = dir + p.substr(5);
        return p;
    }
    bool hasSlashSuffix() const { return tmpl.ends_with("/"); }
};

const std::vector<testFileRef> testFileRefs = {
    {.name = "b", .tmpl = "BASE", .escapes = false},
    {.name = "bs", .tmpl = "BASE/", .escapes = false},
    {.name = "dpb", .tmpl = "DIR/../BASE", .escapes = false},
    {.name = "dpbs", .tmpl = "DIR/../BASE/", .escapes = false},
    {.name = "prb", .tmpl = "../ROOT/BASE", .escapes = true},
    {.name = "prbs", .tmpl = "../ROOT/BASE/", .escapes = true},
    {.name = "srb", .tmpl = "/ROOT/BASE", .escapes = true},
    {.name = "srbs", .tmpl = "/ROOT/BASE/", .escapes = true},
};

// testFileLimitedRefs is a smaller set of references which do not exercise
// path escapes.
const std::vector<testFileRef> testFileLimitedRefs(testFileRefs.begin(), testFileRefs.begin() + 2);

// testFileDesc is a description of a type of file, combining the kind and
// reference type.
struct testFileDesc {
    testFileKind kind = testFileUnused;
    testFileRef ref = {};
    std::shared_ptr<testFileDesc> target = {};  // symlink target, nil when !isLink(kind)

    // String returns the target name, cryptically: "bsSdpbD" is "BASE/", a
    // symlink, to "DIR/../BASE", a directory.
    std::string String() const {
        std::string s = ref.name + char(std::toupper(kindString(kind)[0]));
        if (isLink(kind)) s += target->String();
        return s;
    }

    // escapes reports whether accessing this file escapes the root, either
    // because the file name escapes or because some element of a symlink
    // chain escapes.
    bool escapes() const {
        if (ref.escapes) return true;
        if (kind == testFileSymlink) return target->escapes();
        return kind == testFileJunction;
    }

    bool lescapes() const {
        if (ref.escapes) return true;
        // On POSIX filesystems, a trailing slash at the end of a path causes
        // symlinks in the last path component to be resolved.
        if (ref.hasSlashSuffix() && isLink(kind)) return target->escapes();
        return false;
    }

    // finalKind reports the kind of the file after following all symlinks.
    testFileKind finalKind() const { return isLink(kind) ? target->finalKind() : kind; }

    testFileKind lfinalKind() const {
        if (ref.hasSlashSuffix() && isLink(kind)) return target->finalKind();
        return kind;
    }

    bool isError() const { return isErrorWith(*this, false); }

    static bool isErrorWith(const testFileDesc& desc, bool hasSuffix) {
        if (desc.ref.escapes) return false;
        if (desc.ref.hasSlashSuffix()) hasSuffix = true;
        switch (desc.kind) {
        case testFileDir: return false;
        case testFileSymlink: return isErrorWith(*desc.target, hasSuffix);
        case testFileJunction:
            if (desc.target->kind == testFileFile || (hasSuffix && desc.target->kind != testFileDir)) return true;
            return isErrorWith(*desc.target, hasSuffix);
        default: return hasSuffix;
        }
    }

    // anySlashSuffix reports whether any of the names in the file (either
    // the initial name, or a symlink target) include a trailing /.
    bool anySlashSuffix() const {
        if (slashSuffix()) return true;
        if (isLink(kind)) return target->anySlashSuffix();
        return false;
    }

    // slashSuffix reports whether the name of the file includes a trailing
    // /.
    bool slashSuffix() const { return !ref.tmpl.empty() && os::IsPathSeparator(ref.tmpl.back()); }

    // create creates the file(s) for this descriptor. dir is the test root
    // directory, base the base name of the file we will open within the
    // root (the start of the symlink chain). token identifies the file:
    // when symlinks are involved, it names the final, non-symlink file.
    ptr<FileInfo> create(const std::string& dir, const std::string& base, const std::string& token) const {
        std::string path = join(dir, base);
        ptr<FileInfo> fi;
        switch (kind) {
        case testFileAbsent:
            // File does not exist.
            break;
        case testFileFile:
            // Regular file. We use the token as the file contents.
            if (error err = os::WriteFile(path, token, 0666)) fatal(str(err.Error()));
            break;
        case testFileDir:
            if (error err = os::Mkdir(path, 0777)) fatal(str(err.Error()));
            break;
        case testFileSymlink: {
            // Symlink. We create a symlink target named "s_"+base.
            std::string linktarget = target->ref.path(dir, "s_" + base);
            fi = target->create(dir, "s_" + base, token);
            if (error err = os::Symlink(linktarget, path)) fatal(str(err.Error()));
            break;
        }
        default: fatal("can't create file of kind: " + kindString(kind));
        }
        if (kind == testFileFile || kind == testFileDir) {
            auto [info, err] = os::Lstat(path);
            if (err) fatal(str(err.Error()));
            fi = info;
        }
        return fi;
    }
};

// allTestFileDescs returns all the testFileDescs we use in tests: every
// type of reference to every type of file (except symlink), then direct
// and direct/ references to a symlink to every reference to a file or
// directory, and to a direct reference to a missing file.
const std::vector<testFileDesc>& allTestFileDescs() {
    static const std::vector<testFileDesc> descs = [] {
        std::vector<testFileDesc> v;
        for (auto& ref : testFileRefs) {
            for (int k = 0; k < testFileMax; k++) {
                auto kind = testFileKind(k);
                if (kind == testFileUnused || isLink(kind)) continue;
                v.push_back({.kind = kind, .ref = ref});
            }
        }
        // Only direct references to symlinks; no junctions off Windows.
        for (auto& ref : testFileLimitedRefs) {
            for (int k = 0; k < testFileMax; k++) {
                auto linkKind = testFileKind(k);
                if (linkKind == testFileUnused || isLink(linkKind)) continue;
                const auto& linkRefs =
                    linkKind != testFileFile && linkKind != testFileDir ? testFileLimitedRefs : testFileRefs;
                for (auto& linkRef : linkRefs) {
                    v.push_back({.kind = testFileSymlink,
                                 .ref = ref,
                                 .target = std::make_shared<testFileDesc>(testFileDesc{.kind = linkKind, .ref = linkRef})});
                }
            }
        }
        return v;
    }();
    return descs;
}

// A rootMultiTest is state for testing an os.Root operation in one
// configuration among many: with or without a Root, with a target that may
// be a file, directory, symlink, or entirely absent, referenced by various
// paths ("target", "DIR/../target", etc.), and when the target is a
// symlink, with various link target paths.
struct rootMultiTest {
    // dir is the directory containing the test. dir will always contain a
    // directory named "ROOT" and a subdir named "ROOT/DIR".
    std::string dir;
    // root is the Root for the test. May be nil.
    os::Root* root = nullptr;
    // source and target are files acted on by the test. target is always
    // set; source is only set for tests which request two files.
    testFileDesc source, target;
    // sourcePath and targetPath are the paths which should be used to
    // access the source/target.
    std::string sourcePath, targetPath;
    ptr<FileInfo> sourceInfo, targetInfo;
    // op is the operation being performed, used for reporting errors.
    std::string op;
    caseState* st = nullptr;

    // setOp sets the operation performed by the test (logged in errors).
    void setOp(const std::string& s) { op = (root ? "root." : "os.") + s; }

    void errorf(const std::string& msg) { fatal(op + ":\n  " + msg); }

    // wantError tests whether got matches want. If want is errAny, got may
    // be any non-nil error.
    void wantError(const error& got, const error& want) const;

    // describeFile returns a string identifying a file: "" if f is nil,
    // "source" or "target" for the test's files, otherwise "unknown file".
    std::string describeFile(const ptr<File>& f) const {
        if (!f) return "";
        auto [fi, err] = f->Stat();
        if (err) fatal(str(err.Error()));
        if (sourceInfo && os::SameFile(fi, sourceInfo)) return "source";
        if (targetInfo && os::SameFile(fi, targetInfo)) return "target";
        return "unknown file";
    }
};

const error errAny = errors::New("any error");

// A rootMultiTest function may return this error to disable the check that
// in-root and out-of-root functions have the same outcome.
const error errSkipRootConsistencyCheck = errors::New("skip root consistency check");

void rootMultiTest::wantError(const error& got, const error& want) const {
    if (errors::Is(got, want) || (got && want == errAny)) return;
    fatal(std::format("{}:\ngot error:  {}\nwant error: {}", op, got, want));
}

// dirTreeContents returns a description of the contents of directory, such
// as:
//
//	drwxrwxrwx dir/
//	-rw-rw-rw- dir/file "file contents"
//	Lrw-rw-rw- symlink => dir/file
std::vector<std::string> dirTreeContents(const std::string& dir) {
    auto [root, err] = os::OpenRoot(dir);
    if (err) fatal(str(err.Error()));
    std::vector<std::string> contents;
    fs::WalkDir(*root->FS(), ".", [&](std::string_view path, const ptr<fs::DirEntry>& d, error) -> error {
        if (path == ".") return nullptr;
        auto [info, ierr] = d->Info();
        if (ierr) fatal(str(ierr.Error()));
        std::string ent = str(info->Mode().String()) + " " + std::string(path);
        auto type = d->Type();
        if (type == fs::ModeDir) {
            ent += "/";
        } else if (type == fs::ModeSymlink || type == fs::ModeIrregular) {
            auto [t, rerr] = root->Readlink(path);
            if (rerr) fatal(str(rerr.Error()));
            std::string target = str(t);
            // filepath.Rel(dir, target), when it's local.
            if (target.starts_with(dir + "/")) {
                std::string rel = str(path::Clean(std::string_view(target).substr(dir.size() + 1)));
                if (rel != ".." && !rel.starts_with("../")) target = "/.../" + rel;
            }
            ent += " => " + target;
        } else {
            auto [f, oerr] = root->Open(path);
            if (oerr) {
                ent += " (unreadable)";
            } else {
                auto [content, rerr] = io::ReadAll(*f);
                f->Close();
                if (rerr) fatal(str(rerr.Error()));
                ent += " " + q(asStringView(content));
            }
        }
        contents.push_back(ent);
        return nullptr;
    });
    root->Close();
    return contents;
}

std::unique_ptr<rootMultiTest> newRootTest(caseState& st, const testFileDesc& source, const testFileDesc& target,
                                           bool inRoot) {
    auto test = std::make_unique<rootMultiTest>();
    test->dir = makefs(st.tempDir(), {"DIR/"});
    test->st = &st;
    if (inRoot) {
        auto [root, err] = os::OpenRoot(test->dir);
        if (err) fatal(str(err.Error()));
        test->root = root.get();
        st.cleanups.push_back([root] { root->Close(); });
    }
    test->source = source;
    test->target = target;
    auto createFile = [&](const std::string& name, const testFileDesc& desc) -> std::pair<std::string, ptr<FileInfo>> {
        if (desc.kind == testFileUnused) return {"", nullptr};
        auto fi = desc.create(test->dir, name, name);
        std::string path = desc.ref.path(test->dir, name);
        if (!inRoot && !path::IsAbs(path)) path = test->dir + "/" + path;
        return {path, fi};
    };
    std::tie(test->sourcePath, test->sourceInfo) = createFile("source", source);
    std::tie(test->targetPath, test->targetInfo) = createFile("target", target);
    return test;
}

using multiFunc = std::function<std::pair<std::string, error>(rootMultiTest&)>;

void runRootMultiTestDescs(const testFileDesc& source, const testFileDesc& target, const multiFunc& f) {
    caseState st;
    try {
        auto rootTest = newRootTest(st, source, target, true);
        auto osTest = newRootTest(st, source, target, false);

        auto [rootResult, rootErr] = f(*rootTest);
        auto [osResult, osErr] = f(*osTest);

        if (errors::Is(rootErr, os::detail::errPathEscapes)) {
            // os.Root forbids this operation (and is therefore not
            // consistent with the non-root version).
            return;
        }
        if (rootErr == errSkipRootConsistencyCheck || osErr == errSkipRootConsistencyCheck) return;

        // Consistency check: Performing the same operation in and out of a
        // root should produce the same results.
        if (rootResult != osResult) {
            ADD_FAILURE() << "inconsistent results in/out of root\n"
                          << rootTest->op << ":\n  result: " << rootResult << "\n"
                          << osTest->op << ":\n  result: " << osResult;
        }
        if (!rootErr != !osErr) {
            ADD_FAILURE() << "inconsistent errors in/out of root\n"
                          << rootTest->op << ":\n  error: " << rootErr << "\n"
                          << osTest->op << ":\n  error: " << osErr;
        }

        // Filesystem consistency check: Same files in the same places.
        auto rootContent = dirTreeContents(rootTest->dir);
        auto osContent = dirTreeContents(osTest->dir);
        if (rootContent != osContent) {
            std::string msg = "inconsistent filesystem after running in/out of root\n" + rootTest->op + ":\n";
            for (auto& line : rootContent) msg += "  " + line + "\n";
            msg += osTest->op + ":\n";
            for (auto& line : osContent) msg += "  " + line + "\n";
            ADD_FAILURE() << msg;
        }
    } catch (const rootFatal&) {
    }
}

// runRootMultiTest runs f in a variety of configurations.
void runRootMultiTest(const multiFunc& f) {
    for (auto& target : allTestFileDescs()) {
        SCOPED_TRACE(target.String());
        runRootMultiTestDescs(testFileDesc{}, target, f);
    }
}

// runRootMultiTest2 runs f in a variety of configurations, with both
// source and target files: those where at least one of them is "simple",
// only direct references.
void runRootMultiTest2(const multiFunc& f) {
    auto isSimple = [](const testFileDesc& desc) {
        if (desc.ref.tmpl != "BASE") return false;
        if (isLink(desc.kind) && desc.target->ref.tmpl != "BASE") return false;
        return true;
    };
    for (auto& source : allTestFileDescs()) {
        for (auto& target : allTestFileDescs()) {
            if (!isSimple(source) && !isSimple(target)) continue;
            SCOPED_TRACE(source.String() + "_to_" + target.String());
            runRootMultiTestDescs(source, target, f);
        }
    }
}

const error& ErrPathEscapes = os::detail::errPathEscapes;

}  // namespace

// TestRootMultiOpen tests os.Root.Open.
TEST(GoOsRoot, RootMultiOpen) {
    runRootMultiTest([](rootMultiTest& test) -> std::pair<std::string, error> {
        test.setOp(std::format("Open({})", q(test.targetPath)));
        auto [f, gotErr] = test.root ? test.root->Open(test.targetPath) : os::Open(test.targetPath);
        struct closer {
            ptr<File> f;
            ~closer() {
                if (f) f->Close();
            }
        } c{f};
        std::string got = test.describeFile(f);

        if (test.target.isError()) {
            test.wantError(gotErr, errAny);
        } else if (test.root && test.target.escapes()) {
            // The operation escapes the root.
            test.wantError(gotErr, ErrPathEscapes);
        } else if (test.target.finalKind() == testFileAbsent) {
            // The file does not exist ("absent").
            test.wantError(gotErr, errAny);
        } else if (test.target.anySlashSuffix()) {
            // Trailing slashes are handled differently on different
            // platforms: only consistency is checked.
        } else {
            // We should have successfully opened the file.
            test.wantError(gotErr, nullptr);
            if (got != "target") fatal(std::format("opened file {}, want {}", q(got), q("target")));
        }
        return {got, gotErr};
    });
}

TEST(GoOsRoot, RootMultiChmod) {
    runRootMultiTest([](rootMultiTest& test) -> std::pair<std::string, error> {
        auto chmod = [&](const std::string& p, FileMode m) { return test.root ? test.root->Chmod(p, m) : os::Chmod(p, m); };
        auto stat = [&](const std::string& p) { return test.root ? test.root->Stat(p) : os::Stat(p); };

        // read+execute permissions let us list directory contents, and we
        // restore writability before deleting the temp dir.
        FileMode wantMode = 0500;
        os::Root* root = test.root;
        std::string targetPath = test.targetPath;
        test.st->cleanups.push_back([root, targetPath] {
            if (root) root->Chmod(targetPath, 0700);
            else os::Chmod(targetPath, 0700);
        });

        test.setOp(std::format("Chmod({}, {:o})", q(test.targetPath), uint32(wantMode)));
        error gotErr = chmod(test.targetPath, wantMode);

        FileMode gotMode;
        if (test.root && test.target.escapes()) {
            test.wantError(gotErr, ErrPathEscapes);
        } else if (test.target.finalKind() == testFileAbsent) {
            test.wantError(gotErr, errAny);
        } else if (test.target.anySlashSuffix()) {
            // Don't expect anything, just be consistent with the OS.
        } else {
            test.wantError(gotErr, nullptr);
            auto [fi, err] = stat(test.targetPath);
            if (err) fatal(std::format("could not stat target: {}", err));
            gotMode = fi->Mode() & fs::ModePerm;
            if (gotMode != wantMode) {
                fatal(std::format("file {}:\ngot mode:  {}\nwant mode: {}", q(test.targetPath), gotMode, wantMode));
            }
        }
        return {str(gotMode.String()), gotErr};
    });
}

TEST(GoOsRoot, RootMultiCreate) {
    runRootMultiTest([](rootMultiTest& test) -> std::pair<std::string, error> {
        test.setOp(std::format("Create({})", q(test.targetPath)));
        auto [f, gotErr] = test.root ? test.root->Create(test.targetPath) : os::Create(test.targetPath);
        if (!gotErr) f->Close();

        if (test.target.isError()) {
            test.wantError(gotErr, errAny);
        } else if (test.root && test.target.escapes()) {
            // The operation escapes the root.
            test.wantError(gotErr, ErrPathEscapes);
        }
        return {"", gotErr};
    });
}

TEST(GoOsRoot, RootMultiLink) {
    runRootMultiTest2([](rootMultiTest& test) -> std::pair<std::string, error> {
        test.setOp(std::format("Link({}, {})", q(test.sourcePath), q(test.targetPath)));
        error gotErr = test.root ? test.root->Link(test.sourcePath, test.targetPath)
                                 : os::Link(test.sourcePath, test.targetPath);

        if (test.root && test.source.lescapes()) {
            test.wantError(gotErr, ErrPathEscapes);
        } else if (test.source.lfinalKind() == testFileAbsent) {
            test.wantError(gotErr, errAny);
        } else if (isLink(test.source.kind)) {
            // os.Link(old, new) may or may not deference old when it is a
            // symlink. POSIX says that link(2) should deference the source,
            // but implementations are inconsistent.
            return {"", errSkipRootConsistencyCheck};
        } else if (test.source.slashSuffix() && test.source.lfinalKind() != testFileDir) {
            test.wantError(gotErr, errAny);
        }
        return {"", gotErr};
    });
}

TEST(GoOsRoot, RootMultiLstat) {
    runRootMultiTest([](rootMultiTest& test) -> std::pair<std::string, error> {
        test.setOp(std::format("Lstat({})", q(test.targetPath)));
        auto [gotStat, gotErr] = test.root ? test.root->Lstat(test.targetPath) : os::Lstat(test.targetPath);

        std::string result;
        if (gotStat) result = str(gotStat->Mode().String());

        bool escapes = test.target.lescapes();
        testFileKind finalKind = test.target.lfinalKind();

        if (test.root && escapes) {
            test.wantError(gotErr, ErrPathEscapes);
        } else if (test.target.kind == testFileAbsent) {
            // Target does not exist.
            test.wantError(gotErr, errAny);
        } else if (finalKind == testFileSymlink) {
            test.wantError(gotErr, nullptr);
            if (auto got = gotStat->Mode().Type(); got != fs::ModeSymlink) {
                test.errorf(std::format("got mode {}, want {}", got, fs::ModeSymlink));
            }
        } else if (gotErr) {
        } else {
            if (!os::SameFile(gotStat, test.targetInfo)) test.errorf("stat result is not for target file; want it to be");
        }
        return {result, gotErr};
    });
}

TEST(GoOsRoot, RootMultiMkdir) {
    runRootMultiTest([](rootMultiTest& test) -> std::pair<std::string, error> {
        test.setOp(std::format("Mkdir({}, 0o777)", q(test.targetPath)));
        error gotErr = test.root ? test.root->Mkdir(test.targetPath, 0777) : os::Mkdir(test.targetPath, 0777);

        if (test.root && test.target.ref.escapes) {
            // "mkdir ../target", or equivalent escaping path.
            test.wantError(gotErr, ErrPathEscapes);
        } else if (test.target.slashSuffix() && isLink(test.target.kind)) {
            // "mkdir symlink/", inconsistent behavior across platforms as to
            // whether this follows the symlink or not. If the symlink
            // escapes, this needs to be some kind of error though.
            if (test.root && test.target.escapes()) test.wantError(gotErr, errAny);
        } else if (test.target.kind != testFileAbsent) {
            // "mkdir target", where target exists.
            test.wantError(gotErr, errAny);
        } else {
            test.wantError(gotErr, nullptr);
            auto [fi, err] = test.root ? test.root->Stat(test.targetPath) : os::Stat(test.targetPath);
            if (err) fatal(std::format("could not stat target: {}", err));
            if (!fi->IsDir()) fatal(std::format("{}: not a directory, expected it to be", q(test.targetPath)));
        }
        return {"", gotErr};
    });
}

namespace {

std::pair<std::string, error> testRootMultiMkdirAll(rootMultiTest& test, const std::string& targetPath) {
    test.setOp(std::format("MkdirAll({}, 0o777)", q(targetPath)));
    error gotErr = test.root ? test.root->MkdirAll(targetPath, 0777) : os::MkdirAll(targetPath, 0777);

    if (test.root && test.target.lescapes()) {
        // "mkdir ../target", or equivalent escaping path.
        test.wantError(gotErr, ErrPathEscapes);
    } else if (test.root && test.target.escapes()) {
        test.wantError(gotErr, errAny);
        return {"", errSkipRootConsistencyCheck};
    } else if (test.root && isLink(test.target.kind) && test.target.target->kind == testFileAbsent &&
               targetPath != test.targetPath) {
        // A minor inconsistency between Root.MkdirAll and os.MkdirAll: When
        // an intermediate component of the tree being constructed is a
        // dangling symlink, Root.MkdirAll will follow the symlink and create
        // its target directory, while os.MkdirAll will fail with an error.
        return {"", errSkipRootConsistencyCheck};
    }
    return {"", gotErr};
}

}  // namespace

TEST(GoOsRoot, RootMultiMkdirAllShallow) {
    runRootMultiTest([](rootMultiTest& test) { return testRootMultiMkdirAll(test, test.targetPath); });
}

TEST(GoOsRoot, RootMultiMkdirAllDeep) {
    runRootMultiTest([](rootMultiTest& test) {
        std::string targetPath = test.targetPath;
        if (!targetPath.empty() && os::IsPathSeparator(targetPath.back())) targetPath += "a/b/";
        else targetPath += "/a/b";
        return testRootMultiMkdirAll(test, targetPath);
    });
}

TEST(GoOsRoot, RootMultiRename) {
    runRootMultiTest2([](rootMultiTest& test) -> std::pair<std::string, error> {
        test.setOp(std::format("Rename({}, {})", q(test.sourcePath), q(test.targetPath)));
        error gotErr = test.root ? test.root->Rename(test.sourcePath, test.targetPath)
                                 : os::Rename(test.sourcePath, test.targetPath);

        if (test.root && test.source.lescapes()) {
            test.wantError(gotErr, ErrPathEscapes);
        } else if (test.source.lfinalKind() == testFileAbsent) {
            test.wantError(gotErr, errAny);
        } else if (test.source.slashSuffix() && test.source.lfinalKind() != testFileDir) {
            test.wantError(gotErr, errAny);
        } else if (test.root && test.target.lescapes()) {
            test.wantError(gotErr, ErrPathEscapes);
        } else if (test.target.lfinalKind() == testFileDir) {
            // POSIX rename() will replace an empty target directory, but
            // os.Rename will not.
            test.wantError(gotErr, errAny);
        } else if (test.source.lfinalKind() == testFileDir && test.target.lfinalKind() != testFileAbsent) {
            test.wantError(gotErr, errAny);
        } else if (test.source.anySlashSuffix() || test.target.anySlashSuffix()) {
        } else {
            test.wantError(gotErr, nullptr);
        }

        if (test.source.slashSuffix() || test.target.slashSuffix()) return {"", errSkipRootConsistencyCheck};
        return {"", gotErr};
    });
}

TEST(GoOsRoot, RootMultiReadFile) {
    runRootMultiTest([](rootMultiTest& test) -> std::pair<std::string, error> {
        test.setOp(std::format("ReadFile({})", q(test.targetPath)));
        auto [data, gotErr] = test.root ? test.root->ReadFile(test.targetPath) : os::ReadFile(test.targetPath);
        std::string got;
        if (!gotErr) got = std::string(asStringView(data));

        if (test.target.isError()) {
            test.wantError(gotErr, errAny);
        } else if (test.root && test.target.escapes()) {
            test.wantError(gotErr, ErrPathEscapes);
        } else if (test.target.finalKind() == testFileAbsent) {
            test.wantError(gotErr, errAny);
        } else if (test.target.finalKind() == testFileDir) {
            test.wantError(gotErr, errAny);
        } else if (test.target.anySlashSuffix()) {
            // Only consistency is checked.
        } else {
            test.wantError(gotErr, nullptr);
            if (got != "target") fatal(std::format("read file content {}, want {}", q(got), q("target")));
        }
        return {got, gotErr};
    });
}

TEST(GoOsRoot, RootMultiStat) {
    runRootMultiTest([](rootMultiTest& test) -> std::pair<std::string, error> {
        test.setOp(std::format("Stat({})", q(test.targetPath)));
        auto [gotStat, gotErr] = test.root ? test.root->Stat(test.targetPath) : os::Stat(test.targetPath);

        if (test.target.isError()) {
            test.wantError(gotErr, errAny);
        } else if (test.root && test.target.escapes()) {
            test.wantError(gotErr, ErrPathEscapes);
        } else if (test.target.finalKind() == testFileAbsent) {
            test.wantError(gotErr, errAny);
        } else if (test.target.anySlashSuffix()) {
        } else {
            test.wantError(gotErr, nullptr);
            if (!os::SameFile(gotStat, test.targetInfo)) test.errorf("stat result is not for target file; want it to be");
        }
        return {"", gotErr};
    });
}

TEST(GoOsRoot, RootMultiRemove) {
    runRootMultiTest([](rootMultiTest& test) -> std::pair<std::string, error> {
        test.setOp(std::format("Remove({})", q(test.targetPath)));
        error gotErr = test.root ? test.root->Remove(test.targetPath) : os::Remove(test.targetPath);

        if (test.root && test.target.lescapes()) {
            test.wantError(gotErr, ErrPathEscapes);
        } else if (test.target.kind == testFileAbsent) {
            test.wantError(gotErr, errAny);
        } else if (test.target.anySlashSuffix()) {
            // Linux treats rmdir("symlink/") as an error when "symlink" is a
            // symlink to a directory. Root.Remove prefers the POSIX
            // interpretation of resolving the symlink.
            return {"", errSkipRootConsistencyCheck};
        } else {
            test.wantError(gotErr, nullptr);
        }
        return {"", gotErr};
    });
}

TEST(GoOsRoot, RootMultiRemoveAll) {
    runRootMultiTest([](rootMultiTest& test) -> std::pair<std::string, error> {
        test.setOp(std::format("RemoveAll({})", q(test.targetPath)));
        error gotErr = test.root ? test.root->RemoveAll(test.targetPath) : os::RemoveAll(test.targetPath);

        if (test.root && test.target.ref.escapes) {
            // This is only checking target.ref.escapes, not
            // target.lescapes(), because RemoveAll strips terminal slashes.
            test.wantError(gotErr, ErrPathEscapes);
        } else if (test.target.anySlashSuffix()) {
            // We are inconsistent on some platforms on whether
            // RemoveAll("symlink/") removes the link or the link target.
            return {"", errSkipRootConsistencyCheck};
        } else {
            test.wantError(gotErr, nullptr);
        }
        return {"", gotErr};
    });
}

TEST(GoOsRoot, RootMultiChtimes) {
    runRootMultiTest([](rootMultiTest& test) -> std::pair<std::string, error> {
        time::Time now = time::Now();
        test.setOp(std::format("Chtimes({}, {}, {})", q(test.targetPath), now.UnixNano(), now.UnixNano()));
        error gotErr = test.root ? test.root->Chtimes(test.targetPath, now, now) : os::Chtimes(test.targetPath, now, now);

        if (test.target.isError()) {
            test.wantError(gotErr, errAny);
        } else if (test.root && test.target.escapes()) {
            test.wantError(gotErr, ErrPathEscapes);
        } else if (test.target.finalKind() == testFileAbsent) {
            test.wantError(gotErr, errAny);
        } else if (test.target.anySlashSuffix()) {
        } else {
            test.wantError(gotErr, nullptr);
        }
        return {"", gotErr};
    });
}

TEST(GoOsRoot, RootMultiReadlink) {
    runRootMultiTest([](rootMultiTest& test) -> std::pair<std::string, error> {
        test.setOp(std::format("Readlink({})", q(test.targetPath)));
        auto [g, gotErr] = test.root ? test.root->Readlink(test.targetPath) : os::Readlink(test.targetPath);
        std::string got = str(g);
        if (got.starts_with(test.dir)) {
            // Replace absolute path prefix with /.../
            got = "/..." + got.substr(test.dir.size());
        }

        if (test.root && test.target.lescapes()) {
            test.wantError(gotErr, ErrPathEscapes);
        } else if (!isLink(test.target.kind)) {
            test.wantError(gotErr, errAny);
        } else if (test.target.anySlashSuffix()) {
        } else {
            test.wantError(gotErr, nullptr);
        }
        return {got, gotErr};
    });
}

TEST(GoOsRoot, RootMultiWriteFile) {
    runRootMultiTest([](rootMultiTest& test) -> std::pair<std::string, error> {
        test.setOp(std::format("WriteFile({}, ...)", q(test.targetPath)));
        error gotErr = test.root ? test.root->WriteFile(test.targetPath, "data", 0666)
                                 : os::WriteFile(test.targetPath, "data", 0666);

        if (test.target.isError()) {
            test.wantError(gotErr, errAny);
        } else if (test.root && test.target.escapes()) {
            test.wantError(gotErr, ErrPathEscapes);
        } else if (test.target.finalKind() == testFileDir) {
            test.wantError(gotErr, errAny);
        } else if (test.target.anySlashSuffix()) {
        } else {
            test.wantError(gotErr, nullptr);
        }
        return {"", gotErr};
    });
}

TEST(GoOsRoot, RootMultiOpenFile) {
    runRootMultiTest([](rootMultiTest& test) -> std::pair<std::string, error> {
        test.setOp(std::format("OpenFile({}, O_RDONLY, 0)", q(test.targetPath)));
        auto [f, gotErr] = test.root ? test.root->OpenFile(test.targetPath, os::O_RDONLY_, 0)
                                     : os::OpenFile(test.targetPath, os::O_RDONLY_, 0);
        struct closer {
            ptr<File> f;
            ~closer() {
                if (f) f->Close();
            }
        } c{f};
        std::string got = test.describeFile(f);

        if (test.target.isError()) {
            test.wantError(gotErr, errAny);
        } else if (test.root && test.target.escapes()) {
            test.wantError(gotErr, ErrPathEscapes);
        } else if (test.target.finalKind() == testFileAbsent) {
            test.wantError(gotErr, errAny);
        } else if (test.target.anySlashSuffix()) {
        } else {
            test.wantError(gotErr, nullptr);
            if (got != "target") fatal(std::format("opened file {}, want {}", q(got), q("target")));
        }
        return {got, gotErr};
    });
}
