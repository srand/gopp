// os/signal, after Go's os/signal/signal.go and signal_unix.go. The
// runtime side, which handles the signals, is in runtime/signal.cpp.
#include "../../runtime/runtime.hpp"

#include <gopp/chan.hpp>
#include <gopp/context.hpp>
#include <gopp/os.hpp>
#include <gopp/os/signal.hpp>
#include <gopp/runtime.hpp>
#include <gopp/sync.hpp>

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <vector>

namespace gopp::os::signal {

namespace {

constexpr int num_sig = 65;  // the most of all systems

struct handler {
    uint32_t mask[(num_sig + 31) / 32] = {};

    bool want(int sig) const noexcept { return (mask[sig / 32] >> (sig & 31)) & 1; }
    void set(int sig) noexcept { mask[sig / 32] |= uint32_t(1) << (sig & 31); }
    void clear(int sig) noexcept { mask[sig / 32] &= ~(uint32_t(1) << (sig & 31)); }
    bool empty() const noexcept { return std::all_of(std::begin(mask), std::end(mask), [](uint32_t m) { return m == 0; }); }
};

struct entry {
    chan<os::Signal> c;
    handler h;
};

struct handlers_t {
    sync::Mutex mu;
    // The channels and the signals they get. Not a map: there are few.
    std::vector<entry> m;
    // The number of channels that get each signal.
    int64 ref[num_sig] = {};
    // Channels being stopped: they still get the signals that arrived
    // before Stop.
    std::vector<entry> stopping;
};

handlers_t& handlers() {
    static handlers_t* h = new handlers_t();  // never freed: the loop uses it
    return *h;
}

sync::Once watch_signal_loop_once;

entry* find(std::vector<entry>& v, const chan<os::Signal>& c) {
    for (auto& e : v) {
        if (e.c == c) return &e;
    }
    return nullptr;
}

// Go's signum: the number of a syscall::Signal, or -1.
int signum(const os::Signal& sig) {
    auto [s, ok] = sig.as_syscall();
    if (!ok || s.value < 0 || s.value >= num_sig) return -1;
    return s.value;
}

void process(os::Signal sig) {
    int n = signum(sig);
    if (n < 0) return;
    handlers_t& hs = handlers();
    std::lock_guard l(hs.mu);
    for (auto& e : hs.m) {
        // Send, but don't block for it.
        if (e.h.want(n)) e.c.try_send(os::Signal(sig));
    }
    // Avoid the race mentioned in Stop.
    for (auto& d : hs.stopping) {
        if (d.h.want(n)) d.c.try_send(os::Signal(sig));
    }
}

// Go's loop: the fiber that delivers the signals.
void loop() {
    for (;;) process(syscall::Signal(static_cast<int>(runtime::signal_recv())));
}

// Stops relaying the signals, all if none, to any channel, and either
// gives them back their earlier disposition or ignores them.
void cancel(std::span<const os::Signal> sigs, void (*action)(uint32_t) noexcept) {
    handlers_t& hs = handlers();
    std::lock_guard l(hs.mu);
    auto remove = [&](int n) {
        if (n < 0) return;
        for (auto it = hs.m.begin(); it != hs.m.end();) {
            if (it->h.want(n)) {
                hs.ref[n]--;
                it->h.clear(n);
                if (it->h.empty()) {
                    it = hs.m.erase(it);
                    continue;
                }
            }
            ++it;
        }
        action(static_cast<uint32_t>(n));
    };
    if (sigs.empty()) {
        for (int n = 0; n < num_sig; n++) remove(n);
    } else {
        for (const auto& s : sigs) remove(signum(s));
    }
}

// The cause of a NotifyContext context: "interrupt signal received". It
// is context::Canceled too, for errors::Is.
struct signal_error final : error_interface {
    explicit signal_error(string s) : s(std::move(s)) {}
    string Error() const override { return s; }
    bool Is(const error_interface* target) const noexcept override { return target == context::Canceled.get(); }
    string s;
};

}  // namespace

namespace detail {

void notify(const chan<os::Signal>& c, std::span<const os::Signal> sigs) {
    if (!c) panic("os/signal: Notify using nil channel");
    handlers_t& hs = handlers();
    std::lock_guard l(hs.mu);
    auto add = [&](int n) {
        if (n < 0) return;
        entry* e = find(hs.m, c);
        if (!e) {
            hs.m.push_back(entry{c, {}});
            e = &hs.m.back();
        }
        if (!e->h.want(n)) {
            e->h.set(n);
            if (hs.ref[n] == 0) {
                runtime::signal_enable(static_cast<uint32_t>(n));
                // The runtime wants a signal enabled before the watcher
                // starts.
                watch_signal_loop_once.Do([] { go(loop); });
            }
            hs.ref[n]++;
        }
    };
    if (sigs.empty()) {
        for (int n = 0; n < num_sig; n++) add(n);
    } else {
        for (const auto& s : sigs) add(signum(s));
    }
}

void ignore(std::span<const os::Signal> sigs) { cancel(sigs, runtime::signal_ignore); }

void reset(std::span<const os::Signal> sigs) { cancel(sigs, runtime::signal_disable); }

std::pair<ptr<context::Context>, context::CancelFunc> notify_context(ptr<context::Context> parent,
                                                                    std::span<const os::Signal> sigs) {
    auto [ctx, cancel] = context::WithCancelCause(std::move(parent));
    auto ch = make_chan<os::Signal>(1);
    notify(ch, sigs);
    if (!ctx->Err()) {
        // Watch for a signal until the context is done: by the signal,
        // stop or the parent.
        chan<empty> done = ctx->Done();
        go([ch, done, cancel] {
            select(recv(ch, [&](os::Signal s) {
                       cancel(make_ptr<signal_error>(s.String() + " signal received"));
                   }),
                   recv(done, [] {}));
        });
    }
    return {ctx, [cancel, ch] {
                cancel(nullptr);
                Stop(ch);
            }};
}

}  // namespace detail

void Stop(const chan<os::Signal>& c) {
    handlers_t& hs = handlers();
    entry stopped;
    {
        std::lock_guard l(hs.mu);
        auto it = std::find_if(hs.m.begin(), hs.m.end(), [&](const entry& e) { return e.c == c; });
        if (it == hs.m.end()) return;
        stopped = *it;
        hs.m.erase(it);
        for (int n = 0; n < num_sig; n++) {
            if (stopped.h.want(n)) {
                hs.ref[n]--;
                if (hs.ref[n] == 0) runtime::signal_disable(static_cast<uint32_t>(n));
            }
        }
        // Signals that arrived before are still delivered to c: either c
        // gets a signal such as SIGINT, or the program takes the default
        // action. Keep c until the delivery is done.
        hs.stopping.push_back(stopped);
    }

    runtime::signal_wait_until_idle();

    std::lock_guard l(hs.mu);
    auto it = std::find_if(hs.stopping.begin(), hs.stopping.end(), [&](const entry& e) { return e.c == c; });
    if (it != hs.stopping.end()) hs.stopping.erase(it);
}

bool Ignored(const os::Signal& sig) {
    int n = signum(sig);
    return n >= 0 && runtime::signal_ignored(static_cast<uint32_t>(n));
}

}  // namespace gopp::os::signal
