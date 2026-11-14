// Ported from Go 1.27.3's os tests: read_test.go, path_test.go,
// tempfile_test.go, removeall_test.go and getwd_unix_test.go, with Go's
// test names. See go_os_test.cpp.
#include "go_os_test_util.hpp"

#include <cstdlib>

using namespace os_test;

// read_test.go

namespace {

void checkNamedSize(std::string_view path, int64 size) {
    auto [dir, err] = os::Stat(path);
    ASSERT_FALSE(err) << "Stat " << path << " (looking for size " << size << "): " << err;
    EXPECT_EQ(dir->Size(), size) << "Stat " << path;
}

// Go's makefs: a directory with the entries: "name" is a file holding its
// name, "name/" a directory, "name => target" a symbolic link.
std::string makefs(const TempDir& tmp, std::initializer_list<std::string> entries) {
    std::string root = join(tmp, "ROOT");
    EXPECT_FALSE(os::Mkdir(root, 0777));
    for (std::string ent : entries) {
        std::string link;
        bool is_link = false;
        if (auto i = ent.find(" => "); i != std::string::npos) {
            link = ent.substr(i + 4);
            ent = ent.substr(0, i);
            is_link = true;
        }
        // path.Dir of the entry without its trailing slash.
        std::string base = ent;
        while (!base.empty() && base.back() == '/') base.pop_back();
        if (auto i = base.rfind('/'); i != std::string::npos) {
            EXPECT_FALSE(os::MkdirAll(join(root, base.substr(0, i)), 0777));
        }
        if (is_link) {
            EXPECT_FALSE(os::Symlink(link, join(root, ent)));
        } else if (ent.back() == '/') {
            EXPECT_FALSE(os::MkdirAll(join(root, ent), 0777));
        } else {
            EXPECT_FALSE(os::WriteFile(join(root, ent), ent, 0666));
        }
    }
    return root;
}

}  // namespace

TEST(GoOs, ReadFile) {
    auto [_, err] = os::ReadFile("rumpelstilzchen");
    ASSERT_TRUE(err) << "ReadFile rumpelstilzchen: error expected, none found";

    // Go reads its own source, read_test.go.
    NewFile f;
    const std::string data(3000, 'x');
    f->WriteString(data);
    auto [contents, rerr] = os::ReadFile(f->Name().view());
    ASSERT_FALSE(rerr) << "ReadFile " << f->Name() << ": " << rerr;
    checkNamedSize(f->Name().view(), static_cast<int64>(contents.size()));
}

TEST(GoOs, WriteFile) {
    auto [f, err] = os::CreateTemp("", "os-test");
    ASSERT_FALSE(err) << err;
    const std::string msg =
        "Programming today is a race between software engineers striving to "
        "build bigger and better idiot-proof programs, and the Universe trying "
        "to produce bigger and better idiots. So far, the Universe is winning.";
    ASSERT_FALSE(os::WriteFile(f->Name().view(), msg, 0644));
    EXPECT_EQ(read_file(f->Name().view()), msg) << "ReadFile: wrong data";
    f->Close();
    os::Remove(f->Name().view());
}

TEST(GoOs, ReadOnlyWriteFile) {
    if (::getuid() == 0) GTEST_SKIP() << "Root can write to read-only files anyway, so skip the read-only test.";
    TempDir tmp;
    std::string filename = join(tmp, "blurp.txt");
    ASSERT_FALSE(os::WriteFile(filename, "shmorp", 0444));
    ASSERT_TRUE(os::WriteFile(filename, "florp", 0444)) << "Expected an error when writing to read-only file " << filename;
    ASSERT_EQ(read_file(filename), "shmorp");
}

TEST(GoOs, ReadDir) {
    auto [_, err] = os::ReadDir("rumpelstilzchen");
    ASSERT_TRUE(err) << "ReadDir rumpelstilzchen: error expected, none found";

    TempDir tmp;
    std::string filename = join(tmp, "foo");
    touch(filename);
    auto [list, ferr] = os::ReadDir(filename);
    ASSERT_TRUE(list.empty() && isErrno(errors::Unwrap(ferr), ENOTDIR))
        << "ReadDir " << filename << ": (nil, ENOTDIR) expected, got " << ferr;

    // Go reads its source directory, with read_test.go and exec/.
    touch(join(tmp, "read_test.go"));
    ASSERT_FALSE(os::Mkdir(join(tmp, "exec"), 0777));
    auto [dlist, derr] = os::ReadDir(tmp);
    ASSERT_FALSE(derr) << derr;
    bool found_file = false, found_sub_dir = false;
    for (auto& dir : dlist) {
        if (!dir->IsDir() && dir->Name() == "read_test.go") found_file = true;
        if (dir->IsDir() && dir->Name() == "exec") found_sub_dir = true;
    }
    EXPECT_TRUE(found_file) << "read_test.go file not found";
    EXPECT_TRUE(found_sub_dir) << "exec directory not found";
    // Sorted by name.
    ASSERT_EQ(dlist.size(), 3u);
    EXPECT_EQ(dlist[0]->Name(), "exec");
    EXPECT_EQ(dlist[1]->Name(), "foo");
    EXPECT_EQ(dlist[2]->Name(), "read_test.go");
}

TEST(GoOs, ReadDirFD) {
    if (auto [_, err] = os::Stat("/dev/fd"); err) GTEST_SKIP() << err;
    auto [_, err] = os::ReadDir("/dev/fd");
    ASSERT_FALSE(err) << err;
}

// path_test.go

TEST(GoOs, MkdirAll) {
    InTempDir tmp;
    std::string path = "_TestMkdirAll_/dir/./dir2";
    ASSERT_FALSE(os::MkdirAll(path, 0777)) << "MkdirAll " << path;
    // Already exists, should succeed.
    ASSERT_FALSE(os::MkdirAll(path, 0777)) << "MkdirAll " << path << " (second time)";

    // Make file.
    std::string fpath = path + "/file";
    auto [f, err] = os::Create(fpath);
    ASSERT_FALSE(err) << "create " << fpath << ": " << err;

    // Can't make directory named after file.
    error err1 = os::MkdirAll(fpath, 0777);
    ASSERT_TRUE(err1) << "MkdirAll " << fpath << ": no error";
    auto* perr = dynamic_cast<const os::PathError*>(err1.get());
    ASSERT_TRUE(perr) << "MkdirAll " << fpath << " returned " << err1 << ", not *PathError";
    EXPECT_EQ(perr->Path, fpath);

    // Can't make subdirectory of file.
    std::string ffpath = fpath + "/subdir";
    error err2 = os::MkdirAll(ffpath, 0777);
    ASSERT_TRUE(err2) << "MkdirAll " << ffpath << ": no error";
    perr = dynamic_cast<const os::PathError*>(err2.get());
    ASSERT_TRUE(perr) << "MkdirAll " << ffpath << " returned " << err2 << ", not *PathError";
    EXPECT_EQ(perr->Path, fpath);
    f->Close();
}

TEST(GoOs, MkdirAllAbsPath) {
    TempDir tmp;
    std::string path = tmp.path() + "/a/b/c";
    ASSERT_FALSE(os::MkdirAll(path, 0777));
    auto [st, err] = os::Stat(path);
    ASSERT_FALSE(err) << err;
    ASSERT_TRUE(st->IsDir()) << "after MkdirAll(" << path << ", 0o777), it is not a directory";
}

TEST(GoOs, MkdirAllWithSymlink) {
    TempDir tmp;
    std::string dir = tmp.path() + "/dir";
    ASSERT_FALSE(os::Mkdir(dir, 0755));
    std::string link = tmp.path() + "/link";
    ASSERT_FALSE(os::Symlink("dir", link));
    std::string path = link + "/foo";
    EXPECT_FALSE(os::MkdirAll(path, 0755)) << "MkdirAll " << path;
}

// tempfile_test.go

TEST(GoOs, CreateTemp) {
    TempDir tmp;
    std::string nonexistent_dir = join(tmp, "_not_exists_");
    auto [f, err] = os::CreateTemp(nonexistent_dir, "foo");
    EXPECT_TRUE(!f && err) << "CreateTemp(" << nonexistent_dir << ", `foo`) = " << err;
}

TEST(GoOs, CreateTempPattern) {
    struct test {
        std::string pattern, prefix, suffix;
    } tests[] = {
        {"tempfile_test", "tempfile_test", ""},
        {"tempfile_test*", "tempfile_test", ""},
        {"tempfile_test*xyz", "tempfile_test", "xyz"},
    };
    for (auto& test : tests) {
        auto [f, err] = os::CreateTemp("", test.pattern);
        if (err) {
            ADD_FAILURE() << "CreateTemp(..., " << test.pattern << ") error: " << err;
            continue;
        }
        std::string name = str(f->Name());
        std::string base = name.substr(name.rfind('/') + 1);
        f->Close();
        os::Remove(name);
        EXPECT_TRUE(base.starts_with(test.prefix) && base.ends_with(test.suffix))
            << "CreateTemp pattern " << test.pattern << " created bad name " << base;
    }
}

namespace {

struct bad_pattern_test {
    std::string pattern;
    bool want_err;
};

const std::vector<bad_pattern_test> bad_pattern_tests = {
    {"ioutil*test", false},
    {"tempfile_test*foo", false},
    {"tempfile_test/foo", true},
    {"tempfile_test*/foo", true},
    {"tempfile_test/*foo", true},
    {"/tempfile_test/*foo", true},
    {"tempfile_test*foo/", true},
};

}  // namespace

TEST(GoOs, CreateTempBadPattern) {
    TempDir tmp;
    for (auto& tt : bad_pattern_tests) {
        SCOPED_TRACE(tt.pattern);
        auto [tmpfile, err] = os::CreateTemp(tmp, tt.pattern);
        if (tmpfile) tmpfile->Close();
        if (tt.want_err) {
            EXPECT_TRUE(err) << "CreateTemp succeeded, expected error";
            EXPECT_TRUE(errors::Is(err, os::detail::errPatternHasSeparator)) << err << ", expected ErrPatternHasSeparator";
        } else {
            EXPECT_FALSE(err) << err;
        }
    }
}

TEST(GoOs, MkdirTemp) {
    auto [name, err] = os::MkdirTemp("/_not_exists_", "foo");
    EXPECT_TRUE(name.empty() && err) << "MkdirTemp(`/_not_exists_`, `foo`) = " << name << ", " << err;

    struct test {
        std::string pattern, want_prefix, want_suffix;
    } tests[] = {
        {"tempfile_test", "tempfile_test", ""},
        {"tempfile_test*", "tempfile_test", ""},
        {"tempfile_test*xyz", "tempfile_test", "xyz"},
        {"*xyz", "", "xyz"},
    };
    std::string dir = str(os::TempDir());
    while (dir.size() > 1 && dir.back() == '/') dir.pop_back();
    for (auto& tt : tests) {
        SCOPED_TRACE(tt.pattern);
        auto [n, merr] = os::MkdirTemp(dir, tt.pattern);
        ASSERT_TRUE(!n.empty() && !merr) << "MkdirTemp(dir, " << tt.pattern << ") = " << n << ", " << merr;
        std::string s = str(n);
        os::Remove(s);
        // ^dir/prefix[0-9]+suffix$
        std::string want_prefix = dir + "/" + tt.want_prefix;
        ASSERT_TRUE(s.starts_with(want_prefix) && s.ends_with(tt.want_suffix)) << "MkdirTemp created bad name " << s;
        std::string digits = s.substr(want_prefix.size(), s.size() - want_prefix.size() - tt.want_suffix.size());
        EXPECT_TRUE(!digits.empty() && digits.find_first_not_of("0123456789") == std::string::npos)
            << "MkdirTemp created bad name " << s;
    }
}

// Test that we return a nice error message if the dir argument to TempDir
// doesn't exist (or that it's empty and TempDir doesn't exist).
TEST(GoOs, MkdirTempBadDir) {
    TempDir tmp;
    std::string bad_dir = join(tmp, "not-exist");
    auto [_, err] = os::MkdirTemp(bad_dir, "foo");
    auto* pe = dynamic_cast<const fs::PathError*>(err.get());
    EXPECT_TRUE(pe && os::IsNotExist(err) && pe->Path == bad_dir)
        << "TempDir error = " << err << "; want PathError for path " << bad_dir << " satisfying IsNotExist";
}

TEST(GoOs, MkdirTempBadPattern) {
    TempDir tmp;
    for (auto& tt : bad_pattern_tests) {
        SCOPED_TRACE(tt.pattern);
        auto [_, err] = os::MkdirTemp(tmp, tt.pattern);
        if (tt.want_err) {
            EXPECT_TRUE(err) << "MkdirTemp succeeded, expected error";
            EXPECT_TRUE(errors::Is(err, os::detail::errPatternHasSeparator)) << err << ", expected ErrPatternHasSeparator";
        } else {
            EXPECT_FALSE(err) << err;
        }
    }
}

// removeall_test.go

TEST(GoOs, RemoveAll) {
    TempDir tmp;
    std::string tmp_dir = tmp.path();
    ASSERT_FALSE(os::RemoveAll("")) << "RemoveAll(\"\"): want nil";

    std::string file = join(tmp_dir, "file");
    std::string path = join(tmp_dir, "_TestRemoveAll_");
    std::string fpath = join(path, "file");
    std::string dpath = join(path, "dir");

    // Make a regular file and remove
    touch(file);
    ASSERT_FALSE(os::RemoveAll(file)) << "RemoveAll " << file << " (first)";
    ASSERT_TRUE(os::Lstat(file).second) << "Lstat " << file << " succeeded after RemoveAll (first)";

    // Make directory with 1 file and remove.
    ASSERT_FALSE(os::MkdirAll(path, 0777));
    touch(fpath);
    ASSERT_FALSE(os::RemoveAll(path)) << "RemoveAll " << path << " (second)";
    ASSERT_TRUE(os::Lstat(path).second) << "Lstat " << path << " succeeded after RemoveAll (second)";

    // Make directory with file and subdirectory and remove.
    ASSERT_FALSE(os::MkdirAll(dpath, 0777));
    touch(fpath);
    touch(dpath + "/file");
    ASSERT_FALSE(os::RemoveAll(path)) << "RemoveAll " << path << " (third)";
    ASSERT_TRUE(os::Lstat(path).second) << "Lstat " << path << " succeeded after RemoveAll (third)";

    // Chmod is not supported under Windows or wasip1 and test fails as
    // root.
    if (::getuid() != 0) {
        // Make directory with file and subdirectory and trigger error.
        ASSERT_FALSE(os::MkdirAll(dpath, 0777));
        for (auto& s : {fpath, dpath + "/file1", path + "/zzz"}) touch(s);
        ASSERT_FALSE(os::Chmod(dpath, 0));
        // No error checking here: either RemoveAll will or won't be able to
        // remove dpath; either way we want to see if it removes fpath and
        // path/zzz. Reasons why RemoveAll might succeed in removing dpath
        // as well include: running as root; a file system that doesn't
        // enforce permissions.
        os::RemoveAll(path);
        os::Chmod(dpath, 0777);
        for (auto& s : {fpath, path + "/zzz"}) {
            ASSERT_TRUE(os::Lstat(s).second) << "Lstat " << s << " succeeded after partial RemoveAll";
        }
    }
    ASSERT_FALSE(os::RemoveAll(path)) << "RemoveAll " << path << " after partial RemoveAll";
    ASSERT_TRUE(os::Lstat(path).second) << "Lstat " << path << " succeeded after RemoveAll (final)";
}

// Test RemoveAll on a large directory.
TEST(GoOs, RemoveAllLarge) {
    TempDir tmp;
    std::string path = join(tmp, "_TestRemoveAllLarge_");
    ASSERT_FALSE(os::MkdirAll(path, 0777));
    for (int i = 0; i < 1000; i++) touch(path + "/file" + std::to_string(i));
    ASSERT_FALSE(os::RemoveAll(path));
    ASSERT_TRUE(os::Lstat(path).second) << "Lstat " << path << " succeeded after RemoveAll";
}

TEST(GoOs, RemoveAllLongPath) {
    TempDir tmp;
    std::string start_path = join(tmp, "start");
    ASSERT_FALSE(os::Mkdir(start_path, 0755));
    {
        Chdir cd(start_path);
        std::string name(100, 'a');
        for (int i = 0; i < 41; i++) {
            ASSERT_FALSE(os::Mkdir(name, 0755)) << "Could not mkdir";
            ASSERT_FALSE(os::Chdir(name)) << "Could not chdir";
        }
        // Chdir out of startPath before attempting to remove it, otherwise
        // the RemoveAll might fail.
        ASSERT_FALSE(os::Chdir(tmp));
        EXPECT_FALSE(os::RemoveAll(start_path)) << "RemoveAll could not remove long file path";
    }
}

TEST(GoOs, RemoveAllDot) {
    InTempDir tmp;
    EXPECT_TRUE(os::RemoveAll(".")) << "RemoveAll succeed to remove .";
}

TEST(GoOs, RemoveAllDotDot) {
    TempDir tmp;
    std::string subdir = join(tmp, "x");
    std::string subsubdir = join(subdir, "y");
    ASSERT_FALSE(os::MkdirAll(subsubdir, 0777));
    // filepath.Join(subsubdir, "..") cleans the path to subdir.
    error rerr = os::RemoveAll(subdir);
    EXPECT_FALSE(rerr) << rerr;
    for (auto& dir : {subsubdir, subdir}) EXPECT_TRUE(os::Stat(dir).second) << dir << ": exists after RemoveAll";
}

// Issue #29178.
TEST(GoOs, RemoveReadOnlyDir) {
    TempDir tmp;
    std::string subdir = join(tmp, "x");
    ASSERT_FALSE(os::Mkdir(subdir, 0));
    ASSERT_FALSE(os::RemoveAll(subdir));
    EXPECT_TRUE(os::Stat(subdir).second) << "subdirectory was not removed";
}

// Issue #29983.
TEST(GoOs, RemoveAllButReadOnlyAndPathError) {
    if (::getuid() == 0) GTEST_SKIP() << "skipping test: running as root (which ignores file permissions)";
    TempDir tmp;
    std::string temp_dir = tmp.path();
    std::vector<std::string> dirs = {"a", "a/x", "a/x/1", "b", "b/y", "b/y/2", "c", "c/z", "c/z/3"};
    std::vector<std::string> readonly = {"b"};
    auto in_readonly = [&](const std::string& d) {
        for (auto& ro : readonly) {
            if (d == ro) return true;
            auto i = d.rfind('/');
            if (i != std::string::npos && d.substr(0, i) == ro) return true;
        }
        return false;
    };
    for (auto& dir : dirs) ASSERT_FALSE(os::Mkdir(join(temp_dir, dir), 0777));
    for (auto& dir : readonly) ASSERT_FALSE(os::Chmod(join(temp_dir, dir), 0555));

    error err = os::RemoveAll(temp_dir);
    for (auto& dir : readonly) os::Chmod(join(temp_dir, dir), 0777);
    ASSERT_TRUE(err) << "RemoveAll succeeded unexpectedly";

    // The error should be of type *PathError. see issue 30491 for details.
    auto* path_err = dynamic_cast<const os::PathError*>(err.get());
    if (path_err) {
        std::string want = temp_dir + "/b/y";
        EXPECT_EQ(path_err->Path, want) << "RemoveAll(" << temp_dir << "): err.Path";
    } else {
        ADD_FAILURE() << "RemoveAll(" << temp_dir << "): error " << err << ", want *fs.PathError";
    }

    for (auto& dir : dirs) {
        auto [_, serr] = os::Stat(join(temp_dir, dir));
        if (in_readonly(dir)) {
            EXPECT_FALSE(serr) << "file " << dir << " was deleted but should still exist";
        } else {
            EXPECT_TRUE(serr) << "file " << dir << " still exists but should have been deleted";
        }
    }
}

TEST(GoOs, RemoveUnreadableDir) {
    if (::getuid() == 0) GTEST_SKIP() << "skipping test when running as root";
    TempDir tmp;
    std::string target = tmp.path() + "/d0/d1/d2";
    ASSERT_FALSE(os::MkdirAll(target, 0755));
    ASSERT_FALSE(os::Chmod(target, 0300));
    ASSERT_FALSE(os::RemoveAll(join(tmp, "d0")));
}

// Issue 29921
TEST(GoOs, RemoveAllWithMoreErrorThanReqSize) {
    TempDir tmp;
    std::string path = join(tmp, "_TestRemoveAllWithMoreErrorThanReqSize_");
    // Make directory with 1025 read-only files.
    ASSERT_FALSE(os::MkdirAll(path, 0777));
    for (int i = 0; i < 1025; i++) touch(join(path, "file" + std::to_string(i)));
    // Make the parent directory read-only. On some platforms, this is what
    // prevents os.Remove from removing the files within that directory.
    ASSERT_FALSE(os::Chmod(path, 0555));
    error err = os::RemoveAll(path);
    os::Chmod(path, 0755);

    if (::getuid() == 0) {
        // On many platforms, root can remove files from read-only
        // directories.
        return;
    }
    ASSERT_TRUE(err) << "RemoveAll(<read-only directory>) = nil; want error";

    auto [dir, oerr] = os::Open(path);
    ASSERT_FALSE(oerr) << oerr;
    auto [names, _] = dir->Readdirnames(1025);
    ASSERT_GE(names.size(), 1025u) << "RemoveAll(<read-only directory>) unexpectedly removed "
                                   << 1025 - names.size() << " read-only files from that directory";
    dir->Close();
}

TEST(GoOs, RemoveAllTrailingSlash) {
    TempDir tmp;
    std::string dir = makefs(tmp, {"dir/a/file1", "dir/a/file2", "dir/file3"});
    std::string path = dir + "/dir";
    ASSERT_FALSE(os::RemoveAll(path + "/"));
    EXPECT_TRUE(os::IsNotExist(os::Stat(path).second)) << "after RemoveAll(" << path << "/), directory still exists";
}

TEST(GoOs, RemoveAllSymlinkRemovalFailure) {
    if (::getuid() == 0) GTEST_SKIP() << "skipping test: running as root (which ignores file permissions)";
    TempDir tmp;
    std::string dir = makefs(tmp, {"parent/", "parent/link => target"});
    // Prevent the symlink from being removed.
    ASSERT_FALSE(os::Chmod(dir + "/parent", 0555));
    error err = os::RemoveAll(dir + "/parent/link");
    os::Chmod(dir + "/parent", 0755);
    ASSERT_TRUE(errors::Is(err, os::ErrPermission)) << "RemoveAll = " << err << "; want ErrPermission";
    (void)err.Error();
}

// getwd_unix_test.go

namespace {

void testGetwdDeep(bool set_pwd) {
    TempDir tmp;
    std::string dir = tmp.path();
    Chdir cd(dir);
    std::string old_pwd = str(os::Getenv("PWD"));
    ::setenv("PWD", set_pwd ? dir.c_str() : "", 1);

    std::string name(200, 'a');
    for (;;) {
        ASSERT_FALSE(os::Mkdir(name, 0700));
        ASSERT_FALSE(os::Chdir(name));
        if (set_pwd) {
            dir += "/" + name;
            ::setenv("PWD", dir.c_str(), 1);
        }
        auto [wd, err] = os::Getwd();
        if (err) {
            if (errors::Is(err, os::ErrPermission)) break;  // EACCES or EPERM
            FAIL() << err;
        }
        if (set_pwd) {
            ASSERT_EQ(wd, dir) << "Getwd: want same value as $PWD";
        }
        // Ideally the success criterion would be to find the absolute
        // limit. For now, simply check if we can stat the returned path.
        auto [_, serr] = os::Stat(wd.view());
        if (serr || wd.size() > 4096) {
            if (serr && !isErrno(errors::Unwrap(serr), ENAMETOOLONG)) FAIL() << "unexpected Stat error: " << serr;
            break;
        }
    }
    ::setenv("PWD", old_pwd.c_str(), 1);
    // Leave the deep directory before TempDir removes it.
    os::Chdir(tmp);
}

}  // namespace

TEST(GoOs, GetwdDeep) { testGetwdDeep(false); }

TEST(GoOs, GetwdDeepWithPWDSet) { testGetwdDeep(true); }

// os_unix_test.go: TestSplitPath and TestMkdirStickyUmask

TEST(GoOs, SplitPath) {
    struct test {
        std::string path, want_dir, want_base;
    } tests[] = {
        {"a", ".", "a"},       {"a/", ".", "a"},       {"a//", ".", "a"},       {"a/b", "a", "b"},
        {"a/b/", "a", "b"},    {"a/b/c", "a/b", "c"},  {"/a", "/", "a"},        {"/a/", "/", "a"},
        {"/a/b", "/a", "b"},   {"/a/b/", "/a", "b"},   {"/a/b/c", "/a/b", "c"}, {"//a", "/", "a"},
        {"//a/", "/", "a"},    {"///a", "/", "a"},     {"///a/", "/", "a"},
    };
    for (auto& tt : tests) {
        auto [dir, base] = os::detail::split_path(tt.path);
        EXPECT_TRUE(dir == tt.want_dir && base == tt.want_base)
            << "splitPath(" << tt.path << ") = " << dir << ", " << base << ", want " << tt.want_dir << ", " << tt.want_base;
    }
}

TEST(GoOs, MkdirStickyUmask) {
    constexpr mode_t umask = 0077;
    TempDir tmp;
    mode_t old_umask = ::umask(umask);
    std::string control = join(tmp, "control");
    EXPECT_FALSE(os::Mkdir(control, 0755));
    auto [cfi, err] = os::Stat(control);
    std::string p = join(tmp, "dir1");
    EXPECT_FALSE(os::Mkdir(p, os::ModeSticky | 0755));
    auto [fi, err2] = os::Stat(p);
    ::umask(old_umask);
    ASSERT_FALSE(err) << err;
    ASSERT_FALSE(err2) << err2;
    FileMode want = cfi->Mode() | os::ModeSticky;
    EXPECT_EQ(uint32(fi->Mode()), uint32(want)) << "Mkdir(_, ModeSticky|0755) created dir with mode " << fi->Mode()
                                                << "; want " << want;
}
