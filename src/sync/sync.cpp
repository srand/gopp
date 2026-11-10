// After Go's internal/sync/mutex.Go, sync/rwmutex.Go and sync/waitgroup.Go.
#include "../runtime/runtime.hpp"

#include <gopp/sync.hpp>

namespace gopp::sync {

namespace {

constexpr int64_t starvation_threshold_ns = 1000000;

bool can_spin(int iter) noexcept { return iter < 4 && runtime::NumCPU() > 1; }

void do_spin() noexcept {
    for (int i = 0; i < 30; i++) runtime::cpu_relax();
}

}  // namespace

void Mutex::lock_slow() noexcept {
    int64_t wait_start = 0;
    bool is_starving = false;
    bool awoke = false;
    int iter = 0;
    int32_t old = state_.load(std::memory_order_relaxed);
    for (;;) {
        // Spin while the mutex is locked in normal mode.
        if ((old & (locked | starving)) == locked && can_spin(iter)) {
            if (!awoke && (old & woken) == 0 && (old >> waiter_shift) != 0 &&
                state_.compare_exchange_strong(old, old | woken)) {
                awoke = true;
            }
            do_spin();
            iter++;
            old = state_.load(std::memory_order_relaxed);
            continue;
        }
        int32_t nw = old;
        if ((old & starving) == 0) nw |= locked;  // don't take a starving mutex: queue
        if ((old & (locked | starving)) != 0) nw += 1 << waiter_shift;
        if (is_starving && (old & locked) != 0) nw |= starving;
        if (awoke) {
            if ((nw & woken) == 0) runtime::fatal("sync: inconsistent mutex state");
            nw &= ~woken;
        }
        if (state_.compare_exchange_strong(old, nw, std::memory_order_acquire)) {
            if ((old & (locked | starving)) == 0) break;  // locked with CAS
            bool lifo = wait_start != 0;
            if (wait_start == 0) wait_start = runtime::nanotime();
            runtime::semacquire(&sema_, lifo);
            is_starving = is_starving || runtime::nanotime() - wait_start > starvation_threshold_ns;
            old = state_.load(std::memory_order_relaxed);
            if ((old & starving) != 0) {
                // Handed off in starvation mode.
                if ((old & (locked | woken)) != 0 || (old >> waiter_shift) == 0)
                    runtime::fatal("sync: inconsistent mutex state");
                int32_t delta = locked - (1 << waiter_shift);
                if (!is_starving || (old >> waiter_shift) == 1) delta -= starving;
                state_.fetch_add(delta, std::memory_order_acquire);
                break;
            }
            awoke = true;
            iter = 0;
        }
    }
}

void Mutex::unlock_slow(int32_t nw) noexcept {
    if (((nw + locked) & locked) == 0) runtime::fatal("sync: unlock of unlocked mutex");
    if ((nw & starving) == 0) {
        int32_t old = nw;
        for (;;) {
            if ((old >> waiter_shift) == 0 || (old & (locked | woken | starving)) != 0) return;
            nw = (old - (1 << waiter_shift)) | woken;
            if (state_.compare_exchange_strong(old, nw)) {
                runtime::semrelease(&sema_, false);
                return;
            }
        }
    } else {
        runtime::semrelease(&sema_, true);
    }
}

void RWMutex::lock_shared() noexcept {
    if (reader_count_.fetch_add(1, std::memory_order_acquire) + 1 < 0) runtime::semacquire(&reader_sem_);
}

void RWMutex::unlock_shared() noexcept {
    int32_t r = reader_count_.fetch_sub(1, std::memory_order_release) - 1;
    if (r < 0) {
        if (r + 1 == 0 || r + 1 == -max_readers) runtime::fatal("sync: RUnlock of unlocked RWMutex");
        if (reader_wait_.fetch_sub(1) - 1 == 0) runtime::semrelease(&writer_sem_);
    }
}

void RWMutex::lock() noexcept {
    w_.lock();
    int32_t r = reader_count_.fetch_add(-max_readers, std::memory_order_acquire);
    if (r != 0 && reader_wait_.fetch_add(r) + r != 0) runtime::semacquire(&writer_sem_);
}

void RWMutex::unlock() noexcept {
    int32_t r = reader_count_.fetch_add(max_readers, std::memory_order_release) + max_readers;
    if (r >= max_readers) runtime::fatal("sync: Unlock of unlocked RWMutex");
    for (int32_t i = 0; i < r; i++) runtime::semrelease(&reader_sem_);
    w_.unlock();
}

void WaitGroup::Add(int delta) noexcept {
    uint64_t st = state_.fetch_add(static_cast<uint64_t>(static_cast<int64_t>(delta)) << 32, std::memory_order_acq_rel) +
                  (static_cast<uint64_t>(static_cast<int64_t>(delta)) << 32);
    int32_t v = static_cast<int32_t>(st >> 32);
    uint32_t w = static_cast<uint32_t>(st);
    if (v < 0) panic("sync: negative WaitGroup counter");
    if (w != 0 && delta > 0 && v == delta) panic("sync: WaitGroup misuse: Add called concurrently with Wait");
    if (v > 0 || w == 0) return;
    // The counter reached 0 with waiters: wake them all.
    if (state_.load() != st) panic("sync: WaitGroup misuse: Add called concurrently with Wait");
    state_.store(0);
    for (; w != 0; w--) runtime::semrelease(&sema_);
}

void WaitGroup::Wait() noexcept {
    uint64_t st = state_.load(std::memory_order_acquire);
    for (;;) {
        int32_t v = static_cast<int32_t>(st >> 32);
        if (v == 0) return;
        if (state_.compare_exchange_weak(st, st + 1, std::memory_order_acq_rel)) {
            runtime::semacquire(&sema_);
            if (state_.load() != 0) panic("sync: WaitGroup is reused before previous Wait has returned");
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// Cond: a list of waiters, each parked on its own semaphore. A waiter
// that is signaled before it parks finds its semaphore released already.

void Cond::Wait() noexcept {
    waiter w;
    m_.lock();
    if (tail_) tail_->next = &w;
    else head_ = &w;
    tail_ = &w;
    m_.unlock();
    L->unlock();
    runtime::semacquire(&w.sema);
    L->lock();
}

void Cond::Signal() noexcept {
    m_.lock();
    waiter* w = head_;
    if (w) {
        head_ = w->next;
        if (!head_) tail_ = nullptr;
    }
    m_.unlock();
    if (w) runtime::semrelease(&w->sema);
}

void Cond::Broadcast() noexcept {
    m_.lock();
    waiter* w = head_;
    head_ = tail_ = nullptr;
    m_.unlock();
    while (w) {
        waiter* next = w->next;  // w may be gone once released
        runtime::semrelease(&w->sema);
        w = next;
    }
}

}  // namespace gopp::sync
