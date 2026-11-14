// Ported from Go 1.27.3's os tests: os_test.go, os_unix_test.go,
// read_test.go, path_test.go, tempfile_test.go, removeall_test.go,
// pipe_test.go, error_test.go and getwd_unix_test.go, with Go's test names.
//
// Left out: tests of what go++ doesn't have (processes, os.Root, DirFS,
// CopyFS, Hostname, the User*Dir functions, SyscallConn), of Go internals
// (the stat and testlog hooks, allocations, finalizers, nil *File
// methods: a ptr<File> can't be nil and called), and of other systems.
// Go's "." directory, its own source directory, is a temporary directory
// with the same file names.
#include "go_os_test_util.hpp"

using namespace os_test;

TEST(GoOs, Stat) {
    std::string path = sfdir + "/" + sfname;
    auto [dir, err] = os::Stat(path);
    ASSERT_FALSE(err) << "stat failed: " << err;
    EXPECT_EQ(dir->Name(), sfname);
    EXPECT_EQ(dir->Size(), fileSize(path));
}

TEST(GoOs, StatError) {
    InTempDir tmp;
    std::string path = "no-such-file";
    auto [fi, err] = os::Stat(path);
    ASSERT_TRUE(err) << "got nil, want error";
    EXPECT_FALSE(fi);
    EXPECT_TRUE(dynamic_cast<const os::PathError*>(err.get())) << "want *PathError";

    std::string link = "symlink";
    ASSERT_FALSE(os::Symlink(path, link));
    auto [fi2, err2] = os::Stat(link);
    ASSERT_TRUE(err2) << "got nil, want error";
    EXPECT_FALSE(fi2);
    EXPECT_TRUE(dynamic_cast<const os::PathError*>(err2.get())) << "want *PathError";
}

TEST(GoOs, StatSymlinkLoop) {
    InTempDir tmp;
    ASSERT_FALSE(os::Symlink("x", "y"));
    ASSERT_FALSE(os::Symlink("y", "x"));
    auto [_, err] = os::Stat("x");
    EXPECT_TRUE(dynamic_cast<const fs::PathError*>(err.get())) << "expected *PathError, got " << err;
}

TEST(GoOs, Fstat) {
    std::string path = sfdir + "/" + sfname;
    auto [file, err1] = os::Open(path);
    ASSERT_FALSE(err1) << "open failed: " << err1;
    auto [dir, err2] = file->Stat();
    ASSERT_FALSE(err2) << "fstat failed: " << err2;
    EXPECT_EQ(dir->Name(), sfname);
    EXPECT_EQ(dir->Size(), fileSize(path));
    file->Close();
}

TEST(GoOs, Lstat) {
    std::string path = sfdir + "/" + sfname;
    auto [dir, err] = os::Lstat(path);
    ASSERT_FALSE(err) << "lstat failed: " << err;
    EXPECT_EQ(dir->Name(), sfname);
    if ((dir->Mode() & os::ModeSymlink) == 0) {
        EXPECT_EQ(dir->Size(), fileSize(path));
    }
}

// Read with length 0 should not return EOF.
TEST(GoOs, Read0) {
    std::string path = sfdir + "/" + sfname;
    auto [f, err] = os::Open(path);
    ASSERT_FALSE(err) << "open failed: " << err;
    auto [n, e] = f->Read({});
    EXPECT_TRUE(n == 0 && !e) << "Read(0) = " << n << ", " << e << ", want 0, nil";
    byte buf[100];
    auto [n2, e2] = f->Read(buf);
    EXPECT_TRUE(n2 > 0 && !e2) << "Read(100) = " << n2 << ", " << e2 << ", want >0, nil";
    f->Close();
}

// Reading a closed file should return ErrClosed error.
TEST(GoOs, ReadClosed) {
    std::string path = sfdir + "/" + sfname;
    auto [file, err] = os::Open(path);
    ASSERT_FALSE(err) << "open failed: " << err;
    file->Close();  // close immediately
    byte buf[100];
    auto [_, rerr] = file->Read(buf);
    auto* e = dynamic_cast<const os::PathError*>(rerr.get());
    ASSERT_TRUE(e && e->Err == os::ErrClosed) << "Read: got " << rerr << ", want PathError(" << os::ErrClosed << ")";
}

namespace {

// Go's "." with its files, in a temporary directory.
struct DotDir {
    TempDir dir;
    DotDir() {
        for (auto& n : dot) touch(join(dir.path(), n));
    }
};

template <typename Entry>
void checkContents(std::string_view dir, const std::vector<Entry>& s, const std::vector<std::string>& contents,
                    auto name_of) {
    for (auto& m : contents) {
        bool found = false;
        for (auto& n : s) {
            std::string name = name_of(n);
            if (name == "." || name == "..") ADD_FAILURE() << "got " << name << " in directory";
            if (m != name) continue;
            if (found) ADD_FAILURE() << "present twice: " << m;
            found = true;
        }
        if (!found) ADD_FAILURE() << "could not find " << m << " in " << dir;
    }
}

void testReaddirnames(std::string_view dir, const std::vector<std::string>& contents) {
    auto [file, err] = os::Open(dir);
    ASSERT_FALSE(err) << "open " << dir << " failed: " << err;
    auto [s, err2] = file->Readdirnames(-1);
    ASSERT_FALSE(err2) << "Readdirnames " << dir << " failed: " << err2;
    checkContents(dir, s, contents, [](const string& n) { return str(n); });
    file->Close();
}

void testReaddir(std::string_view dir, const std::vector<std::string>& contents) {
    auto [file, err] = os::Open(dir);
    ASSERT_FALSE(err) << "open " << dir << " failed: " << err;
    auto [s, err2] = file->Readdir(-1);
    ASSERT_FALSE(err2) << "Readdir " << dir << " failed: " << err2;
    checkContents(dir, s, contents, [](const ptr<FileInfo>& n) { return str(n->Name()); });
    file->Close();
}

void testReadDir(std::string_view dir, const std::vector<std::string>& contents) {
    auto [file, err] = os::Open(dir);
    ASSERT_FALSE(err) << "open " << dir << " failed: " << err;
    auto [s, err2] = file->ReadDir(-1);
    ASSERT_FALSE(err2) << "ReadDir " << dir << " failed: " << err2;
    checkContents(dir, s, contents, [](const ptr<os::DirEntry>& n) { return str(n->Name()); });
    for (auto& m : contents) {
        for (auto& n : s) {
            if (n->Name() != m) continue;
            auto [lstat, lerr] = os::Lstat(join(dir, m));
            ASSERT_FALSE(lerr) << lerr;
            EXPECT_EQ(n->IsDir(), lstat->IsDir()) << m;
            EXPECT_EQ(uint32(n->Type()), uint32(lstat->Mode().Type())) << m;
            auto [info, ierr] = n->Info();
            if (ierr) {
                ADD_FAILURE() << m << ": Info: " << ierr;
                continue;
            }
            EXPECT_TRUE(os::SameFile(info, lstat)) << m << ": Info: SameFile(info, lstat) = false";
        }
    }
    file->Close();
}

}  // namespace

TEST(GoOs, FileReaddirnames) {
    DotDir d;
    testReaddirnames(d.dir, dot);
    testReaddirnames(sfdir, sysdir_files);
    TempDir empty;
    testReaddirnames(empty, {});
}

TEST(GoOs, FileReaddir) {
    DotDir d;
    testReaddir(d.dir, dot);
    testReaddir(sfdir, sysdir_files);
    TempDir empty;
    testReaddir(empty, {});
}

TEST(GoOs, FileReadDir) {
    DotDir d;
    testReadDir(d.dir, dot);
    testReadDir(sfdir, sysdir_files);
    TempDir empty;
    testReadDir(empty, {});
}

// Check that reading a directory one entry at a time gives the same result
// as reading it all at once.
TEST(GoOs, ReaddirnamesOneAtATime) {
    std::string dir = "/usr/bin";
    auto [file, err] = os::Open(dir);
    ASSERT_FALSE(err) << "open " << dir << " failed: " << err;
    auto [all, err1] = file->Readdirnames(-1);
    ASSERT_FALSE(err1) << "readdirnames " << dir << " failed: " << err1;
    auto [file1, err2] = os::Open(dir);
    ASSERT_FALSE(err2) << "open " << dir << " failed: " << err2;
    std::vector<string> small;
    for (;;) {
        auto [d, e] = file1->Readdirnames(1);
        if (e == io::EOF_) break;
        ASSERT_FALSE(e) << "readdirnames " << dir << " failed: " << e;
        ASSERT_FALSE(d.empty()) << "readdirnames returned empty slice and no error";
        small.push_back(d[0]);
        ASSERT_LE(small.size(), all.size() + 100);
    }
    ASSERT_GE(small.size(), all.size());
    for (std::size_t i = 0; i < all.size(); i++) EXPECT_EQ(small[i], all[i]) << "small read mismatch";
    file->Close();
    file1->Close();
}

TEST(GoOs, ReaddirNValues) {
    TempDir dir;
    for (int i = 1; i <= 105; i++) {
        auto [f, err] = os::Create(join(dir, std::to_string(i)));
        ASSERT_FALSE(err) << "Create: " << err;
        f->WriteString(std::string(static_cast<std::size_t>(i), 'X'));
        f->Close();
    }

    ptr<File> d;
    auto open_dir = [&] {
        auto [f, err] = os::Open(dir.path());
        ASSERT_FALSE(err) << "Open directory: " << err;
        d = f;
    };
    auto readdir_expect = [&](int n, std::size_t want, error want_err) {
        auto [fi, err] = d->Readdir(n);
        ASSERT_EQ(err, want_err) << "Readdir of " << n << " got error " << err << ", want " << want_err;
        EXPECT_EQ(fi.size(), want) << "Readdir of " << n;
    };
    auto read_dir_expect = [&](int n, std::size_t want, error want_err) {
        auto [de, err] = d->ReadDir(n);
        ASSERT_EQ(err, want_err) << "ReadDir of " << n << " got error " << err << ", want " << want_err;
        EXPECT_EQ(de.size(), want) << "ReadDir of " << n;
    };
    auto readdirnames_expect = [&](int n, std::size_t want, error want_err) {
        auto [fi, err] = d->Readdirnames(n);
        ASSERT_EQ(err, want_err) << "Readdirnames of " << n << " got error " << err << ", want " << want_err;
        EXPECT_EQ(fi.size(), want) << "Readdirnames of " << n;
    };

    std::vector<std::function<void(int, std::size_t, error)>> fns = {readdir_expect, readdirnames_expect,
                                                                      read_dir_expect};
    for (auto& fn : fns) {
        // Test the slurp case
        open_dir();
        fn(0, 105, nullptr);
        fn(0, 0, nullptr);
        d->Close();

        // Slurp with -1 instead
        open_dir();
        fn(-1, 105, nullptr);
        fn(-2, 0, nullptr);
        fn(0, 0, nullptr);
        d->Close();

        // Test the bounded case
        open_dir();
        fn(1, 1, nullptr);
        fn(2, 2, nullptr);
        fn(105, 102, nullptr);  // and tests buffer >100 case
        fn(3, 0, io::EOF_);
        d->Close();
    }
}

TEST(GoOs, ReaddirOfFile) {
    TempDir tmp;
    auto [f, err] = os::CreateTemp(tmp, "_Go_ReaddirOfFile");
    ASSERT_FALSE(err) << err;
    f->WriteString("foo");
    f->Close();
    auto [reg, rerr] = os::Open(f->Name().view());
    ASSERT_FALSE(rerr) << rerr;
    auto [names, nerr] = reg->Readdirnames(-1);
    EXPECT_TRUE(nerr) << "Readdirnames succeeded; want non-nil error";
    auto* pe = errors::As<os::PathError>(nerr);
    EXPECT_TRUE(pe && pe->Path == f->Name()) << "Readdirnames returned " << nerr << "; want a PathError with path "
                                            << f->Name();
    EXPECT_TRUE(names.empty()) << "unexpected dir names in regular file";
    reg->Close();
}

TEST(GoOs, HardLink) {
    InTempDir tmp;
    std::string from = "hardlinktestfrom", to = "hardlinktestto";
    auto [file, err] = os::Create(to);
    ASSERT_FALSE(err) << "open " << to << " failed: " << err;
    EXPECT_FALSE(file->Close());
    ASSERT_FALSE(os::Link(to, from)) << "link failed";

    std::string none = "hardlinktestnone";
    error lerr = os::Link(none, none);
    auto* le = dynamic_cast<const os::LinkError*>(lerr.get());
    EXPECT_TRUE(le && !le->Error().empty()) << "link " << none << " failed to return a valid error";

    auto [tostat, e1] = os::Stat(to);
    ASSERT_FALSE(e1) << e1;
    auto [fromstat, e2] = os::Stat(from);
    ASSERT_FALSE(e2) << e2;
    EXPECT_TRUE(os::SameFile(tostat, fromstat)) << "link did not create hard link";

    // We should not be able to perform the same Link() a second time
    error err3 = os::Link(to, from);
    auto* le3 = dynamic_cast<const os::LinkError*>(err3.get());
    ASSERT_TRUE(le3) << "link: expected *LinkError, got " << err3;
    EXPECT_EQ(le3->Op, "link");
    EXPECT_EQ(le3->Old, to);
    EXPECT_EQ(le3->New, from);
    EXPECT_TRUE(os::IsExist(le3->Err)) << "err.Err = " << le3->Err << "; want file exists error";
}

TEST(GoOs, Symlink) {
    InTempDir tmp;
    std::string from = "symlinktestfrom", to = "symlinktestto";
    auto [file, err] = os::Create(to);
    ASSERT_FALSE(err) << "Create failed: " << err;
    EXPECT_FALSE(file->Close());
    ASSERT_FALSE(os::Symlink(to, from));
    auto [tostat, e1] = os::Lstat(to);
    ASSERT_FALSE(e1) << e1;
    ASSERT_EQ(tostat->Mode() & os::ModeSymlink, 0u);
    auto [fromstat, e2] = os::Stat(from);
    ASSERT_FALSE(e2) << e2;
    EXPECT_TRUE(os::SameFile(tostat, fromstat)) << "Symlink did not create symlink";
    auto [fromlstat, e3] = os::Lstat(from);
    ASSERT_FALSE(e3) << e3;
    ASSERT_NE(fromlstat->Mode() & os::ModeSymlink, 0u);
    auto [fromstat2, e4] = os::Stat(from);
    ASSERT_FALSE(e4) << e4;
    EXPECT_EQ(fromstat2->Name(), from);
    ASSERT_EQ(fromstat2->Mode() & os::ModeSymlink, 0u);
    auto [s, e5] = os::Readlink(from);
    ASSERT_FALSE(e5) << e5;
    ASSERT_EQ(s, to);
    auto [file2, e6] = os::Open(from);
    ASSERT_FALSE(e6) << e6;
    file2->Close();
}

TEST(GoOs, LongSymlink) {
    InTempDir tmp;
    std::string s = "0123456789abcdef";
    // Long, but not too long: a common limit is 255.
    s = s + s + s + s + s + s + s + s + s + s + s + s + s + s + s;
    std::string from = "longsymlinktestfrom";
    ASSERT_FALSE(os::Symlink(s, from));
    auto [r, err] = os::Readlink(from);
    ASSERT_FALSE(err) << err;
    ASSERT_EQ(r, s);
}

TEST(GoOs, Rename) {
    InTempDir tmp;
    std::string from = "renamefrom", to = "renameto";
    auto [file, err] = os::Create(from);
    ASSERT_FALSE(err) << err;
    EXPECT_FALSE(file->Close());
    ASSERT_FALSE(os::Rename(from, to));
    auto [_, serr] = os::Stat(to);
    EXPECT_FALSE(serr) << serr;
}

TEST(GoOs, RenameOverwriteDest) {
    InTempDir tmp;
    std::string from = "renamefrom", to = "renameto";
    std::string to_data = "to", from_data = "from";
    ASSERT_FALSE(os::WriteFile(to, to_data, 0777));
    ASSERT_FALSE(os::WriteFile(from, from_data, 0777));
    ASSERT_FALSE(os::Rename(from, to));
    auto [_, err] = os::Stat(from);
    EXPECT_TRUE(err) << "from file still exists";
    if (err) {
        ASSERT_TRUE(os::IsNotExist(err)) << "stat from: " << err;
    }
    auto [to_fi, terr] = os::Stat(to);
    ASSERT_FALSE(terr) << terr;
    EXPECT_EQ(to_fi->Size(), static_cast<int64>(from_data.size())) << "\"to\" size isn't the old \"from\" size";
}

TEST(GoOs, RenameFailed) {
    InTempDir tmp;
    std::string from = "renamefrom", to = "renameto";
    error err = os::Rename(from, to);
    auto* le = dynamic_cast<const os::LinkError*>(err.get());
    ASSERT_TRUE(le) << "rename: expected *LinkError, got " << err;
    EXPECT_EQ(le->Op, "rename");
    EXPECT_EQ(le->Old, from);
    EXPECT_EQ(le->New, to);
}

TEST(GoOs, RenameNotExisting) {
    InTempDir tmp;
    std::string from = "doesnt-exist", to = "dest";
    os::Mkdir(to, 0777);
    error err = os::Rename(from, to);
    EXPECT_TRUE(os::IsNotExist(err)) << "Rename = " << err << "; want an IsNotExist error";
}

TEST(GoOs, RenameToDirFailed) {
    InTempDir tmp;
    std::string from = "renamefrom", to = "renameto";
    os::Mkdir(from, 0777);
    os::Mkdir(to, 0777);
    error err = os::Rename(from, to);
    auto* le = dynamic_cast<const os::LinkError*>(err.get());
    ASSERT_TRUE(le) << "rename: expected *LinkError, got " << err;
    EXPECT_EQ(le->Op, "rename");
    EXPECT_EQ(le->Old, from);
    EXPECT_EQ(le->New, to);
}

TEST(GoOs, StatRelativeSymlink) {
    TempDir tmpdir;
    std::string target = join(tmpdir, "target");
    auto [f, err] = os::Create(target);
    ASSERT_FALSE(err) << err;
    auto [st, serr] = f->Stat();
    ASSERT_FALSE(serr) << serr;
    std::string link = join(tmpdir, "link");
    ASSERT_FALSE(os::Symlink("target", link));
    auto [st1, serr1] = os::Stat(link);
    ASSERT_FALSE(serr1) << serr1;
    EXPECT_TRUE(os::SameFile(st, st1)) << "Stat doesn't follow relative symlink";
    f->Close();
}

TEST(GoOs, ReadAtEOF) {
    NewFile f;
    byte buf[10];
    auto [_, err] = f->ReadAt(buf, 0);
    ASSERT_TRUE(err) << "ReadAt succeeded";
    ASSERT_EQ(err, io::EOF_) << "ReadAt failed: " << err;
}

TEST(GoOs, LongPath) {
    TempDir tmp;
    std::string tmpdir = tmp.path();
    // Test the boundary of 247 and fewer bytes (normal) and 248 and more
    // bytes (adjusted).
    int sizes[] = {247, 248, 249, 400};
    while (tmpdir.size() < 400) tmpdir += "/dir3456789";
    for (int sz : sizes) {
        SCOPED_TRACE(testing::Message() << "length=" << sz);
        std::string sized = tmpdir.substr(0, static_cast<std::size_t>(sz - 1)) + "x";  // Ensure it does not end with a slash.
        // The various sized runs are for this call to trigger the boundary
        // condition.
        ASSERT_FALSE(os::MkdirAll(sized, 0755)) << "MkdirAll failed";
        const std::string data = "hello world\n";
        ASSERT_FALSE(os::WriteFile(sized + "/foo.txt", data, 0644));
        ASSERT_FALSE(os::Rename(sized + "/foo.txt", sized + "/bar.txt"));
        time::Time mtime = time::Time::Unix(time::Now().Unix() / 60 * 60, 0);
        ASSERT_FALSE(os::Chtimes(sized + "/bar.txt", mtime, mtime));
        std::vector<std::string> names = {"bar.txt"};
        ASSERT_FALSE(os::Symlink(sized + "/bar.txt", sized + "/symlink.txt"));
        names.push_back("symlink.txt");
        ASSERT_FALSE(os::Link(sized + "/bar.txt", sized + "/link.txt"));
        names.push_back("link.txt");
        for (int64 want_size : {static_cast<int64>(data.size()), int64(0)}) {
            for (auto& name : names) {
                std::string path = sized + "/" + name;
                auto [dir, err] = os::Stat(path);
                ASSERT_FALSE(err) << "Stat(" << path << ") failed: " << err;
                int64 filesize = fileSize(path);
                EXPECT_TRUE(dir->Size() == filesize && filesize == want_size)
                    << "Size(" << path << ") is " << dir->Size() << ", len(ReadFile()) is " << filesize << ", want "
                    << want_size;
                ASSERT_FALSE(os::Chmod(path, dir->Mode())) << "Chmod(" << path << ") failed";
            }
            ASSERT_FALSE(os::Truncate(sized + "/bar.txt", 0));
        }
    }
}

namespace {

void mkdirTree(const std::string& root, int level, int max) {
    if (level >= max) return;
    level++;
    for (char i = 'a'; i < 'c'; i++) {
        std::string dir = join(root, std::string(1, i));
        ASSERT_FALSE(os::Mkdir(dir, 0700));
        mkdirTree(dir, level, max);
    }
}

}  // namespace

// Test that simultaneous RemoveAll do not report an error.
// As long as it gets removed, we should be happy.
TEST(GoOs, RemoveAllRace) {
    TempDir tmp;
    std::string root = tmp.path();
    mkdirTree(root, 1, 6);
    auto hold = makeChan<empty>();
    sync::WaitGroup wg;
    for (int i = 0; i < 4; i++) {
        wg.Add(1);
        go([&] {
            hold.recv();
            error err = os::RemoveAll(root);
            EXPECT_FALSE(err) << "unexpected error: " << err;
            wg.Done();
        });
    }
    hold.close();  // let workers race to remove root
    wg.Wait();
}

// Test that reading from a pipe doesn't use up a thread.
TEST(GoOs, PipeThreads) {
    constexpr int threads = 100;
    std::vector<ptr<File>> r(threads), w(threads);
    for (int i = 0; i < threads; i++) {
        auto [rp, wp, err] = os::Pipe();
        ASSERT_FALSE(err) << err;
        r[static_cast<std::size_t>(i)] = rp;
        w[static_cast<std::size_t>(i)] = wp;
    }
    auto creading = makeChan<bool>(threads);
    auto cdone = makeChan<bool>(threads);
    for (std::size_t i = 0; i < threads; i++) {
        go([&, i] {
            byte buf[1];
            creading.send(true);
            auto [_, err] = r[i]->Read(buf);
            EXPECT_FALSE(err) << err;
            EXPECT_FALSE(r[i]->Close());
            cdone.send(true);
        });
    }
    for (int i = 0; i < threads; i++) creading.recv();
    // If we are still alive, it means that the 100 goroutines did not
    // require 100 threads.
    for (std::size_t i = 0; i < threads; i++) {
        byte one[1] = {0};
        auto [_, err] = w[i]->Write(one);
        EXPECT_FALSE(err) << err;
        EXPECT_FALSE(w[i]->Close());
        cdone.recv();
    }
}

namespace {

void testDoubleCloseError(const std::string& path) {
    SCOPED_TRACE(path);
    auto [file, err] = os::Open(path);
    ASSERT_FALSE(err) << err;
    ASSERT_FALSE(file->Close()) << "unexpected error from Close";
    error cerr = file->Close();
    ASSERT_TRUE(cerr) << "second Close did not fail";
    auto* pe = dynamic_cast<const os::PathError*>(cerr.get());
    ASSERT_TRUE(pe) << "second Close: got " << cerr << ", want *PathError";
    EXPECT_EQ(pe->Err, os::ErrClosed) << "second Close: got " << pe->Err << ", want " << os::ErrClosed;
}

}  // namespace

TEST(GoOs, DoubleCloseError) {
    testDoubleCloseError(sfdir + "/" + sfname);
    testDoubleCloseError(sfdir);
}

TEST(GoOs, DirSeek) {
    DotDir d;
    auto [f, err] = os::Open(d.dir);
    ASSERT_FALSE(err) << err;
    auto [dirnames1, e1] = f->Readdirnames(0);
    ASSERT_FALSE(e1) << e1;
    auto [ret, e2] = f->Seek(0, 0);
    ASSERT_FALSE(e2) << e2;
    ASSERT_EQ(ret, 0) << "seek result not zero";
    auto [dirnames2, e3] = f->Readdirnames(0);
    ASSERT_FALSE(e3) << e3;
    ASSERT_EQ(dirnames1.size(), dirnames2.size()) << "listings have different lengths";
    for (std::size_t i = 0; i < dirnames1.size(); i++) {
        ASSERT_EQ(dirnames1[i], dirnames2[i]) << "different name i=" << i;
    }
    f->Close();
}

// Test that making a tiny Readdirnames call and then seeking back to the
// start doesn't lose entries (Go's testdata/issue37161 has 3 files).
TEST(GoOs, ReaddirSmallSeek) {
    TempDir dir;
    for (auto n : {"a", "b", "c"}) touch(join(dir, n));
    auto [df, err] = os::Open(dir);
    ASSERT_FALSE(err) << err;
    auto [names1, e1] = df->Readdirnames(1);
    ASSERT_FALSE(e1) << e1;
    auto [_, e2] = df->Seek(0, 0);
    ASSERT_FALSE(e2) << e2;
    auto [names2, e3] = df->Readdirnames(0);
    ASSERT_FALSE(e3) << e3;
    ASSERT_EQ(names2.size(), 3u);
    df->Close();
}

// Issue 39390: OpenFile with O_TRUNC shouldn't remove write permissions.
TEST(GoOs, OpenFileKeepsPermissions) {
    TempDir dir;
    std::string name = join(dir, "x");
    touch(name);
    auto [f, err] = os::OpenFile(name, os::O_WRONLY_ | os::O_CREATE_ | os::O_TRUNC_, 0);
    ASSERT_FALSE(err) << err;
    if (auto [fi, serr] = f->Stat(); serr) {
        ADD_FAILURE() << serr;
    } else {
        EXPECT_NE(fi->Mode() & 0222, 0u) << "f.Stat.Mode after OpenFile is " << fi->Mode() << ", should be writable";
    }
    EXPECT_FALSE(f->Close());
    if (auto [fi, serr] = os::Stat(name); serr) {
        ADD_FAILURE() << serr;
    } else {
        EXPECT_NE(fi->Mode() & 0222, 0u) << "Stat after OpenFile is " << fi->Mode() << ", should be writable";
    }
}

TEST(GoOs, ReadFileProc) {
    // Linux files in /proc report 0 size, but then if ReadFile reads just
    // a single byte at offset 0, the read at offset 1 returns EOF instead
    // of the next byte. Issue 72080.
    const std::string name = "/proc/sys/fs/pipe-max-size";
    if (auto [_, err] = os::Stat(name); err) GTEST_SKIP() << err;
    std::string data = read_file(name);
    ASSERT_TRUE(!data.empty() && data.back() == '\n') << "read " << name << ": not newline-terminated: " << data;
}

TEST(GoOs, FileIOCloseRace) {
    TempDir tmp;
    auto [file, err] = os::Create(join(tmp, "test.txt"));
    ASSERT_FALSE(err) << err;
    sync::WaitGroup wg;
    wg.Go([&] {
        byte tmpb[100] = {};
        auto [_, werr] = file->Write(tmpb);
        if (werr && !errors::Is(werr, os::ErrClosed)) ADD_FAILURE() << werr;
    });
    wg.Go([&] {
        byte tmpb[100];
        auto [_, rerr] = file->Read(tmpb);
        if (rerr && rerr != io::EOF_ && !errors::Is(rerr, os::ErrClosed)) ADD_FAILURE() << rerr;
    });
    wg.Go([&] { EXPECT_FALSE(file->Close()); });
    wg.Wait();
}

TEST(GoOs, PipeIOCloseRace) {
    auto [r, w, err] = os::Pipe();
    ASSERT_FALSE(err) << err;
    sync::WaitGroup wg;
    wg.Add(3);
    go([&] {
        for (;;) {
            auto [n, werr] = w->Write(b("hi"));
            if (werr) {
                if (!errors::Is(werr, os::ErrClosed) && !strings::Contains(werr.Error(), "broken pipe")) {
                    ADD_FAILURE() << werr;
                }
                break;
            }
            if (n != 2) {
                ADD_FAILURE() << "wrote " << n << " bytes, expected 2";
                break;
            }
        }
        wg.Done();
    });
    go([&] {
        for (;;) {
            byte buf[2];
            auto [n, rerr] = r->Read(buf);
            if (rerr) {
                if (rerr != io::EOF_ && !errors::Is(rerr, os::ErrClosed)) ADD_FAILURE() << rerr;
                break;
            }
            if (n != 2) ADD_FAILURE() << "read " << n << " bytes, want 2";
        }
        wg.Done();
    });
    go([&] {
        time::Sleep(time::Millisecond);
        EXPECT_FALSE(r->Close());
        EXPECT_FALSE(w->Close());
        wg.Done();
    });
    wg.Wait();
}

TEST(GoOs, PipeCloseRace) {
    auto [r, w, err] = os::Pipe();
    ASSERT_FALSE(err) << err;
    sync::WaitGroup wg;
    auto c = makeChan<error>(4);
    auto f = [&] {
        c.send(r->Close());
        c.send(w->Close());
        wg.Done();
    };
    wg.Add(2);
    go(f);
    go(f);
    int nils = 0, errs = 0;
    for (int i = 0; i < 4; i++) {
        if (c.recv()) {
            errs++;
        } else {
            nils++;
        }
    }
    EXPECT_TRUE(nils == 2 && errs == 2) << "got nils " << nils << " errs " << errs << ", want 2 2";
    wg.Wait();
}

TEST(GoOs, RandomLen) {
    for (int i = 0; i < 5; i++) {
        TempDir tmp;
        auto [dir, err] = os::MkdirTemp(tmp, "*");
        ASSERT_FALSE(err) << err;
        std::string base = str(dir).substr(str(dir).rfind('/') + 1);
        EXPECT_LE(base.size(), 10u) << "MkdirTemp returned " << base;
    }
    for (int i = 0; i < 5; i++) {
        TempDir tmp;
        auto [f, err] = os::CreateTemp(tmp, "*");
        ASSERT_FALSE(err) << err;
        std::string base = str(f->Name()).substr(str(f->Name()).rfind('/') + 1);
        f->Close();
        EXPECT_LE(base.size(), 10u) << "CreateTemp returned " << base;
    }
}

TEST(GoOs, AppendDoesntOverwrite) {
    InTempDir tmp;
    std::string name = "file";
    ASSERT_FALSE(os::WriteFile(name, "hello", 0666));
    auto [f, err] = os::OpenFile(name, os::O_APPEND_ | os::O_WRONLY_, 0);
    ASSERT_FALSE(err) << err;
    auto [_, werr] = f->Write(b(" world"));
    if (werr) {
        f->Close();
        FAIL() << werr;
    }
    ASSERT_FALSE(f->Close());
    ASSERT_EQ(read_file(name), "hello world");
}

TEST(GoOs, RemoveReadOnlyFile) {
    InTempDir tmp;
    ASSERT_FALSE(os::WriteFile("file", "1", 0));
    ASSERT_FALSE(os::Remove("file")) << "Remove read-only file";
    auto [_, err] = os::Stat("file");
    ASSERT_TRUE(os::IsNotExist(err)) << "Stat read-only file after removal: " << err << " (want IsNotExist)";
}

TEST(GoOs, OpenFileDevNull) {
    // See https://go.dev/issue/71752.
    auto [f, err] = os::OpenFile(os::DevNull, os::O_WRONLY_ | os::O_CREATE_ | os::O_TRUNC_, 0644);
    ASSERT_FALSE(err) << "OpenFile(DevNull): " << err;
    f->Close();
}

TEST(GoOs, ReadFileContents) {
    struct read_step {
        std::size_t buf_size;  // non-zero to check length of buf to Read
        isize ret_n;           // result of Read call
        error ret_err;         // error result of Read call
    };
    error err_foo = errors::New("foo");
    struct test {
        std::string name;
        int64 stat_size;      // size of file to read, per stat (may be 0 for /proc files)
        std::size_t want_size;  // wanted length of []byte from readFileContents
        error want_err;       // wanted error from readFileContents
        std::vector<read_step> reads;
    } tests[] = {
        {"big-file", 2000, 2000, nullptr, {{2001, 21, nullptr}, {1980, 1979, io::EOF_}}},
        {"small-file", 100, 100, nullptr, {{512, 100, io::EOF_}}},
        {"returning-error", 1000, 50, err_foo, {{1001, 25, nullptr}, {0, 25, err_foo}}},
        {"proc-file", 0, 1023, nullptr, {{512, 512, nullptr}, {0, 511, io::EOF_}}},
        {"plan9-iproute-file", 0, 1032, nullptr, {{512, 511, nullptr}, {0, 511, nullptr}, {0, 10, io::EOF_}}},  // Issue 72080
    };
    for (auto& tt : tests) {
        SCOPED_TRACE(tt.name);
        std::size_t next = 0;
        bool bad = false;
        auto [got, err] = os::detail::read_file_contents(tt.stat_size, [&](std::span<byte> buf) -> std::pair<isize, error> {
            if (bad || next == tt.reads.size()) {
                bad = true;
                return {0, errors::New("unexpected read")};
            }
            if (tt.stat_size == 0 && buf.size() < 512) {
                ADD_FAILURE() << "read[" << next << "] with buf size " << buf.size()
                              << "; want at least 512 for 0-sized file";
            }
            const read_step& step = tt.reads[next++];
            if (step.buf_size != 0 && buf.size() != step.buf_size) {
                ADD_FAILURE() << "read[" << next - 1 << "] has buffer size " << buf.size() << "; want " << step.buf_size;
            }
            return {step.ret_n, step.ret_err};
        });
        EXPECT_FALSE(bad) << "unexpected read after " << tt.reads.size() << " expected reads";
        EXPECT_EQ(next, tt.reads.size()) << "expected " << tt.reads.size() << " reads, got " << next;
        EXPECT_EQ(err.Error(), tt.want_err.Error());
        EXPECT_EQ(got.size(), tt.want_size);
    }
}
