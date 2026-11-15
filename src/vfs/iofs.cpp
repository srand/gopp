// IOFS and FromIOFS, ported from afero v1.15.0's iofs.go.
#include "internal.hpp"

#include <gopp/path.hpp>

#include <algorithm>

namespace gopp::vfs {

// ---------------------------------------------------------------------------
// IOFS

namespace {

// err, as a PathError unless it is one.
error wrapError(std::string_view op, std::string_view path, error err) {
    if (dynamic_cast<const fs::PathError*>(err.get())) return err;
    return detail::pathError(op, path, err);
}

}  // namespace

ptr<IOFS> NewIOFS(ptr<vfs::Fs> fsys) { return make_ptr<IOFS>(std::move(fsys)); }

std::pair<ptr<fs::File>, error> IOFS::Open(std::string_view name) {
    // As fs::FS implementations do.
    if (!fs::ValidPath(name)) return {nullptr, wrapError("open", name, fs::ErrInvalid)};
    auto [file, err] = Fs->Open(name);
    if (err) return {nullptr, wrapError("open", name, err)};
    return {file, nullptr};
}

std::pair<ptr<fs::FileInfo>, error> IOFS::Stat(std::string_view name) { return Fs->Stat(name); }

std::pair<std::vector<string>, error> IOFS::Glob(std::string_view pattern) {
    // vfs::Glob doesn't check, but fs::GlobFS must.
    if (auto [_, err] = path::Match(pattern, ""); err) return {{}, wrapError("glob", pattern, err)};
    auto [items, err] = vfs::Glob(*Fs, pattern);
    if (err) return {{}, wrapError("glob", pattern, err)};
    return {items, nullptr};
}

std::pair<std::vector<ptr<fs::DirEntry>>, error> IOFS::ReadDir(std::string_view name) {
    auto [f, err] = Fs->Open(name);
    if (err) return {{}, wrapError("readdir", name, err)};
    auto [items, rerr] = f->ReadDir(-1);
    f->Close();
    if (rerr) return {{}, wrapError("readdir", name, rerr)};
    std::sort(items.begin(), items.end(),
              [](const ptr<fs::DirEntry>& a, const ptr<fs::DirEntry>& b) { return a->Name() < b->Name(); });
    return {items, nullptr};
}

std::pair<slice<byte>, error> IOFS::ReadFile(std::string_view name) {
    if (!fs::ValidPath(name)) return {{}, wrapError("readfile", name, fs::ErrInvalid)};
    auto [bytes, err] = vfs::ReadFile(*Fs, name);
    if (err) return {{}, wrapError("readfile", name, err)};
    return {bytes, nullptr};
}

std::pair<ptr<fs::FS>, error> IOFS::Sub(std::string_view dir) {
    return {make_ptr<IOFS>(NewBasePathFs(Fs, dir)), nullptr};
}

// ---------------------------------------------------------------------------
// FromIOFS

namespace {

error notImplemented(std::string_view op, std::string_view path) {
    return detail::pathError(op, path, fs::ErrPermission);
}

// A file of an fs::FS as a vfs::File.
class fromIOFSFile final : public File {
public:
    fromIOFSFile(ptr<fs::File> f, string name) : f_(std::move(f)), name_(std::move(name)) {}

    std::pair<isize, error> Read(std::span<byte> p) override { return f_->Read(p); }
    error Close() override { return f_->Close(); }
    std::pair<ptr<fs::FileInfo>, error> Stat() override { return f_->Stat(); }

    std::pair<isize, error> ReadAt(std::span<byte> p, int64 off) override {
        auto* readerAt = dynamic_cast<io::ReaderAt*>(f_.get());
        if (!readerAt) return {-1, notImplemented("readat", name_)};
        return readerAt->ReadAt(p, off);
    }

    std::pair<int64, error> Seek(int64 offset, int whence) override {
        auto* seeker = dynamic_cast<io::Seeker*>(f_.get());
        if (!seeker) return {-1, notImplemented("seek", name_)};
        return seeker->Seek(offset, whence);
    }

    std::pair<isize, error> Write(std::span<const byte>) override { return {-1, notImplemented("write", name_)}; }
    std::pair<isize, error> WriteAt(std::span<const byte>, int64) override {
        return {-1, notImplemented("writeat", name_)};
    }
    string Name() const override { return name_; }

    std::pair<std::vector<ptr<fs::FileInfo>>, error> Readdir(int count) override {
        auto* rdfile = dynamic_cast<fs::ReadDirFile*>(f_.get());
        if (!rdfile) return {{}, notImplemented("readdir", name_)};
        auto [entries, err] = rdfile->ReadDir(count);
        if (err) return {{}, err};
        std::vector<ptr<fs::FileInfo>> ret;
        ret.reserve(entries.size());
        for (auto& e : entries) {
            auto [info, ierr] = e->Info();
            if (ierr) return {{}, ierr};
            ret.push_back(info);
        }
        return {ret, nullptr};
    }

    std::pair<std::vector<string>, error> Readdirnames(int n) override {
        auto* rdfile = dynamic_cast<fs::ReadDirFile*>(f_.get());
        if (!rdfile) return {{}, notImplemented("readdir", name_)};
        auto [entries, err] = rdfile->ReadDir(n);
        if (err) return {{}, err};
        std::vector<string> ret;
        ret.reserve(entries.size());
        for (auto& e : entries) ret.push_back(e->Name());
        return {ret, nullptr};
    }

    error Sync() override { return nullptr; }
    error Truncate(int64) override { return notImplemented("truncate", name_); }
    std::pair<isize, error> WriteString(std::string_view) override { return {-1, notImplemented("writestring", name_)}; }

private:
    ptr<fs::File> f_;
    string name_;
};

}  // namespace

std::pair<ptr<File>, error> FromIOFS::Create(std::string_view name) { return {nullptr, notImplemented("create", name)}; }
error FromIOFS::Mkdir(std::string_view name, fs::FileMode) { return notImplemented("mkdir", name); }
error FromIOFS::MkdirAll(std::string_view path, fs::FileMode) { return notImplemented("mkdirall", path); }

std::pair<ptr<File>, error> FromIOFS::Open(std::string_view name) {
    auto [file, err] = FS->Open(name);
    if (err) return {nullptr, err};
    return {make_ptr<fromIOFSFile>(file, string(name)), nullptr};
}

std::pair<ptr<File>, error> FromIOFS::OpenFile(std::string_view name, int, fs::FileMode) { return Open(name); }
error FromIOFS::Remove(std::string_view name) { return notImplemented("remove", name); }
error FromIOFS::RemoveAll(std::string_view path) { return notImplemented("removeall", path); }
error FromIOFS::Rename(std::string_view oldname, std::string_view) { return notImplemented("rename", oldname); }
std::pair<ptr<fs::FileInfo>, error> FromIOFS::Stat(std::string_view name) { return fs::Stat(*FS, name); }
error FromIOFS::Chmod(std::string_view name, fs::FileMode) { return notImplemented("chmod", name); }
error FromIOFS::Chown(std::string_view name, int, int) { return notImplemented("chown", name); }
error FromIOFS::Chtimes(std::string_view name, time::Time, time::Time) { return notImplemented("chtimes", name); }

}  // namespace gopp::vfs
