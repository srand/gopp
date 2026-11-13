// The testing/fstest package: an in-memory file system, and a test of file
// system implementations.
//
//     auto fsys = fstest::MakeMapFS({
//         {"hello.txt", {.Data = to_bytes("hello, world\n")}},
//         {"dir/link", {.Data = to_bytes("../hello.txt"), .Mode = fs::ModeSymlink}},
//     });
//     auto [data, err] = fs::ReadFile(*fsys, "dir/link");
//
//     if (auto err = fstest::TestFS(os::DirFS(dir), "hello.txt")) FAIL() << err;
#pragma once

// The module interfaces (modules/) import these instead.
#ifndef GOPP_MODULE
#include <gopp/errors.hpp>
#include <gopp/io/fs.hpp>
#include <gopp/ptr.hpp>
#include <gopp/slice.hpp>
#include <gopp/string.hpp>
#include <gopp/time.hpp>

#include <initializer_list>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#endif

namespace gopp::fstest {

// fstest.MapFile: a file in a MapFS.
struct MapFile {
    bytes Data = {};            // file content or symlink destination
    fs::FileMode Mode = {};     // fs::FileInfo::Mode
    time::Time ModTime = {};    // fs::FileInfo::ModTime
    const void* Sys = nullptr;  // fs::FileInfo::Sys
};

// fstest.MapFS: a simple in-memory file system for tests, a map from path
// names (arguments to Open) to the files or directories they represent.
//
// The map need not include parent directories for files it contains: they
// are synthesized as needed, with mode fs::ModeDir | 0555. A file with
// ModeSymlink is a symbolic link to the path in its Data.
//
// It implements every optional FS interface, so it's for testing code that
// uses file systems, not for testing other ones (TestFS is for that).
class MapFS : public fs::ReadFileFS,
              public fs::ReadDirFS,
              public fs::StatFS,
              public fs::GlobFS,
              public fs::SubFS,
              public fs::ReadLinkFS {
public:
    MapFS();
    MapFS(std::initializer_list<std::pair<std::string_view, MapFile>> files);
    ~MapFS() override;

    // m[name]: the file named name, added if it's missing.
    MapFile& operator[](std::string_view name);
    // delete(m, name)
    void erase(std::string_view name);
    std::size_t size() const noexcept { return files_.size(); }

    std::pair<ptr<fs::File>, error> Open(std::string_view name) override;
    std::pair<bytes, error> ReadFile(std::string_view name) override;
    std::pair<std::vector<ptr<fs::DirEntry>>, error> ReadDir(std::string_view name) override;
    std::pair<ptr<fs::FileInfo>, error> Stat(std::string_view name) override;
    std::pair<std::vector<string>, error> Glob(std::string_view pattern) override;
    std::pair<ptr<fs::FS>, error> Sub(std::string_view dir) override;
    std::pair<string, error> ReadLink(std::string_view name) override;
    std::pair<ptr<fs::FileInfo>, error> Lstat(std::string_view name) override;

    // A file, shared with the open files and FileInfos made of it.
    struct entry;

private:
    std::pair<std::string, bool> resolveSymlinks(std::string_view name) const;
    std::pair<ptr<fs::FileInfo>, error> lstat(std::string_view name) const;
    ptr<entry> find(std::string_view name) const;

    std::map<std::string, ptr<entry>, std::less<>> files_;
};

// A new MapFS of files: what Go writes as fstest.MapFS{...}.
ptr<MapFS> MakeMapFS(std::initializer_list<std::pair<std::string_view, MapFile>> files);

// fstest.TestFS: tests a file system implementation. It walks the entire
// tree of files in fsys, opening and checking that each file behaves
// correctly. Symbolic links are not followed, but their Lstat values are
// checked if the file system implements fs::ReadLinkFS. It also checks
// that the file system contains at least the expected files. As a special
// case, if no expected files are listed, fsys must be empty.
//
// It returns nil, or an error joining the misbehaviors it found
// (errors::Is and As see each).
error TestFS(const ptr<fs::FS>& fsys, std::span<const std::string_view> expected);
error TestFS(const ptr<fs::FS>& fsys, std::initializer_list<std::string_view> expected = {});

}  // namespace gopp::fstest
