// The vfs package's internals, shared by its files.
#pragma once

#include <gopp/errors.hpp>
#include <gopp/io.hpp>
#include <gopp/io/fs.hpp>
#include <gopp/os.hpp>
#include <gopp/path/filepath.hpp>
#include <gopp/syscall.hpp>
#include <gopp/vfs.hpp>

#include <algorithm>
#include <string_view>
#include <utility>
#include <vector>

namespace gopp::vfs::detail {

inline ptr<fs::PathError> pathError(std::string_view op, std::string_view path, error err) {
    return make_ptr<fs::PathError>(string(op), string(path), std::move(err));
}

inline ptr<os::LinkError> linkError(std::string_view op, std::string_view oldname, std::string_view newname, error err) {
    return make_ptr<os::LinkError>(string(op), string(oldname), string(newname), std::move(err));
}

// The flags that make OpenFile write.
inline constexpr int writeFlags = os::O_WRONLY_ | os::O_RDWR_ | os::O_APPEND_ | os::O_CREATE_ | os::O_TRUNC_;

// Sorts FileInfos by name (Go's byName).
inline void sortByName(std::vector<ptr<fs::FileInfo>>& infos) {
    std::sort(infos.begin(), infos.end(),
              [](const ptr<fs::FileInfo>& a, const ptr<fs::FileInfo>& b) { return a->Name() < b->Name(); });
}

// Whether any of subslices is in what r reads (Go's readerContainsAny).
bool readerContainsAny(io::Reader& r, std::span<const std::span<const byte>> subslices);

// Go's lstatIfPossible: Lstat if fsys is a Lstater, else Stat.
std::pair<ptr<fs::FileInfo>, error> lstatIfPossible(Fs& fsys, std::string_view path);

// Copies the named file of base to layer (Go's copyToLayer), opening it
// with Open, or with OpenFile, flag and perm (copyFileToLayer).
error copyToLayer(Fs& base, Fs& layer, std::string_view name);
error copyFileToLayer(Fs& base, Fs& layer, std::string_view name, int flag, fs::FileMode perm);

// A File that forwards to another: the base of the files that wrap
// another's, overriding some methods.
class fileWrapper : public File {
public:
    explicit fileWrapper(ptr<File> f) : f_(std::move(f)) {}

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

protected:
    ptr<File> f_;
};

}  // namespace gopp::vfs::detail
