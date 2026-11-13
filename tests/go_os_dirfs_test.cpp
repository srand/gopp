// Ported from Go 1.27.3's os tests of DirFS and CopyFS: TestDirFS,
// TestDirFSRootDir, TestDirFSEmptyDir, TestDirFSPathsValid,
// TestDirFSReadFileProc, TestCopyFS and TestCopyFSWithSymlinks of
// os_test.go, and file_test.go, with Go's test names.
//
// Adapted: Go's testdata/dirfs (files a, b and dir/x) is made in a
// temporary directory; the Windows cases (NUL, backslashes) stay, as names
// that must fail.
#include "go_os_test_util.hpp"

#include <gopp/fmt.hpp>
#include <gopp/io/fs.hpp>
#include <gopp/path.hpp>
#include <gopp/testing/fstest.hpp>

#include <map>

using namespace os_test;

namespace {

// Go's testdata/dirfs.
struct dirfsData {
    TempDir tmp;
    std::string dir = tmp.path() + "/testdata/dirfs";
    dirfsData() {
        EXPECT_FALSE(os::MkdirAll(dir + "/dir", 0755));
        for (auto name : {"a", "b", "dir/x"}) EXPECT_FALSE(os::WriteFile(dir + "/" + name, "", 0444));
    }
};

void testDirFS(const ptr<fs::FS>& fsys, const std::string& parent) {
    if (error err = fstest::TestFS(fsys, {"a", "b", "dir/x"})) FAIL() << err;

    auto* rdfs = dynamic_cast<fs::ReadDirFS*>(fsys.get());
    if (!rdfs) {
        ADD_FAILURE() << "expected DirFS result to implement fs.ReadDirFS";
    } else if (auto [d, err] = rdfs->ReadDir("nonexistent"); !err) {
        ADD_FAILURE() << "fs.ReadDir of nonexistent directory succeeded";
    }

    // Test that the error message does not contain a backslash, and does
    // not contain the DirFS argument.
    const std::string nonesuch = "dir/nonesuch";
    auto [f, err] = fsys->Open(nonesuch);
    if (!err) {
        ADD_FAILURE() << "fs.Open of nonexistent file succeeded";
    } else {
        EXPECT_TRUE(strings::Contains(err.Error().view(), nonesuch)) << "error " << err << " does not contain " << nonesuch;
        auto* pe = dynamic_cast<const fs::PathError*>(err.get());
        ASSERT_TRUE(pe);
        EXPECT_FALSE(strings::Contains(pe->Path.view(), "testdata")) << "error " << err << " contains testdata";
    }

    // Test that Open does not accept backslash as separator.
    auto d = os::DirFS(parent);
    std::tie(f, err) = d->Open("testdata\\dirfs");
    ASSERT_TRUE(err) << "Open testdata\\dirfs succeeded";

    // Test that Open does not open Windows device files.
    std::tie(f, err) = d->Open("NUL");
    EXPECT_TRUE(err) << "Open NUL succeeded";
}

}  // namespace

TEST(GoOsDirFS, DirFS) {
    dirfsData td;
    testDirFS(os::DirFS(td.dir), td.tmp.path());
}

TEST(GoOsDirFS, DirFSRootDir) {
    dirfsData td;
    // Test that Open can open a path starting at /.
    auto d = os::DirFS("/");
    auto [f, err] = d->Open(td.dir.substr(1) + "/a");
    ASSERT_FALSE(err) << err;
    f->Close();
}

TEST(GoOsDirFS, DirFSEmptyDir) {
    dirfsData td;
    Chdir cd(td.tmp.path());
    auto d = os::DirFS("");
    auto [cwd, gerr] = os::Getwd();
    ASSERT_FALSE(gerr);
    for (std::string path : {std::string("testdata/dirfs/a"),        // not DirFS(".")
                             str(cwd) + "/testdata/dirfs/a"}) {  // not DirFS("/")
        auto [f, err] = d->Open(path);
        ASSERT_TRUE(err) << "DirFS(\"\").Open(" << path << ") succeeded";
    }
}

TEST(GoOsDirFS, DirFSPathsValid) {
    TempDir d;
    ASSERT_FALSE(os::WriteFile(join(d, "control.txt"), "Hello, world!", 0644));
    ASSERT_FALSE(os::WriteFile(join(d, "e:xperi\\ment.txt"), "Hello, colon and backslash!", 0644));

    auto fsys = os::DirFS(d.path());
    error err = fs::WalkDir(*fsys, ".", [](std::string_view, const ptr<fs::DirEntry>& e, error) -> error {
        EXPECT_TRUE(fs::ValidPath(e->Name().view())) << e->Name().view() << " INVALID";
        return nullptr;
    });
    ASSERT_FALSE(err) << err;
}

TEST(GoOsDirFS, DirFSReadFileProc) {
    auto fsys = os::DirFS("/");
    std::string name = "proc/sys/fs/pipe-max-size";
    if (auto [fi, err] = fs::Stat(*fsys, name); err) GTEST_SKIP() << err;
    auto [data, err] = fs::ReadFile(*fsys, name);
    ASSERT_FALSE(err) << err;
    ASSERT_TRUE(data.size() > 0 && data[data.size() - 1] == '\n')
        << "read " << name << ": not newline-terminated: " << as_string_view(data);
}

TEST(GoOsDirFS, DirFSReadLink) {
    TempDir root;
    std::string subdir = join(root, "dir");
    ASSERT_FALSE(os::Mkdir(subdir, 0777));
    std::map<std::string, std::string> links = {
        {join(root, "parent-link"), "../foo"},
        {join(root, "sneaky-parent-link"), "dir/../../foo"},
        {join(root, "abs-link"), join(root, "foo")},
        {join(root, "rel-link"), "foo"},
        {join(root, "rel-sub-link"), "dir/foo"},
        {join(subdir, "parent-link"), "../foo"},
    };
    for (auto& [newname, oldname] : links) ASSERT_FALSE(os::Symlink(oldname, newname));

    auto fsys = os::DirFS(root.path());
    std::map<std::string, std::string> want = {
        {"rel-link", "foo"},
        {"rel-sub-link", "dir/foo"},
        {"dir/parent-link", "../foo"},
        {"parent-link", "../foo"},
        {"sneaky-parent-link", "dir/../../foo"},
        {"abs-link", join(root, "foo")},
    };
    for (auto& [name, w] : want) {
        auto [got, err] = fs::ReadLink(*fsys, name);
        EXPECT_TRUE(got.view() == w && !err)
            << "fs.ReadLink(fsys, " << name << ") = " << got.view() << ", " << err << "; want " << w << ", <nil>";
    }

    const std::string nonesuch = "dir/nonesuch";
    auto [got, err] = fs::ReadLink(*fsys, nonesuch);
    ASSERT_TRUE(err) << "fs.ReadLink of nonexistent file succeeded";
    auto* pe = dynamic_cast<const fs::PathError*>(err.get());
    ASSERT_TRUE(pe) << "fs.ReadLink error type = " << typeid(*err.get()).name() << "; want *PathError";
    EXPECT_EQ(pe->Path.view(), nonesuch) << "fs.ReadLink(" << nonesuch << ") error path";
}

TEST(GoOsDirFS, DirFSLstat) {
    TempDir root;
    ASSERT_FALSE(os::Mkdir(join(root, "dir"), 0777));
    ASSERT_FALSE(os::Symlink("dir", join(root, "link")));

    auto fsys = os::DirFS(root.path());
    std::map<std::string, FileMode> want = {{"link", os::ModeSymlink}, {"dir", os::ModeDir}};
    for (auto& [name, w] : want) {
        auto [info, err] = fs::Lstat(*fsys, name);
        FileMode got;
        if (info) got = info->Mode().Type();
        EXPECT_TRUE(got == w && !err) << "fs.Lstat(fsys, " << name << ").Mode().Type() = " << got << ", " << err
                                      << "; want " << w << ", <nil>";
    }

    const std::string nonesuch = "dir/nonesuch";
    auto [info, err] = fs::Lstat(*fsys, nonesuch);
    ASSERT_TRUE(err) << "fs.Lstat of nonexistent file succeeded";
    auto* pe = dynamic_cast<const fs::PathError*>(err.get());
    ASSERT_TRUE(pe) << "fs.Lstat error type = " << typeid(*err.get()).name() << "; want *PathError";
    EXPECT_EQ(pe->Path.view(), nonesuch) << "fs.Lstat(" << nonesuch << ") error path";
}

namespace {

void checkWalk(fs::FS& fsys, std::string_view root, const std::map<std::string, FileMode>& wantTypes) {
    std::map<std::string, int> marks;
    error err = fs::WalkDir(fsys, root, [&](std::string_view path, const ptr<fs::DirEntry>& entry, error err) -> error {
        std::string p(path);
        marks[p]++;
        if (auto it = wantTypes.find(p); it == wantTypes.end()) {
            ADD_FAILURE() << "Unexpected path " << p << " in walk";
        } else if (auto got = entry->Type(); got != it->second) {
            ADD_FAILURE() << p << " entry type = " << got << "; want " << it->second;
        }
        if (err) ADD_FAILURE() << p << ": " << err;
        return nullptr;
    });
    ASSERT_FALSE(err) << err;
    for (auto& [path, _] : wantTypes) EXPECT_EQ(marks[path], 1) << path << " visited";
}

}  // namespace

TEST(GoOsDirFS, DirFSWalkDir) {
    TempDir root;
    ASSERT_FALSE(os::Mkdir(join(root, "dir"), 0777));
    ASSERT_FALSE(os::Symlink("dir", join(root, "link")));
    ASSERT_FALSE(os::WriteFile(join(root, "dir/a"), "", 0666));
    auto fsys = os::DirFS(root.path());

    {
        SCOPED_TRACE("SymlinkRoot");
        checkWalk(*fsys, "link", {{"link", os::ModeDir}, {"link/a", 0}});
    }
    {
        SCOPED_TRACE("SymlinkPresent");
        checkWalk(*fsys, ".", {{".", os::ModeDir}, {"dir", os::ModeDir}, {"dir/a", 0}, {"link", os::ModeSymlink}});
    }
}

namespace {

// verifyCopyFS checks the content and permission of each file inside copied
// FS to ensure the copied files satisfy the convention stipulated in CopyFS.
error verifyCopyFS(fs::FS& originFS, fs::FS& copiedFS) {
    TempDir tmp;
    std::string testDir = join(tmp, "test");
    // There is no general way to get masked perm, so create a dir and a
    // file to compare the permission after umask if any.
    if (error err = os::Mkdir(testDir, os::ModePerm)) return fmt::Errorf("mkdir {} failed: {}", testDir, err);
    auto [dirStat, err] = os::Stat(testDir);
    if (err) return fmt::Errorf("stat dir {} failed: {}", testDir, err);
    FileMode wantDirMode = dirStat->Mode();

    auto [f, cerr] = os::Create(join(testDir, "tmp"));
    if (cerr) return fmt::Errorf("open {} failed: {}", join(testDir, "tmp"), cerr);
    auto [wantFileRWStat, serr] = f->Stat();
    f->Close();
    if (serr) return fmt::Errorf("stat file {} failed: {}", f->Name(), serr);
    FileMode wantFileRWMode = wantFileRWStat->Mode();

    return fs::WalkDir(originFS, ".", [&](std::string_view path, const ptr<fs::DirEntry>& d, error) -> error {
        if (d->IsDir()) {
            // the dir . is not the dir created by CopyFS so skip checking
            // its permission
            if (d->Name().view() == ".") return nullptr;
            auto [dinfo, err] = fs::Stat(copiedFS, path);
            if (err) return err;
            if (dinfo->Mode() != wantDirMode) {
                return fmt::Errorf("dir {} mode is {}, want {}", d->Name(), dinfo->Mode(), wantDirMode);
            }
            return nullptr;
        }

        auto [fInfo, err] = originFS.Open(path);
        if (err) return err;
        auto [copiedInfo, err2] = copiedFS.Open(path);
        if (err2) {
            fInfo->Close();
            return err2;
        }
        auto close = [&] {
            fInfo->Close();
            copiedInfo->Close();
        };

        // verify the file contents are the same
        auto [data, e1] = io::ReadAll(*fInfo);
        auto [newData, e2] = io::ReadAll(*copiedInfo);
        if (e1 || e2) {
            close();
            return e1 ? e1 : e2;
        }
        if (as_string_view(data) != as_string_view(newData)) {
            close();
            return fmt::Errorf("file {} content is {}, want {}", path, as_string_view(newData), as_string_view(data));
        }

        auto [fStat, e3] = fInfo->Stat();
        auto [copiedStat, e4] = copiedInfo->Stat();
        close();
        if (e3 || e4) return e3 ? e3 : e4;

        // check whether the execute permission is inherited from original FS
        if ((copiedStat->Mode() & FileMode(0111) & wantFileRWMode) != (fStat->Mode() & FileMode(0111) & wantFileRWMode)) {
            return fmt::Errorf("file {} execute mode is {}, want {}", path, copiedStat->Mode() & FileMode(0111),
                               fStat->Mode() & FileMode(0111));
        }

        FileMode rwMode = copiedStat->Mode() & ~FileMode(0111);  // unset the executable permission from file mode
        if (rwMode != wantFileRWMode) {
            return fmt::Errorf("file {} rw mode is {}, want {}", path, rwMode, wantFileRWStat->Mode());
        }
        return nullptr;
    });
}

}  // namespace

TEST(GoOsDirFS, CopyFS) {
    // Test with disk filesystem.
    dirfsData td;
    ptr<fs::FS> fsys = os::DirFS(td.dir);
    {
        TempDir tmpDir;
        ASSERT_FALSE(os::CopyFS(tmpDir, *fsys)) << "CopyFS";
        auto tmpFsys = os::DirFS(tmpDir.path());
        if (error err = fstest::TestFS(tmpFsys, {"a", "b", "dir/x"})) FAIL() << "TestFS: " << err;
        if (error err = verifyCopyFS(*fsys, *tmpFsys)) FAIL() << "comparing two directories: " << err;

        // Test whether CopyFS disallows copying for disk filesystem when
        // there is any existing file in the destination directory.
        error err = os::CopyFS(tmpDir, *fsys);
        EXPECT_TRUE(errors::Is(err, fs::ErrExist))
            << "CopyFS should have failed and returned error when there is any existing file in the destination "
               "directory (in disk filesystem), got: "
            << err;
    }

    // Test with memory filesystem.
    fsys = fstest::MakeMapFS({
        {"william", {.Data = to_bytes("Shakespeare\n")}},
        {"carl", {.Data = to_bytes("Gauss\n")}},
        {"daVinci", {.Data = to_bytes("Leonardo\n")}},
        {"einstein", {.Data = to_bytes("Albert\n")}},
        {"dir/newton", {.Data = to_bytes("Sir Isaac\n")}},
    });
    TempDir tmpDir;
    ASSERT_FALSE(os::CopyFS(tmpDir, *fsys)) << "CopyFS";
    auto tmpFsys = os::DirFS(tmpDir.path());
    if (error err = fstest::TestFS(tmpFsys, {"william", "carl", "daVinci", "einstein", "dir/newton"})) {
        FAIL() << "TestFS: " << err;
    }
    if (error err = verifyCopyFS(*fsys, *tmpFsys)) FAIL() << "comparing two directories: " << err;

    // Test whether CopyFS disallows copying for memory filesystem when there
    // is any existing file in the destination directory.
    error err = os::CopyFS(tmpDir, *fsys);
    EXPECT_TRUE(errors::Is(err, fs::ErrExist))
        << "CopyFS should have failed and returned error when there is any existing file in the destination "
           "directory (in memory filesystem), got: "
        << err;
}

TEST(GoOsDirFS, CopyFSWithSymlinks) {
    // Test it with absolute and relative symlinks that point inside and
    // outside the tree.

    // Create a directory and file outside.
    TempDir tmpDir;
    std::string outsideDir = join(tmpDir, "copyfs_out");
    ASSERT_FALSE(os::Mkdir(outsideDir, 0755));
    std::string outsideFile = join(outsideDir, "file.out.txt");
    ASSERT_FALSE(os::WriteFile(outsideFile, "Testing CopyFS outside", 0644));

    // Create a directory and file inside.
    std::string insideDir = join(tmpDir, "copyfs_in");
    ASSERT_FALSE(os::Mkdir(insideDir, 0755));
    std::string insideFile = join(insideDir, "file.in.txt");
    ASSERT_FALSE(os::WriteFile(insideFile, "Testing CopyFS inside", 0644));

    // Create directories for symlinks.
    std::string linkInDir = join(insideDir, "in_symlinks");
    ASSERT_FALSE(os::Mkdir(linkInDir, 0755));
    std::string linkOutDir = join(insideDir, "out_symlinks");
    ASSERT_FALSE(os::Mkdir(linkOutDir, 0755));

    // First, we create the absolute symlink pointing outside.
    ASSERT_FALSE(os::Symlink(outsideFile, join(linkOutDir, "file.abs.out.link")));
    // Then, we create the relative symlink pointing outside (Go's
    // filepath.Rel(linkOutDir, outsideFile)).
    ASSERT_FALSE(os::Symlink("../../copyfs_out/file.out.txt", join(linkOutDir, "file.rel.out.link")));
    // Last, we create the relative symlink pointing inside.
    ASSERT_FALSE(os::Symlink("../file.in.txt", join(linkInDir, "file.rel.in.link")));

    // Copy the directory tree and verify.
    auto fsys = os::DirFS(insideDir);
    std::string tmpDupDir = join(tmpDir, "copyfs_dup");
    ASSERT_FALSE(os::Mkdir(tmpDupDir, 0755));
    ASSERT_FALSE(os::CopyFS(tmpDupDir, *fsys)) << "CopyFS";

    auto tmpFsys = os::DirFS(tmpDupDir);
    if (error err = fstest::TestFS(tmpFsys, {"file.in.txt", "out_symlinks/file.abs.out.link",
                                             "out_symlinks/file.rel.out.link", "in_symlinks/file.rel.in.link"})) {
        FAIL() << "TestFS: " << err;
    }
    error err = fs::WalkDir(*fsys, ".", [&](std::string_view path, const ptr<fs::DirEntry>& d, error) -> error {
        if (d->IsDir()) return nullptr;

        auto [fi, err] = d->Info();
        if (err) return err;
        if (path::Ext(path).view() == ".link") {
            if (!(fi->Mode() & os::ModeSymlink)) return errors::New("original file " + std::string(path) + " should be a symlink");
            auto [tmpfi, serr] = fs::Stat(*tmpFsys, path);
            if (serr) return serr;
            if (tmpfi->Mode() & os::ModeSymlink) {
                return errors::New("copied file " + std::string(path) + " should not be a symlink");
            }
        }

        auto [data, e1] = fs::ReadFile(*fsys, path);
        if (e1) return e1;
        auto [newData, e2] = fs::ReadFile(*tmpFsys, path);
        if (e2) return e2;
        if (as_string_view(data) != as_string_view(newData)) {
            return errors::New("file " + std::string(path) + " contents differ");
        }

        std::string target;
        string fileName = path::Base(path);
        if (fileName.view() == "file.abs.out.link" || fileName.view() == "file.rel.out.link") target = outsideFile;
        else if (fileName.view() == "file.rel.in.link") target = insideFile;
        if (!target.empty()) {
            auto [targetData, e3] = os::ReadFile(target);
            if (e3) return e3;
            if (as_string_view(targetData) != as_string_view(newData)) {
                return errors::New("file " + std::string(path) + " contents differ from target");
            }
        }
        return nullptr;
    });
    ASSERT_FALSE(err) << "comparing two directories: " << err;
}
