// The syscall package: the errors of system calls, and the socket control
// messages of Unix domain sockets (Go's sockcmsg_unix.go).
#pragma once

// The module interfaces (modules/) import these instead.
#ifndef GOPP_MODULE
#include <gopp/errors.hpp>
#include <gopp/slice.hpp>
#include <gopp/string.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <span>
#include <utility>
#include <vector>
#endif

namespace gopp::syscall {

// syscall.Errno: an errno from a system call. errors::Is matches it with
// the fs errors: ENOENT is fs::ErrNotExist, EEXIST and ENOTEMPTY are
// fs::ErrExist, and EACCES and EPERM are fs::ErrPermission. Compare codes
// with errors::As<syscall::Errno>(err)->Code.
struct Errno : error_interface {
    int Code;
    explicit Errno(int c) : Code(c) {}
    string Error() const override;
    bool Timeout() const noexcept override;
    bool Is(const error_interface* target) const noexcept override;
};

// An Errno error for code.
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

// syscall.RawConn: access to a socket's descriptor (net::TCPConn's
// SyscallConn and Dialer::Control).
struct RawConn : public virtual object {
    // Calls f with the descriptor, which stays open meanwhile.
    virtual gopp::error Control(std::function<void(uintptr)> f) = 0;
    // Calls f with the descriptor until it returns true; after each false
    // it waits until the descriptor is readable.
    virtual gopp::error Read(std::function<bool(uintptr)> f) = 0;
    // Likewise, waiting until it's writable.
    virtual gopp::error Write(std::function<bool(uintptr)> f) = 0;
};

}  // namespace gopp::syscall
