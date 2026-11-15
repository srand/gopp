// The File defaults, the errors, and the functions over any Fs, ported
// from afero v1.15.0's afero.go, util.go, ioutil.go, path.go, match.go,
// symlink.go and lstater.go.
#include "internal.hpp"

#include <gopp/bytes.hpp>
#include <gopp/fmt.hpp>
#include <gopp/path.hpp>
#include <gopp/strings.hpp>
#include <gopp/sync.hpp>
#include <gopp/unicode.hpp>
#include <gopp/unicode/utf8.hpp>

#include <algorithm>
#include <mutex>
#include <string>

namespace gopp::vfs {

const error ErrFileClosed = errors::New("File is closed");
const error ErrOutOfRange = errors::New("out of range");
const error ErrTooLarge = errors::New("too large");
const error ErrNoSymlink = errors::New("symlink not supported");
const error ErrNoReadlink = errors::New("readlink not supported");

// ---------------------------------------------------------------------------
// File

std::pair<std::vector<string>, error> File::Readdirnames(int n) {
    auto [fi, err] = Readdir(n);
    std::vector<string> names;
    names.reserve(fi.size());
    for (auto& f : fi) names.push_back(f->Name());
    return {names, err};
}

std::pair<std::vector<ptr<fs::DirEntry>>, error> File::ReadDir(int n) {
    auto [items, err] = Readdir(n);
    if (err) return {{}, err};
    std::vector<ptr<fs::DirEntry>> ret;
    ret.reserve(items.size());
    for (auto& fi : items) ret.push_back(fs::FileInfoToDirEntry(fi));
    return {ret, nullptr};
}

std::pair<isize, error> File::WriteString(std::string_view s) { return Write(as_bytes(s)); }

// ---------------------------------------------------------------------------
// ioutil.go

std::pair<std::vector<ptr<fs::FileInfo>>, error> ReadDir(Fs& fsys, std::string_view dirname) {
    auto [f, err] = fsys.Open(dirname);
    if (err) return {{}, err};
    auto [list, rerr] = f->Readdir(-1);
    f->Close();
    if (rerr) return {{}, rerr};
    detail::sortByName(list);
    return {list, nullptr};
}

std::pair<slice<byte>, error> ReadFile(Fs& fsys, std::string_view filename) {
    auto [f, err] = fsys.Open(filename);
    if (err) return {{}, err};
    auto r = io::ReadAll(*f);
    f->Close();
    return r;
}

std::pair<slice<byte>, error> ReadAll(io::Reader& r) { return io::ReadAll(r); }

error WriteFile(Fs& fsys, std::string_view filename, std::span<const byte> data, fs::FileMode perm) {
    auto [f, err] = fsys.OpenFile(filename, os::O_WRONLY_ | os::O_CREATE_ | os::O_TRUNC_, perm);
    if (err) return err;
    auto [n, werr] = f->Write(data);
    if (!werr && static_cast<std::size_t>(n) < data.size()) werr = io::ErrShortWrite;
    if (error err1 = f->Close(); !werr) werr = err1;
    return werr;
}

namespace {

// The random names of TempFile and TempDir, so that there's a good chance
// the file doesn't exist yet.
sync::Mutex randmu;
uint32 randNum = 0;

uint32 reseed() { return static_cast<uint32>(time::Now().UnixNano() + os::Getpid()); }

std::string nextRandom() {
    uint32 r;
    {
        std::lock_guard lock(randmu);
        r = randNum;
        if (r == 0) r = reseed();
        r = r * 1664525 + 1013904223;  // constants from Numerical Recipes
        randNum = r;
    }
    return std::to_string(1000000000 + r % 1000000000).substr(1);
}

void conflicted(int& nconflict) {
    if (++nconflict > 10) {
        std::lock_guard lock(randmu);
        randNum = reseed();
    }
}

}  // namespace

std::pair<ptr<File>, error> TempFile(Fs& fsys, std::string_view dir, std::string_view pattern) {
    string tmp;
    if (dir.empty()) {
        tmp = os::TempDir();
        dir = tmp;
    }
    std::string_view prefix = pattern, suffix;
    if (auto pos = pattern.rfind('*'); pos != pattern.npos) {
        prefix = pattern.substr(0, pos);
        suffix = pattern.substr(pos + 1);
    }
    int nconflict = 0;
    std::pair<ptr<File>, error> r;
    for (int i = 0; i < 10000; i++) {
        string name = filepath::Join(dir, std::string(prefix) + nextRandom() + std::string(suffix));
        r = fsys.OpenFile(name, os::O_RDWR_ | os::O_CREATE_ | os::O_EXCL_, 0600);
        if (os::IsExist(r.second)) {
            conflicted(nconflict);
            continue;
        }
        break;
    }
    return r;
}

std::pair<string, error> TempDir(Fs& fsys, std::string_view dir, std::string_view prefix) {
    string tmp;
    if (dir.empty()) {
        tmp = os::TempDir();
        dir = tmp;
    }
    int nconflict = 0;
    std::pair<string, error> r;
    for (int i = 0; i < 10000; i++) {
        string attempt = filepath::Join(dir, std::string(prefix) + nextRandom());
        r.second = fsys.Mkdir(attempt, 0700);
        if (os::IsExist(r.second)) {
            conflicted(nconflict);
            continue;
        }
        if (!r.second) r.first = attempt;
        break;
    }
    return r;
}

// ---------------------------------------------------------------------------
// path.go and lstater.go

namespace detail {

std::pair<ptr<fs::FileInfo>, error> lstatIfPossible(Fs& fsys, std::string_view path) {
    if (auto* lfs = dynamic_cast<Lstater*>(&fsys)) {
        auto [fi, _, err] = lfs->LstatIfPossible(path);
        return {fi, err};
    }
    return fsys.Stat(path);
}

}  // namespace detail

namespace {

// The names in the directory, sorted.
std::pair<std::vector<string>, error> readDirNames(Fs& fsys, std::string_view dirname) {
    auto [f, err] = fsys.Open(dirname);
    if (err) return {{}, err};
    auto [names, rerr] = f->Readdirnames(-1);
    f->Close();
    if (rerr) return {{}, rerr};
    std::sort(names.begin(), names.end());
    return {names, nullptr};
}

error walk(Fs& fsys, std::string_view path, const ptr<fs::FileInfo>& info, const filepath::WalkFunc& walkFn) {
    if (error err = walkFn(path, info, nullptr)) {
        if (info->IsDir() && err == filepath::SkipDir) return nullptr;
        return err;
    }
    if (!info->IsDir()) return nullptr;

    auto [names, err] = readDirNames(fsys, path);
    if (err) return walkFn(path, info, err);

    for (auto& name : names) {
        string filename = filepath::Join(path, name);
        auto [fileInfo, lerr] = detail::lstatIfPossible(fsys, filename);
        if (lerr) {
            if (error err = walkFn(filename, fileInfo, lerr); err && err != filepath::SkipDir) return err;
        } else if (error err = walk(fsys, filename, fileInfo, walkFn)) {
            if (!fileInfo->IsDir() || err != filepath::SkipDir) return err;
        }
    }
    return nullptr;
}

}  // namespace

error Walk(Fs& fsys, std::string_view root, const filepath::WalkFunc& walkFn) {
    auto [info, err] = detail::lstatIfPossible(fsys, root);
    if (err) return walkFn(root, nullptr, err);
    return walk(fsys, root, info, walkFn);
}

// ---------------------------------------------------------------------------
// match.go

namespace {

// Whether path has any of the characters special to Match.
bool hasMeta(std::string_view path) { return path.find_first_of("*?[") != path.npos; }

// Appends to matches the names in dir that match pattern, in lexical order.
// A directory that can't be read adds none.
error glob(Fs& fsys, std::string_view dir, std::string_view pattern, std::vector<string>& matches) {
    auto [fi, err] = fsys.Stat(dir);
    if (err || !fi->IsDir()) return nullptr;
    auto [d, oerr] = fsys.Open(dir);
    if (oerr) return nullptr;
    auto [names, _] = d->Readdirnames(-1);
    d->Close();
    std::sort(names.begin(), names.end());
    for (auto& n : names) {
        auto [matched, merr] = filepath::Match(pattern, n);
        if (merr) return merr;
        if (matched) matches.push_back(filepath::Join(dir, n));
    }
    return nullptr;
}

}  // namespace

std::pair<std::vector<string>, error> Glob(Fs& fsys, std::string_view pattern) {
    if (!hasMeta(pattern)) {
        // Not every Fs can Lstat.
        if (auto [_, err] = detail::lstatIfPossible(fsys, pattern); err) return {{}, nullptr};
        return {{string(pattern)}, nullptr};
    }

    auto [d, file] = filepath::Split(pattern);
    string dir(d);
    if (dir.view().empty()) {
        dir = string::fromStatic(".");
    } else if (dir != "/") {
        dir = string(dir.view().substr(0, dir.size() - 1));  // the trailing separator
    }

    std::vector<string> matches;
    if (!hasMeta(dir)) {
        if (error err = glob(fsys, dir, file, matches)) return {matches, err};
        return {matches, nullptr};
    }

    auto [m, err] = Glob(fsys, dir);
    if (err) return {{}, err};
    for (auto& md : m) {
        if (error err = glob(fsys, md, file, matches)) return {matches, err};
    }
    return {matches, nullptr};
}

// ---------------------------------------------------------------------------
// util.go

namespace {

error writeReader(Fs& fsys, std::string_view path, io::Reader& r, bool safe) {
    auto dir = filepath::Split(path).first;
    if (!dir.empty()) {
        // rwx, rw, r
        if (error err = fsys.MkdirAll(dir, 0777); err && (safe || err != fs::ErrExist)) return err;
    }
    if (safe) {
        auto [exists, err] = Exists(fsys, path);
        if (err) return err;
        if (exists) return fmt::Errorf("{} already exists", path);
    }
    auto [file, err] = fsys.Create(path);
    if (err) return err;
    auto [_, cerr] = io::Copy(*file, r);
    file->Close();
    return cerr;
}

}  // namespace

error WriteReader(Fs& fsys, std::string_view path, io::Reader& r) { return writeReader(fsys, path, r, false); }

error SafeWriteReader(Fs& fsys, std::string_view path, io::Reader& r) { return writeReader(fsys, path, r, true); }

string GetTempDir(Fs& fsys, std::string_view subPath) {
    auto addSlash = [](string p) { return p.ends_with("/") ? p : p + "/"; };
    string dir = addSlash(os::TempDir());
    if (!subPath.empty()) {
        dir = dir + UnicodeSanitize(subPath);
        if (auto [exists, _] = Exists(fsys, dir); exists) return addSlash(dir);
        if (error err = fsys.MkdirAll(dir, 0777)) panic(err.Error().view());
        dir = addSlash(dir);
    }
    return dir;
}

string UnicodeSanitize(std::string_view s) {
    std::string target;
    target.reserve(s.size());
    for (std::size_t i = 0; i < s.size();) {
        auto [r, size] = utf8::DecodeRuneInString(s.substr(i));
        if (unicode::IsLetter(r) || unicode::IsDigit(r) || unicode::IsMark(r) || r == '.' || r == '/' || r == '\\' ||
            r == '_' || r == '-' || r == '%' || r == ' ' || r == '#') {
            byte buf[utf8::UTFMax];
            int n = utf8::EncodeRune(buf, r);
            target.append(reinterpret_cast<const char*>(buf), static_cast<std::size_t>(n));
        }
        i += static_cast<std::size_t>(size);
    }
    return string(target);
}

namespace detail {

bool readerContainsAny(io::Reader& r, std::span<const std::span<const byte>> subslices) {
    std::size_t largestSlice = 0;
    for (auto& sl : subslices) largestSlice = std::max(largestSlice, sl.size());
    if (largestSlice == 0) return false;

    std::size_t bufflen = largestSlice * 4;
    std::size_t halflen = bufflen / 2;
    std::vector<byte> buff(bufflen);
    std::span<byte> b(buff);
    for (int i = 1;; i++) {
        isize n;
        error err;
        if (i == 1) {
            std::tie(n, err) = io::ReadAtLeast(r, b.first(halflen), static_cast<isize>(halflen));
        } else {
            // Shift left to catch matches across the halves.
            if (i != 2) std::copy(b.begin() + static_cast<std::ptrdiff_t>(halflen), b.end(), b.begin());
            std::tie(n, err) = io::ReadAtLeast(r, b.subspan(halflen), static_cast<isize>(halflen));
        }
        if (n > 0) {
            for (auto& sl : subslices) {
                if (bytes::Contains(b, sl)) return true;
            }
        }
        if (err) break;
    }
    return false;
}

}  // namespace detail

std::pair<bool, error> FileContainsBytes(Fs& fsys, std::string_view filename, std::span<const byte> subslice) {
    return FileContainsAnyBytes(fsys, filename, std::span(&subslice, 1));
}

std::pair<bool, error> FileContainsAnyBytes(Fs& fsys, std::string_view filename,
                                            std::span<const std::span<const byte>> subslices) {
    auto [f, err] = fsys.Open(filename);
    if (err) return {false, err};
    bool found = detail::readerContainsAny(*f, subslices);
    f->Close();
    return {found, nullptr};
}

std::pair<bool, error> DirExists(Fs& fsys, std::string_view path) {
    auto [fi, err] = fsys.Stat(path);
    if (!err && fi->IsDir()) return {true, nullptr};
    if (os::IsNotExist(err)) return {false, nullptr};
    return {false, err};
}

std::pair<bool, error> IsDir(Fs& fsys, std::string_view path) {
    auto [fi, err] = fsys.Stat(path);
    if (err) return {false, err};
    return {fi->IsDir(), nullptr};
}

std::pair<bool, error> IsEmpty(Fs& fsys, std::string_view path) {
    if (auto [b, _] = Exists(fsys, path); !b) return {false, fmt::Errorf("{} path does not exist", strconv::Quote(path))};
    auto [fi, err] = fsys.Stat(path);
    if (err) return {false, err};
    if (fi->IsDir()) {
        auto [f, oerr] = fsys.Open(path);
        if (oerr) return {false, oerr};
        auto [list, rerr] = f->Readdir(-1);
        f->Close();
        if (rerr) return {false, rerr};
        return {list.empty(), nullptr};
    }
    return {fi->Size() == 0, nullptr};
}

std::pair<bool, error> Exists(Fs& fsys, std::string_view path) {
    auto [_, err] = fsys.Stat(path);
    if (!err) return {true, nullptr};
    if (os::IsNotExist(err)) return {false, nullptr};
    return {false, err};
}

}  // namespace gopp::vfs
