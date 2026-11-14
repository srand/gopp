// MkdirAll and RemoveAll, after Go's os/path.go, path_unix.go and
// removeall_at.go.
#include "internal.hpp"

#include <gopp/io.hpp>

#include <fcntl.h>
#include <unistd.h>

#include <string>

namespace gopp::os {

error MkdirAll(std::string_view path, FileMode perm) {
    // Fast path: if it's already a directory, nothing to do.
    auto [dir, err] = Stat(path);
    if (!err) {
        if (dir->IsDir()) return nullptr;
        return detail::pathError("mkdir", path, detail::errnoError(ENOTDIR));
    }

    // Slow path: make sure the parent exists, then make this one.
    std::ptrdiff_t i = static_cast<std::ptrdiff_t>(path.size()) - 1;
    while (i >= 0 && IsPathSeparator(path[static_cast<std::size_t>(i)])) i--;  // skip trailing separators
    while (i >= 0 && !IsPathSeparator(path[static_cast<std::size_t>(i)])) i--;  // skip the last element
    if (i < 0) i = 0;
    if (std::string_view parent = path.substr(0, static_cast<std::size_t>(i)); !parent.empty()) {
        if (error err = MkdirAll(parent, perm)) return err;
    }

    if (error err = Mkdir(path, perm)) {
        // Handle arguments like "foo/." by double-checking that the
        // directory doesn't exist.
        auto [dir1, err1] = Lstat(path);
        if (!err1 && dir1->IsDir()) return nullptr;
        return err;
    }
    return nullptr;
}

namespace detail {

// Go's splitPath: the parent directory and the base name of path.
std::pair<std::string_view, std::string_view> splitPath(std::string_view path) {
    std::string_view dirname = ".";
    // Leading slashes beyond one don't matter.
    while (path.size() > 1 && path[0] == '/' && path[1] == '/') path.remove_prefix(1);
    std::ptrdiff_t i = static_cast<std::ptrdiff_t>(path.size()) - 1;
    // Remove trailing slashes.
    for (; i > 0 && path[static_cast<std::size_t>(i)] == '/'; i--) path = path.substr(0, static_cast<std::size_t>(i));
    std::string_view basename = path;
    for (i--; i >= 0; i--) {
        if (path[static_cast<std::size_t>(i)] == '/') {
            dirname = i == 0 ? path.substr(0, 1) : path.substr(0, static_cast<std::size_t>(i));
            basename = path.substr(static_cast<std::size_t>(i) + 1);
            break;
        }
    }
    return {dirname, basename};
}

}  // namespace detail

namespace {

bool endsWithDot(std::string_view path) {
    if (path == ".") return true;
    return path.size() >= 2 && path.back() == '.' && IsPathSeparator(path[path.size() - 2]);
}

int unlinkat(int dirfd, const std::string& name, int flags) {
    return detail::unlinkAt(dirfd, name.c_str(), flags);
}

// Go's rootOpenDir, for RemoveAll: opens name in dirfd as a directory
// without following a symbolic link. Returns the descriptor, or -1 and the
// errno, with symlink set if name is a symbolic link.
int openDirAt(int dirfd, const std::string& name, int* err, bool* symlink) {
    *symlink = false;
    int fd = detail::openAt(dirfd, name.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_DIRECTORY, 0, err);
    if (*err == ELOOP || *err == ENOTDIR) {
        char buf[1];
        if (::readlinkat(dirfd, name.c_str(), buf, sizeof(buf)) >= 0) *symlink = true;
    } else if (*err == ENOTSUP || *err == EOPNOTSUPP) {
        *err = ENOTDIR;
    }
    return *err ? -1 : fd;
}

// Prefixes the path of a PathError with dir.
error prefixPath(error err, std::string_view dir) {
    if (auto* pe = dynamic_cast<const PathError*>(err.get())) {
        return detail::pathError(pe->Op.view(), detail::joinPath(dir, pe->Path.view()), pe->Err);
    }
    return err;
}

error removeAllFrom(int parentFd, const std::string& base) {
    // Simple case: a file, or an empty directory where unlinking works.
    int e = unlinkat(parentFd, base, 0);
    if (e == 0 || e == ENOENT) return nullptr;
    // EISDIR means the file is a directory; EPERM or EACCES may mean it is
    // too, or that we can't remove it (Linux returns EISDIR, others EPERM).
    if (e != EISDIR && e != EPERM && e != EACCES) return detail::pathError("unlinkat", base, detail::errnoError(e));
    int uErr = e;

    // Remove the directory's entries.
    error recurseErr;
    for (;;) {
        constexpr int reqSize = 1024;
        std::size_t respSize = 0;

        int oerr;
        bool symlink;
        int fd = openDirAt(parentFd, base, &oerr, &symlink);
        if (fd < 0) {
            if (oerr == ENOENT) return nullptr;
            if (oerr == ENOTDIR || symlink) return detail::pathError("unlinkat", base, detail::errnoError(uErr));
            recurseErr = detail::pathError("openfdat", base, detail::errnoError(oerr));
            break;
        }
        ptr<File> file = detail::newFile(fd, string(base), detail::fileKind::noPoll, false);

        for (;;) {
            int numErr = 0;
            auto [names, readErr] = file->Readdirnames(reqSize);
            // Errors other than EOF should stop us from continuing.
            if (readErr && readErr != io::EOF_) {
                file->Close();
                if (IsNotExist(readErr)) return nullptr;
                return detail::pathError("readdirnames", base, readErr);
            }
            respSize = names.size();
            for (auto& name : names) {
                if (error err = removeAllFrom(fd, std::string(name.view()))) {
                    numErr++;
                    if (!recurseErr) recurseErr = prefixPath(err, base);
                }
            }
            // If we can delete any entry, break to start a new iteration;
            // otherwise skip this batch of undeletable names.
            if (numErr != reqSize) break;
        }

        // Removing files from the directory may have caused the OS to
        // reshuffle it: reopen and read again.
        file->Close();

        // Finish when the end of the directory is reached.
        if (respSize < static_cast<std::size_t>(reqSize)) break;
    }

    // Remove the directory itself.
    int unlinkErr = unlinkat(parentFd, base, AT_REMOVEDIR);
    if (unlinkErr == 0 || unlinkErr == ENOENT) return nullptr;
    if (recurseErr) return recurseErr;
    return detail::pathError("unlinkat", base, detail::errnoError(unlinkErr));
}

}  // namespace

error detail::removeAllFrom(int parentFd, const std::string& base) { return os::removeAllFrom(parentFd, base); }

error RemoveAll(std::string_view path) {
    if (path.empty()) return nullptr;  // fail silently to retain compatibility
    // The rmdir system call doesn't permit removing ".", so we don't permit
    // it either.
    if (endsWithDot(path)) return detail::pathError("RemoveAll", path, detail::errnoError(EINVAL));
    if (path.find('\0') != std::string_view::npos) return detail::pathError("RemoveAll", path, detail::errnoError(EINVAL));

    // Simple case: if Remove works, we're done.
    error err = Remove(path);
    if (!err || IsNotExist(err)) return nullptr;

    // RemoveAll recurses by deleting the path base from its parent
    // directory.
    auto [parentDir, base] = detail::splitPath(path);
    auto [parent, perr] = OpenFile(parentDir, O_RDONLY_, 0);
    if (IsNotExist(perr)) return nullptr;  // if parent does not exist, base cannot exist
    if (perr) return perr;

    error rerr = removeAllFrom(static_cast<int>(parent->Fd()), std::string(base));
    parent->Close();
    if (rerr) return prefixPath(rerr, parentDir);
    return nullptr;
}

}  // namespace gopp::os
