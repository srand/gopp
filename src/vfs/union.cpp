// UnionFile, CopyOnWriteFs and CacheOnReadFs, ported from afero v1.15.0's
// unionFile.go, copyOnWriteFs.go and cacheOnReadFs.go.
#include "internal.hpp"

#include <gopp/fmt.hpp>

#include <cerrno>
#include <map>

namespace gopp::vfs {

const error BADFD = syscall::errnoError(EBADFD);

// ---------------------------------------------------------------------------
// UnionFile

error UnionFile::Close() {
    // The base first, so that the layer's file is the newer: closing the
    // layer first would make the cache stale the next time.
    if (Base) Base->Close();
    if (Layer) return Layer->Close();
    return BADFD;
}

std::pair<isize, error> UnionFile::Read(std::span<byte> s) {
    if (Layer) {
        auto [n, err] = Layer->Read(s);
        if ((!err || err == io::EOF_) && Base) {
            // Move the base's offset along: the next call may write there,
            // or seek from it. Only a failed seek replaces err, which may
            // be io::EOF_.
            if (auto [_, seekErr] = Base->Seek(n, io::SeekCurrent); seekErr) err = seekErr;
        }
        return {n, err};
    }
    if (Base) return Base->Read(s);
    return {0, BADFD};
}

std::pair<isize, error> UnionFile::ReadAt(std::span<byte> s, int64 o) {
    if (Layer) {
        auto [n, err] = Layer->ReadAt(s, o);
        if ((!err || err == io::EOF_) && Base) err = Base->Seek(o + n, io::SeekStart).second;
        return {n, err};
    }
    if (Base) return Base->ReadAt(s, o);
    return {0, BADFD};
}

std::pair<int64, error> UnionFile::Seek(int64 o, int w) {
    if (Layer) {
        auto [pos, err] = Layer->Seek(o, w);
        if ((!err || err == io::EOF_) && Base) err = Base->Seek(o, w).second;
        return {pos, err};
    }
    if (Base) return Base->Seek(o, w);
    return {0, BADFD};
}

std::pair<isize, error> UnionFile::Write(std::span<const byte> s) {
    if (Layer) {
        auto [n, err] = Layer->Write(s);
        if (!err && Base) err = Base->Write(s).second;
        return {n, err};
    }
    if (Base) return Base->Write(s);
    return {0, BADFD};
}

std::pair<isize, error> UnionFile::WriteAt(std::span<const byte> s, int64 o) {
    if (Layer) {
        auto [n, err] = Layer->WriteAt(s, o);
        if (!err && Base) err = Base->WriteAt(s, o).second;
        return {n, err};
    }
    if (Base) return Base->WriteAt(s, o);
    return {0, BADFD};
}

string UnionFile::Name() const {
    if (Layer) return Layer->Name();
    return Base->Name();
}

namespace {

// The layer's entries, and those of the base that the layer doesn't have,
// sorted by name (afero's order is a map's, at random).
std::pair<std::vector<ptr<fs::FileInfo>>, error> defaultUnionMergeDirs(std::vector<ptr<fs::FileInfo>> lofi,
                                                                       std::vector<ptr<fs::FileInfo>> bofi) {
    std::map<string, ptr<fs::FileInfo>> files;
    for (auto& fi : lofi) files.insert_or_assign(fi->Name(), fi);
    for (auto& fi : bofi) files.try_emplace(fi->Name(), fi);
    std::vector<ptr<fs::FileInfo>> rfi;
    rfi.reserve(files.size());
    for (auto& [_, fi] : files) rfi.push_back(fi);
    return {rfi, nullptr};
}

}  // namespace

std::pair<std::vector<ptr<fs::FileInfo>>, error> UnionFile::Readdir(int c) {
    if (off_ == 0) {
        std::vector<ptr<fs::FileInfo>> lfi, bfi;
        if (Layer) {
            auto [fi, err] = Layer->Readdir(-1);
            if (err) return {{}, err};
            lfi = std::move(fi);
        }
        if (Base) {
            auto [fi, err] = Base->Readdir(-1);
            if (err) return {{}, err};
            bfi = std::move(fi);
        }
        auto [merged, err] = Merger ? Merger(std::move(lfi), std::move(bfi)) : defaultUnionMergeDirs(std::move(lfi), std::move(bfi));
        if (err) return {{}, err};
        files_.insert(files_.end(), merged.begin(), merged.end());
    }
    std::vector<ptr<fs::FileInfo>> files(files_.begin() + static_cast<std::ptrdiff_t>(std::min(off_, files_.size())), files_.end());
    if (c <= 0) return {files, nullptr};
    if (files.empty()) return {{}, io::EOF_};
    auto n = std::min(static_cast<std::size_t>(c), files.size());
    files.resize(n);
    off_ += n;
    return {files, nullptr};
}

std::pair<std::vector<string>, error> UnionFile::Readdirnames(int c) {
    auto [rfi, err] = Readdir(c);
    if (err) return {{}, err};
    std::vector<string> names;
    for (auto& fi : rfi) names.push_back(fi->Name());
    return {names, nullptr};
}

std::pair<ptr<fs::FileInfo>, error> UnionFile::Stat() {
    if (Layer) return Layer->Stat();
    if (Base) return Base->Stat();
    return {nullptr, BADFD};
}

error UnionFile::Sync() {
    if (Layer) {
        error err = Layer->Sync();
        if (!err && Base) err = Base->Sync();
        return err;
    }
    if (Base) return Base->Sync();
    return BADFD;
}

error UnionFile::Truncate(int64 s) {
    if (Layer) {
        error err = Layer->Truncate(s);
        if (!err && Base) err = Base->Truncate(s);
        return err;
    }
    if (Base) return Base->Truncate(s);
    return BADFD;
}

std::pair<isize, error> UnionFile::WriteString(std::string_view s) {
    if (Layer) {
        auto [n, err] = Layer->WriteString(s);
        if (!err && Base) err = Base->WriteString(s).second;
        return {n, err};
    }
    if (Base) return Base->WriteString(s);
    return {0, BADFD};
}

// ---------------------------------------------------------------------------
// Copying to a layer

namespace {

error copyFile(Fs& layer, std::string_view name, File& bfh) {
    // First make sure the directory exists.
    string dir = filepath::Dir(name);
    auto [exists, err] = Exists(layer, dir);
    if (err) return err;
    if (!exists) {
        if (error err = layer.MkdirAll(dir, 0777)) return err;
    }

    // Create the file in the layer.
    auto [lfh, cerr] = layer.Create(name);
    if (cerr) return cerr;
    auto [n, copyErr] = io::Copy(*lfh, bfh);
    if (copyErr) {
        // If anything fails, clean up the file.
        layer.Remove(name);
        lfh->Close();
        return copyErr;
    }

    auto [bfi, serr] = bfh.Stat();
    if (serr || bfi->Size() != n) {
        layer.Remove(name);
        lfh->Close();
        return syscall::errnoError(EIO);
    }

    if (error err = lfh->Close()) {
        layer.Remove(name);
        lfh->Close();
        return err;
    }
    return layer.Chtimes(name, bfi->ModTime(), bfi->ModTime());
}

}  // namespace

namespace detail {

error copyToLayer(Fs& base, Fs& layer, std::string_view name) {
    auto [bfh, err] = base.Open(name);
    if (err) return err;
    error cerr = copyFile(layer, name, *bfh);
    bfh->Close();
    return cerr;
}

error copyFileToLayer(Fs& base, Fs& layer, std::string_view name, int flag, fs::FileMode perm) {
    auto [bfh, err] = base.OpenFile(name, flag, perm);
    if (err) return err;
    error cerr = copyFile(layer, name, *bfh);
    bfh->Close();
    return cerr;
}

}  // namespace detail

// ---------------------------------------------------------------------------
// CopyOnWriteFs

namespace {

// Whether err, or the error of the PathError err is, means that the file
// doesn't exist.
bool isNotExist(const error& err) {
    error e = err;
    if (auto* pe = dynamic_cast<const fs::PathError*>(err.get())) e = pe->Err;
    return e == fs::ErrNotExist || e == syscall::ENOENT_ || e == syscall::ENOTDIR_;
}

}  // namespace

ptr<Fs> NewCopyOnWriteFs(ptr<Fs> base, ptr<Fs> layer) {
    return make_ptr<CopyOnWriteFs>(std::move(base), std::move(layer));
}

// Whether the file is in the base but not in the layer.
std::pair<bool, error> CopyOnWriteFs::isBaseFile(std::string_view name) {
    if (auto [_, err] = layer_->Stat(name); !err) return {false, nullptr};
    auto [_, err] = base_->Stat(name);
    if (err) {
        if (auto* oerr = dynamic_cast<const fs::PathError*>(err.get())) {
            if (oerr->Err == fs::ErrNotExist || oerr->Err == syscall::ENOENT_ || oerr->Err == syscall::ENOTDIR_) {
                return {false, nullptr};
            }
        }
        if (err == syscall::ENOENT_) return {false, nullptr};
    }
    return {true, err};
}

error CopyOnWriteFs::Chtimes(std::string_view name, time::Time atime, time::Time mtime) {
    auto [b, err] = isBaseFile(name);
    if (err) return err;
    if (b) {
        if (error err = detail::copyToLayer(*base_, *layer_, name)) return err;
    }
    return layer_->Chtimes(name, atime, mtime);
}

error CopyOnWriteFs::Chmod(std::string_view name, fs::FileMode mode) {
    auto [b, err] = isBaseFile(name);
    if (err) return err;
    if (b) {
        if (error err = detail::copyToLayer(*base_, *layer_, name)) return err;
    }
    return layer_->Chmod(name, mode);
}

error CopyOnWriteFs::Chown(std::string_view name, int uid, int gid) {
    auto [b, err] = isBaseFile(name);
    if (err) return err;
    if (b) {
        if (error err = detail::copyToLayer(*base_, *layer_, name)) return err;
    }
    return layer_->Chown(name, uid, gid);
}

std::pair<ptr<fs::FileInfo>, error> CopyOnWriteFs::Stat(std::string_view name) {
    auto [fi, err] = layer_->Stat(name);
    if (err) {
        if (isNotExist(err)) return base_->Stat(name);
        return {nullptr, err};
    }
    return {fi, nullptr};
}

std::tuple<ptr<fs::FileInfo>, bool, error> CopyOnWriteFs::LstatIfPossible(std::string_view name) {
    if (auto* llayer = dynamic_cast<Lstater*>(layer_.get())) {
        auto [fi, b, err] = llayer->LstatIfPossible(name);
        if (!err) return {fi, b, nullptr};
        if (!isNotExist(err)) return {nullptr, b, err};
    }
    if (auto* lbase = dynamic_cast<Lstater*>(base_.get())) {
        auto [fi, b, err] = lbase->LstatIfPossible(name);
        if (!err) return {fi, b, nullptr};
        if (!isNotExist(err)) return {nullptr, b, err};
    }
    auto [fi, err] = Stat(name);
    return {fi, false, err};
}

error CopyOnWriteFs::SymlinkIfPossible(std::string_view oldname, std::string_view newname) {
    if (auto* slayer = dynamic_cast<Linker*>(layer_.get())) return slayer->SymlinkIfPossible(oldname, newname);
    return detail::linkError("symlink", oldname, newname, ErrNoSymlink);
}

std::pair<string, error> CopyOnWriteFs::ReadlinkIfPossible(std::string_view name) {
    if (auto* rlayer = dynamic_cast<LinkReader*>(layer_.get())) return rlayer->ReadlinkIfPossible(name);
    if (auto* rbase = dynamic_cast<LinkReader*>(base_.get())) return rbase->ReadlinkIfPossible(name);
    return {{}, detail::pathError("readlink", name, ErrNoReadlink)};
}

// Renaming a file that's only in the base isn't permitted.
error CopyOnWriteFs::Rename(std::string_view oldname, std::string_view newname) {
    auto [b, err] = isBaseFile(oldname);
    if (err) return err;
    if (b) return syscall::EPERM_;
    return layer_->Rename(oldname, newname);
}

// Removing a file that's only in the base isn't permitted. If it's in the
// base and the layer, only the layer's is removed.
error CopyOnWriteFs::Remove(std::string_view name) {
    error err = layer_->Remove(name);
    if (err == syscall::ENOENT_) {
        if (auto [_, serr] = base_->Stat(name); !serr) return syscall::EPERM_;
        return syscall::ENOENT_;
    }
    return err;
}

error CopyOnWriteFs::RemoveAll(std::string_view name) {
    error err = layer_->RemoveAll(name);
    if (err == syscall::ENOENT_) {
        if (auto [_, serr] = base_->Stat(name); !serr) return syscall::EPERM_;
        return syscall::ENOENT_;
    }
    return err;
}

std::pair<ptr<File>, error> CopyOnWriteFs::OpenFile(std::string_view name, int flag, fs::FileMode perm) {
    auto [b, err] = isBaseFile(name);
    if (err) return {nullptr, err};

    if (flag & detail::writeFlags) {
        if (b) {
            if (error err = detail::copyToLayer(*base_, *layer_, name)) return {nullptr, err};
            return layer_->OpenFile(name, flag, perm);
        }

        string dir = filepath::Dir(name);
        auto [isaDir, derr] = IsDir(*base_, dir);
        if (derr && !os::IsNotExist(derr)) return {nullptr, derr};
        if (isaDir) {
            if (error err = layer_->MkdirAll(dir, 0777)) return {nullptr, err};
            return layer_->OpenFile(name, flag, perm);
        }

        auto [layerDir, lerr] = IsDir(*layer_, dir);
        if (lerr) return {nullptr, lerr};
        if (layerDir) return layer_->OpenFile(name, flag, perm);

        return {nullptr, detail::pathError("open", name, syscall::ENOTDIR_)};
    }
    if (b) return base_->OpenFile(name, flag, perm);
    return layer_->OpenFile(name, flag, perm);
}

// The nine cases of a union: the layer and the base each without the
// file, with it as a file, or with it as a directory.
std::pair<ptr<File>, error> CopyOnWriteFs::Open(std::string_view name) {
    // The layer overrides the base, so it's checked first.
    auto [b, err] = isBaseFile(name);
    if (err) return {nullptr, err};

    // Not in the layer: the base's, whatever it is.
    if (b) return base_->Open(name);

    // A file in the layer: the layer's.
    auto [dir, derr] = IsDir(*layer_, name);
    if (derr) return {nullptr, derr};
    if (!dir) return layer_->Open(name);

    // A directory in the layer: a file or unreadable in the base gives the
    // layer's; a directory in the base gives the union of both.
    auto [baseDir, berr] = IsDir(*base_, name);
    if (!baseDir || berr) return layer_->Open(name);

    auto [bfile, bErr] = base_->Open(name);
    auto [lfile, lErr] = layer_->Open(name);
    // Errors here mean something is very wrong.
    if (bErr || lErr) return {nullptr, fmt::Errorf("BaseErr: {}\nOverlayErr: {}", bErr, lErr)};
    return {make_ptr<UnionFile>(bfile, lfile), nullptr};
}

error CopyOnWriteFs::Mkdir(std::string_view name, fs::FileMode perm) {
    auto [dir, err] = IsDir(*base_, name);
    if (err) return layer_->MkdirAll(name, perm);
    if (dir) return ErrFileExists;
    return layer_->MkdirAll(name, perm);
}

error CopyOnWriteFs::MkdirAll(std::string_view name, fs::FileMode perm) {
    auto [dir, err] = IsDir(*base_, name);
    if (err) return layer_->MkdirAll(name, perm);
    // As os::MkdirAll does.
    if (dir) return nullptr;
    return layer_->MkdirAll(name, perm);
}

std::pair<ptr<File>, error> CopyOnWriteFs::Create(std::string_view name) {
    return OpenFile(name, os::O_CREATE_ | os::O_TRUNC_ | os::O_RDWR_, 0666);
}

// ---------------------------------------------------------------------------
// CacheOnReadFs

ptr<Fs> NewCacheOnReadFs(ptr<Fs> base, ptr<Fs> layer, time::Duration cacheTime) {
    return make_ptr<CacheOnReadFs>(std::move(base), std::move(layer), cacheTime);
}

// cacheMiss: not in the layer, maybe in the base. cacheStale: in both, and
// the base's is newer. cacheHit: in the layer; with a cacheTime, in the
// base too, and the layer's is as new. cacheLocal: in the layer only, as
// when the layer was written to directly.
std::tuple<CacheOnReadFs::cacheState, ptr<fs::FileInfo>, error> CacheOnReadFs::cacheStatus(std::string_view name) {
    auto [lfi, err] = layer_->Stat(name);
    if (!err) {
        if (cacheTime_ == time::Duration(0)) return {cacheHit, lfi, nullptr};
        if (lfi->ModTime().Add(cacheTime_).Before(time::Now())) {
            auto [bfi, berr] = base_->Stat(name);
            if (berr) return {cacheLocal, lfi, nullptr};
            if (bfi->ModTime().After(lfi->ModTime())) return {cacheStale, bfi, nullptr};
        }
        return {cacheHit, lfi, nullptr};
    }
    if (err == syscall::ENOENT_ || os::IsNotExist(err)) return {cacheMiss, nullptr, nullptr};
    return {cacheMiss, nullptr, err};
}

error CacheOnReadFs::Chtimes(std::string_view name, time::Time atime, time::Time mtime) {
    auto [st, _, err] = cacheStatus(name);
    if (err) return err;
    switch (st) {
    case cacheLocal:
        break;
    case cacheHit:
        err = base_->Chtimes(name, atime, mtime);
        break;
    case cacheStale:
    case cacheMiss:
        if (error err = detail::copyToLayer(*base_, *layer_, name)) return err;
        err = base_->Chtimes(name, atime, mtime);
        break;
    }
    if (err) return err;
    return layer_->Chtimes(name, atime, mtime);
}

error CacheOnReadFs::Chmod(std::string_view name, fs::FileMode mode) {
    auto [st, _, err] = cacheStatus(name);
    if (err) return err;
    switch (st) {
    case cacheLocal:
        break;
    case cacheHit:
        err = base_->Chmod(name, mode);
        break;
    case cacheStale:
    case cacheMiss:
        if (error err = detail::copyToLayer(*base_, *layer_, name)) return err;
        err = base_->Chmod(name, mode);
        break;
    }
    if (err) return err;
    return layer_->Chmod(name, mode);
}

error CacheOnReadFs::Chown(std::string_view name, int uid, int gid) {
    auto [st, _, err] = cacheStatus(name);
    if (err) return err;
    switch (st) {
    case cacheLocal:
        break;
    case cacheHit:
        err = base_->Chown(name, uid, gid);
        break;
    case cacheStale:
    case cacheMiss:
        if (error err = detail::copyToLayer(*base_, *layer_, name)) return err;
        err = base_->Chown(name, uid, gid);
        break;
    }
    if (err) return err;
    return layer_->Chown(name, uid, gid);
}

std::pair<ptr<fs::FileInfo>, error> CacheOnReadFs::Stat(std::string_view name) {
    auto [st, fi, err] = cacheStatus(name);
    if (err) return {nullptr, err};
    // cacheStale has the base's FileInfo; cacheHit and cacheLocal the
    // layer's.
    if (st == cacheMiss) return base_->Stat(name);
    return {fi, nullptr};
}

error CacheOnReadFs::Rename(std::string_view oldname, std::string_view newname) {
    auto [st, _, err] = cacheStatus(oldname);
    if (err) return err;
    switch (st) {
    case cacheLocal:
        break;
    case cacheHit:
        err = base_->Rename(oldname, newname);
        break;
    case cacheStale:
    case cacheMiss:
        if (error err = detail::copyToLayer(*base_, *layer_, oldname)) return err;
        err = base_->Rename(oldname, newname);
        break;
    }
    if (err) return err;
    return layer_->Rename(oldname, newname);
}

error CacheOnReadFs::Remove(std::string_view name) {
    auto [st, _, err] = cacheStatus(name);
    if (err) return err;
    if (st != cacheLocal) err = base_->Remove(name);
    if (err) return err;
    return layer_->Remove(name);
}

error CacheOnReadFs::RemoveAll(std::string_view name) {
    auto [st, _, err] = cacheStatus(name);
    if (err) return err;
    if (st != cacheLocal) err = base_->RemoveAll(name);
    if (err) return err;
    return layer_->RemoveAll(name);
}

std::pair<ptr<File>, error> CacheOnReadFs::OpenFile(std::string_view name, int flag, fs::FileMode perm) {
    auto [st, _, err] = cacheStatus(name);
    if (err) return {nullptr, err};
    if (st != cacheLocal && st != cacheHit) {
        if (error err = detail::copyFileToLayer(*base_, *layer_, name, flag, perm)) return {nullptr, err};
    }
    if (flag & detail::writeFlags) {
        auto [bfi, berr] = base_->OpenFile(name, flag, perm);
        if (berr) return {nullptr, berr};
        auto [lfi, lerr] = layer_->OpenFile(name, flag, perm);
        if (lerr) {
            // afero: what if O_TRUNC was set, and opening the layer's
            // failed?
            bfi->Close();
            return {nullptr, lerr};
        }
        return {make_ptr<UnionFile>(bfi, lfi), nullptr};
    }
    return layer_->OpenFile(name, flag, perm);
}

std::pair<ptr<File>, error> CacheOnReadFs::Open(std::string_view name) {
    auto [st, fi, err] = cacheStatus(name);
    if (err) return {nullptr, err};

    switch (st) {
    case cacheLocal:
        return layer_->Open(name);

    case cacheMiss: {
        auto [bfi, berr] = base_->Stat(name);
        if (berr) return {nullptr, berr};
        if (bfi->IsDir()) return base_->Open(name);
        if (error err = detail::copyToLayer(*base_, *layer_, name)) return {nullptr, err};
        return layer_->Open(name);
    }

    case cacheStale:
        if (!fi->IsDir()) {
            if (error err = detail::copyToLayer(*base_, *layer_, name)) return {nullptr, err};
            return layer_->Open(name);
        }
        break;

    case cacheHit:
        if (!fi->IsDir()) return layer_->Open(name);
        break;
    }
    // The directories of cacheHit and cacheStale.
    auto [bfile, _b] = base_->Open(name);
    auto [lfile, lerr] = layer_->Open(name);
    if (lerr && !bfile) return {nullptr, lerr};
    return {make_ptr<UnionFile>(bfile, lfile), nullptr};
}

error CacheOnReadFs::Mkdir(std::string_view name, fs::FileMode perm) {
    if (error err = base_->Mkdir(name, perm)) return err;
    // MkdirAll: the cache may not have the parents.
    return layer_->MkdirAll(name, perm);
}

error CacheOnReadFs::MkdirAll(std::string_view name, fs::FileMode perm) {
    if (error err = base_->MkdirAll(name, perm)) return err;
    return layer_->MkdirAll(name, perm);
}

std::pair<ptr<File>, error> CacheOnReadFs::Create(std::string_view name) {
    auto [bfh, err] = base_->Create(name);
    if (err) return {nullptr, err};
    auto [lfh, lerr] = layer_->Create(name);
    if (lerr) {
        // afero: should the base's be removed? Then it would have to
        // remember whether the file existed before.
        bfh->Close();
        return {nullptr, lerr};
    }
    return {make_ptr<UnionFile>(bfh, lfh), nullptr};
}

}  // namespace gopp::vfs
