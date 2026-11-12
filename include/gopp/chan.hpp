// Channels and select.
//
//     auto ch = gopp::make_chan<int>();      // ch := make(chan int)
//     auto bc = gopp::make_chan<int>(10);    // bc := make(chan int, 10)
//     ch.send(1);                            // ch <- 1
//     int v = ch.recv();                     // v := <-ch
//     auto [w, ok] = ch.recv2();             // w, ok := <-ch
//     ch.close();                            // close(ch)
//     for (int x : ch) { ... }               // for x := range ch
//
//     gopp::select(                          // select {
//         gopp::recv(ch, [&](int v) { }),    // case v := <-ch:
//         gopp::send(bc, 5, [&] { }),        // case bc <- 5:
//         gopp::default_case([&] { }));      // default: }
#pragma once

// The module interfaces (modules/) import these instead.
#ifndef GOPP_MODULE
#include <gopp/base.hpp>

#include <cstddef>
#include <functional>
#include <memory>
#include <new>
#include <tuple>
#include <type_traits>
#include <utility>
#endif

namespace gopp {

namespace detail {

// How the type-erased channel core moves and destroys elements.
struct elem_ops {
    std::size_t size;
    std::size_t align;
    void (*move_construct)(void* dst, void* src) noexcept;
    void (*destroy)(void* p) noexcept;
};

template <typename T>
inline constexpr elem_ops elem_ops_of = {
    sizeof(T),
    alignof(T),
    [](void* dst, void* src) noexcept { new (dst) T(std::move(*static_cast<T*>(src))); },
    [](void* p) noexcept { static_cast<T*>(p)->~T(); },
};

struct hchan;

hchan* chan_make(const elem_ops* ops, std::size_t size);
void chan_retain(hchan* c) noexcept;
void chan_release(hchan* c) noexcept;
// Moves *elem into the channel. Returns false if !block and it would block.
bool chan_send(hchan* c, void* elem, bool block);
struct recv_result {
    bool selected;
    bool received;  // false if the channel is closed; *elem is not constructed then
};
// Move-constructs a received value into the storage at elem.
recv_result chan_recv(hchan* c, void* elem, bool block);
void chan_close(hchan* c);
std::size_t chan_len(hchan* c) noexcept;
std::size_t chan_cap(hchan* c) noexcept;

struct scase {
    hchan* c;
    void* elem;
};
struct select_result {
    int index;  // -1 for the default case
    bool recv_ok;
};
// Cases: the sends first, then the receives.
select_result selectgo(scase* cases, int nsends, int nrecvs, bool block);

// Uninitialized storage for a T.
template <typename T>
struct slot {
    alignas(T) unsigned char buf[sizeof(T)];
    T* get() noexcept { return std::launder(reinterpret_cast<T*>(buf)); }
    T take() noexcept(std::is_nothrow_move_constructible_v<T>) {
        T* p = get();
        T v(std::move(*p));
        p->~T();
        return v;
    }
};

}  // namespace detail

// chan T: a reference to a channel, nil by default. Copies refer to the
// same channel.
template <typename T>
class chan {
public:
    using value_type = T;

    chan() noexcept = default;
    chan(std::nullptr_t) noexcept {}

    chan(const chan& o) noexcept : c_(o.c_) {
        if (c_) detail::chan_retain(c_);
    }
    chan(chan&& o) noexcept : c_(std::exchange(o.c_, nullptr)) {}
    ~chan() {
        if (c_) detail::chan_release(c_);
    }
    chan& operator=(chan o) noexcept {
        std::swap(c_, o.c_);
        return *this;
    }

    // make(chan T, size)
    static chan make(std::size_t size = 0) { return chan(detail::chan_make(&detail::elem_ops_of<T>, size)); }

    // ch <- v. Sending on a nil channel blocks forever; on a closed one, panics.
    void send(T v) const { detail::chan_send(c_, &v, true); }

    // <-ch. Returns the zero value once the channel is closed and drained.
    T recv() const {
        detail::slot<T> s;
        if (detail::chan_recv(c_, s.get(), true).received) return s.take();
        return T{};
    }

    // v, ok := <-ch
    std::pair<T, bool> recv2() const {
        detail::slot<T> s;
        if (detail::chan_recv(c_, s.get(), true).received) return {s.take(), true};
        return {T{}, false};
    }

    // select { case ch <- v: return true; default: return false }
    bool try_send(T& v) const { return detail::chan_send(c_, &v, false); }
    bool try_send(T&& v) const { return detail::chan_send(c_, &v, false); }

    // select { case v, ok := <-ch: ...; default: }. Returns whether a value
    // was received; closed is set when the channel is closed and empty.
    bool try_recv(T& out, bool* closed = nullptr) const {
        detail::slot<T> s;
        auto r = detail::chan_recv(c_, s.get(), false);
        if (closed) *closed = r.selected && !r.received;
        if (r.received) out = s.take();
        return r.received;
    }

    void close() const { detail::chan_close(c_); }
    std::size_t len() const noexcept { return c_ ? detail::chan_len(c_) : 0; }
    std::size_t cap() const noexcept { return c_ ? detail::chan_cap(c_) : 0; }

    explicit operator bool() const noexcept { return c_ != nullptr; }
    bool operator==(const chan& o) const noexcept { return c_ == o.c_; }
    bool operator==(std::nullptr_t) const noexcept { return c_ == nullptr; }

    detail::hchan* Handle() const noexcept { return c_; }

    // for v := range ch
    class iterator {
    public:
        using value_type = T;
        using difference_type = std::ptrdiff_t;

        iterator() = default;
        explicit iterator(const chan* ch) : ch_(ch) { ++*this; }
        T& operator*() noexcept { return v_; }
        iterator& operator++() {
            auto [v, ok] = ch_->recv2();
            if (ok) v_ = std::move(v);
            else ch_ = nullptr;
            return *this;
        }
        void operator++(int) { ++*this; }
        bool operator==(const iterator& o) const noexcept { return ch_ == o.ch_; }

    private:
        const chan* ch_ = nullptr;
        T v_{};
    };
    iterator begin() const { return iterator(this); }
    iterator end() const { return iterator(); }

private:
    explicit chan(detail::hchan* c) noexcept : c_(c) {}
    detail::hchan* c_ = nullptr;
};

// make(chan T, size)
template <typename T>
chan<T> make_chan(std::size_t size = 0) {
    return chan<T>::make(size);
}

// ---------------------------------------------------------------------------
// select

template <typename T, typename F>
struct recv_case {
    const chan<T>& ch;
    F f;
    detail::slot<T> s;

    static constexpr bool is_send = false;
    static constexpr bool is_default = false;

    void run(bool ok) {
        T v = ok ? s.take() : T{};
        if constexpr (std::is_invocable_v<F&, T&&, bool>) f(std::move(v), ok);
        else if constexpr (std::is_invocable_v<F&, T&&>) f(std::move(v));
        else f();
    }
};

template <typename T, typename F>
struct send_case {
    const chan<T>& ch;
    T v;
    F f;

    static constexpr bool is_send = true;
    static constexpr bool is_default = false;

    void run(bool) { f(); }
};

template <typename F>
struct default_case_t {
    F f;

    static constexpr bool is_send = false;
    static constexpr bool is_default = true;

    void run(bool) { f(); }
};

// case v, ok := <-ch: f(v, ok)   (f may also take just v, or nothing)
template <typename T, typename F>
recv_case<T, std::decay_t<F>> recv(const chan<T>& ch, F&& f) {
    return {ch, std::forward<F>(f), {}};
}

// case ch <- v: f()
template <typename T, typename V, typename F>
send_case<T, std::decay_t<F>> send(const chan<T>& ch, V&& v, F&& f) {
    return {ch, T(std::forward<V>(v)), std::forward<F>(f)};
}

// default: f()
template <typename F>
default_case_t<std::decay_t<F>> default_case(F&& f) {
    return {std::forward<F>(f)};
}

// select { cases... }: runs the case that can proceed, picking one at
// random if several can. Without a default case, blocks until one can.
template <typename... Cases>
void select(Cases&&... cases) {
    constexpr int ndefault = (0 + ... + (std::decay_t<Cases>::is_default ? 1 : 0));
    static_assert(ndefault <= 1, "select: multiple defaults");
    constexpr int n = static_cast<int>(sizeof...(Cases)) - ndefault;

    detail::scase sc[n > 0 ? n : 1];
    int map[n > 0 ? n : 1];
    int nsends = 0, nrecvs = 0;
    int idx = 0;
    auto add_send = [&](auto& c) {
        using C = std::decay_t<decltype(c)>;
        if constexpr (C::is_send) {
            sc[nsends] = {c.ch.Handle(), &c.v};
            map[nsends] = idx;
            nsends++;
        }
        idx++;
    };
    (add_send(cases), ...);
    idx = 0;
    auto add_recv = [&](auto& c) {
        using C = std::decay_t<decltype(c)>;
        if constexpr (!C::is_send && !C::is_default) {
            sc[nsends + nrecvs] = {c.ch.Handle(), c.s.get()};
            map[nsends + nrecvs] = idx;
            nrecvs++;
        }
        idx++;
    };
    (add_recv(cases), ...);

    auto r = detail::selectgo(sc, nsends, nrecvs, ndefault == 0);

    int chosen = -1;
    if (r.index >= 0) chosen = map[r.index];
    idx = 0;
    auto dispatch = [&](auto& c) {
        using C = std::decay_t<decltype(c)>;
        bool hit = r.index < 0 ? C::is_default : idx == chosen;
        idx++;
        if (hit) c.run(r.recv_ok);
    };
    (dispatch(cases), ...);
}

}  // namespace gopp
