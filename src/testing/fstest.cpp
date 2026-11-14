// Ported from Go 1.27.3's testing/fstest/mapfs.go and testfs.go.
#include <gopp/fmt.hpp>
#include <gopp/path.hpp>
#include <gopp/strings.hpp>
#include <gopp/testing/fstest.hpp>
#include <gopp/testing/iotest.hpp>

#include <gopp/unicode/utf8.hpp>

#include <algorithm>
#include <set>
#include <string>

namespace gopp::fstest {

struct MapFS::entry : object {
    explicit entry(MapFile f_) : f(std::move(f_)) {}
    MapFile f;
};

namespace {

using entry = MapFS::entry;

error pathError(std::string_view op, std::string_view path, error err) {
    return make_ptr<fs::PathError>(string(op), string(path), std::move(err));
}

ptr<entry> synthDir() { return make_ptr<entry>(MapFile{.Mode = fs::ModeDir | 0555}); }

// Go's mapFileInfo: a FileInfo, and a DirEntry, of a MapFile.
class mapFileInfo final : public fs::FileInfo, public fs::DirEntry {
public:
    mapFileInfo(string name, ptr<const entry> f) : name_(std::move(name)), f_(std::move(f)) {}

    string Name() const override { return path::Base(name_.view()); }
    int64 Size() const override { return int64(f_->f.Data.size()); }
    fs::FileMode Mode() const override { return f_->f.Mode; }
    fs::FileMode Type() const override { return f_->f.Mode.Type(); }
    time::Time ModTime() const override { return f_->f.ModTime; }
    bool IsDir() const override { return bool(f_->f.Mode & fs::ModeDir); }
    const void* Sys() const override { return f_->f.Sys; }
    std::pair<ptr<fs::FileInfo>, error> Info() const override {
        return {ptr<fs::FileInfo>(const_cast<mapFileInfo*>(this)), nullptr};
    }

    const string& name() const noexcept { return name_; }
    const MapFile& file() const noexcept { return f_->f; }

private:
    string name_;
    ptr<const entry> f_;
};

// Go's openMapFile.
class openMapFile final : public fs::File, public io::Seeker, public io::ReaderAt {
public:
    openMapFile(string path, ptr<mapFileInfo> info) : path_(std::move(path)), info_(std::move(info)) {}

    std::pair<ptr<fs::FileInfo>, error> Stat() override { return {info_, nullptr}; }
    error Close() override { return nullptr; }

    std::pair<isize, error> Read(std::span<byte> b) override {
        const slice<byte>& data = info_->file().Data;
        if (offset_ >= int64(data.size())) return {0, io::EOF_};
        if (offset_ < 0) return {0, pathError("read", path_.view(), fs::ErrInvalid)};
        std::size_t n = std::min(b.size(), data.size() - std::size_t(offset_));
        std::copy_n(data.data() + offset_, n, b.data());
        offset_ += int64(n);
        return {isize(n), nullptr};
    }

    std::pair<int64, error> Seek(int64 offset, int whence) override {
        int64 size = int64(info_->file().Data.size());
        switch (whence) {
        case 0: break;
        case 1: offset += offset_; break;
        case 2: offset += size; break;
        }
        if (offset < 0 || offset > size) return {0, pathError("seek", path_.view(), fs::ErrInvalid)};
        offset_ = offset;
        return {offset, nullptr};
    }

    std::pair<isize, error> ReadAt(std::span<byte> b, int64 offset) override {
        const slice<byte>& data = info_->file().Data;
        if (offset < 0 || offset > int64(data.size())) return {0, pathError("read", path_.view(), fs::ErrInvalid)};
        std::size_t n = std::min(b.size(), data.size() - std::size_t(offset));
        std::copy_n(data.data() + offset, n, b.data());
        if (n < b.size()) return {isize(n), io::EOF_};
        return {isize(n), nullptr};
    }

private:
    string path_;
    ptr<mapFileInfo> info_;
    int64 offset_ = 0;
};

// Go's mapDir.
class mapDir final : public fs::ReadDirFile {
public:
    mapDir(string path, ptr<mapFileInfo> info, std::vector<ptr<mapFileInfo>> entries)
        : path_(std::move(path)), info_(std::move(info)), entry_(std::move(entries)) {}

    std::pair<ptr<fs::FileInfo>, error> Stat() override { return {info_, nullptr}; }
    error Close() override { return nullptr; }
    std::pair<isize, error> Read(std::span<byte>) override {
        return {0, pathError("read", path_.view(), fs::ErrInvalid)};
    }

    std::pair<std::vector<ptr<fs::DirEntry>>, error> ReadDir(int count) override {
        std::size_t n = entry_.size() - offset_;
        if (n == 0 && count > 0) return {{}, io::EOF_};
        if (count > 0 && n > std::size_t(count)) n = std::size_t(count);
        std::vector<ptr<fs::DirEntry>> list(n);
        for (std::size_t i = 0; i < n; i++) list[i] = entry_[offset_ + i];
        offset_ += n;
        return {list, nullptr};
    }

private:
    string path_;
    ptr<mapFileInfo> info_;
    std::vector<ptr<mapFileInfo>> entry_;
    std::size_t offset_ = 0;
};

// Go's fsOnly: hides the methods of an FS but Open, so that the fs
// functions don't call back into them.
struct fsOnly final : fs::FS {
    explicit fsOnly(fs::FS& f_) : f(f_) {}
    std::pair<ptr<fs::File>, error> Open(std::string_view name) override { return f.Open(name); }
    fs::FS& f;
};

// Go's noSub: a MapFS without its Sub, for fs::Sub to wrap.
class noSub final : public fs::ReadFileFS,
                    public fs::ReadDirFS,
                    public fs::StatFS,
                    public fs::GlobFS,
                    public fs::ReadLinkFS {
public:
    explicit noSub(ptr<MapFS> m) : m_(std::move(m)) {}
    std::pair<ptr<fs::File>, error> Open(std::string_view name) override { return m_->Open(name); }
    std::pair<slice<byte>, error> ReadFile(std::string_view name) override { return m_->ReadFile(name); }
    std::pair<std::vector<ptr<fs::DirEntry>>, error> ReadDir(std::string_view name) override {
        return m_->ReadDir(name);
    }
    std::pair<ptr<fs::FileInfo>, error> Stat(std::string_view name) override { return m_->Stat(name); }
    std::pair<std::vector<string>, error> Glob(std::string_view pattern) override { return m_->Glob(pattern); }
    std::pair<string, error> ReadLink(std::string_view name) override { return m_->ReadLink(name); }
    std::pair<ptr<fs::FileInfo>, error> Lstat(std::string_view name) override { return m_->Lstat(name); }

private:
    ptr<MapFS> m_;
};

}  // namespace

MapFS::MapFS(std::initializer_list<std::pair<std::string_view, MapFile>> files) {
    for (auto& [name, f] : files) (*this)[name] = f;
}

MapFS::MapFS() = default;
MapFS::~MapFS() = default;

MapFile& MapFS::operator[](std::string_view name) {
    auto it = files_.find(name);
    if (it == files_.end()) it = files_.emplace(std::string(name), make_ptr<entry>(MapFile{})).first;
    return it->second->f;
}

void MapFS::erase(std::string_view name) {
    if (auto it = files_.find(name); it != files_.end()) files_.erase(it);
}

ptr<MapFS::entry> MapFS::find(std::string_view name) const {
    auto it = files_.find(name);
    return it == files_.end() ? nullptr : it->second;
}

ptr<MapFS> MakeMapFS(std::initializer_list<std::pair<std::string_view, MapFile>> files) {
    auto m = make_ptr<MapFS>();
    for (auto& [name, f] : files) (*m)[name] = f;
    return m;
}

std::pair<ptr<fs::File>, error> MapFS::Open(std::string_view name) {
    if (!fs::ValidPath(name)) return {nullptr, pathError("open", name, fs::ErrNotExist)};
    auto [realName, ok] = resolveSymlinks(name);
    if (!ok) return {nullptr, pathError("open", name, fs::ErrNotExist)};

    ptr<entry> file = find(realName);
    if (file && !(file->f.Mode & fs::ModeDir)) {
        // Ordinary file
        return {make_ptr<openMapFile>(string(name), make_ptr<mapFileInfo>(path::Base(name), file)), nullptr};
    }

    // Directory, possibly synthesized. Note that file can be nil here:
    // the map need not contain explicit parent directories for all its
    // files. But file can also be non-nil, in case the user wants to set
    // metadata for the directory explicitly. Either way, we need to
    // construct the list of children of this directory.
    std::vector<ptr<mapFileInfo>> list;
    std::set<std::string, std::less<>> need;
    if (realName == ".") {
        for (auto& [fname, f] : files_) {
            auto i = fname.find('/');
            if (i == fname.npos) {
                if (fname != ".") list.push_back(make_ptr<mapFileInfo>(string(fname), f));
            } else {
                need.insert(fname.substr(0, i));
            }
        }
    } else {
        std::string prefix = realName + "/";
        for (auto it = files_.lower_bound(prefix); it != files_.end() && it->first.starts_with(prefix); ++it) {
            std::string_view felem = std::string_view(it->first).substr(prefix.size());
            auto i = felem.find('/');
            if (i == felem.npos) list.push_back(make_ptr<mapFileInfo>(string(felem), it->second));
            else need.insert(std::string(felem.substr(0, i)));
        }
        // If the directory name is not in the map, and there are no
        // children of the name in the map, then the directory is treated
        // as not existing.
        if (!file && list.empty() && need.empty()) return {nullptr, pathError("open", name, fs::ErrNotExist)};
    }
    for (auto& fi : list) need.erase(std::string(fi->name().view()));
    for (auto& n : need) list.push_back(make_ptr<mapFileInfo>(string(n), synthDir()));
    std::ranges::sort(list, [](const ptr<mapFileInfo>& a, const ptr<mapFileInfo>& b) {
        return a->name().view() < b->name().view();
    });

    if (!file) file = synthDir();
    std::string_view elem = name == "." ? "." : name.substr(name.rfind('/') + 1);
    return {make_ptr<mapDir>(string(name), make_ptr<mapFileInfo>(string(elem), file), std::move(list)), nullptr};
}

// resolveSymlinks: the name with its symbolic links resolved, and whether
// it's valid (it isn't for absolute links).
std::pair<std::string, bool> MapFS::resolveSymlinks(std::string_view name) const {
    // Fast path: if a symlink is in the map, resolve it.
    if (auto file = find(name); file && file->f.Mode.Type() == fs::ModeSymlink) {
        std::string_view target = asStringView(file->f.Data);
        if (path::IsAbs(target)) return {{}, false};
        return resolveSymlinks(path::Join(path::Dir(name).view(), target).view());
    }

    // Check if each parent directory (starting at root) is a symlink.
    for (std::size_t i = 0; i < name.size();) {
        auto j = name.substr(i).find('/');
        std::string_view dir;
        if (j == name.npos) {
            dir = name;
            i = name.size();
        } else {
            dir = name.substr(0, i + j);
            i += j;
        }
        if (auto file = find(dir); file && file->f.Mode.Type() == fs::ModeSymlink) {
            std::string_view target = asStringView(file->f.Data);
            if (path::IsAbs(target)) return {{}, false};
            std::string next(path::Join(path::Dir(dir).view(), target).view());
            next += name.substr(i);
            return resolveSymlinks(next);
        }
        i += 1;  // len("/")
    }
    return {std::string(name), fs::ValidPath(name)};
}

std::pair<string, error> MapFS::ReadLink(std::string_view name) {
    auto [info, err] = lstat(name);
    if (err) return {{}, pathError("readlink", name, err)};
    auto* mi = dynamic_cast<mapFileInfo*>(info.get());
    if (mi->file().Mode.Type() != fs::ModeSymlink) return {{}, pathError("readlink", name, fs::ErrInvalid)};
    return {String(mi->file().Data), nullptr};
}

std::pair<ptr<fs::FileInfo>, error> MapFS::Lstat(std::string_view name) {
    auto [info, err] = lstat(name);
    if (err) return {nullptr, pathError("lstat", name, err)};
    return {info, nullptr};
}

std::pair<ptr<fs::FileInfo>, error> MapFS::lstat(std::string_view name) const {
    if (!fs::ValidPath(name)) return {nullptr, fs::ErrNotExist};
    auto [realDir, ok] = resolveSymlinks(path::Dir(name).view());
    if (!ok) return {nullptr, fs::ErrNotExist};
    string elem = path::Base(name);
    string realName = path::Join(realDir, elem.view());

    if (auto file = find(realName.view())) return {make_ptr<mapFileInfo>(elem, file), nullptr};

    if (realName.view() == ".") return {make_ptr<mapFileInfo>(elem, synthDir()), nullptr};
    // Maybe a directory.
    std::string prefix = std::string(realName.view()) + "/";
    if (auto it = files_.lower_bound(prefix); it != files_.end() && it->first.starts_with(prefix)) {
        return {make_ptr<mapFileInfo>(elem, synthDir()), nullptr};
    }
    return {nullptr, fs::ErrNotExist};
}

std::pair<slice<byte>, error> MapFS::ReadFile(std::string_view name) {
    fsOnly f(*this);
    return fs::ReadFile(f, name);
}

std::pair<ptr<fs::FileInfo>, error> MapFS::Stat(std::string_view name) {
    fsOnly f(*this);
    return fs::Stat(f, name);
}

std::pair<std::vector<ptr<fs::DirEntry>>, error> MapFS::ReadDir(std::string_view name) {
    fsOnly f(*this);
    return fs::ReadDir(f, name);
}

std::pair<std::vector<string>, error> MapFS::Glob(std::string_view pattern) {
    fsOnly f(*this);
    return fs::Glob(f, pattern);
}

std::pair<ptr<fs::FS>, error> MapFS::Sub(std::string_view dir) {
    return fs::Sub(make_ptr<noSub>(ptr<MapFS>(this)), dir);
}

// ---------------------------------------------------------------------------
// testfs.go

namespace {

// formatEntry formats an fs.DirEntry into a string for error messages and
// comparison.
std::string formatEntry(const fs::DirEntry& entry) {
    return std::format("{} IsDir={} Type={}", entry.Name(), entry.IsDir(), entry.Type());
}

// formatInfoEntry formats an fs.FileInfo into a string like the result of
// formatEntry, for error messages and comparison.
std::string formatInfoEntry(const fs::FileInfo& info) {
    return std::format("{} IsDir={} Type={}", info.Name(), info.IsDir(), info.Mode().Type());
}

// formatInfo formats an fs.FileInfo into a string for error messages and
// comparison.
std::string formatInfo(const fs::FileInfo& info) {
    return std::format("{} IsDir={} Mode={} Size={} ModTime={}", info.Name(), info.IsDir(), info.Mode(), info.Size(),
                       info.ModTime().UnixNano());
}

// Go's %#q: backquoted where it can be.
std::string bq(std::string_view s) {
    if (s.find('`') == s.npos) return "`" + std::string(s) + "`";
    return std::string(strconv::Quote(s).view());
}

std::string joinLines(const std::vector<std::string>& v, std::string_view sep = "\n") {
    std::string out;
    for (std::size_t i = 0; i < v.size(); i++) {
        if (i > 0) out += sep;
        out += v[i];
    }
    return out;
}

std::string_view sv(const string& s) { return s.view(); }

// An fsTester holds state for running the test.
class fsTester {
public:
    explicit fsTester(fs::FS& fsys) : fsys_(fsys) {}

    std::vector<error> errors;
    std::vector<std::string> dirs;
    std::vector<std::string> files;

    // errorf adds an error to the list of errors.
    template <typename... A>
    void errorf(std::format_string<A...> f, A&&... a) {
        errors.push_back(fmt::Errorf(f, std::forward<A>(a)...));
    }

    ptr<fs::ReadDirFile> openDir(std::string_view dir) {
        auto [f, err] = fsys_.Open(dir);
        if (err) {
            errorf("{}: Open: {}", dir, fmt::W(err));
            return nullptr;
        }
        auto d = dynamic_pointer_cast<fs::ReadDirFile>(f);
        if (!d) {
            f->Close();
            errorf("{}: Open returned File type {}, not a fs.ReadDirFile", dir, typeid(*f).name());
            return nullptr;
        }
        return d;
    }

    // checkDir checks the directory dir, which is expected to exist (it is
    // either the root or was found in a directory listing with IsDir true).
    void checkDir(const std::string& dir) {
        // Read entire directory.
        dirs.push_back(dir);
        auto d = openDir(dir);
        if (!d) return;
        auto [list, err] = d->ReadDir(-1);
        if (err) {
            d->Close();
            errorf("{}: ReadDir(-1): {}", dir, fmt::W(err));
            return;
        }

        // Check all children.
        std::string prefix = dir == "." ? "" : dir + "/";
        for (auto& info : list) {
            string name = info->Name();
            if (name.view() == "." || name.view() == ".." || name.view().empty()) {
                errorf("{}: ReadDir: child has invalid name: {}", dir, bq(name.view()));
                continue;
            }
            if (strings::Contains(name.view(), "/")) {
                errorf("{}: ReadDir: child name contains slash: {}", dir, bq(name.view()));
                continue;
            }
            if (strings::Contains(name.view(), "\\")) {
                errorf("{}: ReadDir: child name contains backslash: {}", dir, bq(name.view()));
                continue;
            }
            std::string path = prefix + std::string(name.view());
            checkStat(path, *info);
            checkOpen(path);
            if (info->Type() == fs::ModeDir) {
                checkDir(path);
            } else if (info->Type() == fs::ModeSymlink) {
                // No further processing. Avoid following symlinks to avoid
                // potentially unbounded recursion.
                files.push_back(path);
            } else {
                checkFile(path);
            }
        }

        // Check ReadDir(-1) at EOF.
        auto [list2, err2] = d->ReadDir(-1);
        if (!list2.empty() || err2) {
            d->Close();
            errorf("{}: ReadDir(-1) at EOF = {} entries, {}, wanted 0 entries, nil", dir, list2.size(), fmt::W(err2));
            return;
        }

        // Check ReadDir(1) at EOF (different results).
        std::tie(list2, err2) = d->ReadDir(1);
        if (!list2.empty() || err2 != io::EOF_) {
            d->Close();
            errorf("{}: ReadDir(1) at EOF = {} entries, {}, wanted 0 entries, EOF", dir, list2.size(), fmt::W(err2));
            return;
        }

        // Check that close does not report an error.
        if (auto cerr = d->Close()) errorf("{}: Close: {}", dir, fmt::W(cerr));

        // Check that closing twice doesn't crash. The return value doesn't
        // matter.
        d->Close();

        // Reopen directory, read a second time, make sure contents match.
        if (d = openDir(dir); !d) return;
        std::tie(list2, err2) = d->ReadDir(-1);
        d->Close();
        if (err2) {
            errorf("{}: second Open+ReadDir(-1): {}", dir, fmt::W(err2));
            return;
        }
        checkDirList(dir, "first Open+ReadDir(-1) vs second Open+ReadDir(-1)", list, list2);

        // Reopen directory, read a third time in pieces, make sure contents
        // match.
        if (d = openDir(dir); !d) return;
        list2.clear();
        for (;;) {
            int n = list2.empty() ? 1 : 2;
            auto [frag, ferr] = d->ReadDir(n);
            if (frag.size() > std::size_t(n)) {
                errorf("{}: third Open: ReadDir({}) after {}: {} entries (too many)", dir, n, list2.size(),
                       frag.size());
                d->Close();
                return;
            }
            list2.insert(list2.end(), frag.begin(), frag.end());
            if (ferr == io::EOF_) break;
            if (ferr) {
                errorf("{}: third Open: ReadDir({}) after {}: {}", dir, n, list2.size(), fmt::W(ferr));
                d->Close();
                return;
            }
            if (n == 0) {
                errorf("{}: third Open: ReadDir({}) after {}: 0 entries but nil error", dir, n, list2.size());
                d->Close();
                return;
            }
        }
        d->Close();
        checkDirList(dir, "first Open+ReadDir(-1) vs third Open+ReadDir(1,2) loop", list, list2);

        // If fsys has ReadDir, check that it matches and is sorted.
        if (auto* rd = dynamic_cast<fs::ReadDirFS*>(&fsys_)) {
            auto [list3, err3] = rd->ReadDir(dir);
            if (err3) {
                errorf("{}: fsys.ReadDir: {}", dir, fmt::W(err3));
                return;
            }
            checkDirList(dir, "first Open+ReadDir(-1) vs fsys.ReadDir", list, list3);
            for (std::size_t i = 0; i + 1 < list3.size(); i++) {
                if (sv(list3[i]->Name()) >= sv(list3[i + 1]->Name())) {
                    errorf("{}: fsys.ReadDir: list not sorted: {} before {}", dir, list3[i]->Name(),
                           list3[i + 1]->Name());
                }
            }
        }

        // Check fs.ReadDir as well.
        std::tie(list2, err2) = fs::ReadDir(fsys_, dir);
        if (err2) {
            errorf("{}: fs.ReadDir: {}", dir, fmt::W(err2));
            return;
        }
        checkDirList(dir, "first Open+ReadDir(-1) vs fs.ReadDir", list, list2);
        for (std::size_t i = 0; i + 1 < list2.size(); i++) {
            if (sv(list2[i]->Name()) >= sv(list2[i + 1]->Name())) {
                errorf("{}: fs.ReadDir: list not sorted: {} before {}", dir, list2[i]->Name(), list2[i + 1]->Name());
            }
        }

        checkGlob(dir, list2);
    }

    // checkGlob checks that various glob patterns work if the file system
    // implements GlobFS.
    void checkGlob(const std::string& dir, const std::vector<ptr<fs::DirEntry>>& list) {
        auto* gfs = dynamic_cast<fs::GlobFS*>(&fsys_);
        if (!gfs) return;

        // Make a complex glob pattern prefix that only matches dir.
        std::string glob;
        if (dir != ".") {
            std::vector<std::string> elems;
            std::string_view rest = dir;
            for (int i = 0;; i++) {
                auto k = rest.find('/');
                std::string_view e = rest.substr(0, k);
                std::string pattern;
                int j = 0;
                for (std::size_t p = 0; p < e.size(); j++) {
                    auto [r, n] = utf8::DecodeRuneInString(e.substr(p));
                    std::string_view rs = e.substr(p, std::size_t(n));
                    p += std::size_t(n);
                    if (r == '*' || r == '?' || r == '\\' || r == '[' || r == '-') {
                        pattern += '\\';
                        pattern += rs;
                        continue;
                    }
                    switch ((i + j) % 5) {
                    case 0: pattern += rs; break;
                    case 1: pattern += "[" + std::string(rs) + "]"; break;
                    case 2: pattern += "[" + std::string(rs) + "-" + std::string(rs) + "]"; break;
                    case 3: pattern += "[\\" + std::string(rs) + "]"; break;
                    case 4: pattern += "[\\" + std::string(rs) + "-\\" + std::string(rs) + "]"; break;
                    }
                }
                elems.push_back(pattern);
                if (k == rest.npos) break;
                rest = rest.substr(k + 1);
            }
            glob = joinLines(elems, "/") + "/";
        }

        // Test that malformed patterns are detected. The error is likely
        // path.ErrBadPattern but need not be.
        if (auto [m, err] = gfs->Glob(glob + "nonexist/[]"); !err) {
            errorf("{}: Glob({}): bad pattern not detected", dir, bq(glob + "nonexist/[]"));
        }

        // Try to find a letter that appears in only some of the final names.
        char c = 'a';
        for (; c <= 'z'; c++) {
            bool have = false, haveNot = false;
            for (auto& d : list) {
                if (strings::Contains(d->Name().view(), std::string_view(&c, 1))) have = true;
                else haveNot = true;
            }
            if (have && haveNot) break;
        }
        if (c > 'z') c = 'a';
        glob += "*" + std::string(1, c) + "*";

        std::vector<std::string> want;
        for (auto& d : list) {
            if (strings::Contains(d->Name().view(), std::string_view(&c, 1))) {
                want.emplace_back(path::Join(dir, d->Name().view()).view());
            }
        }

        auto [got, err] = gfs->Glob(glob);
        if (err) {
            errorf("{}: Glob({}): {}", dir, bq(glob), fmt::W(err));
            return;
        }
        std::vector<std::string> names;
        for (auto& g : got) names.emplace_back(g.view());
        if (want == names) return;

        if (!std::ranges::is_sorted(names)) {
            errorf("{}: Glob({}): unsorted output:\n{}", dir, bq(glob), joinLines(names));
            std::ranges::sort(names);
        }

        std::vector<std::string> problems;
        std::size_t wi = 0, ni = 0;
        while (wi < want.size() || ni < names.size()) {
            if (wi < want.size() && ni < names.size() && want[wi] == names[ni]) {
                wi++;
                ni++;
            } else if (wi < want.size() && (ni == names.size() || want[wi] < names[ni])) {
                problems.push_back("missing: " + want[wi++]);
            } else {
                problems.push_back("extra: " + names[ni++]);
            }
        }
        errorf("{}: Glob({}): wrong output:\n{}", dir, bq(glob), joinLines(problems));
    }

    // checkStat checks that a direct stat of path matches entry, which was
    // found in the parent's directory listing.
    void checkStat(const std::string& path, const fs::DirEntry& entry) {
        auto [file, err] = fsys_.Open(path);
        if (err) {
            errorf("{}: Open: {}", path, fmt::W(err));
            return;
        }
        auto [info, serr] = file->Stat();
        file->Close();
        if (serr) {
            errorf("{}: Stat: {}", path, fmt::W(serr));
            return;
        }
        std::string fentry = formatEntry(entry);
        std::string fientry = formatInfoEntry(*info);
        // Note: mismatch here is OK for symlink, because Open dereferences
        // symlink.
        if (fentry != fientry && !(entry.Type() & fs::ModeSymlink)) {
            errorf("{}: mismatch:\n\tentry = {}\n\tfile.Stat() = {}", path, fentry, fientry);
        }

        auto [einfo, eerr] = entry.Info();
        if (eerr) {
            errorf("{}: entry.Info: {}", path, fmt::W(eerr));
            return;
        }
        std::string finfo = formatInfo(*info);
        if (entry.Type() & fs::ModeSymlink) {
            // For symlink, just check that entry.Info matches entry on
            // common fields. Open deferences symlink, so info itself may
            // differ.
            std::string feentry = formatInfoEntry(*einfo);
            if (fentry != feentry) {
                errorf("{}: mismatch\n\tentry = {}\n\tentry.Info() = {}\n", path, fentry, feentry);
            }
        } else {
            std::string feinfo = formatInfo(*einfo);
            if (feinfo != finfo) {
                errorf("{}: mismatch:\n\tentry.Info() = {}\n\tfile.Stat() = {}\n", path, feinfo, finfo);
            }
        }

        // Stat should be the same as Open+Stat, even for symlinks.
        auto [info2, err2] = fs::Stat(fsys_, path);
        if (err2) {
            errorf("{}: fs.Stat: {}", path, fmt::W(err2));
            return;
        }
        std::string finfo2 = formatInfo(*info2);
        if (finfo2 != finfo) errorf("{}: fs.Stat(...) = {}\n\twant {}", path, finfo2, finfo);

        if (auto* sfs = dynamic_cast<fs::StatFS*>(&fsys_)) {
            auto [info3, err3] = sfs->Stat(path);
            if (err3) {
                errorf("{}: fsys.Stat: {}", path, fmt::W(err3));
                return;
            }
            std::string finfo3 = formatInfo(*info3);
            if (finfo3 != finfo) errorf("{}: fsys.Stat(...) = {}\n\twant {}", path, finfo3, finfo);
        }

        if (auto* lfs = dynamic_cast<fs::ReadLinkFS*>(&fsys_)) {
            auto [info4, err4] = lfs->Lstat(path);
            if (err4) {
                errorf("{}: fsys.Lstat: {}", path, err4);
                return;
            }
            std::string fientry2 = formatInfoEntry(*info4);
            if (fentry != fientry2) {
                errorf("{}: mismatch:\n\tentry = {}\n\tfsys.Lstat(...) = {}", path, fentry, fientry2);
            }
            std::string feinfo = formatInfo(*einfo);
            std::string finfo4 = formatInfo(*info4);
            if (feinfo != finfo4) {
                errorf("{}: mismatch:\n\tentry.Info() = {}\n\tfsys.Lstat(...) = {}\n", path, feinfo, finfo4);
            }
        }
    }

    // checkDirList checks that two directory lists contain the same files
    // and file info. The order of the lists need not match.
    void checkDirList(const std::string& dir, std::string_view desc, const std::vector<ptr<fs::DirEntry>>& list1,
                      const std::vector<ptr<fs::DirEntry>>& list2) {
        std::map<std::string, ptr<fs::DirEntry>> old;
        auto checkMode = [&](const fs::DirEntry& entry) {
            if (entry.IsDir() != bool(entry.Type() & fs::ModeDir)) {
                if (entry.IsDir()) {
                    errorf("{}: ReadDir returned {} with IsDir() = true, Type() & ModeDir = 0", dir, entry.Name());
                } else {
                    errorf("{}: ReadDir returned {} with IsDir() = false, Type() & ModeDir = ModeDir", dir,
                           entry.Name());
                }
            }
        };

        for (auto& entry1 : list1) {
            old[std::string(entry1->Name().view())] = entry1;
            checkMode(*entry1);
        }

        std::vector<std::string> diffs;
        for (auto& entry2 : list2) {
            auto it = old.find(std::string(entry2->Name().view()));
            if (it == old.end()) {
                checkMode(*entry2);
                diffs.push_back("+ " + formatEntry(*entry2));
                continue;
            }
            if (formatEntry(*it->second) != formatEntry(*entry2)) {
                diffs.push_back("- " + formatEntry(*it->second));
                diffs.push_back("+ " + formatEntry(*entry2));
            }
            old.erase(it);
        }
        for (auto& [_, entry1] : old) diffs.push_back("- " + formatEntry(*entry1));

        if (diffs.empty()) return;

        std::ranges::sort(diffs, [](const std::string& a, const std::string& b) {
            // sort by name (i < j) and then +/- (j < i, because + < -)
            auto field = [](const std::string& s, int k) {
                auto p = s.find(' ');
                if (k == 0) return s.substr(0, p);
                auto q = s.find(' ', p + 1);
                return s.substr(p + 1, q - p - 1);
            };
            return field(a, 1) + " " + field(b, 0) < field(b, 1) + " " + field(a, 0);
        });

        errorf("{}: diff {}:\n\t{}", dir, desc, joinLines(diffs, "\n\t"));
    }

    // checkFile checks that basic file reading works correctly.
    void checkFile(const std::string& file) {
        files.push_back(file);

        // Read entire file.
        auto [f, err] = fsys_.Open(file);
        if (err) {
            errorf("{}: Open: {}", file, fmt::W(err));
            return;
        }

        auto [data, rerr] = io::ReadAll(*f);
        if (rerr) {
            f->Close();
            errorf("{}: Open+ReadAll: {}", file, fmt::W(rerr));
            return;
        }

        if (auto cerr = f->Close()) errorf("{}: Close: {}", file, fmt::W(cerr));

        // Check that closing twice doesn't crash. The return value doesn't
        // matter.
        f->Close();

        // Check that ReadFile works if present.
        if (auto* rfs = dynamic_cast<fs::ReadFileFS*>(&fsys_)) {
            auto [data2, err2] = rfs->ReadFile(file);
            if (err2) {
                errorf("{}: fsys.ReadFile: {}", file, fmt::W(err2));
                return;
            }
            checkFileRead(file, "ReadAll vs fsys.ReadFile", data, data2);

            // Modify the data and check it again. Modifying the returned
            // byte slice should not affect the next call.
            for (auto& b : data2) b++;
            std::tie(data2, err2) = rfs->ReadFile(file);
            if (err2) {
                errorf("{}: second call to fsys.ReadFile: {}", file, fmt::W(err2));
                return;
            }
            checkFileRead(file, "Readall vs second fsys.ReadFile", data, data2);

            checkBadPath(file, "ReadFile", [&](std::string_view name) { return rfs->ReadFile(name).second; });
        }

        // Check that fs.ReadFile works with t.fsys.
        auto [data3, err3] = fs::ReadFile(fsys_, file);
        if (err3) {
            errorf("{}: fs.ReadFile: {}", file, fmt::W(err3));
            return;
        }
        checkFileRead(file, "ReadAll vs fs.ReadFile", data, data3);

        // Use iotest.TestReader to check small reads, Seek, ReadAt.
        std::tie(f, err) = fsys_.Open(file);
        if (err) {
            errorf("{}: second Open: {}", file, fmt::W(err));
            return;
        }
        if (auto terr = iotest::TestReader(*f, data)) {
            std::string msg(terr.Error().view());
            std::string indented;
            for (char ch : msg) {
                indented += ch;
                if (ch == '\n') indented += '\t';
            }
            errorf("{}: failed TestReader:\n\t{}", file, indented);
        }
        f->Close();
    }

    void checkFileRead(const std::string& file, std::string_view desc, const slice<byte>& data1, const slice<byte>& data2) {
        if (asStringView(data1) != asStringView(data2)) {
            errorf("{}: {}: different data returned\n\t{}\n\t{}", file, desc, strconv::Quote(asStringView(data1)),
                   strconv::Quote(asStringView(data2)));
        }
    }

    // checkOpen validates file opening behavior by attempting to open and
    // then close the given file path.
    void checkOpen(const std::string& file) {
        checkBadPath(file, "Open", [&](std::string_view name) {
            auto [f, err] = fsys_.Open(name);
            if (!err) f->Close();
            return err;
        });
    }

    // checkBadPath checks that various invalid forms of file's name cannot
    // be opened using open.
    template <typename Open>
    void checkBadPath(const std::string& file, std::string_view desc, Open open) {
        std::vector<std::string> bad = {"/" + file, file + "/."};
        if (file == ".") bad.push_back("/");
        if (auto i = file.find('/'); i != file.npos) {
            std::string a = file.substr(0, i), b = file.substr(i + 1);
            bad.push_back(a + "//" + b);
            bad.push_back(a + "/./" + b);
            bad.push_back(a + "\\" + b);
            bad.push_back(a + "/../" + file);
        }
        if (auto i = file.rfind('/'); i != file.npos) {
            std::string a = file.substr(0, i), b = file.substr(i + 1);
            bad.push_back(a + "//" + b);
            bad.push_back(a + "/./" + b);
            bad.push_back(a + "\\" + b);
            bad.push_back(file + "/../" + b);
        }

        for (auto& b : bad) {
            if (!open(b)) errorf("{}: {}({}) succeeded, want error", file, desc, b);
        }
    }

private:
    fs::FS& fsys_;
};

error testFS(fs::FS& fsys, std::span<const std::string_view> expected) {
    fsTester t(fsys);
    t.checkDir(".");
    t.checkOpen(".");
    std::set<std::string, std::less<>> found;
    for (auto& dir : t.dirs) found.insert(dir);
    for (auto& file : t.files) found.insert(file);
    found.erase(".");
    if (expected.empty() && !found.empty()) {
        std::vector<std::string> list(found.begin(), found.end());
        if (list.size() > 15) {
            list.resize(10);
            list.push_back("...");
        }
        t.errorf("expected empty file system but found files:\n{}", joinLines(list));
    }
    for (auto name : expected) {
        if (!found.contains(name)) t.errorf("expected but not found: {}", name);
    }
    if (t.errors.empty()) return nullptr;
    return fmt::Errorf("TestFS found errors:\n{}", fmt::W(errors::Join(t.errors)));
}

}  // namespace

error TestFS(const ptr<fs::FS>& fsys, std::span<const std::string_view> expected) {
    if (auto err = testFS(*fsys, expected)) return err;
    for (auto name : expected) {
        if (auto i = name.find('/'); i != name.npos) {
            std::string_view dir = name.substr(0, i), dirSlash = name.substr(0, i + 1);
            std::vector<std::string_view> subExpected;
            for (auto n : expected) {
                if (n.starts_with(dirSlash)) subExpected.push_back(n.substr(dirSlash.size()));
            }
            auto [sub, err] = fs::Sub(fsys, dir);
            if (err) return err;
            if (auto serr = testFS(*sub, subExpected)) {
                return fmt::Errorf("testing fs.Sub(fsys, {}): {}", dir, fmt::W(serr));
            }
            break;  // one sub-test is enough
        }
    }
    return nullptr;
}

error TestFS(const ptr<fs::FS>& fsys, std::initializer_list<std::string_view> expected) {
    return TestFS(fsys, std::span<const std::string_view>(expected.begin(), expected.size()));
}

}  // namespace gopp::fstest
