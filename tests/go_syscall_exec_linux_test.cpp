// Ported from Go 1.27.3's syscall/exec_linux_test.go, with Go's test names.
//
// Go's tests that run the test binary again select the child's part with
// -test.run and GO_WANT_HELPER_PROCESS; here the child is a helper command
// of go_helper.hpp. Adapted: user "nobody" is looked up in /etc/passwd
// (go++ has no os/user); TestUnshareMountNameSpaceChroot, where Go builds a
// static test binary to run in the chroot, copies this one and the shared
// libraries it has loaded into the chroot instead.
#include "go_helper.hpp"
#include "go_os_test_util.hpp"
#include "syscall/exec_linux.hpp"

#include <gopp/fmt.hpp>
#include <gopp/os/exec.hpp>
#include <gopp/path.hpp>

#include <elf.h>
#include <sys/syscall.h>

#include <fstream>

using namespace os_test;
using go_helper::helper_command;

namespace {

constexpr const char* mountNotSupported = "mount is not supported: ";  // Output prefix indicating a test skip.

constexpr uintptr CAP_SYS_TIME = 25;
constexpr uintptr CAP_SYSLOG = 34;

struct capHeader {
    uint32_t version;
    int32_t pid;
};
struct capData {
    uint32_t effective;
    uint32_t permitted;
    uint32_t inheritable;
};
struct caps {
    capHeader hdr;
    capData data[2];
};

std::pair<caps, error> getCaps() {
    caps c{};
    // Get capability version
    if (::syscall(SYS_capget, &c.hdr, nullptr) < 0) return {c, fmt::Errorf("SYS_CAPGET: {}", syscall::errnoError(errno))};
    // Get current capabilities
    if (::syscall(SYS_capget, &c.hdr, &c.data[0]) < 0) {
        return {c, fmt::Errorf("SYS_CAPGET: {}", syscall::errnoError(errno))};
    }
    return {c, nullptr};
}

// Go's testenv.SyscallIsNotSupported.
bool syscallIsNotSupported(const error& err) {
    if (!err) return false;
    if (auto* e = errors::As<syscall::Errno>(err)) {
        switch (e->Code) {
        case EPERM:
        case EROFS:
            // User lacks permission: either the call requires root
            // permission and the user is not root, or the call is denied
            // by a container security policy.
            return true;
        case EINVAL:
            // Some containers return EINVAL instead of EPERM if a system
            // call is denied by security policy.
            return true;
        case ENOSYS:
        case ENOTSUP:
            return true;
        }
    }
    return errors::Is(err, fs::ErrPermission);
}

[[noreturn]] void exitWith(int code, std::string_view msg) {
    fmt::Print(os::Stderr, "{}\n", msg);
    os::Exit(code);
}

const bool helpers_registered = [] {
    // TestUnshareMountNameSpace's child: mount proc on its argument.
    go_helper::registerHelper("mountproc", [](const std::vector<std::string>& args) {
        if (error err = syscall::Mount("none", args.at(0), "proc", 0, "")) {
            if (syscallIsNotSupported(err)) {
                fmt::Print("{}{}", mountNotSupported, err);
            } else {
                exitWith(2, std::format("unshare: mount {}: {}", args.at(0), err));
            }
        }
        os::Exit(0);
    });
    // TestUnshareUidGidMapping's child: chroot, as root of its namespace.
    go_helper::registerHelper("chroottmp", [](const std::vector<std::string>&) {
        if (error err = syscall::Chroot(str(os::TempDir()))) exitWith(2, str(err.Error()));
        os::Exit(0);
    });
    // TestUseCgroupFD's child: read and print own cgroup path.
    go_helper::registerHelper("selfcgroup", [](const std::vector<std::string>&) {
        auto [selfCg, err] = os::ReadFile("/proc/self/cgroup");
        if (err) exitWith(2, str(err.Error()));
        fmt::Print("{}", asStringView(selfCg));
        os::Exit(0);
    });
    // TestCloneTimeNamespace's child: print its time namespace.
    go_helper::registerHelper("timens", [](const std::vector<std::string>&) {
        auto [timens, err] = os::Readlink("/proc/self/ns/time");
        if (err) exitWith(2, str(err.Error()));
        fmt::Print("{}", timens);
        os::Exit(0);
    });
    // testPidFD's child: wait for a signal.
    go_helper::registerHelper("waitsignal", [](const std::vector<std::string>&) { time::Sleep(time::Hour); });
    // testAmbientCaps's child.
    go_helper::registerHelper("ambientcaps", [](const std::vector<std::string>&) {
        auto [c, err] = getCaps();
        if (err) exitWith(2, str(err.Error()));
        if ((c.data[0].effective & (1u << CAP_SYS_TIME)) == 0) {
            exitWith(2, "CAP_SYS_TIME unexpectedly not in the effective capability mask");
        }
        if ((c.data[1].effective & (1u << (CAP_SYSLOG & 31))) == 0) {
            exitWith(2, "CAP_SYSLOG unexpectedly not in the effective capability mask");
        }
        os::Exit(0);
    });
    return true;
}();

std::string trimSpace(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return std::string(s);
}

std::string out(const slice<byte>& b) { return std::string(asStringView(b)); }

// Go's testenv.MustHaveExecPath.
bool haveExecPath(const std::string& name) { return !os::exec::LookPath(name).second; }

// whoamiNEWUSER returns a command that runs "whoami" with CLONE_NEWUSER,
// mapping uid and gid 0 to the actual uid and gid of the test.
ptr<os::exec::Cmd> whoamiNEWUSER(int uid, int gid, bool setgroups) {
    auto cmd = os::exec::Command("whoami");
    syscall::SysProcAttr sys;
    sys.Cloneflags = syscall::CLONE_NEWUSER_;
    sys.UidMappings = {{.ContainerID = 0, .HostID = uid, .Size = 1}};
    sys.GidMappings = {{.ContainerID = 0, .HostID = gid, .Size = 1}};
    sys.GidMappingsEnableSetgroups = setgroups;
    cmd->SysProcAttr = sys;
    return cmd;
}

}  // namespace

TEST(GoSyscall, CloneNEWUSERAndRemap) {
    if (!haveExecPath("whoami")) GTEST_SKIP() << "whoami not found";
    for (bool setgroups : {false, true}) {
        SCOPED_TRACE(std::format("setgroups={}", setgroups));
        [&] {
            int uid = os::Getuid();
            int gid = os::Getgid();

            auto cmd = whoamiNEWUSER(uid, gid, setgroups);
            auto [o, err] = cmd->CombinedOutput();

            if (uid != 0 && setgroups) {
                // As non-root, expected permission error due to
                // unprivileged gid_map.
                if (!os::IsPermission(err)) {
                    if (!err) return;  // t.Skip: probably old kernel without security fix
                    if (syscallIsNotSupported(err)) return;  // t.Skip: CLONE_NEWUSER appears to be unsupported
                    FAIL() << "got non-permission error: " << err;
                }
                return;
            }

            if (err) {
                // May be inside a container that disallows CLONE_NEWUSER.
                if (syscallIsNotSupported(err)) return;  // t.Skip
                FAIL() << "unexpected command failure: " << err << "; output:\n" << out(o);
            }
            ASSERT_EQ(trimSpace(out(o)), "root") << "whoami";
        }();
    }
}

TEST(GoSyscall, EmptyCredGroupsDisableSetgroups) {
    if (!haveExecPath("whoami")) GTEST_SKIP() << "whoami not found";
    auto cmd = whoamiNEWUSER(os::Getuid(), os::Getgid(), false);
    cmd->SysProcAttr->Credential = syscall::Credential{};
    if (error err = cmd->Run()) {
        if (syscallIsNotSupported(err)) GTEST_SKIP() << err;
        FAIL() << err;
    }
}

TEST(GoSyscall, Unshare) {
    const std::string path = "/proc/net/dev";
    if (auto [fi, err] = os::Stat(path); err) {
        if (os::IsNotExist(err)) GTEST_SKIP() << "kernel doesn't support proc filesystem";
        if (os::IsPermission(err)) GTEST_SKIP() << "unable to test proc filesystem due to permissions";
        FAIL() << err;
    }

    auto [b, rerr] = os::ReadFile(path);
    ASSERT_FALSE(rerr) << rerr;
    std::string orig = trimSpace(out(b));
    if (orig.find("lo:") != orig.npos && std::ranges::count(orig, ':') == 1) {
        // This test expects there to be at least 1 more network interface
        // in addition to the local network interface, so that it can tell
        // that unshare worked.
        GTEST_SKIP() << "not enough network interfaces to test unshare with";
    }

    auto cmd = os::exec::Command("cat", path);
    syscall::SysProcAttr sys;
    sys.Unshareflags = syscall::CLONE_NEWNET_;
    cmd->SysProcAttr = sys;
    auto [o, err] = cmd->CombinedOutput();
    if (err) {
        // CLONE_NEWNET does not appear to be supported.
        if (syscallIsNotSupported(err)) GTEST_SKIP() << "skipping due to permission error: " << err;
        FAIL() << "Cmd failed with err " << err << ", output: " << out(o);
    }

    // Check there is only the local network interface.
    std::string sout = trimSpace(out(o));
    ASSERT_NE(sout.find("lo:"), sout.npos) << "Expected lo network interface to exist, got " << sout;
    auto lines = std::ranges::count(sout, '\n') + 1;
    auto origLines = std::ranges::count(orig, '\n') + 1;
    ASSERT_LT(lines, origLines) << path << " before unshare:\n" << orig << "\nafter unshare:\n" << sout;
}

TEST(GoSyscall, GroupCleanup) {
    if (!haveExecPath("id")) GTEST_SKIP() << "id not found";
    auto cmd = os::exec::Command("id");
    syscall::SysProcAttr sys;
    sys.Credential = syscall::Credential{.Uid = 0, .Gid = 0};
    cmd->SysProcAttr = sys;
    auto [o, err] = cmd->CombinedOutput();
    if (err) {
        if (syscallIsNotSupported(err)) GTEST_SKIP() << "skipping: " << err;
        FAIL() << "Cmd failed with err " << err << ", output: " << out(o);
    }
    std::string strOut = trimSpace(out(o));
    // Just check prefix because some distros reportedly output a context
    // parameter.
    EXPECT_TRUE(strOut.starts_with("uid=0(root) gid=0(root)")) << "id: " << strOut;
}

TEST(GoSyscall, GroupCleanupUserNamespace) {
    if (!haveExecPath("id")) GTEST_SKIP() << "id not found";
    auto cmd = os::exec::Command("id");
    int uid = os::Getuid(), gid = os::Getgid();
    syscall::SysProcAttr sys;
    sys.Cloneflags = syscall::CLONE_NEWUSER_;
    sys.Credential = syscall::Credential{.Uid = uint32(uid), .Gid = uint32(gid)};
    sys.UidMappings = {{.ContainerID = 0, .HostID = uid, .Size = 1}};
    sys.GidMappings = {{.ContainerID = 0, .HostID = gid, .Size = 1}};
    cmd->SysProcAttr = sys;
    auto [o, err] = cmd->CombinedOutput();
    if (err) {
        if (syscallIsNotSupported(err)) GTEST_SKIP() << "skipping: " << err;
        FAIL() << "Cmd failed with err " << err << ", output: " << out(o);
    }
    std::string strOut = trimSpace(out(o));
    // As in TestGroupCleanup, just check prefix.
    EXPECT_TRUE(strOut.starts_with("uid=0(root) gid=0(root) groups=0(root)")) << "id: " << strOut;
}

// Test for https://go.dev/issue/19661: unshare fails because systemd has
// forced / to be shared
TEST(GoSyscall, UnshareMountNameSpace) {
    TempDir td;
    std::string d = td.path();
    auto cmd = helper_command("mountproc", {d});
    syscall::SysProcAttr sys;
    sys.Unshareflags = syscall::CLONE_NEWNS_;
    cmd->SysProcAttr = sys;

    auto [o, err] = cmd->CombinedOutput();
    // If the subprocess fails to unshare the parent directory,
    // force-unmount it so that the test can clean it up.
    auto cleanup = [&] {
        if (!os::Stat(d).second) syscall::Unmount(d, syscall::MNT_FORCE);
    };
    if (err) {
        cleanup();
        if (syscallIsNotSupported(err)) GTEST_SKIP() << "skipping: could not start process with CLONE_NEWNS: " << err;
        FAIL() << "unshare failed: " << err << "\n" << out(o);
    } else if (!o.empty()) {
        cleanup();
        if (out(o).starts_with(mountNotSupported)) GTEST_SKIP() << "skipping: helper process reported " << out(o);
        FAIL() << "unexpected output from helper process: " << out(o);
    }

    // How do we tell if the namespace was really unshared? It turns out to
    // be simple: just try to remove the directory. If it's still mounted on
    // the rm will fail with EBUSY.
    if (error rerr = os::Remove(d)) {
        cleanup();
        ADD_FAILURE() << "rmdir failed on " << d << ": " << rerr;
    }
    os::Mkdir(d, 0700);  // for TempDir's cleanup
}

namespace {

// The program interpreter (PT_INTERP) of the ELF file exe, or "".
std::string elfInterp(const std::string& exe) {
    std::ifstream f(exe, std::ios::binary);
    Elf64_Ehdr eh{};
    if (!f.read(reinterpret_cast<char*>(&eh), sizeof(eh)) || eh.e_ident[EI_CLASS] != ELFCLASS64) return "";
    for (int i = 0; i < eh.e_phnum; i++) {
        Elf64_Phdr ph{};
        f.seekg(std::streamoff(eh.e_phoff + Elf64_Off(i) * eh.e_phentsize));
        if (!f.read(reinterpret_cast<char*>(&ph), sizeof(ph))) return "";
        if (ph.p_type != PT_INTERP) continue;
        std::string interp(ph.p_filesz, '\0');
        f.seekg(std::streamoff(ph.p_offset));
        f.read(interp.data(), std::streamsize(interp.size()));
        return std::string(interp.c_str());
    }
    return "";
}

// Copies the file src (following links) to dst, creating its directory.
error copyFile(const std::string& src, const std::string& dst, FileMode perm) {
    if (error err = os::MkdirAll(str(path::Dir(dst)), 0755)) return err;
    auto [data, err] = os::ReadFile(src);
    if (err) return err;
    return os::WriteFile(dst, data, perm);
}

// Go's test builds a static syscall.test for the chroot: here the chroot
// gets this program, its interpreter and the shared libraries it has
// loaded (from /proc/self/maps), at their paths. Returns the program's
// path in the chroot.
std::pair<std::string, error> installInChroot(const std::string& d) {
    std::string exe = str(go_helper::executable());
    if (error err = copyFile(exe, d + "/syscall.test", 0755)) return {"", err};
    std::vector<std::string> files;
    if (std::string interp = elfInterp(exe); !interp.empty()) files.push_back(interp);
    std::ifstream maps("/proc/self/maps");
    for (std::string line; std::getline(maps, line);) {
        auto i = line.find('/');
        if (i == line.npos) continue;
        std::string file = line.substr(i);
        if (file.find(".so") != file.npos && std::ranges::find(files, file) == files.end()) files.push_back(file);
    }
    for (auto& file : files) {
        if (error err = copyFile(file, d + file, 0755)) return {"", err};
    }
    return {"/syscall.test", nullptr};
}

}  // namespace

// Test for Issue 20103: unshare fails when chroot is used
TEST(GoSyscall, UnshareMountNameSpaceChroot) {
    TempDir td;
    std::string d = td.path();

    // Since we are doing a chroot, we need the binary there.
    auto [x, ierr] = installInChroot(d);
    ASSERT_FALSE(ierr) << ierr;
    // If the subprocess fails to unshare the parent directory,
    // force-unmount it so that the test can clean it up.
    auto cleanup = [&] {
        if (!os::Stat(d).second) syscall::Unmount(d, syscall::MNT_FORCE);
    };

    auto cmd = helper_command("mountproc", {"/"});
    cmd->Path = string(x);
    // The working directory would be outside the chroot, which googletest
    // can't start in: the chroot's root (chdir comes after chroot).
    cmd->Dir = string("/");
    syscall::SysProcAttr sys;
    sys.Chroot = string(d);
    sys.Unshareflags = syscall::CLONE_NEWNS_;
    cmd->SysProcAttr = sys;

    auto [o, err] = cmd->CombinedOutput();
    if (err) {
        cleanup();
        if (syscallIsNotSupported(err)) {
            GTEST_SKIP() << "skipping: could not start process with CLONE_NEWNS and Chroot " << d << ": " << err;
        }
        FAIL() << "unshare failed: " << err << "\n" << out(o);
    } else if (!o.empty()) {
        cleanup();
        if (out(o).starts_with(mountNotSupported)) GTEST_SKIP() << "skipping: helper process reported " << out(o);
        FAIL() << "unexpected output from helper process: " << out(o);
    }

    // How do we tell if the namespace was really unshared? It turns out to
    // be simple: just try to remove the executable. If it's still mounted
    // on, the rm will fail.
    if (error rerr = os::Remove(d + x)) {
        cleanup();
        ADD_FAILURE() << "rm failed on " << d + x << ": " << rerr;
    }
}

// Test for Issue 29789: unshare fails when uid/gid mapping is specified
TEST(GoSyscall, UnshareUidGidMapping) {
    if (os::Getuid() == 0) GTEST_SKIP() << "test exercises unprivileged user namespace, fails with privileges";

    auto cmd = helper_command("chroottmp");
    syscall::SysProcAttr sys;
    sys.Unshareflags = syscall::CLONE_NEWNS_ | syscall::CLONE_NEWUSER_;
    sys.GidMappingsEnableSetgroups = false;
    sys.UidMappings = {{.ContainerID = 0, .HostID = syscall::Getuid(), .Size = 1}};
    sys.GidMappings = {{.ContainerID = 0, .HostID = syscall::Getgid(), .Size = 1}};
    cmd->SysProcAttr = sys;
    auto [o, err] = cmd->CombinedOutput();
    if (err) {
        if (syscallIsNotSupported(err)) {
            GTEST_SKIP() << "skipping: could not start process with CLONE_NEWNS and CLONE_NEWUSER: " << err;
        }
        FAIL() << "Cmd failed with err " << err << ", output: " << out(o);
    }
}

namespace {

// prepareCgroupFD: an O_PATH descriptor of a new sub-cgroup of ours, and
// its name; a reason to skip otherwise.
struct cgroupFD {
    int fd = -1;
    std::string suffix, subCgroup, skip;
    ~cgroupFD() {
        if (fd >= 0) ::close(fd);
        if (!subCgroup.empty()) ::rmdir(subCgroup.c_str());
    }
};

void prepareCgroupFD(cgroupFD& c) {
    // Requires cgroup v2.
    const std::string prefix = "/sys/fs/cgroup";
    auto [selfCg, err] = os::ReadFile("/proc/self/cgroup");
    if (err) {
        if (os::IsNotExist(err) || os::IsPermission(err)) {
            c.skip = str(err.Error());
            return;
        }
        FAIL() << err;
    }
    // Expect a single line like this:
    // 0::/user.slice/user-1000.slice/user@1000.service/app.slice/vte-spawn-....scope
    // Otherwise it's either cgroup v1 or a hybrid hierarchy.
    std::string s = out(selfCg);
    if (std::ranges::count(s, '\n') > 1) {
        c.skip = "cgroup v2 not available";
        return;
    }
    if (!s.starts_with("0::")) {
        c.skip = "cgroup v2 not available (/proc/self/cgroup contents: " + s + ")";
        return;
    }
    std::string cg = trimSpace(std::string_view(s).substr(3));

    // Need an ability to create a sub-cgroup.
    auto [sub, merr] = os::MkdirTemp(prefix + cg, "subcg-");
    if (merr) {
        // ErrPermission or EROFS when running in an unprivileged
        // container. ErrNotExist when cgroupfs is not mounted.
        if (os::IsNotExist(merr) || syscallIsNotSupported(merr)) {
            c.skip = "skipping: " + str(merr.Error());
            return;
        }
        FAIL() << merr;
    }
    c.subCgroup = str(sub);
    c.fd = ::open(c.subCgroup.c_str(), O_PATH | O_CLOEXEC);
    ASSERT_GE(c.fd, 0) << "open " << c.subCgroup << ": " << syscall::errnoError(errno);
    c.suffix = "/" + str(path::Base(c.subCgroup));
}

}  // namespace

TEST(GoSyscall, UseCgroupFD) {
    cgroupFD c;
    prepareCgroupFD(c);
    if (HasFatalFailure()) return;
    if (!c.skip.empty()) GTEST_SKIP() << c.skip;

    auto cmd = helper_command("selfcgroup");
    syscall::SysProcAttr sys;
    sys.UseCgroupFD = true;
    sys.CgroupFD = c.fd;
    cmd->SysProcAttr = sys;
    auto [o, err] = cmd->CombinedOutput();
    if (err) {
        if (syscallIsNotSupported(err) && !errors::Is(err, syscall::EINVAL_)) {
            // Can be one of: clone3 not supported (old kernel); clone3 not
            // allowed (by e.g. seccomp); lack of CAP_SYS_ADMIN.
            GTEST_SKIP() << "clone3 with CLONE_INTO_CGROUP not available: " << err;
        }
        FAIL() << "Cmd failed with err " << err << ", output: " << out(o);
    }
    // NB: this wouldn't work with cgroupns.
    ASSERT_TRUE(trimSpace(out(o)).ends_with(c.suffix)) << "got: " << out(o) << ", want: a line that ends with "
                                                        << c.suffix;
}

TEST(GoSyscall, CloneTimeNamespace) {
    auto cmd = helper_command("timens");
    syscall::SysProcAttr sys;
    sys.Cloneflags = syscall::CLONE_NEWTIME_;
    cmd->SysProcAttr = sys;
    auto [o, err] = cmd->CombinedOutput();
    if (err) {
        // CLONE_NEWTIME does not appear to be supported.
        if (syscallIsNotSupported(err)) GTEST_SKIP() << "skipping, CLONE_NEWTIME not supported: " << err;
        FAIL() << "Cmd failed with err " << err << ", output: " << out(o);
    }

    // Inode number of the time namespaces should be different.
    auto [timens, rerr] = os::Readlink("/proc/self/ns/time");
    ASSERT_FALSE(rerr) << rerr;
    std::string parentTimeNS = str(timens);
    std::string childTimeNS = out(o);
    ASSERT_NE(childTimeNS, parentTimeNS) << "expected child time namespace to be different from parent time namespace";
}

namespace {

// Go's testPidFD; the error of Start, or nil (skip set if the test is to
// be skipped).
error testPidFD(bool userns, std::string& skip) {
    int pidfd = -1;
    auto cmd = helper_command("waitsignal");
    syscall::SysProcAttr sys;
    sys.PidFD = &pidfd;
    if (userns) sys.Cloneflags = syscall::CLONE_NEWUSER_;
    cmd->SysProcAttr = sys;
    if (error err = cmd->Start()) return err;
    struct killer {
        ptr<os::exec::Cmd> cmd;
        ~killer() {
            cmd->Process->Kill();
            cmd->Wait();
        }
    } k{cmd};
    // If pidfd is not supported by the kernel, -1 is returned.
    if (pidfd == -1) {
        skip = "pidfd not supported";
        return nullptr;
    }
    struct closer {
        int fd;
        ~closer() { ::close(fd); }
    } c{pidfd};

    // Use pidfd to send a signal to the child.
    const syscall::Signal sig = syscall::SIGINT_;
    if (::syscall(SYS_pidfd_send_signal, pidfd, sig.value, nullptr, 0) < 0) {
        error err = syscall::errnoError(errno);
        if (!errors::Is(err, syscall::EINVAL_) && syscallIsNotSupported(err)) {
            skip = "pidfd_send_signal syscall not supported: " + str(err.Error());
            return nullptr;
        }
        ADD_FAILURE() << "pidfd_send_signal syscall failed: " << err;
        return nullptr;
    }
    // Check if the child received our signal.
    error err = cmd->Wait();
    if (!cmd->ProcessState || syscall::WaitStatus(cmd->ProcessState->Sys()).Signal() != sig) {
        ADD_FAILURE() << "unexpected child error: " << err;
    }
    return nullptr;
}

}  // namespace

TEST(GoSyscall, PidFD) {
    std::string skip;
    if (error err = testPidFD(false, skip)) FAIL() << "can't start a process: " << err;
    if (!skip.empty()) GTEST_SKIP() << skip;
}

TEST(GoSyscall, PidFDWithUserNS) {
    std::string skip;
    if (error err = testPidFD(true, skip)) {
        if (syscallIsNotSupported(err)) GTEST_SKIP() << "userns not supported: " << err;
        FAIL() << "can't start a process: " << err;
    }
    if (!skip.empty()) GTEST_SKIP() << skip;
}

TEST(GoSyscall, PidFDClone3) {
    syscall::detail::force_clone3 = true;
    struct reset {
        ~reset() { syscall::detail::force_clone3 = false; }
    } r;
    std::string skip;
    if (error err = testPidFD(false, skip)) {
        if (syscallIsNotSupported(err)) GTEST_SKIP() << "clone3 not supported: " << err;
        FAIL() << "can't start a process: " << err;
    }
    if (!skip.empty()) GTEST_SKIP() << skip;
}

namespace {

// The uid and gid of user "nobody", from /etc/passwd.
std::optional<std::pair<int, int>> lookupNobody() {
    std::ifstream f("/etc/passwd");
    std::string line;
    while (std::getline(f, line)) {
        // name:password:uid:gid:...
        std::vector<std::string> fields;
        std::size_t start = 0;
        for (std::size_t i; (i = line.find(':', start)) != line.npos; start = i + 1) fields.push_back(line.substr(start, i - start));
        fields.push_back(line.substr(start));
        if (fields.size() >= 4 && fields[0] == "nobody") return std::pair{std::stoi(fields[2]), std::stoi(fields[3])};
    }
    return std::nullopt;
}

void testAmbientCaps(bool userns) {
    auto nobodyIDs = lookupNobody();
    if (!nobodyIDs) GTEST_SKIP() << "skipping: the nobody user does not exist; see Issue 71644";
    auto [uid, gid] = *nobodyIDs;

    // Copy the test binary to a temporary location which is readable by
    // nobody.
    auto [f, err] = os::CreateTemp("", "gotest");
    ASSERT_FALSE(err) << err;
    std::string fname = str(f->Name());
    struct remover {
        ptr<File> f;
        std::string name;
        ~remover() {
            f->Close();
            os::Remove(name);
        }
    } rm{f, fname};

    auto [e, oerr] = os::Open(go_helper::executable().view());
    ASSERT_FALSE(oerr) << oerr;
    auto [n, cerr] = io::Copy(*f, *e);
    e->Close();
    ASSERT_FALSE(cerr) << cerr;
    ASSERT_FALSE(f->Chmod(0755));
    ASSERT_FALSE(f->Close());

    auto cmd = helper_command("ambientcaps");
    cmd->Path = string(fname);
    cmd->Stdout = ptr<File>(&os::Stdout);
    cmd->Stderr = ptr<File>(&os::Stderr);
    syscall::SysProcAttr sys;
    sys.Credential = syscall::Credential{.Uid = uint32(uid), .Gid = uint32(gid)};
    sys.AmbientCaps = {CAP_SYS_TIME, CAP_SYSLOG};
    if (userns) {
        sys.Cloneflags = syscall::CLONE_NEWUSER_;
        constexpr int nobody = 65534;
        sys.UidMappings = {{.ContainerID = nobody, .HostID = os::Getuid(), .Size = 1}};
        sys.GidMappings = {{.ContainerID = nobody, .HostID = os::Getgid(), .Size = 1}};
        // Set credentials to run as user and group nobody.
        sys.Credential = syscall::Credential{.Uid = nobody, .Gid = nobody};
    }
    cmd->SysProcAttr = sys;
    if (error rerr = cmd->Run()) {
        if (syscallIsNotSupported(rerr)) GTEST_SKIP() << "skipping: " << rerr;
        FAIL() << rerr;
    }
}

}  // namespace

TEST(GoSyscall, AmbientCaps) { testAmbientCaps(false); }

TEST(GoSyscall, AmbientCapsUserns) {
    auto [b, err] = os::ReadFile("/proc/sys/kernel/apparmor_restrict_unprivileged_userns");
    if (!err && trimSpace(out(b)) == "1") {
        GTEST_SKIP() << "AppArmor restriction for unprivileged user namespaces is enabled";
    }
    testAmbientCaps(true);
}
