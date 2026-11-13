// The context package: cancellation and deadlines across fibers.
//
//     auto [ctx, cancel] = context::WithTimeout(context::Background(), 2 * time::Second);
//     select(recv(ctx->Done(), [&] { return ctx->Err(); }), ...);
#pragma once

// The module interfaces (modules/) import these instead.
#ifndef GOPP_MODULE
#include <gopp/chan.hpp>
#include <gopp/errors.hpp>
#include <gopp/ptr.hpp>
#include <gopp/time.hpp>

#include <functional>
#include <optional>
#include <utility>
#endif

namespace gopp::context {

// context.Canceled and context.DeadlineExceeded.
extern const error Canceled;
extern const error DeadlineExceeded;

struct Context : public virtual object {
    // The time when work on behalf of this context should be canceled.
    virtual std::optional<time::Time> Deadline() const { return std::nullopt; }
    // A channel closed when the context is canceled; nil if it never is.
    virtual chan<empty> Done() const = 0;
    // nil until done() is closed; then Canceled or DeadlineExceeded.
    virtual error Err() const = 0;
    // The value for key, or nullptr. Keys are compared by address.
    virtual ptr<object> Value(const void* key) const { return nullptr; }
};

using CancelFunc = std::function<void()>;

// context.Background: never canceled, no values, no deadline.
ptr<Context> Background();
// context.TODO
ptr<Context> TODO();

// context.CancelCauseFunc: cancels with a cause, which Cause returns; a
// nil cause means Canceled.
using CancelCauseFunc = std::function<void(error cause)>;

// context.WithCancel
std::pair<ptr<Context>, CancelFunc> WithCancel(ptr<Context> parent);
// context.WithCancelCause
std::pair<ptr<Context>, CancelCauseFunc> WithCancelCause(ptr<Context> parent);
// context.WithDeadline
std::pair<ptr<Context>, CancelFunc> WithDeadline(ptr<Context> parent, time::Time d);
// context.WithTimeout
std::pair<ptr<Context>, CancelFunc> WithTimeout(ptr<Context> parent, time::Duration timeout);
// context.WithValue
ptr<Context> WithValue(ptr<Context> parent, const void* key, ptr<object> val);

// context.Cause
error Cause(const ptr<Context>& ctx);

}  // namespace gopp::context
