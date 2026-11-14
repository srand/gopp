// The environment, after Go's os/env.go and syscall/env_unix.go, and the
// user's directories, after os/file.go.
//
// Go keeps a copy of the environment; go++ uses the C library's, so that C
// code sees the changes too. A lock orders the calls of this package, as
// getenv(3) isn't safe while another thread calls setenv(3).
#include <gopp/errors.hpp>
#include <gopp/os.hpp>
#include <gopp/sync.hpp>
#include <gopp/syscall.hpp>

#include <cstdlib>
#include <string>
#include <unistd.h>

namespace gopp::os {

namespace {

sync::RWMutex env_lock;

}  // namespace

std::pair<string, bool> LookupEnv(std::string_view key) {
    if (key.empty() || key.find('\0') != std::string_view::npos) return {string(), false};
    std::string k(key);
    env_lock.RLock();
    const char* v = std::getenv(k.c_str());
    std::pair<string, bool> r = v ? std::pair<string, bool>{string(v), true} : std::pair<string, bool>{string(), false};
    env_lock.RUnlock();
    return r;
}

string Getenv(std::string_view key) { return LookupEnv(key).first; }

error Setenv(std::string_view key, std::string_view value) {
    if (key.empty() || key.find_first_of(std::string_view("=\0", 2)) != std::string_view::npos ||
        value.find('\0') != std::string_view::npos) {
        return NewSyscallError("setenv", syscall::EINVAL_);
    }
    std::string k(key), v(value);
    env_lock.Lock();
    int r = ::setenv(k.c_str(), v.c_str(), 1);
    int e = errno;
    env_lock.Unlock();
    if (r < 0) return NewSyscallError("setenv", syscall::errnoError(e));
    return nullptr;
}

error Unsetenv(std::string_view key) {
    // Go's Unsetenv of a key that can't be set does nothing.
    if (key.empty() || key.find_first_of(std::string_view("=\0", 2)) != std::string_view::npos) return nullptr;
    std::string k(key);
    env_lock.Lock();
    ::unsetenv(k.c_str());
    env_lock.Unlock();
    return nullptr;
}

void Clearenv() {
    env_lock.Lock();
    ::clearenv();
    env_lock.Unlock();
}

slice<string> Environ() {
    env_lock.RLock();
    std::size_t n = 0;
    for (char** e = environ; e && *e; e++) n++;
    slice<string> a = slice<string>::make(0, n);
    for (char** e = environ; e && *e; e++) a = append(a, string(*e));
    env_lock.RUnlock();
    return a;
}

namespace {

bool isShellSpecialVar(char c) {
    switch (c) {
    case '*': case '#': case '$': case '@': case '!': case '?': case '-':
    case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7': case '8': case '9':
        return true;
    }
    return false;
}

bool isAlphaNum(char c) {
    return c == '_' || ('0' <= c && c <= '9') || ('a' <= c && c <= 'z') || ('A' <= c && c <= 'Z');
}

// Go's getShellName: the name of the variable that s starts with (after
// the $), and how many bytes it takes. A bad ${...} gives "" and the bytes
// to eat.
std::pair<std::string_view, std::size_t> get_shell_name(std::string_view s) {
    if (s[0] == '{') {
        if (s.size() > 2 && isShellSpecialVar(s[1]) && s[2] == '}') return {s.substr(1, 1), 3};
        for (std::size_t i = 1; i < s.size(); i++) {
            if (s[i] == '}') {
                if (i == 1) return {{}, 2};  // bad syntax; eat "${}"
                return {s.substr(1, i - 1), i + 1};
            }
        }
        return {{}, 1};  // bad syntax; eat "${"
    }
    if (isShellSpecialVar(s[0])) return {s.substr(0, 1), 1};
    std::size_t i = 0;
    while (i < s.size() && isAlphaNum(s[i])) i++;
    return {s.substr(0, i), i};
}

}  // namespace

string Expand(std::string_view s, const std::function<string(std::string_view)>& mapping) {
    std::string buf;
    bool any = false;
    std::size_t i = 0;
    for (std::size_t j = 0; j < s.size(); j++) {
        if (s[j] == '$' && j + 1 < s.size()) {
            if (!any) {
                buf.reserve(2 * s.size());
                any = true;
            }
            buf.append(s.substr(i, j - i));
            auto [name, w] = get_shell_name(s.substr(j + 1));
            if (name.empty() && w > 0) {
                // Bad syntax: eat the characters.
            } else if (name.empty()) {
                // A $ followed by something that isn't a name.
                buf.push_back(s[j]);
            } else {
                buf.append(mapping(name).view());
            }
            j += w;
            i = j + 1;
        }
    }
    if (!any) return string(s);
    buf.append(s.substr(i));
    return string(buf);
}

string ExpandEnv(std::string_view s) {
    return Expand(s, [](std::string_view k) { return Getenv(k); });
}

std::pair<string, error> UserHomeDir() {
    if (string v = Getenv("HOME"); !v.empty()) return {v, nullptr};
    return {string(), errors::New("$HOME is not defined")};
}

std::pair<string, error> UserCacheDir() {
    string dir = Getenv("XDG_CACHE_HOME");
    if (dir.empty()) {
        dir = Getenv("HOME");
        if (dir.empty()) return {string(), errors::New("neither $XDG_CACHE_HOME nor $HOME are defined")};
        return {dir + "/.cache", nullptr};
    }
    if (dir[0] != '/') return {string(), errors::New("path in $XDG_CACHE_HOME is relative")};
    return {dir, nullptr};
}

std::pair<string, error> UserConfigDir() {
    string dir = Getenv("XDG_CONFIG_HOME");
    if (dir.empty()) {
        dir = Getenv("HOME");
        if (dir.empty()) return {string(), errors::New("neither $XDG_CONFIG_HOME nor $HOME are defined")};
        return {dir + "/.config", nullptr};
    }
    if (dir[0] != '/') return {string(), errors::New("path in $XDG_CONFIG_HOME is relative")};
    return {dir, nullptr};
}

}  // namespace gopp::os
