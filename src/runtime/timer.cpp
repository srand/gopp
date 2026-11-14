// Timers: a heap per P, as in Go. A timer joins the heap of the P its
// fiber runs on, so that fibers on different Ps arm and stop timers
// without contending for one lock. Any M may run any P's due timers: an M
// runs its own P's when it schedules, and those of all Ps before it goes
// idle; the M that blocks in netpoll sleeps until the earliest timer of
// all; and sysmon runs the late ones.
#include "runtime.hpp"

namespace gopp::runtime {

namespace {

void siftUp(std::vector<timer*>& h, int32_t i) noexcept {
    timer* t = h[i];
    while (i > 0) {
        int32_t p = (i - 1) / 2;
        if (h[p]->when <= t->when) break;
        h[i] = h[p];
        h[i]->idx = i;
        i = p;
    }
    h[i] = t;
    t->idx = i;
}

void siftDown(std::vector<timer*>& h, int32_t i) noexcept {
    int32_t n = static_cast<int32_t>(h.size());
    timer* t = h[i];
    for (;;) {
        int32_t c = 2 * i + 1;
        if (c >= n) break;
        if (c + 1 < n && h[c + 1]->when < h[c]->when) c++;
        if (t->when <= h[c]->when) break;
        h[i] = h[c];
        h[i]->idx = i;
        i = c;
    }
    h[i] = t;
    t->idx = i;
}

// pp's timer lock must be held.
void removeAt(P* pp, int32_t i) noexcept {
    auto& h = pp->timers.heap;
    timer* t = h[i];
    timer* last = h.back();
    h.pop_back();
    t->idx = -1;
    t->pp.store(nullptr, std::memory_order_relaxed);
    if (last != t) {
        h[i] = last;
        last->idx = i;
        siftDown(h, i);
        siftUp(h, last->idx);
    }
}

// pp's timer lock must be held.
void push(P* pp, timer* t) noexcept {
    auto& h = pp->timers.heap;
    h.push_back(t);
    t->pp.store(pp, std::memory_order_relaxed);
    siftUp(h, static_cast<int32_t>(h.size() - 1));
}

void updateNext(P* pp) noexcept {
    auto& ts = pp->timers;
    ts.next.store(ts.heap.empty() ? INT64_MAX : ts.heap[0]->when, std::memory_order_release);
}

// Locks the heap that holds t and returns its P, or returns nullptr if t
// isn't scheduled. The callers serialize the operations on a timer, but a
// timer that fires moves out of its heap under another M.
P* lockTimer(timer* t) noexcept {
    for (;;) {
        P* pp = t->pp.load(std::memory_order_acquire);
        if (!pp) return nullptr;
        pp->timers.lock.lock();
        if (t->pp.load(std::memory_order_relaxed) == pp) return pp;
        pp->timers.lock.unlock();
    }
}

}  // namespace

void addTimer(timer* t) noexcept {
    ensureInit();
    if (t->when <= 0) t->when = INT64_MAX;
    P* pp = timerP();
    pp->timers.lock.lock();
    if (t->idx >= 0) fatal("add_timer: timer already scheduled");
    push(pp, t);
    bool earliest = pp->timers.heap[0] == t;
    updateNext(pp);
    int64_t when = t->when;
    pp->timers.lock.unlock();
    if (earliest) wakeNetpoller(when);
}

bool delTimer(timer* t) noexcept {
    P* pp = lockTimer(t);
    if (!pp) return false;
    removeAt(pp, t->idx);
    updateNext(pp);
    pp->timers.lock.unlock();
    return true;
}

bool modTimer(timer* t, int64_t when) noexcept {
    ensureInit();
    P* pp = lockTimer(t);
    bool pending = pp != nullptr;
    if (pending) {
        removeAt(pp, t->idx);
    } else {
        pp = timerP();
        pp->timers.lock.lock();
    }
    t->when = when <= 0 ? INT64_MAX : when;
    push(pp, t);
    bool earliest = pp->timers.heap[0] == t;
    updateNext(pp);
    when = t->when;
    pp->timers.lock.unlock();
    if (earliest) wakeNetpoller(when);
    return pending;
}

int64_t nextTimer() noexcept {
    int64_t next = INT64_MAX;
    for (P& pp : allPs()) next = std::min(next, pp.timers.next.load(std::memory_order_acquire));
    return next;
}

int64_t runTimers(P* pp, int64_t now) noexcept {
    auto& ts = pp->timers;
    if (ts.next.load(std::memory_order_acquire) > now) return ts.next.load(std::memory_order_relaxed);
    ts.lock.lock();
    while (!ts.heap.empty() && ts.heap[0]->when <= now) {
        timer* t = ts.heap[0];
        removeAt(pp, 0);
        auto f = t->f;
        void* arg = t->arg;
        uint64_t seq = t->seq;
        if (t->period > 0) {
            t->when += t->period * (1 + (now - t->when) / t->period);
            push(pp, t);
        }
        updateNext(pp);
        ts.lock.unlock();
        f(arg, seq);  // may not block; t may be gone after this
        ts.lock.lock();
    }
    updateNext(pp);
    int64_t next = ts.heap.empty() ? INT64_MAX : ts.heap[0]->when;
    ts.lock.unlock();
    return next;
}

void runDueTimers(int64_t now) noexcept {
    for (P& pp : allPs()) {
        if (pp.timers.next.load(std::memory_order_acquire) <= now) runTimers(&pp, now);
    }
}

}  // namespace gopp::runtime
