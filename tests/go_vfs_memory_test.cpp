// Ported from afero v1.15.0's tests of MemMapFs, memmap_test.go, and of
// its files, mem/file_test.go, with afero's test names.
//
// Adapted: the race tests (TestRacingDeleteAndClose, TestMemFsDataRace)
// wait for their fibers; TestMemFsDataRace yields as it polls.
#include "go_vfs_test_util.hpp"

#include <gopp/chan.hpp>
#include <gopp/runtime.hpp>
#include <gopp/strings.hpp>
#include <gopp/sync.hpp>
#include <gopp/time.hpp>

#include <atomic>

using namespace vfs_test;
using namespace std::chrono_literals;

namespace {

// afero's checkPathError: err is a PathError, which doesn't wrap another.
void checkPathError(const error& err, std::string_view op) {
    auto* pathErr = dynamic_cast<const fs::PathError*>(err.get());
    if (!pathErr) {
        ADD_FAILURE() << op << ": " << err << " is not a os.PathError";
        return;
    }
    EXPECT_FALSE(dynamic_cast<const fs::PathError*>(pathErr->Err.get())) << op << ": " << err << " contains another os.PathError";
}

void expectMode(vfs::Fs& fs, std::string_view name, fs::FileMode want) {
    auto [info, err] = fs.Stat(name);
    ASSERT_FALSE(err) << err;
    EXPECT_EQ(info->Mode(), want) << name << ": wrong permissions, expected " << want << ", got " << info->Mode();
}

}  // namespace

TEST(GoVfsMemory, NormalizePath) {
    std::pair<std::string_view, std::string_view> data[] = {
        {".", "/"}, {"./", "/"}, {"..", "/"}, {"../", "/"}, {"./..", "/"}, {"./../", "/"},
    };
    int i = 0;
    for (auto& [input, expected] : data) {
        EXPECT_EQ(vfs::detail::normalizePath(input), expected) << "Test " << i;
        i++;
    }
}

TEST(GoVfsMemory, PathErrors) {
    string path = filepath::Join(".", "some", "path");
    string path2 = filepath::Join(".", "different", "path");
    auto fs = vfs::NewMemoryFs();
    fs::FileMode perm = 0755;

    checkPathError(fs->Chmod(path, perm), "Chmod");
    checkPathError(fs->Chown(path, 1000, 1000), "Chown");
    checkPathError(fs->Chtimes(path, time::Now(), time::Now()), "Chtimes");
    // Create doesn't fail.
    EXPECT_FALSE(fs->Mkdir(path2, perm));
    checkPathError(fs->Mkdir(path2, perm), "Mkdir");
    EXPECT_FALSE(fs->MkdirAll(path2, perm)) << "MkdirAll";
    checkPathError(fs->Open(path).second, "Open");
    checkPathError(fs->OpenFile(path, os::O_RDWR_, perm).second, "OpenFile");
    checkPathError(fs->Remove(path), "Remove");
    EXPECT_FALSE(fs->RemoveAll(path)) << "RemoveAll";
    checkPathError(fs->Rename(path, path2), "Rename");
    checkPathError(fs->Stat(path).second, "Stat");
}

// O_EXCL is handled.
TEST(GoVfsMemory, OpenFileExcl) {
    auto fs = vfs::NewMemoryFs();
    // The first creation succeeds.
    auto [f, err] = fs->OpenFile("/myFileTest", os::O_CREATE_ | os::O_EXCL_, 0765);
    ASSERT_FALSE(err) << "OpenFile Create Excl failed: " << err;
    f->Close();
    // The second fails.
    std::tie(f, err) = fs->OpenFile("/myFileTest", os::O_CREATE_ | os::O_EXCL_, 0765);
    EXPECT_TRUE(err) << "OpenFile Create Excl should have failed, but it didn't";
    checkPathError(err, "Open");
}

// OpenFile, Mkdir and MkdirAll set permissions.
TEST(GoVfsMemory, PermSet) {
    constexpr fs::FileMode fileMode = 0765;
    // Directories also have the directory bit.
    constexpr fs::FileMode dirMode = fileMode | fs::ModeDir;
    auto fs = vfs::NewMemoryFs();

    auto [f, err] = fs->OpenFile("/myFileTest", os::O_CREATE_, fileMode);
    ASSERT_FALSE(err) << "OpenFile Create failed: " << err;
    f->Close();
    auto [s, serr] = fs->Stat("/myFileTest");
    ASSERT_FALSE(serr);
    EXPECT_EQ(s->Mode().String(), fileMode.String());

    ASSERT_FALSE(fs->Mkdir("/myDirTest", dirMode));
    std::tie(s, serr) = fs->Stat("/myDirTest");
    ASSERT_FALSE(serr);
    EXPECT_EQ(s->Mode().String(), dirMode.String());

    ASSERT_FALSE(fs->MkdirAll("/my/path/to/dir", dirMode));
    std::tie(s, serr) = fs->Stat("/my/path/to/dir");
    ASSERT_FALSE(serr);
    EXPECT_EQ(s->Mode().String(), dirMode.String());
}

// Fails if the handles of a file share their offset.
TEST(GoVfsMemory, MultipleOpenFiles) {
    registry reg;
    auto fss = newFss();
    std::vector<std::string> data(fss.size());
    for (std::size_t i = 0; i < fss.size(); i++) {
        auto& fs = fss[i];
        string path = filepath::Join(reg.testDir(fs), "afero-demo2.txt");
        auto [fh1, err] = fs->Create(path);
        ASSERT_FALSE(err) << "fs.Create failed: " << err;
        EXPECT_FALSE(fh1->Write(as_bytes("test")).second);
        EXPECT_FALSE(fh1->Seek(0, io::SeekStart).second);

        auto [fh2, oerr] = fs->OpenFile(path, os::O_RDWR_, 0777);
        ASSERT_FALSE(oerr) << "fs.OpenFile failed: " << oerr;
        EXPECT_FALSE(fh2->Seek(0, io::SeekEnd).second);
        EXPECT_FALSE(fh2->Write(as_bytes("data")).second);
        EXPECT_FALSE(fh2->Close());

        EXPECT_FALSE(fh1->Write(as_bytes("data")).second);
        EXPECT_FALSE(fh1->Close());
        // Now "datadata".
        auto [b, rerr] = vfs::ReadFile(*fs, path);
        EXPECT_FALSE(rerr) << rerr;
        data[i] = str(b);
    }
    for (std::size_t i = 1; i < fss.size(); i++) {
        EXPECT_EQ(data[0], data[i]) << fss[0]->Name() << " and " << fss[i]->Name() << " don't behave the same";
    }
}

// Write fails on a file opened read-only.
TEST(GoVfsMemory, ReadOnly) {
    registry reg;
    for (auto& fs : newFss()) {
        string path = filepath::Join(reg.testDir(fs), "afero-demo.txt");
        auto [f, err] = fs->Create(path);
        ASSERT_FALSE(err) << fs->Name() << ": fs.Create failed: " << err;
        EXPECT_FALSE(f->Write(as_bytes("test")).second) << fs->Name() << ": Write failed";
        f->Close();

        std::tie(f, err) = fs->Open(path);
        ASSERT_FALSE(err) << "fs.Open failed: " << err;
        EXPECT_TRUE(f->Write(as_bytes("data")).second) << fs->Name() << ": No write error";
        f->Close();

        std::tie(f, err) = fs->OpenFile(path, os::O_RDONLY_, 0644);
        ASSERT_FALSE(err) << "fs.Open failed: " << err;
        EXPECT_TRUE(f->Write(as_bytes("data")).second) << fs->Name() << ": No write error";
        f->Close();
    }
}

TEST(GoVfsMemory, WriteCloseTime) {
    registry reg;
    for (auto& fs : newFss()) {
        string path = filepath::Join(reg.testDir(fs), "afero-demo.txt");
        auto [f, err] = fs->Create(path);
        ASSERT_FALSE(err) << fs->Name() << ": fs.Create failed: " << err;
        f->Close();

        std::tie(f, err) = fs->Create(path);
        ASSERT_FALSE(err) << fs->Name() << ": fs.Create failed: " << err;
        auto [fi, serr] = f->Stat();
        ASSERT_FALSE(serr) << fs->Name() << ": Stat failed: " << serr;
        time::Time timeBefore = fi->ModTime();

        // So that the time advances, on any file system.
        time::Sleep(time::Second);

        EXPECT_FALSE(f->Write(as_bytes("test")).second) << fs->Name() << ": Write failed";
        f->Close();
        std::tie(fi, serr) = fs->Stat(path);
        ASSERT_FALSE(serr) << fs->Name() << ": fs.Stat failed: " << serr;
        EXPECT_FALSE(fi->ModTime().Equal(timeBefore)) << fs->Name() << ": ModTime was not set on Close()";
    }
}

TEST(GoVfsMemory, RacingDeleteAndClose) {
    auto fs = vfs::NewMemoryFs();
    auto [f, err] = fs->Create("testfile");
    ASSERT_FALSE(err) << err;

    auto in = make_chan<bool>();
    sync::WaitGroup wg;
    wg.Go([in, f] {
        in.recv();
        f->Close();
    });
    wg.Go([in, fs] {
        in.recv();
        fs->Remove("testfile");
    });
    in.close();
    wg.Wait();
}

TEST(GoVfsMemory, MemFsDataRace) {
    constexpr std::string_view dir = "test_dir";
    auto fs = vfs::NewMemoryFs();
    ASSERT_FALSE(fs->MkdirAll(dir, 0777));

    constexpr int n = 1000;
    std::atomic<bool> done = false;
    sync::WaitGroup wg;
    wg.Go([&] {
        for (int i = 0; i < n; i++) {
            string fname = filepath::Join(dir, std::to_string(i) + ".txt");
            if (error err = vfs::WriteFile(*fs, fname, {}, 0777)) panic(err.Error().view());
            if (error err = fs->Remove(fname)) panic(err.Error().view());
        }
        done = true;
    });
    while (!done) {
        auto [_, err] = vfs::ReadDir(*fs, dir);
        ASSERT_FALSE(err) << err;
        runtime::Gosched();
    }
    wg.Wait();
}

// The root is a directory.
TEST(GoVfsMemory, MemFsRootDirMode) {
    auto fs = vfs::NewMemoryFs();
    auto [info, err] = fs->Stat("/");
    ASSERT_FALSE(err) << err;
    EXPECT_TRUE(info->IsDir()) << "should be a directory";
    EXPECT_TRUE(info->Mode().IsDir()) << "FileMode is not directory, is " << info->Mode();
}

// MkdirAll creates intermediate directories with the mode.
TEST(GoVfsMemory, MemFsMkdirAllMode) {
    auto fs = vfs::NewMemoryFs();
    ASSERT_FALSE(fs->MkdirAll("/a/b/c", 0755));
    for (std::string_view name : {"/a", "/a/b", "/a/b/c"}) {
        auto [info, err] = fs->Stat(name);
        ASSERT_FALSE(err) << err;
        EXPECT_TRUE(info->Mode().IsDir()) << name << ": mode is not directory";
        EXPECT_TRUE(info->ModTime().After(time::Now().Add(-1 * time::Hour))) << name << ": mod time not set";
        EXPECT_EQ(info->Mode(), fs::ModeDir | 0755) << name << ": wrong permissions, got " << info->Mode();
    }
}

// MkdirAll doesn't change the permissions of directories that exist.
TEST(GoVfsMemory, MemFsMkdirAllNoClobber) {
    auto fs = vfs::NewMemoryFs();
    ASSERT_FALSE(fs->MkdirAll("/a/b/c", 0755));
    expectMode(*fs, "/a/b", fs::ModeDir | 0755);
    ASSERT_FALSE(fs->MkdirAll("/a/b/c/d/e/f", 0710));
    // '/a/b' is unchanged.
    expectMode(*fs, "/a/b", fs::ModeDir | 0755);
    // The new directories have the new permissions.
    expectMode(*fs, "/a/b/c/d", fs::ModeDir | 0710);
    expectMode(*fs, "/a/b/c/d/e", fs::ModeDir | 0710);
    expectMode(*fs, "/a/b/c/d/e/f", fs::ModeDir | 0710);
}

TEST(GoVfsMemory, MemFsDirMode) {
    auto fs = vfs::NewMemoryFs();
    EXPECT_FALSE(fs->Mkdir("/testDir1", 0644));
    EXPECT_FALSE(fs->MkdirAll("/sub/testDir2", 0644));
    for (std::string_view name : {"/testDir1", "/sub/testDir2"}) {
        auto [info, err] = fs->Stat(name);
        ASSERT_FALSE(err) << err;
        EXPECT_TRUE(info->IsDir()) << "should be a directory";
        EXPECT_TRUE(info->Mode().IsDir()) << "FileMode is not directory";
    }
}

TEST(GoVfsMemory, MemFsUnexpectedEOF) {
    auto fs = vfs::NewMemoryFs();
    ASSERT_FALSE(vfs::WriteFile(*fs, "file.txt", as_bytes("abc"), 0777));
    auto [f, err] = fs->Open("file.txt");
    ASSERT_FALSE(err) << err;
    // Seek beyond the end.
    ASSERT_FALSE(f->Seek(512, 0).second);
    std::vector<byte> buff(256);
    auto [_, rerr] = io::ReadAtLeast(*f, buff, 256);
    EXPECT_EQ(rerr, io::ErrUnexpectedEOF) << "Expected ErrUnexpectedEOF";
    f->Close();
}

TEST(GoVfsMemory, MemFsChmod) {
    auto fs = vfs::NewMemoryFs();
    ASSERT_FALSE(fs->Mkdir("hello", 0700));
    auto [info, err] = fs->Stat("hello");
    ASSERT_FALSE(err) << err;
    ASSERT_EQ(info->Mode().String(), "drwx------") << "mkdir failed to create a directory";

    EXPECT_FALSE(fs->Chmod("hello", 0)) << "Failed to run chmod";
    std::tie(info, err) = fs->Stat("hello");
    ASSERT_FALSE(err) << err;
    EXPECT_EQ(info->Mode().String(), "d---------") << "chmod should not change file type";
}

// Mkdir can't set other bits than the permissions.
TEST(GoVfsMemory, MemFsMkdirModeIllegal) {
    auto fs = vfs::NewMemoryFs();
    ASSERT_FALSE(fs->Mkdir("/a", fs::ModeSocket | 0755));
    expectMode(*fs, "/a", fs::ModeDir | 0755);
}

// Nor can OpenFile.
TEST(GoVfsMemory, MemFsOpenFileModeIllegal) {
    auto fs = vfs::NewMemoryFs();
    auto [file, err] = fs->OpenFile("/a", os::O_CREATE_, fs::ModeSymlink | 0644);
    ASSERT_FALSE(err) << err;
    expectMode(*fs, "/a", 0644);
    file->Close();
}

// LstatIfPossible always says false: a MemoryFs has no links.
TEST(GoVfsMemory, MemFsLstatIfPossible) {
    auto fs = vfs::NewMemoryFs();
    auto* fsAsserted = dynamic_cast<vfs::Lstater*>(fs.get());
    ASSERT_TRUE(fsAsserted) << "The filesytem does not implement Lstater";
    auto [file, err] = fs->OpenFile("/a.txt", os::O_CREATE_, 0644);
    ASSERT_FALSE(err) << "Error when opening file: " << err;
    auto [_, lstatCalled, lerr] = fsAsserted->LstatIfPossible("/a.txt");
    ASSERT_FALSE(lerr) << "Function returned err: " << lerr;
    EXPECT_FALSE(lstatCalled) << "Function indicated lstat was called. This should never be true.";
    file->Close();
}

TEST(GoVfsMemory, MemMapFsConfurrentMkdir) {
    constexpr std::string_view dir = "test_dir";
    constexpr int n = 1000;
    auto mfs = vfs::NewMemoryFs();

    std::vector<string> allFilePaths;
    sync::WaitGroup wg;
    for (int i = 0; i < n; i++) {
        // afero's test puts them all in "00" (n % 10).
        string fp = filepath::Join(dir, "00", std::to_string(i) + ".txt");
        allFilePaths.push_back(fp);
        wg.Go([mfs, fp] {
            EXPECT_FALSE(mfs->MkdirAll(filepath::Dir(fp), 0755));
            auto [wt, err] = mfs->Create(fp);
            EXPECT_FALSE(err) << err;
            // 30 bytes
            for (int j = 0; j < 10; j++) EXPECT_FALSE(wt->Write(as_bytes("000")).second);
            EXPECT_FALSE(wt->Close());
        });
    }
    wg.Wait();

    // Each file, by its name.
    for (auto& fp : allFilePaths) {
        auto [info, err] = mfs->Stat(fp);
        ASSERT_FALSE(err) << err;
        EXPECT_EQ(info->Size(), 30) << "file size should be 30";
    }

    // Each file, by walking.
    std::vector<string> foundFiles;
    error wErr = vfs::Walk(*mfs, dir, [&](std::string_view path, const ptr<fs::FileInfo>& info, error err) {
        EXPECT_FALSE(err) << err;
        if (info->IsDir()) return error();  // skip directories
        if (info->Name().ends_with(".txt")) foundFiles.push_back(string(path));
        return error();
    });
    EXPECT_FALSE(wErr) << wErr;
    EXPECT_EQ(foundFiles.size(), static_cast<std::size_t>(n));
}

TEST(GoVfsMemory, MemFsRenameDir) {
    auto fs = vfs::NewMemoryFs();
    ASSERT_FALSE(fs->MkdirAll("/src/dir", 0777));
    auto [f, err] = fs->Create("/src/file.txt");
    ASSERT_FALSE(err) << "Create failed: " << err;
    ASSERT_FALSE(f->Close());

    ASSERT_FALSE(fs->Rename("/src", "/dst"));
    EXPECT_TRUE(fs->Stat("/src/dir").second) << "SubDir still exists in the source dir";
    EXPECT_TRUE(fs->Stat("/src/file.txt").second) << "SubFile still exists in the source dir";
    EXPECT_FALSE(fs->Stat("/dst/dir").second) << "SubDir stat in the destination dir";
    EXPECT_FALSE(fs->Stat("/dst/file.txt").second) << "SubFile stat in the destination dir";

    ASSERT_FALSE(fs->Mkdir("/src", 0777)) << "Cannot recreate the source dir";
    EXPECT_FALSE(fs->Mkdir("/src/dir", 0777)) << "Cannot recreate the subdir in the source dir";
}

TEST(GoVfsMemory, MemMapFsRename) {
    registry reg;
    auto fsPtr = make_ptr<vfs::MemoryFs>();
    auto& fs = *fsPtr;
    string tDir = reg.testDir(fsPtr);
    constexpr std::string_view rFrom = "/renamefrom", rTo = "/renameto", rExists = "/renameexists";

    struct test {
        std::vector<string> dirs;
        string from, to, exists;
    };

    auto parts = strings::Split(tDir, "/");
    string root = "/";
    if (parts.size() > 1) root = filepath::Join("/", parts[1]);

    std::vector<test> testData;
    for (auto i = parts.size(); i > 0; i--) {
        string prefix = strings::Join(std::span(parts).first(i), "/");
        string suffix = strings::Join(std::span(parts).subspan(i), "/");
        testData.push_back({
            {filepath::Join(prefix, rFrom, suffix), filepath::Join(prefix, rExists, suffix)},
            filepath::Join(prefix, rFrom),
            filepath::Join(prefix, rTo),
            filepath::Join(prefix, rExists),
        });
    }

    for (auto& data : testData) {
        ASSERT_FALSE(fs.RemoveAll(root)) << "RemoveAll " << root;
        for (auto& dir : data.dirs) ASSERT_FALSE(fs.MkdirAll(dir, 0775)) << "MkdirAll " << dir;

        auto dataCnt = vfs::detail::memoryNames(fs).size();
        ASSERT_FALSE(fs.Rename(data.from, data.to)) << "rename " << data.from << ", " << data.to;
        ASSERT_FALSE(fs.Mkdir(data.from, 0775)) << "Mkdir " << data.from;
        EXPECT_FALSE(fs.Rename(data.from, data.exists)) << "rename " << data.from << ", " << data.exists;

        for (auto& p : vfs::detail::memoryNames(fs)) {
            EXPECT_EQ(p.find(data.from), string::npos) << "File was not renamed to renameto: " << p;
        }
        EXPECT_FALSE(fs.Stat(data.to).second) << "stat " << data.to;
        EXPECT_EQ(dataCnt, vfs::detail::memoryNames(fs).size()) << "invalid data len";
    }
}

// ---------------------------------------------------------------------------
// mem/file_test.go

TEST(GoVfsMemory, FileDataNameRace) {
    auto d = make_ptr<vfs::mem::FileData>("someName");
    EXPECT_EQ(d->Name(), "someName");
    vfs::mem::ChangeFileName(*d, "someOtherName");
    EXPECT_EQ(d->Name(), "someOtherName");
    sync::WaitGroup wg;
    wg.Go([d] { vfs::mem::ChangeFileName(*d, "someName"); });
    string name = d->Name();
    EXPECT_TRUE(name == "someName" || name == "someOtherName") << name;
    wg.Wait();
}

TEST(GoVfsMemory, FileDataModTimeRace) {
    time::Time someTime = time::Now();
    time::Time someOtherTime = someTime.Add(time::Minute);
    auto d = make_ptr<vfs::mem::FileData>("");
    d->modtime = someTime;
    auto s = vfs::mem::GetFileInfo(d);
    EXPECT_TRUE(s->ModTime().Equal(someTime));
    vfs::mem::SetModTime(*d, someOtherTime);
    EXPECT_TRUE(s->ModTime().Equal(someOtherTime));
    sync::WaitGroup wg;
    wg.Go([d, someTime] { vfs::mem::SetModTime(*d, someTime); });
    auto t = s->ModTime();
    EXPECT_TRUE(t.Equal(someTime) || t.Equal(someOtherTime));
    wg.Wait();
}

TEST(GoVfsMemory, FileDataModeRace) {
    auto d = make_ptr<vfs::mem::FileData>("");
    d->mode = 0777;
    auto s = vfs::mem::GetFileInfo(d);
    EXPECT_EQ(s->Mode(), fs::FileMode(0777));
    vfs::mem::SetMode(*d, 0660);
    EXPECT_EQ(s->Mode(), fs::FileMode(0660));
    sync::WaitGroup wg;
    wg.Go([d] { vfs::mem::SetMode(*d, 0777); });
    auto m = s->Mode();
    EXPECT_TRUE(m == fs::FileMode(0777) || m == fs::FileMode(0660));
    wg.Wait();
}

TEST(GoVfsMemory, FileWriteAt) {
    auto data = vfs::mem::CreateFile("abc.txt");
    auto f = vfs::mem::NewFileHandle(data);
    const byte testData[] = {1, 2, 3, 4, 5};
    constexpr int64 offset = 5;

    // 5 zeros + testData
    ASSERT_FALSE(f->WriteAt(testData, offset).second);
    // 2 * testData
    ASSERT_FALSE(f->WriteAt(testData, 0).second);
    // 3 * testData
    ASSERT_FALSE(f->WriteAt(testData, offset * 2).second);
    // 3 * testData + 5 zeros + testData
    ASSERT_FALSE(f->WriteAt(testData, offset * 4).second);
    // 5 * testData
    ASSERT_FALSE(f->WriteAt(testData, offset * 3).second);
    ASSERT_FALSE(f->Close());

    std::vector<byte> expected;
    for (int i = 0; i < 5; i++) expected.insert(expected.end(), std::begin(testData), std::end(testData));
    EXPECT_EQ(data->data, expected);
}

TEST(GoVfsMemory, FileDataIsDirRace) {
    auto d = make_ptr<vfs::mem::FileData>("");
    d->dir = true;
    auto s = vfs::mem::GetFileInfo(d);
    EXPECT_TRUE(s->IsDir());
    sync::WaitGroup wg;
    wg.Go([d] {
        std::lock_guard lock(d->mu);
        d->dir = false;
    });
    s->IsDir();  // a read, racing with the write
    wg.Wait();
}

TEST(GoVfsMemory, FileDataSizeRace) {
    auto d = make_ptr<vfs::mem::FileData>("");
    auto hello = as_bytes("Hello");
    d->data.assign(hello.begin(), hello.end());
    auto s = vfs::mem::GetFileInfo(d);
    EXPECT_EQ(s->Size(), 5);
    sync::WaitGroup wg;
    wg.Go([d] {
        std::lock_guard lock(d->mu);
        auto other = as_bytes("Hello World");
        d->data.assign(other.begin(), other.end());
    });
    s->Size();  // a read, racing with the write
    wg.Wait();

    // A directory's size.
    d->dir = true;
    EXPECT_EQ(s->Size(), 42);
}

TEST(GoVfsMemory, FileReadAtSeekOffset) {
    auto f = vfs::mem::NewFileHandle(vfs::mem::CreateFile("foo"));
    ASSERT_FALSE(f->WriteString("TEST").second);
    auto [offset, err] = f->Seek(0, io::SeekStart);
    ASSERT_FALSE(err);
    EXPECT_EQ(offset, 0);

    auto [offsetBeforeReadAt, berr] = f->Seek(0, io::SeekCurrent);
    ASSERT_FALSE(berr);
    ASSERT_EQ(offsetBeforeReadAt, 0);

    std::vector<byte> b(4);
    auto [n, rerr] = f->ReadAt(b, 0);
    ASSERT_FALSE(rerr) << rerr;
    EXPECT_EQ(n, 4);
    EXPECT_EQ(str(b), "TEST");

    auto [offsetAfterReadAt, aerr] = f->Seek(0, io::SeekCurrent);
    ASSERT_FALSE(aerr);
    ASSERT_EQ(offsetAfterReadAt, offsetBeforeReadAt) << "ReadAt should not affect offset";
    ASSERT_FALSE(f->Close());
}

TEST(GoVfsMemory, FileWriteAndSeek) {
    auto f = vfs::mem::NewFileHandle(vfs::mem::CreateFile("foo"));
    const byte data4[] = {0, 1, 2, 3};
    std::vector<byte> data20;
    for (int i = 0; i < 5; i++) data20.insert(data20.end(), std::begin(data4), std::end(data4));
    int64 off = 0;

    for (int i = 0; i < 100; i++) {
        // Write 20 bytes.
        auto [n, err] = f->Write(data20);
        ASSERT_FALSE(err) << err;
        off += n;
        ASSERT_EQ(n, 20);
        ASSERT_EQ(off, (i + 1) * 20);

        // Rewind to the start, and write 4 bytes there.
        auto [cur, serr] = f->Seek(-off, io::SeekCurrent);
        ASSERT_FALSE(serr);
        ASSERT_EQ(cur, 0);
        std::tie(n, err) = f->Write(data4);
        ASSERT_FALSE(err);
        ASSERT_EQ(n, 4);

        // Back at the end.
        std::tie(cur, serr) = f->Seek(off - n, io::SeekCurrent);
        ASSERT_FALSE(serr);
        ASSERT_EQ(cur, off);
    }
}
