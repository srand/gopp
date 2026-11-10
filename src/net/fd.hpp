// A pollable file descriptor, after Go's internal/poll.FD and net.netFD.
#pragma once

#include "../runtime/netpoll.hpp"

#include <gopp/errors.hpp>
#include <gopp/net.hpp>
#include <gopp/sync.hpp>

#include <atomic>
#include <span>

namespace gopp::net {

class netfd final : public object {
public:
    netfd(int sysfd, int family, string net) : sysfd_(sysfd), family_(family), net_(std::move(net)) {}
    ~netfd() override;

    // Registers with the poller.
    error init();

    std::pair<isize, error> Read(std::span<byte> p);
    std::pair<isize, error> Write(std::span<const byte> p);
    std::pair<ptr<netfd>, error> accept();
    // Connects to the sockaddr; waits for completion or the deadline.
    error connect(const void* sa, unsigned salen, int64 deadline);
    error Close();
    error shutdown(int how);
    error SetDeadline(time::Time t, int mode);

    int sysfd() const noexcept { return sysfd_; }
    string LocalAddr;
    string RemoteAddr;
    const string& network() const noexcept { return net_; }

private:
    // Reference counting of in-flight operations against close (Go's
    // fdMutex, without its per-direction serialization queue).
    bool incref() noexcept;
    void decref() noexcept;
    error op_error(int pollerr) const;

    int sysfd_;
    int family_;
    string net_;
    runtime::poll_desc* pd_ = nullptr;
    std::atomic<uint64_t> state_{1};  // bit 63: closed; low bits: references
    sync::Mutex rmu_;  // one reader at a time, as Go's readLock
    sync::Mutex wmu_;
};

// Formats a sockaddr as "host:port".
string sockaddr_string(const void* sa);

}  // namespace gopp::net
