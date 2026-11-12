// Getwd, after Go's os/getwd.go.
#include "internal.hpp"

#include <unistd.h>

#include <cerrno>
#include <climits>
#include <string>

namespace gopp::os {

namespace {

sync::Mutex getwd_mu;
std::string getwd_cache;  // under getwd_mu

}  // namespace

std::pair<string, error> Getwd() {
    // Clumsy but widespread kludge: if $PWD is set and is the same
    // directory as ".", use it.
    ptr<FileInfo> dot;
    string pwd = Getenv("PWD");
    if (!pwd.empty() && pwd.view()[0] == '/') {
        auto [d, err] = Stat(".");
        if (err) return {{}, err};
        dot = d;
        auto [p, perr] = Stat(pwd.view());
        if (!perr && SameFile(dot, p)) return {pwd, nullptr};
    }

    // The getcwd system call.
    {
        std::string buf(PATH_MAX, '\0');
        int e = detail::sys([&] { return ::getcwd(buf.data(), buf.size()) ? 0 : -1; });
        if (e != ENAMETOOLONG && e != EINVAL && e != ERANGE && e != ENOMEM) {
            if (e) return {{}, NewSyscallError("getwd", detail::errno_error(e))};
            buf.resize(buf.find('\0'));
            return {string(buf), nullptr};
        }
    }

    // A path too long for getcwd: the cache from an earlier walk, or walk
    // up from "." to "/".
    if (!dot) {
        auto [d, err] = Stat(".");
        if (err) return {{}, err};
        dot = d;
    }
    {
        std::lock_guard l(getwd_mu);
        if (!getwd_cache.empty()) {
            auto [d, err] = Stat(getwd_cache);
            if (!err && SameFile(dot, d)) return {string(getwd_cache), nullptr};
        }
    }
    auto [root, rerr] = Stat("/");
    if (rerr) return {{}, rerr};
    if (SameFile(root, dot)) return {string::from_static("/"), nullptr};

    // General algorithm: find the name in the parent and then find the
    // name of the parent. Each iteration adds /name to the beginning of
    // dir.
    std::string dir;
    for (std::string parent = "..";; parent = "../" + parent) {
        if (parent.size() >= 1024) return {{}, NewSyscallError("getwd", detail::errno_error(ENAMETOOLONG))};
        auto [fd, err] = Open(parent);
        if (err) return {{}, err};
        bool found = false;
        while (!found) {
            auto [names, nerr] = fd->Readdirnames(100);
            if (nerr) {
                fd->Close();
                return {{}, NewSyscallError("getwd", detail::errno_error(ENAMETOOLONG))};
            }
            for (auto& name : names) {
                auto [d, lerr] = Lstat(parent + "/" + std::string(name.view()));
                if (!lerr && SameFile(d, dot)) {
                    dir = "/" + std::string(name.view()) + dir;
                    found = true;
                    break;
                }
            }
        }
        auto [pd, serr] = fd->Stat();
        fd->Close();
        if (serr) return {{}, serr};
        if (SameFile(pd, root)) break;
        // Set up for next round.
        dot = pd;
    }

    std::lock_guard l(getwd_mu);
    getwd_cache = dir;
    return {string(dir), nullptr};
}

}  // namespace gopp::os
