// Functions over any FS, ported from Go 1.27.3's io/fs readfile.go,
// readdir.go, stat.go, sub.go, glob.go, readlink.go and walk.go.
#include <gopp/io/fs.hpp>
#include <gopp/path.hpp>

#include <algorithm>
#include <string>

namespace gopp::fs {

const error SkipDir = errors::New("skip this directory");
const error SkipAll = errors::New("skip everything and stop the walk");

namespace {

error pathError(std::string_view op, std::string_view path, error err) {
    return make_ptr<PathError>(string(op), string(path), std::move(err));
}

// Go's dirInfo: a DirEntry of a FileInfo.
class dirInfo final : public DirEntry {
public:
    explicit dirInfo(ptr<FileInfo> info) : fileInfo_(std::move(info)) {}
    string Name() const override { return fileInfo_->Name(); }
    bool IsDir() const override { return fileInfo_->IsDir(); }
    FileMode Type() const override { return fileInfo_->Mode().Type(); }
    std::pair<ptr<FileInfo>, error> Info() const override { return {fileInfo_, nullptr}; }

private:
    ptr<FileInfo> fileInfo_;
};

}  // namespace

std::pair<bytes, error> ReadFile(FS& fsys, std::string_view name) {
    if (auto* rf = dynamic_cast<ReadFileFS*>(&fsys)) return rf->ReadFile(name);
    auto [file, err] = fsys.Open(name);
    if (err) return {{}, err};
    std::size_t size = 0;
    if (auto [info, serr] = file->Stat(); !serr && info->Size() > 0) size = std::size_t(info->Size());
    bytes data = bytes::make(0, size + 1);
    for (;;) {
        if (data.size() >= data.cap()) data = append(data, byte(0)).Sub(0, data.size());
        auto [n, rerr] = file->Read(std::span<byte>(data.data() + data.size(), data.cap() - data.size()));
        data = data.Sub(0, data.size() + std::size_t(n));
        if (rerr) {
            file->Close();
            if (rerr == io::EOF_) rerr = nullptr;
            return {data, rerr};
        }
    }
}

std::pair<std::vector<ptr<DirEntry>>, error> ReadDir(FS& fsys, std::string_view name) {
    if (auto* rd = dynamic_cast<ReadDirFS*>(&fsys)) return rd->ReadDir(name);
    auto [file, err] = fsys.Open(name);
    if (err) return {{}, err};
    auto* dir = dynamic_cast<ReadDirFile*>(file.get());
    if (!dir) {
        file->Close();
        return {{}, pathError("readdir", name, errors::New("not implemented"))};
    }
    auto [list, rerr] = dir->ReadDir(-1);
    std::sort(list.begin(), list.end(),
              [](const ptr<DirEntry>& a, const ptr<DirEntry>& b) { return a->Name().view() < b->Name().view(); });
    file->Close();
    return {list, rerr};
}

std::pair<ptr<FileInfo>, error> Stat(FS& fsys, std::string_view name) {
    if (auto* sf = dynamic_cast<StatFS*>(&fsys)) return sf->Stat(name);
    auto [file, err] = fsys.Open(name);
    if (err) return {nullptr, err};
    auto r = file->Stat();
    file->Close();
    return r;
}

std::pair<string, error> ReadLink(FS& fsys, std::string_view name) {
    auto* sym = dynamic_cast<ReadLinkFS*>(&fsys);
    if (!sym) return {{}, pathError("readlink", name, ErrInvalid)};
    return sym->ReadLink(name);
}

std::pair<ptr<FileInfo>, error> Lstat(FS& fsys, std::string_view name) {
    auto* sym = dynamic_cast<ReadLinkFS*>(&fsys);
    if (!sym) return Stat(fsys, name);
    return sym->Lstat(name);
}

ptr<DirEntry> FileInfoToDirEntry(const ptr<FileInfo>& info) {
    if (!info) return nullptr;
    return make_ptr<dirInfo>(info);
}

// ---------------------------------------------------------------------------
// sub.go

namespace {

class subFS final : public ReadDirFS, public ReadFileFS, public ReadLinkFS, public GlobFS, public SubFS {
public:
    subFS(ptr<FS> fsys, string dir) : fsys_(std::move(fsys)), dir_(std::move(dir)) {}

    std::pair<ptr<File>, error> Open(std::string_view name) override {
        auto [full, err] = fullName("open", name);
        if (err) return {nullptr, err};
        auto [file, oerr] = fsys_->Open(full.view());
        return {file, fixErr(oerr)};
    }

    std::pair<std::vector<ptr<DirEntry>>, error> ReadDir(std::string_view name) override {
        auto [full, err] = fullName("read", name);
        if (err) return {{}, err};
        auto [dir, rerr] = fs::ReadDir(*fsys_, full.view());
        return {dir, fixErr(rerr)};
    }

    std::pair<bytes, error> ReadFile(std::string_view name) override {
        auto [full, err] = fullName("read", name);
        if (err) return {{}, err};
        auto [data, rerr] = fs::ReadFile(*fsys_, full.view());
        return {data, fixErr(rerr)};
    }

    std::pair<string, error> ReadLink(std::string_view name) override {
        auto [full, err] = fullName("readlink", name);
        if (err) return {{}, err};
        auto [target, rerr] = fs::ReadLink(*fsys_, full.view());
        if (rerr) return {{}, fixErr(rerr)};
        return {target, nullptr};
    }

    std::pair<ptr<FileInfo>, error> Lstat(std::string_view name) override {
        auto [full, err] = fullName("lstat", name);
        if (err) return {nullptr, err};
        auto [info, lerr] = fs::Lstat(*fsys_, full.view());
        if (lerr) return {nullptr, fixErr(lerr)};
        return {info, nullptr};
    }

    std::pair<std::vector<string>, error> Glob(std::string_view pattern) override {
        // Check pattern is well-formed.
        if (auto [m, err] = path::Match(pattern, ""); err) return {{}, err};
        if (pattern == ".") return {{string::from_static(".")}, nullptr};
        std::string full = std::string(dir_.view()) + "/" + std::string(pattern);
        auto [list, err] = fs::Glob(*fsys_, full);
        for (auto& name : list) {
            auto [s, ok] = shorten(name.view());
            if (!ok) {
                return {{}, errors::New("invalid result from inner fsys Glob: " + std::string(name.view()) + " not in " +
                                        std::string(dir_.view()))};
            }
            name = s;
        }
        return {list, fixErr(err)};
    }

    std::pair<ptr<FS>, error> Sub(std::string_view dir) override {
        if (dir == ".") return {ptr<FS>(this), nullptr};
        auto [full, err] = fullName("sub", dir);
        if (err) return {nullptr, err};
        return {make_ptr<subFS>(fsys_, full), nullptr};
    }

private:
    // fullName maps name to the fully-qualified name dir/name.
    std::pair<string, error> fullName(std::string_view op, std::string_view name) const {
        if (!ValidPath(name)) return {{}, pathError(op, name, ErrInvalid)};
        return {path::Join(dir_.view(), name), nullptr};
    }

    // shorten maps name, which should start with f.dir, back to the
    // suffix after f.dir.
    std::pair<string, bool> shorten(std::string_view name) const {
        std::string_view dir = dir_.view();
        if (name == dir) return {string::from_static("."), true};
        if (name.size() >= dir.size() + 2 && name[dir.size()] == '/' && name.substr(0, dir.size()) == dir) {
            return {string(name.substr(dir.size() + 1)), true};
        }
        return {{}, false};
    }

    // fixErr shortens any reported names in PathErrors.
    error fixErr(error err) const {
        if (auto* e = errors::As<PathError>(err); e && err.get() == e) {
            if (auto [s, ok] = shorten(e->Path.view()); ok) {
                return make_ptr<PathError>(e->Op, s, e->Err);
            }
        }
        return err;
    }

    ptr<FS> fsys_;
    string dir_;
};

}  // namespace

std::pair<ptr<FS>, error> Sub(const ptr<FS>& fsys, std::string_view dir) {
    if (!ValidPath(dir)) return {nullptr, pathError("sub", dir, ErrInvalid)};
    if (dir == ".") return {fsys, nullptr};
    if (auto* sf = dynamic_cast<SubFS*>(fsys.get())) return sf->Sub(dir);
    return {make_ptr<subFS>(fsys, string(dir)), nullptr};
}

// ---------------------------------------------------------------------------
// glob.go

namespace {

bool hasMeta(std::string_view path) {
    return path.find_first_of("*?[\\") != std::string_view::npos;
}

// cleanGlobPath prepares path for glob matching.
std::string cleanGlobPath(std::string_view path) {
    if (path.empty()) return ".";
    return std::string(path.substr(0, path.size() - 1));  // chop off trailing separator
}

// glob searches for files matching pattern in the directory dir and
// appends them to matches, and returns them. If the directory cannot be
// opened, glob returns the existing matches. New matches are added in
// lexicographical order.
error glob(FS& fsys, std::string_view dir, std::string_view pattern, std::vector<string>& m) {
    auto [infos, err] = ReadDir(fsys, dir);
    if (err) return nullptr;  // ignore I/O error
    for (auto& info : infos) {
        string n = info->Name();
        auto [matched, merr] = path::Match(pattern, n.view());
        if (merr) return merr;
        if (matched) m.push_back(path::Join(dir, n.view()));
    }
    return nullptr;
}

std::pair<std::vector<string>, error> globWithLimit(FS& fsys, std::string_view pattern, int depth) {
    // This limit is added to prevent stack exhaustion issues. See
    // CVE-2022-30630.
    constexpr int pathSeparatorsLimit = 10000;
    if (depth > pathSeparatorsLimit) return {{}, path::ErrBadPattern};
    if (auto* gf = dynamic_cast<GlobFS*>(&fsys)) return gf->Glob(pattern);
    // Check pattern is well-formed.
    if (auto [m, err] = path::Match(pattern, ""); err) return {{}, err};
    if (!hasMeta(pattern)) {
        if (auto [info, err] = Stat(fsys, pattern); err) return {{}, nullptr};
        return {{string(pattern)}, nullptr};
    }
    auto [d, file] = path::Split(pattern);
    std::string dir = cleanGlobPath(d.view());
    std::vector<string> matches;
    if (!hasMeta(dir)) {
        error err = glob(fsys, dir, file.view(), matches);
        return {matches, err};
    }
    // Prevent infinite recursion. See issue 15879.
    if (dir == pattern) return {{}, path::ErrBadPattern};
    auto [m, err] = globWithLimit(fsys, dir, depth + 1);
    if (err) return {{}, err};
    for (auto& dd : m) {
        if (error gerr = glob(fsys, dd.view(), file.view(), matches)) return {matches, gerr};
    }
    return {matches, nullptr};
}

}  // namespace

std::pair<std::vector<string>, error> Glob(FS& fsys, std::string_view pattern) {
    return globWithLimit(fsys, pattern, 0);
}

// ---------------------------------------------------------------------------
// walk.go

namespace {

// walkDir recursively descends path, calling walkDirFn.
error walkDir(FS& fsys, std::string_view name, const ptr<DirEntry>& d, const WalkDirFunc& walkDirFn) {
    if (error err = walkDirFn(name, d, nullptr); err || !d->IsDir()) {
        if (err == SkipDir && d->IsDir()) {
            // Successfully skipped directory.
            err = nullptr;
        }
        return err;
    }
    auto [dirs, err] = ReadDir(fsys, name);
    if (err) {
        // Second call, to report ReadDir error.
        err = walkDirFn(name, d, err);
        if (err) {
            if (err == SkipDir && d->IsDir()) err = nullptr;
            return err;
        }
    }
    for (auto& d1 : dirs) {
        string name1 = path::Join(name, d1->Name().view());
        if (error werr = walkDir(fsys, name1.view(), d1, walkDirFn)) {
            if (werr == SkipDir) break;
            return werr;
        }
    }
    return nullptr;
}

}  // namespace

error WalkDir(FS& fsys, std::string_view root, const WalkDirFunc& fn) {
    auto [info, err] = Stat(fsys, root);
    if (err) err = fn(root, nullptr, err);
    else err = walkDir(fsys, root, FileInfoToDirEntry(info), fn);
    if (err == SkipDir || err == SkipAll) return nullptr;
    return err;
}

}  // namespace gopp::fs
