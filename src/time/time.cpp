#include "../runtime/runtime.hpp"
#include "internal.hpp"

#include <gopp/strings.hpp>
#include <gopp/time.hpp>

#include <time.h>

#include <cstdio>

namespace gopp::time {

namespace {
// Go's parseDurationError quotes with the time package's quote.
string strconv_quote(std::string_view s) { return string(quote(s)); }
}  // namespace

// ---------------------------------------------------------------------------
// Duration and Time

string Duration::String() const {
    // After Go's Duration.String: "1h2m3.5s", "1.5ms", "0s".
    char buf[40];
    int w = sizeof(buf);
    uint64 u = static_cast<uint64>(ns);
    bool neg = ns < 0;
    if (neg) u = -u;

    auto fmt_frac = [&](uint64 v, int prec) {
        bool print = false;
        for (int i = 0; i < prec; i++) {
            int digit = static_cast<int>(v % 10);
            print = print || digit != 0;
            if (print) buf[--w] = static_cast<char>('0' + digit);
            v /= 10;
        }
        if (print) buf[--w] = '.';
        return v;
    };
    auto fmt_int = [&](uint64 v) {
        if (v == 0) {
            buf[--w] = '0';
            return;
        }
        while (v > 0) {
            buf[--w] = static_cast<char>('0' + v % 10);
            v /= 10;
        }
    };

    if (u < 1000000000) {
        int prec;
        buf[--w] = 's';
        if (u == 0) return string::from_static("0s");
        if (u < 1000) {
            prec = 0;
            buf[--w] = 'n';
        } else if (u < 1000000) {
            prec = 3;
            buf[--w] = '\xb5';  // U+00B5 'µ' in UTF-8, before the 's'
            buf[--w] = '\xc2';
        } else {
            prec = 6;
            buf[--w] = 'm';
        }
        u = fmt_frac(u, prec);
        fmt_int(u);
    } else {
        buf[--w] = 's';
        u = fmt_frac(u, 9);
        fmt_int(u % 60);
        u /= 60;
        if (u > 0) {
            buf[--w] = 'm';
            fmt_int(u % 60);
            u /= 60;
            if (u > 0) {
                buf[--w] = 'h';
                fmt_int(u);
            }
        }
    }
    if (neg) buf[--w] = '-';
    return string(buf + w, sizeof(buf) - w);
}

namespace {

// Consumes leading digits; false on overflow (after Go's leadingInt).
bool leading_int(std::string_view& s, uint64& x) {
    x = 0;
    std::size_t i = 0;
    for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; i++) {
        if (x > (uint64(1) << 63) / 10) return false;
        x = x * 10 + static_cast<uint64>(s[i] - '0');
        if (x > uint64(1) << 63) return false;
    }
    s.remove_prefix(i);
    return true;
}

// Consumes leading digits as a fraction x / scale (after Go's leadingFraction).
void leading_fraction(std::string_view& s, uint64& x, double& scale) {
    x = 0;
    scale = 1;
    bool overflow = false;
    std::size_t i = 0;
    for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; i++) {
        if (overflow) continue;
        if (x > ((uint64(1) << 63) - 1) / 10) {
            overflow = true;
            continue;
        }
        uint64 y = x * 10 + static_cast<uint64>(s[i] - '0');
        if (y > uint64(1) << 63) {
            overflow = true;
            continue;
        }
        x = y;
        scale *= 10;
    }
    s.remove_prefix(i);
}

}  // namespace

std::pair<Duration, error> ParseDuration(std::string_view s) {
    std::string_view orig = s;
    auto invalid = [&](std::string_view what) {
        return std::pair<Duration, error>{Duration(), errors::New("time: " + string(what) + " " + strconv_quote(orig))};
    };
    uint64 d = 0;
    bool neg = false;
    if (!s.empty() && (s[0] == '-' || s[0] == '+')) {
        neg = s[0] == '-';
        s.remove_prefix(1);
    }
    if (s == "0") return {Duration(), nullptr};
    if (s.empty()) return invalid("invalid duration");
    while (!s.empty()) {
        uint64 v = 0, f = 0;
        double scale = 1;
        if (!(s[0] == '.' || (s[0] >= '0' && s[0] <= '9'))) return invalid("invalid duration");
        std::size_t pl = s.size();
        if (!leading_int(s, v)) return invalid("invalid duration");
        bool pre = pl != s.size();
        bool post = false;
        if (!s.empty() && s[0] == '.') {
            s.remove_prefix(1);
            pl = s.size();
            leading_fraction(s, f, scale);
            post = pl != s.size();
        }
        if (!pre && !post) return invalid("invalid duration");
        std::size_t i = 0;
        while (i < s.size() && s[i] != '.' && !(s[i] >= '0' && s[i] <= '9')) i++;
        if (i == 0) return invalid("missing unit in duration");
        std::string_view u = s.substr(0, i);
        s.remove_prefix(i);
        uint64 unit;
        if (u == "ns") unit = 1;
        else if (u == "us" || u == "\xc2\xb5s" || u == "\xce\xbcs") unit = 1000;
        else if (u == "ms") unit = 1000000;
        else if (u == "s") unit = 1000000000;
        else if (u == "m") unit = 60 * uint64(1000000000);
        else if (u == "h") unit = 3600 * uint64(1000000000);
        else return {Duration(), errors::New("time: unknown unit " + strconv_quote(u) + " in duration " + strconv_quote(orig))};
        if (v > (uint64(1) << 63) / unit) return invalid("invalid duration");
        v *= unit;
        if (f > 0) {
            v += static_cast<uint64>(static_cast<double>(f) * (static_cast<double>(unit) / scale));
            if (v > uint64(1) << 63) return invalid("invalid duration");
        }
        d += v;
        if (d > uint64(1) << 63) return invalid("invalid duration");
    }
    if (neg) return {Duration(-static_cast<int64>(d)), nullptr};
    if (d > (uint64(1) << 63) - 1) return invalid("invalid duration");
    return {Duration(static_cast<int64>(d)), nullptr};
}

Time Now() noexcept {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    Time t = Time::internal::make(static_cast<int64>(ts.tv_sec) + Time::unixToInternal, static_cast<int32>(ts.tv_nsec),
                                  Local);
    Time::internal::setMono(t, runtime::nanotime());
    return t;
}

// ---------------------------------------------------------------------------
// Sleep

void Sleep(Duration d) {
    if (d.ns <= 0) return;
    runtime::timer t;
    t.when = runtime::nanotime() + d.ns;
    t.f = [](void* arg, uint64_t) { runtime::ready(static_cast<runtime::G*>(arg)); };
    t.arg = runtime::getg();
    // Schedule the timer only once the fiber is parked, so that it can't
    // be readied while still running.
    runtime::park(
        [](runtime::G*, void* arg) {
            runtime::add_timer(static_cast<runtime::timer*>(arg));
            return true;
        },
        &t);
}

// ---------------------------------------------------------------------------
// Timers
//
// An armed timer holds a reference to its owner, so that time::After works
// without keeping the Timer. Each arming has a generation; a callback of an
// older generation, already running when the timer was stopped or reset,
// only drops its reference.

struct Timer::impl {
    runtime::lock_t mu;
    runtime::timer t;
    object* owner = nullptr;
    chan<Time> c;  // for NewTimer and NewTicker
    std::function<void()> f;  // for AfterFunc
    int64 period = 0;
    uint64 gen = 0;
    bool active = false;

    static void fire(void* arg, uint64_t seq) {
        auto* im = static_cast<impl*>(arg);
        im->mu.lock();
        if (seq != im->gen || !im->active) {
            im->mu.unlock();
            im->owner->release_ref();
            return;
        }
        if (im->f) {
            gopp::go(im->f);
        } else {
            Time n = Now();
            im->c.try_send(n);
        }
        if (im->period > 0) {
            im->t.when = runtime::nanotime() + im->period;
            runtime::add_timer(&im->t);  // keeps the reference
            im->mu.unlock();
            return;
        }
        im->active = false;
        im->mu.unlock();
        im->owner->release_ref();
    }

    // mu must be held.
    void arm(int64 d) {
        gen++;
        active = true;
        owner->add_ref();
        t.when = runtime::nanotime() + (d > 0 ? d : 0);
        if (t.when <= 0) t.when = 1;
        t.period = 0;
        t.f = fire;
        t.arg = this;
        t.seq = gen;
        runtime::add_timer(&t);
    }

    bool stop() {
        mu.lock();
        gen++;
        active = false;
        bool removed = runtime::del_timer(&t);
        mu.unlock();
        if (removed) owner->release_ref();
        return removed;
    }

    bool reset(int64 d) {
        mu.lock();
        bool removed = runtime::del_timer(&t);
        if (removed) owner->release_ref();
        arm(d);
        mu.unlock();
        return removed;
    }
};

Timer::Timer() : impl_(new impl()) { impl_->owner = this; }

Timer::~Timer() { delete impl_; }

bool Timer::Stop() noexcept { return impl_->stop(); }

bool Timer::Reset(Duration d) noexcept { return impl_->reset(d.ns); }

ptr<Timer> NewTimer(Duration d) {
    ptr<Timer> t(new Timer());
    t->C = make_chan<Time>(1);
    t->impl_->c = t->C;
    t->impl_->mu.lock();
    t->impl_->arm(d.ns);
    t->impl_->mu.unlock();
    return t;
}

chan<Time> After(Duration d) { return NewTimer(d)->C; }

ptr<Timer> AfterFunc(Duration d, std::function<void()> f) {
    ptr<Timer> t(new Timer());
    t->impl_->f = std::move(f);
    t->impl_->mu.lock();
    t->impl_->arm(d.ns);
    t->impl_->mu.unlock();
    return t;
}

Ticker::Ticker() : impl_(new Timer::impl()) { impl_->owner = this; }

Ticker::~Ticker() { delete impl_; }

void Ticker::Stop() noexcept { impl_->stop(); }

void Ticker::Reset(Duration d) noexcept {
    if (d.ns <= 0) panic("non-positive interval for Ticker.Reset");
    impl_->mu.lock();
    impl_->period = d.ns;
    impl_->mu.unlock();
    impl_->reset(d.ns);
}

ptr<Ticker> NewTicker(Duration d) {
    if (d.ns <= 0) panic("non-positive interval for NewTicker");
    ptr<Ticker> t(new Ticker());
    t->C = make_chan<Time>(1);
    t->impl_->c = t->C;
    t->impl_->period = d.ns;
    t->impl_->mu.lock();
    t->impl_->arm(d.ns);
    t->impl_->mu.unlock();
    return t;
}

}  // namespace gopp::time
