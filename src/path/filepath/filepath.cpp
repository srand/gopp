// Ported from Go 1.27.3's path/filepath/path.go, path_unix.go, match.go and
// symlink.go, and internal/filepathlite's IsLocal and Localize, for Linux.
#include <gopp/os.hpp>
#include <gopp/path/filepath.hpp>
#include <gopp/strings.hpp>
#include <gopp/syscall.hpp>

#include <algorithm>
#include <string>

namespace gopp::filepath {

const error ErrBadPattern = errors::New("syntax error in pattern");

namespace {

const error errInvalidPath = errors::New("invalid path");

bool isPathSeparator(char c) noexcept { return c == Separator; }

// The names in the directory, sorted.
std::pair<std::vector<string>, error> readDirNames(std::string_view dirname) {
    auto [f, err] = os::Open(dirname);
    if (err) return {{}, err};
    auto [names, rerr] = f->Readdirnames(-1);
    f->Close();
    if (rerr) return {{}, rerr};
    std::sort(names.begin(), names.end());
    return {names, nullptr};
}

error walkDir(std::string_view path, const ptr<fs::DirEntry>& d, const fs::WalkDirFunc& walkDirFn) {
    if (auto err = walkDirFn(path, d, nullptr); err || !d->IsDir()) {
        if (err == SkipDir && d->IsDir()) {
            // Successfully skipped directory.
            err = nullptr;
        }
        return err;
    }

    auto [dirs, err] = os::ReadDir(path);
    if (err) {
        // Second call, to report ReadDir error.
        err = walkDirFn(path, d, err);
        if (err) {
            if (err == SkipDir && d->IsDir()) err = nullptr;
            return err;
        }
    }

    for (const auto& d1 : dirs) {
        string path1 = Join(path, d1->Name());
        if (auto err1 = walkDir(path1, d1, walkDirFn)) {
            if (err1 == SkipDir) break;
            return err1;
        }
    }
    return nullptr;
}

error walk(std::string_view path, const ptr<fs::FileInfo>& info, const WalkFunc& walkFn) {
    if (!info->IsDir()) return walkFn(path, info, nullptr);

    auto [names, err] = readDirNames(path);
    error err1 = walkFn(path, info, err);
    // If err != nil, walk can't walk into this directory. err1 != nil
    // means walkFn want walk to skip this directory or stop walking.
    // Therefore, if one of err and err1 isn't nil, walk will return.
    if (err || err1) {
        // The caller's behavior is controlled by the return value, which
        // is decided by walkFn. walkFn may ignore err and return nil. If
        // walkFn returns SkipDir or SkipAll, it will be handled by the
        // caller. So walk should return whatever walkFn returns.
        return err1;
    }

    for (const auto& name : names) {
        string filename = Join(path, name);
        auto [fileInfo, lerr] = os::Lstat(filename);
        if (lerr) {
            if (auto werr = walkFn(filename, fileInfo, lerr); werr && werr != SkipDir) return werr;
        } else {
            if (auto werr = walk(filename, fileInfo, walkFn)) {
                if (!fileInfo->IsDir() || werr != SkipDir) return werr;
            }
        }
    }
    return nullptr;
}

// Whether path contains any of the magic characters recognized by Match.
bool hasMeta(std::string_view path) { return strings::ContainsAny(path, "*?[\\"); }

// Prepares path for glob matching.
string cleanGlobPath(const string& path) {
    if (path.empty()) return string::fromStatic(".");
    if (path == std::string_view(&Separator, 1)) return path;
    return path.slice(0, path.size() - 1);  // chop off trailing separator
}

// Searches for files matching pattern in the directory dir and appends
// them to matches. If the directory cannot be opened, it returns the
// existing matches. New matches are added in lexicographical order.
error glob(std::string_view dir, std::string_view pattern, std::vector<string>& matches) {
    auto [fi, err] = os::Stat(dir);
    if (err) return nullptr;  // ignore I/O error
    if (!fi->IsDir()) return nullptr;  // ignore I/O error
    auto [d, oerr] = os::Open(dir);
    if (oerr) return nullptr;  // ignore I/O error
    auto names = d->Readdirnames(-1).first;
    d->Close();
    std::sort(names.begin(), names.end());
    for (const auto& n : names) {
        auto [matched, merr] = Match(pattern, n);
        if (merr) return merr;
        if (matched) matches.push_back(Join(dir, n));
    }
    return nullptr;
}

std::pair<std::vector<string>, error> globWithLimit(std::string_view pattern, int depth) {
    // Check pattern is well-formed.
    constexpr int pathSeparatorsLimit = 10000;
    if (depth == pathSeparatorsLimit) return {{}, ErrBadPattern};
    if (auto [_, err] = Match(pattern, ""); err) return {{}, err};
    if (!hasMeta(pattern)) {
        if (auto [_, err] = os::Lstat(pattern); err) return {{}, nullptr};
        return {{string(pattern)}, nullptr};
    }

    auto [dir0, file] = Split(pattern);
    string dir = cleanGlobPath(dir0);

    std::vector<string> matches;
    if (!hasMeta(dir)) {
        auto err = glob(dir, file, matches);
        return {matches, err};
    }

    // Prevent infinite recursion. See issue 15879.
    if (dir == pattern) return {{}, ErrBadPattern};

    auto [m, err] = globWithLimit(dir, depth + 1);
    if (err) return {{}, err};
    for (const auto& d : m) {
        if (auto gerr = glob(d, file, matches)) return {matches, gerr};
    }
    return {matches, nullptr};
}

}  // namespace

bool IsLocal(std::string_view path) {
    if (IsAbs(path) || path.empty()) return false;
    bool hasDots = false;
    for (std::string_view p = path; !p.empty();) {
        auto i = p.find('/');
        std::string_view part = p.substr(0, i);
        p = i == p.npos ? std::string_view() : p.substr(i + 1);
        if (part == "." || part == "..") {
            hasDots = true;
            break;
        }
    }
    string cleaned = hasDots ? Clean(path) : string(path);
    if (cleaned == ".." || cleaned.starts_with("../")) return false;
    return true;
}

std::pair<string, error> Localize(std::string_view path) {
    if (!fs::ValidPath(path)) return {{}, errInvalidPath};
    if (path.find('\0') != path.npos) return {{}, errInvalidPath};
    return {string(path), nullptr};
}

slice<string> SplitList(std::string_view path) {
    if (path.empty()) return slice<string>::make(0);
    return strings::Split(string(path), std::string_view(&ListSeparator, 1));
}

std::pair<bool, error> Match(std::string_view pattern, std::string_view name) {
    auto [matched, err] = path::Match(pattern, name);
    if (err == path::ErrBadPattern) return {false, ErrBadPattern};
    return {matched, err};
}

std::pair<std::vector<string>, error> Glob(std::string_view pattern) { return globWithLimit(pattern, 0); }

std::pair<string, error> Abs(std::string_view path) {
    if (IsAbs(path)) return {Clean(path), nullptr};
    auto [wd, err] = os::Getwd();
    if (err) return {{}, err};
    return {Join(wd, path), nullptr};
}

std::pair<string, error> Rel(std::string_view basepath, std::string_view targpath) {
    string base = Clean(basepath);
    string targ = Clean(targpath);
    if (targ == base) return {string::fromStatic("."), nullptr};
    if (base == ".") base = {};
    // Can't use IsAbs - `\a` and `a` are both relative in Windows.
    bool baseSlashed = !base.empty() && base[0] == Separator;
    bool targSlashed = !targ.empty() && targ[0] == Separator;
    auto cant = [&] {
        return std::pair<string, error>{{}, errors::New("Rel: can't make " + string(targpath) + " relative to " + string(basepath))};
    };
    if (baseSlashed != targSlashed) return cant();
    // Position base[b0:bi] and targ[t0:ti] at the first differing elements.
    std::size_t bl = base.size(), tl = targ.size();
    std::size_t b0 = 0, bi = 0, t0 = 0, ti = 0;
    for (;;) {
        while (bi < bl && base[bi] != Separator) bi++;
        while (ti < tl && targ[ti] != Separator) ti++;
        if (targ.view().substr(t0, ti - t0) != base.view().substr(b0, bi - b0)) break;
        if (bi < bl) bi++;
        if (ti < tl) ti++;
        b0 = bi;
        t0 = ti;
    }
    if (base.view().substr(b0, bi - b0) == "..") return cant();
    if (b0 != bl) {
        // Base elements left. Must go up before going down.
        auto seps = std::count(base.begin() + b0, base.begin() + bl, Separator);
        std::string buf = "..";
        for (decltype(seps) i = 0; i < seps; i++) buf += "/..";
        if (t0 != tl) {
            buf += Separator;
            buf += targ.view().substr(t0);
        }
        return {Clean(buf), nullptr};
    }
    return {targ.slice(t0), nullptr};
}

std::pair<string, error> EvalSymlinks(std::string_view path0) {
    std::string path(path0);
    std::size_t volLen = 0;
    const std::string pathSeparator(1, Separator);
    if (volLen < path.size() && isPathSeparator(path[volLen])) volLen++;
    std::string vol = path.substr(0, volLen);
    std::string dest = vol;
    int linksWalked = 0;
    for (std::size_t start = volLen, end = volLen; start < path.size(); start = end) {
        while (start < path.size() && isPathSeparator(path[start])) start++;
        end = start;
        while (end < path.size() && !isPathSeparator(path[end])) end++;

        // The next path component is in path[start:end].
        std::string_view part = std::string_view(path).substr(start, end - start);
        if (end == start) {
            // No more path components.
            break;
        } else if (part == ".") {
            // Ignore path component ".".
            continue;
        } else if (part == "..") {
            // Back up to previous component if possible. Note that volLen
            // includes any leading slash.

            // Set r to the index of the last slash in dest, after the
            // volume.
            isize r = isize(dest.size()) - 1;
            for (; r >= isize(volLen); r--) {
                if (isPathSeparator(dest[std::size_t(r)])) break;
            }
            if (r < isize(volLen) || std::string_view(dest).substr(std::size_t(r) + 1) == "..") {
                // Either path has no slashes (it's empty or just "C:") or
                // it ends in a ".." we had to keep. Either way, keep this
                // "..".
                if (dest.size() > volLen) dest += pathSeparator;
                dest += "..";
            } else {
                // Discard everything since the last slash.
                dest.resize(std::size_t(r));
            }
            continue;
        }

        // Ordinary path component. Add it to result.
        if (!dest.empty() && !isPathSeparator(dest.back())) dest += pathSeparator;
        dest += part;

        // Resolve symlink.
        auto [fi, err] = os::Lstat(dest);
        if (err) return {{}, err};

        if (!(fi->Mode() & fs::ModeSymlink)) {
            if (!fi->Mode().IsDir() && end < path.size()) return {{}, syscall::ENOTDIR_};
            continue;
        }

        // Found symlink.
        linksWalked++;
        if (linksWalked > 255) return {{}, errors::New("EvalSymlinks: too many links")};

        auto [link, lerr] = os::Readlink(dest);
        if (lerr) return {{}, lerr};

        path = std::string(link.view()) + path.substr(end);

        if (!link.empty() && isPathSeparator(link[0])) {
            // Symlink to absolute path.
            dest = std::string(link.view().substr(0, 1));
            end = 1;
            vol = dest;
            volLen = 1;
        } else {
            // Symlink to relative path; replace last path component in
            // dest.
            isize r = isize(dest.size()) - 1;
            for (; r >= isize(volLen); r--) {
                if (isPathSeparator(dest[std::size_t(r)])) break;
            }
            if (r < isize(volLen)) {
                dest = vol;
            } else {
                dest.resize(std::size_t(r));
            }
            end = 0;
        }
    }
    return {Clean(dest), nullptr};
}

error WalkDir(std::string_view root, const fs::WalkDirFunc& fn) {
    auto [info, err] = os::Lstat(root);
    if (err) {
        err = fn(root, nullptr, err);
    } else {
        err = walkDir(root, fs::FileInfoToDirEntry(info), fn);
    }
    if (err == SkipDir || err == SkipAll) return nullptr;
    return err;
}

error Walk(std::string_view root, const WalkFunc& fn) {
    auto [info, err] = os::Lstat(root);
    if (err) {
        err = fn(root, nullptr, err);
    } else {
        err = walk(root, info, fn);
    }
    if (err == SkipDir || err == SkipAll) return nullptr;
    return err;
}

}  // namespace gopp::filepath
