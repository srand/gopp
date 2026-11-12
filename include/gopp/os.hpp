// The os package: files and directories, the standard streams, arguments,
// exit and the environment.
//
//     auto [data, err] = os::ReadFile("config.json");
//     if (os::IsNotExist(err)) ...
//
//     auto [f, err] = os::Create("out.txt");
//     if (err) return err;
//     fmt::Fprintln(*f, "hello");
//     err = f->Close();
//
// Calls on regular files and directories block their thread, which hands
// its processor to another thread meanwhile, as Go does around system
// calls. Pipes, and descriptors that NewFile is given in non-blocking mode,
// use the network poller: a read waiting for data parks only its fiber,
// and deadlines apply.
#pragma once

// The module interfaces (modules/) import these instead.
#ifndef GOPP_MODULE
#include <gopp/base.hpp>
#include <gopp/errors.hpp>
#include <gopp/io.hpp>
#include <gopp/io/fs.hpp>
#include <gopp/ptr.hpp>
#include <gopp/slice.hpp>
#include <gopp/string.hpp>
#include <gopp/time.hpp>

#include <memory>
#include <span>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>
#endif

namespace gopp::os {

// The types of io/fs, under their os names.
using fs::DirEntry;
using fs::FileInfo;
using fs::FileMode;
using fs::PathError;

using fs::ModeAppend;
using fs::ModeCharDevice;
using fs::ModeDevice;
using fs::ModeDir;
using fs::ModeExclusive;
using fs::ModeIrregular;
using fs::ModeNamedPipe;
using fs::ModePerm;
using fs::ModeSetgid;
using fs::ModeSetuid;
using fs::ModeSocket;
using fs::ModeSticky;
using fs::ModeSymlink;
using fs::ModeTemporary;
using fs::ModeType;

// Errors.
using fs::ErrClosed;      // "file already closed"
using fs::ErrExist;       // "file already exists"
using fs::ErrInvalid;     // "invalid argument"
using fs::ErrNotExist;    // "file does not exist"
using fs::ErrPermission;  // "permission denied"
// os.ErrNoDeadline: "file type does not support deadline".
extern const error ErrNoDeadline;
// os.ErrDeadlineExceeded: "i/o timeout"; Timeout() is true. Also
// net::ErrDeadlineExceeded.
extern const error ErrDeadlineExceeded;

// os.LinkError: an error of a call with two paths, such as Rename.
struct LinkError : error_interface {
    LinkError(string op, string old_path, string new_path, gopp::error err)
        : Op(std::move(op)), Old(std::move(old_path)), New(std::move(new_path)), Err(std::move(err)) {}
    string Op;
    string Old;
    string New;
    gopp::error Err;
    string Error() const override;
    const error_interface* Unwrap() const noexcept override { return Err.get(); }
};

// os.SyscallError: an error of a system call.
struct SyscallError : error_interface {
    SyscallError(string syscall, gopp::error err) : Syscall(std::move(syscall)), Err(std::move(err)) {}
    string Syscall;
    gopp::error Err;
    string Error() const override;
    const error_interface* Unwrap() const noexcept override { return Err.get(); }
    bool Timeout() const noexcept override { return Err && Err->Timeout(); }
};

// os.NewSyscallError: a SyscallError, or nil if err is nil.
gopp::error NewSyscallError(string syscall, gopp::error err);

// Whether err, or the error of a PathError, LinkError or SyscallError
// err is, means that a file exists, doesn't exist, or that permission was
// denied. errors::Is(err, fs::ErrNotExist) is the newer way: it also looks
// through other wrapping.
bool IsExist(const gopp::error& err) noexcept;
bool IsNotExist(const gopp::error& err) noexcept;
bool IsPermission(const gopp::error& err) noexcept;
// os.IsTimeout
bool IsTimeout(const gopp::error& err) noexcept;

// The flags of OpenFile: one of O_RDONLY_, O_WRONLY_ and O_RDWR_, or'ed
// with any of the others. Go's names with a trailing _, because <fcntl.h>
// defines Go's names as macros. Linux's values.
inline constexpr int O_RDONLY_ = 00;         // open the file read-only
inline constexpr int O_WRONLY_ = 01;         // open the file write-only
inline constexpr int O_RDWR_ = 02;           // open the file read-write
inline constexpr int O_CREATE_ = 0100;       // create a new file if none exists
inline constexpr int O_EXCL_ = 0200;         // with O_CREATE_, the file must not exist
inline constexpr int O_TRUNC_ = 01000;       // truncate a regular writable file when opened
inline constexpr int O_APPEND_ = 02000;      // append data to the file when writing
inline constexpr int O_SYNC_ = 04010000;     // open for synchronous I/O

inline constexpr char PathSeparator = '/';
inline constexpr char PathListSeparator = ':';
inline constexpr bool IsPathSeparator(char c) noexcept { return c == '/'; }

// os.DevNull
inline constexpr std::string_view DevNull = "/dev/null";

namespace detail {
struct file_impl;
}

// os.File: an open file descriptor. The descriptor is closed by Close, or
// when the last ptr to the File goes away.
//
// Its methods are safe to call from several fibers at once. Reads are
// serialized with each other, and writes with each other, but a read and a
// write can be in flight together.
class File final : public fs::ReadDirFile,
                   public io::Writer,
                   public io::Seeker,
                   public io::ReaderAt,
                   public io::WriterAt {
public:
    explicit File(std::unique_ptr<detail::file_impl> impl) noexcept;
    ~File() override;
    File(const File&) = delete;
    File& operator=(const File&) = delete;

    // The name the file was opened with.
    const string& Name() const noexcept;

    // Reads up to p.size() bytes; at the end of the file, returns 0 and
    // io::EOF_.
    std::pair<isize, error> Read(std::span<byte> p) override;
    // Reads p.size() bytes at offset off, or fewer and an error: io::EOF_
    // at the end of the file. Doesn't change the offset of Read and Write.
    std::pair<isize, error> ReadAt(std::span<byte> p, int64 off) override;
    // Writes all of p, or returns the count written and an error.
    std::pair<isize, error> Write(std::span<const byte> p) override;
    std::pair<isize, error> WriteString(std::string_view s);
    // Writes p at offset off. Fails for files opened with O_APPEND.
    std::pair<isize, error> WriteAt(std::span<const byte> p, int64 off) override;
    // Sets the offset of the next Read or Write: relative to the start of
    // the file (io::SeekStart), the current offset (io::SeekCurrent) or the
    // end (io::SeekEnd).
    std::pair<int64, error> Seek(int64 offset, int whence) override;

    // Closes the file. A second Close returns an error that is ErrClosed.
    error Close() override;

    std::pair<ptr<FileInfo>, error> Stat() override;
    // Commits the file's contents to stable storage (fsync).
    error Sync();
    error Truncate(int64 size);
    error Chmod(FileMode mode);
    error Chown(int uid, int gid);
    // Makes the file, a directory, the working directory.
    error Chdir();

    // Reads up to n entries of the directory (all if n <= 0), in directory
    // order, without "." and "..". With n > 0, at the end it returns no
    // entries and io::EOF_. The FileInfos of Readdir are Lstat's.
    std::pair<std::vector<ptr<DirEntry>>, error> ReadDir(int n) override;
    std::pair<std::vector<ptr<FileInfo>>, error> Readdir(int n);
    std::pair<std::vector<string>, error> Readdirnames(int n);

    // Deadlines for the reads and writes of pipes and other pollable
    // files; a zero Time means none. Fails with ErrNoDeadline for regular
    // files. Reads and writes after the deadline fail with
    // ErrDeadlineExceeded.
    error SetDeadline(time::Time t);
    error SetReadDeadline(time::Time t);
    error SetWriteDeadline(time::Time t);

    // The file descriptor, valid until Close. As in Go, a pollable file's
    // descriptor is put in blocking mode first, so that it can be used
    // directly, which turns off its deadlines.
    uintptr Fd();

private:
    friend struct detail::file_impl;
    error wrap_err(std::string_view op, error err) const;
    std::unique_ptr<detail::file_impl> f_;
};

// os.Stdin, os.Stdout, os.Stderr: files on descriptors 0, 1 and 2.
extern File& Stdin;
extern File& Stdout;
extern File& Stderr;

// os.NewFile: a File for the open descriptor fd, or nullptr if fd < 0. If
// fd is in non-blocking mode, the File uses the poller.
ptr<File> NewFile(uintptr fd, string name);

// os.Open: opens the file for reading.
std::pair<ptr<File>, error> Open(std::string_view name);
// os.Create: creates or truncates the file, mode 0666 (before umask), for
// reading and writing.
std::pair<ptr<File>, error> Create(std::string_view name);
// os.OpenFile: opens with flag (O_RDONLY_ and friends) and, when creating
// the file, mode perm (before umask).
std::pair<ptr<File>, error> OpenFile(std::string_view name, int flag, FileMode perm);

// os.ReadFile: the contents of the file.
std::pair<bytes, error> ReadFile(std::string_view name);
// os.WriteFile: writes data to the file, creating it with perm (before
// umask) or truncating it.
error WriteFile(std::string_view name, std::span<const byte> data, FileMode perm);
error WriteFile(std::string_view name, std::string_view data, FileMode perm);

// os.Stat: the FileInfo of the file, following symbolic links.
std::pair<ptr<FileInfo>, error> Stat(std::string_view name);
// os.Lstat: like Stat, but of a symbolic link itself.
std::pair<ptr<FileInfo>, error> Lstat(std::string_view name);
// os.SameFile: whether the FileInfos, from Stat or Lstat, are of the same
// file.
bool SameFile(const ptr<FileInfo>& fi1, const ptr<FileInfo>& fi2) noexcept;

// os.ReadDir: the entries of the directory, sorted by name.
std::pair<std::vector<ptr<DirEntry>>, error> ReadDir(std::string_view name);

// os.Mkdir: creates the directory with perm (before umask).
error Mkdir(std::string_view name, FileMode perm);
// os.MkdirAll: creates the directory and any parents that don't exist.
// Nil if it's already a directory.
error MkdirAll(std::string_view path, FileMode perm);
// os.Remove: removes the file or empty directory.
error Remove(std::string_view name);
// os.RemoveAll: removes path and anything it contains. Nil if path doesn't
// exist.
error RemoveAll(std::string_view path);
// os.Rename: moves oldpath to newpath, replacing newpath unless it's a
// directory.
error Rename(std::string_view oldpath, std::string_view newpath);
// os.Truncate: changes the size of the file.
error Truncate(std::string_view name, int64 size);

// os.Chmod: the mode bits of the file: permissions, ModeSetuid, ModeSetgid
// and ModeSticky.
error Chmod(std::string_view name, FileMode mode);
// os.Chown, os.Lchown: changes the owner; -1 keeps one.
error Chown(std::string_view name, int uid, int gid);
error Lchown(std::string_view name, int uid, int gid);
// os.Chtimes: the access and modification times; a zero Time keeps one.
error Chtimes(std::string_view name, time::Time atime, time::Time mtime);

// os.Link: newname as a hard link to oldname.
error Link(std::string_view oldname, std::string_view newname);
// os.Symlink: newname as a symbolic link to oldname.
error Symlink(std::string_view oldname, std::string_view newname);
// os.Readlink: the destination of the symbolic link.
std::pair<string, error> Readlink(std::string_view name);

// os.TempDir: $TMPDIR, or /tmp.
string TempDir();
// os.CreateTemp: creates a file in dir (TempDir() if empty) named after
// pattern, with its last "*" replaced by a random string (or the string
// appended), open for reading and writing, mode 0600.
std::pair<ptr<File>, error> CreateTemp(std::string_view dir, std::string_view pattern);
// os.MkdirTemp: likewise, a directory, mode 0700, and returns its name.
std::pair<string, error> MkdirTemp(std::string_view dir, std::string_view pattern);

// os.Getwd: the working directory.
std::pair<string, error> Getwd();
// os.Chdir: changes the working directory.
error Chdir(std::string_view dir);

// os.Pipe: a connected pair of Files; reads from r return the bytes
// written to w.
std::tuple<ptr<File>, ptr<File>, error> Pipe();

// os.Getpagesize
int Getpagesize() noexcept;

// os.Args, as given to os::SetArgs (flag::Parse calls it).
const std::vector<string>& Args();
void SetArgs(int argc, char** argv);

// os.Getenv
string Getenv(std::string_view key);

// os.Exit: exits at once, without running destructors.
[[noreturn]] void Exit(int code);

}  // namespace gopp::os
