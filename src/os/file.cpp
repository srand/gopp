// os.File and the calls on paths, after Go's os/file.go, file_unix.go and
// file_posix.go.
#include "internal.hpp"

#include "../runtime/runtime.hpp"

#include <gopp/io.hpp>
#include <gopp/os.hpp>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <functional>
#include <string>

namespace gopp::os {

static_assert(O_RDONLY_ == O_RDONLY && O_WRONLY_ == O_WRONLY && O_RDWR_ == O_RDWR && O_CREATE_ == O_CREAT &&
              O_EXCL_ == O_EXCL && O_TRUNC_ == O_TRUNC && O_APPEND_ == O_APPEND && O_SYNC_ == O_SYNC);

namespace {

const error errWriteAtInAppendMode = errors::New("os: invalid use of WriteAt on file opened with O_APPEND");

// Go's syscallMode: the mode bits of FileMode for open, mkdir and chmod.
mode_t syscall_mode(FileMode m) {
    mode_t o = static_cast<mode_t>(uint32(m.Perm()));
    if (m & ModeSetuid) o |= S_ISUID;
    if (m & ModeSetgid) o |= S_ISGID;
    if (m & ModeSticky) o |= S_ISVTX;
    return o;
}

bool set_nonblock(int fd, bool nonblocking) {
    int fl = ::fcntl(fd, F_GETFL);
    if (fl < 0) return false;
    int nfl = nonblocking ? fl | O_NONBLOCK : fl & ~O_NONBLOCK;
    return nfl == fl || ::fcntl(fd, F_SETFL, nfl) == 0;
}

// The standard streams: made before main and never freed.
File& std_file(int fd, const char* name) {
    int fl = ::fcntl(fd, F_GETFL);
    if (fl < 0) fl = 0;
    auto impl = std::make_unique<detail::file_impl>(fd, string(name));
    impl->stdout_or_err = fd == 1 || fd == 2;
    impl->append_mode = fl & O_APPEND;
    struct stat st;
    impl->pfd.set_regular(::fstat(fd, &st) == 0 && (S_ISREG(st.st_mode) || S_ISBLK(st.st_mode)));
    if (fl & O_NONBLOCK) {
        // Go makes a non-blocking descriptor pollable; the poller can't be
        // used before main, so it's registered on first use.
        impl->nonblock.store(true, std::memory_order_relaxed);
        impl->lazy_poll.store(true, std::memory_order_relaxed);
    }
    File* f = new File(std::move(impl));
    f->add_ref();
    return *f;
}

}  // namespace

File& Stdin = std_file(0, "/dev/stdin");
File& Stdout = std_file(1, "/dev/stdout");
File& Stderr = std_file(2, "/dev/stderr");

namespace detail {

ptr<File> new_file(int fd, string name, file_kind kind, bool non_blocking, bool append_mode) {
    auto impl = std::make_unique<file_impl>(fd, std::move(name));
    impl->stdout_or_err = fd == 1 || fd == 2;
    impl->append_mode = append_mode;
    bool pollable = kind == file_kind::open_file || kind == file_kind::pipe || non_blocking;
    // epoll refuses regular files and directories; Go lets it fail on
    // Linux, but asking first saves two fcntls and an epoll_ctl.
    struct stat st;
    bool have_st = kind != file_kind::pipe && ::fstat(fd, &st) == 0;
    if (have_st && (S_ISREG(st.st_mode) || S_ISDIR(st.st_mode)) && kind == file_kind::open_file && !non_blocking) {
        pollable = false;
    }
    impl->pfd.set_regular(have_st && (S_ISREG(st.st_mode) || S_ISBLK(st.st_mode)));
    bool clear_nonblock = false;
    if (pollable) {
        if (non_blocking) {
            impl->nonblock.store(true, std::memory_order_relaxed);
        } else if (set_nonblock(fd, true)) {
            impl->nonblock.store(true, std::memory_order_relaxed);
            clear_nonblock = true;
        } else {
            pollable = false;
        }
    }
    if (impl->pfd.Init(pollable) && clear_nonblock) {
        if (set_nonblock(fd, false)) impl->nonblock.store(false, std::memory_order_relaxed);
    }
    return make_ptr<File>(std::move(impl));
}

std::string join_path(std::string_view dir, std::string_view name) {
    std::string s(dir);
    if (s.empty() || !IsPathSeparator(s.back())) s += PathSeparator;
    s += name;
    return s;
}

}  // namespace detail

File::File(std::unique_ptr<detail::file_impl> impl) noexcept : f_(std::move(impl)) {}

File::~File() = default;

const string& File::Name() const noexcept { return f_->name; }

error File::wrap_err(std::string_view op, error err) const {
    if (!err || err == io::EOF_) return err;
    if (err == poll::ErrFileClosing) err = ErrClosed;
    return detail::path_error(op, f_->name.view(), std::move(err));
}

std::pair<isize, error> File::Read(std::span<byte> p) {
    f_->ready();
    auto [n, e] = f_->pfd.Read(p);
    return {n, wrap_err("read", std::move(e))};
}

std::pair<isize, error> File::ReadAt(std::span<byte> p, int64 off) {
    if (off < 0) return {0, detail::path_error("readat", f_->name.view(), errors::New("negative offset"))};
    f_->ready();
    isize n = 0;
    while (!p.empty()) {
        auto [m, e] = f_->pfd.Pread(p, off);
        if (e) return {n, wrap_err("read", std::move(e))};
        n += m;
        p = p.subspan(static_cast<std::size_t>(m));
        off += m;
    }
    return {n, nullptr};
}

std::pair<isize, error> File::Write(std::span<const byte> p) {
    f_->ready();
    auto [n, e] = f_->pfd.Write(p);
    if (n < 0) n = 0;
    error err;
    if (static_cast<std::size_t>(n) != p.size()) err = io::ErrShortWrite;
    // Go's epipecheck: a broken standard output or error kills the program
    // with SIGPIPE, as it would a C program.
    if (f_->stdout_or_err && detail::errno_of(e) == EPIPE) runtime::sigpipe();
    if (e) err = wrap_err("write", std::move(e));
    return {n, err};
}

std::pair<isize, error> File::WriteString(std::string_view s) { return Write(as_bytes(s)); }

std::pair<isize, error> File::WriteAt(std::span<const byte> p, int64 off) {
    if (f_->append_mode) return {0, errWriteAtInAppendMode};
    if (off < 0) return {0, detail::path_error("writeat", f_->name.view(), errors::New("negative offset"))};
    f_->ready();
    isize n = 0;
    while (!p.empty()) {
        auto [m, e] = f_->pfd.Pwrite(p, off);
        n += m;
        if (e) return {n, wrap_err("write", std::move(e))};
        p = p.subspan(static_cast<std::size_t>(m));
        off += m;
    }
    return {n, nullptr};
}

std::pair<int64, error> File::Seek(int64 offset, int whence) {
    {
        // Seeking restarts reading the directory.
        std::lock_guard l(f_->dir_mu);
        f_->dir.reset();
    }
    auto [r, e] = f_->pfd.Seek(offset, whence);
    if (e) return {0, wrap_err("seek", std::move(e))};
    return {r, nullptr};
}

error File::Close() {
    {
        std::lock_guard l(f_->dir_mu);
        f_->dir.reset();
    }
    if (error e = f_->pfd.Close()) {
        if (e == poll::ErrFileClosing) e = ErrClosed;
        return detail::path_error("close", f_->name.view(), std::move(e));
    }
    return nullptr;
}

error File::Sync() { return wrap_err("sync", f_->pfd.Fsync()); }
error File::Truncate(int64 size) { return wrap_err("truncate", f_->pfd.Ftruncate(size)); }
error File::Chmod(FileMode mode) { return wrap_err("chmod", f_->pfd.Fchmod(syscall_mode(mode))); }
error File::Chown(int uid, int gid) { return wrap_err("chown", f_->pfd.Fchown(uid, gid)); }
error File::Chdir() { return wrap_err("chdir", f_->pfd.Fchdir()); }

namespace {

// Deadline errors aren't wrapped, as in Go; a closed file's is ErrClosed.
error deadline_error(error e) { return e == poll::ErrFileClosing ? ErrClosed : e; }

}  // namespace

error File::SetDeadline(time::Time t) {
    f_->ready();
    return deadline_error(f_->pfd.SetDeadline(t, 'r' + 'w'));
}

error File::SetReadDeadline(time::Time t) {
    f_->ready();
    return deadline_error(f_->pfd.SetDeadline(t, 'r'));
}

error File::SetWriteDeadline(time::Time t) {
    f_->ready();
    return deadline_error(f_->pfd.SetDeadline(t, 'w'));
}

uintptr File::Fd() {
    f_->ready();
    if (f_->nonblock.exchange(false)) f_->pfd.SetBlocking();
    return static_cast<uintptr>(static_cast<intptr_t>(f_->pfd.Sysfd()));
}

ptr<File> NewFile(uintptr fd, string name) {
    int fdi = static_cast<int>(static_cast<intptr_t>(fd));
    if (fdi < 0) return nullptr;
    int fl = ::fcntl(fdi, F_GETFL);
    if (fl < 0) fl = 0;
    return detail::new_file(fdi, std::move(name), detail::file_kind::new_file, fl & O_NONBLOCK, fl & O_APPEND);
}

std::pair<ptr<File>, error> Open(std::string_view name) { return OpenFile(name, O_RDONLY_, 0); }

std::pair<ptr<File>, error> Create(std::string_view name) {
    return OpenFile(name, O_RDWR_ | O_CREATE_ | O_TRUNC_, 0666);
}

std::pair<ptr<File>, error> OpenFile(std::string_view name, int flag, FileMode perm) {
    std::string p;
    if (!detail::cpath(name, p)) return {nullptr, detail::path_error("open", name, detail::errno_error(EINVAL))};
    int e;
    int fd = detail::open_at(AT_FDCWD, p.c_str(), flag | O_CLOEXEC, syscall_mode(perm), &e);
    if (e) return {nullptr, detail::path_error("open", name, detail::errno_error(e))};
    return {detail::new_file(fd, string(name), detail::file_kind::open_file, flag & O_NONBLOCK, flag & O_APPEND), nullptr};
}

std::pair<bytes, error> ReadFile(std::string_view name) {
    auto [f, err] = Open(name);
    if (err) return {bytes(), err};
    int64 stat_size = 0;
    if (auto [fi, e] = f->Stat(); !e) stat_size = fi->Size();
    auto r = detail::read_file_contents(stat_size, [&](std::span<byte> p) { return f->Read(p); });
    f->Close();
    return r;
}

namespace detail {

std::pair<bytes, error> read_file_contents(int64 stat_size,
                                           const std::function<std::pair<isize, error>(std::span<byte>)>& read) {
    // Room for the whole file and a byte for the read at its end, at least
    // 512 bytes: /proc's files claim size 0 but don't work if read in small
    // pieces.
    constexpr std::size_t min_buf = 512;
    bool zero_size = stat_size == 0;
    std::size_t size = static_cast<std::size_t>(stat_size) + 1;
    if (size < min_buf) size = min_buf;
    bytes data = bytes::make(0, size);
    for (;;) {
        auto [n, e] = read(std::span<byte>(data.data() + data.len(), data.cap() - data.len()));
        data = data.Sub(0, data.len() + static_cast<std::size_t>(n));
        if (e) {
            if (e == io::EOF_) e = nullptr;
            return {data, e};
        }
        // Out of capacity, or a /proc-like zero-sized file: grow, so that
        // reads of zero-sized files always get a buffer of at least 512.
        std::size_t cap_remain = data.cap() - data.len();
        if (cap_remain == 0 || (zero_size && cap_remain < min_buf)) {
            bytes grown = bytes::make(data.len(), data.cap() + std::max(data.cap(), min_buf));
            std::copy(data.begin(), data.end(), grown.begin());
            data = std::move(grown);
        }
    }
}

}  // namespace detail

error WriteFile(std::string_view name, std::span<const byte> data, FileMode perm) {
    auto [f, err] = OpenFile(name, O_WRONLY_ | O_CREATE_ | O_TRUNC_, perm);
    if (err) return err;
    err = f->Write(data).second;
    if (error err1 = f->Close(); err1 && !err) err = err1;
    return err;
}

error WriteFile(std::string_view name, std::string_view data, FileMode perm) {
    return WriteFile(name, as_bytes(data), perm);
}

namespace {

// A call on one path that fails with a PathError.
template <typename F>
error path_call(std::string_view op, std::string_view name, F&& f) {
    std::string p;
    if (!detail::cpath(name, p)) return detail::path_error(op, name, detail::errno_error(EINVAL));
    if (int e = detail::sys([&] { return f(p.c_str()); })) return detail::path_error(op, name, detail::errno_error(e));
    return nullptr;
}

// A call on two paths that fails with a LinkError, through io_uring if it
// supports opcode: prep(sqe, old, new) prepares it, f(old, new) makes it.
template <typename Prep, typename F>
error link_call(std::string_view op, std::string_view oldname, std::string_view newname, int opcode, Prep&& prep,
                F&& f) {
    std::string o, n;
    int e = 0;
    if (!detail::cpath(oldname, o) || !detail::cpath(newname, n)) {
        e = EINVAL;
    } else {
        e = detail::sys_io(
            opcode, [&](io_uring_sqe* sqe) { prep(sqe, o.c_str(), n.c_str()); }, [&] { return f(o.c_str(), n.c_str()); });
    }
    if (e) return make_ptr<LinkError>(string(op), string(oldname), string(newname), detail::errno_error(e));
    return nullptr;
}

}  // namespace

error Mkdir(std::string_view name, FileMode perm) {
    std::string p;
    if (!detail::cpath(name, p)) return detail::path_error("mkdir", name, detail::errno_error(EINVAL));
    mode_t mode = syscall_mode(perm);
    int e = detail::sys_io(
        IORING_OP_MKDIRAT, [&](io_uring_sqe* sqe) { io_uring_prep_mkdirat(sqe, AT_FDCWD, p.c_str(), mode); },
        [&] { return ::mkdir(p.c_str(), mode); });
    if (e) return detail::path_error("mkdir", name, detail::errno_error(e));
    return nullptr;
}

error Chdir(std::string_view dir) {
    return path_call("chdir", dir, [](const char* p) { return ::chdir(p); });
}

error Remove(std::string_view name) {
    // unlink, then rmdir: which one fits is unknown, and asking would race.
    std::string p;
    if (!detail::cpath(name, p)) return detail::path_error("remove", name, detail::errno_error(EINVAL));
    int e = detail::unlink_at(AT_FDCWD, p.c_str(), 0);
    if (e == 0) return nullptr;
    int e1 = detail::unlink_at(AT_FDCWD, p.c_str(), AT_REMOVEDIR);
    if (e1 == 0) return nullptr;
    // Both failed: report rmdir's error unless it's ENOTDIR, which means
    // unlink's is the one that matters.
    if (e1 != ENOTDIR) e = e1;
    return detail::path_error("remove", name, detail::errno_error(e));
}

error Rename(std::string_view oldpath, std::string_view newpath) {
    // rename(2) replaces an empty directory; Go doesn't.
    if (auto [fi, err] = Lstat(newpath); !err && fi->IsDir()) {
        auto [ofi, oerr] = Lstat(oldpath);
        if (oerr) {
            error e = oerr;
            if (auto* pe = dynamic_cast<const PathError*>(e.get())) e = pe->Err;
            return make_ptr<LinkError>(string("rename"), string(oldpath), string(newpath), e);
        }
        if (newpath == oldpath || !SameFile(fi, ofi)) {
            return make_ptr<LinkError>(string("rename"), string(oldpath), string(newpath), detail::errno_error(EEXIST));
        }
    }
    return link_call(
        "rename", oldpath, newpath, IORING_OP_RENAMEAT,
        [](io_uring_sqe* sqe, const char* o, const char* n) { io_uring_prep_renameat(sqe, AT_FDCWD, o, AT_FDCWD, n, 0); },
        [](const char* o, const char* n) { return ::rename(o, n); });
}

error Truncate(std::string_view name, int64 size) {
    return path_call("truncate", name, [&](const char* p) { return ::truncate(p, size); });
}

error Chmod(std::string_view name, FileMode mode) {
    return path_call("chmod", name, [&](const char* p) { return ::chmod(p, syscall_mode(mode)); });
}

error Chown(std::string_view name, int uid, int gid) {
    return path_call("chown", name, [&](const char* p) {
        return ::chown(p, static_cast<uid_t>(uid), static_cast<gid_t>(gid));
    });
}

error Lchown(std::string_view name, int uid, int gid) {
    return path_call("lchown", name, [&](const char* p) {
        return ::lchown(p, static_cast<uid_t>(uid), static_cast<gid_t>(gid));
    });
}

error Chtimes(std::string_view name, time::Time atime, time::Time mtime) {
    timespec ts[2];
    auto set = [](timespec& t, const time::Time& tm) {
        if (tm.IsZero()) {
            t.tv_sec = 0;
            t.tv_nsec = UTIME_OMIT;
        } else {
            int64 ns = tm.UnixNano();
            t.tv_sec = ns / 1000000000;
            t.tv_nsec = ns % 1000000000;
            if (t.tv_nsec < 0) {
                t.tv_sec--;
                t.tv_nsec += 1000000000;
            }
        }
    };
    set(ts[0], atime);
    set(ts[1], mtime);
    return path_call("chtimes", name, [&](const char* p) { return ::utimensat(AT_FDCWD, p, ts, 0); });
}

error Link(std::string_view oldname, std::string_view newname) {
    return link_call(
        "link", oldname, newname, IORING_OP_LINKAT,
        [](io_uring_sqe* sqe, const char* o, const char* n) { io_uring_prep_linkat(sqe, AT_FDCWD, o, AT_FDCWD, n, 0); },
        [](const char* o, const char* n) { return ::link(o, n); });
}

error Symlink(std::string_view oldname, std::string_view newname) {
    return link_call(
        "symlink", oldname, newname, IORING_OP_SYMLINKAT,
        [](io_uring_sqe* sqe, const char* o, const char* n) { io_uring_prep_symlinkat(sqe, o, AT_FDCWD, n); },
        [](const char* o, const char* n) { return ::symlink(o, n); });
}

std::pair<string, error> Readlink(std::string_view name) {
    std::string p;
    if (!detail::cpath(name, p)) return {{}, detail::path_error("readlink", name, detail::errno_error(EINVAL))};
    for (std::size_t len = 128;; len *= 2) {
        std::string b(len, '\0');
        ssize_t n = 0;
        int e = detail::sys([&] { return n = ::readlink(p.c_str(), b.data(), len); });
        if (e) return {{}, detail::path_error("readlink", name, detail::errno_error(e))};
        if (static_cast<std::size_t>(n) < len) return {string(b.data(), static_cast<std::size_t>(n)), nullptr};
    }
}

string TempDir() {
    string dir = Getenv("TMPDIR");
    if (dir.empty()) dir = string::from_static("/tmp");
    return dir;
}

std::tuple<ptr<File>, ptr<File>, error> Pipe() {
    int p[2];
    if (::pipe2(p, O_CLOEXEC) < 0) return {nullptr, nullptr, NewSyscallError("pipe2", detail::errno_error(errno))};
    return {detail::new_file(p[0], string::from_static("|0"), detail::file_kind::pipe, false),
            detail::new_file(p[1], string::from_static("|1"), detail::file_kind::pipe, false), nullptr};
}

int Getpagesize() noexcept { return static_cast<int>(::sysconf(_SC_PAGESIZE)); }

}  // namespace gopp::os
