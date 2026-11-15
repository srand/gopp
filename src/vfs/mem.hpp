// The files of a MemoryFs: afero's mem package, ported. Internal: the
// tests use it.
#pragma once

#include <gopp/io/fs.hpp>
#include <gopp/sync.hpp>
#include <gopp/time.hpp>
#include <gopp/vfs.hpp>

#include <atomic>
#include <map>
#include <memory>
#include <vector>

namespace gopp::vfs::mem {

// mem.FileData: a file or directory, which its handles share. Its mutex
// guards its fields.
struct FileData final : object {
    explicit FileData(string n) : name(std::move(n)) {}

    string Name();

    sync::Mutex mu;
    string name;  // the full, cleaned name
    std::vector<byte> data;
    // A directory's entries, by their full names (mem.DirMap).
    std::unique_ptr<std::map<string, ptr<FileData>>> memDir;
    bool dir = false;
    fs::FileMode mode;
    time::Time modtime;
    int uid = 0;
    int gid = 0;
};

ptr<FileData> CreateFile(std::string_view name);
ptr<FileData> CreateDir(std::string_view name);
void ChangeFileName(FileData& f, std::string_view newname);
void SetMode(FileData& f, fs::FileMode mode);
void SetModTime(FileData& f, time::Time mtime);
void SetUID(FileData& f, int uid);
void SetGID(FileData& f, int gid);

// mem.Dir's functions. The caller locks dir.
void InitializeDir(FileData& d);
void AddToMemDir(FileData& dir, const ptr<FileData>& f);
void RemoveFromMemDir(FileData& dir, FileData& f);
// The entries of a directory, sorted by name; the caller locks d.
std::vector<ptr<FileData>> Files(FileData& d);

// mem.FileInfo
class FileInfo final : public fs::FileInfo {
public:
    explicit FileInfo(ptr<FileData> d) : d_(std::move(d)) {}
    string Name() const override;
    // 42 for a directory.
    int64 Size() const override;
    fs::FileMode Mode() const override;
    time::Time ModTime() const override;
    bool IsDir() const override;

private:
    ptr<FileData> d_;
};

ptr<fs::FileInfo> GetFileInfo(const ptr<FileData>& f);

// mem.File: an open handle of a FileData, with its own offset.
class File final : public vfs::File {
public:
    File(ptr<FileData> d, bool readOnly) : fileData_(std::move(d)), readOnly_(readOnly) {}

    const ptr<FileData>& Data() const noexcept { return fileData_; }

    // Reopens a closed handle, at offset 0.
    error Open();
    error Close() override;
    string Name() const override { return fileData_->Name(); }
    std::pair<ptr<fs::FileInfo>, error> Stat() override { return {GetFileInfo(fileData_), nullptr}; }
    error Sync() override { return nullptr; }
    std::pair<std::vector<ptr<fs::FileInfo>>, error> Readdir(int count) override;
    std::pair<std::vector<string>, error> Readdirnames(int n) override;
    std::pair<isize, error> Read(std::span<byte> p) override;
    std::pair<isize, error> ReadAt(std::span<byte> p, int64 off) override;
    error Truncate(int64 size) override;
    std::pair<int64, error> Seek(int64 offset, int whence) override;
    std::pair<isize, error> Write(std::span<const byte> p) override;
    std::pair<isize, error> WriteAt(std::span<const byte> p, int64 off) override;

private:
    // Reads at off, without the offset; the caller locks.
    std::pair<isize, error> readAt(std::span<byte> p, int64 off);
    // Writes at off; the caller locks.
    void writeAt(std::span<const byte> p, int64 off);
    error notWritable(std::string_view op);

    ptr<FileData> fileData_;
    bool readOnly_;
    std::atomic<int64> at_{0};
    int64 readDirCount_ = 0;
    bool closed_ = false;
};

ptr<File> NewFileHandle(ptr<FileData> data);
ptr<File> NewReadOnlyFileHandle(ptr<FileData> data);

}  // namespace gopp::vfs::mem

namespace gopp::vfs::detail {

// A MemoryFs's name for path: cleaned, and "/" for ".", ".." and "/".
string normalizePath(std::string_view path);

// The names of the files and directories a MemoryFs holds (Go's
// getData()'s keys), for the tests.
std::vector<string> memoryNames(MemoryFs& m);

}  // namespace gopp::vfs::detail
