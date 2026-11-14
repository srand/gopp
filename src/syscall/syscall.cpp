#include <gopp/io/fs.hpp>
#include <gopp/syscall.hpp>

#include <gopp/runtime.hpp>

#include <signal.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>

namespace gopp::syscall {

static_assert(sizeof(Rusage) == sizeof(struct rusage));
static_assert(SIGHUP_.value == SIGHUP && SIGINT_.value == SIGINT && SIGQUIT_.value == SIGQUIT &&
              SIGKILL_.value == SIGKILL && SIGTERM_.value == SIGTERM && SIGPIPE_.value == SIGPIPE &&
              SIGCHLD_.value == SIGCHLD && SIGUSR1_.value == SIGUSR1 && SIGWINCH_.value == SIGWINCH &&
              SIGURG_.value == SIGURG && SIGCONT_.value == SIGCONT && SIGTSTP_.value == SIGTSTP &&
              SIGSTOP_.value == SIGSTOP && SIGSYS_.value == SIGSYS && SIGTRAP_.value == SIGTRAP);

string Errno::Error() const {
    // Go's errno texts are strerror's, starting in lowercase.
    char buf[128];
    std::string s(strerror_r(Code, buf, sizeof(buf)));
    if (s.size() > 1 && s[0] >= 'A' && s[0] <= 'Z' && !(s[1] >= 'A' && s[1] <= 'Z')) s[0] = static_cast<char>(s[0] + 32);
    return string(s);
}

bool Errno::Timeout() const noexcept { return Code == EAGAIN || Code == ETIMEDOUT; }

bool Errno::Is(const errorInterface* target) const noexcept {
    if (target == fs::ErrPermission.get()) return Code == EACCES || Code == EPERM;
    if (target == fs::ErrExist.get()) return Code == EEXIST || Code == ENOTEMPTY;
    if (target == fs::ErrNotExist.get()) return Code == ENOENT;
    if (auto* e = dynamic_cast<const Errno*>(target)) return e->Code == Code;
    return false;
}

namespace {

// One error per errno Linux defines, made on first use and kept.
constexpr int nerrno = 134;

const error* errnoTable() {
    static const error* table = [] {
        auto* t = new error[nerrno];
        for (int i = 0; i < nerrno; i++) t[i] = make_ptr<Errno>(i);
        return t;
    }();
    return table;
}

}  // namespace

gopp::error errnoError(int code) {
    if (code >= 0 && code < nerrno) return errnoTable()[code];
    return make_ptr<Errno>(code);
}

const error E2BIG_ = errnoError(E2BIG);
const error EACCES_ = errnoError(EACCES);
const error EAGAIN_ = errnoError(EAGAIN);
const error EBADF_ = errnoError(EBADF);
const error ECHILD_ = errnoError(ECHILD);
const error EEXIST_ = errnoError(EEXIST);
const error EINTR_ = errnoError(EINTR);
const error EINVAL_ = errnoError(EINVAL);
const error EISDIR_ = errnoError(EISDIR);
const error ENOENT_ = errnoError(ENOENT);
const error ENOEXEC_ = errnoError(ENOEXEC);
const error ENOSYS_ = errnoError(ENOSYS);
const error ENOTDIR_ = errnoError(ENOTDIR);
const error EPERM_ = errnoError(EPERM);
const error EPIPE_ = errnoError(EPIPE);
const error ESRCH_ = errnoError(ESRCH);

namespace {

// Go's zerrors_linux_amd64.go signals table.
constexpr const char* signalNames[] = {
    nullptr,
    "hangup",
    "interrupt",
    "quit",
    "illegal instruction",
    "trace/breakpoint trap",
    "aborted",
    "bus error",
    "floating point exception",
    "killed",
    "user defined signal 1",
    "segmentation fault",
    "user defined signal 2",
    "broken pipe",
    "alarm clock",
    "terminated",
    "stack fault",
    "child exited",
    "continued",
    "stopped (signal)",
    "stopped",
    "stopped (tty input)",
    "stopped (tty output)",
    "urgent I/O condition",
    "CPU time limit exceeded",
    "file size limit exceeded",
    "virtual timer expired",
    "profiling timer expired",
    "window changed",
    "I/O possible",
    "power failure",
    "bad system call",
};

}  // namespace

string Signal::String() const {
    if (value >= 0 && value < static_cast<int>(std::size(signalNames)) && signalNames[value]) {
        return string::fromStatic(signalNames[value]);
    }
    return string("signal " + std::to_string(value));
}

sync::RWMutex ForkLock;

int Getpid() noexcept { return static_cast<int>(::getpid()); }
int Getppid() noexcept { return static_cast<int>(::getppid()); }
int Getuid() noexcept { return static_cast<int>(::getuid()); }
int Geteuid() noexcept { return static_cast<int>(::geteuid()); }
int Getgid() noexcept { return static_cast<int>(::getgid()); }
int Getegid() noexcept { return static_cast<int>(::getegid()); }

gopp::error Kill(int pid, Signal sig) {
    if (::kill(static_cast<pid_t>(pid), sig.value) < 0) return errnoError(errno);
    return nullptr;
}

static_assert(MS_RDONLY == ::MS_RDONLY && MS_BIND == ::MS_BIND && MS_REC == ::MS_REC && MS_PRIVATE == ::MS_PRIVATE &&
              MS_SHARED == ::MS_SHARED && MNT_FORCE == ::MNT_FORCE && MNT_DETACH == ::MNT_DETACH);

namespace {

// A C string of s: false if it holds a NUL (Go's BytePtrFromString, which
// fails with EINVAL).
bool cstr(std::string_view s, std::string& out) {
    if (s.find('\0') != std::string_view::npos) return false;
    out.assign(s);
    return true;
}

// Runs a call that may block, retrying after EINTR; nil or its errno.
template <typename F>
gopp::error blocking(F&& f) {
    for (;;) {
        int r, e;
        {
            runtime::blockingCall b;
            r = f();
            e = errno;
        }
        if (r == 0) return nullptr;
        if (e != EINTR) return errnoError(e);
    }
}

}  // namespace

gopp::error Chroot(std::string_view path) {
    std::string p;
    if (!cstr(path, p)) return EINVAL_;
    return blocking([&] { return ::chroot(p.c_str()); });
}

gopp::error Mount(std::string_view source, std::string_view target, std::string_view fstype, uintptr flags,
                  std::string_view data) {
    std::string src, tgt, fst, dat;
    if (!cstr(source, src) || !cstr(target, tgt) || !cstr(fstype, fst) || !cstr(data, dat)) return EINVAL_;
    // Certain file systems get rather angry and EINVAL if you give them an
    // empty string of data, rather than NULL.
    const void* datap = data.empty() ? nullptr : dat.c_str();
    return blocking([&] { return ::mount(src.c_str(), tgt.c_str(), fst.c_str(), flags, datap); });
}

gopp::error Unmount(std::string_view target, int flags) {
    std::string tgt;
    if (!cstr(target, tgt)) return EINVAL_;
    return blocking([&] { return ::umount2(tgt.c_str(), flags); });
}

int CmsgLen(int datalen) { return static_cast<int>(CMSG_LEN(static_cast<unsigned>(datalen))); }
int CmsgSpace(int datalen) { return static_cast<int>(CMSG_SPACE(static_cast<unsigned>(datalen))); }

namespace {

constexpr std::size_t cmsgAlign(std::size_t n) { return (n + sizeof(std::size_t) - 1) & ~(sizeof(std::size_t) - 1); }

}  // namespace

std::pair<std::vector<SocketControlMessage>, gopp::error> ParseSocketControlMessage(std::span<const byte> b) {
    std::vector<SocketControlMessage> msgs;
    std::size_t i = 0;
    while (i + CMSG_LEN(0) <= b.size()) {
        cmsghdr h;
        std::memcpy(&h, b.data() + i, sizeof(h));
        if (h.cmsg_len < sizeof(cmsghdr) || h.cmsg_len > b.size() - i) return {{}, errnoError(EINVAL)};
        SocketControlMessage m;
        m.Header = {static_cast<uint64_t>(h.cmsg_len), h.cmsg_level, h.cmsg_type};
        m.Data = slice<byte>::of(b.subspan(i + cmsgAlign(sizeof(cmsghdr)), h.cmsg_len - cmsgAlign(sizeof(cmsghdr))));
        msgs.push_back(std::move(m));
        i += cmsgAlign(h.cmsg_len);
    }
    return {msgs, nullptr};
}

slice<byte> UnixRights(std::initializer_list<int> fds) {
    std::size_t datalen = fds.size() * sizeof(int);
    auto b = slice<byte>::make(CMSG_SPACE(datalen));
    cmsghdr h{};
    h.cmsg_level = SOL_SOCKET;
    h.cmsg_type = SCM_RIGHTS;
    h.cmsg_len = CMSG_LEN(datalen);
    std::memcpy(b.data(), &h, sizeof(h));
    std::memcpy(b.data() + cmsgAlign(sizeof(cmsghdr)), fds.begin(), datalen);
    return b;
}

std::pair<std::vector<int>, gopp::error> ParseUnixRights(const SocketControlMessage& m) {
    if (m.Header.Level != SOL_SOCKET || m.Header.Type != SCM_RIGHTS) return {{}, errnoError(EINVAL)};
    std::vector<int> fds(m.Data.size() / sizeof(int));
    std::memcpy(fds.data(), m.Data.data(), fds.size() * sizeof(int));
    return {fds, nullptr};
}

std::pair<std::array<int, 2>, gopp::error> Socketpair(int domain, int typ, int proto) {
    std::array<int, 2> fd{-1, -1};
    if (::socketpair(domain, typ, proto, fd.data()) < 0) return {{-1, -1}, errnoError(errno)};
    return {fd, nullptr};
}

}  // namespace gopp::syscall
