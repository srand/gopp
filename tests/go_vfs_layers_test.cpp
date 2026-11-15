// Ported from afero v1.15.0's tests of its layers: composite_test.go,
// copyOnWriteFs_test.go, basepath_test.go, ro_regexp_test.go and
// iofs_test.go, with afero's test names; and a test of HttpFs, which afero
// has none of.
//
// Adapted: TestIOFSNativeDirEntryWhenPossible tells the DirEntries that
// fs::FileInfoToDirEntry makes by their type, where afero names the type.
// BasePathOutsideSibling is new: afero's BasePathFs let "../" reach a
// sibling whose name begins with the base's.
#include "go_vfs_test_util.hpp"

#include <gopp/net/http.hpp>
#include <gopp/net/http/httptest.hpp>
#include <gopp/regexp.hpp>
#include <gopp/strings.hpp>
#include <gopp/testing/fstest.hpp>
#include <gopp/time.hpp>
#include <gopp/vfs/http.hpp>

#include <algorithm>
#include <random>
#include <typeinfo>

using namespace vfs_test;

namespace {

ptr<vfs::Fs> readOnly(ptr<vfs::Fs> fs) { return make_ptr<vfs::ReadOnlyFs>(std::move(fs)); }
ptr<vfs::Fs> copyOnWrite(ptr<vfs::Fs> base, ptr<vfs::Fs> layer) {
    return make_ptr<vfs::CopyOnWriteFs>(std::move(base), std::move(layer));
}

void write(vfs::Fs& fs, std::string_view name, std::string_view content) {
    auto [fh, err] = fs.Create(name);
    ASSERT_FALSE(err) << name << ": " << err;
    fh->WriteString(content);
    fh->Close();
}

std::vector<string> readdirnames(vfs::Fs& fs, std::string_view name) {
    auto [fh, err] = fs.Open(name);
    EXPECT_FALSE(err) << err;
    if (err) return {};
    auto [files, rerr] = fh->Readdirnames(-1);
    EXPECT_FALSE(rerr) << "Readdirnames failed: " << rerr;
    fh->Close();
    return files;
}

std::size_t readdirCount(vfs::Fs& fs, std::string_view name) {
    auto [fh, err] = fs.Open(name);
    EXPECT_FALSE(err) << err;
    if (err) return 0;
    auto [list, rerr] = fh->Readdir(-1);
    EXPECT_FALSE(rerr) << "Readdir failed " << rerr;
    fh->Close();
    return list.size();
}

// afero's NewTempOsBaseFs and CleanupTempDirs: a BasePathFs of a new
// temporary directory of the OsFs, removed at the end.
struct tempOsBaseFss {
    ~tempOsBaseFss() {
        for (auto& x : tempDirs) EXPECT_FALSE(os::RemoveAll(x)) << "error removing tempDir " << x;
    }
    ptr<vfs::Fs> make() {
        auto [name, err] = vfs::TempDir(*vfs::NewOsFs(), "", "");
        EXPECT_FALSE(err) << "error creating tempDir " << err;
        tempDirs.push_back(name);
        return vfs::NewBasePathFs(vfs::NewOsFs(), name);
    }
    std::vector<string> tempDirs;
};

}  // namespace

// ---------------------------------------------------------------------------
// composite_test.go

TEST(GoVfsLayers, UnionCreateExisting) {
    auto base = vfs::NewMemoryFs();
    auto ufs = vfs::NewCopyOnWriteFs(readOnly(base), vfs::NewMemoryFs());

    base->MkdirAll("/home/test", 0777);
    write(*base, "/home/test/file.txt", "This is a test");

    auto [fh, err] = ufs->OpenFile("/home/test/file.txt", os::O_RDWR_, 0666);
    ASSERT_FALSE(err) << "Failed to open file r/w: " << err;
    EXPECT_FALSE(fh->Write(as_bytes("####")).second) << "Failed to write file";
    fh->Seek(0, 0);
    auto [data, rerr] = io::ReadAll(*fh);
    EXPECT_FALSE(rerr) << "Failed to read file: " << rerr;
    EXPECT_EQ(str(data), "#### is a test") << "Got wrong data";
    fh->Close();

    std::tie(fh, err) = base->Open("/home/test/file.txt");
    std::tie(data, rerr) = io::ReadAll(*fh);
    EXPECT_EQ(str(data), "This is a test") << "Got wrong data in base file";
    fh->Close();

    std::tie(fh, err) = ufs->Create("/home/test/file.txt");
    ASSERT_FALSE(err) << "Create failed on existing file";
    auto [fi, _] = fh->Stat();
    EXPECT_EQ(fi->Size(), 0) << "Create did not truncate file";
    fh->Close();
}

TEST(GoVfsLayers, UnionMergeReaddir) {
    auto base = vfs::NewMemoryFs();
    auto ufs = copyOnWrite(readOnly(base), vfs::NewMemoryFs());
    base->MkdirAll("/home/test", 0777);
    write(*base, "/home/test/file.txt", "This is a test");
    write(*ufs, "/home/test/file2.txt", "This is a test");
    EXPECT_EQ(readdirnames(*ufs, "/home/test").size(), 2u) << "Got wrong number of files";
}

TEST(GoVfsLayers, ExistingDirectoryCollisionReaddir) {
    auto base = vfs::NewMemoryFs();
    auto overlay = vfs::NewMemoryFs();
    auto ufs = copyOnWrite(readOnly(base), overlay);

    base->MkdirAll("/home/test", 0777);
    write(*base, "/home/test/file.txt", "This is a test");
    overlay->MkdirAll("home/test", 0777);
    write(*overlay, "/home/test/file2.txt", "This is a test");
    write(*ufs, "/home/test/file3.txt", "This is a test");

    EXPECT_EQ(readdirnames(*ufs, "/home/test").size(), 3u) << "Got wrong number of files in union";
    EXPECT_EQ(readdirnames(*overlay, "/home/test").size(), 2u) << "Got wrong number of files in overlay";
}

TEST(GoVfsLayers, NestedDirBaseReaddir) {
    auto base = vfs::NewMemoryFs();
    auto overlay = vfs::NewMemoryFs();
    auto ufs = copyOnWrite(readOnly(base), overlay);

    base->MkdirAll("/home/test/foo/bar", 0777);
    write(*base, "/home/test/file.txt", "This is a test");
    write(*base, "/home/test/foo/file2.txt", "This is a test");
    write(*base, "/home/test/foo/bar/file3.txt", "This is a test");
    overlay->MkdirAll("/", 0777);

    // Something only in the base.
    EXPECT_EQ(readdirCount(*ufs, "/home/test/foo"), 2u) << "Got wrong number of files in union";
}

TEST(GoVfsLayers, NestedDirOverlayReaddir) {
    auto base = vfs::NewMemoryFs();
    auto overlay = vfs::NewMemoryFs();
    auto ufs = copyOnWrite(readOnly(base), overlay);

    base->MkdirAll("/", 0777);
    overlay->MkdirAll("/home/test/foo/bar", 0777);
    write(*overlay, "/home/test/file.txt", "This is a test");
    write(*overlay, "/home/test/foo/file2.txt", "This is a test");
    write(*overlay, "/home/test/foo/bar/file3.txt", "This is a test");

    // A nested directory only in the overlay.
    EXPECT_EQ(readdirCount(*ufs, "/home/test/foo"), 2u) << "Got wrong number of files in union";
}

TEST(GoVfsLayers, NestedDirOverlayOsFsReaddir) {
    tempOsBaseFss temps;
    auto base = temps.make();
    auto overlay = temps.make();
    auto ufs = copyOnWrite(readOnly(base), overlay);

    base->MkdirAll("/", 0777);
    overlay->MkdirAll("/home/test/foo/bar", 0777);
    write(*overlay, "/home/test/file.txt", "This is a test");
    write(*overlay, "/home/test/foo/file2.txt", "This is a test");
    write(*overlay, "/home/test/foo/bar/file3.txt", "This is a test");

    EXPECT_EQ(readdirCount(*ufs, "/home/test/foo"), 2u) << "Got wrong number of files in union";
}

TEST(GoVfsLayers, CopyOnWriteFsWithOsFs) {
    tempOsBaseFss temps;
    auto base = temps.make();
    auto overlay = temps.make();
    auto ufs = copyOnWrite(readOnly(base), overlay);

    base->MkdirAll("/home/test", 0777);
    write(*base, "/home/test/file.txt", "This is a test");
    overlay->MkdirAll("home/test", 0777);
    write(*overlay, "/home/test/file2.txt", "This is a test");
    write(*ufs, "/home/test/file3.txt", "This is a test");

    EXPECT_EQ(readdirnames(*ufs, "/home/test").size(), 3u) << "Got wrong number of files in union";
    EXPECT_EQ(readdirnames(*overlay, "/home/test").size(), 2u) << "Got wrong number of files in overlay";
}

TEST(GoVfsLayers, UnionCacheWrite) {
    auto base = vfs::NewMemoryFs();
    auto layer = vfs::NewMemoryFs();
    auto ufs = vfs::NewCacheOnReadFs(base, layer, time::Duration(0));

    base->Mkdir("/data", 0777);
    auto [fh, err] = ufs->Create("/data/file.txt");
    ASSERT_FALSE(err) << "Failed to create file";
    EXPECT_FALSE(fh->Write(as_bytes("This is a test")).second) << "Failed to write file";

    fh->Seek(0, io::SeekStart);
    std::vector<byte> buf(4);
    fh->Read(buf);
    fh->Write(as_bytes(" IS A"));
    fh->Close();

    auto [baseData, _] = vfs::ReadFile(*base, "/data/file.txt");
    auto [layerData, _] = vfs::ReadFile(*layer, "/data/file.txt");
    EXPECT_EQ(str(baseData), str(layerData)) << "Different data";
}

TEST(GoVfsLayers, UnionCacheExpire) {
    auto base = vfs::NewMemoryFs();
    auto layer = vfs::NewMemoryFs();
    auto ufs = vfs::NewCacheOnReadFs(base, layer, time::Second);

    base->Mkdir("/data", 0777);
    auto [fh, err] = ufs->Create("/data/file.txt");
    ASSERT_FALSE(err) << "Failed to create file";
    EXPECT_FALSE(fh->Write(as_bytes("This is a test")).second) << "Failed to write file";
    fh->Close();

    std::tie(fh, err) = base->Create("/data/file.txt");
    // So that the write gets a later time.
    time::Sleep(2 * time::Second);
    fh->WriteString("Another test");
    fh->Close();

    auto [data, _] = vfs::ReadFile(*ufs, "/data/file.txt");
    EXPECT_EQ(str(data), "Another test") << "cache time failed";
}

TEST(GoVfsLayers, CacheOnReadFsNotInLayer) {
    auto base = vfs::NewMemoryFs();
    auto layer = vfs::NewMemoryFs();
    auto fs = vfs::NewCacheOnReadFs(base, layer, time::Duration(0));

    write(*base, "/file.txt", "This is a test");

    auto [fh, err] = fs->Open("/file.txt");
    ASSERT_FALSE(err) << "could not open file: " << err;
    auto [b, rerr] = vfs::ReadAll(*fh);
    fh->Close();
    ASSERT_FALSE(rerr) << "could not read file: " << rerr;
    EXPECT_EQ(str(b), "This is a test");

    std::tie(fh, err) = layer->Open("/file.txt");
    ASSERT_FALSE(err) << "could not open file from layer: " << err;
    fh->Close();
}

// #194
TEST(GoVfsLayers, UnionFileReaddirEmpty) {
    auto ufs = copyOnWrite(vfs::NewMemoryFs(), vfs::NewMemoryFs());
    // The OsFs's files return io::EOF_ at the end of a directory.
    for (auto& fs : {vfs::NewOsFs(), ufs, vfs::NewMemoryFs()}) {
        auto [baseDir, err] = vfs::TempDir(*fs, "", "empty-dir");
        ASSERT_FALSE(err) << err;
        auto [f, oerr] = fs->Open(baseDir);
        ASSERT_FALSE(oerr) << oerr;
        auto [names, rerr] = f->Readdirnames(1);
        EXPECT_EQ(rerr, io::EOF_) << fs->Name() << ": " << rerr;
        EXPECT_TRUE(names.empty()) << "should be empty";
        f->Close();
        fs->RemoveAll(baseDir);
    }
}

// #197
TEST(GoVfsLayers, UnionFileReaddirDuplicateEmpty) {
    auto base = vfs::NewMemoryFs();
    auto [dir, err] = vfs::TempDir(*base, "", "empty-dir");
    ASSERT_FALSE(err) << err;

    // The overlay has the same empty directory.
    auto overlay = vfs::NewMemoryFs();
    ASSERT_FALSE(overlay->Mkdir(dir, 0700));

    auto ufs = copyOnWrite(base, overlay);
    auto [f, oerr] = ufs->Open(dir);
    ASSERT_FALSE(oerr) << oerr;
    auto [names, rerr] = f->Readdirnames(0);
    EXPECT_NE(rerr, io::EOF_) << "unexpected io.EOF error";
    EXPECT_TRUE(names.empty()) << "should be empty";
    f->Close();
}

TEST(GoVfsLayers, UnionFileReaddirAskForTooMany) {
    auto base = vfs::NewMemoryFs();
    auto overlay = vfs::NewMemoryFs();
    constexpr int testFiles = 5;
    for (int i = 0; i < testFiles; i++) {
        vfs::WriteFile(*base, "file" + std::to_string(i) + ".txt", as_bytes("afero"), 0777);
    }
    auto ufs = copyOnWrite(base, overlay);

    auto [f, err] = ufs->Open("");
    ASSERT_FALSE(err) << err;

    // Some of the files.
    std::size_t wantNames = 3;
    auto [names, rerr] = f->Readdirnames(static_cast<int>(wantNames));
    ASSERT_FALSE(rerr) << rerr;
    ASSERT_EQ(names.size(), wantNames);

    // More files than are left.
    wantNames = testFiles - names.size();
    std::tie(names, rerr) = f->Readdirnames(static_cast<int>(wantNames) + 1);
    ASSERT_FALSE(rerr) << rerr;
    ASSERT_EQ(names.size(), wantNames);

    // The end of the directory.
    std::tie(names, rerr) = f->Readdirnames(3);
    EXPECT_EQ(rerr, io::EOF_) << rerr;
    f->Close();
}

// ---------------------------------------------------------------------------
// copyOnWriteFs_test.go

TEST(GoVfsLayers, CopyOnWrite) {
    auto osFs = vfs::NewOsFs();
    auto [writeDir, err] = vfs::TempDir(*osFs, "", "copy-on-write-test");
    ASSERT_FALSE(err) << "error creating tempDir " << err;

    auto compositeFs = vfs::NewCopyOnWriteFs(vfs::NewReadOnlyFs(vfs::NewOsFs()), osFs);
    string dir = filepath::Join(writeDir, "some/path");
    ASSERT_FALSE(compositeFs->MkdirAll(dir, 0744));
    auto [f, cerr] = compositeFs->Create(filepath::Join(dir, "newfile"));
    ASSERT_FALSE(cerr) << cerr;
    f->Close();

    // https://github.com/spf13/afero/issues/189: Mkdir and MkdirAll as the
    // OsFs's.
    for (auto& fs : {osFs, compositeFs}) {
        err = fs->Mkdir(dir, 0744);
        EXPECT_TRUE(err && os::IsExist(err)) << "Mkdir: Got " << err << " for " << fs->Name();
        // MkdirAll doesn't fail when the directory exists.
        EXPECT_FALSE(fs->MkdirAll(dir, 0744)) << "MkdirAll for " << fs->Name();
    }
    osFs->RemoveAll(writeDir);
}

TEST(GoVfsLayers, CopyOnWriteFileInMemMapBase) {
    auto base = vfs::NewMemoryFs();
    auto layer = vfs::NewMemoryFs();
    ASSERT_FALSE(vfs::WriteFile(*base, "base.txt", as_bytes("base"), 0755));
    auto ufs = vfs::NewCopyOnWriteFs(base, layer);
    auto [_, err] = ufs->Stat("base.txt");
    EXPECT_FALSE(err) << err;
}

// ---------------------------------------------------------------------------
// basepath_test.go

TEST(GoVfsLayers, BasePath) {
    auto baseFs = vfs::NewMemoryFs();
    baseFs->MkdirAll("/base/path/tmp", 0777);
    auto bp = vfs::NewBasePathFs(baseFs, "/base/path");
    EXPECT_FALSE(bp->Create("/tmp/foo").second) << "Failed to set real path";
    auto [fh, err] = bp->Create("../tmp/bar");
    EXPECT_TRUE(err) << "succeeded in creating " << fh->Name();
}

TEST(GoVfsLayers, BasePathOutsideSibling) {
    auto baseFs = vfs::NewMemoryFs();
    baseFs->MkdirAll("/base", 0777);
    baseFs->MkdirAll("/base-other", 0777);
    write(*baseFs, "/base-other/secret", "secret");
    auto bp = vfs::NewBasePathFs(baseFs, "/base");
    auto [_, err] = bp->Open("../base-other/secret");
    EXPECT_TRUE(os::IsNotExist(err)) << err;
    auto [path, rerr] = dynamic_cast<vfs::BasePathFs&>(*bp).RealPath("../base-other/secret");
    EXPECT_EQ(rerr, fs::ErrNotExist) << path;

    // A relative base.
    auto rel = make_ptr<vfs::BasePathFs>(baseFs, "base");
    EXPECT_EQ(rel->RealPath("x").first, "base/x");
    EXPECT_TRUE(rel->RealPath("../base-other/secret").second);
    auto dot = make_ptr<vfs::BasePathFs>(baseFs, ".");
    EXPECT_EQ(dot->RealPath("x").first, "x");
    EXPECT_TRUE(dot->RealPath("../x").second);
}

TEST(GoVfsLayers, BasePathRoot) {
    auto baseFs = vfs::NewMemoryFs();
    baseFs->MkdirAll("/base/path/foo/baz", 0777);
    baseFs->MkdirAll("/base/path/boo/", 0777);
    auto bp = vfs::NewBasePathFs(baseFs, "/base/path");
    auto [rd, err] = vfs::ReadDir(*bp, "/");
    EXPECT_EQ(rd.size(), 2u) << "base path doesn't respect root";
    EXPECT_FALSE(err) << err;
}

TEST(GoVfsLayers, RealPath) {
    auto fs = vfs::NewOsFs();
    auto [baseDir, err] = vfs::TempDir(*fs, "", "base");
    ASSERT_FALSE(err) << "error creating tempDir " << err;
    auto [anotherDir, aerr] = vfs::TempDir(*fs, "", "another");
    ASSERT_FALSE(aerr) << "error creating tempDir " << aerr;

    auto bp = make_ptr<vfs::BasePathFs>(fs, baseDir);
    string subDir = filepath::Join(baseDir, "s1");
    auto [realPath, rerr] = bp->RealPath("/s1");
    EXPECT_FALSE(rerr) << rerr;
    EXPECT_EQ(realPath, subDir);

    // On Unix, a name can't be told to be outside the base by looking at
    // it: anotherDir is under it. Opening it fails later.
    auto [surrealPath, serr] = bp->RealPath(anotherDir);
    EXPECT_FALSE(serr) << serr;
    EXPECT_EQ(surrealPath, filepath::Join(baseDir, anotherDir));

    fs->RemoveAll(baseDir);
    fs->RemoveAll(anotherDir);
}

TEST(GoVfsLayers, NestedBasePaths) {
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

        for (auto& baseFs : {level3Fs, level2Fs, level1Fs}) {
            EXPECT_FALSE(baseFs->MkdirAll("f.txt", 0755));
            EXPECT_FALSE(baseFs->Stat("f.txt").second);
            if (baseFs == level3Fs) {
                string pathToExist = filepath::Join(ds.Dir3, "f.txt");
                EXPECT_FALSE(level2Fs->Stat(pathToExist).second) << pathToExist;
            } else if (baseFs == level2Fs) {
                string pathToExist = filepath::Join(ds.Dir2, ds.Dir3, "f.txt");
                EXPECT_FALSE(level1Fs->Stat(pathToExist).second) << pathToExist;
            }
        }
    }
}

TEST(GoVfsLayers, BasePathOpenFile) {
    auto baseFs = vfs::NewMemoryFs();
    baseFs->MkdirAll("/base/path/tmp", 0777);
    auto bp = vfs::NewBasePathFs(baseFs, "/base/path");
    auto [f, err] = bp->OpenFile("/tmp/file.txt", os::O_CREATE_, 0600);
    ASSERT_FALSE(err) << "failed to open file: " << err;
    EXPECT_EQ(filepath::Dir(f->Name()), filepath::Clean("/tmp")) << "realpath leaked: " << f->Name();
}

TEST(GoVfsLayers, BasePathCreate) {
    auto baseFs = vfs::NewMemoryFs();
    baseFs->MkdirAll("/base/path/tmp", 0777);
    auto bp = vfs::NewBasePathFs(baseFs, "/base/path");
    auto [f, err] = bp->Create("/tmp/file.txt");
    ASSERT_FALSE(err) << "failed to create file: " << err;
    EXPECT_EQ(filepath::Dir(f->Name()), filepath::Clean("/tmp")) << "realpath leaked: " << f->Name();
}

TEST(GoVfsLayers, BasePathTempFile) {
    auto baseFs = vfs::NewMemoryFs();
    baseFs->MkdirAll("/base/path/tmp", 0777);
    auto bp = vfs::NewBasePathFs(baseFs, "/base/path");

    auto [tDir, err] = vfs::TempDir(*bp, "/tmp", "");
    ASSERT_FALSE(err) << "Failed to TempDir: " << err;
    EXPECT_EQ(filepath::Dir(tDir), filepath::Clean("/tmp")) << "Tempdir realpath leaked: " << tDir;
    auto [tempFile, terr] = vfs::TempFile(*bp, tDir, "");
    ASSERT_FALSE(terr) << "Failed to TempFile: " << terr;
    EXPECT_EQ(filepath::Dir(tempFile->Name()), tDir) << "TempFile realpath leaked";
    tempFile->Close();
}

// ---------------------------------------------------------------------------
// ro_regexp_test.go

TEST(GoVfsLayers, FilterReadOnly) {
    auto fs = readOnly(vfs::NewMemoryFs());
    EXPECT_TRUE(fs->Create("/file.txt").second) << "Did not fail to create file";
}

TEST(GoVfsLayers, FilterReadonlyRemoveAndRead) {
    auto mfs = vfs::NewMemoryFs();
    write(*mfs, "/file.txt", "content here");

    auto fs = vfs::NewReadOnlyFs(mfs);
    EXPECT_TRUE(fs->Remove("/file.txt")) << "Did not fail to remove file";

    auto [fh, err] = fs->Open("/file.txt");
    ASSERT_FALSE(err) << "Failed to open file: " << err;
    std::vector<byte> buf(12);
    fh->Read(buf);
    fh->Close();
    EXPECT_EQ(str(buf), "content here") << "Failed to read file";

    EXPECT_FALSE(mfs->Remove("/file.txt")) << "Failed to remove file";
    std::tie(fh, err) = fs->Open("/file.txt");
    EXPECT_TRUE(err) << "File still present";
}

TEST(GoVfsLayers, FilterRegexp) {
    auto fs = vfs::NewRegexpFs(vfs::NewMemoryFs(), regexp::MustCompile(R"(\.txt$)"));
    EXPECT_TRUE(fs->Create("/file.html").second) << "Did not fail to create file";
}

TEST(GoVfsLayers, FilterRORegexpChain) {
    auto rofs = readOnly(vfs::NewMemoryFs());
    auto fs = make_ptr<vfs::RegexpFs>(rofs, regexp::MustCompile(R"(\.txt$)"));
    EXPECT_TRUE(fs->Create("/file.txt").second) << "Did not fail to create file";
}

TEST(GoVfsLayers, FilterRegexReadDir) {
    auto mfs = vfs::NewMemoryFs();
    auto fs1 = make_ptr<vfs::RegexpFs>(mfs, regexp::MustCompile(R"(\.txt$)"));
    auto fs = make_ptr<vfs::RegexpFs>(fs1, regexp::MustCompile("^a"));

    mfs->MkdirAll("/dir/sub", 0777);
    for (std::string_view name : {"afile.txt", "afile.html", "bfile.txt"}) {
        for (std::string_view dir : {"/dir/", "/dir/sub/"}) write(*mfs, std::string(dir) + std::string(name), "");
    }

    auto [files, _] = vfs::ReadDir(*fs, "/dir");
    EXPECT_EQ(files.size(), 2u) << "Got wrong number of files";  // afile.txt, sub

    auto [f, err] = fs->Open("/dir/sub");
    ASSERT_FALSE(err) << err;
    auto [names, _] = f->Readdirnames(-1);
    EXPECT_EQ(names.size(), 1u) << "Got wrong number of names";
    f->Close();
}

// ---------------------------------------------------------------------------
// iofs_test.go

TEST(GoVfsLayers, IOFS) {
    {
        SCOPED_TRACE("use MemoryFs");
        auto mmfs = vfs::NewMemoryFs();
        ASSERT_FALSE(mmfs->MkdirAll("dir1/dir2", fs::ModePerm));
        auto [f, err] = mmfs->OpenFile("dir1/dir2/test.txt", os::O_RDWR_ | os::O_CREATE_, fs::ModePerm);
        ASSERT_FALSE(err) << "OpenFile (O_CREATE) failed: " << err;
        f->Close();
        EXPECT_FALSE(fstest::TestFS(vfs::NewIOFS(mmfs), {"dir1/dir2/test.txt"})) << fstest::TestFS(vfs::NewIOFS(mmfs), {"dir1/dir2/test.txt"});
    }
    {
        SCOPED_TRACE("use OsFs");
        auto [tmp, terr] = os::MkdirTemp("", "vfs_iofs");
        ASSERT_FALSE(terr);
        auto osfs = vfs::NewBasePathFs(vfs::NewOsFs(), tmp);
        ASSERT_FALSE(osfs->MkdirAll("dir1/dir2", fs::ModePerm));
        auto [f, err] = osfs->OpenFile("dir1/dir2/test.txt", os::O_RDWR_ | os::O_CREATE_, fs::ModePerm);
        ASSERT_FALSE(err) << "OpenFile (O_CREATE) failed: " << err;
        f->Close();
        error terr2 = fstest::TestFS(vfs::NewIOFS(osfs), {"dir1/dir2/test.txt"});
        EXPECT_FALSE(terr2) << terr2;
        os::RemoveAll(tmp);
    }
}

TEST(GoVfsLayers, IOFSNativeDirEntryWhenPossible) {
    auto [tmp, terr] = os::MkdirTemp("", "vfs_iofs");
    ASSERT_FALSE(terr);
    auto osfs = vfs::NewBasePathFs(vfs::NewOsFs(), tmp);
    ASSERT_FALSE(osfs->MkdirAll("dir1/dir2", fs::ModePerm));

    constexpr int numFiles = 10;
    std::vector<int> fileNumbers;
    for (int i = 0; i < numFiles; i++) fileNumbers.push_back(i);
    std::shuffle(fileNumbers.begin(), fileNumbers.end(), std::mt19937(std::random_device()()));
    for (int i : fileNumbers) {
        auto [f, err] = osfs->Create("dir1/dir2/test" + std::to_string(i) + ".txt");
        ASSERT_FALSE(err) << err;
        f->Close();
    }

    // The type of the DirEntries that FileInfoToDirEntry makes.
    auto [someInfo, _] = osfs->Stat("dir1");
    auto converted = fs::FileInfoToDirEntry(someInfo);
    auto& convertedType = typeid(*converted);

    auto assertDirEntries = [&](const std::vector<ptr<fs::DirEntry>>& entries, bool ordered) {
        ASSERT_EQ(entries.size(), static_cast<std::size_t>(numFiles));
        for (std::size_t i = 0; i < entries.size(); i++) {
            ASSERT_NE(typeid(*entries[i]), convertedType) << "DirEntry not native";
            if (ordered) {
                ASSERT_EQ(entries[i]->Name(), "test" + std::to_string(i) + ".txt");
            }
        }
    };

    auto [dir2, err] = osfs->Open("dir1/dir2");
    ASSERT_FALSE(err) << err;
    auto [dirEntries, rerr] = dir2->ReadDir(-1);
    ASSERT_FALSE(rerr) << rerr;
    assertDirEntries(dirEntries, false);
    dir2->Close();

    auto iofs = vfs::NewIOFS(osfs);
    std::tie(dirEntries, rerr) = iofs->ReadDir("dir1/dir2");
    ASSERT_FALSE(rerr) << rerr;
    assertDirEntries(dirEntries, true);

    int fileCount = 0;
    err = fs::WalkDir(*iofs, "", [&](std::string_view path, const ptr<fs::DirEntry>& d, error err) -> error {
        if (err) return err;
        if (!d->IsDir()) fileCount++;
        // WalkDir makes the root's with FileInfoToDirEntry itself (afero
        // checks for its own type, which that isn't).
        if (!path.empty()) {
            EXPECT_NE(typeid(*d), convertedType) << "DirEntry not native: " << path;
        }
        return nullptr;
    });
    ASSERT_FALSE(err) << err;
    EXPECT_EQ(fileCount, numFiles);
    os::RemoveAll(tmp);
}

namespace {

// afero's assertPermissionError: a PathError wrapping fs::ErrPermission.
void assertPermissionError(const error& err) {
    auto* perr = errors::As<fs::PathError>(err);
    ASSERT_TRUE(perr) << "Expected *fs.PathError, got " << err;
    EXPECT_EQ(perr->Err, fs::ErrPermission) << "Expected (*fs.PathError).Err == fs.ErrPermisson, got " << err;
}

}  // namespace

TEST(GoVfsLayers, FromIOFS) {
    auto now = time::Now();
    auto fsys = make_ptr<fstest::MapFS>(std::initializer_list<std::pair<std::string_view, fstest::MapFile>>{
        {"test.txt", {.Data = toBytes("File in root"), .Mode = fs::ModePerm, .ModTime = now}},
        {"dir1", {.Mode = fs::ModeDir | fs::ModePerm, .ModTime = now}},
        {"dir1/dir2", {.Mode = fs::ModeDir | fs::ModePerm, .ModTime = now}},
        {"dir1/dir2/hello.txt", {.Data = toBytes("Hello world"), .Mode = fs::ModePerm, .ModTime = now}},
    });
    vfs::FromIOFS fromIOFS(fsys);

    assertPermissionError(fromIOFS.Create("test").second);
    assertPermissionError(fromIOFS.Mkdir("test", 0));
    assertPermissionError(fromIOFS.MkdirAll("test", 0));

    // Open
    EXPECT_TRUE(errors::Is(fromIOFS.Open("nonexisting").second, fs::ErrNotExist));
    {
        auto [dirFile, err] = fromIOFS.Open("dir1");
        ASSERT_FALSE(err) << "dir1 open failed: " << err;
        auto [dirStat, serr] = dirFile->Stat();
        ASSERT_FALSE(serr) << "dir1 stat failed: " << serr;
        EXPECT_TRUE(dirStat->IsDir()) << "dir1 stat told that it is not a directory";
        dirFile->Close();
    }
    {
        auto [file, err] = fromIOFS.Open("test.txt");
        ASSERT_FALSE(err) << "test.txt open failed: " << err;
        auto [fileStat, serr] = file->Stat();
        ASSERT_FALSE(serr) << "test.txt stat failed: " << serr;
        EXPECT_FALSE(fileStat->IsDir()) << "test.txt stat told that it is a directory";
        file->Close();
    }

    assertPermissionError(fromIOFS.Remove("test"));
    assertPermissionError(fromIOFS.Rename("test", "test2"));

    // Stat
    EXPECT_TRUE(errors::Is(fromIOFS.Stat("nonexisting").second, fs::ErrNotExist));
    {
        auto [stat, err] = fromIOFS.Stat("dir1/dir2");
        ASSERT_FALSE(err) << "dir1/dir2 stat failed: " << err;
        EXPECT_TRUE(stat->IsDir()) << "dir1/dir2 stat told that it is not a directory";
    }
    {
        auto [stat, err] = fromIOFS.Stat("dir1/dir2/hello.txt");
        ASSERT_FALSE(err) << "dir1/dir2/hello.txt stat failed: " << err;
        EXPECT_FALSE(stat->IsDir()) << "dir1/dir2/hello.txt stat told that it is a directory";
        EXPECT_EQ(stat->Size(), 11) << "dir1/dir2/hello.txt stat told invalid size";
    }

    assertPermissionError(fromIOFS.Chmod("test", fs::ModePerm));
    assertPermissionError(fromIOFS.Chown("test", 0, 0));
    assertPermissionError(fromIOFS.Chtimes("test", time::Now(), time::Now()));
}

TEST(GoVfsLayers, FromIOFS_File) {
    auto now = time::Now();
    auto fsys = make_ptr<fstest::MapFS>(std::initializer_list<std::pair<std::string_view, fstest::MapFile>>{
        {"test.txt", {.Data = toBytes("File in root"), .Mode = fs::ModePerm, .ModTime = now}},
        {"dir1", {.Mode = fs::ModeDir | fs::ModePerm, .ModTime = now}},
        {"dir2", {.Mode = fs::ModeDir | fs::ModePerm, .ModTime = now}},
    });
    vfs::FromIOFS fromIOFS(fsys);

    auto [file, err] = fromIOFS.Open("test.txt");
    ASSERT_FALSE(err) << "test.txt open failed: " << err;
    auto [fileStat, serr] = file->Stat();
    ASSERT_FALSE(serr) << "test.txt stat failed: " << serr;
    ASSERT_FALSE(fileStat->IsDir()) << "test.txt stat told that it is a directory";

    // ReadAt: MapFS's files are io::ReaderAts.
    std::vector<byte> b(2);
    auto [_, rerr] = file->ReadAt(b, 2);
    EXPECT_FALSE(rerr) << "ReadAt failed: " << rerr;
    EXPECT_EQ(str(b), "le") << "Unexpected content read";

    // Seek
    auto [n, sk] = file->Seek(2, io::SeekStart);
    EXPECT_FALSE(sk) << "Seek failed: " << sk;
    EXPECT_EQ(n, 2) << "Seek returned unexpected value";

    assertPermissionError(file->Write({}).second);
    assertPermissionError(file->WriteAt({}, 0).second);
    EXPECT_EQ(file->Name(), "test.txt");

    // Readdir
    assertPermissionError(file->Readdir(-1).second);
    {
        auto [root, oerr] = fromIOFS.Open(".");
        ASSERT_FALSE(oerr) << "root open failed: " << oerr;
        auto [items, rderr] = root->Readdir(-1);
        ASSERT_FALSE(rderr) << "Readdir error: " << rderr;
        struct expectedItem {
            std::string_view Name;
            bool IsDir;
            int64 Size;
        };
        expectedItem expectedItems[] = {{"dir1", true, 0}, {"dir2", true, 0}, {"test.txt", false, 12}};
        ASSERT_EQ(items.size(), std::size(expectedItems)) << "Items count mismatch";
        for (std::size_t i = 0; i < items.size(); i++) {
            EXPECT_EQ(items[i]->Name(), expectedItems[i].Name) << "Item " << i;
            EXPECT_EQ(items[i]->IsDir(), expectedItems[i].IsDir) << "Item " << i;
            EXPECT_EQ(items[i]->Size(), expectedItems[i].Size) << "Item " << i;
        }
        root->Close();
    }

    // Readdirnames
    assertPermissionError(file->Readdirnames(-1).second);
    {
        auto [root, oerr] = fromIOFS.Open(".");
        ASSERT_FALSE(oerr) << "root open failed: " << oerr;
        auto [items, rderr] = root->Readdirnames(-1);
        ASSERT_FALSE(rderr) << "Readdirnames error: " << rderr;
        std::vector<string> expectedItems = {"dir1", "dir2", "test.txt"};
        EXPECT_EQ(items, expectedItems);
        root->Close();
    }

    assertPermissionError(file->Truncate(1));
    assertPermissionError(file->WriteString("a").second);
    file->Close();
}

// ---------------------------------------------------------------------------
// HttpFs

TEST(GoVfsLayers, HttpFs) {
    auto memFs = vfs::NewMemoryFs();
    memFs->MkdirAll("/www/sub", 0755);
    vfs::WriteFile(*memFs, "/www/index.txt", as_bytes("hello from memory"), 0644);
    vfs::WriteFile(*memFs, "/secret.txt", as_bytes("secret"), 0644);

    auto httpFs = vfs::NewHttpFs(memFs);
    EXPECT_EQ(httpFs->Name(), "h HttpFs");
    auto handler = http::FileServer(httpFs->Dir("/www"));

    auto rr = httptest::NewRecorder();
    handler->ServeHTTP(*rr, *httptest::NewRequest("GET", "/index.txt"));
    EXPECT_EQ(rr->Code, http::StatusOK);
    EXPECT_EQ(rr->Body, "hello from memory");

    // Request paths can't leave the directory.
    rr = httptest::NewRecorder();
    handler->ServeHTTP(*rr, *httptest::NewRequest("GET", "/../secret.txt"));
    EXPECT_NE(rr->Body, "secret");

    // A directory lists its entries.
    rr = httptest::NewRecorder();
    handler->ServeHTTP(*rr, *httptest::NewRequest("GET", "/"));
    EXPECT_EQ(rr->Code, http::StatusOK);
    EXPECT_NE(rr->Body.find("index.txt"), std::string::npos) << rr->Body;
    EXPECT_NE(rr->Body.find("sub/"), std::string::npos) << rr->Body;

    rr = httptest::NewRecorder();
    handler->ServeHTTP(*rr, *httptest::NewRequest("GET", "/missing.txt"));
    EXPECT_EQ(rr->Code, http::StatusNotFound);

    // The other methods pass through.
    auto [f, err] = httpFs->Create("/www/new.txt");
    ASSERT_FALSE(err) << err;
    f->Close();
    EXPECT_FALSE(httpFs->Stat("/www/new.txt").second);
}
