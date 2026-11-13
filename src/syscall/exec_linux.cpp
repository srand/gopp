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
#include <string>
#include <vector>

#ifndef CLONE_PIDFD
#define CLONE_PIDFD 0x00001000
#endif

namespace gopp::syscall {

namespace detail {

long raw_syscall(long n, long a1, long a2, long a3, long a4, long a5, long a6) noexcept {
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

using detail::raw_syscall;

// The kernel's struct sigaction (with sa_restorer, on x86-64 and ARM64).
struct kernel_sigaction {
    unsigned long handler;
    unsigned long flags;
    unsigned long restorer;
    uint64_t mask;
};

constexpr int child_stack_size = 64 * 1024;
constexpr int nsig = 65;

// What the child needs, made by the parent before the clone.
struct child_args {
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
};

// The child: system calls only, then exec. On failure, writes the errno
// to the pipe and exits.
[[gnu::noinline]] int child_main(void* arg) {
    auto* a = static_cast<child_args*>(arg);
    const SysProcAttr* sys = a->sys;
    long r = 0;
    int err1 = 0;
    int pipe = a->pipe;
    int nextfd = a->nextfd;

#define CHILD_CALL(...)                     \
    do {                                    \
        r = raw_syscall(__VA_ARGS__);       \
        if (r < 0 && r > -4096) goto childerror; \
    } while (0)

    // Session and process group.
    if (sys->Setsid) CHILD_CALL(SYS_setsid, 0, 0, 0, 0, 0, 0);
    if (sys->Setpgid || sys->Foreground) CHILD_CALL(SYS_setpgid, 0, sys->Pgid, 0, 0, 0, 0);
    if (sys->Foreground) {
        int pgrp = sys->Pgid;
        if (pgrp == 0) pgrp = static_cast<int>(raw_syscall(SYS_getpid, 0, 0, 0, 0, 0, 0));
        // Before the signal mask is restored, so that SIGTTOU doesn't
        // stop the child.
        CHILD_CALL(SYS_ioctl, sys->Ctty, TIOCSPGRP, reinterpret_cast<long>(&pgrp), 0, 0, 0);
    }

    // Go's runtime_AfterForkInChild: the handlers of this process back to
    // the default, ignored signals kept ignored; then the signal mask.
    for (int s = 1; s < nsig; s++) {
        kernel_sigaction old{};
        if (raw_syscall(SYS_rt_sigaction, s, 0, reinterpret_cast<long>(&old), 8, 0, 0) < 0) continue;
        if (old.handler != reinterpret_cast<unsigned long>(SIG_DFL) &&
            old.handler != reinterpret_cast<unsigned long>(SIG_IGN)) {
            kernel_sigaction dfl{};
            raw_syscall(SYS_rt_sigaction, s, reinterpret_cast<long>(&dfl), 0, 8, 0, 0);
        }
    }
    raw_syscall(SYS_rt_sigprocmask, SIG_SETMASK, reinterpret_cast<long>(a->oldmask), 0, 8, 0, 0);

    if (sys->Unshareflags != 0) {
        CHILD_CALL(SYS_unshare, static_cast<long>(sys->Unshareflags), 0, 0, 0, 0, 0);
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
        if (!sys->Credential->NoSetGroups) {
            CHILD_CALL(SYS_setgroups, a->ngroups, reinterpret_cast<long>(a->groups), 0, 0, 0, 0);
        }
        CHILD_CALL(SYS_setgid, sys->Credential->Gid, 0, 0, 0, 0, 0);
        CHILD_CALL(SYS_setuid, sys->Credential->Uid, 0, 0, 0, 0, 0);
    }

    if (a->dir) CHILD_CALL(SYS_chdir, reinterpret_cast<long>(a->dir), 0, 0, 0, 0, 0);

    // Parent death signal.
    if (sys->Pdeathsig.value != 0) {
        CHILD_CALL(SYS_prctl, PR_SET_PDEATHSIG, sys->Pdeathsig.value, 0, 0, 0, 0);
        // The parent may have died already.
        if (raw_syscall(SYS_getppid, 0, 0, 0, 0, 0, 0) != a->ppid) {
            long pid = raw_syscall(SYS_getpid, 0, 0, 0, 0, 0, 0);
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
            raw_syscall(SYS_close, i, 0, 0, 0, 0, 0);
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
    for (int i = a->nfd; i < 3; i++) raw_syscall(SYS_close, i, 0, 0, 0, 0, 0);

    if (sys->Noctty) CHILD_CALL(SYS_ioctl, 0, TIOCNOTTY, 0, 0, 0, 0);
    if (sys->Setctty) CHILD_CALL(SYS_ioctl, sys->Ctty, TIOCSCTTY, 1, 0, 0, 0);

    // Tracing last, so that the setup above isn't traced.
    if (sys->Ptrace) CHILD_CALL(SYS_ptrace, PTRACE_TRACEME, 0, 0, 0, 0, 0);

    r = raw_syscall(SYS_execve, reinterpret_cast<long>(a->argv0), reinterpret_cast<long>(a->argv),
                    reinterpret_cast<long>(a->envv), 0, 0, 0);

childerror:
#undef CHILD_CALL
    err1 = static_cast<int>(-r);
    raw_syscall(SYS_write, pipe, reinterpret_cast<long>(&err1), sizeof(err1), 0, 0, 0);
    for (;;) raw_syscall(SYS_exit, 253, 0, 0, 0, 0, 0);
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

const SysProcAttr zero_sys_proc_attr{};

// Makes a child running child_main(args) with clone(2); returns its pid,
// or -errno. Signals are blocked around it.
long clone_child(child_args& args, int flags, int* pidfd) {
    void* stack = ::mmap(nullptr, child_stack_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK,
                         -1, 0);
    if (stack == MAP_FAILED) return -errno;
    sigset_t all, old;
    sigfillset(&all);
    long pid;
    {
        // The thread is suspended until the child execs: a blocking call.
        // Go's runtime_BeforeFork blocks the signals.
        runtime::blocking_call b;
        pthread_sigmask(SIG_SETMASK, &all, &old);
        args.oldmask = &old;
        pid = ::clone(child_main, static_cast<char*>(stack) + child_stack_size, flags, &args, pidfd, nullptr, nullptr);
        if (pid < 0) pid = -errno;
        pthread_sigmask(SIG_SETMASK, &old, nullptr);
    }
    ::munmap(stack, child_stack_size);
    return pid;
}

}  // namespace

namespace detail {

bool check_clone_pidfd() {
    // A child that exits at once, without an exit signal, as in Go: a
    // SIGCHLD would reach a program that notifies SIGCHLD.
    int pidfd = -1;
    child_args args{};
    void* stack = ::mmap(nullptr, child_stack_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK,
                         -1, 0);
    if (stack == MAP_FAILED) return false;
    sigset_t all, old;
    sigfillset(&all);
    pthread_sigmask(SIG_SETMASK, &all, &old);
    long pid = ::clone(
        [](void*) -> int {
            for (;;) raw_syscall(SYS_exit_group, 0, 0, 0, 0, 0, 0);
        },
        static_cast<char*>(stack) + child_stack_size, CLONE_VM | CLONE_VFORK | CLONE_PIDFD, &args, &pidfd,
        nullptr, nullptr);
    pthread_sigmask(SIG_SETMASK, &old, nullptr);
    ::munmap(stack, child_stack_size);
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
    static const ProcAttr zero_attr{};
    if (!attr) attr = &zero_attr;
    const SysProcAttr* sys = attr->Sys ? attr->Sys : &zero_sys_proc_attr;

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

    int flags = static_cast<int>(sys->Cloneflags);
    if (((sys->Cloneflags | sys->Unshareflags) & CLONE_NEWUSER_) == 0) flags |= CLONE_VFORK | CLONE_VM;
    if (sys->PidFD) flags |= CLONE_PIDFD;
    flags |= SIGCHLD;

    child_args args{};
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

    ForkLock.Lock();
    // The pipe the child reports an error on; exec closes it.
    int p[2];
    if (::pipe2(p, O_CLOEXEC) < 0) {
        int e = errno;
        ForkLock.Unlock();
        return {0, errno_error(e)};
    }
    args.pipe = p[1];
    int pidfd = -1;
    long pid = clone_child(args, flags, &pidfd);
    ForkLock.Unlock();
    ::close(p[1]);
    if (pid < 0) {
        ::close(p[0]);
        return {0, errno_error(static_cast<int>(-pid))};
    }

    // The child's error, or EOF if exec closed the pipe. Without
    // CLONE_VFORK the child may still be running: a blocking call.
    int err1 = 0;
    ssize_t n;
    {
        runtime::blocking_call b;
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
        if (n == static_cast<ssize_t>(sizeof(err1))) err = errno_error(err1);
        else if (rerr) err = errno_error(rerr);
        else err = EPIPE_;
        // The child failed: reap it, so that zombies don't pile up.
        int status;
        {
            runtime::blocking_call b;
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
