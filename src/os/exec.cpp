// os.Process and os.ProcessState, after Go's os/exec.go, exec_posix.go,
// exec_unix.go, pidfd_linux.go and wait_waitid.go.
//
// A Process holds a pidfd when the kernel has them (Linux 5.4). Wait then
// waits for the pidfd to be readable through the network poller, which
// parks only the calling fiber, and reaps the child with waitid(P_PIDFD,
// WNOHANG). Without pidfds, Wait blocks its thread in waitid, as a
// blocking call whose thread hands its processor to another, as Go does.
#include "internal.hpp"

#include "../syscall/exec_linux.hpp"

#include <gopp/os.hpp>
#include <gopp/runtime.hpp>
#include <gopp/sync.hpp>
#include <gopp/syscall.hpp>

#include <fcntl.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <string>

#ifndef P_PIDFD
#define P_PIDFD 3
#endif

namespace gopp::os {

const error ErrProcessDone = errors::New("os: process already finished");
const error ErrNoHandle = errors::New("os: process handle unavailable");

namespace {

const error errProcessReleased = errors::New("os: process already released");

// Special values of Process::Pid.
constexpr int pidUnset = 0;
constexpr int pidReleased = -1;

enum processStatus : uint32 { statusOk, statusDone, statusReleased };

long sysPidfdOpen(int pid, unsigned flags) { return ::syscall(SYS_pidfd_open, pid, flags); }

long sysPidfdSendSignal(int fd, int sig) { return ::syscall(SYS_pidfd_send_signal, fd, sig, nullptr, 0); }

// waitid(2) with the rusage argument the C library's lacks.
long sysWaitid(int idtype, int id, siginfo_t* info, int options, struct rusage* ru) {
    return ::syscall(SYS_waitid, idtype, id, info, options, ru);
}

error convertEsrch(error err) { return err == syscall::ESRCH_ ? ErrProcessDone : err; }

// The WaitStatus of a siginfo from waitid (Go's SiginfoChild.WaitStatus).
syscall::WaitStatus waitStatus(const siginfo_t& info) {
    uint32 st = static_cast<uint32>(info.si_status);
    switch (info.si_code) {
    case CLD_EXITED: return syscall::WaitStatus(st << 8);
    case CLD_DUMPED: return syscall::WaitStatus(st | 0x80);
    case CLD_KILLED: return syscall::WaitStatus(st);
    case CLD_TRAPPED:
    case CLD_STOPPED: return syscall::WaitStatus((st << 8) | 0x7f);
    case CLD_CONTINUED: return syscall::WaitStatus(0xffff);
    default: return syscall::WaitStatus();
    }
}

syscall::Rusage toRusage(const struct rusage& ru) {
    syscall::Rusage r;
    r.Utime = {ru.ru_utime.tv_sec, ru.ru_utime.tv_usec};
    r.Stime = {ru.ru_stime.tv_sec, ru.ru_stime.tv_usec};
    r.Maxrss = ru.ru_maxrss;
    r.Ixrss = ru.ru_ixrss;
    r.Idrss = ru.ru_idrss;
    r.Isrss = ru.ru_isrss;
    r.Minflt = ru.ru_minflt;
    r.Majflt = ru.ru_majflt;
    r.Nswap = ru.ru_nswap;
    r.Inblock = ru.ru_inblock;
    r.Oublock = ru.ru_oublock;
    r.Msgsnd = ru.ru_msgsnd;
    r.Msgrcv = ru.ru_msgrcv;
    r.Nsignals = ru.ru_nsignals;
    r.Nvcsw = ru.ru_nvcsw;
    r.Nivcsw = ru.ru_nivcsw;
    return r;
}

// Go's checkPidfd.
bool checkPidfd() {
    long fd = sysPidfdOpen(::getpid(), 0);
    if (fd < 0) return false;
    bool ok = true;
    // waitid(P_PIDFD) of this process: ECHILD, as it's not its own child.
    siginfo_t info{};
    long r;
    do {
        r = sysWaitid(P_PIDFD, static_cast<int>(fd), &info, WEXITED, nullptr);
    } while (r < 0 && errno == EINTR);
    if (r == 0 || errno != ECHILD) ok = false;
    if (ok && sysPidfdSendSignal(static_cast<int>(fd), 0) < 0) ok = false;
    ::close(static_cast<int>(fd));
    return ok && syscall::detail::checkClonePidfd();
}

}  // namespace

namespace detail {

std::atomic<bool> pidfdDisabled{false};

bool pidfdWorks() {
    static const bool works = checkPidfd();
    return works && !pidfdDisabled.load(std::memory_order_relaxed);
}

// Go's processHandle: a pidfd, closed when the last reference goes.
struct processHandle {
    explicit processHandle(int fd) : pfd(fd) {
        // epoll reports a pidfd readable when its process exits. If it
        // can't be polled, Wait blocks in waitid instead.
        pfd.Init(true);
    }

    poll::FD pfd;
    std::atomic<int32> refs{1};

    bool acquire() noexcept {
        int32 r = refs.load(std::memory_order_acquire);
        for (;;) {
            if (r < 0) panic("internal error: negative process handle reference count");
            if (r == 0) return false;
            if (refs.compare_exchange_weak(r, r + 1, std::memory_order_acq_rel)) return true;
        }
    }

    void release() noexcept {
        int32 r = refs.fetch_sub(1, std::memory_order_acq_rel);
        if (r <= 0) panic("internal error: too many releases of process handle");
        if (r == 1) {
            pfd.Close();
            delete this;
        }
    }
};

struct processImpl {
    std::atomic<uint32> state{statusOk};
    sync::RWMutex sigMu;              // orders Signal before a pid-based Wait
    processHandle* handle = nullptr;  // nullptr: the pid is used

    // Go's doRelease: sets the status if it's still OK, dropping the
    // handle's reference; returns the earlier status.
    uint32 doRelease(uint32 newStatus) noexcept {
        uint32 s = state.load(std::memory_order_acquire);
        for (;;) {
            if (s != statusOk) return s;
            if (state.compare_exchange_weak(s, newStatus, std::memory_order_acq_rel)) break;
        }
        if (handle) handle->release();
        return statusOk;
    }

    // Go's handleTransientAcquire: the handle with a reference, or the
    // status that keeps it from being used.
    std::pair<processHandle*, uint32> transientAcquire() noexcept {
        uint32 s = state.load(std::memory_order_acquire);
        if (s != statusOk) return {nullptr, s};
        if (handle->acquire()) return {handle, statusOk};
        s = state.load(std::memory_order_acquire);
        if (s == statusOk) panic("inconsistent process status");
        return {nullptr, s};
    }

    static ptr<Process> newPidProcess(int pid) {
        return make_ptr<Process>(pid, std::make_unique<processImpl>());
    }

    static ptr<Process> newHandleProcess(int pid, int fd) {
        auto impl = std::make_unique<processImpl>();
        impl->handle = new processHandle(fd);
        return make_ptr<Process>(pid, std::move(impl));
    }

    static ptr<Process> newDoneProcess(int pid) {
        auto impl = std::make_unique<processImpl>();
        impl->state.store(statusDone, std::memory_order_relaxed);
        return make_ptr<Process>(pid, std::move(impl));
    }

    // Go's pidfdWait.
    static std::pair<ptr<ProcessState>, error> pidfdWait(processImpl& p) {
        auto [h, status] = p.transientAcquire();
        if (status == statusDone) {
            // Waited for already, or not found by FindProcess: ECHILD, as
            // waitid would say.
            return {nullptr, NewSyscallError("wait", syscall::ECHILD_)};
        }
        if (status == statusReleased) return {nullptr, syscall::EINVAL_};

        siginfo_t info{};
        struct rusage ru{};
        int e = 0;
        if (h->pfd.pollable()) {
            error err = h->pfd.RawRead([&](int fd) {
                for (;;) {
                    std::memset(&info, 0, sizeof(info));
                    if (sysWaitid(P_PIDFD, fd, &info, WEXITED | WNOHANG, &ru) == 0) {
                        // si_pid is 0 while the process runs.
                        return info.si_pid != 0;
                    }
                    if (errno == EINTR) continue;
                    e = errno;
                    return true;
                }
            });
            if (err && !e) {
                h->release();
                return {nullptr, NewSyscallError("waitid", err)};
            }
        } else {
            runtime::blockingCall b;
            while (sysWaitid(P_PIDFD, h->pfd.Sysfd(), &info, WEXITED, &ru) < 0) {
                if (errno != EINTR) {
                    e = errno;
                    break;
                }
            }
        }
        h->release();
        if (e) return {nullptr, NewSyscallError("waitid", syscall::errnoError(e))};
        p.doRelease(statusDone);
        return {make_ptr<ProcessState>(static_cast<int>(info.si_pid), waitStatus(info), toRusage(ru)), nullptr};
    }

    // Go's pidWait, with blockUntilWaitable: waits until the process can
    // be reaped, then marks it done before reaping, so that Signal can't
    // signal a recycled pid.
    static std::pair<ptr<ProcessState>, error> pidWait(processImpl& p, int pid) {
        if (p.state.load(std::memory_order_acquire) == statusReleased) return {nullptr, syscall::EINVAL_};
        bool ready = false;
        {
            runtime::blockingCall b;
            siginfo_t info{};
            long r;
            do {
                r = sysWaitid(P_PID, pid, &info, WEXITED | WNOWAIT, nullptr);
            } while (r < 0 && errno == EINTR);
            if (r == 0) {
                ready = true;
            } else if (errno != ENOSYS) {
                int e = errno;
                return {nullptr, NewSyscallError("waitid", syscall::errnoError(e))};
            }
        }
        if (ready) {
            p.doRelease(statusDone);
            // Wait for a Signal in progress.
            p.sigMu.Lock();
            p.sigMu.Unlock();
        }
        int status = 0;
        struct rusage ru{};
        pid_t pid1;
        {
            runtime::blockingCall b;
            do {
                pid1 = ::wait4(static_cast<pid_t>(pid), &status, 0, &ru);
            } while (pid1 < 0 && errno == EINTR);
        }
        if (pid1 < 0) {
            int e = errno;
            return {nullptr, NewSyscallError("wait", syscall::errnoError(e))};
        }
        p.doRelease(statusDone);
        return {make_ptr<ProcessState>(static_cast<int>(pid1), syscall::WaitStatus(static_cast<uint32>(status)),
                                       toRusage(ru)),
                nullptr};
    }
};

}  // namespace detail

Process::Process(int pid, std::unique_ptr<detail::processImpl> impl) noexcept : Pid(pid), p_(std::move(impl)) {}

Process::~Process() {
    // Go's cleanup: the handle's reference goes with the Process.
    if (p_) p_->doRelease(statusReleased);
}

error Process::Release() {
    // For historical reasons, Release sets Pid to -1 on Unix.
    Pid = pidReleased;
    p_->doRelease(statusReleased);
    return nullptr;
}

error Process::Kill() { return Signal(os::Kill); }

std::pair<ptr<ProcessState>, error> Process::Wait() {
    if (p_->handle) return detail::processImpl::pidfdWait(*p_);
    return detail::processImpl::pidWait(*p_, Pid);
}

error Process::Signal(const gopp::os::Signal& sig) {
    auto [s, ok] = sig.asSyscall();
    if (!ok) return errors::New("os: unsupported signal type");
    if (p_->handle) {
        // Go's pidfdSendSignal.
        auto [h, status] = p_->transientAcquire();
        if (status == statusDone) return ErrProcessDone;
        if (status == statusReleased) return errProcessReleased;
        long r = sysPidfdSendSignal(h->pfd.Sysfd(), s.value);
        int e = errno;
        h->release();
        if (r < 0) return convertEsrch(syscall::errnoError(e));
        return nullptr;
    }
    // Go's pidSignal.
    if (Pid == pidReleased) return errProcessReleased;
    if (Pid == pidUnset) return errors::New("os: process not initialized");
    p_->sigMu.RLock();
    error err;
    switch (p_->state.load(std::memory_order_acquire)) {
    case statusDone: err = ErrProcessDone; break;
    case statusReleased: err = errProcessReleased; break;
    default: err = convertEsrch(syscall::Kill(Pid, s)); break;
    }
    p_->sigMu.RUnlock();
    return err;
}

error Process::WithHandle(const std::function<void(uintptr)>& f) {
    if (!p_->handle) return ErrNoHandle;
    auto [h, status] = p_->transientAcquire();
    if (status == statusDone) return ErrProcessDone;
    if (status == statusReleased) return errProcessReleased;
    f(static_cast<uintptr>(h->pfd.Sysfd()));
    h->release();
    return nullptr;
}

string ProcessState::String() const {
    std::string res;
    syscall::WaitStatus st = status_;
    if (st.Exited()) {
        res = "exit status " + std::to_string(st.ExitStatus());
    } else if (st.Signaled()) {
        res = "signal: " + st.Signal().String().str();
    } else if (st.Stopped()) {
        res = "stop signal: " + st.StopSignal().String().str();
        if (st.StopSignal() == syscall::SIGTRAP_ && st.TrapCause() != 0) {
            res += " (trap " + std::to_string(st.TrapCause()) + ")";
        }
    } else if (st.Continued()) {
        res = "continued";
    }
    if (st.CoreDump()) res += " (core dumped)";
    return string(res);
}

string Signal::String() const {
    if (sys_) return syscall::Signal(num_).String();
    if (p_) return p_->String();
    return string::fromStatic("<nil>");
}

std::pair<ptr<Process>, error> StartProcess(std::string_view name, std::span<const string> argv, const ProcAttr& attr) {
    // Without SysProcAttr (no Chroot or credentials), check that the
    // directory exists, for a clearer error.
    if (!attr.Sys && !attr.Dir.empty()) {
        if (auto [_, err] = Stat(attr.Dir); err) {
            if (auto* pe = dynamic_cast<const PathError*>(err.get())) {
                return {nullptr, detail::pathError("chdir", pe->Path.view(), pe->Err)};
            }
            return {nullptr, err};
        }
    }

    // Go's ensurePidfd: ask for a pidfd, unless the caller did; then it's
    // the caller's, and the Process gets a duplicate.
    std::optional<syscall::SysProcAttr> sys = attr.Sys;
    bool works = detail::pidfdWorks();
    bool shouldDup = false;
    int pidfd = -1;
    if (works) {
        if (!sys) sys.emplace();
        if (!sys->PidFD) sys->PidFD = &pidfd;
        else shouldDup = true;
    }

    syscall::ProcAttr sysattr;
    sysattr.Dir = attr.Dir;
    sysattr.Env = attr.Env.isNil() ? Environ() : attr.Env;
    sysattr.Sys = sys ? &*sys : nullptr;
    sysattr.Files.reserve(attr.Files.size());
    for (const ptr<File>& f : attr.Files) sysattr.Files.push_back(f ? f->Fd() : ~uintptr(0));

    auto [pid, err] = syscall::ForkExec(name, argv, &sysattr);
    if (err) return {nullptr, detail::pathError("fork/exec", name, err)};

    if (works) {
        int h = *sys->PidFD;
        if (shouldDup) h = ::fcntl(h, F_DUPFD_CLOEXEC, 0);
        if (h >= 0) return {detail::processImpl::newHandleProcess(pid, h), nullptr};
    }
    return {detail::processImpl::newPidProcess(pid), nullptr};
}

std::pair<ptr<Process>, error> FindProcess(int pid) {
    // Go's pidfdFind.
    if (detail::pidfdWorks()) {
        long h = sysPidfdOpen(pid, 0);
        if (h >= 0) return {detail::processImpl::newHandleProcess(pid, static_cast<int>(h)), nullptr};
        // A process that doesn't exist is done: Signal and Wait say so.
        // Other errors fall back to the pid.
        if (errno == ESRCH) return {detail::processImpl::newDoneProcess(pid), nullptr};
    }
    return {detail::processImpl::newPidProcess(pid), nullptr};
}

}  // namespace gopp::os
