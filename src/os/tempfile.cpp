// CreateTemp and MkdirTemp, after Go's os/tempfile.go.
#include "internal.hpp"

#include "../runtime/runtime.hpp"

#include <string>

namespace gopp::os {

namespace detail {

const error errPatternHasSeparator = errors::New("pattern contains path separator");

}  // namespace detail

namespace {

std::string nextRandom() { return std::to_string(runtime::fastrand()); }

// Go's prefixAndSuffix: splits pattern at its last "*".
error prefix_and_suffix(std::string_view pattern, std::string_view& prefix, std::string_view& suffix) {
    for (char c : pattern) {
        if (IsPathSeparator(c)) return detail::errPatternHasSeparator;
    }
    if (auto pos = pattern.rfind('*'); pos != std::string_view::npos) {
        prefix = pattern.substr(0, pos);
        suffix = pattern.substr(pos + 1);
    } else {
        prefix = pattern;
        suffix = {};
    }
    return nullptr;
}

}  // namespace

std::pair<ptr<File>, error> CreateTemp(std::string_view dir, std::string_view pattern) {
    string tmp;
    if (dir.empty()) {
        tmp = TempDir();
        dir = tmp.view();
    }
    std::string_view pre, suf;
    if (error err = prefix_and_suffix(pattern, pre, suf)) return {nullptr, detail::path_error("createtemp", pattern, err)};
    std::string prefix = detail::join_path(dir, pre);
    for (int attempt = 0;;) {
        std::string name = prefix + nextRandom() + std::string(suf);
        auto [f, err] = OpenFile(name, O_RDWR_ | O_CREATE_ | O_EXCL_, 0600);
        if (IsExist(err)) {
            if (++attempt < 10000) continue;
            return {nullptr, detail::path_error("createtemp", prefix + "*" + std::string(suf), ErrExist)};
        }
        return {f, err};
    }
}

std::pair<string, error> MkdirTemp(std::string_view dir, std::string_view pattern) {
    string tmp;
    if (dir.empty()) {
        tmp = TempDir();
        dir = tmp.view();
    }
    std::string_view pre, suf;
    if (error err = prefix_and_suffix(pattern, pre, suf)) return {{}, detail::path_error("mkdirtemp", pattern, err)};
    std::string prefix = detail::join_path(dir, pre);
    for (int attempt = 0;;) {
        std::string name = prefix + nextRandom() + std::string(suf);
        error err = Mkdir(name, 0700);
        if (!err) return {string(name), nullptr};
        if (IsExist(err)) {
            if (++attempt < 10000) continue;
            return {{}, detail::path_error("mkdirtemp", prefix + "*" + std::string(suf), ErrExist)};
        }
        if (IsNotExist(err)) {
            if (auto [fi, serr] = Stat(dir); IsNotExist(serr)) return {{}, serr};
        }
        return {{}, err};
    }
}

}  // namespace gopp::os
