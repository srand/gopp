// Ported from Go 1.27.3's os_test.go: the tests of File's methods and of
// the calls that change files, with Go's test names. See go_os_test.cpp.
#include "go_os_test_util.hpp"

#include <cstdlib>

using namespace os_test;

namespace {

void checkMode(std::string_view path, FileMode mode) {
    auto [dir, err] = os::Stat(path);
    ASSERT_FALSE(err) << "Stat " << path << " (looking for mode " << std::oct << uint32(mode) << "): " << err;
    EXPECT_EQ(dir->Mode() & os::ModePerm, uint32(mode)) << "Stat " << path << ": mode " << dir->Mode();
}

void checkSize(File& f, int64 want) {
    auto [dir, err] = f.Stat();
    ASSERT_FALSE(err) << "Stat " << f.Name() << " (looking for size " << want << "): " << err;
    EXPECT_EQ(dir->Size(), want) << "Stat " << f.Name();
}

// Go's export_test Atime: the access time of a FileInfo from Stat.
time::Time atime(const ptr<FileInfo>& fi) {
    auto* st = static_cast<const struct stat*>(fi->Sys());
    return time::Time::Unix(st->st_atim.tv_sec, st->st_atim.tv_nsec);
}

}  // namespace

TEST(GoOs, Chmod) {
    NewFile f;
    FileMode fm = 0456;
    ASSERT_FALSE(os::Chmod(f->Name().view(), fm));
    checkMode(f->Name().view(), fm);

    fm = 0123;
    ASSERT_FALSE(f->Chmod(fm));
    checkMode(f->Name().view(), fm);
}

TEST(GoOs, FTruncate) {
    NewFile f;
    checkSize(*f, 0);
    f->Write(b("hello, world\n"));
    checkSize(*f, 13);
    f->Truncate(10);
    checkSize(*f, 10);
    f->Truncate(1024);
    checkSize(*f, 1024);
    f->Truncate(0);
    checkSize(*f, 0);
    auto [_, err] = f->Write(b("surprise!"));
    if (!err) checkSize(*f, 13 + 9);  // wrote at offset past where hello, world was.
}

TEST(GoOs, Truncate) {
    NewFile f;
    checkSize(*f, 0);
    f->Write(b("hello, world\n"));
    checkSize(*f, 13);
    os::Truncate(f->Name().view(), 10);
    checkSize(*f, 10);
    os::Truncate(f->Name().view(), 1024);
    checkSize(*f, 1024);
    os::Truncate(f->Name().view(), 0);
    checkSize(*f, 0);
    auto [_, err] = f->Write(b("surprise!"));
    if (!err) checkSize(*f, 13 + 9);  // wrote at offset past where hello, world was.
}

TEST(GoOs, TruncateNonexistentFile) {
    auto assertPathError = [](const std::string& path, const error& err) {
        auto* pe = dynamic_cast<const os::PathError*>(err.get());
        EXPECT_TRUE(pe && os::IsNotExist(err) && pe->Path == path)
            << "got error: " << err << "\nwant an ErrNotExist PathError with path " << path;
    };
    TempDir tmp;
    std::string path = join(tmp, "nonexistent");
    assertPathError(path, os::Truncate(path, 1));
    assertPathError(path, os::Stat(path).second);
}

namespace {

void testChtimes(std::string_view name) {
    auto [preStat, err] = os::Stat(name);
    ASSERT_FALSE(err) << "Stat " << name << ": " << err;
    // Move access and modification time back a second
    time::Time at = atime(preStat);
    time::Time mt = preStat->ModTime();
    ASSERT_FALSE(os::Chtimes(name, at.Add(-time::Second), mt.Add(-time::Second)));
    auto [postStat, err2] = os::Stat(name);
    ASSERT_FALSE(err2) << "second Stat " << name << ": " << err2;
    EXPECT_TRUE(atime(postStat).Before(at)) << "AccessTime didn't go backwards";
    EXPECT_TRUE(postStat->ModTime().Before(mt)) << "ModTime didn't go backwards";
}

void testChtimesOmit(bool omitAt, bool omitMt) {
    SCOPED_TRACE(testing::Message() << "omit atime: " << omitAt << ", mtime: " << omitMt);
    NewFile file;
    std::string name = str(file->Name());
    EXPECT_FALSE(file->Close());
    auto [fs, err] = os::Stat(name);
    ASSERT_FALSE(err) << err;

    time::Time wantAtime = atime(fs);
    time::Time wantMtime = fs->ModTime();
    time::Time setAtime, setMtime;  // Zero value means omit.
    if (!omitAt) {
        wantAtime = wantAtime.Add(-time::Second);
        setAtime = wantAtime;
    }
    if (!omitMt) {
        wantMtime = wantMtime.Add(-time::Second);
        setMtime = wantMtime;
    }
    EXPECT_FALSE(os::Chtimes(name, setAtime, setMtime));
    auto [fs2, err2] = os::Stat(name);
    ASSERT_FALSE(err2) << err2;
    EXPECT_TRUE(atime(fs2).Equal(wantAtime)) << "atime mismatch";
    EXPECT_TRUE(fs2->ModTime().Equal(wantMtime)) << "mtime mismatch";
}

}  // namespace

TEST(GoOs, Chtimes) {
    NewFile f;
    f->Close();
    testChtimes(f->Name().view());
}

TEST(GoOs, ChtimesOmit) {
    testChtimesOmit(true, false);
    testChtimesOmit(false, true);
    testChtimesOmit(true, true);
    testChtimesOmit(false, false);  // Same as TestChtimes.
}

TEST(GoOs, ChtimesDir) {
    TempDir tmp;
    testChtimes(tmp);
}

// TestChtimesToUnixZero is left out: go++'s zero Time is the Unix epoch,
// time::Time::Unix(0, 0), which Chtimes takes for "keep the time".

TEST(GoOs, FileChdir) {
    auto [wd, err] = os::Getwd();
    ASSERT_FALSE(err) << "Getwd: " << err;
    Chdir restore(".");  // Ensure wd is restored after the test.

    auto [fd, oerr] = os::Open(".");
    ASSERT_FALSE(oerr) << "Open .: " << oerr;
    ASSERT_FALSE(os::Chdir("/"));
    ASSERT_FALSE(fd->Chdir()) << "fd.Chdir";
    auto [wdNew, werr] = os::Getwd();
    ASSERT_FALSE(werr) << "Getwd: " << werr;

    auto [wdInfo, e1] = fd->Stat();
    ASSERT_FALSE(e1) << e1;
    auto [newInfo, e2] = os::Stat(wdNew.view());
    ASSERT_FALSE(e2) << e2;
    ASSERT_TRUE(os::SameFile(wdInfo, newInfo)) << "fd.Chdir failed: got " << wdNew << ", want " << wd;
    fd->Close();
}

TEST(GoOs, ChdirAndGetwd) {
    TempDir tmp;
    Chdir restore(tmp);  // Ensure wd is restored after the test.
    std::string oldPwd = str(os::Getenv("PWD"));
    std::vector<std::string> dirs = {"/", "/usr/bin", "/tmp"};
    for (int mode = 0; mode < 2; mode++) {
        for (auto& d : dirs) {
            error err;
            if (mode == 0) {
                err = os::Chdir(d);
            } else {
                auto [fd1, err1] = os::Open(d);
                if (err1) {
                    ADD_FAILURE() << "Open " << d << ": " << err1;
                    continue;
                }
                err = fd1->Chdir();
                fd1->Close();
            }
            if (d == "/tmp") ::setenv("PWD", "/tmp", 1);
            auto [pwd, err1] = os::Getwd();
            ASSERT_FALSE(err) << "Chdir " << d << ": " << err;
            ASSERT_FALSE(err1) << "Getwd in " << d << ": " << err1;
            ASSERT_EQ(pwd, d) << "Getwd returned " << pwd << " want " << d;
        }
    }
    ::setenv("PWD", oldPwd.c_str(), 1);
}

// Test that Chdir+Getwd is program-wide.
TEST(GoOs, ProgWideChdir) {
    constexpr int N = 10;
    sync::WaitGroup wg;
    auto hold = make_chan<empty>();
    auto done = make_chan<empty>();
    TempDir tmp;
    std::string d = tmp.path();
    Chdir restore(d);
    std::atomic<int> failures{0};
    for (int i = 0; i < N; i++) {
        wg.Add(1);
        go([&] {
            // Lock half the goroutines in their own operating system thread
            // to exercise more scheduler possibilities: there's no
            // LockOSThread in go++.
            select(recv(done, [] {}), recv(hold, [] {}));
            auto [f0, err] = os::Stat(".");
            auto [pwd, err2] = os::Getwd();
            if (err || err2 || pwd != d) {
                failures++;
            } else if (auto [f1, err3] = os::Stat(pwd.view()); err3 || !os::SameFile(f0, f1)) {
                failures++;
            }
            wg.Done();
        });
    }
    ASSERT_FALSE(os::Chdir(d));
    // OS X sets TMPDIR to a symbolic link. So we resolve our working
    // directory again before the test.
    auto [dd, err] = os::Getwd();
    ASSERT_FALSE(err) << "Getwd: " << err;
    d = str(dd);
    hold.close();
    wg.Wait();
    done.close();
    EXPECT_EQ(failures.load(), 0);
}

TEST(GoOs, Seek) {
    NewFile f;
    const std::string data = "hello, world\n";
    io::WriteString(*f, data);
    struct test {
        int64 in;
        int whence;
        int64 out;
    };
    int64 n = static_cast<int64>(data.size());
    test tests[] = {
        {0, io::SeekCurrent, n},
        {0, io::SeekStart, 0},
        {5, io::SeekStart, 5},
        {0, io::SeekEnd, n},
        {0, io::SeekStart, 0},
        {-1, io::SeekEnd, n - 1},
        {int64(1) << 33, io::SeekStart, int64(1) << 33},
        {int64(1) << 33, io::SeekEnd, (int64(1) << 33) + n},
        // Issue 21681, Windows 4G-1, etc:
        {(int64(1) << 32) - 1, io::SeekStart, (int64(1) << 32) - 1},
        {0, io::SeekCurrent, (int64(1) << 32) - 1},
        {(int64(2) << 32) - 1, io::SeekStart, (int64(2) << 32) - 1},
        {0, io::SeekCurrent, (int64(2) << 32) - 1},
    };
    int i = 0;
    for (auto& tt : tests) {
        auto [off, err] = f->Seek(tt.in, tt.whence);
        EXPECT_TRUE(off == tt.out && !err) << "#" << i << ": Seek(" << tt.in << ", " << tt.whence << ") = " << off
                                           << ", " << err << " want " << tt.out << ", nil";
        i++;
    }
}

TEST(GoOs, SeekError) {
    auto [r, w, err] = os::Pipe();
    ASSERT_FALSE(err) << err;
    auto [_, rerr] = r->Seek(0, 0);
    ASSERT_TRUE(rerr) << "Seek on pipe should fail";
    auto* perr = dynamic_cast<const os::PathError*>(rerr.get());
    EXPECT_TRUE(perr && isErrno(perr->Err, ESPIPE)) << "Seek returned error " << rerr << ", want &PathError{Err: ESPIPE}";
    auto [_2, werr] = w->Seek(0, 0);
    ASSERT_TRUE(werr) << "Seek on pipe should fail";
    perr = dynamic_cast<const os::PathError*>(werr.get());
    EXPECT_TRUE(perr && isErrno(perr->Err, ESPIPE)) << "Seek returned error " << werr << ", want &PathError{Err: ESPIPE}";
    r->Close();
    w->Close();
}

TEST(GoOs, OpenError) {
    TempDir dir;
    touch(join(dir, "is-a-file"));
    ASSERT_FALSE(os::Mkdir(join(dir, "is-a-dir"), 0777));
    struct test {
        std::string path;
        int mode;
        int error;
    } tests[] = {
        {"no-such-file", os::O_RDONLY_, ENOENT},
        {"is-a-dir", os::O_WRONLY_, EISDIR},
        {"is-a-file/no-such-file", os::O_WRONLY_, ENOTDIR},
    };
    for (auto& tt : tests) {
        std::string path = join(dir, tt.path);
        auto [f, err] = os::OpenFile(path, tt.mode, 0);
        if (!err) {
            ADD_FAILURE() << "OpenFile(" << path << ", " << tt.mode << ") succeeded";
            f->Close();
            continue;
        }
        auto* perr = dynamic_cast<const os::PathError*>(err.get());
        ASSERT_TRUE(perr) << "OpenFile(" << path << ") returns error " << err << "; want *PathError";
        EXPECT_TRUE(isErrno(perr->Err, tt.error))
            << "OpenFile(" << path << ") = _, " << perr->Err << "; want " << syscall::Errno(tt.error).Error();
    }
}

TEST(GoOs, OpenNoName) {
    auto [f, err] = os::Open("");
    if (!err) {
        f->Close();
        FAIL() << "Open(\"\") succeeded";
    }
}

TEST(GoOs, ReadAt) {
    NewFile f;
    io::WriteString(*f, "hello, world\n");
    byte buf[5];
    auto [n, err] = f->ReadAt(buf, 7);
    ASSERT_TRUE(!err && n == 5) << "ReadAt 7: " << n << ", " << err;
    ASSERT_EQ(asStringView(buf), "world");
}

// Verify that ReadAt doesn't affect seek offset.
TEST(GoOs, ReadAtOffset) {
    NewFile f;
    io::WriteString(*f, "hello, world\n");
    f->Seek(0, 0);
    byte buf[5];
    auto [n, err] = f->ReadAt(buf, 7);
    ASSERT_TRUE(!err && n == 5) << "ReadAt 7: " << n << ", " << err;
    ASSERT_EQ(asStringView(buf), "world");
    auto [n2, err2] = f->Read(buf);
    ASSERT_TRUE(!err2 && n2 == 5) << "Read: " << n2 << ", " << err2;
    ASSERT_EQ(asStringView(buf), "hello");
}

// Verify that ReadAt doesn't allow negative offset.
TEST(GoOs, ReadAtNegativeOffset) {
    NewFile f;
    io::WriteString(*f, "hello, world\n");
    f->Seek(0, 0);
    byte buf[5];
    auto [n, err] = f->ReadAt(buf, -10);
    EXPECT_TRUE(err && strings::Contains(err.Error(), "negative offset") && n == 0)
        << "ReadAt(-10) = " << n << ", " << err << "; want 0, ...negative offset...";
}

TEST(GoOs, WriteAt) {
    NewFile f;
    io::WriteString(*f, "hello, world");
    auto [n, err] = f->WriteAt(b("WOR"), 7);
    ASSERT_TRUE(!err && n == 3) << "WriteAt 7: " << n << ", " << err;
    auto [n2, err2] = io::WriteString(*f, "!");  // test that WriteAt doesn't change the file offset
    ASSERT_TRUE(!err2 && n2 == 1) << err2;
    ASSERT_EQ(readFile(f->Name().view()), "hello, WORld!");
}

TEST(GoOs, WriteAtConcurrent) {
    NewFile f;
    io::WriteString(*f, "0000000000");
    sync::WaitGroup wg;
    for (int i = 0; i < 10; i++) {
        wg.Add(1);
        go([&, i] {
            std::string s = std::to_string(i);
            auto [n, err] = f->WriteAt(b(s), i);
            EXPECT_TRUE(!err && n == 1) << "WriteAt " << i << ": " << n << ", " << err;
            auto [n2, err2] = io::WriteString(*f, "!");  // test that WriteAt doesn't change the file offset
            EXPECT_TRUE(!err2 && n2 == 1) << err2;
            wg.Done();
        });
    }
    wg.Wait();
    ASSERT_EQ(readFile(f->Name().view()), "0123456789!!!!!!!!!!");
}

// Verify that WriteAt doesn't allow negative offset.
TEST(GoOs, WriteAtNegativeOffset) {
    NewFile f;
    auto [n, err] = f->WriteAt(b("WORLD"), -10);
    EXPECT_TRUE(err && strings::Contains(err.Error(), "negative offset") && n == 0)
        << "WriteAt(-10) = " << n << ", " << err << "; want 0, ...negative offset...";
}

// Verify that WriteAt doesn't work in append mode.
TEST(GoOs, WriteAtInAppendMode) {
    InTempDir tmp;
    auto [f, err] = os::OpenFile("write_at_in_append_mode.txt", os::O_APPEND_ | os::O_CREATE_, 0666);
    ASSERT_FALSE(err) << "OpenFile: " << err;
    auto [_, werr] = f->WriteAt(b(""), 1);
    ASSERT_EQ(werr.Error(), "os: invalid use of WriteAt on file opened with O_APPEND") << werr;
    f->Close();
}

namespace {

std::string writeFile(std::string_view fname, int flag, std::string_view text) {
    auto [f, err] = os::OpenFile(fname, flag, 0666);
    if (err) {
        ADD_FAILURE() << "Open: " << err;
        return {};
    }
    auto [n, werr] = io::WriteString(*f, text);
    EXPECT_FALSE(werr) << "WriteString: " << n << ", " << werr;
    f->Close();
    return readFile(fname);
}

}  // namespace

TEST(GoOs, Append) {
    InTempDir tmp;
    const std::string f = "append.txt";
    using namespace os;
    std::string s = writeFile(f, O_CREATE_ | O_TRUNC_ | O_RDWR_, "new");
    ASSERT_EQ(s, "new");
    s = writeFile(f, O_APPEND_ | O_RDWR_, "|append");
    ASSERT_EQ(s, "new|append");
    s = writeFile(f, O_CREATE_ | O_APPEND_ | O_RDWR_, "|append");
    ASSERT_EQ(s, "new|append|append");
    ASSERT_FALSE(os::Remove(f));
    s = writeFile(f, O_CREATE_ | O_APPEND_ | O_RDWR_, "new&append");
    ASSERT_EQ(s, "new&append") << "after append";
    s = writeFile(f, O_CREATE_ | O_RDWR_, "old");
    ASSERT_EQ(s, "old&append") << "after create";
    s = writeFile(f, O_CREATE_ | O_TRUNC_ | O_RDWR_, "new");
    ASSERT_EQ(s, "new") << "after truncate";
}

TEST(GoOs, FilePermissions) {
    if (::getuid() == 0) GTEST_SKIP() << "skipping test when running as root";
    struct test {
        std::string name;
        FileMode mode;
    } tests[] = {{"r", 0444}, {"w", 0222}, {"rw", 0666}};
    for (auto& test : tests) {
        SCOPED_TRACE(test.name);
        InTempDir tmp;
        const std::string filename = "f";
        auto [f, err] = os::OpenFile(filename, os::O_RDWR_ | os::O_CREATE_ | os::O_EXCL_, test.mode);
        ASSERT_FALSE(err) << err;
        f->Close();
        auto [bts, rerr] = os::ReadFile(filename);
        if (test.mode & 0444) {
            EXPECT_FALSE(rerr) << "ReadFile = " << rerr << "; want success";
        } else {
            EXPECT_TRUE(rerr) << "ReadFile = <nil>; want failure";
        }
        auto [_, serr] = os::Stat(filename);
        EXPECT_FALSE(serr) << "Stat = " << serr << "; want success";
        error werr = os::WriteFile(filename, std::string_view(), 0666);
        if (test.mode & 0222) {
            EXPECT_FALSE(werr) << "WriteFile = " << werr << "; want success";
        } else {
            EXPECT_TRUE(werr) << "WriteFile = <nil>; want failure";
        }
    }
}

TEST(GoOs, OpenFileCreateExclDanglingSymlink) {
    InTempDir tmp;
    const std::string link = "link";
    ASSERT_FALSE(os::Symlink("does_not_exist", link));
    auto [f, err] = os::OpenFile(link, os::O_WRONLY_ | os::O_CREATE_ | os::O_EXCL_, 0444);
    if (!err) f->Close();
    EXPECT_TRUE(errors::Is(err, os::ErrExist))
        << "OpenFile of a dangling symlink with O_CREATE|O_EXCL = " << err << ", want ErrExist";
    auto [_, serr] = os::Stat(link);
    EXPECT_TRUE(serr) << "OpenFile of a dangling symlink with O_CREATE|O_EXCL created a file";
}

// TestFileRDWRFlags tests the O_RDONLY, O_WRONLY, and O_RDWR flags.
TEST(GoOs, FileRDWRFlags) {
    struct test {
        std::string name;
        int flag;
    } tests[] = {{"O_RDONLY", os::O_RDONLY_}, {"O_WRONLY", os::O_WRONLY_}, {"O_RDWR", os::O_RDWR_}};
    for (auto& test : tests) {
        SCOPED_TRACE(test.name);
        InTempDir tmp;
        const std::string filename = "f";
        const std::string content = "content";
        ASSERT_FALSE(os::WriteFile(filename, content, 0666));
        auto [f, err] = os::OpenFile(filename, test.flag, 0);
        ASSERT_FALSE(err) << err;
        auto [got, rerr] = io::ReadAll(*f);
        if (test.flag == os::O_WRONLY_) {
            EXPECT_TRUE(rerr) << "read file: " << asStringView(got) << "; want error";
        } else {
            EXPECT_TRUE(!rerr && asStringView(got) == content) << "read file: " << asStringView(got) << ", " << rerr;
        }
        auto [_, serr] = f->Seek(0, 0);
        ASSERT_FALSE(serr) << "f.Seek: " << serr;
        const std::string newcontent = "CONTENT";
        auto [_2, werr] = f->Write(b(newcontent));
        if (test.flag == os::O_RDONLY_) {
            EXPECT_TRUE(werr) << "write file: succeeded, want error";
        } else {
            EXPECT_FALSE(werr) << "write file: " << werr << ", want success";
        }
        f->Close();
        std::string want = test.flag != os::O_RDONLY_ ? newcontent : content;
        ASSERT_EQ(readFile(filename), want);
    }
}

TEST(GoOs, StatDirWithTrailingSlash) {
    // Create new temporary directory and arrange to clean it up.
    TempDir tmp;
    std::string path = tmp.path();
    auto [_, err] = os::Stat(path);
    ASSERT_FALSE(err) << "stat " << path << " failed: " << err;
    // Stat of path+"/" should succeed too.
    path += "/";
    auto [_2, err2] = os::Stat(path);
    ASSERT_FALSE(err2) << "stat " << path << " failed: " << err2;
}

TEST(GoOs, SameFile) {
    InTempDir tmp;
    touch("a");
    touch("b");
    auto [ia1, e1] = os::Stat("a");
    ASSERT_FALSE(e1) << e1;
    auto [ia2, e2] = os::Stat("a");
    ASSERT_FALSE(e2) << e2;
    EXPECT_TRUE(os::SameFile(ia1, ia2)) << "files should be same";
    auto [ib, e3] = os::Stat("b");
    ASSERT_FALSE(e3) << e3;
    EXPECT_FALSE(os::SameFile(ia1, ib)) << "files should be different";
}

namespace {

void testDevNullFileInfo(std::string_view statname, const ptr<FileInfo>& fi) {
    SCOPED_TRACE(statname);
    EXPECT_EQ(fi->Size(), 0) << "wrong file size";
    EXPECT_NE(fi->Mode() & os::ModeDevice, 0u) << "wrong file mode " << fi->Mode() << ": ModeDevice is not set";
    EXPECT_NE(fi->Mode() & os::ModeCharDevice, 0u) << "wrong file mode " << fi->Mode() << ": ModeCharDevice is not set";
    EXPECT_FALSE(fi->Mode().IsRegular()) << "wrong file mode " << fi->Mode() << ": IsRegular returns true";
}

}  // namespace

TEST(GoOs, DevNullFile) {
    auto [f, err] = os::Open(os::DevNull);
    ASSERT_FALSE(err) << err;
    auto [fi, serr] = f->Stat();
    ASSERT_FALSE(serr) << serr;
    testDevNullFileInfo("f.Stat", fi);
    auto [fi2, serr2] = os::Stat(os::DevNull);
    ASSERT_FALSE(serr2) << serr2;
    testDevNullFileInfo("Stat", fi2);
    f->Close();
}

TEST(GoOs, StatDirModeExec) {
    constexpr uint32 mode = 0111;
    TempDir tmp;
    ASSERT_FALSE(os::Chmod(tmp, 0777));
    auto [dir, err] = os::Stat(tmp);
    ASSERT_FALSE(err) << err;
    EXPECT_EQ(dir->Mode() & mode, mode);
}
