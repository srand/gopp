// The syscall package: the errors of system calls, signals, wait statuses
// and the attributes of new processes (Linux's), and the socket control
// messages of Unix domain sockets (Go's sockcmsg_unix.go).
//
// Go's names of errno and signal constants are C macros, so they carry a
// trailing _ here, as io::EOF_ does: syscall::SIGTERM_, syscall::ECHILD_.
#pragma once

// The module interfaces (modules/) import these instead.
#ifndef GOPP_MODULE
#include <gopp/base.hpp>
#include <gopp/errors.hpp>
#include <gopp/slice.hpp>
#include <gopp/string.hpp>
#include <gopp/sync.hpp>

#include <array>
#include <cstdint>
#include <format>
#include <functional>
#include <initializer_list>
#include <optional>
#include <ostream>
#include <span>
#include <string_view>
#include <utility>
#include <vector>
#endif

namespace gopp::syscall {

// syscall.Errno: an errno from a system call. errors::Is matches it with
// the fs errors: ENOENT is fs::ErrNotExist, EEXIST and ENOTEMPTY are
// fs::ErrExist, and EACCES and EPERM are fs::ErrPermission; and with
// another Errno of the same code, such as syscall::ECHILD_. Compare codes
// with errors::As<syscall::Errno>(err)->Code.
struct Errno : error_interface {
    int Code;
    explicit Errno(int c) : Code(c) {}
    string Error() const override;
    bool Timeout() const noexcept override;
    bool Is(const error_interface* target) const noexcept override;
};

// An Errno error for code. The codes Linux defines have one error each,
// so that err == syscall::ECHILD_ holds for an error made from ECHILD.
gopp::error errno_error(int code);

// syscall.Cmsghdr: the header of a socket control message.
struct Cmsghdr {
    uint64_t Len = 0;
    int32_t Level = 0;
    int32_t Type = 0;
};

// syscall.SocketControlMessage
struct SocketControlMessage {
    Cmsghdr Header;
    slice<byte> Data;
};

// The length of a control message with datalen bytes of data, and the
// room it takes in a buffer (CMSG_LEN and CMSG_SPACE).
int CmsgLen(int datalen);
int CmsgSpace(int datalen);

// syscall.ParseSocketControlMessage: the messages in the out-of-band data
// of a ReadMsgUnix; EINVAL if they're malformed.
std::pair<std::vector<SocketControlMessage>, gopp::error> ParseSocketControlMessage(std::span<const byte> b);

// syscall.UnixRights: an SCM_RIGHTS message passing the descriptors fds,
// for WriteMsgUnix.
slice<byte> UnixRights(std::initializer_list<int> fds);

// syscall.ParseUnixRights: the descriptors of an SCM_RIGHTS message.
std::pair<std::vector<int>, gopp::error> ParseUnixRights(const SocketControlMessage& m);

// syscall.Socketpair: a connected pair of sockets, as socketpair(2).
std::pair<std::array<int, 2>, gopp::error> Socketpair(int domain, int typ, int proto);

// syscall.RawConn: access to a descriptor (the SyscallConn of net's
// connections and os::File, and Dialer::Control).
struct RawConn : public virtual object {
    // Calls f with the descriptor, which stays open meanwhile.
    virtual gopp::error Control(std::function<void(uintptr)> f) = 0;
    // Calls f with the descriptor until it returns true; after each false
    // it waits until the descriptor is readable.
    virtual gopp::error Read(std::function<bool(uintptr)> f) = 0;
    // Likewise, waiting until it's writable.
    virtual gopp::error Write(std::function<bool(uintptr)> f) = 0;
};

// syscall.Conn: something with a descriptor (net's connections and
// listeners, os::File).
struct Conn : public virtual object {
    virtual std::pair<ptr<RawConn>, gopp::error> SyscallConn() = 0;
};

// Go's errno constants, as errors (Linux's codes).
extern const error E2BIG_;
extern const error EACCES_;
extern const error EAGAIN_;
extern const error EBADF_;
extern const error ECHILD_;
extern const error EEXIST_;
extern const error EINTR_;
extern const error EINVAL_;
extern const error EISDIR_;
extern const error ENOENT_;
extern const error ENOEXEC_;
extern const error ENOSYS_;
extern const error ENOTDIR_;
extern const error EPERM_;
extern const error EPIPE_;
extern const error ESRCH_;

// syscall.Signal: a signal number. Its String is Linux's description:
// "interrupt", "terminated", or "signal 99" for others. It converts to
// os::Signal.
struct Signal {
    int value = 0;

    constexpr Signal() noexcept = default;
    constexpr explicit Signal(int v) noexcept : value(v) {}
    constexpr explicit operator int() const noexcept { return value; }

    string String() const;

    friend constexpr bool operator==(Signal a, Signal b) noexcept { return a.value == b.value; }
    friend std::ostream& operator<<(std::ostream& os, Signal s) { return os << s.String(); }
};

inline constexpr Signal SIGABRT_{0x6};
inline constexpr Signal SIGALRM_{0xe};
inline constexpr Signal SIGBUS_{0x7};
inline constexpr Signal SIGCHLD_{0x11};
inline constexpr Signal SIGCLD_{0x11};
inline constexpr Signal SIGCONT_{0x12};
inline constexpr Signal SIGFPE_{0x8};
inline constexpr Signal SIGHUP_{0x1};
inline constexpr Signal SIGILL_{0x4};
inline constexpr Signal SIGINT_{0x2};
inline constexpr Signal SIGIO_{0x1d};
inline constexpr Signal SIGIOT_{0x6};
inline constexpr Signal SIGKILL_{0x9};
inline constexpr Signal SIGPIPE_{0xd};
inline constexpr Signal SIGPOLL_{0x1d};
inline constexpr Signal SIGPROF_{0x1b};
inline constexpr Signal SIGPWR_{0x1e};
inline constexpr Signal SIGQUIT_{0x3};
inline constexpr Signal SIGSEGV_{0xb};
inline constexpr Signal SIGSTKFLT_{0x10};
inline constexpr Signal SIGSTOP_{0x13};
inline constexpr Signal SIGSYS_{0x1f};
inline constexpr Signal SIGTERM_{0xf};
inline constexpr Signal SIGTRAP_{0x5};
inline constexpr Signal SIGTSTP_{0x14};
inline constexpr Signal SIGTTIN_{0x15};
inline constexpr Signal SIGTTOU_{0x16};
inline constexpr Signal SIGUNUSED_{0x1f};
inline constexpr Signal SIGURG_{0x17};
inline constexpr Signal SIGUSR1_{0xa};
inline constexpr Signal SIGUSR2_{0xc};
inline constexpr Signal SIGVTALRM_{0x1a};
inline constexpr Signal SIGWINCH_{0x1c};
inline constexpr Signal SIGXCPU_{0x18};
inline constexpr Signal SIGXFSZ_{0x19};

// syscall.WaitStatus: a status of wait(2).
struct WaitStatus {
    uint32 value = 0;

    constexpr WaitStatus() noexcept = default;
    constexpr explicit WaitStatus(uint32 v) noexcept : value(v) {}

    constexpr bool Exited() const noexcept { return (value & 0x7f) == 0; }
    constexpr bool Signaled() const noexcept { return (value & 0x7f) != 0x7f && (value & 0x7f) != 0; }
    constexpr bool Stopped() const noexcept { return (value & 0xff) == 0x7f; }
    constexpr bool Continued() const noexcept { return value == 0xffff; }
    constexpr bool CoreDump() const noexcept { return Signaled() && (value & 0x80) != 0; }
    // The exit code, or -1 if the process didn't exit.
    constexpr int ExitStatus() const noexcept { return Exited() ? static_cast<int>((value >> 8) & 0xff) : -1; }
    // The signal that ended the process, or -1.
    constexpr syscall::Signal Signal() const noexcept {
        return syscall::Signal(Signaled() ? static_cast<int>(value & 0x7f) : -1);
    }
    // The signal that stopped the process, or -1.
    constexpr syscall::Signal StopSignal() const noexcept {
        return syscall::Signal(Stopped() ? static_cast<int>((value >> 8) & 0xff) : -1);
    }
    constexpr int TrapCause() const noexcept {
        return StopSignal() == SIGTRAP_ ? static_cast<int>((value >> 8) >> 8) : -1;
    }

    friend constexpr bool operator==(WaitStatus a, WaitStatus b) noexcept { return a.value == b.value; }
};

// syscall.Timeval
struct Timeval {
    int64 Sec = 0;
    int64 Usec = 0;
    constexpr int64 Nano() const noexcept { return Sec * 1000000000 + Usec * 1000; }
};

// syscall.Rusage: struct rusage of getrusage(2), on 64-bit Linux.
struct Rusage {
    Timeval Utime;
    Timeval Stime;
    int64 Maxrss = 0;
    int64 Ixrss = 0;
    int64 Idrss = 0;
    int64 Isrss = 0;
    int64 Minflt = 0;
    int64 Majflt = 0;
    int64 Nswap = 0;
    int64 Inblock = 0;
    int64 Oublock = 0;
    int64 Msgsnd = 0;
    int64 Msgrcv = 0;
    int64 Nsignals = 0;
    int64 Nvcsw = 0;
    int64 Nivcsw = 0;
};

// The flags of clone(2) and unshare(2), for SysProcAttr's Cloneflags and
// Unshareflags.
inline constexpr uintptr CLONE_VM_ = 0x00000100;
inline constexpr uintptr CLONE_FS_ = 0x00000200;
inline constexpr uintptr CLONE_FILES_ = 0x00000400;
inline constexpr uintptr CLONE_SIGHAND_ = 0x00000800;
inline constexpr uintptr CLONE_PIDFD_ = 0x00001000;
inline constexpr uintptr CLONE_PTRACE_ = 0x00002000;
inline constexpr uintptr CLONE_VFORK_ = 0x00004000;
inline constexpr uintptr CLONE_PARENT_ = 0x00008000;
inline constexpr uintptr CLONE_THREAD_ = 0x00010000;
inline constexpr uintptr CLONE_NEWNS_ = 0x00020000;
inline constexpr uintptr CLONE_SYSVSEM_ = 0x00040000;
inline constexpr uintptr CLONE_NEWCGROUP_ = 0x02000000;
inline constexpr uintptr CLONE_NEWUTS_ = 0x04000000;
inline constexpr uintptr CLONE_NEWIPC_ = 0x08000000;
inline constexpr uintptr CLONE_NEWUSER_ = 0x10000000;
inline constexpr uintptr CLONE_NEWPID_ = 0x20000000;
inline constexpr uintptr CLONE_NEWNET_ = 0x40000000;
inline constexpr uintptr CLONE_IO_ = 0x80000000;

// syscall.Credential: the user and groups a new process runs as.
struct Credential {
    uint32 Uid = 0;
    uint32 Gid = 0;
    std::vector<uint32> Groups{};  // supplementary groups
    bool NoSetGroups = false;    // don't set the supplementary groups
};

// syscall.SysProcAttr: Linux's attributes of a new process. Go's user
// namespace mappings (UidMappings, GidMappings), AmbientCaps and
// UseCgroupFD aren't supported.
struct SysProcAttr {
    string Chroot{};
    std::optional<syscall::Credential> Credential{};
    // Ptrace makes the child call ptrace(PTRACE_TRACEME).
    bool Ptrace = false;
    bool Setsid = false;  // create a session
    // Setpgid sets the process group of the child to Pgid, or, if Pgid
    // is 0, to the child's pid.
    bool Setpgid = false;
    // Setctty makes Ctty, a descriptor of the child (an index into
    // ProcAttr's Files), its controlling terminal. Only with Setsid.
    bool Setctty = false;
    bool Noctty = false;  // detach descriptor 0 from the controlling terminal
    int Ctty = 0;
    // Foreground puts the child's process group in the foreground of the
    // terminal Ctty, a descriptor of the parent. Implies Setpgid.
    bool Foreground = false;
    int Pgid = 0;
    // Pdeathsig: the signal the child gets when the thread that started
    // it ends. The runtime's threads don't end, so in practice that's when
    // this process ends.
    syscall::Signal Pdeathsig;
    uintptr Cloneflags = 0;    // more flags for clone(2)
    uintptr Unshareflags = 0;  // flags for unshare(2), in the child
    // PidFD, if not null, receives the pidfd of the child, or -1.
    int* PidFD = nullptr;
};

// syscall.ProcAttr: the attributes of ForkExec.
struct ProcAttr {
    string Dir{};                // the working directory, if not empty
    slice<string> Env{};         // the environment
    std::vector<uintptr> Files{};  // descriptors 0, 1, 2, ...; ~uintptr(0) closes one
    const SysProcAttr* Sys = nullptr;
};

// syscall.ForkExec: starts the program argv0 with the arguments argv, and
// returns its pid. The child is made with clone(CLONE_VM|CLONE_VFORK), on
// a stack of its own, and makes only system calls until it execs; an
// error it meets on the way comes back through a close-on-exec pipe, as
// in Go. If attr->Sys->PidFD is set, it receives a pidfd of the child.
std::pair<int, gopp::error> ForkExec(std::string_view argv0, std::span<const string> argv, const ProcAttr* attr);

// syscall.ForkLock: held for writing while a process is started, so that
// a descriptor made without close-on-exec can be marked before a child
// inherits it. go++ makes its descriptors close-on-exec when it makes
// them; code that can't takes the lock for reading.
extern sync::RWMutex ForkLock;

// syscall.Getpid and friends.
int Getpid() noexcept;
int Getppid() noexcept;
int Getuid() noexcept;
int Geteuid() noexcept;
int Getgid() noexcept;
int Getegid() noexcept;

// syscall.Kill: sends sig to the process pid (or group, if negative).
gopp::error Kill(int pid, Signal sig);

}  // namespace gopp::syscall

template <>
struct std::formatter<gopp::syscall::Signal> : std::formatter<std::string_view> {
    auto format(gopp::syscall::Signal s, auto& ctx) const {
        return std::formatter<std::string_view>::format(s.String().view(), ctx);
    }
};
