// Helpers of the tests ported from Go's os package (go_os_*_test.cpp):
// Go's t.TempDir, t.Chdir and newFile, and the like.
#pragma once

#include "os/internal.hpp"

#include <gopp/chan.hpp>
#include <gopp/errors.hpp>
#include <gopp/io.hpp>
#include <gopp/os.hpp>
#include <gopp/runtime.hpp>
#include <gopp/strings.hpp>
#include <gopp/sync.hpp>
#include <gopp/syscall.hpp>
#include <gopp/time.hpp>

#include <gtest/gtest.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <string>
#include <thread>
#include <vector>

#include <functional>

namespace os_test {

using namespace gopp;
using os::File;
using os::FileInfo;
using os::FileMode;

inline std::string str(const string& s) { return std::string(s.view()); }

inline std::string test_name() {
    auto* t = testing::UnitTest::GetInstance()->current_test_info();
    return std::string(t->name());
}

// Go's filepath.Join for the simple paths of these tests.
inline std::string join(std::string_view a, std::string_view b) { return os::detail::join_path(a, b); }

// t.TempDir(): a new directory, removed at the end of the test.
class TempDir {
public:
    TempDir() {
        auto [d, err] = os::MkdirTemp("", "gopp_os_" + test_name());
        if (err) ADD_FAILURE() << "MkdirTemp: " << err;
        path_ = str(d);
    }
    ~TempDir() {
        if (error err = os::RemoveAll(path_)) ADD_FAILURE() << "TempDir RemoveAll cleanup: " << err;
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    const std::string& path() const noexcept { return path_; }
    operator std::string_view() const noexcept { return path_; }
    operator const std::string&() const noexcept { return path_; }

private:
    std::string path_;
};

// t.Chdir(dir): changes the working directory until the end of the test.
class Chdir {
public:
    explicit Chdir(std::string_view dir) {
        auto [wd, err] = os::Getwd();
        if (err) ADD_FAILURE() << "Getwd: " << err;
        old_ = str(wd);
        if (error err = os::Chdir(dir)) ADD_FAILURE() << "Chdir: " << err;
    }
    ~Chdir() {
        if (error err = os::Chdir(old_)) ADD_FAILURE() << "Chdir back: " << err;
    }

private:
    std::string old_;
};

// A test's own temporary directory to work in: t.Chdir(t.TempDir()).
struct InTempDir {
    TempDir dir;
    Chdir cd{dir.path()};
};

// Go's newFile: a temporary file, closed and removed at the end of the test.
class NewFile {
public:
    NewFile() {
        auto [f, err] = os::CreateTemp("", "_Go_" + test_name());
        if (err) ADD_FAILURE() << err;
        f_ = f;
    }
    ~NewFile() {
        if (error err = f_->Close(); err && !errors::Is(err, os::ErrClosed)) ADD_FAILURE() << err;
        if (error err = os::Remove(f_->Name().view())) ADD_FAILURE() << err;
    }
    File* operator->() const noexcept { return f_.get(); }
    File& operator*() const noexcept { return *f_; }
    const ptr<File>& get() const noexcept { return f_; }

private:
    ptr<File> f_;
};

inline std::span<const byte> b(std::string_view s) { return as_bytes(s); }

// Reads all of f from its current offset.
inline std::string read_all(File& f) {
    auto [data, err] = io::ReadAll(f);
    EXPECT_FALSE(err) << err;
    return std::string(asStringView(data));
}

inline std::string read_file(std::string_view name) {
    auto [data, err] = os::ReadFile(name);
    EXPECT_FALSE(err) << err;
    return std::string(asStringView(data));
}

inline const syscall::Errno* errnoOf(const error& err) { return errors::As<syscall::Errno>(err); }

inline bool isErrno(const error& err, int code) {
    auto* e = errnoOf(err);
    return e && e->Code == code;
}

// The files of Go's "dot" test directory.
inline const std::vector<std::string> dot = {
    "dir_unix.go", "env.go", "error.go", "file.go", "os_test.go", "types.go", "stat_darwin.go", "stat_linux.go",
};

inline const std::string sfdir = "/etc";
inline const std::vector<std::string> sysdir_files = {"group", "hosts", "passwd"};
inline const std::string sfname = "group";

inline int64 fileSize(std::string_view name) {
    auto [file, err] = os::Open(name);
    if (err) {
        ADD_FAILURE() << "open failed: " << err;
        return -1;
    }
    auto [n, cerr] = io::Copy(io::Discard(), *file);
    EXPECT_FALSE(cerr) << cerr;
    EXPECT_FALSE(file->Close());
    return n;
}

inline void touch(std::string_view name) {
    auto [f, err] = os::Create(name);
    ASSERT_FALSE(err) << err;
    ASSERT_FALSE(f->Close());
}

}  // namespace os_test
