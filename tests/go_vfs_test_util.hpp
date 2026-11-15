// Helpers of the tests ported from afero (go_vfs_*_test.cpp): its Fss,
// testDir, tmpFile and testRegistry, and setupTestFiles.
#pragma once

#include "vfs/internal.hpp"
#include "vfs/mem.hpp"

#include <gopp/errors.hpp>
#include <gopp/io.hpp>
#include <gopp/os.hpp>
#include <gopp/path/filepath.hpp>
#include <gopp/vfs.hpp>

#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

namespace vfs_test {

using namespace gopp;

inline std::string str(const string& s) { return std::string(s.view()); }
inline std::string str(std::span<const byte> b) { return std::string(asStringView(b)); }

// afero's Fss: a MemoryFs and the OsFs.
inline std::vector<ptr<vfs::Fs>> newFss() { return {vfs::NewMemoryFs(), vfs::NewOsFs()}; }

// afero's testRegistry and removeAllTestFiles: the directories and files a
// test made, removed when it ends.
class registry {
public:
    registry() = default;
    registry(const registry&) = delete;
    ~registry() {
        for (auto& [fs, path] : paths_) {
            if (error err = fs->RemoveAll(path)) ADD_FAILURE() << fs->Name() << ": " << err;
        }
    }

    void add(const ptr<vfs::Fs>& fs, string path) { paths_.emplace_back(fs, std::move(path)); }

    // afero's testDir: a new directory in the temporary directory.
    string testDir(const ptr<vfs::Fs>& fs) {
        auto [name, err] = vfs::TempDir(*fs, "", "afero");
        if (err) panic(("unable to work with test dir" + str(err.Error())).c_str());
        add(fs, name);
        return name;
    }

    // afero's tmpFile: a new file in the temporary directory.
    ptr<vfs::File> tmpFile(const ptr<vfs::Fs>& fs) {
        auto [x, err] = vfs::TempFile(*fs, "", "afero");
        if (err) panic(("unable to work with temp file" + str(err.Error())).c_str());
        add(fs, x->Name());
        return x;
    }

private:
    std::vector<std::pair<ptr<vfs::Fs>, string>> paths_;
};

// afero's setupTestFiles: four files in path/more/subdirectories/for/
// testing/we, which it returns.
inline string setupTestFiles(vfs::Fs& fs, std::string_view path) {
    string testSubDir = filepath::Join(path, "more", "subdirectories", "for", "testing", "we");
    error err = fs.MkdirAll(testSubDir, 0700);
    if (err && !os::IsExist(err)) ADD_FAILURE() << err;
    for (int i = 1; i <= 4; i++) {
        auto n = std::to_string(i);
        auto [f, cerr] = fs.Create(filepath::Join(testSubDir, "testfile" + n));
        if (cerr) {
            ADD_FAILURE() << cerr;
            continue;
        }
        f->WriteString("Testfile " + n + " content");
        f->Close();
    }
    return testSubDir;
}

// afero's setupTestDir: setupTestFiles in a new testDir.
inline string setupTestDir(registry& reg, const ptr<vfs::Fs>& fs) { return setupTestFiles(*fs, reg.testDir(fs)); }

// afero's checkSize
inline void checkSize(vfs::File& f, int64 size) {
    auto [dir, err] = f.Stat();
    ASSERT_FALSE(err) << "Stat " << f.Name() << " (looking for size " << size << "): " << err;
    EXPECT_EQ(dir->Size(), size) << "Stat " << f.Name();
}

}  // namespace vfs_test
