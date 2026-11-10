// The time package: durations, wall and monotonic time, sleeping and
// timers that work with channels.
#pragma once

#include <gopp/base.hpp>
#include <gopp/chan.hpp>
#include <gopp/errors.hpp>
#include <gopp/ptr.hpp>
#include <gopp/string.hpp>

#include <chrono>
#include <compare>
#include <format>
#include <functional>

namespace gopp::time {

// time.Duration, in nanoseconds. Converts from std::chrono durations, so
// time::Sleep(100ms) works with std::chrono_literals.
struct Duration {
    int64 ns = 0;

    constexpr Duration() noexcept = default;
    constexpr explicit Duration(int64 n) noexcept : ns(n) {}
    template <typename Rep, typename Period>
    constexpr Duration(std::chrono::duration<Rep, Period> d) noexcept
        : ns(std::chrono::duration_cast<std::chrono::nanoseconds>(d).count()) {}

    constexpr int64 Nanoseconds() const noexcept { return ns; }
    constexpr int64 Microseconds() const noexcept { return ns / 1000; }
    constexpr int64 Milliseconds() const noexcept { return ns / 1000000; }
    constexpr double Seconds() const noexcept { return static_cast<double>(ns) / 1e9; }
    string String() const;

    friend constexpr Duration operator+(Duration a, Duration b) noexcept { return Duration(a.ns + b.ns); }
    friend constexpr Duration operator-(Duration a, Duration b) noexcept { return Duration(a.ns - b.ns); }
    friend constexpr Duration operator-(Duration a) noexcept { return Duration(-a.ns); }
    friend constexpr Duration operator*(Duration a, int64 k) noexcept { return Duration(a.ns * k); }
    friend constexpr Duration operator*(int64 k, Duration a) noexcept { return Duration(a.ns * k); }
    friend constexpr Duration operator/(Duration a, int64 k) noexcept { return Duration(a.ns / k); }
    friend constexpr int64 operator/(Duration a, Duration b) noexcept { return a.ns / b.ns; }
    friend constexpr auto operator<=>(Duration a, Duration b) noexcept = default;
    constexpr Duration& operator+=(Duration o) noexcept {
        ns += o.ns;
        return *this;
    }
};

inline constexpr Duration Nanosecond{1};
inline constexpr Duration Microsecond{1000};
inline constexpr Duration Millisecond{1000000};
inline constexpr Duration Second{1000000000};
inline constexpr Duration Minute{60 * 1000000000LL};
inline constexpr Duration Hour{3600 * 1000000000LL};

// time.ParseDuration: "300ms", "-1.5h", "2h45m". Units: ns, us (or µs),
// ms, s, m, h.
std::pair<Duration, error> ParseDuration(std::string_view s);

// time.Time: wall clock time with a monotonic reading for measuring.
class Time {
public:
    constexpr Time() noexcept = default;

    // The zero Time, as returned by Time{}.
    constexpr bool IsZero() const noexcept { return wall_ == 0 && mono_ == 0; }

    int64 Unix() const noexcept { return floordiv(wall_, 1000000000); }
    int64 UnixMilli() const noexcept { return floordiv(wall_, 1000000); }
    int64 UnixNano() const noexcept { return wall_; }

    Time Add(Duration d) const noexcept {
        Time t = *this;
        t.wall_ += d.ns;
        if (t.mono_) t.mono_ += d.ns;
        return t;
    }

    // t.Sub(u), using monotonic readings when both have them.
    Duration Sub(const Time& u) const noexcept {
        if (mono_ && u.mono_) return Duration(mono_ - u.mono_);
        return Duration(wall_ - u.wall_);
    }

    bool Before(const Time& u) const noexcept { return Sub(u).ns < 0; }
    bool After(const Time& u) const noexcept { return Sub(u).ns > 0; }
    bool Equal(const Time& u) const noexcept { return wall_ == u.wall_; }

    // RFC 1123 in UTC, as HTTP uses: "Mon, 02 Jan 2006 15:04:05 GMT".
    string FormatHTTP() const;
    // RFC 3339 in UTC with nanoseconds.
    string FormatRFC3339() const;

    static Time Unix(int64 sec, int64 nsec) noexcept {
        Time t;
        t.wall_ = sec * 1000000000 + nsec;
        return t;
    }

    friend Time Now() noexcept;

private:
    static constexpr int64 floordiv(int64 a, int64 b) noexcept { return a / b - (a % b != 0 && (a < 0) != (b < 0)); }

    int64 wall_ = 0;  // Unix nanoseconds
    int64 mono_ = 0;  // runtime nanotime, 0 if none
};

// time.Now
Time Now() noexcept;

// time.Since
inline Duration Since(const Time& t) noexcept { return Now().Sub(t); }

// time.Until
inline Duration Until(const Time& t) noexcept { return t.Sub(Now()); }

// time.Sleep: parks the fiber for at least d.
void Sleep(Duration d);

// time.Timer: sends the time on c when it fires, or calls f (AfterFunc).
class Timer : public object {
public:
    chan<Time> C;

    // Stops the timer. Returns false if it already fired or was stopped.
    bool Stop() noexcept;
    // Restarts the timer to fire after d. Returns whether it was active.
    bool Reset(Duration d) noexcept;

    ~Timer() override;

private:
    friend ptr<Timer> NewTimer(Duration d);
    friend ptr<Timer> AfterFunc(Duration d, std::function<void()> f);
    friend class Ticker;
    struct impl;
    Timer();
    impl* impl_;
};

// time.NewTimer
ptr<Timer> NewTimer(Duration d);

// time.After: a channel that receives the time after d.
chan<Time> After(Duration d);

// time.AfterFunc: calls f on its own fiber after d.
ptr<Timer> AfterFunc(Duration d, std::function<void()> f);

// time.Ticker: sends the time on c every period.
class Ticker : public object {
public:
    chan<Time> C;
    void Stop() noexcept;
    void Reset(Duration d) noexcept;
    ~Ticker() override;

private:
    friend ptr<Ticker> NewTicker(Duration d);
    Ticker();
    Timer::impl* impl_;
};

// time.NewTicker
ptr<Ticker> NewTicker(Duration d);

}  // namespace gopp::time

template <>
struct std::formatter<gopp::time::Duration> : std::formatter<std::string_view> {
    auto format(const gopp::time::Duration& d, auto& ctx) const {
        return std::formatter<std::string_view>::format(d.String().view(), ctx);
    }
};
