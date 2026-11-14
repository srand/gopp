// The io/fs package: the interfaces and types of file systems, which the
// os package implements, and functions over any FS.
//
//     auto [info, err] = os::Stat("go.mod");
//     if (errors::Is(err, fs::ErrNotExist)) ...
//     fmt::Println("{} {}", info->Mode(), info->Size());   // -rw-r--r-- 42
//
//     auto fsys = os::DirFS("/etc");
//     auto [data, err] = fs::ReadFile(*fsys, "hostname");
//     fs::WalkDir(*fsys, ".", [](std::string_view path, const ptr<fs::DirEntry>& d, error err) -> error { ... });
#pragma once

// The module interfaces (modules/) import these instead.
#ifndef GOPP_MODULE
#include <gopp/base.hpp>
#include <gopp/errors.hpp>
#include <gopp/io.hpp>
#include <gopp/ptr.hpp>
#include <gopp/slice.hpp>
#include <gopp/string.hpp>
#include <gopp/time.hpp>

#include <format>
#include <functional>
#include <ostream>
#include <string_view>
#include <utility>
#include <vector>
#endif

namespace gopp::fs {

// fs.FileMode: a file's mode and permission bits. Its constants combine
// with each other and with permissions into FileModes, as in Go:
// ModeDir | 0755. It converts from uint32, and to it explicitly.
class FileMode {
public:
    constexpr FileMode(uint32 m = 0) noexcept : m_(m) {}
    constexpr explicit operator uint32() const noexcept { return m_; }
    // Whether any bit is set: if (mode & ModeSymlink) ...
    constexpr explicit operator bool() const noexcept { return m_ != 0; }

    // "drwxr-xr-x": the type letters set ("dalTLDpSugct?", or "-" for
    // none) and the permissions.
    string String() const;
    constexpr bool IsDir() const noexcept;
    constexpr bool IsRegular() const noexcept;
    constexpr FileMode Perm() const noexcept;
    constexpr FileMode Type() const noexcept;

    friend constexpr FileMode operator|(FileMode a, FileMode b) noexcept { return a.m_ | b.m_; }
    friend constexpr FileMode operator&(FileMode a, FileMode b) noexcept { return a.m_ & b.m_; }
    friend constexpr FileMode operator^(FileMode a, FileMode b) noexcept { return a.m_ ^ b.m_; }
    // Go's ^m; m & ~b is Go's m &^ b.
    friend constexpr FileMode operator~(FileMode a) noexcept { return ~a.m_; }
    constexpr FileMode& operator|=(FileMode b) noexcept { return m_ |= b.m_, *this; }
    constexpr FileMode& operator&=(FileMode b) noexcept { return m_ &= b.m_, *this; }
    friend constexpr bool operator==(FileMode a, FileMode b) noexcept { return a.m_ == b.m_; }

    friend std::ostream& operator<<(std::ostream& os, FileMode m) { return os << m.String().view(); }

private:
    uint32 m_;
};

inline constexpr FileMode ModeDir = 1u << 31;         // d: is a directory
inline constexpr FileMode ModeAppend = 1u << 30;      // a: append-only
inline constexpr FileMode ModeExclusive = 1u << 29;   // l: exclusive use
inline constexpr FileMode ModeTemporary = 1u << 28;   // T: temporary file; Plan 9 only
inline constexpr FileMode ModeSymlink = 1u << 27;     // L: symbolic link
inline constexpr FileMode ModeDevice = 1u << 26;      // D: device file
inline constexpr FileMode ModeNamedPipe = 1u << 25;   // p: named pipe (FIFO)
inline constexpr FileMode ModeSocket = 1u << 24;      // S: Unix domain socket
inline constexpr FileMode ModeSetuid = 1u << 23;      // u: setuid
inline constexpr FileMode ModeSetgid = 1u << 22;      // g: setgid
inline constexpr FileMode ModeCharDevice = 1u << 21;  // c: Unix character device, when ModeDevice is set
inline constexpr FileMode ModeSticky = 1u << 20;      // t: sticky
inline constexpr FileMode ModeIrregular = 1u << 19;   // ?: non-regular file; nothing else is known about this file
// The type bits.
inline constexpr FileMode ModeType =
    ModeDir | ModeSymlink | ModeNamedPipe | ModeSocket | ModeDevice | ModeCharDevice | ModeIrregular;
// The Unix permission bits.
inline constexpr FileMode ModePerm = 0777;

constexpr bool FileMode::IsDir() const noexcept { return (m_ & ModeDir.m_) != 0; }
constexpr bool FileMode::IsRegular() const noexcept { return (m_ & ModeType.m_) == 0; }
constexpr FileMode FileMode::Perm() const noexcept { return m_ & ModePerm.m_; }
constexpr FileMode FileMode::Type() const noexcept { return m_ & ModeType.m_; }

// The errors of file systems, which errors::Is matches with the
// syscall::Errno errors they stand for.
extern const error ErrInvalid;     // "invalid argument"
extern const error ErrPermission;  // "permission denied"
extern const error ErrExist;       // "file already exists"
extern const error ErrNotExist;    // "file does not exist"
extern const error ErrClosed;      // "file already closed"

// fs.FileInfo: describes a file, as Stat returns it.
struct FileInfo : public virtual object {
    // The base name of the file.
    virtual string Name() const = 0;
    // The length in bytes for regular files; system-dependent for others.
    virtual int64 Size() const = 0;
    virtual FileMode Mode() const = 0;
    virtual time::Time ModTime() const = 0;
    virtual bool IsDir() const { return Mode().IsDir(); }
    // The underlying data, or nullptr: for the os package's FileInfos, a
    // pointer to the struct stat that it was made from.
    virtual const void* Sys() const { return nullptr; }
};

// fs.DirEntry: an entry read from a directory.
struct DirEntry : public virtual object {
    // The base name of the file or subdirectory.
    virtual string Name() const = 0;
    virtual bool IsDir() const = 0;
    // The type bits of the entry's mode: Mode().Type().
    virtual FileMode Type() const = 0;
    // The FileInfo of the file or subdirectory, as of the call.
    virtual std::pair<ptr<FileInfo>, error> Info() const = 0;
};

// fs.File: an open file.
struct File : public io::Reader, public io::Closer {
    virtual std::pair<ptr<FileInfo>, error> Stat() = 0;
};

// fs.ReadDirFile: a directory file that reads its entries.
struct ReadDirFile : public File {
    // Reads up to n entries (all if n <= 0), in directory order. With
    // n > 0, at the end of the directory it returns no entries and
    // io::EOF_.
    virtual std::pair<std::vector<ptr<DirEntry>>, error> ReadDir(int n) = 0;
};

// fs.PathError: an error and the operation and path that caused it.
struct PathError : errorInterface {
    PathError(string op, string path, gopp::error err) : Op(std::move(op)), Path(std::move(path)), Err(std::move(err)) {}
    string Op;
    string Path;
    gopp::error Err;
    string Error() const override;
    const errorInterface* Unwrap() const noexcept override { return Err.get(); }
    bool Timeout() const noexcept override { return Err && Err->Timeout(); }
};

// fs.ValidPath: whether name is valid for fs.FS.Open: unrooted, slash
// separated, without "." or ".." elements or empty ones, or ".".
bool ValidPath(std::string_view name);

// fs.FS: a hierarchical file system. Names are slash-separated and
// unrooted, as ValidPath says: "dir/file.txt", or "." for the root.
struct FS : public virtual object {
    // Open: the named file; a *PathError with Op "open" if it can't be
    // opened (ErrInvalid for a name that isn't valid).
    virtual std::pair<ptr<File>, error> Open(std::string_view name) = 0;
};

// fs.ReadFileFS: an FS with its own ReadFile.
struct ReadFileFS : public virtual FS {
    virtual std::pair<slice<byte>, error> ReadFile(std::string_view name) = 0;
};

// fs.ReadDirFS: an FS with its own ReadDir, which sorts by name.
struct ReadDirFS : public virtual FS {
    virtual std::pair<std::vector<ptr<DirEntry>>, error> ReadDir(std::string_view name) = 0;
};

// fs.StatFS: an FS with its own Stat.
struct StatFS : public virtual FS {
    virtual std::pair<ptr<FileInfo>, error> Stat(std::string_view name) = 0;
};

// fs.SubFS: an FS with its own Sub.
struct SubFS : public virtual FS {
    virtual std::pair<ptr<FS>, error> Sub(std::string_view dir) = 0;
};

// fs.GlobFS: an FS with its own Glob.
struct GlobFS : public virtual FS {
    virtual std::pair<std::vector<string>, error> Glob(std::string_view pattern) = 0;
};

// fs.ReadLinkFS: an FS with symbolic links.
struct ReadLinkFS : public virtual FS {
    // ReadLink: the destination of the named link.
    virtual std::pair<string, error> ReadLink(std::string_view name) = 0;
    // Lstat: the FileInfo of the named file, not following a final link.
    virtual std::pair<ptr<FileInfo>, error> Lstat(std::string_view name) = 0;
};

// fs.ReadFile: the contents of the named file.
std::pair<slice<byte>, error> ReadFile(FS& fsys, std::string_view name);
// fs.ReadDir: the entries of the named directory, sorted by name.
std::pair<std::vector<ptr<DirEntry>>, error> ReadDir(FS& fsys, std::string_view name);
// fs.Stat: the FileInfo of the named file.
std::pair<ptr<FileInfo>, error> Stat(FS& fsys, std::string_view name);
// fs.Sub: the FS of the subtree at dir.
std::pair<ptr<FS>, error> Sub(const ptr<FS>& fsys, std::string_view dir);
// fs.Glob: the names that match pattern (path::Match), in lexical order
// per directory; path::ErrBadPattern if it's malformed.
std::pair<std::vector<string>, error> Glob(FS& fsys, std::string_view pattern);
// fs.ReadLink: a link's destination; ErrInvalid if fsys has no links.
std::pair<string, error> ReadLink(FS& fsys, std::string_view name);
// fs.Lstat: as Stat, not following a final link where fsys has them.
std::pair<ptr<FileInfo>, error> Lstat(FS& fsys, std::string_view name);

// fs.SkipDir and fs.SkipAll: what a WalkDirFunc returns to skip the
// current directory, or everything that's left.
extern const error SkipDir;
extern const error SkipAll;

// fs.WalkDirFunc: called by WalkDir for each file or directory (d), or
// with the error of reading a directory or of the root's Stat (then d is
// nil for the root).
using WalkDirFunc = std::function<error(std::string_view path, const ptr<DirEntry>& d, error err)>;

// fs.WalkDir: walks the tree at root in lexical order, calling fn for each
// file and directory.
error WalkDir(FS& fsys, std::string_view root, const WalkDirFunc& fn);

// fs.FileInfoToDirEntry: a DirEntry of info; nil for nil.
ptr<DirEntry> FileInfoToDirEntry(const ptr<FileInfo>& info);

// fs.FormatFileInfo: "-rw-r--r-- 100 1970-01-01 12:00:00 hello.go", with
// the modification time in UTC, as go++ formats times.
string FormatFileInfo(const FileInfo& info);

// fs.FormatDirEntry: "d subdir/" or "- hello.go".
string FormatDirEntry(const DirEntry& dir);

}  // namespace gopp::fs

template <>
struct std::formatter<gopp::fs::FileMode> : std::formatter<std::string_view> {
    auto format(const gopp::fs::FileMode& m, auto& ctx) const {
        return std::formatter<std::string_view>::format(m.String().view(), ctx);
    }
};
