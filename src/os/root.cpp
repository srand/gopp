// os.Root, ported from Go 1.27.3's os/root.go, root_openat.go and
// root_unix.go.
#include "internal.hpp"

#include <gopp/io.hpp>

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <mutex>
#include <string>
#include <vector>

namespace gopp::os {

namespace detail {

// Go's root: the directory's descriptor, closed when the Root is closed and
// no operation is using it.
struct rootImpl {
    rootImpl(int fd_, string name_) : name(std::move(name_)), fd(fd_) {}
    ~rootImpl() {
        if (!closed && refs == 0) ::close(fd);
    }

    error close() {
        std::lock_guard l(mu);
        if (!closed && refs == 0) ::close(fd);
        closed = true;
        return nullptr;
    }
    error incref() {
        std::lock_guard l(mu);
        if (closed) return ErrClosed;
        refs++;
        return nullptr;
    }
    void decref() {
        std::lock_guard l(mu);
        if (refs <= 0) panic("bad Root refcount");
        refs--;
        if (closed && refs == 0) ::close(fd);
    }

    string name;
    std::mutex mu;  // held only briefly, never across a park
    int fd;
    int refs = 0;         // number of active operations
    bool closed = false;  // set when closed
};

const error errPathEscapes = errors::New("path escapes from parent");

}  // namespace detail

namespace {

using detail::errnoError;
using detail::errnoOf;
using detail::pathError;

constexpr int rootMaxSymlinks = 8;

using detail::errPathEscapes;

// Go's errSymlink: an operation found a symbolic link to target, which
// doInRoot follows. Never seen outside this file.
struct errSymlink final : errorInterface {
    explicit errSymlink(std::string t) : target(std::move(t)) {}
    string Error() const override { panic("errSymlink is not user-visible"); }
    std::string target;
};

const errSymlink* asSymlink(const error& err) { return dynamic_cast<const errSymlink*>(err.get()); }

error errnoOr(int e) { return e ? errnoError(e) : nullptr; }

bool isNoFollowErr(int e) { return e == ELOOP || e == EMLINK; }

bool isLink(const FileInfo& fi) { return bool(fi.Mode() & ModeSymlink); }

// openat(2), as a blocking call, retried after EINTR; the descriptor, or -1
// and the errno.
int openat(int dirfd, const std::string& name, int flags, mode_t mode, int* e) {
    int fd = -1;
    *e = detail::sys([&] { return fd = ::openat(dirfd, name.c_str(), flags, mode); });
    return *e ? -1 : fd;
}

std::pair<std::string, error> readlinkat(int fd, const std::string& name) {
    for (std::size_t len = 128;; len *= 2) {
        std::string b(len, '\0');
        ssize_t n = 0;
        int e = detail::sys([&] { return n = ::readlinkat(fd, name.c_str(), b.data(), len); });
        if (e == ERANGE) continue;
        if (e) return {{}, errnoError(e)};
        n = std::max<ssize_t>(n, 0);
        if (std::size_t(n) < len) {
            b.resize(std::size_t(n));
            return {b, nullptr};
        }
    }
}

// checkSymlink: errSymlink if name is a symbolic link, otherwise origError.
error checkSymlink(int parent, const std::string& name, error origError) {
    auto [link, err] = readlinkat(parent, name);
    if (err) return origError;
    return make_ptr<errSymlink>(std::move(link));
}

std::pair<ptr<FileInfo>, error> lstatatWithName(int parent, std::string_view origName, const std::string& name) {
    auto fs = make_ptr<detail::fileStat>();
    if (int e = detail::sys([&] { return ::fstatat(parent, name.c_str(), &fs->sys, AT_SYMLINK_NOFOLLOW); })) {
        return {nullptr, errnoError(e)};
    }
    detail::fillFileStat(*fs, origName);
    return {fs, nullptr};
}

std::pair<ptr<FileInfo>, error> lstatat(int parent, const std::string& name) {
    return lstatatWithName(parent, name, name);
}

std::pair<FileMode, error> modeAt(int parent, const std::string& name) {
    auto [fi, err] = lstatat(parent, name);
    if (err) return {0, err};
    return {fi->Mode(), nullptr};
}

// Go's rootOpenDir: name in parent as a directory, without following a
// symbolic link (an errSymlink).
std::pair<int, error> rootOpenDir(int parent, const std::string& name) {
    int e;
    int fd = openat(parent, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_DIRECTORY, 0, &e);
    if (isNoFollowErr(e) || e == ENOTDIR) return {-1, checkSymlink(parent, name, errnoError(e))};
    if (e == ENOTSUP || e == EOPNOTSUPP) return {-1, errnoError(ENOTDIR)};
    if (e) return {-1, errnoError(e)};
    return {fd, nullptr};
}

error afterResolvingSymlink(int parent, const std::string& name, const std::function<error()>& f) {
    if (error err = checkSymlink(parent, name, nullptr)) return err;
    return f();
}

error chmodat(int parent, const std::string& name, FileMode mode) {
    return afterResolvingSymlink(parent, name, [&] {
        // fchmodat with AT_SYMLINK_NOFOLLOW (Go's unix.Fchmodat), through
        // fchmodat2 where the kernel has it.
        return errnoOr(detail::sys([&] {
            return ::fchmodat(parent, name.c_str(), detail::syscallMode(mode), AT_SYMLINK_NOFOLLOW);
        }));
    });
}

error chownat(int parent, const std::string& name, int uid, int gid) {
    return afterResolvingSymlink(parent, name, [&] {
        return errnoOr(detail::sys([&] { return ::fchownat(parent, name.c_str(), uid_t(uid), gid_t(gid), AT_SYMLINK_NOFOLLOW); }));
    });
}

error lchownat(int parent, const std::string& name, int uid, int gid) {
    return errnoOr(detail::sys([&] { return ::fchownat(parent, name.c_str(), uid_t(uid), gid_t(gid), AT_SYMLINK_NOFOLLOW); }));
}

error chtimesat(int parent, const std::string& name, time::Time atime, time::Time mtime) {
    return afterResolvingSymlink(parent, name, [&] {
        timespec ts[2];
        detail::chtimesUtimes(atime, mtime, ts);
        return errnoOr(detail::sys([&] { return ::utimensat(parent, name.c_str(), ts, AT_SYMLINK_NOFOLLOW); }));
    });
}

error mkdirat(int fd, const std::string& name, FileMode perm) {
    return errnoOr(detail::sys([&] { return ::mkdirat(fd, name.c_str(), detail::syscallMode(perm)); }));
}

error removeat(int fd, const std::string& name) {
    // The system call interface forces us to know whether name is a file or
    // directory. Try both.
    int e = detail::unlinkAt(fd, name.c_str(), 0);
    if (e == 0) return nullptr;
    int e1 = detail::unlinkAt(fd, name.c_str(), AT_REMOVEDIR);
    if (e1 == 0) return nullptr;
    // Both failed. See RemoveAll's comments: report the more interesting
    // error.
    if (e1 != ENOTDIR) return errnoError(e1);
    return errnoError(e);
}

error renameat(int oldfd, const std::string& oldname, int newfd, const std::string& newname) {
    return errnoOr(detail::sys([&] { return ::renameat(oldfd, oldname.c_str(), newfd, newname.c_str()); }));
}

error linkat(int oldfd, const std::string& oldname, int newfd, const std::string& newname) {
    return errnoOr(detail::sys([&] { return ::linkat(oldfd, oldname.c_str(), newfd, newname.c_str(), 0); }));
}

error symlinkat(std::string_view oldname, int newfd, const std::string& newname) {
    std::string old;
    if (!detail::cpath(oldname, old)) return errnoError(EINVAL);
    return errnoOr(detail::sys([&] { return ::symlinkat(old.c_str(), newfd, newname.c_str()); }));
}

bool endsWithDot(std::string_view path) {
    if (path == ".") return true;
    return path.size() >= 2 && path.back() == '.' && IsPathSeparator(path[path.size() - 2]);
}

// Go's underlyingError: the error of a PathError, LinkError or
// SyscallError.
error underlyingError(const error& err) {
    if (auto* e = dynamic_cast<const PathError*>(err.get())) return e->Err;
    if (auto* e = dynamic_cast<const LinkError*>(err.get())) return e->Err;
    if (auto* e = dynamic_cast<const SyscallError*>(err.get())) return e->Err;
    return err;
}

// splitPathInRoot splits a path into components and joins it with the
// given prefix and suffix.
//
// The path is relative to a Root, and must not be absolute, volume-
// relative, or "".
//
// "." components are removed, except in the last component.
//
// Path separators following the last component are returned in
// endsInSlash.
struct splitResult {
    std::vector<std::string> parts;
    bool endsInSlash = false;
    error err;
};

splitResult splitPathInRoot(std::string_view s, std::span<const std::string> prefix,
                            std::span<const std::string> suffix) {
    if (s.empty()) return {{}, false, errors::New("empty path")};
    if (IsPathSeparator(s[0])) return {{}, false, errPathEscapes};

    splitResult r;
    r.parts.assign(prefix.begin(), prefix.end());
    std::size_t i = 0, j = 1;
    for (;;) {
        if (j < s.size() && !IsPathSeparator(s[j])) {
            // Keep looking for the end of this component.
            j++;
            continue;
        }
        r.parts.emplace_back(s.substr(i, j - i));
        // Advance to the next component, or end of the path.
        std::size_t partEnd = j;
        while (j < s.size() && IsPathSeparator(s[j])) j++;
        if (j == s.size()) {
            // If this is the last path component, preserve any trailing
            // path separators.
            r.endsInSlash = partEnd < s.size();
            break;
        }
        if (r.parts.back() == ".") {
            // Remove "." components, except at the end.
            r.parts.pop_back();
        }
        i = j;
    }
    if (!suffix.empty() && !r.parts.empty() && r.parts.back() == ".") {
        // Remove a trailing "." component if we're joining to a suffix.
        r.parts.pop_back();
    }
    r.parts.insert(r.parts.end(), suffix.begin(), suffix.end());
    return r;
}

enum : unsigned {
    // Don't handle a terminal slash: the operation does (mkdirat on Linux
    // takes "dir/" for a new directory).
    doInRootNoHandleTerminalSlash = 1 << 0,
    // The operation creates a directory, so a missing final element with a
    // trailing slash is fine.
    doInRootCreatingDirectory = 1 << 1,
    // Always resolve a symlink with a terminal slash (only for Windows in
    // Go).
    doInRootAlwaysResolveTerminalSlash = 1 << 2,
};

using OpenDirFunc = std::function<std::pair<int, error>(int parent, const std::string& name)>;

// doInRoot performs an operation on a path in a Root.
//
// It calls f with the FD or handle for the directory containing the last
// path element, and the name of the last path element.
//
// For example, given the path a/b/c it calls f with the FD for a/b and the
// name "c".
//
// If f returns errSymlink, doInRoot will follow the symlink and call f
// again.
//
// If openDirFunc is set, it is called to open intermediate directories
// (rootOpenDir otherwise).
template <typename T, typename F>
std::pair<T, error> doInRoot(detail::rootImpl& root, std::string_view name, unsigned flags,
                             const OpenDirFunc& openDirFunc, F&& f) {
    if (error err = root.incref()) return {T{}, err};
    struct unref {
        detail::rootImpl& r;
        ~unref() { r.decref(); }
    } unref_{root};

    auto [parts, endsInSlash, serr] = splitPathInRoot(name, {}, {});
    if (serr) return {T{}, serr};
    const OpenDirFunc& openDir = openDirFunc ? openDirFunc : OpenDirFunc(rootOpenDir);

    const int rootfd = root.fd;
    int dirfd = rootfd;
    struct closer {
        int& dirfd;
        int rootfd;
        void reset() {
            if (dirfd != rootfd) ::close(dirfd);
            dirfd = rootfd;
        }
        ~closer() { reset(); }
    } closer_{dirfd, rootfd};

    // When resolving .. path components, we restart path resolution from
    // the root. We limit the number of restarts to avoid unbounded
    // quadratic behavior in the presence of a large number of .. path
    // components.
    constexpr int maxSteps = 255;
    constexpr int maxRestarts = 8;

    std::size_t i = 0;
    int steps = 0;
    int restarts = 0;
    int symlinks = 0;
    for (;;) {
        steps++;
        if (steps > maxSteps && restarts > maxRestarts) return {T{}, errnoError(ENAMETOOLONG)};

        if (parts[i] == "..") {
            // Resolve one or more parent ("..") path components.
            //
            // Rewrite the original path, removing the elements eliminated
            // by ".." components, and start over from the beginning.
            restarts++;
            std::size_t end = i + 1;
            while (end < parts.size() && parts[end] == "..") end++;
            std::size_t count = end - i;
            if (count > i) return {T{}, errPathEscapes};
            parts.erase(parts.begin() + std::ptrdiff_t(i - count), parts.begin() + std::ptrdiff_t(end));
            if (parts.empty()) parts = {"."};
            i = 0;
            closer_.reset();
            continue;
        }

        T ret{};
        error err;
        if (parts[i].find('\0') != std::string::npos) {
            // Go's system calls refuse a name with a NUL byte.
            return {T{}, errnoError(EINVAL)};
        }
        if (i == parts.size() - 1) {
            // This is the last path element. Call f to perform the
            // operation.
            //
            // If the path ends in a slash, make sure the final element is
            // a directory or a symlink to one (and what the symlink points
            // to is resolved below).
            if (endsInSlash && !(flags & doInRootNoHandleTerminalSlash)) {
                auto [fi, lerr] = lstatat(dirfd, parts[i]);
                if (IsNotExist(lerr) && (flags & doInRootCreatingDirectory)) {
                    err = nullptr;
                } else if (lerr) {
                    return {T{}, lerr};
                } else if (fi->Mode().Type() == ModeDir) {
                    // OK
                } else if (isLink(*fi)) {
                    err = checkSymlink(dirfd, parts[i], errnoError(ENOTDIR));
                } else {
                    return {T{}, errnoError(ENOTDIR)};
                }
            }
            if (!err) {
                std::tie(ret, err) = f(dirfd, parts[i], endsInSlash);
                if (!err) return {std::move(ret), nullptr};
            }
        } else {
            // This is not the last path element. Open the next directory
            // and continue.
            auto [fd, oerr] = openDir(dirfd, parts[i]);
            err = oerr;
            if (!err) {
                if (dirfd != rootfd) ::close(dirfd);
                dirfd = fd;
            }
        }

        if (!err) {
            i++;
            continue;
        }
        if (auto* e = asSymlink(err)) {
            symlinks++;
            if (symlinks > rootMaxSymlinks) return {T{}, errnoError(ELOOP)};
            bool lastPart = i == parts.size() - 1;
            auto split = splitPathInRoot(e->target, std::span(parts).first(i), std::span(parts).subspan(i + 1));
            if (split.err) return {T{}, split.err};
            if (lastPart && split.endsInSlash) {
                // Symlink pointing to a directory: "a" -> "b/", say.
                endsInSlash = true;
            }
            if (split.parts.size() < i || !std::equal(parts.begin(), parts.begin() + std::ptrdiff_t(i), split.parts.begin())) {
                // Some component in the path which we have already
                // resolved has changed: restart from the root.
                i = 0;
                closer_.reset();
            }
            parts = std::move(split.parts);
            continue;
        }
        if (auto* e = dynamic_cast<const PathError*>(err.get())) {
            // This is strictly a heuristic: the path is the parts walked
            // so far.
            std::string path = parts[0];
            for (std::size_t k = 1; k <= i && k < parts.size(); k++) path += PathSeparator + parts[k];
            return {T{}, pathError(e->Op.view(), path, e->Err)};
        }
        return {T{}, err};
    }
}

ptr<Root> newRoot(int fd, string name, error* err) {
    detail::fileStat fs;
    int e = detail::sys([&] { return ::fstat(fd, &fs.sys); });
    detail::fillFileStat(fs, name.view());
    if (e == 0 && !fs.IsDir()) {
        ::close(fd);
        *err = pathError("open", name.view(), errors::New("not a directory"));
        return nullptr;
    }
    *err = nullptr;
    return make_ptr<Root>(std::make_unique<detail::rootImpl>(fd, std::move(name)));
}

// Go's rootFS: a Root as an fs.FS.
class rootFS final : public fs::StatFS, public fs::ReadFileFS, public fs::ReadDirFS, public fs::ReadLinkFS {
public:
    explicit rootFS(ptr<Root> r) : r_(std::move(r)) {}

    std::pair<ptr<fs::File>, error> Open(std::string_view name) override {
        if (!fs::ValidPath(name)) return {nullptr, pathError("open", name, ErrInvalid)};
        auto [f, err] = r_->Open(name);
        if (err) return {nullptr, err};
        return {f, nullptr};
    }

    std::pair<std::vector<ptr<DirEntry>>, error> ReadDir(std::string_view name) override {
        if (!fs::ValidPath(name)) return {{}, pathError("readdir", name, ErrInvalid)};
        auto [f, err] = r_->Open(name);
        if (err) return {{}, err};
        auto [dirs, rerr] = f->ReadDir(-1);
        f->Close();
        std::ranges::sort(dirs, [](const ptr<DirEntry>& a, const ptr<DirEntry>& b) {
            return a->Name().view() < b->Name().view();
        });
        return {dirs, rerr};
    }

    std::pair<slice<byte>, error> ReadFile(std::string_view name) override {
        if (!fs::ValidPath(name)) return {{}, pathError("readfile", name, ErrInvalid)};
        return r_->ReadFile(name);
    }

    std::pair<string, error> ReadLink(std::string_view name) override {
        if (!fs::ValidPath(name)) return {{}, pathError("readlink", name, ErrInvalid)};
        return r_->Readlink(name);
    }

    std::pair<ptr<FileInfo>, error> Stat(std::string_view name) override {
        if (!fs::ValidPath(name)) return {nullptr, pathError("stat", name, ErrInvalid)};
        return r_->Stat(name);
    }

    std::pair<ptr<FileInfo>, error> Lstat(std::string_view name) override {
        if (!fs::ValidPath(name)) return {nullptr, pathError("lstat", name, ErrInvalid)};
        return r_->Lstat(name);
    }

private:
    ptr<Root> r_;
};

error unsupportedMode(std::string_view op, std::string_view name) {
    return pathError(op, name, errors::New("unsupported file mode"));
}

}  // namespace

Root::Root(std::unique_ptr<detail::rootImpl> impl) noexcept : r_(std::move(impl)) {}

Root::~Root() = default;

const string& Root::Name() const noexcept { return r_->name; }

error Root::Close() { return r_->close(); }

std::pair<ptr<Root>, error> OpenRoot(std::string_view name) {
    std::string p;
    if (!detail::cpath(name, p)) return {nullptr, pathError("open", name, errnoError(EINVAL))};
    int e;
    int fd = openat(AT_FDCWD, p, O_RDONLY | O_CLOEXEC, 0, &e);
    if (e) return {nullptr, pathError("open", name, errnoError(e))};
    error err;
    auto r = newRoot(fd, string(name), &err);
    return {r, err};
}

std::pair<ptr<File>, error> OpenInRoot(std::string_view dir, std::string_view name) {
    auto [r, err] = OpenRoot(dir);
    if (err) return {nullptr, err};
    auto res = r->Open(name);
    r->Close();
    return res;
}

std::pair<ptr<File>, error> Root::Open(std::string_view name) { return OpenFile(name, O_RDONLY_, 0); }

std::pair<ptr<File>, error> Root::Create(std::string_view name) {
    return OpenFile(name, O_RDWR_ | O_CREATE_ | O_TRUNC_, 0666);
}

std::pair<ptr<File>, error> Root::OpenFile(std::string_view name, int flag, FileMode perm) {
    if ((perm & FileMode(0777)) != perm) return {nullptr, unsupportedMode("openat", name)};
    auto [fd, err] = doInRoot<int>(*r_, name, 0, nullptr, [&](int parent, const std::string& n, bool) -> std::pair<int, error> {
        int e;
        int openFlag = O_NOFOLLOW | O_CLOEXEC | flag;
        int fd = openat(parent, n, openFlag, mode_t(uint32(perm)), &e);
        if (!e) return {fd, nullptr};
        error err = errnoError(e);
        bool isCreateExcl = (flag & (O_CREATE_ | O_EXCL_)) == (O_CREATE_ | O_EXCL_);
        if (!isCreateExcl && (isNoFollowErr(e) || e == ENOTDIR)) err = checkSymlink(parent, n, err);
        // When an O_CREAT|O_EXCL file creation fails with ELOOP, the file
        // being created is a symlink: report that it exists.
        if (isCreateExcl && e == ELOOP) err = errnoError(EEXIST);
        return {-1, err};
    });
    if (err) return {nullptr, pathError("openat", name, err)};
    ptr<File> f = detail::newFile(fd, string(detail::joinPath(r_->name.view(), name)), detail::fileKind::openFile,
                                   flag & O_NONBLOCK, flag & O_APPEND_);
    detail::fileImpl::of(*f).inRoot = true;
    return {f, nullptr};
}

std::pair<ptr<Root>, error> Root::OpenRoot(std::string_view name) {
    auto [fd, err] = doInRoot<int>(*r_, name, 0, nullptr, [&](int parent, const std::string& n, bool) -> std::pair<int, error> {
        int e;
        int fd = openat(parent, n, O_RDONLY | O_NOFOLLOW | O_CLOEXEC, 0, &e);
        if (!e) return {fd, nullptr};
        error err = errnoError(e);
        if (isNoFollowErr(e)) err = checkSymlink(parent, n, err);
        return {-1, err};
    });
    if (err) return {nullptr, pathError("openat", name, err)};
    error nerr;
    auto r = newRoot(fd, string(detail::joinPath(r_->name.view(), name)), &nerr);
    return {r, nerr};
}

error Root::Chmod(std::string_view name, FileMode mode) {
    auto [_, err] = doInRoot<int>(*r_, name, 0, nullptr, [&](int parent, const std::string& n, bool) {
        return std::pair<int, error>{0, chmodat(parent, n, mode)};
    });
    if (err) return pathError("chmodat", name, err);
    return nullptr;
}

error Root::Chown(std::string_view name, int uid, int gid) {
    auto [_, err] = doInRoot<int>(*r_, name, 0, nullptr, [&](int parent, const std::string& n, bool) {
        return std::pair<int, error>{0, chownat(parent, n, uid, gid)};
    });
    if (err) return pathError("chownat", name, err);
    return nullptr;
}

error Root::Lchown(std::string_view name, int uid, int gid) {
    auto [_, err] = doInRoot<int>(*r_, name, 0, nullptr, [&](int parent, const std::string& n, bool) {
        return std::pair<int, error>{0, lchownat(parent, n, uid, gid)};
    });
    if (err) return pathError("lchownat", name, err);
    return nullptr;
}

error Root::Chtimes(std::string_view name, time::Time atime, time::Time mtime) {
    auto [_, err] = doInRoot<int>(*r_, name, 0, nullptr, [&](int parent, const std::string& n, bool) {
        return std::pair<int, error>{0, chtimesat(parent, n, atime, mtime)};
    });
    if (err) return pathError("chtimesat", name, err);
    return nullptr;
}

error Root::Mkdir(std::string_view name, FileMode perm) {
    if ((perm & FileMode(0777)) != perm) return unsupportedMode("mkdirat", name);
    // mkdirat("dir/") works on Linux: let it handle a terminal slash.
    unsigned flags = doInRootCreatingDirectory | doInRootNoHandleTerminalSlash;
    auto [_, err] = doInRoot<int>(*r_, name, flags, nullptr, [&](int parent, const std::string& n, bool) {
        return std::pair<int, error>{0, mkdirat(parent, n, perm)};
    });
    if (err) return pathError("mkdirat", name, err);
    return nullptr;
}

error Root::MkdirAll(std::string_view fullname, FileMode perm) {
    if ((perm & FileMode(0777)) != perm) return unsupportedMode("mkdirat", fullname);
    // openDirFunc opens an intermediate directory, creating it if it
    // doesn't exist.
    OpenDirFunc openDirFunc = [&](int parent, const std::string& name) -> std::pair<int, error> {
        for (int attempt = 0; attempt < 2; attempt++) {
            auto [fd, err] = rootOpenDir(parent, name);
            if (!err || asSymlink(err)) return {fd, err};
            if (attempt > 0 || !IsNotExist(err)) return {-1, pathError("openat", "", err)};
            if (error merr = mkdirat(parent, name, perm); merr && errnoOf(merr) != EEXIST) {
                return {-1, pathError("mkdirat", "", merr)};
            }
        }
        panic("unreachable");
    };
    // openLastComponentFunc opens or creates the last path component.
    auto openLastComponentFunc = [&](int parent, const std::string& name, bool) -> std::pair<int, error> {
        error err = mkdirat(parent, name, perm);
        if (errnoOf(err) == EEXIST) {
            auto [fi, e] = lstatat(parent, name);
            if (!e) {
                if (fi->Mode().IsDir()) {
                    // The target of MkdirAll is an existing directory.
                    err = nullptr;
                } else if (isLink(*fi)) {
                    // The target of MkdirAll is an existing symlink. For
                    // consistency with os.MkdirAll, succeed if the link
                    // resolves to a directory. We don't return errSymlink
                    // here, because we don't want to create the link
                    // target if it doesn't exist.
                    auto [fi2, e2] = Stat(fullname);
                    if (!e2 && fi2->Mode().IsDir()) err = nullptr;
                    else if (!e2) err = errnoError(ENOTDIR);
                    else if (!IsNotExist(e2)) err = e2;
                }
            }
        }
        if (!err || asSymlink(err)) return {0, err};
        return {0, pathError("mkdirat", "", err)};
    };
    unsigned flags = doInRootCreatingDirectory | doInRootNoHandleTerminalSlash;
    auto [_, err] = doInRoot<int>(*r_, fullname, flags, openDirFunc, openLastComponentFunc);
    if (err && !dynamic_cast<const PathError*>(err.get())) err = pathError("mkdirat", fullname, err);
    return err;
}

std::pair<string, error> Root::Readlink(std::string_view name) {
    auto [target, err] = doInRoot<std::string>(*r_, name, 0, nullptr, [&](int parent, const std::string& n, bool) {
        return readlinkat(parent, n);
    });
    if (err) return {{}, pathError("readlinkat", name, err)};
    return {string(target), nullptr};
}

error Root::Remove(std::string_view name) {
    auto [_, err] = doInRoot<int>(*r_, name, 0, nullptr, [&](int parent, const std::string& n, bool) {
        return std::pair<int, error>{0, removeat(parent, n)};
    });
    if (err) return pathError("removeat", name, err);
    return nullptr;
}

error Root::RemoveAll(std::string_view name) {
    // Consistency with os.RemoveAll: Strip trailing /s from the name, so
    // RemoveAll("not_a_directory/") succeeds.
    while (!name.empty() && IsPathSeparator(name.back())) name.remove_suffix(1);
    if (endsWithDot(name)) {
        // Consistency with os.RemoveAll: Return EINVAL when trying to
        // remove .
        return pathError("RemoveAll", name, errnoError(EINVAL));
    }
    auto [_, err] = doInRoot<int>(*r_, name, 0, nullptr, [&](int parent, const std::string& n, bool) {
        return std::pair<int, error>{0, detail::removeAllFrom(parent, n)};
    });
    if (IsNotExist(err)) return nullptr;
    if (err) return pathError("RemoveAll", name, underlyingError(err));
    return nullptr;
}

std::pair<ptr<FileInfo>, error> Root::Stat(std::string_view name) {
    auto [fi, err] = doInRoot<ptr<FileInfo>>(*r_, name, 0, nullptr,
                                             [&](int parent, const std::string& n, bool) -> std::pair<ptr<FileInfo>, error> {
                                                 auto [fi, err] = lstatatWithName(parent, name, n);
                                                 if (err) return {nullptr, err};
                                                 if (fi->Mode() & ModeSymlink) {
                                                     return {nullptr, checkSymlink(parent, n, errnoError(ELOOP))};
                                                 }
                                                 return {fi, nullptr};
                                             });
    if (err) return {nullptr, pathError("statat", name, err)};
    return {fi, nullptr};
}

std::pair<ptr<FileInfo>, error> Root::Lstat(std::string_view name) {
    auto [fi, err] = doInRoot<ptr<FileInfo>>(*r_, name, 0, nullptr, [&](int parent, const std::string& n, bool) {
        return lstatatWithName(parent, name, n);
    });
    if (err) return {nullptr, pathError("statat", name, err)};
    return {fi, nullptr};
}

error Root::Rename(std::string_view oldname, std::string_view newname) {
    auto [_, err] = doInRoot<int>(*r_, oldname, 0, nullptr, [&](int oldparent, const std::string& oldn, bool) {
        auto [_2, err] = doInRoot<int>(
            *r_, newname, doInRootCreatingDirectory, nullptr,
            [&](int newparent, const std::string& newn, bool newEndsInSlash) -> std::pair<int, error> {
                if (newEndsInSlash) {
                    // Linux and some others reject a trailing slash on the
                    // destination unless the source is a directory.
                    auto [oldMode, merr] = modeAt(oldparent, oldn);
                    if (merr) return {0, merr};
                    if (oldMode.Type() != ModeDir) return {0, errnoError(ENOTDIR)};
                }
                // On most platforms, renaming a file to an existing
                // directory fails, but some replace an empty directory.
                // Make it an error everywhere, as os.Rename does.
                auto [fi, lerr] = lstatat(newparent, newn);
                if (!lerr && fi->IsDir()) {
                    // A rename of a directory to itself (or another name
                    // of it) is not an error.
                    auto [ofi, oerr] = lstatat(oldparent, oldn);
                    if (oerr) return {0, oerr};
                    if (newn == oldn || !SameFile(fi, ofi)) return {0, errnoError(EEXIST)};
                }
                return {0, renameat(oldparent, oldn, newparent, newn)};
            });
        return std::pair<int, error>{0, err};
    });
    if (err) return make_ptr<LinkError>(string("renameat"), string(oldname), string(newname), err);
    return nullptr;
}

error Root::Link(std::string_view oldname, std::string_view newname) {
    auto [_, err] = doInRoot<int>(*r_, oldname, 0, nullptr, [&](int oldparent, const std::string& oldn, bool) {
        auto [_2, err] = doInRoot<int>(*r_, newname, 0, nullptr, [&](int newparent, const std::string& newn, bool) {
            return std::pair<int, error>{0, linkat(oldparent, oldn, newparent, newn)};
        });
        return std::pair<int, error>{0, err};
    });
    if (err) return make_ptr<LinkError>(string("linkat"), string(oldname), string(newname), err);
    return nullptr;
}

error Root::Symlink(std::string_view oldname, std::string_view newname) {
    auto [_, err] = doInRoot<int>(*r_, newname, 0, nullptr, [&](int parent, const std::string& n, bool) {
        return std::pair<int, error>{0, symlinkat(oldname, parent, n)};
    });
    if (err) return make_ptr<LinkError>(string("symlinkat"), string(oldname), string(newname), err);
    return nullptr;
}

std::pair<slice<byte>, error> Root::ReadFile(std::string_view name) {
    auto [f, err] = Open(name);
    if (err) return {{}, err};
    int64 statSize = 0;
    if (auto [fi, e] = f->Stat(); !e) statSize = fi->Size();
    auto r = detail::readFileContents(statSize, [&](std::span<byte> p) { return f->Read(p); });
    f->Close();
    return r;
}

error Root::WriteFile(std::string_view name, std::span<const byte> data, FileMode perm) {
    auto [f, err] = OpenFile(name, O_WRONLY_ | O_CREATE_ | O_TRUNC_, perm);
    if (err) return err;
    auto [n, werr] = f->Write(data);
    error cerr = f->Close();
    return werr ? werr : cerr;
}

error Root::WriteFile(std::string_view name, std::string_view data, FileMode perm) {
    return WriteFile(name, as_bytes(data), perm);
}

ptr<fs::FS> Root::FS() { return make_ptr<rootFS>(ptr<Root>(this)); }

}  // namespace gopp::os
