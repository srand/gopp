// A pollable socket, after Go's internal/poll.FD and net.netFD.
#pragma once

#include "../runtime/netpoll.hpp"

#include <gopp/errors.hpp>
#include <gopp/net.hpp>
#include <gopp/sync.hpp>

#include <sys/socket.h>

#include <atomic>
#include <span>
#include <tuple>

namespace gopp::net {

class netfd final : public object {
public:
    netfd(int sysfd, int family, int sotype, string net)
        : sysfd_(sysfd), family_(family), sotype_(sotype), net_(std::move(net)) {}
    ~netfd() override;

    // Registers with the poller.
    error init();

    // read(2) and write(2). Write writes all of p on a stream socket, one
    // packet on another.
    std::pair<isize, error> Read(std::span<byte> p);
    std::pair<isize, error> Write(std::span<const byte> p);
    // recvmsg(2): from gets the sender (ss_family AF_UNSPEC if there's
    // none). Returns n, oobn, flags and an error.
    std::tuple<isize, isize, int, error> readMsg(std::span<byte> p, std::span<byte> oob, int flags,
                                                 sockaddr_storage* from);
    // sendmsg(2) to sa (nullptr: the peer). Returns n, oobn and an error.
    std::tuple<isize, isize, error> writeMsg(std::span<const byte> p, std::span<const byte> oob, const void* sa,
                                             unsigned salen);
    std::pair<ptr<netfd>, error> accept();
    // Connects to the sockaddr; waits for completion or the deadline.
    error connect(const void* sa, unsigned salen, int64 deadline);
    error Close();
    error shutdown(int how);
    error SetDeadline(time::Time t, int mode);
    error setsockopt_int(int level, int name, int value);

    int sysfd() const noexcept { return sysfd_; }
    int family() const noexcept { return family_; }
    int sotype() const noexcept { return sotype_; }
    const string& network() const noexcept { return net_; }

    // Sets laddr from getsockname(2) and raddr from getpeername(2), or
    // from raddr_sa when it's given.
    void set_addrs(const sockaddr_storage* raddr_sa = nullptr);
    ptr<Addr> laddr;
    ptr<Addr> raddr;

private:
    // Reference counting of in-flight operations against close (Go's
    // fdMutex, without its per-direction serialization queue).
    bool incref() noexcept;
    void decref() noexcept;
    error op_error(int pollerr) const;

    int sysfd_;
    int family_;
    int sotype_;
    string net_;
    runtime::poll_desc* pd_ = nullptr;
    std::atomic<uint64_t> state_{1};  // bit 63: closed; low bits: references
    sync::Mutex rmu_;  // one reader at a time, as Go's readLock
    sync::Mutex wmu_;
};

// The Addr of a sockaddr for a socket of type sotype: a TCPAddr for
// streams and a UDPAddr for datagrams; nullptr for AF_UNSPEC.
ptr<Addr> sockaddr_to_addr(const sockaddr_storage& sa, int sotype);

// Go's ipToSockaddr: ip (nil for the wildcard), port and zone as a
// sockaddr of family.
error ip_to_sockaddr(int family, const IP& ip, int port, std::string_view zone, sockaddr_storage* sa,
                     socklen_t* len);

}  // namespace gopp::net
