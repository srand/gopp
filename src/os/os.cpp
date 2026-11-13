// Arguments, exit and the process's ids, after Go's os/proc.go,
// sys_linux.go and executable_procfs.go.
#include <gopp/os.hpp>
#include <gopp/strings.hpp>
#include <gopp/syscall.hpp>

#include <sys/utsname.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace gopp::os {

namespace {

std::vector<string>& argv_storage() {
    static std::vector<string>* v = new std::vector<string>();
    return *v;
}

}  // namespace

const std::vector<string>& Args() { return argv_storage(); }

void SetArgs(int argc, char** argv) {
    auto& v = argv_storage();
    v.clear();
    for (int i = 0; i < argc; i++) v.emplace_back(argv[i]);
}

void Exit(int code) {
    std::fflush(nullptr);
    std::_Exit(code);
}

int Getpid() noexcept { return syscall::Getpid(); }
int Getppid() noexcept { return syscall::Getppid(); }
int Getuid() noexcept { return syscall::Getuid(); }
int Geteuid() noexcept { return syscall::Geteuid(); }
int Getgid() noexcept { return syscall::Getgid(); }
int Getegid() noexcept { return syscall::Getegid(); }

std::pair<std::vector<int>, error> Getgroups() {
    for (;;) {
        int n = ::getgroups(0, nullptr);
        if (n < 0) return {{}, NewSyscallError("getgroups", syscall::errno_error(errno))};
        std::vector<gid_t> g(static_cast<std::size_t>(n));
        int m = ::getgroups(n, g.data());
        if (m < 0) {
            if (errno == EINVAL) continue;  // the groups changed meanwhile
            return {{}, NewSyscallError("getgroups", syscall::errno_error(errno))};
        }
        return {std::vector<int>(g.begin(), g.begin() + m), nullptr};
    }
}

std::pair<string, error> Hostname() {
    // uname first: one system call. A name of 64 bytes may be cut short.
    struct utsname un;
    if (::uname(&un) == 0) {
        std::string_view name(un.nodename);
        if (!name.empty() && name.size() < 64) return {string(name), nullptr};
    }
    auto [b, err] = ReadFile("/proc/sys/kernel/hostname");
    if (err) return {string(), err};
    std::string_view s = as_string_view(b);
    if (!s.empty() && s.back() == '\n') s.remove_suffix(1);
    return {string(s), nullptr};
}

std::pair<string, error> Executable() {
    auto [p, err] = Readlink("/proc/self/exe");
    // The path of an executable that has been deleted ends in " (deleted)".
    return {strings::TrimSuffix(p, " (deleted)"), err};
}

}  // namespace gopp::os
