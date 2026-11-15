// Ported from afero v1.15.0's tests: afero_test.go, ioutil_test.go,
// util_test.go, path_test.go, match_test.go, lstater_test.go and
// symlink_test.go, with afero's test names.
//
// Adapted: each test has its own MemoryFs, where afero's share one;
// TestReaderContains drops the cases of a nil reader, which C++'s
// references rule out.
#include "go_vfs_test_util.hpp"

#include <gopp/strings.hpp>
#include <gopp/time.hpp>

#include <algorithm>
#include <map>

using namespace vfs_test;

namespace {

// Go's fmt.Sprintln(n, err).
std::string sprintln(std::size_t n, const error& err) { return std::to_string(n) + " " + str(err.Error()) + "\n"; }

bool contains(const std::vector<string>& vector, std::string_view s) {
    return std::find(vector.begin(), vector.end(), s) != vector.end();
}

// afero's findNames
void findNames(vfs::Fs& fs, std::string_view tDir, std::string_view testSubDir, const std::vector<string>& root,
               const std::vector<string>& sub) {
    bool foundRoot = false;
    for (auto& e : root) {
        auto [f, err] = fs.Open(filepath::Join(tDir, e));
        EXPECT_FALSE(err) << "Open " << filepath::Join(tDir, e) << ": " << err;
        if (f) f->Close();
        if (e == "we") foundRoot = true;
    }
    EXPECT_TRUE(foundRoot) << "Didn't find subdirectory we";

    bool found1 = false, found2 = false;
    for (auto& e : sub) {
        auto [f, err] = fs.Open(filepath::Join(testSubDir, e));
        EXPECT_FALSE(err) << "Open " << filepath::Join(testSubDir, e) << ": " << err;
        if (f) f->Close();
        if (e == "testfile1") found1 = true;
        if (e == "testfile2") found2 = true;
    }
    EXPECT_TRUE(found1) << "Didn't find testfile1";
    EXPECT_TRUE(found2) << "Didn't find testfile2";
}

}  // namespace

// ---------------------------------------------------------------------------
// afero_test.go

// Read with length 0 should not return EOF.
TEST(GoVfs, Read0) {
    registry reg;
    for (auto& fs : newFss()) {
        auto f = reg.tmpFile(fs);
        f->WriteString(
            "Lorem ipsum dolor sit amet, consectetur adipisicing elit, sed do eiusmod tempor incididunt ut labore et "
            "dolore magna aliqua. Ut enim ad minim veniam, quis nostrud exercitation ullamco laboris nisi ut aliquip "
            "ex ea commodo consequat. Duis aute irure dolor in reprehenderit in voluptate velit esse cillum dolore eu "
            "fugiat nulla pariatur. Excepteur sint occaecat cupidatat non proident, sunt in culpa qui officia deserunt "
            "mollit anim id est laborum.");
        auto [n, err] = f->Read({});
        EXPECT_TRUE(n == 0 && !err) << fs->Name() << ": Read(0) = " << n << ", " << err << ", want 0, nil";
        f->Seek(0, 0);
        std::vector<byte> b(100);
        std::tie(n, err) = f->Read(b);
        EXPECT_TRUE(n > 0 && !err) << fs->Name() << ": Read(100) = " << n << ", " << err << ", want >0, nil";
        f->Close();
    }
}

TEST(GoVfs, OpenFile) {
    registry reg;
    for (auto& fs : newFss()) {
        string tmp = reg.testDir(fs);
        string path = filepath::Join(tmp, "test.txt");

        auto [f, err] = fs->OpenFile(path, os::O_RDWR_ | os::O_CREATE_, 0600);
        ASSERT_FALSE(err) << fs->Name() << " OpenFile (O_CREATE) failed: " << err;
        io::WriteString(*f, "initial");
        f->Close();

        std::tie(f, err) = fs->OpenFile(path, os::O_WRONLY_ | os::O_APPEND_, 0600);
        ASSERT_FALSE(err) << fs->Name() << " OpenFile (O_APPEND) failed: " << err;
        io::WriteString(*f, "|append");
        f->Close();

        std::tie(f, err) = fs->OpenFile(path, os::O_RDONLY_, 0600);
        auto [contents, _] = io::ReadAll(*f);
        EXPECT_EQ(str(contents), "initial|append") << fs->Name() << ": appending";
        f->Close();

        std::tie(f, err) = fs->OpenFile(path, os::O_RDWR_ | os::O_TRUNC_, 0600);
        ASSERT_FALSE(err) << fs->Name() << " OpenFile (O_TRUNC) failed: " << err;
        std::tie(contents, std::ignore) = io::ReadAll(*f);
        EXPECT_EQ(str(contents), "") << fs->Name() << ": expected truncated file";
        f->Close();
    }
}

TEST(GoVfs, Create) {
    registry reg;
    for (auto& fs : newFss()) {
        string tmp = reg.testDir(fs);
        string path = filepath::Join(tmp, "test.txt");

        auto [f, err] = fs->Create(path);
        ASSERT_FALSE(err) << fs->Name() << " Create failed: " << err;
        io::WriteString(*f, "initial");
        f->Close();

        std::tie(f, err) = fs->Create(path);
        ASSERT_FALSE(err) << fs->Name() << " Create failed: " << err;
        std::string secondContent = "second create";
        io::WriteString(*f, secondContent);
        f->Close();

        std::tie(f, err) = fs->Open(path);
        ASSERT_FALSE(err) << fs->Name() << " Open failed: " << err;
        auto [buf, rerr] = vfs::ReadAll(*f);
        ASSERT_FALSE(rerr) << fs->Name() << " ReadAll failed: " << rerr;
        EXPECT_EQ(str(buf), secondContent) << fs->Name();
        f->Close();
    }
}

TEST(GoVfs, MemFileRead) {
    registry reg;
    auto f = reg.tmpFile(vfs::NewMemoryFs());
    f->WriteString("abcd");
    f->Seek(0, 0);
    std::vector<byte> b(8);
    auto [n, err] = f->Read(b);
    EXPECT_EQ(n, 4) << "didn't read all bytes: " << err;
    EXPECT_FALSE(err) << "err is not nil: " << err;
    std::tie(n, err) = f->Read(b);
    EXPECT_EQ(n, 0) << "read more bytes";
    EXPECT_EQ(err, io::EOF_) << "error is not EOF: " << err;
}

TEST(GoVfs, Rename) {
    registry reg;
    for (auto& fs : newFss()) {
        string tDir = reg.testDir(fs);
        string from = filepath::Join(tDir, "/renamefrom");
        string to = filepath::Join(tDir, "/renameto");
        string exists = filepath::Join(tDir, "/renameexists");
        auto [file, err] = fs->Create(from);
        ASSERT_FALSE(err) << fs->Name() << ": open " << to << " failed: " << err;
        EXPECT_FALSE(file->Close()) << fs->Name() << ": close failed";
        std::tie(file, err) = fs->Create(exists);
        ASSERT_FALSE(err) << fs->Name() << ": open " << to << " failed: " << err;
        EXPECT_FALSE(file->Close()) << fs->Name() << ": close failed";
        err = fs->Rename(from, to);
        ASSERT_FALSE(err) << fs->Name() << ": rename " << to << ", " << from << " failed: " << err;
        std::tie(file, err) = fs->Create(from);
        ASSERT_FALSE(err) << fs->Name() << ": open " << to << " failed: " << err;
        EXPECT_FALSE(file->Close()) << fs->Name() << ": close failed";
        err = fs->Rename(from, exists);
        EXPECT_FALSE(err) << fs->Name() << ": rename " << exists << ", " << from << " failed: " << err;

        auto [d, oerr] = fs->Open(tDir);
        ASSERT_FALSE(oerr);
        auto [names, rerr] = d->Readdirnames(-1);
        d->Close();
        EXPECT_FALSE(rerr) << fs->Name() << ": readDirNames error: " << rerr;
        EXPECT_FALSE(contains(names, "renamefrom")) << "File is still called renamefrom";
        EXPECT_TRUE(contains(names, "renameto")) << "File was not renamed to renameto";

        auto [_, serr] = fs->Stat(to);
        EXPECT_FALSE(serr) << fs->Name() << ": stat " << to << " failed: " << serr;
    }
}

TEST(GoVfs, Remove) {
    for (auto& fs : newFss()) {
        auto [x, err] = vfs::TempFile(*fs, "", "afero");
        ASSERT_FALSE(err) << "unable to work with temp file" << err;
        string path = x->Name();
        x->Close();
        string tDir = filepath::Dir(path);

        err = fs->Remove(path);
        ASSERT_FALSE(err) << fs->Name() << ": Remove() failed: " << err;

        auto [_, serr] = fs->Stat(path);
        EXPECT_TRUE(os::IsNotExist(serr)) << fs->Name() << ": Remove() didn't remove file";

        // Deleting a file that doesn't exist fails.
        err = fs->Remove(path);
        EXPECT_TRUE(os::IsNotExist(err)) << fs->Name() << ": Remove() didn't raise error for non-existent file";

        auto [f, oerr] = fs->Open(tDir);
        ASSERT_FALSE(oerr) << "TestDir should still exist: " << oerr;
        auto [names, rerr] = f->Readdirnames(-1);
        EXPECT_FALSE(rerr) << "Readdirnames failed: " << rerr;
        EXPECT_FALSE(contains(names, filepath::Base(path))) << "File was not removed from parent directory";
        f->Close();
    }
}

TEST(GoVfs, Truncate) {
    registry reg;
    for (auto& fs : newFss()) {
        SCOPED_TRACE(str(fs->Name()));
        auto f = reg.tmpFile(fs);
        checkSize(*f, 0);
        f->Write(as_bytes("hello, world\n"));
        checkSize(*f, 13);
        f->Truncate(10);
        checkSize(*f, 10);
        f->Truncate(1024);
        checkSize(*f, 1024);
        f->Truncate(0);
        checkSize(*f, 0);
        if (auto [_, err] = f->Write(as_bytes("surprise!")); !err) {
            checkSize(*f, 13 + 9);  // wrote at offset past where hello, world was
        }
        f->Close();
    }
}

TEST(GoVfs, Seek) {
    registry reg;
    for (auto& fs : newFss()) {
        auto f = reg.tmpFile(fs);
        const std::string data = "hello, world\n";
        io::WriteString(*f, data);
        auto len = static_cast<int64>(data.size());

        struct test {
            int64 in;
            int whence;
            int64 out;
        };
        test tests[] = {
            {0, 1, len},
            {0, 0, 0},
            {5, 0, 5},
            {0, 2, len},
            {0, 0, 0},
            {-1, 2, len - 1},
            {int64(1) << 33, 0, int64(1) << 33},
            {int64(1) << 33, 2, (int64(1) << 33) + len},
        };
        int i = 0;
        for (auto& tt : tests) {
            auto [off, err] = f->Seek(tt.in, tt.whence);
            EXPECT_TRUE(off == tt.out && !err) << fs->Name() << " #" << i << ": Seek(" << tt.in << ", " << tt.whence
                                               << ") = " << off << ", " << err << " want " << tt.out << ", nil";
            i++;
        }
        f->Close();
    }
}

TEST(GoVfs, ReadAt) {
    registry reg;
    for (auto& fs : newFss()) {
        auto f = reg.tmpFile(fs);
        io::WriteString(*f, "hello, world\n");
        std::vector<byte> b(5);
        auto [n, err] = f->ReadAt(b, 7);
        ASSERT_TRUE(!err && n == 5) << fs->Name() << ": ReadAt 7: " << n << ", " << err;
        EXPECT_EQ(str(b), "world") << fs->Name();
        f->Close();
    }
}

TEST(GoVfs, WriteAt) {
    registry reg;
    for (auto& fs : newFss()) {
        auto f = reg.tmpFile(fs);
        io::WriteString(*f, "hello, world\n");
        auto [n, err] = f->WriteAt(as_bytes("WORLD"), 7);
        ASSERT_TRUE(!err && n == 5) << fs->Name() << ": WriteAt 7: " << n << ", " << err;

        auto [f2, oerr] = fs->Open(f->Name());
        ASSERT_FALSE(oerr) << fs->Name() << ": ReadFile " << f->Name() << ": " << oerr;
        auto [b, _] = io::ReadAll(*f2);
        EXPECT_EQ(str(b), "hello, WORLD\n") << fs->Name() << ": after write";
        f2->Close();
        f->Close();
    }
}

TEST(GoVfs, Readdirnames) {
    registry reg;
    for (auto& fs : newFss()) {
        string testSubDir = setupTestDir(reg, fs);
        string tDir = filepath::Dir(testSubDir);

        auto [root, err] = fs->Open(tDir);
        ASSERT_FALSE(err) << fs->Name() << " " << tDir << " " << err;
        auto [namesRoot, rerr] = root->Readdirnames(-1);
        ASSERT_FALSE(rerr) << fs->Name() << " " << rerr;
        root->Close();

        auto [sub, serr] = fs->Open(testSubDir);
        ASSERT_FALSE(serr) << serr;
        auto [namesSub, srerr] = sub->Readdirnames(-1);
        ASSERT_FALSE(srerr) << fs->Name() << " " << srerr;
        sub->Close();

        findNames(*fs, tDir, testSubDir, namesRoot, namesSub);
    }
}

TEST(GoVfs, ReaddirSimple) {
    registry reg;
    for (auto& fs : newFss()) {
        string testSubDir = setupTestDir(reg, fs);
        string tDir = filepath::Dir(testSubDir);

        auto [root, err] = fs->Open(tDir);
        ASSERT_FALSE(err) << err;
        auto [rootInfo, rerr] = root->Readdir(1);
        EXPECT_FALSE(rerr) << fs->Name() << ": " << rerr;
        std::tie(rootInfo, rerr) = root->Readdir(5);
        EXPECT_EQ(rerr, io::EOF_) << fs->Name() << ": " << rerr;
        root->Close();

        auto [sub, serr] = fs->Open(testSubDir);
        ASSERT_FALSE(serr) << serr;
        auto [subInfo, srerr] = sub->Readdir(5);
        EXPECT_FALSE(srerr) << fs->Name() << ": " << srerr;
        sub->Close();
    }
}

TEST(GoVfs, Readdir) {
    registry reg;
    constexpr int nums = 6;
    auto fss = newFss();
    for (int num = 0; num < nums; num++) {
        std::vector<std::string> outputs(fss.size()), infos(fss.size());
        for (std::size_t i = 0; i < fss.size(); i++) {
            auto& fs = fss[i];
            string testSubDir = setupTestDir(reg, fs);
            auto [root, err] = fs->Open(testSubDir);
            ASSERT_FALSE(err) << err;

            std::vector<std::string> infosn(nums);
            for (int j = 0; j < nums; j++) {
                auto [info, rerr] = root->Readdir(num);
                std::string s = sprintln(info.size(), rerr);
                for (auto& e : info) outputs[i] += "  " + str(e->Name()) + "\n";
                outputs[i] += "  Error: " + str(rerr.Error()) + "\n";
                infosn[j] = s;
                infos[i] += s;
            }
            root->Close();

            // fs::ReadDirFile's ReadDir too.
            std::tie(root, err) = fs->Open(testSubDir);
            ASSERT_FALSE(err) << err;
            for (int j = 0; j < nums; j++) {
                auto [dirEntries, rerr] = root->ReadDir(num);
                std::string s = sprintln(dirEntries.size(), rerr);
                ASSERT_EQ(s, infosn[j]) << fs->Name();
            }
            root->Close();
        }
        for (std::size_t i = 1; i < infos.size(); i++) {
            EXPECT_EQ(infos[i], infos[i - 1]) << "Readdir outputs not equal for Readdir(" << num << ")\n"
                                              << fss[i - 1]->Name() << "\n"
                                              << outputs[i - 1] << fss[i]->Name() << "\n"
                                              << outputs[i];
        }
    }
}

// https://github.com/spf13/afero/issues/169
TEST(GoVfs, ReaddirRegularFile) {
    registry reg;
    for (auto& fs : newFss()) {
        auto f = reg.tmpFile(fs);
        auto [_, err] = f->Readdirnames(-1);
        EXPECT_TRUE(err) << fs->Name() << ": Expected error";
        auto [_, rerr] = f->Readdir(-1);
        EXPECT_TRUE(rerr) << fs->Name() << ": Expected error";
        f->Close();
    }
}

TEST(GoVfs, ReaddirAll) {
    registry reg;
    for (auto& fs : newFss()) {
        string testSubDir = setupTestDir(reg, fs);
        string tDir = filepath::Dir(testSubDir);

        auto [root, err] = fs->Open(tDir);
        ASSERT_FALSE(err) << err;
        auto [rootInfo, rerr] = root->Readdir(-1);
        ASSERT_FALSE(rerr) << rerr;
        root->Close();
        std::vector<string> namesRoot;
        for (auto& e : rootInfo) namesRoot.push_back(e->Name());

        auto [sub, serr] = fs->Open(testSubDir);
        ASSERT_FALSE(serr) << serr;
        auto [subInfo, srerr] = sub->Readdir(-1);
        ASSERT_FALSE(srerr) << srerr;
        sub->Close();
        std::vector<string> namesSub;
        for (auto& e : subInfo) namesSub.push_back(e->Name());

        findNames(*fs, tDir, testSubDir, namesRoot, namesSub);
    }
}

// ---------------------------------------------------------------------------
// ioutil_test.go

TEST(GoVfs, ReadFile) {
    auto testFS = vfs::NewMemoryFs();
    vfs::Vfs fsutil{testFS};

    testFS->Create("this_exists.go");
    auto [_, err] = fsutil.ReadFile("rumpelstilzchen");
    EXPECT_TRUE(err) << "ReadFile rumpelstilzchen: error expected, none found";

    auto [contents, rerr] = fsutil.ReadFile("this_exists.go");
    ASSERT_FALSE(rerr) << "ReadFile this_exists.go: " << rerr;
    auto [dir, serr] = testFS->Stat("this_exists.go");
    ASSERT_FALSE(serr);
    EXPECT_EQ(dir->Size(), static_cast<int64>(contents.size()));
}

TEST(GoVfs, WriteFile) {
    auto testFS = vfs::NewMemoryFs();
    vfs::Vfs fsutil{testFS};
    auto [f, err] = fsutil.TempFile("", "ioutil-test");
    ASSERT_FALSE(err) << err;
    string filename = f->Name();
    std::string data =
        "Programming today is a race between software engineers striving to "
        "build bigger and better idiot-proof programs, and the Universe trying "
        "to produce bigger and better idiots. So far, the Universe is winning.";

    err = fsutil.WriteFile(filename, as_bytes(data), 0644);
    ASSERT_FALSE(err) << "WriteFile " << filename << ": " << err;

    auto [contents, rerr] = fsutil.ReadFile(filename);
    ASSERT_FALSE(rerr) << "ReadFile " << filename << ": " << rerr;
    EXPECT_EQ(str(contents), data);

    f->Close();
    testFS->Remove(filename);
}

TEST(GoVfs, ReadDir) {
    auto testFS = vfs::NewMemoryFs();
    testFS->Mkdir("/i-am-a-dir", 0777);
    testFS->Create("/this_exists.go");
    auto [_, err] = vfs::ReadDir(*testFS, "rumpelstilzchen");
    EXPECT_TRUE(err) << "ReadDir rumpelstilzchen: error expected, none found";

    auto [list, rerr] = vfs::ReadDir(*testFS, "..");
    ASSERT_FALSE(rerr) << "ReadDir ..: " << rerr;
    bool foundFile = false, foundSubDir = false;
    for (auto& dir : list) {
        if (!dir->IsDir() && dir->Name() == "this_exists.go") foundFile = true;
        if (dir->IsDir() && dir->Name() == "i-am-a-dir") foundSubDir = true;
    }
    EXPECT_TRUE(foundFile) << "ReadDir ..: this_exists.go file not found";
    EXPECT_TRUE(foundSubDir) << "ReadDir ..: i-am-a-dir directory not found";
}

TEST(GoVfs, TempFile) {
    struct test {
        std::string name;
        std::string pattern;
        std::function<bool(std::string_view base)> want;
    };
    test tests[] = {
        // a simple file name
        {"foo", "foo", [](std::string_view base) { return base.starts_with("foo") && base.size() > 3; }},
        // with an extension
        {"foo.bar", "foo.bar", [](std::string_view base) { return base.starts_with("foo.bar") && base.size() > 7; }},
        // with a wildcard
        {"foo-*.bar", "foo-*.bar",
         [](std::string_view base) {
             return (base.starts_with("foo-") || base.starts_with("bar")) && base.size() > 9;
         }},
    };
    for (auto& tt : tests) {
        auto [file, err] = vfs::TempFile(*vfs::NewMemoryFs(), "", tt.pattern);
        ASSERT_FALSE(err) << tt.name << ": TempFile() error = " << err << ", none expected";
        ASSERT_TRUE(file) << tt.name << ": TempFile() file should not be nil";
        string base = filepath::Base(file->Name());
        EXPECT_TRUE(tt.want(base)) << tt.name << ": TempFile() file = " << base << ", invalid file name";
    }
}

// ---------------------------------------------------------------------------
// util_test.go

TEST(GoVfs, DirExists) {
    auto testFS = vfs::NewMemoryFs();
    // Directories, so that there's something in the file system.
    testFS->MkdirAll("/foo/bar", 0777);

    std::pair<std::string_view, bool> data[] = {
        {".", true},
        {"./", true},
        {"..", true},
        {"../", true},
        {"./..", true},
        {"./../", true},
        {"/foo/", true},
        {"/foo", true},
        {"/foo/bar", true},
        {"/foo/bar/", true},
        {"/", true},
        {"/some-really-random-directory-name", false},
        {"/some/really/random/directory/name", false},
        {"./some-really-random-local-directory-name", false},
        {"./some/really/random/local/directory/name", false},
    };
    int i = 0;
    for (auto& [input, expected] : data) {
        auto [exists, _] = vfs::DirExists(*testFS, filepath::FromSlash(input));
        EXPECT_EQ(exists, expected) << "Test " << i << " " << input;
        i++;
    }
}

TEST(GoVfs, IsDir) {
    auto testFS = vfs::NewMemoryFs();
    std::pair<std::string_view, bool> data[] = {
        {"./", true},
        {"/", true},
        {"./this-directory-does-not-existi", false},
        {"/this-absolute-directory/does-not-exist", false},
    };
    int i = 0;
    for (auto& [input, expected] : data) {
        auto [exists, _] = vfs::IsDir(*testFS, input);
        EXPECT_EQ(exists, expected) << "Test " << i;
        i++;
    }
}

namespace {

// util_test.go's temporary files and directories, in a MemoryFs.
struct utilFiles {
    explicit utilFiles(ptr<vfs::Fs> fs) : testFS(std::move(fs)) {}

    ptr<vfs::File> createZeroSizedFileInTempDir() {
        auto [f, err] = vfs::TempFile(*testFS, "", "_path_test_");
        EXPECT_FALSE(err) << err;
        return f;
    }

    ptr<vfs::File> createNonZeroSizedFileInTempDir() {
        auto f = createZeroSizedFileInTempDir();
        EXPECT_FALSE(vfs::WriteFile(*testFS, f->Name(), as_bytes("byteString"), 0644));
        return f;
    }

    string createEmptyTempDir() {
        auto [d, err] = vfs::TempDir(*testFS, "", "_dir_prefix_");
        EXPECT_FALSE(err) << err;
        return d;
    }

    string createTempDirWithZeroLengthFiles() {
        string d = createEmptyTempDir();
        auto [_, err] = vfs::TempFile(*testFS, d, "_path_test_");
        EXPECT_FALSE(err) << err;
        return d;
    }

    string createTempDirWithNonZeroLengthFiles() {
        string d = createEmptyTempDir();
        auto [f, err] = vfs::TempFile(*testFS, d, "_path_test_");
        EXPECT_FALSE(err) << err;
        EXPECT_FALSE(vfs::WriteFile(*testFS, f->Name(), as_bytes("byteString"), 0644));
        return d;
    }

    ptr<vfs::Fs> testFS;
};

}  // namespace

TEST(GoVfs, IsEmpty) {
    utilFiles u(vfs::NewMemoryFs());
    auto zeroSizedFile = u.createZeroSizedFileInTempDir();
    auto nonZeroSizedFile = u.createNonZeroSizedFileInTempDir();
    string emptyDirectory = u.createEmptyTempDir();
    string nonEmptyZeroLengthFilesDirectory = u.createTempDirWithZeroLengthFiles();
    string nonEmptyNonZeroLengthFilesDirectory = u.createTempDirWithNonZeroLengthFiles();
    string nonExistentFile = os::TempDir() + "/this-file-does-not-exist.txt";
    string nonExistentDir = os::TempDir() + "/this/directory/does/not/exist/";

    struct test {
        string input;
        bool expectedResult;
        std::string expectedErr;  // empty for nil
    };
    test data[] = {
        {zeroSizedFile->Name(), true, ""},
        {nonZeroSizedFile->Name(), false, ""},
        {emptyDirectory, true, ""},
        {nonEmptyZeroLengthFilesDirectory, false, ""},
        {nonEmptyNonZeroLengthFilesDirectory, false, ""},
        {nonExistentFile, false, str(strconv::Quote(nonExistentFile)) + " path does not exist"},
        {nonExistentDir, false, str(strconv::Quote(nonExistentDir)) + " path does not exist"},
    };
    int i = 0;
    for (auto& d : data) {
        auto [exists, err] = vfs::IsEmpty(*u.testFS, d.input);
        EXPECT_EQ(exists, d.expectedResult) << "Test " << i << " " << d.input << " failed exists";
        if (!d.expectedErr.empty()) {
            EXPECT_EQ(str(err.Error()), d.expectedErr) << "Test " << i;
        } else {
            EXPECT_FALSE(err) << "Test " << i << ": " << err;
        }
        i++;
    }
}

TEST(GoVfs, ReaderContains) {
    struct test {
        std::string v1;
        std::vector<std::string> v2;
        bool expect;
    };
    test tests[] = {
        {"abc", {"a"}, true},
        {"abc", {"b"}, true},
        {"abcdefg", {"efg"}, true},
        {"abc", {"d"}, false},
        {"abc", {"d", "e"}, false},
        {"abc", {"d", "a"}, true},
        {"abc", {"b", "e"}, true},
        {"", {}, false},
        {"", {"a"}, false},
        {"a", {""}, false},
        {"", {""}, false},
    };
    int i = 0;
    for (auto& t : tests) {
        std::vector<std::span<const byte>> subslices;
        for (auto& s : t.v2) subslices.push_back(as_bytes(s));
        auto r = strings::NewReader(t.v1);
        EXPECT_EQ(vfs::detail::readerContainsAny(*r, subslices), t.expect) << "[" << i << "] readerContains";
        i++;
    }
}

TEST(GoVfs, Exists) {
    utilFiles u(vfs::NewMemoryFs());
    auto zeroSizedFile = u.createZeroSizedFileInTempDir();
    auto nonZeroSizedFile = u.createNonZeroSizedFileInTempDir();
    string emptyDirectory = u.createEmptyTempDir();
    string nonExistentFile = os::TempDir() + "/this-file-does-not-exist.txt";
    string nonExistentDir = os::TempDir() + "/this/directory/does/not/exist/";

    std::pair<string, bool> data[] = {
        {zeroSizedFile->Name(), true}, {nonZeroSizedFile->Name(), true}, {emptyDirectory, true},
        {nonExistentFile, false},      {nonExistentDir, false},
    };
    int i = 0;
    for (auto& [input, expected] : data) {
        auto [exists, err] = vfs::Exists(*u.testFS, input);
        EXPECT_EQ(exists, expected) << "Test " << i;
        EXPECT_FALSE(err) << "Test " << i << ": " << err;
        i++;
    }
}

TEST(GoVfs, SafeWriteToDisk) {
    utilFiles u(vfs::NewMemoryFs());
    auto emptyFile = u.createZeroSizedFileInTempDir();
    string tmpDir = u.createEmptyTempDir();

    std::string randomString = "This is a random string!";
    auto reader = strings::NewReader(randomString);
    std::string nowStr = std::to_string(time::Now().Unix());

    std::pair<string, std::string> data[] = {
        {emptyFile->Name(), str(emptyFile->Name()) + " already exists"},
        {tmpDir + "/" + std::string_view(nowStr), ""},
    };
    int i = 0;
    for (auto& [filename, expectedErr] : data) {
        error e = vfs::SafeWriteReader(*u.testFS, filename, *reader);
        if (!expectedErr.empty()) {
            EXPECT_EQ(str(e.Error()), expectedErr) << "Test " << i;
        } else {
            EXPECT_FALSE(e) << "Test " << i << ": " << e;
            auto [contents, _] = vfs::ReadFile(*u.testFS, filename);
            EXPECT_EQ(str(contents), randomString) << "Test " << i;
        }
        reader->Seek(0, 0);
        i++;
    }
}

TEST(GoVfs, WriteToDisk) {
    utilFiles u(vfs::NewMemoryFs());
    auto emptyFile = u.createZeroSizedFileInTempDir();
    string tmpDir = u.createEmptyTempDir();

    std::string randomString = "This is a random string!";
    auto reader = strings::NewReader(randomString);
    std::string nowStr = std::to_string(time::Now().Unix());

    string data[] = {emptyFile->Name(), tmpDir + "/" + std::string_view(nowStr)};
    int i = 0;
    for (auto& filename : data) {
        error e = vfs::WriteReader(*u.testFS, filename, *reader);
        EXPECT_FALSE(e) << "Test " << i << ": WriteToDisk Error " << e;
        auto [contents, rerr] = vfs::ReadFile(*u.testFS, filename);
        EXPECT_FALSE(rerr) << "Test " << i << ": Could not read file " << filename << ": " << rerr;
        EXPECT_EQ(str(contents), randomString) << "Test " << i;
        reader->Seek(0, 0);
        i++;
    }
}

TEST(GoVfs, GetTempDir) {
    std::string dir = str(os::TempDir());
    if (!dir.ends_with("/")) dir += "/";
    std::string testDir = "hugoTestFolder/";
    std::pair<std::string, std::string> tests[] = {
        {"", dir},
        {testDir + "  Foo bar  ", dir + testDir + "  Foo bar  /"},
        {testDir + "Foo.Bar/foo_Bar-Foo", dir + testDir + "Foo.Bar/foo_Bar-Foo/"},
        {testDir + "fOO,bar:foo%bAR", dir + testDir + "fOObarfoo%bAR/"},
        {testDir + "FOo/BaR.html", dir + testDir + "FOo/BaR.html/"},
        {testDir + "трям/трям", dir + testDir + "трям/трям/"},
        {testDir + "은행", dir + testDir + "은행/"},
        {testDir + "Банковский кассир", dir + testDir + "Банковский кассир/"},
    };
    for (auto& [input, expected] : tests) {
        EXPECT_EQ(str(vfs::GetTempDir(*vfs::NewMemoryFs(), input)), expected);
    }
}

TEST(GoVfs, FullBaseFsPath) {
    struct dirSpec {
        std::string_view Dir1, Dir2, Dir3;
    };
    dirSpec dirSpecs[] = {
        {"/", "/", "/"},
        {"/", "/path2", "/"},
        {"/path1/dir", "/path2/dir/", "/path3/dir"},
        {"C:/path1", "path2/dir", "/path3/dir/"},
    };
    for (auto& ds : dirSpecs) {
        auto memFs = vfs::NewMemoryFs();
        auto level1Fs = vfs::NewBasePathFs(memFs, ds.Dir1);
        auto level2Fs = vfs::NewBasePathFs(level1Fs, ds.Dir2);
        auto level3Fs = vfs::NewBasePathFs(level2Fs, ds.Dir3);

        struct spec {
            ptr<vfs::Fs> BaseFs;
            std::string_view FileName;
            string ExpectedPath;
        };
        spec specs[] = {
            {level3Fs, "f.txt", filepath::Join(ds.Dir1, ds.Dir2, ds.Dir3, "f.txt")},
            {level3Fs, "", filepath::Join(ds.Dir1, ds.Dir2, ds.Dir3, "")},
            {level2Fs, "f.txt", filepath::Join(ds.Dir1, ds.Dir2, "f.txt")},
            {level2Fs, "", filepath::Join(ds.Dir1, ds.Dir2, "")},
            {level1Fs, "f.txt", filepath::Join(ds.Dir1, "f.txt")},
            {level1Fs, "", filepath::Join(ds.Dir1, "")},
        };
        for (auto& s : specs) {
            auto& bp = dynamic_cast<vfs::BasePathFs&>(*s.BaseFs);
            EXPECT_EQ(vfs::FullBaseFsPath(bp, s.FileName), s.ExpectedPath);
        }
    }
}

// ---------------------------------------------------------------------------
// path_test.go

TEST(GoVfs, Walk) {
    registry reg;
    auto fss = newFss();
    string testDir;
    for (std::size_t i = 0; i < fss.size(); i++) {
        if (i == 0) {
            testDir = reg.testDir(fss[i]);
        } else {
            reg.add(fss[i], testDir);
        }
        setupTestFiles(*fss[i], testDir);
    }

    std::vector<std::string> outputs(fss.size());
    for (std::size_t i = 0; i < fss.size(); i++) {
        error err = vfs::Walk(*fss[i], testDir, [&](std::string_view path, const ptr<fs::FileInfo>& info, error err) {
            EXPECT_FALSE(err) << "walkFn err: " << err;
            int64 size = info->IsDir() ? 0 : info->Size();
            outputs[i] += std::string(path) + " " + str(info->Name()) + " " + std::to_string(size) + " " +
                          (info->IsDir() ? "true" : "false") + " " + str(err.Error()) + "\n";
            return error();
        });
        EXPECT_FALSE(err) << err;
    }
    for (std::size_t i = 1; i < outputs.size(); i++) {
        EXPECT_EQ(outputs[i], outputs[i - 1]) << "Walk outputs not equal!";
    }
}

// ---------------------------------------------------------------------------
// match_test.go

namespace {

string setupGlobFiles(vfs::Fs& fs, std::string_view path) {
    string testSubDir = filepath::Join(path, "globs", "bobs");
    error err = fs.MkdirAll(testSubDir, 0700);
    if (err && !os::IsExist(err)) ADD_FAILURE() << err;
    std::pair<std::string_view, std::string_view> files[] = {
        {"/matcher", "Testfile 1 content"},
        {"/../submatcher", "Testfile 2 content"},
        {"/../../match", "Testfile 3 content"},
    };
    for (auto& [name, content] : files) {
        auto [f, cerr] = fs.Create(filepath::Join(testSubDir, name));
        if (cerr) {
            ADD_FAILURE() << cerr;
            continue;
        }
        f->WriteString(content);
        f->Close();
    }
    return testSubDir;
}

}  // namespace

TEST(GoVfs, Glob) {
    registry reg;
    auto fss = newFss();
    string testDir;
    for (std::size_t i = 0; i < fss.size(); i++) {
        if (i == 0) {
            testDir = reg.testDir(fss[i]);
        } else {
            reg.add(fss[i], testDir);
        }
        setupGlobFiles(*fss[i], testDir);
    }

    std::pair<string, string> globTests[] = {
        {testDir + "/globs/bobs/matcher", testDir + "/globs/bobs/matcher"},
        {testDir + "/globs/*/mat?her", testDir + "/globs/bobs/matcher"},
        {testDir + "/globs/bobs/../*", testDir + "/globs/submatcher"},
        {testDir + "/match", testDir + "/match"},
    };
    for (auto& fs : fss) {
        for (auto& [pattern, result] : globTests) {
            auto [matches, err] = vfs::Glob(*fs, pattern);
            if (err) {
                ADD_FAILURE() << "Glob error for " << pattern << ": " << err;
                continue;
            }
            EXPECT_TRUE(contains(matches, result)) << fs->Name() << ": Glob(" << pattern << ") want " << result;
        }
        for (std::string_view pattern : {"no_match", "../*/no_match"}) {
            auto [matches, err] = vfs::Glob(*fs, pattern);
            EXPECT_FALSE(err) << "Glob error for " << pattern << ": " << err;
            EXPECT_TRUE(matches.empty()) << fs->Name() << ": Glob(" << pattern << ") want []";
        }
    }
}

TEST(GoVfs, GlobSymlink) {
    registry reg;
    auto fs = vfs::NewOsFs();
    string testDir = reg.testDir(fs);
    setupGlobFiles(*fs, testDir);

    if (os::Symlink("target", filepath::Join(testDir, "symlink"))) GTEST_SKIP() << "no symbolic links";

    struct test {
        std::string_view path, dest;
        bool brokenLink;
    };
    test globSymlinkTests[] = {
        {"test1", "link1", false},
        {"test2", "link2", true},
    };
    for (auto& tt : globSymlinkTests) {
        string path = filepath::Join(testDir, tt.path);
        string dest = filepath::Join(testDir, tt.dest);
        auto [f, err] = fs->Create(path);
        ASSERT_FALSE(err) << err;
        ASSERT_FALSE(f->Close());
        ASSERT_FALSE(os::Symlink(path, dest));
        if (tt.brokenLink) fs->Remove(path);  // break the link
        auto [matches, gerr] = vfs::Glob(*fs, dest);
        EXPECT_FALSE(gerr) << "GlobSymlink error for " << dest << ": " << gerr;
        EXPECT_TRUE(contains(matches, dest)) << "Glob(" << dest << ") want " << dest;
    }
}

TEST(GoVfs, GlobError) {
    for (auto& fs : newFss()) {
        auto [_, err] = vfs::Glob(*fs, "[7]");
        EXPECT_FALSE(err) << fs->Name() << ": " << err;
    }
}

// ---------------------------------------------------------------------------
// lstater_test.go and symlink_test.go

namespace {

// The file systems of the link tests, over a temporary directory of the
// OsFs and memWorkDir of a MemoryFs, each with a file.
struct linkFss {
    explicit linkFss(std::string_view prefix, std::string_view memWorkDir) {
        auto [wd, _] = os::Getwd();
        oldwd = wd;
        auto [dir, err] = vfs::TempDir(*osFs, "", prefix);
        EXPECT_FALSE(err) << err;
        workDir = dir;
        overlayFs1 = make_ptr<vfs::CopyOnWriteFs>(osFs, memFs);
        overlayFs2 = make_ptr<vfs::CopyOnWriteFs>(memFs, osFs);
        overlayFsMemOnly = make_ptr<vfs::CopyOnWriteFs>(memFs, vfs::NewMemoryFs());
        basePathFs = make_ptr<vfs::BasePathFs>(osFs, workDir);
        basePathFsMem = make_ptr<vfs::BasePathFs>(memFs, memWorkDir);
        roFs = make_ptr<vfs::ReadOnlyFs>(osFs);
        roFsMem = make_ptr<vfs::ReadOnlyFs>(memFs);
        pathFileMem = filepath::Join(memWorkDir, "aferom.txt");
        osPath = filepath::Join(workDir, "afero.txt");
        vfs::WriteFile(*osFs, osPath, as_bytes("Hi, Afero!"), 0777);
        vfs::WriteFile(*memFs, pathFileMem, as_bytes("Hi, Afero!"), 0777);
    }
    ~linkFss() {
        os::Chdir(oldwd);
        osFs->RemoveAll(workDir);
    }

    string oldwd;
    ptr<vfs::Fs> osFs = vfs::NewOsFs();
    ptr<vfs::Fs> memFs = vfs::NewMemoryFs();
    string workDir;
    ptr<vfs::Fs> overlayFs1, overlayFs2, overlayFsMemOnly, basePathFs, basePathFsMem, roFs, roFsMem;
    string pathFileMem;
    string osPath;
};

}  // namespace

TEST(GoVfs, LstatIfPossible) {
    linkFss l("afero-lstate", "/lstate");
    ASSERT_FALSE(os::Chdir(l.workDir));
    ASSERT_FALSE(os::Symlink("afero.txt", "symafero.txt"));

    string pathFile = filepath::Join(l.workDir, "afero.txt");
    string pathSymlink = filepath::Join(l.workDir, "symafero.txt");

    auto checkLstat = [](vfs::Fs& fs, std::string_view name, bool shouldLstat) -> ptr<fs::FileInfo> {
        auto [statFile, isLstat, err] = dynamic_cast<vfs::Lstater&>(fs).LstatIfPossible(name);
        EXPECT_FALSE(err) << "Lstat check failed: " << err;
        EXPECT_EQ(isLstat, shouldLstat) << "Lstat status for " << name;
        return statFile;
    };

    auto testLstat = [&](vfs::Fs& fs, std::string_view pathFile, std::string_view pathSymlink) {
        SCOPED_TRACE(str(fs.Name()));
        bool shouldLstat = !pathSymlink.empty();
        auto statRegular = checkLstat(fs, pathFile, shouldLstat);
        auto statSymlink = checkLstat(fs, pathSymlink, shouldLstat);
        ASSERT_TRUE(statRegular && statSymlink) << "got nil FileInfo";

        bool symSym = (statSymlink->Mode() & fs::ModeSymlink) == fs::ModeSymlink;
        EXPECT_NE(symSym, pathSymlink.empty()) << "expected the FileInfo to describe the symlink";

        auto [_, _, err] = dynamic_cast<vfs::Lstater&>(fs).LstatIfPossible("this-should-not-exist.txt");
        EXPECT_TRUE(err && os::IsNotExist(err)) << "expected file to not exist, got " << err;
    };

    testLstat(*l.osFs, pathFile, pathSymlink);
    testLstat(*l.overlayFs1, pathFile, pathSymlink);
    testLstat(*l.overlayFs2, pathFile, pathSymlink);
    testLstat(*l.basePathFs, "afero.txt", "symafero.txt");
    testLstat(*l.overlayFsMemOnly, l.pathFileMem, "");
    testLstat(*l.basePathFsMem, "aferom.txt", "");
    testLstat(*l.roFs, pathFile, pathSymlink);
    testLstat(*l.roFsMem, l.pathFileMem, "");
}

TEST(GoVfs, SymlinkIfPossible) {
    linkFss l("afero-symlink", "/sym");

    // output: the error expected, or empty for success.
    auto testLink = [](vfs::Fs& fs, std::string_view source, std::string_view destination, std::string_view output) {
        SCOPED_TRACE(str(fs.Name()));
        string dir = filepath::Dir(destination);
        if (!dir.view().empty()) fs.MkdirAll(dir, 0777);

        error err = dynamic_cast<vfs::Linker&>(fs).SymlinkIfPossible(source, destination);
        if (!err && !output.empty()) {
            FAIL() << "Error creating symlink, succeeded when expecting error " << output;
        } else if (err && output.empty()) {
            FAIL() << "Error creating symlink, expected success, got " << err;
        } else if (err && !err.Error().ends_with(output)) {
            FAIL() << "Error creating symlink, expected error '" << output << "', instead got output '" << err << "'";
        } else if (output.empty()) {
            // Check the link with Lstat, where it can.
            if (auto* lst = dynamic_cast<vfs::Lstater*>(&fs)) {
                auto [_, ok, lerr] = lst->LstatIfPossible(destination);
                EXPECT_TRUE(ok) << "Error calling lstat on file after successful link: " << lerr;
            }
        }
    };

    std::string notSupported = str(vfs::ErrNoSymlink.Error());
    testLink(*l.osFs, l.osPath, filepath::Join(l.workDir, "os/link.txt"), "");
    testLink(*l.overlayFs1, l.osPath, filepath::Join(l.workDir, "overlay/link1.txt"), notSupported);
    testLink(*l.overlayFs2, l.pathFileMem, filepath::Join(l.workDir, "overlay2/link2.txt"), "");
    testLink(*l.overlayFsMemOnly, l.pathFileMem, filepath::Join("/sym", "overlay3/link.txt"), notSupported);
    testLink(*l.basePathFs, "afero.txt", "basepath/link.txt", "");
    testLink(*l.basePathFsMem, l.pathFileMem, "link/file.txt", notSupported);
    testLink(*l.roFs, l.osPath, filepath::Join(l.workDir, "ro/link.txt"), notSupported);
    testLink(*l.roFsMem, l.pathFileMem, filepath::Join("/sym", "ro/link.txt"), notSupported);
}

TEST(GoVfs, ReadlinkIfPossible) {
    linkFss l("afero-readlink", "/read");

    auto testRead = [](vfs::Fs& fs, std::string_view name, std::string_view output) {
        SCOPED_TRACE(str(fs.Name()));
        auto [_, err] = dynamic_cast<vfs::LinkReader&>(fs).ReadlinkIfPossible(name);
        if (err && output.empty()) {
            ADD_FAILURE() << "Error reading link, expected success, got error: " << err;
        } else if (!err && !output.empty()) {
            ADD_FAILURE() << "Error reading link, succeeded when expecting error: " << output;
        } else if (err && !err.Error().ends_with(output)) {
            ADD_FAILURE() << "Error reading link, expected error '" << output << "', instead received '" << err << "'";
        }
    };

    std::string notSupported = str(vfs::ErrNoReadlink.Error());
    string link = filepath::Join(l.workDir, "os/link.txt");
    l.osFs->MkdirAll(filepath::Dir(link), 0777);
    ASSERT_FALSE(dynamic_cast<vfs::Linker&>(*l.osFs).SymlinkIfPossible(l.osPath, link));

    testRead(*l.osFs, link, "");
    testRead(*l.overlayFs1, link, "");
    testRead(*l.overlayFs2, link, "");
    testRead(*l.overlayFsMemOnly, l.pathFileMem, notSupported);
    testRead(*l.basePathFs, "os/link.txt", "");
    testRead(*l.basePathFsMem, l.pathFileMem, notSupported);
    testRead(*l.roFs, link, "");
    testRead(*l.roFsMem, l.pathFileMem, notSupported);
}
