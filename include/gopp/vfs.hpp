// The vfs package: file systems to read and write, real or in memory, and
// layers that combine them. A port of spf13/afero v1.15.0.
//
//     ptr<vfs::Fs> fsys = vfs::NewMemoryFs();          // or vfs::NewOsFs()
//     vfs::WriteFile(*fsys, "/etc/app.conf", as_bytes("debug = true"), 0644);
//     auto [data, err] = vfs::ReadFile(*fsys, "/etc/app.conf");
//
// Code that takes a vfs::Fs instead of calling the os package can be
// tested against a MemoryFs, and run against the OsFs. The layers wrap
// other Fss:
//
//     BasePathFs     confines names to a directory of another Fs
//     ReadOnlyFs     refuses writes
//     RegexpFs       shows only the files whose names match a regexp
//     CopyOnWriteFs  writes to a layer over a base it leaves alone
//     CacheOnReadFs  caches a slow base in a fast layer
//
//     auto base = vfs::NewReadOnlyFs(vfs::NewOsFs());
//     auto fsys = vfs::NewCopyOnWriteFs(base, vfs::NewMemoryFs());
//
// IOFS turns an Fs into an fs::FS, for the functions of io/fs and
// http::FileServerFS; FromIOFS turns an fs::FS into a read-only Fs.
// <gopp/vfs/http.hpp> has afero's HttpFs.
//
// Renamed from afero: the package (afero is vfs), MemMapFs (MemoryFs) and
// the Afero struct (Vfs). Not ported: NeuterAccents (it needs Unicode
// normalization), MemMapFs.List (a debugging aid), and afero's tarfs,
// zipfs, sftpfs and gcsfs. Fixed: BasePathFs no longer lets "../" reach a
// sibling directory whose name begins with the base's ("/base-other"), and
// a MemoryFs file's ReadAt and WriteAt behave as io's interfaces say:
// ReadAt returns io::EOF_ when it reads less than asked, and neither
// moves the offset of Read and Write.
#pragma once

// The module interfaces (modules/) import these instead.
#ifndef GOPP_MODULE
#include <gopp/base.hpp>
#include <gopp/errors.hpp>
#include <gopp/io.hpp>
#include <gopp/io/fs.hpp>
#include <gopp/os.hpp>
#include <gopp/path/filepath.hpp>
#include <gopp/ptr.hpp>
#include <gopp/regexp.hpp>
#include <gopp/slice.hpp>
#include <gopp/string.hpp>
#include <gopp/time.hpp>

#include <functional>
#include <memory>
#include <span>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>
#endif

namespace gopp::vfs {

// vfs.File: an open file of an Fs. The OsFs's are os::Files'; the methods
// behave as os::File's do.
struct File : public fs::ReadDirFile,
              public io::Writer,
              public io::Seeker,
              public io::ReaderAt,
              public io::WriterAt {
    // The name the file was opened with.
    virtual string Name() const = 0;
    // Reads up to count entries of a directory (all if count <= 0); with
    // count > 0, no entries and io::EOF_ at the end.
    virtual std::pair<std::vector<ptr<fs::FileInfo>>, error> Readdir(int count) = 0;
    // Readdir's names. By default, from Readdir.
    virtual std::pair<std::vector<string>, error> Readdirnames(int n);
    // ReadDir: fs::ReadDirFile's. By default, Readdir's FileInfos as
    // DirEntries; on an error, no entries.
    std::pair<std::vector<ptr<fs::DirEntry>>, error> ReadDir(int n) override;
    // Commits the contents to stable storage, where there is some.
    virtual error Sync() = 0;
    virtual error Truncate(int64 size) = 0;
    // By default, Write.
    virtual std::pair<isize, error> WriteString(std::string_view s);
};

// vfs.Fs: a file system. Names are as the os package takes them: with
// slashes, absolute or relative. Each method behaves as the os function of
// the same name does, as far as the file system allows.
struct Fs : public virtual object {
    // Creates or truncates the named file, open for reading and writing.
    virtual std::pair<ptr<File>, error> Create(std::string_view name) = 0;
    virtual error Mkdir(std::string_view name, fs::FileMode perm) = 0;
    // Creates the directory and any parents that don't exist.
    virtual error MkdirAll(std::string_view path, fs::FileMode perm) = 0;
    // Opens the named file for reading.
    virtual std::pair<ptr<File>, error> Open(std::string_view name) = 0;
    // Opens with flag (os::O_RDONLY_ and friends) and, when creating, perm.
    virtual std::pair<ptr<File>, error> OpenFile(std::string_view name, int flag, fs::FileMode perm) = 0;
    virtual error Remove(std::string_view name) = 0;
    // Removes path and anything it contains; nil if path doesn't exist.
    virtual error RemoveAll(std::string_view path) = 0;
    virtual error Rename(std::string_view oldname, std::string_view newname) = 0;
    virtual std::pair<ptr<fs::FileInfo>, error> Stat(std::string_view name) = 0;
    // The file system's name: "OsFs", "MemoryFs", ...
    virtual string Name() const = 0;
    virtual error Chmod(std::string_view name, fs::FileMode mode) = 0;
    virtual error Chown(std::string_view name, int uid, int gid) = 0;
    virtual error Chtimes(std::string_view name, time::Time atime, time::Time mtime) = 0;
};

// vfs.Lstater: an Fs that can Lstat, where its files can be links.
struct Lstater : public virtual object {
    // Lstat if the file system has links (or wraps one that has), else
    // Stat; and whether it was Lstat.
    virtual std::tuple<ptr<fs::FileInfo>, bool, error> LstatIfPossible(std::string_view name) = 0;
};

// vfs.Linker: an Fs that can make symbolic links.
struct Linker : public virtual object {
    // os::Symlink, or a LinkError wrapping ErrNoSymlink.
    virtual error SymlinkIfPossible(std::string_view oldname, std::string_view newname) = 0;
};

// vfs.LinkReader: an Fs that can read symbolic links.
struct LinkReader : public virtual object {
    // os::Readlink, or a PathError wrapping ErrNoReadlink.
    virtual std::pair<string, error> ReadlinkIfPossible(std::string_view name) = 0;
};

// vfs.Symlinker: Lstater, Linker and LinkReader.
struct Symlinker : public Lstater, public Linker, public LinkReader {};

// The errors of the package.
extern const error ErrFileClosed;  // "File is closed"
extern const error ErrOutOfRange;  // "out of range"
extern const error ErrTooLarge;    // "too large"
extern const error ErrNoSymlink;   // "symlink not supported"
extern const error ErrNoReadlink;  // "readlink not supported"
// EBADFD: a UnionFile with neither layer.
extern const error BADFD;
inline const error& ErrFileNotFound = fs::ErrNotExist;
inline const error& ErrFileExists = fs::ErrExist;
inline const error& ErrDestinationExists = fs::ErrExist;

// vfs.FilePathSeparator
inline constexpr std::string_view FilePathSeparator = "/";

// ---------------------------------------------------------------------------
// File systems

// vfs.OsFs: the operating system's file system, by the os package.
class OsFs final : public Fs, public Symlinker {
public:
    std::pair<ptr<File>, error> Create(std::string_view name) override;
    error Mkdir(std::string_view name, fs::FileMode perm) override;
    error MkdirAll(std::string_view path, fs::FileMode perm) override;
    std::pair<ptr<File>, error> Open(std::string_view name) override;
    std::pair<ptr<File>, error> OpenFile(std::string_view name, int flag, fs::FileMode perm) override;
    error Remove(std::string_view name) override;
    error RemoveAll(std::string_view path) override;
    error Rename(std::string_view oldname, std::string_view newname) override;
    std::pair<ptr<fs::FileInfo>, error> Stat(std::string_view name) override;
    string Name() const override { return string::fromStatic("OsFs"); }
    error Chmod(std::string_view name, fs::FileMode mode) override;
    error Chown(std::string_view name, int uid, int gid) override;
    error Chtimes(std::string_view name, time::Time atime, time::Time mtime) override;
    std::tuple<ptr<fs::FileInfo>, bool, error> LstatIfPossible(std::string_view name) override;
    error SymlinkIfPossible(std::string_view oldname, std::string_view newname) override;
    std::pair<string, error> ReadlinkIfPossible(std::string_view name) override;
};

// vfs.NewOsFs
ptr<Fs> NewOsFs();

namespace detail {
struct memoryFs;
}

// vfs.MemoryFs (afero's MemMapFs): a file system in memory, which starts
// with an empty root directory, "/". Names are cleaned (filepath::Clean),
// and ".", ".." and "/" are the root. Its files are safe to use from
// several fibers at once.
//
// afero's ways are kept: creating a file creates the directories it's in;
// Remove removes a directory that isn't empty; MkdirAll succeeds where
// a file has the name; and a directory's Size is 42.
class MemoryFs final : public Fs, public Lstater {
public:
    MemoryFs();
    ~MemoryFs() override;

    std::pair<ptr<File>, error> Create(std::string_view name) override;
    error Mkdir(std::string_view name, fs::FileMode perm) override;
    error MkdirAll(std::string_view path, fs::FileMode perm) override;
    std::pair<ptr<File>, error> Open(std::string_view name) override;
    std::pair<ptr<File>, error> OpenFile(std::string_view name, int flag, fs::FileMode perm) override;
    error Remove(std::string_view name) override;
    error RemoveAll(std::string_view path) override;
    error Rename(std::string_view oldname, std::string_view newname) override;
    std::pair<ptr<fs::FileInfo>, error> Stat(std::string_view name) override;
    string Name() const override { return string::fromStatic("MemoryFs"); }
    error Chmod(std::string_view name, fs::FileMode mode) override;
    error Chown(std::string_view name, int uid, int gid) override;
    error Chtimes(std::string_view name, time::Time atime, time::Time mtime) override;
    // Stat, and false: a MemoryFs has no links.
    std::tuple<ptr<fs::FileInfo>, bool, error> LstatIfPossible(std::string_view name) override;

private:
    friend struct detail::memoryFs;
    std::unique_ptr<detail::memoryFs> m_;
};

// vfs.NewMemoryFs
ptr<Fs> NewMemoryFs();

// vfs.BasePathFs: the tree at a directory of another Fs. Names are joined
// to the directory, and a name that leads out of it doesn't exist. Errors
// can show the full names.
class BasePathFs final : public Fs, public Symlinker {
public:
    BasePathFs(ptr<Fs> source, std::string_view path);

    // The name in the source Fs: name joined to the base path, cleaned; or
    // name and fs::ErrNotExist if it's outside.
    std::pair<string, error> RealPath(std::string_view name) const;

    std::pair<ptr<File>, error> Create(std::string_view name) override;
    error Mkdir(std::string_view name, fs::FileMode perm) override;
    error MkdirAll(std::string_view path, fs::FileMode perm) override;
    std::pair<ptr<File>, error> Open(std::string_view name) override;
    std::pair<ptr<File>, error> OpenFile(std::string_view name, int flag, fs::FileMode perm) override;
    error Remove(std::string_view name) override;
    error RemoveAll(std::string_view path) override;
    error Rename(std::string_view oldname, std::string_view newname) override;
    std::pair<ptr<fs::FileInfo>, error> Stat(std::string_view name) override;
    string Name() const override { return string::fromStatic("BasePathFs"); }
    error Chmod(std::string_view name, fs::FileMode mode) override;
    error Chown(std::string_view name, int uid, int gid) override;
    error Chtimes(std::string_view name, time::Time atime, time::Time mtime) override;
    std::tuple<ptr<fs::FileInfo>, bool, error> LstatIfPossible(std::string_view name) override;
    error SymlinkIfPossible(std::string_view oldname, std::string_view newname) override;
    std::pair<string, error> ReadlinkIfPossible(std::string_view name) override;

private:
    friend string FullBaseFsPath(const BasePathFs& basePathFs, std::string_view relativePath);
    ptr<Fs> source_;
    string path_;
};

// vfs.NewBasePathFs
ptr<Fs> NewBasePathFs(ptr<Fs> source, std::string_view path);

// vfs.FullBaseFsPath: relativePath joined to the base paths of basePathFs
// and of the BasePathFss it wraps.
string FullBaseFsPath(const BasePathFs& basePathFs, std::string_view relativePath);

// vfs.ReadOnlyFs: another Fs without its writes, which fail with
// syscall::EPERM_.
class ReadOnlyFs final : public Fs, public Symlinker {
public:
    explicit ReadOnlyFs(ptr<Fs> source) : source_(std::move(source)) {}

    // vfs::ReadDir of the source.
    std::pair<std::vector<ptr<fs::FileInfo>>, error> ReadDir(std::string_view name);

    std::pair<ptr<File>, error> Create(std::string_view name) override;
    error Mkdir(std::string_view name, fs::FileMode perm) override;
    error MkdirAll(std::string_view path, fs::FileMode perm) override;
    std::pair<ptr<File>, error> Open(std::string_view name) override;
    // EPERM for a flag that writes, creates, appends or truncates.
    std::pair<ptr<File>, error> OpenFile(std::string_view name, int flag, fs::FileMode perm) override;
    error Remove(std::string_view name) override;
    error RemoveAll(std::string_view path) override;
    error Rename(std::string_view oldname, std::string_view newname) override;
    std::pair<ptr<fs::FileInfo>, error> Stat(std::string_view name) override;
    string Name() const override { return string::fromStatic("ReadOnlyFilter"); }
    error Chmod(std::string_view name, fs::FileMode mode) override;
    error Chown(std::string_view name, int uid, int gid) override;
    error Chtimes(std::string_view name, time::Time atime, time::Time mtime) override;
    std::tuple<ptr<fs::FileInfo>, bool, error> LstatIfPossible(std::string_view name) override;
    error SymlinkIfPossible(std::string_view oldname, std::string_view newname) override;
    std::pair<string, error> ReadlinkIfPossible(std::string_view name) override;

private:
    ptr<Fs> source_;
};

// vfs.NewReadOnlyFs
ptr<Fs> NewReadOnlyFs(ptr<Fs> source);

// vfs.RegexpFs: another Fs with only the files (not directories) whose
// names match a regular expression; the others fail with
// syscall::ENOENT_, and reading a directory leaves them out.
class RegexpFs final : public Fs {
public:
    // With a nil re, every file matches.
    RegexpFs(ptr<Fs> source, ptr<regexp::Regexp> re) : re_(std::move(re)), source_(std::move(source)) {}

    std::pair<ptr<File>, error> Create(std::string_view name) override;
    error Mkdir(std::string_view name, fs::FileMode perm) override;
    error MkdirAll(std::string_view path, fs::FileMode perm) override;
    std::pair<ptr<File>, error> Open(std::string_view name) override;
    std::pair<ptr<File>, error> OpenFile(std::string_view name, int flag, fs::FileMode perm) override;
    error Remove(std::string_view name) override;
    error RemoveAll(std::string_view path) override;
    error Rename(std::string_view oldname, std::string_view newname) override;
    std::pair<ptr<fs::FileInfo>, error> Stat(std::string_view name) override;
    string Name() const override { return string::fromStatic("RegexpFs"); }
    error Chmod(std::string_view name, fs::FileMode mode) override;
    error Chown(std::string_view name, int uid, int gid) override;
    error Chtimes(std::string_view name, time::Time atime, time::Time mtime) override;

private:
    error matchesName(std::string_view name) const;
    error dirOrMatches(std::string_view name) const;
    ptr<regexp::Regexp> re_;
    ptr<Fs> source_;
};

// vfs.NewRegexpFs
ptr<Fs> NewRegexpFs(ptr<Fs> source, ptr<regexp::Regexp> re);

// vfs.CopyOnWriteFs: a union of a base Fs, which it only reads, and a
// layer over it, which it writes. Changing a file of the base, including
// its mode, owner or times, copies it to the layer first. Renaming or
// removing a file that's only in the base fails with syscall::EPERM_.
// Directories in both are read as one; only Open reads directories, not
// OpenFile.
class CopyOnWriteFs final : public Fs, public Symlinker {
public:
    CopyOnWriteFs(ptr<Fs> base, ptr<Fs> layer) : base_(std::move(base)), layer_(std::move(layer)) {}

    std::pair<ptr<File>, error> Create(std::string_view name) override;
    error Mkdir(std::string_view name, fs::FileMode perm) override;
    error MkdirAll(std::string_view path, fs::FileMode perm) override;
    std::pair<ptr<File>, error> Open(std::string_view name) override;
    std::pair<ptr<File>, error> OpenFile(std::string_view name, int flag, fs::FileMode perm) override;
    error Remove(std::string_view name) override;
    error RemoveAll(std::string_view path) override;
    error Rename(std::string_view oldname, std::string_view newname) override;
    std::pair<ptr<fs::FileInfo>, error> Stat(std::string_view name) override;
    string Name() const override { return string::fromStatic("CopyOnWriteFs"); }
    error Chmod(std::string_view name, fs::FileMode mode) override;
    error Chown(std::string_view name, int uid, int gid) override;
    error Chtimes(std::string_view name, time::Time atime, time::Time mtime) override;
    std::tuple<ptr<fs::FileInfo>, bool, error> LstatIfPossible(std::string_view name) override;
    error SymlinkIfPossible(std::string_view oldname, std::string_view newname) override;
    std::pair<string, error> ReadlinkIfPossible(std::string_view name) override;

private:
    std::pair<bool, error> isBaseFile(std::string_view name);
    ptr<Fs> base_;
    ptr<Fs> layer_;
};

// vfs.NewCopyOnWriteFs
ptr<Fs> NewCopyOnWriteFs(ptr<Fs> base, ptr<Fs> layer);

// vfs.CacheOnReadFs: a union of a base Fs and a layer that caches the
// files read from it. A file in the layer is used for cacheTime after its
// modification, and then again only if the base's isn't newer; a zero
// cacheTime uses it forever. Writes go to the base and to the layer: wrap
// the base in a ReadOnlyFs to keep it as it is.
class CacheOnReadFs final : public Fs {
public:
    CacheOnReadFs(ptr<Fs> base, ptr<Fs> layer, time::Duration cacheTime)
        : base_(std::move(base)), layer_(std::move(layer)), cacheTime_(cacheTime) {}

    std::pair<ptr<File>, error> Create(std::string_view name) override;
    error Mkdir(std::string_view name, fs::FileMode perm) override;
    error MkdirAll(std::string_view path, fs::FileMode perm) override;
    std::pair<ptr<File>, error> Open(std::string_view name) override;
    std::pair<ptr<File>, error> OpenFile(std::string_view name, int flag, fs::FileMode perm) override;
    error Remove(std::string_view name) override;
    error RemoveAll(std::string_view path) override;
    error Rename(std::string_view oldname, std::string_view newname) override;
    std::pair<ptr<fs::FileInfo>, error> Stat(std::string_view name) override;
    string Name() const override { return string::fromStatic("CacheOnReadFs"); }
    error Chmod(std::string_view name, fs::FileMode mode) override;
    error Chown(std::string_view name, int uid, int gid) override;
    error Chtimes(std::string_view name, time::Time atime, time::Time mtime) override;

private:
    enum cacheState { cacheMiss, cacheStale, cacheHit, cacheLocal };
    std::tuple<cacheState, ptr<fs::FileInfo>, error> cacheStatus(std::string_view name);
    ptr<Fs> base_;
    ptr<Fs> layer_;
    time::Duration cacheTime_;
};

// vfs.NewCacheOnReadFs
ptr<Fs> NewCacheOnReadFs(ptr<Fs> base, ptr<Fs> layer, time::Duration cacheTime);

// vfs.DirsMerger: how a UnionFile merges the entries of its layer's and
// its base's directories.
using DirsMerger = std::function<std::pair<std::vector<ptr<fs::FileInfo>>, error>(
    std::vector<ptr<fs::FileInfo>> lofi, std::vector<ptr<fs::FileInfo>> bofi)>;

// vfs.UnionFile: a file of CopyOnWriteFs and CacheOnReadFs, open in their
// layer and their base. It writes to both, the layer first, and reads from
// the layer, moving the base's offset along. It reads a directory as the
// layer's and the base's entries merged by Merger; by default, the layer's
// and those of the base that the layer doesn't have, sorted by name.
class UnionFile final : public File {
public:
    UnionFile(ptr<File> base, ptr<File> layer, DirsMerger merger = nullptr)
        : Base(std::move(base)), Layer(std::move(layer)), Merger(std::move(merger)) {}

    ptr<File> Base;
    ptr<File> Layer;
    DirsMerger Merger;

    // Closes the base, then the layer, so that the layer's file is the
    // newer.
    error Close() override;
    std::pair<isize, error> Read(std::span<byte> p) override;
    std::pair<isize, error> ReadAt(std::span<byte> p, int64 off) override;
    std::pair<int64, error> Seek(int64 offset, int whence) override;
    std::pair<isize, error> Write(std::span<const byte> p) override;
    std::pair<isize, error> WriteAt(std::span<const byte> p, int64 off) override;
    string Name() const override;
    std::pair<std::vector<ptr<fs::FileInfo>>, error> Readdir(int count) override;
    std::pair<std::vector<string>, error> Readdirnames(int n) override;
    std::pair<ptr<fs::FileInfo>, error> Stat() override;
    error Sync() override;
    error Truncate(int64 size) override;
    std::pair<isize, error> WriteString(std::string_view s) override;

private:
    std::size_t off_ = 0;
    std::vector<ptr<fs::FileInfo>> files_;
};

// vfs.IOFS: an Fs as an fs::FS (with fs::StatFS, ReadFileFS, ReadDirFS,
// SubFS and GlobFS), for io/fs's functions and http::FileServerFS. Names
// must be valid for fs::FS (fs::ValidPath).
class IOFS final : public fs::StatFS, public fs::ReadFileFS, public fs::ReadDirFS, public fs::SubFS, public fs::GlobFS {
public:
    explicit IOFS(ptr<vfs::Fs> fsys) : Fs(std::move(fsys)) {}

    ptr<vfs::Fs> Fs;

    std::pair<ptr<fs::File>, error> Open(std::string_view name) override;
    std::pair<ptr<fs::FileInfo>, error> Stat(std::string_view name) override;
    std::pair<slice<byte>, error> ReadFile(std::string_view name) override;
    std::pair<std::vector<ptr<fs::DirEntry>>, error> ReadDir(std::string_view name) override;
    // A BasePathFs of the Fs at dir.
    std::pair<ptr<fs::FS>, error> Sub(std::string_view dir) override;
    std::pair<std::vector<string>, error> Glob(std::string_view pattern) override;
};

// vfs.NewIOFS
ptr<IOFS> NewIOFS(ptr<Fs> fsys);

// vfs.FromIOFS: an fs::FS as a read-only Fs. What it can't do fails with
// a PathError wrapping fs::ErrPermission: put a CopyOnWriteFs over it to
// write.
class FromIOFS final : public Fs {
public:
    explicit FromIOFS(ptr<fs::FS> fsys) : FS(std::move(fsys)) {}

    ptr<fs::FS> FS;

    std::pair<ptr<File>, error> Create(std::string_view name) override;
    error Mkdir(std::string_view name, fs::FileMode perm) override;
    error MkdirAll(std::string_view path, fs::FileMode perm) override;
    std::pair<ptr<File>, error> Open(std::string_view name) override;
    // Open.
    std::pair<ptr<File>, error> OpenFile(std::string_view name, int flag, fs::FileMode perm) override;
    error Remove(std::string_view name) override;
    error RemoveAll(std::string_view path) override;
    error Rename(std::string_view oldname, std::string_view newname) override;
    std::pair<ptr<fs::FileInfo>, error> Stat(std::string_view name) override;
    string Name() const override { return string::fromStatic("fromiofs"); }
    error Chmod(std::string_view name, fs::FileMode mode) override;
    error Chown(std::string_view name, int uid, int gid) override;
    error Chtimes(std::string_view name, time::Time atime, time::Time mtime) override;
};

// ---------------------------------------------------------------------------
// Functions over any Fs, after io/ioutil, path/filepath and os.

// vfs.ReadDir: the FileInfos of the directory's entries, sorted by name.
std::pair<std::vector<ptr<fs::FileInfo>>, error> ReadDir(Fs& fsys, std::string_view dirname);
// vfs.ReadFile: the contents of the named file.
std::pair<slice<byte>, error> ReadFile(Fs& fsys, std::string_view filename);
// vfs.ReadAll: io::ReadAll.
std::pair<slice<byte>, error> ReadAll(io::Reader& r);
// vfs.WriteFile: writes data to the named file, creating it with perm if
// it doesn't exist, or truncating it.
error WriteFile(Fs& fsys, std::string_view filename, std::span<const byte> data, fs::FileMode perm);
// vfs.TempFile: creates a file in dir (os::TempDir() if empty), named
// after pattern with a random string replacing its last "*", or added at
// its end; opens it for reading and writing.
std::pair<ptr<File>, error> TempFile(Fs& fsys, std::string_view dir, std::string_view pattern);
// vfs.TempDir: likewise, creates a directory, mode 0700, whose name begins
// with prefix, and returns its name.
std::pair<string, error> TempDir(Fs& fsys, std::string_view dir, std::string_view prefix);
// vfs.Walk: filepath::Walk on fsys: walks the tree at root in lexical
// order, calling walkFn for each file and directory, with their Lstat
// FileInfos where fsys is a Lstater.
error Walk(Fs& fsys, std::string_view root, const filepath::WalkFunc& walkFn);
// vfs.Glob: filepath::Glob on fsys: the names that match pattern;
// path::ErrBadPattern if it's malformed.
std::pair<std::vector<string>, error> Glob(Fs& fsys, std::string_view pattern);

// vfs.WriteReader: writes what r reads to the file at path, creating its
// directories.
error WriteReader(Fs& fsys, std::string_view path, io::Reader& r);
// vfs.SafeWriteReader: as WriteReader, but fails if the file exists.
error SafeWriteReader(Fs& fsys, std::string_view path, io::Reader& r);
// vfs.GetTempDir: os::TempDir() with a trailing slash; with subPath (put
// through UnicodeSanitize), that directory under it, created with mode
// 0777. Panics if it can't create it.
string GetTempDir(Fs& fsys, std::string_view subPath);
// vfs.UnicodeSanitize: s with only letters, digits, marks and . / \ _ - %
// space and #.
string UnicodeSanitize(std::string_view s);
// vfs.FileContainsBytes: whether the named file contains subslice.
std::pair<bool, error> FileContainsBytes(Fs& fsys, std::string_view filename, std::span<const byte> subslice);
// vfs.FileContainsAnyBytes: whether the named file contains any of
// subslices.
std::pair<bool, error> FileContainsAnyBytes(Fs& fsys, std::string_view filename,
                                            std::span<const std::span<const byte>> subslices);
// vfs.DirExists: whether path exists and is a directory.
std::pair<bool, error> DirExists(Fs& fsys, std::string_view path);
// vfs.IsDir: whether path is a directory; an error if it doesn't exist.
std::pair<bool, error> IsDir(Fs& fsys, std::string_view path);
// vfs.IsEmpty: whether path is an empty file or directory; an error if it
// doesn't exist.
std::pair<bool, error> IsEmpty(Fs& fsys, std::string_view path);
// vfs.Exists: whether path exists.
std::pair<bool, error> Exists(Fs& fsys, std::string_view path);

// vfs.Vfs (afero's Afero): an Fs with the functions above as methods.
// Its -> calls the Fs's own:
//
//     vfs::Vfs app{vfs::NewMemoryFs()};
//     app->MkdirAll("/srv", 0755);
//     app.WriteFile("/srv/index.html", as_bytes("hello"), 0644);
struct Vfs {
    ptr<vfs::Fs> Fs;

    vfs::Fs* operator->() const noexcept { return Fs.get(); }

    std::pair<std::vector<ptr<fs::FileInfo>>, error> ReadDir(std::string_view dirname) const {
        return vfs::ReadDir(*Fs, dirname);
    }
    std::pair<slice<byte>, error> ReadFile(std::string_view filename) const { return vfs::ReadFile(*Fs, filename); }
    error WriteFile(std::string_view filename, std::span<const byte> data, fs::FileMode perm) const {
        return vfs::WriteFile(*Fs, filename, data, perm);
    }
    std::pair<ptr<File>, error> TempFile(std::string_view dir, std::string_view pattern) const {
        return vfs::TempFile(*Fs, dir, pattern);
    }
    std::pair<string, error> TempDir(std::string_view dir, std::string_view prefix) const {
        return vfs::TempDir(*Fs, dir, prefix);
    }
    error Walk(std::string_view root, const filepath::WalkFunc& walkFn) const { return vfs::Walk(*Fs, root, walkFn); }
    error WriteReader(std::string_view path, io::Reader& r) const { return vfs::WriteReader(*Fs, path, r); }
    error SafeWriteReader(std::string_view path, io::Reader& r) const { return vfs::SafeWriteReader(*Fs, path, r); }
    string GetTempDir(std::string_view subPath) const { return vfs::GetTempDir(*Fs, subPath); }
    std::pair<bool, error> FileContainsBytes(std::string_view filename, std::span<const byte> subslice) const {
        return vfs::FileContainsBytes(*Fs, filename, subslice);
    }
    std::pair<bool, error> FileContainsAnyBytes(std::string_view filename,
                                                std::span<const std::span<const byte>> subslices) const {
        return vfs::FileContainsAnyBytes(*Fs, filename, subslices);
    }
    std::pair<bool, error> DirExists(std::string_view path) const { return vfs::DirExists(*Fs, path); }
    std::pair<bool, error> IsDir(std::string_view path) const { return vfs::IsDir(*Fs, path); }
    std::pair<bool, error> IsEmpty(std::string_view path) const { return vfs::IsEmpty(*Fs, path); }
    std::pair<bool, error> Exists(std::string_view path) const { return vfs::Exists(*Fs, path); }
};

}  // namespace gopp::vfs
