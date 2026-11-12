#include <gopp/io/fs.hpp>
#include <gopp/syscall.hpp>

#include <sys/socket.h>

#include <cerrno>
#include <cstring>
#include <string>

namespace gopp::syscall {

string Errno::Error() const {
    // Go's errno texts are strerror's, starting in lowercase.
    char buf[128];
    std::string s(strerror_r(Code, buf, sizeof(buf)));
    if (s.size() > 1 && s[0] >= 'A' && s[0] <= 'Z' && !(s[1] >= 'A' && s[1] <= 'Z')) s[0] = static_cast<char>(s[0] + 32);
    return string(s);
}

bool Errno::Timeout() const noexcept { return Code == EAGAIN || Code == ETIMEDOUT; }

bool Errno::Is(const error_interface* target) const noexcept {
    if (target == fs::ErrPermission.get()) return Code == EACCES || Code == EPERM;
    if (target == fs::ErrExist.get()) return Code == EEXIST || Code == ENOTEMPTY;
    if (target == fs::ErrNotExist.get()) return Code == ENOENT;
    return false;
}

gopp::error errno_error(int code) { return make_ptr<Errno>(code); }

int CmsgLen(int datalen) { return static_cast<int>(CMSG_LEN(static_cast<unsigned>(datalen))); }
int CmsgSpace(int datalen) { return static_cast<int>(CMSG_SPACE(static_cast<unsigned>(datalen))); }

namespace {

constexpr std::size_t cmsg_align(std::size_t n) { return (n + sizeof(std::size_t) - 1) & ~(sizeof(std::size_t) - 1); }

}  // namespace

std::pair<std::vector<SocketControlMessage>, gopp::error> ParseSocketControlMessage(std::span<const byte> b) {
    std::vector<SocketControlMessage> msgs;
    std::size_t i = 0;
    while (i + CMSG_LEN(0) <= b.size()) {
        cmsghdr h;
        std::memcpy(&h, b.data() + i, sizeof(h));
        if (h.cmsg_len < sizeof(cmsghdr) || h.cmsg_len > b.size() - i) return {{}, errno_error(EINVAL)};
        SocketControlMessage m;
        m.Header = {static_cast<uint64_t>(h.cmsg_len), h.cmsg_level, h.cmsg_type};
        m.Data = slice<byte>::of(b.subspan(i + cmsg_align(sizeof(cmsghdr)), h.cmsg_len - cmsg_align(sizeof(cmsghdr))));
        msgs.push_back(std::move(m));
        i += cmsg_align(h.cmsg_len);
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
    std::memcpy(b.data() + cmsg_align(sizeof(cmsghdr)), fds.begin(), datalen);
    return b;
}

std::pair<std::vector<int>, gopp::error> ParseUnixRights(const SocketControlMessage& m) {
    if (m.Header.Level != SOL_SOCKET || m.Header.Type != SCM_RIGHTS) return {{}, errno_error(EINVAL)};
    std::vector<int> fds(m.Data.size() / sizeof(int));
    std::memcpy(fds.data(), m.Data.data(), fds.size() * sizeof(int));
    return {fds, nullptr};
}

std::pair<std::array<int, 2>, gopp::error> Socketpair(int domain, int typ, int proto) {
    std::array<int, 2> fd{-1, -1};
    if (::socketpair(domain, typ, proto, fd.data()) < 0) return {{-1, -1}, errno_error(errno)};
    return {fd, nullptr};
}

}  // namespace gopp::syscall
