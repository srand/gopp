// The insides of os::File, shared by the os sources and tests.
#pragma once

#include "../internal/poll/fd.hpp"
#include "../runtime/uring.hpp"

#include <gopp/os.hpp>
#include <gopp/runtime.hpp>
#include <gopp/sync.hpp>
#include <gopp/syscall.hpp>

#include <fcntl.h>
#include "../runtime/liburing.hpp"
#include <sys/stat.h>

#include <atomic>
#include <cerrno>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace gopp::os {

namespace detail {

// The state of reading a directory (Go's dirInfo).
struct dirInfo {
    std::vector<byte> buf;  // buffer for getdents64
    std::size_t nbuf = 0;   // bytes in buf
    std::size_t bufp = 0;   // the next record
};

// How a File came to be (Go's newFileKind).
enum class fileKind { newFile, openFile, pipe, noPoll };

struct fileImpl {
    fileImpl(int fd, string name) : pfd(fd), name(std::move(name)) {}
    // The insides of f.
    static fileImpl& of(File& f) noexcept { return *f.f_; }

    poll::FD pfd;
    string name;
    sync::Mutex dirMu;
    std::unique_ptr<dirInfo> dir;  // set while the directory is being read; under dirMu
    std::atomic<bool> nonblock{false};  // whether the descriptor is in non-blocking mode
    bool stdoutOrErr = false;
    bool appendMode = false;
    bool inRoot = false;  // opened in a Root: ReadDir lstats each entry (Go's inRoot)

    // A standard stream given in non-blocking mode registers with the poller
    // on first use: it's made before main, when the runtime may not be.
    std::atomic<bool> lazyPoll{false};
    std::once_flag lazyOnce;
    void ready() {
        if (lazyPoll.load(std::memory_order_acquire)) {
            std::call_once(lazyOnce, [this] {
                pfd.Init(true);
                lazyPoll.store(false, std::memory_order_release);
            });
        }
    }
};

// Go's newFile: a File for fd, pollable if it's a pipe or socket, or was
// given in non-blocking mode.
ptr<File> newFile(int fd, string name, fileKind kind, bool nonBlocking, bool appendMode = false);

// os.fileStat
class fileStat final : public FileInfo {
public:
    string Name() const override { return name; }
    int64 Size() const override { return size; }
    FileMode Mode() const override { return mode; }
    time::Time ModTime() const override { return modTime; }
    bool IsDir() const override { return mode.IsDir(); }
    const void* Sys() const override { return &sys; }

    string name;
    int64 size = 0;
    FileMode mode;
    time::Time modTime;
    struct stat sys{};
};

// Go's fillFileStatFromSys: fills fs from fs->sys; the name is name's base.
void fillFileStat(fileStat& fs, std::string_view name);

// The C string of a path; false if it holds a NUL, which Go reports as
// EINVAL.
inline bool cpath(std::string_view s, std::string& out) {
    if (s.find('\0') != std::string_view::npos) return false;
    out.assign(s);
    return true;
}

// Runs a file system call, as a blocking call, retrying it after EINTR;
// returns 0 or the errno.
template <typename F>
int sys(F&& f) {
    for (;;) {
        int r, e;
        {
            runtime::blockingCall b;
            r = static_cast<int>(f());
            e = errno;
        }
        if (r >= 0) return 0;
        if (e != EINTR) return e;
    }
}

// Runs a file system call through io_uring if it supports opcode, or as
// a blocking call (see sys); returns 0 or the errno.
template <typename Prep, typename Call>
int sysIo(int opcode, Prep&& prep, Call&& call) {
    long r = runtime::uringOrBlocking(opcode, prep, call);
    return r < 0 ? static_cast<int>(-r) : 0;
}

// openat(2) through sys_io: the descriptor, or -1 and the errno in *err.
inline int openAt(int dirfd, const char* path, int flags, mode_t mode, int* err) {
    long r = runtime::uringOrBlocking(
        IORING_OP_OPENAT, [&](io_uring_sqe* sqe) { io_uring_prep_openat(sqe, dirfd, path, flags, mode); },
        [&] { return ::openat(dirfd, path, flags, mode); });
    *err = r < 0 ? static_cast<int>(-r) : 0;
    return r < 0 ? -1 : static_cast<int>(r);
}

// unlinkat(2) through sys_io; returns 0 or the errno.
inline int unlinkAt(int dirfd, const char* path, int flags) {
    return sysIo(
        IORING_OP_UNLINKAT, [&](io_uring_sqe* sqe) { io_uring_prep_unlinkat(sqe, dirfd, path, flags); },
        [&] { return ::unlinkat(dirfd, path, flags); });
}

inline error errnoError(int e) { return syscall::errnoError(e); }

// The errno of err if it's a syscall::Errno, otherwise 0.
inline int errnoOf(const error& err) {
    auto* e = dynamic_cast<const syscall::Errno*>(err.get());
    return e ? e->Code : 0;
}

inline ptr<PathError> pathError(std::string_view op, std::string_view path, error err) {
    return make_ptr<PathError>(string(op), string(path), std::move(err));
}

// Go's readFileContents: reads a file of statSize bytes, by stat, with
// read.
std::pair<slice<byte>, error> readFileContents(int64 statSize,
                                                 const std::function<std::pair<isize, error>(std::span<byte>)>& read);

// The error of a CreateTemp or MkdirTemp pattern with a '/'.
extern const error errPatternHasSeparator;

// Go's splitPath: the parent directory and the base name of path.
std::pair<std::string_view, std::string_view> splitPath(std::string_view path);

// Go's errPathEscapes: the error of a Root operation whose name leads out
// of the Root (export_test's ErrPathEscapes).
extern const error errPathEscapes;

// Go's removeAllFrom: removes base, and what it holds, in parentFd.
error removeAllFrom(int parentFd, const std::string& base);

// Go's syscallMode: the mode bits of FileMode for open, mkdir and chmod.
mode_t syscallMode(FileMode m);

// The timespecs of Chtimes's times (Go's chtimesUtimes): UTIME_OMIT for a
// zero Time.
void chtimesUtimes(time::Time atime, time::Time mtime, timespec ts[2]);

// Go's joinPath.
std::string joinPath(std::string_view dir, std::string_view name);

// Go's lstatat: the Lstat of name in the directory f.
std::pair<ptr<FileInfo>, error> lstatat(File& f, fileImpl& fi, std::string_view name);

// Go's pidfdWorks: whether pidfd_open, pidfdSendSignal, waitid(P_PIDFD)
// and clone(CLONE_PIDFD) work, checked once.
bool pidfdWorks();
// A test hook: while set, the os package doesn't use pidfds, as on a
// kernel without them.
extern std::atomic<bool> pidfdDisabled;

}  // namespace detail

}  // namespace gopp::os
