// HttpFs, ported from afero v1.15.0's httpFs.go.
#include "internal.hpp"

#include <gopp/path.hpp>
#include <gopp/vfs/http.hpp>

namespace gopp::vfs {

namespace {

const error errInvalidCharacter = errors::New("http: invalid character in file path");

// A vfs::File as an http::File (in Go, every afero.File is one).
class httpFile final : public http::File {
public:
    explicit httpFile(ptr<vfs::File> f) : f_(std::move(f)) {}

    std::pair<isize, error> Read(std::span<byte> p) override { return f_->Read(p); }
    std::pair<int64, error> Seek(int64 offset, int whence) override { return f_->Seek(offset, whence); }
    error Close() override { return f_->Close(); }
    std::pair<std::vector<ptr<fs::FileInfo>>, error> Readdir(int count) override { return f_->Readdir(count); }
    std::pair<ptr<fs::FileInfo>, error> Stat() override { return f_->Stat(); }

private:
    ptr<vfs::File> f_;
};

class httpDir final : public http::FileSystem {
public:
    httpDir(string basePath, ptr<HttpFs> fs) : basePath_(std::move(basePath)), fs_(std::move(fs)) {}

    std::pair<ptr<http::File>, error> Open(std::string_view name) override {
        if (name.find('\0') != name.npos) return {nullptr, errInvalidCharacter};
        std::string_view dir = basePath_;
        if (dir.empty()) dir = ".";
        return fs_->Open(filepath::Join(dir, filepath::FromSlash(path::Clean("/"_s + name))));
    }

private:
    string basePath_;
    ptr<HttpFs> fs_;
};

}  // namespace

ptr<HttpFs> NewHttpFs(ptr<Fs> source) { return make_ptr<HttpFs>(std::move(source)); }

ptr<http::FileSystem> HttpFs::Dir(std::string_view s) const {
    return make_ptr<httpDir>(string(s), make_ptr<HttpFs>(source_));
}

std::pair<ptr<http::File>, error> HttpFs::Open(std::string_view name) {
    auto [f, err] = source_->Open(name);
    if (err) return {nullptr, err};
    return {make_ptr<httpFile>(f), nullptr};
}

}  // namespace gopp::vfs
