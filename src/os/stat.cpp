// Stat, Lstat and SameFile, after Go's os/stat_unix.go and stat_linux.go.
#include "internal.hpp"

#include <fcntl.h>
#include <sys/stat.h>

namespace gopp::os {

namespace detail {

void fillFileStat(fileStat& fs, std::string_view name) {
    // filepathlite.Base: the last element, without trailing slashes.
    while (name.size() > 1 && name.back() == '/') name.remove_suffix(1);
    if (auto i = name.rfind('/'); i != std::string_view::npos && name.size() > 1) name = name.substr(i + 1);
    if (name.empty()) name = ".";
    fs.name = string(name);
    fs.size = fs.sys.st_size;
    fs.modTime = time::Time::Unix(fs.sys.st_mtim.tv_sec, fs.sys.st_mtim.tv_nsec);
    FileMode m = fs.sys.st_mode & 0777;
    switch (fs.sys.st_mode & S_IFMT) {
    case S_IFBLK: m |= ModeDevice; break;
    case S_IFCHR: m |= ModeDevice | ModeCharDevice; break;
    case S_IFDIR: m |= ModeDir; break;
    case S_IFIFO: m |= ModeNamedPipe; break;
    case S_IFLNK: m |= ModeSymlink; break;
    case S_IFREG: break;
    case S_IFSOCK: m |= ModeSocket; break;
    }
    if (fs.sys.st_mode & S_ISGID) m |= ModeSetgid;
    if (fs.sys.st_mode & S_ISUID) m |= ModeSetuid;
    if (fs.sys.st_mode & S_ISVTX) m |= ModeSticky;
    fs.mode = m;
}

std::pair<ptr<FileInfo>, error> lstatat(File& f, fileImpl& fi, std::string_view name) {
    std::string p;
    auto st = make_ptr<fileStat>();
    error e = cpath(name, p) ? fi.pfd.Fstatat(p.c_str(), &st->sys, AT_SYMLINK_NOFOLLOW) : errnoError(EINVAL);
    if (e) {
        if (e == poll::ErrFileClosing) e = ErrClosed;
        return {nullptr, pathError("fstatat", joinPath(f.Name().view(), name), e)};
    }
    fillFileStat(*st, name);
    return {st, nullptr};
}

}  // namespace detail

namespace {

std::pair<ptr<FileInfo>, error> statPath(std::string_view op, std::string_view name, int flags) {
    std::string p;
    if (!detail::cpath(name, p)) return {nullptr, detail::pathError(op, name, detail::errnoError(EINVAL))};
    auto st = make_ptr<detail::fileStat>();
    if (int e = poll::statat(AT_FDCWD, p.c_str(), &st->sys, flags)) {
        return {nullptr, detail::pathError(op, name, detail::errnoError(e))};
    }
    detail::fillFileStat(*st, name);
    return {st, nullptr};
}

}  // namespace

std::pair<ptr<FileInfo>, error> Stat(std::string_view name) {
    return statPath("stat", name, 0);
}

std::pair<ptr<FileInfo>, error> Lstat(std::string_view name) {
    return statPath("lstat", name, AT_SYMLINK_NOFOLLOW);
}

std::pair<ptr<FileInfo>, error> File::Stat() {
    auto st = make_ptr<detail::fileStat>();
    if (error e = f_->pfd.Fstat(&st->sys)) return {nullptr, wrapErr("stat", std::move(e))};
    detail::fillFileStat(*st, f_->name.view());
    return {st, nullptr};
}

bool SameFile(const ptr<FileInfo>& fi1, const ptr<FileInfo>& fi2) noexcept {
    auto* a = dynamic_cast<const detail::fileStat*>(fi1.get());
    auto* b = dynamic_cast<const detail::fileStat*>(fi2.get());
    if (!a || !b) return false;
    return a->sys.st_dev == b->sys.st_dev && a->sys.st_ino == b->sys.st_ino;
}

}  // namespace gopp::os
