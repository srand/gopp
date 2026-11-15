// vfs::HttpFs: an Fs as an http::FileSystem, for http::FileServer. Ported
// from afero v1.15.0's httpFs.go; a header of its own, so that <gopp/vfs.hpp>
// doesn't bring in net/http.
//
//     auto httpFs = vfs::NewHttpFs(vfs::NewMemoryFs());
//     http::Handle("/", http::FileServer(httpFs->Dir("/srv/www")));
#pragma once

// The module interfaces (modules/) import these instead.
#ifndef GOPP_MODULE
#include <gopp/errors.hpp>
#include <gopp/io/fs.hpp>
#include <gopp/net/http.hpp>
#include <gopp/ptr.hpp>
#include <gopp/string.hpp>
#include <gopp/time.hpp>
#include <gopp/vfs.hpp>

#include <string_view>
#include <utility>
#endif

namespace gopp::vfs {

// vfs.HttpFs: an http::FileSystem whose files are those of an Fs. The
// other methods of Fs pass through to it.
class HttpFs final : public http::FileSystem {
public:
    explicit HttpFs(ptr<Fs> source) : source_(std::move(source)) {}

    // The FileSystem of the tree at the directory s ("." if empty), which
    // opens request paths (cleaned with path::Clean) in it.
    ptr<http::FileSystem> Dir(std::string_view s) const;

    // Opens the named file of the Fs for reading.
    std::pair<ptr<http::File>, error> Open(std::string_view name) override;

    string Name() const { return string::fromStatic("h HttpFs"); }
    std::pair<ptr<File>, error> Create(std::string_view name) { return source_->Create(name); }
    error Chmod(std::string_view name, fs::FileMode mode) { return source_->Chmod(name, mode); }
    error Chown(std::string_view name, int uid, int gid) { return source_->Chown(name, uid, gid); }
    error Chtimes(std::string_view name, time::Time atime, time::Time mtime) {
        return source_->Chtimes(name, atime, mtime);
    }
    error Mkdir(std::string_view name, fs::FileMode perm) { return source_->Mkdir(name, perm); }
    error MkdirAll(std::string_view path, fs::FileMode perm) { return source_->MkdirAll(path, perm); }
    std::pair<ptr<File>, error> OpenFile(std::string_view name, int flag, fs::FileMode perm) {
        return source_->OpenFile(name, flag, perm);
    }
    error Remove(std::string_view name) { return source_->Remove(name); }
    error RemoveAll(std::string_view path) { return source_->RemoveAll(path); }
    error Rename(std::string_view oldname, std::string_view newname) { return source_->Rename(oldname, newname); }
    std::pair<ptr<fs::FileInfo>, error> Stat(std::string_view name) { return source_->Stat(name); }

private:
    ptr<Fs> source_;
};

// vfs.NewHttpFs
ptr<HttpFs> NewHttpFs(ptr<Fs> source);

}  // namespace gopp::vfs
