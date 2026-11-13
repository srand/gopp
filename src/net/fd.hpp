// A pollable socket, after Go's internal/poll.FD and net.netFD.
#pragma once

#include "../runtime/netpoll.hpp"

#include <gopp/errors.hpp>
#include <gopp/net.hpp>
#include <gopp/os.hpp>
#include <gopp/sync.hpp>

#include <sys/socket.h>

#include <atomic>
#include <functional>
#include <span>
#include <tuple>
#include <vector>

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
    // Go's netFD.connect: connects, giving up when ctx is done (with
    // errCanceled or errTimeout). Sets *peer to getpeername's address.
    error connect(const ptr<context::Context>& ctx, const void* sa, unsigned salen, sockaddr_storage* peer,
                  bool* havePeer);
    // writev(2) of the buffers, consuming what was written.
    std::pair<int64, error> writeBuffers(std::vector<slice<byte>>& v);
    // Go's RawControl, RawRead and RawWrite.
    error raw_control(const std::function<void(uintptr)>& f);
    error raw_read(const std::function<bool(uintptr)>& f);
    error raw_write(const std::function<bool(uintptr)>& f);
    // Waits until the socket is readable or writable ('r', 'w'), for
    // callers doing their own non-blocking calls (splice, sendfile); the
    // locks of Read or Write are taken by those callers through these.
    error wait(int mode);
    sync::Mutex& read_lock() noexcept { return rmu_; }
    sync::Mutex& write_lock() noexcept { return wmu_; }
    error getsockopt_int(int level, int name, int* value);
    error setsockopt_linger(int sec);
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
    // A copy of the descriptor as an os::File (Go's netFD.dup).
    std::pair<ptr<os::File>, error> dup();
    ptr<Addr> laddr;
    ptr<Addr> raddr;

    // Reference counting of in-flight operations against close (Go's
    // fdMutex, without its per-direction serialization queue).
    bool incref() noexcept;
    void decref() noexcept;

private:
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
// streams and a UDPAddr for datagrams; a UnixAddr; nullptr for AF_UNSPEC.
// sa must have been zeroed before the kernel filled it in.
ptr<Addr> sockaddr_to_addr(const sockaddr_storage& sa, int sotype);

// Go's SockaddrUnix.sockaddr: name as a sockaddr_un. "@name" (or a name
// starting with NUL) is in the abstract namespace; "" is unnamed, which
// bind(2) gives an abstract name.
error unix_sockaddr(std::string_view name, sockaddr_storage* sa, socklen_t* len);

// The network of a Unix socket of type sotype: "unix", "unixgram" or
// "unixpacket".
string unix_net(int sotype);

// net.OpError for a failed operation, unless err is nil or io::EOF_.
error op_error(std::string_view op, const string& network, ptr<Addr> source, ptr<Addr> addr, error err);

extern const error errMissingAddress;
// Go's errCanceled ("operation was canceled", which is context::Canceled)
// and errTimeout ("i/o timeout", which is context::DeadlineExceeded).
extern const error errCanceled;
extern const error errTimeout;
// Go's mapErr: a context's error as the net package reports it.
error mapErr(const error& err);

// The sockaddr of a TCPAddr, UDPAddr, IPAddr or UnixAddr for family; len 0
// for nullptr.
error addr_sockaddr(const Addr* a, int family, sockaddr_storage* sa, socklen_t* len);

// The descriptor of a Conn of this package, or nullptr.
netfd* netfd_of(const object* c);

// Go's ipToSockaddr: ip (nil for the wildcard), port and zone as a
// sockaddr of family.
error ip_to_sockaddr(int family, const IP& ip, int port, std::string_view zone, sockaddr_storage* sa,
                     socklen_t* len);

}  // namespace gopp::net
