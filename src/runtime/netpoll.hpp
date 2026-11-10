// Network poller interface for internal/poll, after Go's runtime/netpoll.Go.
//
// Each pollable file descriptor gets a pollDesc, registered with epoll
// once, edge-triggered. A fiber whose read would block parks on the
// pollDesc until the poller reports readiness, a deadline expires or the
// descriptor is closed.
#pragma once

#include <cstdint>

namespace gopp::runtime {

struct poll_desc;

enum poll_error : int {
    poll_no_error = 0,
    poll_err_closing = 1,
    poll_err_timeout = 2,
    poll_err_not_pollable = 3,
};

// Registers fd; returns nullptr and sets err (an errno) on failure.
poll_desc* poll_open(int fd, int* err) noexcept;
// Unregisters and frees pd. poll_unblock must have been called.
void poll_close(poll_desc* pd) noexcept;
// Prepares for a read ('r') or write ('w'); returns a poll_error.
int poll_reset(poll_desc* pd, int mode) noexcept;
// Waits until pd is ready for mode; returns a poll_error.
int poll_wait(poll_desc* pd, int mode) noexcept;
// Sets the deadline of mode ('r', 'w' or 'r'+'w'): an absolute nanotime,
// 0 for none, or negative for one that has passed.
void poll_set_deadline(poll_desc* pd, int64_t deadline, int mode) noexcept;
// Marks pd as closing and wakes its waiters.
void poll_unblock(poll_desc* pd) noexcept;

}  // namespace gopp::runtime
