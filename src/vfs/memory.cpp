// MemoryFs and its files, ported from afero v1.15.0's memmap.go and mem/.
#include "internal.hpp"
#include "mem.hpp"

#include <gopp/path/filepath.hpp>

#include <algorithm>
#include <map>
#include <mutex>
#include <shared_mutex>

namespace gopp::vfs {

namespace mem {

namespace {

const error errNotDir = errors::New("not a dir");
const error errReadOnly = errors::New("file handle is read only");
const error errNegativeOffset = errors::New("negative offset");

}  // namespace

string FileData::Name() {
    std::lock_guard lock(mu);
    return name;
}

ptr<FileData> CreateFile(std::string_view name) {
    auto f = make_ptr<FileData>(string(name));
    f->mode = fs::ModeTemporary;
    f->modtime = time::Now();
    return f;
}

ptr<FileData> CreateDir(std::string_view name) {
    auto f = make_ptr<FileData>(string(name));
    f->memDir = std::make_unique<std::map<string, ptr<FileData>>>();
    f->dir = true;
    f->modtime = time::Now();
    return f;
}

void ChangeFileName(FileData& f, std::string_view newname) {
    std::lock_guard lock(f.mu);
    f.name = string(newname);
}

void SetMode(FileData& f, fs::FileMode mode) {
    std::lock_guard lock(f.mu);
    f.mode = mode;
}

void SetModTime(FileData& f, time::Time mtime) {
    std::lock_guard lock(f.mu);
    f.modtime = mtime;
}

void SetUID(FileData& f, int uid) {
    std::lock_guard lock(f.mu);
    f.uid = uid;
}

void SetGID(FileData& f, int gid) {
    std::lock_guard lock(f.mu);
    f.gid = gid;
}

void InitializeDir(FileData& d) {
    if (!d.memDir) {
        d.dir = true;
        d.memDir = std::make_unique<std::map<string, ptr<FileData>>>();
    }
}

void AddToMemDir(FileData& dir, const ptr<FileData>& f) { (*dir.memDir)[f->name] = f; }

void RemoveFromMemDir(FileData& dir, FileData& f) {
    if (dir.memDir) dir.memDir->erase(f.name);
}

std::vector<ptr<FileData>> Files(FileData& d) {
    std::vector<ptr<FileData>> files;
    if (!d.memDir) return files;
    files.reserve(d.memDir->size());
    for (auto& [_, f] : *d.memDir) files.push_back(f);  // the map sorts them
    return files;
}

string FileInfo::Name() const {
    std::lock_guard lock(d_->mu);
    return string(filepath::Split(d_->name).second);
}

int64 FileInfo::Size() const {
    if (IsDir()) return 42;
    std::lock_guard lock(d_->mu);
    return static_cast<int64>(d_->data.size());
}

fs::FileMode FileInfo::Mode() const {
    std::lock_guard lock(d_->mu);
    return d_->mode;
}

time::Time FileInfo::ModTime() const {
    std::lock_guard lock(d_->mu);
    return d_->modtime;
}

bool FileInfo::IsDir() const {
    std::lock_guard lock(d_->mu);
    return d_->dir;
}

ptr<fs::FileInfo> GetFileInfo(const ptr<FileData>& f) { return make_ptr<FileInfo>(f); }

ptr<File> NewFileHandle(ptr<FileData> data) { return make_ptr<File>(std::move(data), false); }
ptr<File> NewReadOnlyFileHandle(ptr<FileData> data) { return make_ptr<File>(std::move(data), true); }

error File::Open() {
    at_ = 0;
    std::lock_guard lock(fileData_->mu);
    readDirCount_ = 0;
    closed_ = false;
    return nullptr;
}

error File::Close() {
    std::lock_guard lock(fileData_->mu);
    closed_ = true;
    if (!readOnly_) fileData_->modtime = time::Now();
    return nullptr;
}

std::pair<std::vector<ptr<fs::FileInfo>>, error> File::Readdir(int count) {
    std::unique_lock lock(fileData_->mu);
    if (!fileData_->dir) return {{}, detail::pathError("readdir", fileData_->name, errNotDir)};
    auto all = Files(*fileData_);
    auto first = std::min(static_cast<std::size_t>(readDirCount_), all.size());
    std::size_t left = all.size() - first;
    std::size_t outLength = left;
    error err;
    if (count > 0) {
        outLength = std::min(left, static_cast<std::size_t>(count));
        if (left == 0) err = io::EOF_;
    }
    readDirCount_ += static_cast<int64>(outLength);
    lock.unlock();

    std::vector<ptr<fs::FileInfo>> res;
    res.reserve(outLength);
    for (std::size_t i = 0; i < outLength; i++) res.push_back(GetFileInfo(all[first + i]));
    return {res, err};
}

std::pair<std::vector<string>, error> File::Readdirnames(int n) {
    auto [fi, err] = Readdir(n);
    std::vector<string> names;
    names.reserve(fi.size());
    for (auto& f : fi) names.push_back(string(filepath::Split(f->Name()).second));
    return {names, err};
}

std::pair<isize, error> File::readAt(std::span<byte> p, int64 off) {
    auto size = static_cast<int64>(fileData_->data.size());
    if (!p.empty() && off == size) return {0, io::EOF_};
    if (off > size) return {0, io::ErrUnexpectedEOF};
    auto n = std::min(static_cast<int64>(p.size()), size - off);
    std::copy_n(fileData_->data.begin() + off, n, p.begin());
    return {static_cast<isize>(n), nullptr};
}

std::pair<isize, error> File::Read(std::span<byte> p) {
    std::lock_guard lock(fileData_->mu);
    if (closed_) return {0, ErrFileClosed};
    auto [n, err] = readAt(p, at_);
    at_ += n;
    return {n, err};
}

std::pair<isize, error> File::ReadAt(std::span<byte> p, int64 off) {
    std::lock_guard lock(fileData_->mu);
    if (closed_) return {0, ErrFileClosed};
    if (off < 0) return {0, detail::pathError("readat", fileData_->name, errNegativeOffset)};
    auto [n, err] = readAt(p, off);
    // io.ReaderAt: fewer bytes than asked come with an error.
    if (!err && static_cast<std::size_t>(n) < p.size()) err = io::EOF_;
    return {n, err};
}

error File::notWritable(std::string_view op) { return detail::pathError(op, fileData_->Name(), errReadOnly); }

error File::Truncate(int64 size) {
    if (closed_) return ErrFileClosed;
    if (readOnly_) return notWritable("truncate");
    if (size < 0) return ErrOutOfRange;
    std::lock_guard lock(fileData_->mu);
    fileData_->data.resize(static_cast<std::size_t>(size));
    fileData_->modtime = time::Now();
    return nullptr;
}

std::pair<int64, error> File::Seek(int64 offset, int whence) {
    if (closed_) return {0, ErrFileClosed};
    switch (whence) {
    case io::SeekStart:
        at_ = offset;
        break;
    case io::SeekCurrent:
        at_ += offset;
        break;
    case io::SeekEnd: {
        std::lock_guard lock(fileData_->mu);
        at_ = static_cast<int64>(fileData_->data.size()) + offset;
        break;
    }
    }
    return {at_, nullptr};
}

void File::writeAt(std::span<const byte> p, int64 off) {
    auto& data = fileData_->data;
    auto end = static_cast<std::size_t>(off) + p.size();
    // Past the end, the gap fills with zeros.
    if (end > data.size()) data.resize(end);
    std::copy(p.begin(), p.end(), data.begin() + off);
    fileData_->modtime = time::Now();
}

std::pair<isize, error> File::Write(std::span<const byte> p) {
    if (closed_) return {0, ErrFileClosed};
    if (readOnly_) return {0, notWritable("write")};
    std::lock_guard lock(fileData_->mu);
    int64 cur = at_;
    if (cur < 0) return {0, detail::pathError("write", fileData_->name, errNegativeOffset)};
    writeAt(p, cur);
    at_ += static_cast<int64>(p.size());
    return {static_cast<isize>(p.size()), nullptr};
}

std::pair<isize, error> File::WriteAt(std::span<const byte> p, int64 off) {
    if (closed_) return {0, ErrFileClosed};
    if (readOnly_) return {0, notWritable("write")};
    std::lock_guard lock(fileData_->mu);
    if (off < 0) return {0, detail::pathError("writeat", fileData_->name, errNegativeOffset)};
    writeAt(p, off);
    return {static_cast<isize>(p.size()), nullptr};
}

}  // namespace mem

namespace {

// The mode bits Chmod changes, as os.Chmod documents.
constexpr fs::FileMode chmodBits = fs::ModePerm | fs::ModeSetuid | fs::ModeSetgid | fs::ModeSticky;

}  // namespace

namespace detail {

string normalizePath(std::string_view path) {
    string p = filepath::Clean(path);
    if (p == "." || p == "..") return string::fromStatic("/");
    return p;
}

// The state of a MemoryFs. The ...Locked methods expect mu held.
struct memoryFs {
    memoryFs() {
        auto root = mem::CreateDir("/");
        mem::SetMode(*root, fs::ModeDir | 0755);
        data.emplace(string::fromStatic("/"), root);
    }

    // The named file, or nil.
    ptr<mem::FileData> openLocked(std::string_view name) {
        auto it = data.find(normalizePath(name));
        return it == data.end() ? nullptr : it->second;
    }

    ptr<mem::FileData> findParentLocked(mem::FileData& f) {
        auto pdir = filepath::Split(f.Name()).first;
        return openLocked(filepath::Clean(pdir));
    }

    // Adds f to its directory, creating the directory, and its parents,
    // with perm if it doesn't exist.
    void registerWithParentLocked(const ptr<mem::FileData>& f, fs::FileMode perm) {
        if (!f) return;
        auto parent = findParentLocked(*f);
        if (!parent) {
            string pdir = filepath::Dir(filepath::Clean(f->Name()));
            if (mkdirLocked(pdir, perm)) return;
            parent = openLocked(pdir);
            if (!parent) return;
        }
        if (parent == f) return;  // the root
        std::lock_guard lock(parent->mu);
        mem::InitializeDir(*parent);
        mem::AddToMemDir(*parent, f);
    }

    error unRegisterWithParentLocked(std::string_view name) {
        auto f = openLocked(name);
        if (!f) return ErrFileNotFound;
        // afero panics if the parent is gone, as it is for the files of a
        // directory that Remove removed.
        auto parent = findParentLocked(*f);
        if (!parent) return nullptr;
        std::lock_guard lock(parent->mu);
        mem::RemoveFromMemDir(*parent, *f);
        return nullptr;
    }

    // Go's lockfreeMkdir: makes the directory unless it exists;
    // ErrFileExists if a file has the name.
    error mkdirLocked(std::string_view name, fs::FileMode perm) {
        string n = normalizePath(name);
        if (auto it = data.find(n); it != data.end()) {
            std::lock_guard lock(it->second->mu);
            if (!it->second->dir) return ErrFileExists;
            return nullptr;
        }
        auto item = mem::CreateDir(n);
        mem::SetMode(*item, fs::ModeDir | perm);
        data.emplace(n, item);
        registerWithParentLocked(item, perm);
        return nullptr;
    }

    // The files under name, shallowest first.
    std::vector<ptr<mem::FileData>> findDescendantsLocked(std::string_view name) {
        string prefix = string(name) + "/";
        std::vector<ptr<mem::FileData>> descendants;
        for (auto& [p, f] : data) {
            if (p.starts_with(prefix)) descendants.push_back(f);
        }
        auto depth = [](const ptr<mem::FileData>& f) {
            string n = f->Name();
            return std::count(n.view().begin(), n.view().end(), '/');
        };
        std::stable_sort(descendants.begin(), descendants.end(),
                         [&](const ptr<mem::FileData>& a, const ptr<mem::FileData>& b) { return depth(a) < depth(b); });
        return descendants;
    }

    error renameDescendantsLocked(std::string_view oldname, std::string_view newname) {
        std::vector<string> removes;
        for (auto& desc : findDescendantsLocked(oldname)) {
            string descName = desc->Name();
            // The first oldname in it, as Go's strings.Replace(..., 1).
            auto pos = descName.find(oldname);
            string descNewName = string(descName.view().substr(0, pos)) + newname + descName.view().substr(pos + oldname.size());
            if (error err = unRegisterWithParentLocked(descName)) return err;
            removes.push_back(descName);
            mem::ChangeFileName(*desc, descNewName);
            data.insert_or_assign(descNewName, desc);
            registerWithParentLocked(desc, 0);
        }
        for (auto& r : removes) data.erase(r);
        return nullptr;
    }

    error setFileMode(std::string_view name, fs::FileMode mode) {
        string n = normalizePath(name);
        std::lock_guard lock(mu);
        auto it = data.find(n);
        if (it == data.end()) return pathError("chmod", n, ErrFileNotFound);
        mem::SetMode(*it->second, mode);
        return nullptr;
    }

    // The named file, under a read lock; a PathError if it doesn't exist.
    std::pair<ptr<mem::FileData>, error> open(std::string_view name) {
        string n = normalizePath(name);
        std::shared_lock lock(mu);
        auto it = data.find(n);
        if (it == data.end()) return {nullptr, pathError("open", n, ErrFileNotFound)};
        return {it->second, nullptr};
    }

    // The named file, under a read lock, or nil.
    ptr<mem::FileData> lookup(std::string_view name) {
        std::shared_lock lock(mu);
        return openLocked(name);
    }

    static memoryFs& of(MemoryFs& m) { return *m.m_; }

    sync::RWMutex mu;
    std::map<string, ptr<mem::FileData>, std::less<>> data;
};

std::vector<string> memoryNames(MemoryFs& m) {
    auto& s = memoryFs::of(m);
    std::shared_lock lock(s.mu);
    std::vector<string> names;
    for (auto& [name, _] : s.data) names.push_back(name);
    return names;
}

}  // namespace detail

MemoryFs::MemoryFs() : m_(std::make_unique<detail::memoryFs>()) {}
MemoryFs::~MemoryFs() = default;

ptr<Fs> NewMemoryFs() { return make_ptr<MemoryFs>(); }

std::pair<ptr<File>, error> MemoryFs::Create(std::string_view name) {
    string n = detail::normalizePath(name);
    std::lock_guard lock(m_->mu);
    auto file = mem::CreateFile(n);
    m_->data.insert_or_assign(n, file);
    m_->registerWithParentLocked(file, 0);
    return {mem::NewFileHandle(file), nullptr};
}

error MemoryFs::Mkdir(std::string_view name, fs::FileMode perm) {
    perm &= chmodBits;
    string n = detail::normalizePath(name);
    std::lock_guard lock(m_->mu);
    if (m_->data.contains(n)) return detail::pathError("mkdir", n, ErrFileExists);
    auto item = mem::CreateDir(n);
    mem::SetMode(*item, fs::ModeDir | perm);
    m_->data.emplace(n, item);
    m_->registerWithParentLocked(item, perm);
    return nullptr;
}

error MemoryFs::MkdirAll(std::string_view path, fs::FileMode perm) {
    error err = Mkdir(path, perm);
    if (auto* pe = dynamic_cast<const fs::PathError*>(err.get()); pe && pe->Err == ErrFileExists) return nullptr;
    return err;
}

std::pair<ptr<File>, error> MemoryFs::Open(std::string_view name) {
    auto [f, err] = m_->open(name);
    if (err) return {nullptr, err};
    return {mem::NewReadOnlyFileHandle(f), nullptr};
}

std::pair<ptr<File>, error> MemoryFs::OpenFile(std::string_view name, int flag, fs::FileMode perm) {
    perm &= chmodBits;
    bool chmod = false;
    ptr<File> file;
    auto [data, err] = m_->open(name);
    if (!err) {
        if (flag & os::O_EXCL_) return {nullptr, detail::pathError("open", name, ErrFileExists)};
        file = mem::NewFileHandle(data);
    } else if (os::IsNotExist(err) && (flag & os::O_CREATE_)) {
        std::tie(file, err) = Create(name);
        chmod = true;
    }
    if (err) return {nullptr, err};
    if (flag == os::O_RDONLY_) file = mem::NewReadOnlyFileHandle(static_pointer_cast<mem::File>(file)->Data());
    if (flag & os::O_APPEND_) {
        if (auto [_, serr] = file->Seek(0, io::SeekEnd); serr) {
            file->Close();
            return {nullptr, serr};
        }
    }
    if ((flag & os::O_TRUNC_) && (flag & (os::O_RDWR_ | os::O_WRONLY_))) {
        if (error terr = file->Truncate(0)) {
            file->Close();
            return {nullptr, terr};
        }
    }
    if (chmod) return {file, m_->setFileMode(name, perm)};
    return {file, nullptr};
}

error MemoryFs::Remove(std::string_view name) {
    string n = detail::normalizePath(name);
    std::lock_guard lock(m_->mu);
    if (!m_->data.contains(n)) return detail::pathError("remove", n, fs::ErrNotExist);
    if (error err = m_->unRegisterWithParentLocked(n)) return detail::pathError("remove", n, err);
    m_->data.erase(n);
    return nullptr;
}

error MemoryFs::RemoveAll(std::string_view path) {
    string p = detail::normalizePath(path);
    string prefix = p + "/";
    std::lock_guard lock(m_->mu);
    m_->unRegisterWithParentLocked(p);
    std::erase_if(m_->data, [&](const auto& kv) { return kv.first == p || kv.first.starts_with(prefix); });
    return nullptr;
}

error MemoryFs::Rename(std::string_view oldname, std::string_view newname) {
    string o = detail::normalizePath(oldname);
    string n = detail::normalizePath(newname);
    if (o == n) return nullptr;
    std::lock_guard lock(m_->mu);
    auto it = m_->data.find(o);
    if (it == m_->data.end()) return detail::pathError("rename", o, ErrFileNotFound);
    auto fileData = it->second;
    if (error err = m_->unRegisterWithParentLocked(o)) return err;
    mem::ChangeFileName(*fileData, n);
    m_->data.insert_or_assign(n, fileData);
    if (error err = m_->renameDescendantsLocked(o, n)) return err;
    m_->data.erase(o);
    m_->registerWithParentLocked(fileData, 0);
    return nullptr;
}

std::pair<ptr<fs::FileInfo>, error> MemoryFs::Stat(std::string_view name) {
    auto [f, err] = m_->open(name);
    if (err) return {nullptr, err};
    return {mem::GetFileInfo(f), nullptr};
}

std::tuple<ptr<fs::FileInfo>, bool, error> MemoryFs::LstatIfPossible(std::string_view name) {
    auto [fi, err] = Stat(name);
    return {fi, false, err};
}

error MemoryFs::Chmod(std::string_view name, fs::FileMode mode) {
    mode &= chmodBits;
    auto f = m_->lookup(name);
    if (!f) return detail::pathError("chmod", name, ErrFileNotFound);
    fs::FileMode prevOtherBits = mem::GetFileInfo(f)->Mode() & ~chmodBits;
    return m_->setFileMode(name, prevOtherBits | mode);
}

error MemoryFs::Chown(std::string_view name, int uid, int gid) {
    auto f = m_->lookup(name);
    if (!f) return detail::pathError("chown", detail::normalizePath(name), ErrFileNotFound);
    mem::SetUID(*f, uid);
    mem::SetGID(*f, gid);
    return nullptr;
}

error MemoryFs::Chtimes(std::string_view name, time::Time atime, time::Time mtime) {
    auto f = m_->lookup(name);
    if (!f) return detail::pathError("chtimes", detail::normalizePath(name), ErrFileNotFound);
    mem::SetModTime(*f, mtime);
    return nullptr;
}

}  // namespace gopp::vfs
