// Timers: one heap for the process, run by the scheduler of whichever M
// looks for work first once they are due. The M that blocks in netpoll
// sleeps until the earliest timer.
//
// Go keeps a heap per P; one heap under a lock is simpler and enough for
// the prototype.
#include "runtime.hpp"

#include <vector>

namespace gopp::runtime {

namespace {

struct timers_t {
    lock_t lock;
    std::vector<timer*> heap;
    std::atomic<int64_t> next{INT64_MAX};
};

timers_t& timers() {
    static timers_t* t = new timers_t();  // never destroyed
    return *t;
}

void sift_up(std::vector<timer*>& h, int32_t i) noexcept {
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

void sift_down(std::vector<timer*>& h, int32_t i) noexcept {
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

void remove_at(std::vector<timer*>& h, int32_t i) noexcept {
    timer* t = h[i];
    timer* last = h.back();
    h.pop_back();
    t->idx = -1;
    if (last != t) {
        h[i] = last;
        last->idx = i;
        sift_down(h, i);
        sift_up(h, last->idx);
    }
}

// timers().lock must be held.
void push(timer* t) noexcept {
    auto& h = timers().heap;
    h.push_back(t);
    sift_up(h, static_cast<int32_t>(h.size() - 1));
}

void update_next() noexcept {
    auto& ts = timers();
    ts.next.store(ts.heap.empty() ? INT64_MAX : ts.heap[0]->when, std::memory_order_release);
}

}  // namespace

void add_timer(timer* t) noexcept {
    ensure_init();
    auto& ts = timers();
    if (t->when <= 0) t->when = INT64_MAX;
    ts.lock.lock();
    if (t->idx >= 0) fatal("add_timer: timer already scheduled");
    push(t);
    bool earliest = ts.heap[0] == t;
    update_next();
    int64_t when = t->when;
    ts.lock.unlock();
    if (earliest) wake_netpoller(when);
}

bool del_timer(timer* t) noexcept {
    auto& ts = timers();
    ts.lock.lock();
    bool pending = t->idx >= 0;
    if (pending) {
        remove_at(ts.heap, t->idx);
        update_next();
    }
    ts.lock.unlock();
    return pending;
}

bool mod_timer(timer* t, int64_t when) noexcept {
    auto& ts = timers();
    ts.lock.lock();
    bool pending = t->idx >= 0;
    if (pending) remove_at(ts.heap, t->idx);
    t->when = when <= 0 ? INT64_MAX : when;
    push(t);
    bool earliest = ts.heap[0] == t;
    update_next();
    ts.lock.unlock();
    if (earliest) wake_netpoller(t->when);
    return pending;
}

int64_t next_timer() noexcept { return timers().next.load(std::memory_order_acquire); }

int64_t run_timers(int64_t now) noexcept {
    auto& ts = timers();
    ts.lock.lock();
    while (!ts.heap.empty() && ts.heap[0]->when <= now) {
        timer* t = ts.heap[0];
        remove_at(ts.heap, 0);
        auto f = t->f;
        void* arg = t->arg;
        uint64_t seq = t->seq;
        if (t->period > 0) {
            t->when += t->period * (1 + (now - t->when) / t->period);
            push(t);
        }
        update_next();
        ts.lock.unlock();
        f(arg, seq);  // may not block; t may be gone after this
        ts.lock.lock();
    }
    update_next();
    int64_t next = ts.heap.empty() ? INT64_MAX : ts.heap[0]->when;
    ts.lock.unlock();
    return next;
}

}  // namespace gopp::runtime
