// BasePathFs, ported from afero v1.15.0's basepath.go.
#include "internal.hpp"

#include <gopp/strings.hpp>

namespace gopp::vfs {

namespace {

// Whether the cleaned path p is base or under it. afero's test is
// strings.HasPrefix(p, base), which lets "../base-other" out.
bool within(std::string_view p, std::string_view base) {
    if (base == "/") return true;
    if (base == ".") return p != ".." && !p.starts_with("../") && !p.starts_with("/");
    return p == base || (p.starts_with(base) && p[base.size()] == '/');
}

// vfs.BasePathFile: a file of a BasePathFs, named without the base.
class basePathFile final : public detail::fileWrapper {
public:
    basePathFile(ptr<File> f, string path) : fileWrapper(std::move(f)), path_(std::move(path)) {}

    string Name() const override {
        string sourcename = f_->Name();
        return string(strings::TrimPrefix(sourcename, filepath::Clean(path_)));
    }

private:
    string path_;
};

}  // namespace

BasePathFs::BasePathFs(ptr<Fs> source, std::string_view path) : source_(std::move(source)), path_(path) {}

ptr<Fs> NewBasePathFs(ptr<Fs> source, std::string_view path) { return make_ptr<BasePathFs>(std::move(source), path); }

std::pair<string, error> BasePathFs::RealPath(std::string_view name) const {
    string bpath = filepath::Clean(path_);
    string path = filepath::Clean(filepath::Join(bpath, name));
    if (!within(path, bpath)) return {string(name), fs::ErrNotExist};
    return {path, nullptr};
}

error BasePathFs::Chtimes(std::string_view name, time::Time atime, time::Time mtime) {
    auto [path, err] = RealPath(name);
    if (err) return detail::pathError("chtimes", path, err);
    return source_->Chtimes(path, atime, mtime);
}

error BasePathFs::Chmod(std::string_view name, fs::FileMode mode) {
    auto [path, err] = RealPath(name);
    if (err) return detail::pathError("chmod", path, err);
    return source_->Chmod(path, mode);
}

error BasePathFs::Chown(std::string_view name, int uid, int gid) {
    auto [path, err] = RealPath(name);
    if (err) return detail::pathError("chown", path, err);
    return source_->Chown(path, uid, gid);
}

std::pair<ptr<fs::FileInfo>, error> BasePathFs::Stat(std::string_view name) {
    auto [path, err] = RealPath(name);
    if (err) return {nullptr, detail::pathError("stat", path, err)};
    return source_->Stat(path);
}

error BasePathFs::Rename(std::string_view oldname, std::string_view newname) {
    auto [oldpath, err] = RealPath(oldname);
    if (err) return detail::pathError("rename", oldpath, err);
    auto [newpath, nerr] = RealPath(newname);
    if (nerr) return detail::pathError("rename", newpath, nerr);
    return source_->Rename(oldpath, newpath);
}

error BasePathFs::RemoveAll(std::string_view name) {
    auto [path, err] = RealPath(name);
    if (err) return detail::pathError("remove_all", path, err);
    return source_->RemoveAll(path);
}

error BasePathFs::Remove(std::string_view name) {
    auto [path, err] = RealPath(name);
    if (err) return detail::pathError("remove", path, err);
    return source_->Remove(path);
}

std::pair<ptr<File>, error> BasePathFs::OpenFile(std::string_view name, int flag, fs::FileMode mode) {
    auto [path, err] = RealPath(name);
    if (err) return {nullptr, detail::pathError("openfile", path, err)};
    auto [sourcef, oerr] = source_->OpenFile(path, flag, mode);
    if (oerr) return {nullptr, oerr};
    return {make_ptr<basePathFile>(sourcef, path_), nullptr};
}

std::pair<ptr<File>, error> BasePathFs::Open(std::string_view name) {
    auto [path, err] = RealPath(name);
    if (err) return {nullptr, detail::pathError("open", path, err)};
    auto [sourcef, oerr] = source_->Open(path);
    if (oerr) return {nullptr, oerr};
    return {make_ptr<basePathFile>(sourcef, path_), nullptr};
}

error BasePathFs::Mkdir(std::string_view name, fs::FileMode mode) {
    auto [path, err] = RealPath(name);
    if (err) return detail::pathError("mkdir", path, err);
    return source_->Mkdir(path, mode);
}

error BasePathFs::MkdirAll(std::string_view name, fs::FileMode mode) {
    auto [path, err] = RealPath(name);
    if (err) return detail::pathError("mkdir", path, err);
    return source_->MkdirAll(path, mode);
}

std::pair<ptr<File>, error> BasePathFs::Create(std::string_view name) {
    auto [path, err] = RealPath(name);
    if (err) return {nullptr, detail::pathError("create", path, err)};
    auto [sourcef, cerr] = source_->Create(path);
    if (cerr) return {nullptr, cerr};
    return {make_ptr<basePathFile>(sourcef, path_), nullptr};
}

std::tuple<ptr<fs::FileInfo>, bool, error> BasePathFs::LstatIfPossible(std::string_view name) {
    auto [path, err] = RealPath(name);
    if (err) return {nullptr, false, detail::pathError("lstat", path, err)};
    if (auto* lstater = dynamic_cast<Lstater*>(source_.get())) return lstater->LstatIfPossible(path);
    auto [fi, serr] = source_->Stat(path);
    return {fi, false, serr};
}

error BasePathFs::SymlinkIfPossible(std::string_view oldname, std::string_view newname) {
    auto [oldpath, err] = RealPath(oldname);
    if (err) return detail::linkError("symlink", oldpath, newname, err);
    auto [newpath, nerr] = RealPath(newname);
    if (nerr) return detail::linkError("symlink", oldpath, newpath, nerr);
    if (auto* linker = dynamic_cast<Linker*>(source_.get())) return linker->SymlinkIfPossible(oldpath, newpath);
    return detail::linkError("symlink", oldpath, newpath, ErrNoSymlink);
}

std::pair<string, error> BasePathFs::ReadlinkIfPossible(std::string_view name) {
    auto [path, err] = RealPath(name);
    if (err) return {{}, detail::pathError("readlink", path, err)};
    if (auto* reader = dynamic_cast<LinkReader*>(source_.get())) return reader->ReadlinkIfPossible(path);
    return {{}, detail::pathError("readlink", path, ErrNoReadlink)};
}

string FullBaseFsPath(const BasePathFs& basePathFs, std::string_view relativePath) {
    string combinedPath = filepath::Join(basePathFs.path_, relativePath);
    if (auto* parent = dynamic_cast<const BasePathFs*>(basePathFs.source_.get())) {
        return FullBaseFsPath(*parent, combinedPath);
    }
    return combinedPath;
}

}  // namespace gopp::vfs
