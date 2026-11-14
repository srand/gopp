// os.DirFS and os.CopyFS, ported from Go 1.27.3's os/file.go and os/dir.go.
#include "internal.hpp"

#include <gopp/io.hpp>

namespace gopp::os {

namespace {

// Go's filepathlite.Localize on Unix: a valid io/fs path, without NULs.
bool localize(std::string_view name) {
    return fs::ValidPath(name) && name.find('\0') == name.npos;
}

// err, with name as its path where it's a PathError (Go assigns Path).
error renamed(error err, std::string_view name) {
    if (auto* pe = errors::As<PathError>(err); pe && pe == err.get()) {
        return make_ptr<PathError>(pe->Op, string(name), pe->Err);
    }
    return err;
}

class dirFS final : public fs::StatFS, public fs::ReadFileFS, public fs::ReadDirFS, public fs::ReadLinkFS {
public:
    explicit dirFS(string dir) : dir_(std::move(dir)) {}

    std::pair<ptr<fs::File>, error> Open(std::string_view name) override {
        auto [fullname, err] = join(name);
        if (err) return {nullptr, detail::path_error("open", name, err)};
        auto [f, oerr] = os::Open(fullname);
        if (oerr) return {nullptr, renamed(oerr, name)};
        return {f, nullptr};
    }

    std::pair<slice<byte>, error> ReadFile(std::string_view name) override {
        auto [fullname, err] = join(name);
        if (err) return {{}, detail::path_error("readfile", name, err)};
        auto [b, rerr] = os::ReadFile(fullname);
        if (rerr) return {{}, renamed(rerr, name)};
        return {b, nullptr};
    }

    std::pair<std::vector<ptr<fs::DirEntry>>, error> ReadDir(std::string_view name) override {
        auto [fullname, err] = join(name);
        if (err) return {{}, detail::path_error("readdir", name, err)};
        auto [entries, rerr] = os::ReadDir(fullname);
        if (rerr) return {{}, renamed(rerr, name)};
        return {entries, nullptr};
    }

    std::pair<ptr<fs::FileInfo>, error> Stat(std::string_view name) override {
        auto [fullname, err] = join(name);
        if (err) return {nullptr, detail::path_error("stat", name, err)};
        auto [fi, serr] = os::Stat(fullname);
        if (serr) return {nullptr, renamed(serr, name)};
        return {fi, nullptr};
    }

    std::pair<ptr<fs::FileInfo>, error> Lstat(std::string_view name) override {
        auto [fullname, err] = join(name);
        if (err) return {nullptr, detail::path_error("lstat", name, err)};
        auto [fi, serr] = os::Lstat(fullname);
        if (serr) return {nullptr, renamed(serr, name)};
        return {fi, nullptr};
    }

    std::pair<string, error> ReadLink(std::string_view name) override {
        auto [fullname, err] = join(name);
        if (err) return {{}, detail::path_error("readlink", name, err)};
        auto [target, rerr] = os::Readlink(fullname);
        if (rerr) return {{}, renamed(rerr, name)};
        return {target, nullptr};
    }

private:
    // join returns the path for name in dir.
    std::pair<std::string, error> join(std::string_view name) const {
        std::string_view dir = dir_.view();
        if (dir.empty()) return {{}, errors::New("os: DirFS with empty root")};
        if (!localize(name)) return {{}, ErrInvalid};
        if (IsPathSeparator(dir.back())) return {std::string(dir) + std::string(name), nullptr};
        return {std::string(dir) + PathSeparator + std::string(name), nullptr};
    }

    string dir_;
};

}  // namespace

ptr<fs::FS> DirFS(std::string_view dir) { return make_ptr<dirFS>(string(dir)); }

error CopyFS(std::string_view dir, fs::FS& fsys) {
    return fs::WalkDir(fsys, ".", [&](std::string_view path, const ptr<DirEntry>& d, error err) -> error {
        if (err) return err;
        if (!localize(path)) return ErrInvalid;
        std::string newPath = detail::join_path(dir, path);

        FileMode type = d->Type();
        if (type == ModeDir) return MkdirAll(newPath, 0777);
        if (type == ModeSymlink) {
            auto [target, lerr] = fs::ReadLink(fsys, path);
            if (lerr) return lerr;
            return Symlink(target.view(), newPath);
        }
        if (type == FileMode(0)) {
            auto [r, oerr] = fsys.Open(path);
            if (oerr) return oerr;
            auto [info, serr] = r->Stat();
            if (serr) {
                r->Close();
                return serr;
            }
            auto [w, werr] = OpenFile(newPath, O_CREATE_ | O_EXCL_ | O_WRONLY_, FileMode(0666) | (info->Mode() & FileMode(0777)));
            if (werr) {
                r->Close();
                return werr;
            }
            if (auto [n, cerr] = io::Copy(*w, *r); cerr) {
                w->Close();
                r->Close();
                return detail::path_error("Copy", newPath, cerr);
            }
            r->Close();
            return w->Close();
        }
        return detail::path_error("CopyFS", path, ErrInvalid);
    });
}

}  // namespace gopp::os
