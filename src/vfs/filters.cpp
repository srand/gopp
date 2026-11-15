// ReadOnlyFs and RegexpFs, ported from afero v1.15.0's readonlyfs.go and
// regexpfs.go.
#include "internal.hpp"

namespace gopp::vfs {

// ---------------------------------------------------------------------------
// ReadOnlyFs

ptr<Fs> NewReadOnlyFs(ptr<Fs> source) { return make_ptr<ReadOnlyFs>(std::move(source)); }

std::pair<std::vector<ptr<fs::FileInfo>>, error> ReadOnlyFs::ReadDir(std::string_view name) {
    return vfs::ReadDir(*source_, name);
}

error ReadOnlyFs::Chtimes(std::string_view, time::Time, time::Time) { return syscall::EPERM_; }
error ReadOnlyFs::Chmod(std::string_view, fs::FileMode) { return syscall::EPERM_; }
error ReadOnlyFs::Chown(std::string_view, int, int) { return syscall::EPERM_; }
std::pair<ptr<fs::FileInfo>, error> ReadOnlyFs::Stat(std::string_view name) { return source_->Stat(name); }

std::tuple<ptr<fs::FileInfo>, bool, error> ReadOnlyFs::LstatIfPossible(std::string_view name) {
    if (auto* lsf = dynamic_cast<Lstater*>(source_.get())) return lsf->LstatIfPossible(name);
    auto [fi, err] = Stat(name);
    return {fi, false, err};
}

error ReadOnlyFs::SymlinkIfPossible(std::string_view oldname, std::string_view newname) {
    return detail::linkError("symlink", oldname, newname, ErrNoSymlink);
}

std::pair<string, error> ReadOnlyFs::ReadlinkIfPossible(std::string_view name) {
    if (auto* srdr = dynamic_cast<LinkReader*>(source_.get())) return srdr->ReadlinkIfPossible(name);
    return {{}, detail::pathError("readlink", name, ErrNoReadlink)};
}

error ReadOnlyFs::Rename(std::string_view, std::string_view) { return syscall::EPERM_; }
error ReadOnlyFs::RemoveAll(std::string_view) { return syscall::EPERM_; }
error ReadOnlyFs::Remove(std::string_view) { return syscall::EPERM_; }

std::pair<ptr<File>, error> ReadOnlyFs::OpenFile(std::string_view name, int flag, fs::FileMode perm) {
    if (flag & detail::writeFlags) return {nullptr, syscall::EPERM_};
    return source_->OpenFile(name, flag, perm);
}

std::pair<ptr<File>, error> ReadOnlyFs::Open(std::string_view name) { return source_->Open(name); }
error ReadOnlyFs::Mkdir(std::string_view, fs::FileMode) { return syscall::EPERM_; }
error ReadOnlyFs::MkdirAll(std::string_view, fs::FileMode) { return syscall::EPERM_; }
std::pair<ptr<File>, error> ReadOnlyFs::Create(std::string_view) { return {nullptr, syscall::EPERM_}; }

// ---------------------------------------------------------------------------
// RegexpFs

namespace {

// vfs.RegexpFile: a directory of a RegexpFs, read without the files whose
// names don't match.
class regexpFile final : public detail::fileWrapper {
public:
    regexpFile(ptr<File> f, ptr<regexp::Regexp> re) : fileWrapper(std::move(f)), re_(std::move(re)) {}

    std::pair<std::vector<ptr<fs::FileInfo>>, error> Readdir(int c) override {
        auto [rfi, err] = f_->Readdir(c);
        if (err) return {{}, err};
        std::vector<ptr<fs::FileInfo>> fi;
        for (auto& i : rfi) {
            if (i->IsDir() || !re_ || re_->MatchString(i->Name())) fi.push_back(i);
        }
        return {fi, nullptr};
    }

    std::pair<std::vector<string>, error> Readdirnames(int c) override { return File::Readdirnames(c); }
    // From the filtered Readdir.
    std::pair<std::vector<ptr<fs::DirEntry>>, error> ReadDir(int n) override { return File::ReadDir(n); }

private:
    ptr<regexp::Regexp> re_;
};

}  // namespace

ptr<Fs> NewRegexpFs(ptr<Fs> source, ptr<regexp::Regexp> re) {
    return make_ptr<RegexpFs>(std::move(source), std::move(re));
}

error RegexpFs::matchesName(std::string_view name) const {
    if (!re_) return nullptr;
    if (re_->MatchString(name)) return nullptr;
    return syscall::ENOENT_;
}

error RegexpFs::dirOrMatches(std::string_view name) const {
    auto [dir, err] = IsDir(*source_, name);
    if (err) return err;
    if (dir) return nullptr;
    return matchesName(name);
}

error RegexpFs::Chtimes(std::string_view name, time::Time a, time::Time m) {
    if (error err = dirOrMatches(name)) return err;
    return source_->Chtimes(name, a, m);
}

error RegexpFs::Chmod(std::string_view name, fs::FileMode mode) {
    if (error err = dirOrMatches(name)) return err;
    return source_->Chmod(name, mode);
}

error RegexpFs::Chown(std::string_view name, int uid, int gid) {
    if (error err = dirOrMatches(name)) return err;
    return source_->Chown(name, uid, gid);
}

std::pair<ptr<fs::FileInfo>, error> RegexpFs::Stat(std::string_view name) {
    if (error err = dirOrMatches(name)) return {nullptr, err};
    return source_->Stat(name);
}

error RegexpFs::Rename(std::string_view oldname, std::string_view newname) {
    auto [dir, err] = IsDir(*source_, oldname);
    if (err) return err;
    if (dir) return nullptr;
    if (error err = matchesName(oldname)) return err;
    if (error err = matchesName(newname)) return err;
    return source_->Rename(oldname, newname);
}

error RegexpFs::RemoveAll(std::string_view p) {
    auto [dir, err] = IsDir(*source_, p);
    if (err) return err;
    if (!dir) {
        if (error err = matchesName(p)) return err;
    }
    return source_->RemoveAll(p);
}

error RegexpFs::Remove(std::string_view name) {
    if (error err = dirOrMatches(name)) return err;
    return source_->Remove(name);
}

std::pair<ptr<File>, error> RegexpFs::OpenFile(std::string_view name, int flag, fs::FileMode perm) {
    if (error err = dirOrMatches(name)) return {nullptr, err};
    return source_->OpenFile(name, flag, perm);
}

std::pair<ptr<File>, error> RegexpFs::Open(std::string_view name) {
    auto [dir, err] = IsDir(*source_, name);
    if (err) return {nullptr, err};
    if (!dir) {
        if (error err = matchesName(name)) return {nullptr, err};
    }
    auto [f, oerr] = source_->Open(name);
    if (oerr) return {nullptr, oerr};
    return {make_ptr<regexpFile>(f, re_), nullptr};
}

error RegexpFs::Mkdir(std::string_view n, fs::FileMode p) { return source_->Mkdir(n, p); }
error RegexpFs::MkdirAll(std::string_view n, fs::FileMode p) { return source_->MkdirAll(n, p); }

std::pair<ptr<File>, error> RegexpFs::Create(std::string_view name) {
    if (error err = matchesName(name)) return {nullptr, err};
    return source_->Create(name);
}

}  // namespace gopp::vfs
