// fork and exec, after Go's syscall/exec_unix.go and exec_linux.go.
//
// The child is made with clone(CLONE_VM|CLONE_VFORK): it shares this
// process's memory, and the calling thread is suspended until the child
// execs or exits, so that nothing is copied. The child runs on a stack of
// its own and makes only raw system calls until exec: no locks, no
// allocation, no errno, as other threads may hold the C library's locks.
// The calling thread blocks all signals around the clone, and the child
// resets the handlers to the default before it unblocks them, so that no
// handler of this process runs in the child.
//
// clone3, which glibc doesn't wrap, starts the child through a trampoline
// of a few instructions that calls child_main on the new stack, as glibc's
// clone does.
#include "exec_linux.hpp"

#include <gopp/errors.hpp>
#include <gopp/runtime.hpp>
#include <gopp/syscall.hpp>

#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include <cerrno>
#include <optional>
#include <string>
#include <vector>

#ifndef CLONE_PIDFD
#define CLONE_PIDFD 0x00001000
#endif
#ifndef CLONE_INTO_CGROUP
#define CLONE_INTO_CGROUP 0x200000000ULL
#endif
#ifndef SYS_clone3
#define SYS_clone3 435
#endif
#ifndef PR_CAP_AMBIENT
#define PR_CAP_AMBIENT 47
#define PR_CAP_AMBIENT_RAISE 2
#endif

// gopp_clone3_trampoline(args, size, fn, arg): clone3(args, size); in the
// child, on the stack args gives, exit(fn(arg)); in the parent, the result:
// the child's pid or -errno.
extern "C" long gopp_clone3_trampoline(void* args, unsigned long size, int (*fn)(void*), void* arg);

#if defined(__x86_64__)
// The syscall instruction keeps r8 and r9; the child starts with the
// parent's registers and the stack pointer at the top of its stack.
asm(R"(
    .pushsection .text
    .p2align 4
    .globl gopp_clone3_trampoline
    .hidden gopp_clone3_trampoline
    .type gopp_clone3_trampoline, @function
gopp_clone3_trampoline:
    movq %rcx, %r8
    movq %rdx, %r9
    movl $435, %eax
    syscall
    testq %rax, %rax
    jnz 1f
    xorl %ebp, %ebp
    movq %r8, %rdi
    call *%r9
    movl %eax, %edi
    movl $60, %eax
    syscall
    hlt
1:
    ret
    .size gopp_clone3_trampoline, .-gopp_clone3_trampoline
    .popsection
)");
#elif defined(__aarch64__)
asm(R"(
    .pushsection .text
    .p2align 2
    .globl gopp_clone3_trampoline
    .hidden gopp_clone3_trampoline
    .type gopp_clone3_trampoline, %function
gopp_clone3_trampoline:
    mov x10, x2
    mov x11, x3
    mov x8, #435
    svc #0
    cbnz x0, 1f
    mov x29, xzr
    mov x30, xzr
    mov x0, x11
    blr x10
    mov x8, #93
    svc #0
    brk #0
1:
    ret
    .size gopp_clone3_trampoline, .-gopp_clone3_trampoline
    .popsection
)");
#endif

namespace gopp::syscall {

namespace detail {

long rawSyscall(long n, long a1, long a2, long a3, long a4, long a5, long a6) noexcept {
    long ret;
#if defined(__x86_64__)
    register long r10 asm("r10") = a4;
    register long r8 asm("r8") = a5;
    register long r9 asm("r9") = a6;
    asm volatile("syscall"
                 : "=a"(ret)
                 : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8), "r"(r9)
                 : "rcx", "r11", "memory");
#elif defined(__aarch64__)
    register long x8 asm("x8") = n;
    register long x0 asm("x0") = a1;
    register long x1 asm("x1") = a2;
    register long x2 asm("x2") = a3;
    register long x3 asm("x3") = a4;
    register long x4 asm("x4") = a5;
    register long x5 asm("x5") = a6;
    asm volatile("svc 0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5) : "memory", "cc");
    ret = x0;
#else
#error "raw_syscall: unsupported architecture"
#endif
    return ret;
}

}  // namespace detail

namespace {

using detail::rawSyscall;

// The kernel's struct sigaction (with sa_restorer, on x86-64 and ARM64).
struct kernelSigaction {
    unsigned long handler;
    unsigned long flags;
    unsigned long restorer;
    uint64_t mask;
};

constexpr int childStackSize = 64 * 1024;
constexpr int nsig = 65;

// What the child needs, made by the parent before the clone.
struct childArgs {
    const char* argv0;
    char* const* argv;
    char* const* envv;
    const char* chroot;
    const char* dir;
    int* fd;  // the descriptors to give the child, -1 for closed
    int nfd;
    int nextfd;  // above any descriptor of fd
    int pipe;    // the error pipe's write end
    const SysProcAttr* sys;
    const gid_t* groups;
    long ngroups;
    long ppid;
    const sigset_t* oldmask;  // the mask to restore
    // The ID mappings: the pipe the parent reports writing them on, -1
    // without; and what the child writes after unshare(CLONE_NEWUSER).
    int mapPipe[2];
    const char* uidmap;
    long uidmapLen;
    const char* gidmap;
    long gidmapLen;
    const char* setgroups;
    long setgroupsLen;
};

// The kernel's capabilities, version 3 (Go's caps).
constexpr uint32_t linuxCapabilityVersion3 = 0x20080522;
struct capHeader {
    uint32_t version;
    int32_t pid;
};
struct capData {
    uint32_t effective;
    uint32_t permitted;
    uint32_t inheritable;
};

// The child: system calls only, then exec. On failure, writes the errno
// to the pipe and exits.
[[gnu::noinline]] int childMain(void* arg) {
    auto* a = static_cast<childArgs*>(arg);
    const SysProcAttr* sys = a->sys;
    long r = 0;
    int err1 = 0;
    int pipe = a->pipe;
    int nextfd = a->nextfd;

#define CHILD_CALL(...)                     \
    do {                                    \
        r = rawSyscall(__VA_ARGS__);       \
        if (r < 0 && r > -4096) goto childerror; \
    } while (0)

    // Enable the "keep capabilities" flag to set ambient capabilities
    // later.
    if (!sys->AmbientCaps.empty()) CHILD_CALL(SYS_prctl, PR_SET_KEEPCAPS, 1, 0, 0, 0, 0);

    // Wait for the User ID/Group ID mappings to be written.
    if (a->mapPipe[0] >= 0) {
        CHILD_CALL(SYS_close, a->mapPipe[1], 0, 0, 0, 0, 0);
        int err2 = 0;
        CHILD_CALL(SYS_read, a->mapPipe[0], reinterpret_cast<long>(&err2), sizeof(err2), 0, 0, 0);
        if (r != sizeof(err2)) {
            r = -EINVAL;
            goto childerror;
        }
        if (err2 != 0) {
            r = -err2;
            goto childerror;
        }
    }

    // Session and process group.
    if (sys->Setsid) CHILD_CALL(SYS_setsid, 0, 0, 0, 0, 0, 0);
    if (sys->Setpgid || sys->Foreground) CHILD_CALL(SYS_setpgid, 0, sys->Pgid, 0, 0, 0, 0);
    if (sys->Foreground) {
        int pgrp = sys->Pgid;
        if (pgrp == 0) pgrp = static_cast<int>(rawSyscall(SYS_getpid, 0, 0, 0, 0, 0, 0));
        // Before the signal mask is restored, so that SIGTTOU doesn't
        // stop the child.
        CHILD_CALL(SYS_ioctl, sys->Ctty, TIOCSPGRP, reinterpret_cast<long>(&pgrp), 0, 0, 0);
    }

    // Go's runtime_AfterForkInChild: the handlers of this process back to
    // the default, ignored signals kept ignored; then the signal mask.
    for (int s = 1; s < nsig; s++) {
        kernelSigaction old{};
        if (rawSyscall(SYS_rt_sigaction, s, 0, reinterpret_cast<long>(&old), 8, 0, 0) < 0) continue;
        if (old.handler != reinterpret_cast<unsigned long>(SIG_DFL) &&
            old.handler != reinterpret_cast<unsigned long>(SIG_IGN)) {
            kernelSigaction dfl{};
            rawSyscall(SYS_rt_sigaction, s, reinterpret_cast<long>(&dfl), 0, 8, 0, 0);
        }
    }
    rawSyscall(SYS_rt_sigprocmask, SIG_SETMASK, reinterpret_cast<long>(a->oldmask), 0, 8, 0, 0);

    if (sys->Unshareflags != 0) {
        CHILD_CALL(SYS_unshare, static_cast<long>(sys->Unshareflags), 0, 0, 0, 0, 0);
        // In a new user namespace, the child writes its own mappings.
        if ((sys->Unshareflags & CLONE_NEWUSER_) != 0) {
            struct {
                const char* path;
                const char* data;
                long len;
            } maps[3] = {};
            int n = 0;
            static const char setgroupsPath[] = "/proc/self/setgroups";
            static const char gidMapPath[] = "/proc/self/gid_map";
            static const char uidMapPath[] = "/proc/self/uid_map";
            if (a->gidmap) {
                maps[n++] = {setgroupsPath, a->setgroups, a->setgroupsLen};
                maps[n++] = {gidMapPath, a->gidmap, a->gidmapLen};
            }
            if (a->uidmap) maps[n++] = {uidMapPath, a->uidmap, a->uidmapLen};
            for (int k = 0; k < n; k++) {
                CHILD_CALL(SYS_openat, AT_FDCWD, reinterpret_cast<long>(maps[k].path), O_WRONLY, 0, 0, 0);
                long fd1 = r;
                CHILD_CALL(SYS_write, fd1, reinterpret_cast<long>(maps[k].data), maps[k].len, 0, 0, 0);
                CHILD_CALL(SYS_close, fd1, 0, 0, 0, 0, 0);
            }
        }
        // Mount points shared with the parent's namespace stay shared
        // after unshare; Go makes / private, as unshare(1) does.
        if ((sys->Unshareflags & CLONE_NEWNS_) == CLONE_NEWNS_) {
            static const char none[] = "none";
            static const char slash[] = "/";
            CHILD_CALL(SYS_mount, reinterpret_cast<long>(none), reinterpret_cast<long>(slash), 0, MS_REC | MS_PRIVATE,
                       0, 0);
        }
    }

    if (a->chroot) CHILD_CALL(SYS_chroot, reinterpret_cast<long>(a->chroot), 0, 0, 0, 0, 0);

    // User and groups.
    if (sys->Credential) {
        // With GidMappings and setgroups denied, an empty Groups is left
        // alone: setgroups would fail.
        if (!(!sys->GidMappings.empty() && !sys->GidMappingsEnableSetgroups && a->ngroups == 0) &&
            !sys->Credential->NoSetGroups) {
            CHILD_CALL(SYS_setgroups, a->ngroups, reinterpret_cast<long>(a->groups), 0, 0, 0, 0);
        }
        CHILD_CALL(SYS_setgid, sys->Credential->Gid, 0, 0, 0, 0, 0);
        CHILD_CALL(SYS_setuid, sys->Credential->Uid, 0, 0, 0, 0, 0);
    }

    if (!sys->AmbientCaps.empty()) {
        capHeader hdr{linuxCapabilityVersion3, 0};
        capData data[2] = {};
        CHILD_CALL(SYS_capget, reinterpret_cast<long>(&hdr), reinterpret_cast<long>(data), 0, 0, 0, 0);
        // Add each capability to the permitted and inheritable sets,
        // otherwise it can't be added to the ambient set.
        for (uintptr c : sys->AmbientCaps) {
            uintptr idx = c >> 5;
            if (idx >= 2) {
                r = -EINVAL;
                goto childerror;
            }
            data[idx].permitted |= uint32_t(1) << (c & 31);
            data[idx].inheritable |= uint32_t(1) << (c & 31);
        }
        CHILD_CALL(SYS_capset, reinterpret_cast<long>(&hdr), reinterpret_cast<long>(data), 0, 0, 0, 0);
        for (uintptr c : sys->AmbientCaps) {
            CHILD_CALL(SYS_prctl, PR_CAP_AMBIENT, PR_CAP_AMBIENT_RAISE, static_cast<long>(c), 0, 0, 0);
        }
    }

    if (a->dir) CHILD_CALL(SYS_chdir, reinterpret_cast<long>(a->dir), 0, 0, 0, 0, 0);

    // Parent death signal.
    if (sys->Pdeathsig.value != 0) {
        CHILD_CALL(SYS_prctl, PR_SET_PDEATHSIG, sys->Pdeathsig.value, 0, 0, 0, 0);
        // The parent may have died already.
        if (rawSyscall(SYS_getppid, 0, 0, 0, 0, 0, 0) != a->ppid) {
            long pid = rawSyscall(SYS_getpid, 0, 0, 0, 0, 0, 0);
            CHILD_CALL(SYS_kill, pid, sys->Pdeathsig.value, 0, 0, 0, 0);
        }
    }

    // Pass 1: move the descriptors fd[i] < i, and the pipe, above all of
    // them, so that pass 2 doesn't overwrite one it needs later.
    if (pipe < nextfd) {
        CHILD_CALL(SYS_dup3, pipe, nextfd, O_CLOEXEC, 0, 0, 0);
        pipe = nextfd;
        nextfd++;
    }
    for (int i = 0; i < a->nfd; i++) {
        if (a->fd[i] >= 0 && a->fd[i] < i) {
            if (nextfd == pipe) nextfd++;  // don't overwrite the pipe
            CHILD_CALL(SYS_dup3, a->fd[i], nextfd, O_CLOEXEC, 0, 0, 0);
            a->fd[i] = nextfd;
            nextfd++;
        }
    }
    // Pass 2: dup fd[i] onto i, without close-on-exec.
    for (int i = 0; i < a->nfd; i++) {
        if (a->fd[i] == -1) {
            rawSyscall(SYS_close, i, 0, 0, 0, 0, 0);
            continue;
        }
        if (a->fd[i] == i) {
            // dup3(i, i) fails; clear close-on-exec instead.
            CHILD_CALL(SYS_fcntl, a->fd[i], F_SETFD, 0, 0, 0, 0);
            continue;
        }
        CHILD_CALL(SYS_dup3, a->fd[i], i, 0, 0, 0, 0);
    }
    // By convention, 0, 1 and 2 aren't left to the child unless given.
    for (int i = a->nfd; i < 3; i++) rawSyscall(SYS_close, i, 0, 0, 0, 0, 0);

    if (sys->Noctty) CHILD_CALL(SYS_ioctl, 0, TIOCNOTTY, 0, 0, 0, 0);
    if (sys->Setctty) CHILD_CALL(SYS_ioctl, sys->Ctty, TIOCSCTTY, 1, 0, 0, 0);

    // Tracing last, so that the setup above isn't traced.
    if (sys->Ptrace) CHILD_CALL(SYS_ptrace, PTRACE_TRACEME, 0, 0, 0, 0, 0);

    r = rawSyscall(SYS_execve, reinterpret_cast<long>(a->argv0), reinterpret_cast<long>(a->argv),
                    reinterpret_cast<long>(a->envv), 0, 0, 0);

childerror:
#undef CHILD_CALL
    err1 = static_cast<int>(-r);
    rawSyscall(SYS_write, pipe, reinterpret_cast<long>(&err1), sizeof(err1), 0, 0, 0);
    for (;;) rawSyscall(SYS_exit, 253, 0, 0, 0, 0, 0);
}

// NUL-terminated copies of strings, and pointers to them; false if one
// holds a NUL (Go's SlicePtrFromStrings).
struct cstrings {
    std::vector<std::string> s;
    std::vector<char*> p;

    bool assign(std::span<const string> v) {
        s.reserve(v.size());
        for (const string& x : v) {
            if (x.find('\0') != string::npos) return false;
            s.emplace_back(x.view());
        }
        for (auto& x : s) p.push_back(x.data());
        p.push_back(nullptr);
        return true;
    }
};

const SysProcAttr zeroSysProcAttr{};

// The kernel's struct clone_args, of clone3(2).
struct cloneArgsK {
    uint64_t flags;
    uint64_t pidfd;
    uint64_t childTid;
    uint64_t parentTid;
    uint64_t exitSignal;
    uint64_t stack;
    uint64_t stackSize;
    uint64_t tls;
    uint64_t setTid;
    uint64_t setTidSize;
    uint64_t cgroup;
};

// Makes a child running child_main(args) with clone(2), or clone3(2) if
// clone3 isn't null (its flags, pidfd and cgroup set); returns its pid, or
// -errno. Signals are blocked around it.
long cloneChild(childArgs& args, int flags, int* pidfd, cloneArgsK* clone3) {
    void* stack = ::mmap(nullptr, childStackSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK,
                         -1, 0);
    if (stack == MAP_FAILED) return -errno;
    sigset_t all, old;
    sigfillset(&all);
    long pid;
    {
        // The thread is suspended until the child execs: a blocking call.
        // Go's runtime_BeforeFork blocks the signals.
        runtime::blockingCall b;
        pthread_sigmask(SIG_SETMASK, &all, &old);
        args.oldmask = &old;
        if (clone3) {
            clone3->stack = reinterpret_cast<uint64_t>(stack);
            clone3->stackSize = childStackSize;
            pid = gopp_clone3_trampoline(clone3, sizeof(*clone3), childMain, &args);
        } else {
            pid = ::clone(childMain, static_cast<char*>(stack) + childStackSize, flags, &args, pidfd, nullptr,
                          nullptr);
            if (pid < 0) pid = -errno;
        }
        pthread_sigmask(SIG_SETMASK, &old, nullptr);
    }
    ::munmap(stack, childStackSize);
    return pid;
}

// Go's formatIDMappings.
std::string formatIDMappings(const std::vector<SysProcIDMap>& idMap) {
    std::string data;
    for (auto& im : idMap) {
        data += std::to_string(im.ContainerID) + " " + std::to_string(im.HostID) + " " + std::to_string(im.Size) + "\n";
    }
    return data;
}

// Writes data to the file path; 0 or the errno.
int writeProcFile(const std::string& path, std::string_view data) {
    int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) return errno;
    ssize_t n = ::write(fd, data.data(), data.size());
    int e = n < 0 ? errno : 0;
    if (::close(fd) < 0 && e == 0) e = errno;
    return e;
}

// Go's writeUidGidMappings: the mappings of the child pid, from the
// parent; 0 or the errno.
int writeUidGidMappings(long pid, const SysProcAttr& sys, const std::string& uidmap, const std::string& gidmap) {
    std::string proc = "/proc/" + std::to_string(pid);
    if (!sys.UidMappings.empty()) {
        if (int e = writeProcFile(proc + "/uid_map", uidmap)) return e;
    }
    if (!sys.GidMappings.empty()) {
        // If the kernel is too old to support /proc/PID/setgroups,
        // writing it fails with ENOENT; this is OK.
        if (int e = writeProcFile(proc + "/setgroups", sys.GidMappingsEnableSetgroups ? "allow" : "deny");
            e && e != ENOENT) {
            return e;
        }
        if (int e = writeProcFile(proc + "/gid_map", gidmap)) return e;
    }
    return 0;
}

}  // namespace

namespace detail {

bool forceClone3 = false;

bool checkClonePidfd() {
    // A child that exits at once, without an exit signal, as in Go: a
    // SIGCHLD would reach a program that notifies SIGCHLD.
    int pidfd = -1;
    childArgs args{};
    void* stack = ::mmap(nullptr, childStackSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK,
                         -1, 0);
    if (stack == MAP_FAILED) return false;
    sigset_t all, old;
    sigfillset(&all);
    pthread_sigmask(SIG_SETMASK, &all, &old);
    long pid = ::clone(
        [](void*) -> int {
            for (;;) rawSyscall(SYS_exit_group, 0, 0, 0, 0, 0, 0);
        },
        static_cast<char*>(stack) + childStackSize, CLONE_VM | CLONE_VFORK | CLONE_PIDFD, &args, &pidfd,
        nullptr, nullptr);
    pthread_sigmask(SIG_SETMASK, &old, nullptr);
    ::munmap(stack, childStackSize);
    if (pid < 0) return false;
    int status;
    // A child without an exit signal is a "clone" child: __WCLONE.
    while (::waitpid(static_cast<pid_t>(pid), &status, __WCLONE) < 0 && errno == EINTR) {
    }
    if (pidfd < 0) return false;
    ::close(pidfd);
    return true;
}

}  // namespace detail

std::pair<int, gopp::error> ForkExec(std::string_view argv0, std::span<const string> argv, const ProcAttr* attr) {
    static const ProcAttr zeroAttr{};
    if (!attr) attr = &zeroAttr;
    const SysProcAttr* sys = attr->Sys ? attr->Sys : &zeroSysProcAttr;

    // The C forms of the arguments.
    if (argv0.find('\0') != std::string_view::npos) return {0, EINVAL_};
    std::string argv0s(argv0);
    cstrings argvp, envvp;
    if (!argvp.assign(argv) || !envvp.assign(std::span<const string>(attr->Env.begin(), attr->Env.size()))) {
        return {0, EINVAL_};
    }
    std::string chroot, dir;
    if (!sys->Chroot.empty()) {
        if (sys->Chroot.find('\0') != string::npos) return {0, EINVAL_};
        chroot = sys->Chroot.str();
    }
    if (!attr->Dir.empty()) {
        if (attr->Dir.find('\0') != string::npos) return {0, EINVAL_};
        dir = attr->Dir.str();
    }

    // Setctty and Foreground both use Ctty, with different meanings.
    if (sys->Setctty && sys->Foreground) return {0, errors::New("both Setctty and Foreground set in SysProcAttr")};
    if (sys->Setctty && sys->Ctty >= static_cast<int>(attr->Files.size())) {
        return {0, errors::New("Setctty set but Ctty not valid in child")};
    }

    // The descriptors, and the first descriptor above them.
    std::vector<int> fd(attr->Files.size());
    int nextfd = static_cast<int>(attr->Files.size());
    for (std::size_t i = 0; i < attr->Files.size(); i++) {
        fd[i] = static_cast<int>(static_cast<intptr_t>(attr->Files[i]));
        if (nextfd < fd[i]) nextfd = fd[i];
    }
    nextfd++;

    std::vector<gid_t> groups;
    if (sys->Credential) groups.assign(sys->Credential->Groups.begin(), sys->Credential->Groups.end());

    // The ID mappings, and what the child writes after unshare.
    bool mappings = !sys->UidMappings.empty() || !sys->GidMappings.empty();
    std::string uidmap = formatIDMappings(sys->UidMappings);
    std::string gidmap = formatIDMappings(sys->GidMappings);
    std::string setgroups = sys->GidMappingsEnableSetgroups ? "allow" : "deny";

    uint64_t flags = sys->Cloneflags;
    // With CLONE_VFORK the parent would wait for the child, while the
    // child waits for the parent's mappings (Go would hang there); a new
    // user namespace rules it out anyway.
    if (((sys->Cloneflags | sys->Unshareflags) & CLONE_NEWUSER_) == 0 && !mappings) flags |= CLONE_VFORK | CLONE_VM;
    if (sys->PidFD) flags |= CLONE_PIDFD;
    int pidfd = -1;
    // Whether to use clone3: CLONE_INTO_CGROUP and CLONE_NEWTIME need it.
    std::optional<cloneArgsK> clone3;
    if (sys->UseCgroupFD || (flags & CLONE_NEWTIME_) != 0 || detail::forceClone3) {
        clone3 = cloneArgsK{};
        clone3->flags = flags;
        clone3->exitSignal = SIGCHLD;
        if (sys->UseCgroupFD) {
            clone3->flags |= CLONE_INTO_CGROUP;
            clone3->cgroup = static_cast<uint64_t>(sys->CgroupFD);
        }
        if (sys->PidFD) clone3->pidfd = reinterpret_cast<uint64_t>(&pidfd);
    }

    childArgs args{};
    args.argv0 = argv0s.c_str();
    args.argv = argvp.p.data();
    args.envv = envvp.p.data();
    args.chroot = chroot.empty() ? nullptr : chroot.c_str();
    args.dir = dir.empty() ? nullptr : dir.c_str();
    args.fd = fd.data();
    args.nfd = static_cast<int>(fd.size());
    args.nextfd = nextfd;
    args.sys = sys;
    args.groups = groups.data();
    args.ngroups = static_cast<long>(groups.size());
    args.ppid = sys->Pdeathsig.value != 0 ? static_cast<long>(::getpid()) : 0;
    args.mapPipe[0] = args.mapPipe[1] = -1;
    if (!sys->UidMappings.empty()) {
        args.uidmap = uidmap.c_str();
        args.uidmapLen = static_cast<long>(uidmap.size());
    }
    if (!sys->GidMappings.empty()) {
        args.gidmap = gidmap.c_str();
        args.gidmapLen = static_cast<long>(gidmap.size());
        args.setgroups = setgroups.c_str();
        args.setgroupsLen = static_cast<long>(setgroups.size());
    }

    ForkLock.Lock();
    // The pipe the child reports an error on; exec closes it.
    int p[2];
    if (::pipe2(p, O_CLOEXEC) < 0) {
        int e = errno;
        ForkLock.Unlock();
        return {0, errnoError(e)};
    }
    args.pipe = p[1];
    // Another pipe for parent to child communication for synchronizing
    // writing of User ID/Group ID mappings.
    if (mappings && ::pipe2(args.mapPipe, O_CLOEXEC) < 0) {
        int e = errno;
        ForkLock.Unlock();
        ::close(p[0]);
        ::close(p[1]);
        return {0, errnoError(e)};
    }
    long pid = cloneChild(args, clone3 ? 0 : static_cast<int>(flags | SIGCHLD), &pidfd, clone3 ? &*clone3 : nullptr);
    ForkLock.Unlock();
    ::close(p[1]);
    if (pid < 0) {
        ::close(p[0]);
        if (mappings) {
            ::close(args.mapPipe[0]);
            ::close(args.mapPipe[1]);
        }
        return {0, errnoError(static_cast<int>(-pid))};
    }

    if (mappings) {
        ::close(args.mapPipe[0]);
        // The uid/gid mappings are written after fork, or by the child
        // after unshare(2) of its user namespace.
        int err2 = 0;
        if ((sys->Unshareflags & CLONE_NEWUSER_) == 0) err2 = writeUidGidMappings(pid, *sys, uidmap, gidmap);
        while (::write(args.mapPipe[1], &err2, sizeof(err2)) < 0 && errno == EINTR) {
        }
        ::close(args.mapPipe[1]);
    }

    // The child's error, or EOF if exec closed the pipe. Without
    // CLONE_VFORK the child may still be running: a blocking call.
    int err1 = 0;
    ssize_t n;
    {
        runtime::blockingCall b;
        std::size_t got = 0;
        for (;;) {
            n = ::read(p[0], reinterpret_cast<char*>(&err1) + got, sizeof(err1) - got);
            if (n < 0 && errno == EINTR) continue;
            if (n > 0) {
                got += static_cast<std::size_t>(n);
                if (got < sizeof(err1)) continue;
                n = static_cast<ssize_t>(got);
            } else if (n == 0) {
                n = static_cast<ssize_t>(got);
            }
            break;
        }
    }
    int rerr = n < 0 ? errno : 0;
    ::close(p[0]);
    if (n != 0 || rerr != 0) {
        gopp::error err;
        if (n == static_cast<ssize_t>(sizeof(err1))) err = errnoError(err1);
        else if (rerr) err = errnoError(rerr);
        else err = EPIPE_;
        // The child failed: reap it, so that zombies don't pile up.
        int status;
        {
            runtime::blockingCall b;
            while (::waitpid(static_cast<pid_t>(pid), &status, 0) < 0 && errno == EINTR) {
            }
        }
        if (pidfd >= 0) ::close(pidfd);
        if (sys->PidFD) *sys->PidFD = -1;
        return {0, err};
    }
    if (sys->PidFD) *sys->PidFD = pidfd;
    return {static_cast<int>(pid), nullptr};
}

}  // namespace gopp::syscall
