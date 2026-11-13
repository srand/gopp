// The os package: files and directories, the standard streams, arguments,
// exit, the environment, processes and signals.
//
//     auto [data, err] = os::ReadFile("config.json");
//     if (os::IsNotExist(err)) ...
//
//     auto [f, err] = os::Create("out.txt");
//     if (err) return err;
//     fmt::Fprintln(*f, "hello");
//     err = f->Close();
//
// Calls on regular files and paths go through io_uring when the kernel has
// it and the thread has other fibers to run: the calling fiber parks until
// the call completes. Otherwise they block their thread, which hands its
// processor to another thread meanwhile, as Go does around system calls
// (GOPP_IOURING=0 always does that). Pipes, and descriptors that NewFile
// is given in non-blocking mode, use the network poller: a read waiting
// for data parks only its fiber, and deadlines apply.
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
#include <gopp/syscall.hpp>
#include <gopp/time.hpp>

#include <cstddef>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <ostream>
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
                   public io::WriterAt,
                   public io::ReaderFrom,
                   public io::WriterTo,
                   public syscall::Conn {
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
    // ReadFrom: io::ReaderFrom. From another file it copies with
    // copy_file_range(2), and from a TCP or Unix stream connection with
    // splice(2), unless the file is in append mode; otherwise through a
    // buffer. A LimitedReader of those works too.
    std::pair<int64, error> ReadFrom(io::Reader& r) override;
    // WriteTo: io::WriterTo. To a TCP or Unix stream connection it sends
    // with sendfile(2); otherwise through a buffer.
    std::pair<int64, error> WriteTo(io::Writer& w) override;
    // SyscallConn: the file's descriptor, for calls go++ doesn't wrap.
    std::pair<ptr<syscall::RawConn>, error> SyscallConn() override;

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

// os.Exit: exits at once, without running destructors.
[[noreturn]] void Exit(int code);

// ---------------------------------------------------------------------------
// The environment. It's the C library's: changes are seen by C code and
// inherited by processes started with the C library too.

// os.Getenv: the value of the variable, or "" if it isn't set.
string Getenv(std::string_view key);
// os.LookupEnv: the value and whether the variable is set.
std::pair<string, bool> LookupEnv(std::string_view key);
// os.Setenv. Fails with EINVAL for an empty key or one with '=' or a NUL,
// and a value with a NUL.
error Setenv(std::string_view key, std::string_view value);
// os.Unsetenv
// os.DirFS: the file system of the tree at dir, an fs::FS that also
// implements fs::StatFS, ReadFileFS, ReadDirFS and ReadLinkFS. Its names
// are ones fs::ValidPath accepts, joined to dir; like dir itself, they can
// follow symbolic links out of the tree (os.Root doesn't).
ptr<fs::FS> DirFS(std::string_view dir);
// os.CopyFS: copies fsys into the directory dir, creating it if needed:
// files with mode 0666 plus their permissions, before umask, directories
// with 0777, and symbolic links as they are. Files that already exist are
// an error (fs::ErrExist); so are files that are neither regular files,
// directories nor links.
error CopyFS(std::string_view dir, fs::FS& fsys);

error Unsetenv(std::string_view key);
// os.Clearenv: unsets all variables.
void Clearenv();
// os.Environ: the environment as "key=value" strings.
slice<string> Environ();
// os.Expand: replaces $var and ${var} in s with mapping(var), as a shell
// does; also $1, $*, $# and the like.
string Expand(std::string_view s, const std::function<string(std::string_view)>& mapping);
// os.ExpandEnv: Expand with Getenv.
string ExpandEnv(std::string_view s);

// os.UserHomeDir: $HOME.
std::pair<string, error> UserHomeDir();
// os.UserCacheDir: $XDG_CACHE_HOME, or $HOME/.cache.
std::pair<string, error> UserCacheDir();
// os.UserConfigDir: $XDG_CONFIG_HOME, or $HOME/.config.
std::pair<string, error> UserConfigDir();

// os.Hostname
std::pair<string, error> Hostname();
// os.Executable: the path of the running program (/proc/self/exe).
std::pair<string, error> Executable();

// The ids of the process and its user.
int Getpid() noexcept;
int Getppid() noexcept;
int Getuid() noexcept;
int Geteuid() noexcept;
int Getgid() noexcept;
int Getegid() noexcept;
// os.Getgroups: the supplementary groups.
std::pair<std::vector<int>, error> Getgroups();

// ---------------------------------------------------------------------------
// Signals

// The interface behind os::Signal values that aren't syscall::Signals:
// Go's os.Signal interface, for signal types of a program's own.
struct signal_interface : public virtual object {
    virtual string String() const = 0;
    virtual void Signal() const = 0;  // to tell signals from other Stringers
};

// os.Signal: an interface value, as gopp::error is. It's nil, a
// syscall::Signal (held as its number, without allocating), or a
// signal_interface. Values compare as Go compares interfaces: syscall
// signals by number, others by identity.
//
//     auto c = make_chan<os::Signal>(1);
//     signal::Notify(c, os::Interrupt, syscall::SIGTERM_);
//     os::Signal s = c.recv();
//     if (s == syscall::SIGTERM_) ...
class Signal {
public:
    constexpr Signal() noexcept = default;
    constexpr Signal(std::nullptr_t) noexcept {}
    constexpr Signal(syscall::Signal s) noexcept : num_(s.value), sys_(true) {}
    Signal(ptr<const signal_interface> p) noexcept : p_(std::move(p)) {}
    template <typename T>
        requires std::is_base_of_v<signal_interface, T>
    Signal(ptr<T> p) noexcept : p_(std::move(p)) {}

    // s.String(); "<nil>" for a nil Signal, as fmt prints it.
    string String() const;

    explicit operator bool() const noexcept { return sys_ || p_; }
    bool is_nil() const noexcept { return !*this; }

    // sig.(syscall.Signal): the signal and true if it holds a
    // syscall::Signal.
    std::pair<syscall::Signal, bool> as_syscall() const noexcept { return {syscall::Signal(num_), sys_}; }
    // The signal_interface it holds, if any.
    const signal_interface* get() const noexcept { return p_.get(); }

    friend bool operator==(const Signal& a, const Signal& b) noexcept {
        if (a.sys_ || b.sys_) return a.sys_ == b.sys_ && a.num_ == b.num_;
        return a.p_.get() == b.p_.get();
    }
    friend bool operator==(const Signal& a, std::nullptr_t) noexcept { return !a; }
    friend std::ostream& operator<<(std::ostream& os, const Signal& s) { return os << s.String(); }

private:
    ptr<const signal_interface> p_;
    int num_ = 0;
    bool sys_ = false;
};

// os.Interrupt and os.Kill: the signals every system has.
inline const Signal Interrupt{syscall::SIGINT_};
inline const Signal Kill{syscall::SIGKILL_};

// ---------------------------------------------------------------------------
// Processes

// os.ErrProcessDone: "os: process already finished".
extern const error ErrProcessDone;
// os.ErrNoHandle: "os: process handle unavailable".
extern const error ErrNoHandle;

// os.ProcessState: how a process ended, as Wait reports it.
class ProcessState final : public object {
public:
    ProcessState(int pid, syscall::WaitStatus status, const syscall::Rusage& rusage) noexcept
        : pid_(pid), status_(status), rusage_(rusage) {}

    // The process id of the exited process.
    int Pid() const noexcept { return pid_; }
    // Whether the program exited, rather than being killed by a signal.
    bool Exited() const noexcept { return status_.Exited(); }
    // Whether it exited with status 0.
    bool Success() const noexcept { return status_.ExitStatus() == 0; }
    // The exit code, or -1 if the process was killed by a signal. (Go's
    // ExitCode of a nil *ProcessState, -1, has no equivalent: check the
    // ptr.)
    int ExitCode() const noexcept { return status_.ExitStatus(); }
    // Go's Sys() any: the syscall::WaitStatus.
    syscall::WaitStatus Sys() const noexcept { return status_; }
    // Go's SysUsage() any: the resource usage of the process.
    const syscall::Rusage* SysUsage() const noexcept { return &rusage_; }
    // The user and system CPU time of the process and its children.
    time::Duration UserTime() const noexcept { return time::Duration(rusage_.Utime.Nano()); }
    time::Duration SystemTime() const noexcept { return time::Duration(rusage_.Stime.Nano()); }
    // "exit status 1", "signal: killed", "stop signal: stopped", ...
    string String() const;

private:
    int pid_;
    syscall::WaitStatus status_;
    syscall::Rusage rusage_;
};

namespace detail {
struct process_impl;
}

// os.Process: a process started by StartProcess or found by FindProcess.
// On Linux 5.4 and later it holds a pidfd, which Wait waits on through the
// network poller, parking only its fiber; otherwise Wait blocks its thread,
// which hands its processor to another, as Go does.
class Process final : public object {
public:
    // The process id; -1 after Release.
    int Pid = 0;

    // Waits for the process to exit and reports how it ended. The process
    // must be a child of this one. Releases the Process.
    std::pair<ptr<ProcessState>, error> Wait();
    // Sends sig (a syscall::Signal); ErrProcessDone if the process has
    // been waited for, or has exited.
    error Signal(const gopp::os::Signal& sig);
    // Sends SIGKILL: the process exits at once.
    error Kill();
    // Releases the process's resources, its pidfd, if Wait isn't called.
    error Release();
    // Calls f with the pidfd, which refers to the process until f
    // returns; ErrNoHandle without one.
    error WithHandle(const std::function<void(uintptr)>& f);

    ~Process() override;

    // Made by StartProcess and FindProcess.
    explicit Process(int pid, std::unique_ptr<detail::process_impl> impl) noexcept;

private:
    friend struct detail::process_impl;
    std::unique_ptr<detail::process_impl> p_;
};

// os.ProcAttr: the attributes of a new process.
struct ProcAttr {
    // The working directory of the process, if not empty.
    string Dir{};
    // The environment, as Environ returns it; nil for this process's.
    slice<string> Env{};
    // The files of the process: descriptors 0, 1, 2, ... A nil entry
    // closes that descriptor. Pollable files are put in blocking mode, so
    // that the process inherits them so, which turns off their deadlines.
    std::vector<ptr<File>> Files{};
    // Linux's attributes; nullopt for none.
    std::optional<syscall::SysProcAttr> Sys{};
};

// os.StartProcess: starts the program name with the arguments argv (argv[0]
// is the program's name, normally) and attr. The errors are PathErrors.
// It's safe in a program with many threads: the child is made with
// clone(CLONE_VM|CLONE_VFORK) and runs only system calls until exec, as in
// Go. os/exec is easier to use.
std::pair<ptr<Process>, error> StartProcess(std::string_view name, std::span<const string> argv, const ProcAttr& attr);

// os.FindProcess: a Process for pid, whether or not it exists. Signal(0)
// tells whether it does.
std::pair<ptr<Process>, error> FindProcess(int pid);

}  // namespace gopp::os

template <>
struct std::formatter<gopp::os::Signal> : std::formatter<std::string_view> {
    auto format(const gopp::os::Signal& s, auto& ctx) const {
        return std::formatter<std::string_view>::format(s.String().view(), ctx);
    }
};
