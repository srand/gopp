// The insides of os::File, shared by the os sources and tests.
#pragma once

#include "../internal/poll/fd.hpp"

#include <gopp/os.hpp>
#include <gopp/runtime.hpp>
#include <gopp/sync.hpp>
#include <gopp/syscall.hpp>

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
struct dir_info {
    std::vector<byte> buf;  // buffer for getdents64
    std::size_t nbuf = 0;   // bytes in buf
    std::size_t bufp = 0;   // the next record
};

// How a File came to be (Go's newFileKind).
enum class file_kind { new_file, open_file, pipe, no_poll };

struct file_impl {
    file_impl(int fd, string name) : pfd(fd), name(std::move(name)) {}

    poll::FD pfd;
    string name;
    sync::Mutex dir_mu;
    std::unique_ptr<dir_info> dir;  // set while the directory is being read; under dir_mu
    std::atomic<bool> nonblock{false};  // whether the descriptor is in non-blocking mode
    bool stdout_or_err = false;
    bool append_mode = false;

    // A standard stream given in non-blocking mode registers with the poller
    // on first use: it's made before main, when the runtime may not be.
    std::atomic<bool> lazy_poll{false};
    std::once_flag lazy_once;
    void ready() {
        if (lazy_poll.load(std::memory_order_acquire)) {
            std::call_once(lazy_once, [this] {
                pfd.Init(true);
                lazy_poll.store(false, std::memory_order_release);
            });
        }
    }
};

// Go's newFile: a File for fd, pollable if it's a pipe or socket, or was
// given in non-blocking mode.
ptr<File> new_file(int fd, string name, file_kind kind, bool non_blocking, bool append_mode = false);

// os.fileStat
class file_stat final : public FileInfo {
public:
    string Name() const override { return name; }
    int64 Size() const override { return size; }
    FileMode Mode() const override { return mode; }
    time::Time ModTime() const override { return mod_time; }
    bool IsDir() const override { return mode.IsDir(); }
    const void* Sys() const override { return &sys; }

    string name;
    int64 size = 0;
    FileMode mode;
    time::Time mod_time;
    struct stat sys{};
};

// Go's fillFileStatFromSys: fills fs from fs->sys; the name is name's base.
void fill_file_stat(file_stat& fs, std::string_view name);

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
            runtime::blocking_call b;
            r = static_cast<int>(f());
            e = errno;
        }
        if (r >= 0) return 0;
        if (e != EINTR) return e;
    }
}

inline error errno_error(int e) { return syscall::errno_error(e); }

// The errno of err if it's a syscall::Errno, otherwise 0.
inline int errno_of(const error& err) {
    auto* e = dynamic_cast<const syscall::Errno*>(err.get());
    return e ? e->Code : 0;
}

inline ptr<PathError> path_error(std::string_view op, std::string_view path, error err) {
    return make_ptr<PathError>(string(op), string(path), std::move(err));
}

// Go's readFileContents: reads a file of stat_size bytes, by stat, with
// read.
std::pair<bytes, error> read_file_contents(int64 stat_size,
                                           const std::function<std::pair<isize, error>(std::span<byte>)>& read);

// The error of a CreateTemp or MkdirTemp pattern with a '/'.
extern const error errPatternHasSeparator;

// Go's splitPath: the parent directory and the base name of path.
std::pair<std::string_view, std::string_view> split_path(std::string_view path);

// Go's joinPath.
std::string join_path(std::string_view dir, std::string_view name);

// Go's lstatat: the Lstat of name in the directory f.
std::pair<ptr<FileInfo>, error> lstatat(File& f, file_impl& fi, std::string_view name);

}  // namespace detail

}  // namespace gopp::os
