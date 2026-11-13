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
constexpr int pid_unset = 0;
constexpr int pid_released = -1;

enum process_status : uint32 { status_ok, status_done, status_released };

long sys_pidfd_open(int pid, unsigned flags) { return ::syscall(SYS_pidfd_open, pid, flags); }

long sys_pidfd_send_signal(int fd, int sig) { return ::syscall(SYS_pidfd_send_signal, fd, sig, nullptr, 0); }

// waitid(2) with the rusage argument the C library's lacks.
long sys_waitid(int idtype, int id, siginfo_t* info, int options, struct rusage* ru) {
    return ::syscall(SYS_waitid, idtype, id, info, options, ru);
}

error convert_esrch(error err) { return err == syscall::ESRCH_ ? ErrProcessDone : err; }

// The WaitStatus of a siginfo from waitid (Go's SiginfoChild.WaitStatus).
syscall::WaitStatus wait_status(const siginfo_t& info) {
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

syscall::Rusage to_rusage(const struct rusage& ru) {
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
bool check_pidfd() {
    long fd = sys_pidfd_open(::getpid(), 0);
    if (fd < 0) return false;
    bool ok = true;
    // waitid(P_PIDFD) of this process: ECHILD, as it's not its own child.
    siginfo_t info{};
    long r;
    do {
        r = sys_waitid(P_PIDFD, static_cast<int>(fd), &info, WEXITED, nullptr);
    } while (r < 0 && errno == EINTR);
    if (r == 0 || errno != ECHILD) ok = false;
    if (ok && sys_pidfd_send_signal(static_cast<int>(fd), 0) < 0) ok = false;
    ::close(static_cast<int>(fd));
    return ok && syscall::detail::check_clone_pidfd();
}

}  // namespace

namespace detail {

std::atomic<bool> pidfd_disabled{false};

bool pidfd_works() {
    static const bool works = check_pidfd();
    return works && !pidfd_disabled.load(std::memory_order_relaxed);
}

// Go's processHandle: a pidfd, closed when the last reference goes.
struct process_handle {
    explicit process_handle(int fd) : pfd(fd) {
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

struct process_impl {
    std::atomic<uint32> state{status_ok};
    sync::RWMutex sig_mu;              // orders Signal before a pid-based Wait
    process_handle* handle = nullptr;  // nullptr: the pid is used

    // Go's doRelease: sets the status if it's still OK, dropping the
    // handle's reference; returns the earlier status.
    uint32 do_release(uint32 new_status) noexcept {
        uint32 s = state.load(std::memory_order_acquire);
        for (;;) {
            if (s != status_ok) return s;
            if (state.compare_exchange_weak(s, new_status, std::memory_order_acq_rel)) break;
        }
        if (handle) handle->release();
        return status_ok;
    }

    // Go's handleTransientAcquire: the handle with a reference, or the
    // status that keeps it from being used.
    std::pair<process_handle*, uint32> transient_acquire() noexcept {
        uint32 s = state.load(std::memory_order_acquire);
        if (s != status_ok) return {nullptr, s};
        if (handle->acquire()) return {handle, status_ok};
        s = state.load(std::memory_order_acquire);
        if (s == status_ok) panic("inconsistent process status");
        return {nullptr, s};
    }

    static ptr<Process> new_pid_process(int pid) {
        return make_ptr<Process>(pid, std::make_unique<process_impl>());
    }

    static ptr<Process> new_handle_process(int pid, int fd) {
        auto impl = std::make_unique<process_impl>();
        impl->handle = new process_handle(fd);
        return make_ptr<Process>(pid, std::move(impl));
    }

    static ptr<Process> new_done_process(int pid) {
        auto impl = std::make_unique<process_impl>();
        impl->state.store(status_done, std::memory_order_relaxed);
        return make_ptr<Process>(pid, std::move(impl));
    }

    // Go's pidfdWait.
    static std::pair<ptr<ProcessState>, error> pidfd_wait(process_impl& p) {
        auto [h, status] = p.transient_acquire();
        if (status == status_done) {
            // Waited for already, or not found by FindProcess: ECHILD, as
            // waitid would say.
            return {nullptr, NewSyscallError("wait", syscall::ECHILD_)};
        }
        if (status == status_released) return {nullptr, syscall::EINVAL_};

        siginfo_t info{};
        struct rusage ru{};
        int e = 0;
        if (h->pfd.pollable()) {
            error err = h->pfd.RawRead([&](int fd) {
                for (;;) {
                    std::memset(&info, 0, sizeof(info));
                    if (sys_waitid(P_PIDFD, fd, &info, WEXITED | WNOHANG, &ru) == 0) {
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
            runtime::blocking_call b;
            while (sys_waitid(P_PIDFD, h->pfd.Sysfd(), &info, WEXITED, &ru) < 0) {
                if (errno != EINTR) {
                    e = errno;
                    break;
                }
            }
        }
        h->release();
        if (e) return {nullptr, NewSyscallError("waitid", syscall::errno_error(e))};
        p.do_release(status_done);
        return {make_ptr<ProcessState>(static_cast<int>(info.si_pid), wait_status(info), to_rusage(ru)), nullptr};
    }

    // Go's pidWait, with blockUntilWaitable: waits until the process can
    // be reaped, then marks it done before reaping, so that Signal can't
    // signal a recycled pid.
    static std::pair<ptr<ProcessState>, error> pid_wait(process_impl& p, int pid) {
        if (p.state.load(std::memory_order_acquire) == status_released) return {nullptr, syscall::EINVAL_};
        bool ready = false;
        {
            runtime::blocking_call b;
            siginfo_t info{};
            long r;
            do {
                r = sys_waitid(P_PID, pid, &info, WEXITED | WNOWAIT, nullptr);
            } while (r < 0 && errno == EINTR);
            if (r == 0) {
                ready = true;
            } else if (errno != ENOSYS) {
                int e = errno;
                return {nullptr, NewSyscallError("waitid", syscall::errno_error(e))};
            }
        }
        if (ready) {
            p.do_release(status_done);
            // Wait for a Signal in progress.
            p.sig_mu.Lock();
            p.sig_mu.Unlock();
        }
        int status = 0;
        struct rusage ru{};
        pid_t pid1;
        {
            runtime::blocking_call b;
            do {
                pid1 = ::wait4(static_cast<pid_t>(pid), &status, 0, &ru);
            } while (pid1 < 0 && errno == EINTR);
        }
        if (pid1 < 0) {
            int e = errno;
            return {nullptr, NewSyscallError("wait", syscall::errno_error(e))};
        }
        p.do_release(status_done);
        return {make_ptr<ProcessState>(static_cast<int>(pid1), syscall::WaitStatus(static_cast<uint32>(status)),
                                       to_rusage(ru)),
                nullptr};
    }
};

}  // namespace detail

Process::Process(int pid, std::unique_ptr<detail::process_impl> impl) noexcept : Pid(pid), p_(std::move(impl)) {}

Process::~Process() {
    // Go's cleanup: the handle's reference goes with the Process.
    if (p_) p_->do_release(status_released);
}

error Process::Release() {
    // For historical reasons, Release sets Pid to -1 on Unix.
    Pid = pid_released;
    p_->do_release(status_released);
    return nullptr;
}

error Process::Kill() { return Signal(os::Kill); }

std::pair<ptr<ProcessState>, error> Process::Wait() {
    if (p_->handle) return detail::process_impl::pidfd_wait(*p_);
    return detail::process_impl::pid_wait(*p_, Pid);
}

error Process::Signal(const gopp::os::Signal& sig) {
    auto [s, ok] = sig.as_syscall();
    if (!ok) return errors::New("os: unsupported signal type");
    if (p_->handle) {
        // Go's pidfdSendSignal.
        auto [h, status] = p_->transient_acquire();
        if (status == status_done) return ErrProcessDone;
        if (status == status_released) return errProcessReleased;
        long r = sys_pidfd_send_signal(h->pfd.Sysfd(), s.value);
        int e = errno;
        h->release();
        if (r < 0) return convert_esrch(syscall::errno_error(e));
        return nullptr;
    }
    // Go's pidSignal.
    if (Pid == pid_released) return errProcessReleased;
    if (Pid == pid_unset) return errors::New("os: process not initialized");
    p_->sig_mu.RLock();
    error err;
    switch (p_->state.load(std::memory_order_acquire)) {
    case status_done: err = ErrProcessDone; break;
    case status_released: err = errProcessReleased; break;
    default: err = convert_esrch(syscall::Kill(Pid, s)); break;
    }
    p_->sig_mu.RUnlock();
    return err;
}

error Process::WithHandle(const std::function<void(uintptr)>& f) {
    if (!p_->handle) return ErrNoHandle;
    auto [h, status] = p_->transient_acquire();
    if (status == status_done) return ErrProcessDone;
    if (status == status_released) return errProcessReleased;
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
    return string::from_static("<nil>");
}

std::pair<ptr<Process>, error> StartProcess(std::string_view name, std::span<const string> argv, const ProcAttr& attr) {
    // Without SysProcAttr (no Chroot or credentials), check that the
    // directory exists, for a clearer error.
    if (!attr.Sys && !attr.Dir.empty()) {
        if (auto [_, err] = Stat(attr.Dir); err) {
            if (auto* pe = dynamic_cast<const PathError*>(err.get())) {
                return {nullptr, detail::path_error("chdir", pe->Path.view(), pe->Err)};
            }
            return {nullptr, err};
        }
    }

    // Go's ensurePidfd: ask for a pidfd, unless the caller did; then it's
    // the caller's, and the Process gets a duplicate.
    std::optional<syscall::SysProcAttr> sys = attr.Sys;
    bool works = detail::pidfd_works();
    bool should_dup = false;
    int pidfd = -1;
    if (works) {
        if (!sys) sys.emplace();
        if (!sys->PidFD) sys->PidFD = &pidfd;
        else should_dup = true;
    }

    syscall::ProcAttr sysattr;
    sysattr.Dir = attr.Dir;
    sysattr.Env = attr.Env.is_nil() ? Environ() : attr.Env;
    sysattr.Sys = sys ? &*sys : nullptr;
    sysattr.Files.reserve(attr.Files.size());
    for (const ptr<File>& f : attr.Files) sysattr.Files.push_back(f ? f->Fd() : ~uintptr(0));

    auto [pid, err] = syscall::ForkExec(name, argv, &sysattr);
    if (err) return {nullptr, detail::path_error("fork/exec", name, err)};

    if (works) {
        int h = *sys->PidFD;
        if (should_dup) h = ::fcntl(h, F_DUPFD_CLOEXEC, 0);
        if (h >= 0) return {detail::process_impl::new_handle_process(pid, h), nullptr};
    }
    return {detail::process_impl::new_pid_process(pid), nullptr};
}

std::pair<ptr<Process>, error> FindProcess(int pid) {
    // Go's pidfdFind.
    if (detail::pidfd_works()) {
        long h = sys_pidfd_open(pid, 0);
        if (h >= 0) return {detail::process_impl::new_handle_process(pid, static_cast<int>(h)), nullptr};
        // A process that doesn't exist is done: Signal and Wait say so.
        // Other errors fall back to the pid.
        if (errno == ESRCH) return {detail::process_impl::new_done_process(pid), nullptr};
    }
    return {detail::process_impl::new_pid_process(pid), nullptr};
}

}  // namespace gopp::os
