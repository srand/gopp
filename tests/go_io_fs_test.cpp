// Ported from Go 1.27.3's io/fs tests: format_test.go and TestValidPath of
// fs_test.go, with Go's test names.
#include <gopp/io/fs.hpp>

#include <gtest/gtest.h>

#include <limits>

using namespace gopp;

namespace {

// formatTest implements FileInfo to test FormatFileInfo, and implements
// DirEntry to test FormatDirEntry.
struct format_test final : fs::FileInfo, fs::DirEntry {
    format_test(string name, int64 size, fs::FileMode mode, time::Time mod_time, bool is_dir)
        : name_(std::move(name)), size_(size), mode_(mode), mod_time_(mod_time), is_dir_(is_dir) {}

    string Name() const override { return name_; }
    int64 Size() const override { return size_; }
    fs::FileMode Mode() const override { return mode_; }
    time::Time ModTime() const override { return mod_time_; }
    bool IsDir() const override { return is_dir_; }
    fs::FileMode Type() const override { return mode_.Type(); }
    std::pair<ptr<fs::FileInfo>, error> Info() const override { return {nullptr, nullptr}; }

    string name_;
    int64 size_;
    fs::FileMode mode_;
    time::Time mod_time_;
    bool is_dir_;
};

struct format_case {
    format_test input;
    std::string want_file_info;
    std::string want_dir_entry;
};

std::vector<format_case> format_tests() {
    time::Time noon = time::Time::Unix(12 * 3600, 0);  // 1970-01-01 12:00:00 UTC
    return {
        {{"hello.go", 100, 0644, noon, false}, "-rw-r--r-- 100 1970-01-01 12:00:00 hello.go", "- hello.go"},
        {{"home/gopher", 0, fs::ModeDir | 0755, noon, true},
         "drwxr-xr-x 0 1970-01-01 12:00:00 home/gopher/",
         "d home/gopher/"},
        {{"big", std::numeric_limits<int64>::max(), fs::ModeIrregular | 0644, noon, false},
         "?rw-r--r-- 9223372036854775807 1970-01-01 12:00:00 big",
         "? big"},
        {{"small", std::numeric_limits<int64>::min(), fs::ModeSocket | fs::ModeSetuid | 0644, noon, false},
         "Surw-r--r-- -9223372036854775808 1970-01-01 12:00:00 small",
         "S small"},
    };
}

}  // namespace

TEST(GoIoFs, FormatFileInfo) {
    int i = 0;
    for (auto& test : format_tests()) {
        EXPECT_EQ(fs::FormatFileInfo(test.input), test.want_file_info) << i;
        i++;
    }
}

TEST(GoIoFs, FormatDirEntry) {
    int i = 0;
    for (auto& test : format_tests()) {
        EXPECT_EQ(fs::FormatDirEntry(test.input), test.want_dir_entry) << i;
        i++;
    }
}

TEST(GoIoFs, ValidPath) {
    struct test {
        std::string name;
        bool ok;
    } tests[] = {
        {".", true},       {"x", true},       {"x/y", true},

        {"", false},       {"..", false},     {"/", false},       {"x/", false},     {"/x", false},
        {"x/y/", false},   {"/x/y", false},   {"./", false},      {"./x", false},    {"x/.", false},
        {"x/./y", false},  {"../", false},    {"../x", false},    {"x/..", false},   {"x/../y", false},
        {"x//y", false},   {"x\\", true},     {"x\\y", true},     {"x:y", true},     {"\\x", true},
    };
    for (auto& tt : tests) EXPECT_EQ(fs::ValidPath(tt.name), tt.ok) << "ValidPath(" << tt.name << ")";
}

TEST(GoIoFs, FileModeString) {
    // Not a Go test: FileMode.String of the examples in io/fs's docs.
    EXPECT_EQ(fs::FileMode(0644).String(), "-rw-r--r--");
    EXPECT_EQ((fs::ModeDir | 0755).String(), "drwxr-xr-x");
    EXPECT_EQ(fs::FileMode(fs::ModeSymlink | 0777).String(), "Lrwxrwxrwx");
    EXPECT_EQ(std::format("{}", fs::FileMode(0600)), "-rw-------");
}
