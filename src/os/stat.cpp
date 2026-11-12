// Stat, Lstat and SameFile, after Go's os/stat_unix.go and stat_linux.go.
#include "internal.hpp"

#include <fcntl.h>
#include <sys/stat.h>

namespace gopp::os {

namespace detail {

void fill_file_stat(file_stat& fs, std::string_view name) {
    // filepathlite.Base: the last element, without trailing slashes.
    while (name.size() > 1 && name.back() == '/') name.remove_suffix(1);
    if (auto i = name.rfind('/'); i != std::string_view::npos && name.size() > 1) name = name.substr(i + 1);
    if (name.empty()) name = ".";
    fs.name = string(name);
    fs.size = fs.sys.st_size;
    fs.mod_time = time::Time::Unix(fs.sys.st_mtim.tv_sec, fs.sys.st_mtim.tv_nsec);
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

std::pair<ptr<FileInfo>, error> lstatat(File& f, file_impl& fi, std::string_view name) {
    std::string p;
    auto st = make_ptr<file_stat>();
    error e = cpath(name, p) ? fi.pfd.Fstatat(p.c_str(), &st->sys, AT_SYMLINK_NOFOLLOW) : errno_error(EINVAL);
    if (e) {
        if (e == poll::ErrFileClosing) e = ErrClosed;
        return {nullptr, path_error("fstatat", join_path(f.Name().view(), name), e)};
    }
    fill_file_stat(*st, name);
    return {st, nullptr};
}

}  // namespace detail

namespace {

template <typename F>
std::pair<ptr<FileInfo>, error> stat_path(std::string_view op, std::string_view name, F&& f) {
    std::string p;
    if (!detail::cpath(name, p)) return {nullptr, detail::path_error(op, name, detail::errno_error(EINVAL))};
    auto st = make_ptr<detail::file_stat>();
    if (int e = detail::sys([&] { return f(p.c_str(), &st->sys); })) {
        return {nullptr, detail::path_error(op, name, detail::errno_error(e))};
    }
    detail::fill_file_stat(*st, name);
    return {st, nullptr};
}

}  // namespace

std::pair<ptr<FileInfo>, error> Stat(std::string_view name) {
    return stat_path("stat", name, [](const char* p, struct stat* st) { return ::stat(p, st); });
}

std::pair<ptr<FileInfo>, error> Lstat(std::string_view name) {
    return stat_path("lstat", name, [](const char* p, struct stat* st) { return ::lstat(p, st); });
}

std::pair<ptr<FileInfo>, error> File::Stat() {
    auto st = make_ptr<detail::file_stat>();
    if (error e = f_->pfd.Fstat(&st->sys)) return {nullptr, wrap_err("stat", std::move(e))};
    detail::fill_file_stat(*st, f_->name.view());
    return {st, nullptr};
}

bool SameFile(const ptr<FileInfo>& fi1, const ptr<FileInfo>& fi2) noexcept {
    auto* a = dynamic_cast<const detail::file_stat*>(fi1.get());
    auto* b = dynamic_cast<const detail::file_stat*>(fi2.get());
    if (!a || !b) return false;
    return a->sys.st_dev == b->sys.st_dev && a->sys.st_ino == b->sys.st_ino;
}

}  // namespace gopp::os
