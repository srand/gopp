// Reading directories, after Go's os/dir.go and dir_unix.go.
#include "internal.hpp"

#include <gopp/io.hpp>

#include <dirent.h>
#include <fcntl.h>

#include <algorithm>
#include <cstddef>
#include <cstring>

namespace gopp::os {

namespace {

constexpr std::size_t blockSize = 8192;

enum class readdirMode { name, dirEntry, fileInfo };

// Go's direntType: the type bits of a dirent, or ~0 if unknown.
FileMode direntType(unsigned char t) {
    switch (t) {
    case DT_BLK: return ModeDevice;
    case DT_CHR: return ModeDevice | ModeCharDevice;
    case DT_DIR: return ModeDir;
    case DT_FIFO: return ModeNamedPipe;
    case DT_LNK: return ModeSymlink;
    case DT_REG: return 0;
    case DT_SOCK: return ModeSocket;
    }
    return ~uint32(0);
}

// Go's unixDirent.
class unixDirent final : public DirEntry {
public:
    unixDirent(string parent, string name, FileMode typ) : parent_(std::move(parent)), name_(std::move(name)), typ_(typ) {}
    unixDirent(string parent, string name, ptr<FileInfo> info)
        : parent_(std::move(parent)), name_(std::move(name)), typ_(info->Mode().Type()), info_(std::move(info)) {}

    string Name() const override { return name_; }
    bool IsDir() const override { return typ_.IsDir(); }
    FileMode Type() const override { return typ_; }
    std::pair<ptr<FileInfo>, error> Info() const override {
        if (info_) return {info_, nullptr};
        return Lstat(detail::joinPath(parent_.view(), name_.view()));
    }

private:
    string parent_;
    string name_;
    FileMode typ_;
    ptr<FileInfo> info_;
};

// The getdents64 record header (struct linux_dirent64).
struct dirent64Header {
    uint64_t dIno;
    int64_t dOff;
    unsigned short dReclen;
    unsigned char dType;
};
constexpr std::size_t nameOffset = offsetof(dirent64Header, dType) + 1;

}  // namespace

// Go's File.readdir: reads up to n entries (all if n <= 0) in the given
// form. With n > 0, no entries at the end of the directory is io::EOF_.
template <typename Out, typename Add>
static error readdir(File& f, detail::fileImpl& fi, int n, Out& out, Add&& add) {
    std::lock_guard l(fi.dirMu);
    if (!fi.dir) fi.dir = std::make_unique<detail::dirInfo>();
    detail::dirInfo& d = *fi.dir;
    if (d.buf.empty()) d.buf.resize(blockSize);
    bool limited = n > 0;
    if (n == 0) n = -1;
    while (n != 0) {
        // Refill the buffer.
        if (d.bufp >= d.nbuf) {
            d.bufp = 0;
            auto [nb, err] = fi.pfd.ReadDirent(d.buf);
            if (err) {
                if (err == poll::ErrFileClosing) err = ErrClosed;
                return detail::pathError("readdirent", f.Name().view(), err);
            }
            d.nbuf = static_cast<std::size_t>(nb);
            if (d.nbuf == 0) {
                d.buf.clear();
                d.buf.shrink_to_fit();
                break;  // EOF
            }
        }
        // Drain the buffer.
        const byte* rec = d.buf.data() + d.bufp;
        std::size_t avail = d.nbuf - d.bufp;
        if (avail < nameOffset) break;
        dirent64Header h;
        std::memcpy(&h, rec, sizeof(h));
        if (h.dReclen > avail || h.dReclen < nameOffset) break;
        d.bufp += h.dReclen;
        std::size_t namelen = ::strnlen(reinterpret_cast<const char*>(rec) + nameOffset, h.dReclen - nameOffset);
        std::string_view name(reinterpret_cast<const char*>(rec) + nameOffset, namelen);
        if (name == "." || name == "..") continue;
        if (n > 0) n--;
        if (error err = add(name, h.dType)) {
            if (IsNotExist(err)) continue;  // removed since the directory was read
            return err;
        }
    }
    if (limited && out.empty()) return io::EOF_;
    return nullptr;
}

std::pair<std::vector<string>, error> File::Readdirnames(int n) {
    std::vector<string> names;
    error err = readdir(*this, *f_, n, names, [&](std::string_view name, unsigned char) -> error {
        names.emplace_back(name);
        return nullptr;
    });
    return {std::move(names), err};
}

std::pair<std::vector<ptr<DirEntry>>, error> File::ReadDir(int n) {
    std::vector<ptr<DirEntry>> dirents;
    error err = readdir(*this, *f_, n, dirents, [&](std::string_view name, unsigned char t) -> error {
        FileMode typ = direntType(t);
        // In a Root, the lazy Lstat of a name can't be used: lstatat now.
        if (typ != FileMode(~uint32(0)) && !f_->inRoot) {
            dirents.push_back(make_ptr<unixDirent>(f_->name, string(name), typ));
            return nullptr;
        }
        // The file system doesn't say: ask.
        auto [info, err] = detail::lstatat(*this, *f_, name);
        if (err) return err;
        dirents.push_back(make_ptr<unixDirent>(f_->name, string(name), std::move(info)));
        return nullptr;
    });
    return {std::move(dirents), err};
}

std::pair<std::vector<ptr<FileInfo>>, error> File::Readdir(int n) {
    std::vector<ptr<FileInfo>> infos;
    error err = readdir(*this, *f_, n, infos, [&](std::string_view name, unsigned char) -> error {
        auto [info, err] = detail::lstatat(*this, *f_, name);
        if (err) return err;
        infos.push_back(std::move(info));
        return nullptr;
    });
    return {std::move(infos), err};
}

std::pair<std::vector<ptr<DirEntry>>, error> ReadDir(std::string_view name) {
    // Go's openDir: O_DIRECTORY, never polled.
    std::string p;
    if (!detail::cpath(name, p)) return {{}, detail::pathError("open", name, detail::errnoError(EINVAL))};
    int e;
    int fd = detail::openAt(AT_FDCWD, p.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY, 0, &e);
    if (e) return {{}, detail::pathError("open", name, detail::errnoError(e))};
    ptr<File> f = detail::newFile(fd, string(name), detail::fileKind::noPoll, false);
    auto [dirs, err] = f->ReadDir(-1);
    f->Close();
    std::sort(dirs.begin(), dirs.end(), [](const ptr<DirEntry>& a, const ptr<DirEntry>& b) {
        return a->Name().view() < b->Name().view();
    });
    return {std::move(dirs), err};
}

}  // namespace gopp::os
