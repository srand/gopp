// OsFs, ported from afero v1.15.0's os.go.
#include "internal.hpp"

namespace gopp::vfs {

namespace {

// An os::File as a vfs::File. It passes io::Copy's ReaderFrom and WriterTo
// on, for copy_file_range(2), splice(2) and sendfile(2).
class osFile final : public File, public io::ReaderFrom, public io::WriterTo {
public:
    explicit osFile(ptr<os::File> f) : f_(std::move(f)) {}

    std::pair<isize, error> Read(std::span<byte> p) override { return f_->Read(p); }
    std::pair<isize, error> ReadAt(std::span<byte> p, int64 off) override { return f_->ReadAt(p, off); }
    std::pair<isize, error> Write(std::span<const byte> p) override { return f_->Write(p); }
    std::pair<isize, error> WriteAt(std::span<const byte> p, int64 off) override { return f_->WriteAt(p, off); }
    std::pair<int64, error> Seek(int64 offset, int whence) override { return f_->Seek(offset, whence); }
    error Close() override { return f_->Close(); }
    std::pair<ptr<fs::FileInfo>, error> Stat() override { return f_->Stat(); }
    string Name() const override { return f_->Name(); }
    std::pair<std::vector<ptr<fs::FileInfo>>, error> Readdir(int count) override { return f_->Readdir(count); }
    std::pair<std::vector<string>, error> Readdirnames(int n) override { return f_->Readdirnames(n); }
    std::pair<std::vector<ptr<fs::DirEntry>>, error> ReadDir(int n) override { return f_->ReadDir(n); }
    error Sync() override { return f_->Sync(); }
    error Truncate(int64 size) override { return f_->Truncate(size); }
    std::pair<isize, error> WriteString(std::string_view s) override { return f_->WriteString(s); }
    std::pair<int64, error> ReadFrom(io::Reader& r) override { return f_->ReadFrom(r); }
    std::pair<int64, error> WriteTo(io::Writer& w) override { return f_->WriteTo(w); }

private:
    ptr<os::File> f_;
};

std::pair<ptr<File>, error> wrap(std::pair<ptr<os::File>, error> r) {
    if (!r.first) return {nullptr, r.second};
    return {make_ptr<osFile>(std::move(r.first)), r.second};
}

}  // namespace

ptr<Fs> NewOsFs() { return make_ptr<OsFs>(); }

std::pair<ptr<File>, error> OsFs::Create(std::string_view name) { return wrap(os::Create(name)); }
error OsFs::Mkdir(std::string_view name, fs::FileMode perm) { return os::Mkdir(name, perm); }
error OsFs::MkdirAll(std::string_view path, fs::FileMode perm) { return os::MkdirAll(path, perm); }
std::pair<ptr<File>, error> OsFs::Open(std::string_view name) { return wrap(os::Open(name)); }
std::pair<ptr<File>, error> OsFs::OpenFile(std::string_view name, int flag, fs::FileMode perm) {
    return wrap(os::OpenFile(name, flag, perm));
}
error OsFs::Remove(std::string_view name) { return os::Remove(name); }
error OsFs::RemoveAll(std::string_view path) { return os::RemoveAll(path); }
error OsFs::Rename(std::string_view oldname, std::string_view newname) { return os::Rename(oldname, newname); }
std::pair<ptr<fs::FileInfo>, error> OsFs::Stat(std::string_view name) { return os::Stat(name); }
error OsFs::Chmod(std::string_view name, fs::FileMode mode) { return os::Chmod(name, mode); }
error OsFs::Chown(std::string_view name, int uid, int gid) { return os::Chown(name, uid, gid); }
error OsFs::Chtimes(std::string_view name, time::Time atime, time::Time mtime) { return os::Chtimes(name, atime, mtime); }

std::tuple<ptr<fs::FileInfo>, bool, error> OsFs::LstatIfPossible(std::string_view name) {
    auto [fi, err] = os::Lstat(name);
    return {fi, true, err};
}

error OsFs::SymlinkIfPossible(std::string_view oldname, std::string_view newname) {
    return os::Symlink(oldname, newname);
}

std::pair<string, error> OsFs::ReadlinkIfPossible(std::string_view name) { return os::Readlink(name); }

}  // namespace gopp::vfs
