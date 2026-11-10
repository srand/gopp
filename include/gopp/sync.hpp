// The sync package: locks that park fibers instead of threads.
#pragma once

#include <gopp/base.hpp>

#include <atomic>
#include <functional>
#include <mutex>  // std::lock_guard and std::unique_lock work with these locks
#include <utility>

namespace gopp::sync {

// sync.Mutex, with Go's normal and starvation modes. Satisfies the C++
// Lockable requirements, so std::lock_guard and std::unique_lock work.
class Mutex {
public:
    Mutex() noexcept = default;
    Mutex(const Mutex&) = delete;
    Mutex& operator=(const Mutex&) = delete;

    void lock() noexcept {
        int32_t z = 0;
        if (state_.compare_exchange_strong(z, locked, std::memory_order_acquire)) return;
        lock_slow();
    }

    bool try_lock() noexcept {
        int32_t old = state_.load(std::memory_order_relaxed);
        if (old & (locked | starving)) return false;
        return state_.compare_exchange_strong(old, old | locked, std::memory_order_acquire);
    }

    void unlock() noexcept {
        int32_t n = state_.fetch_sub(locked, std::memory_order_release) - locked;
        if (n != 0) unlock_slow(n);
    }

    // Go spelling.
    void Lock() noexcept { lock(); }
    bool TryLock() noexcept { return try_lock(); }
    void Unlock() noexcept { unlock(); }

private:
    static constexpr int32_t locked = 1;
    static constexpr int32_t woken = 2;
    static constexpr int32_t starving = 4;
    static constexpr int waiter_shift = 3;

    void lock_slow() noexcept;
    void unlock_slow(int32_t n) noexcept;

    std::atomic<int32_t> state_{0};
    std::atomic<uint32_t> sema_{0};
};

// sync.RWMutex. Satisfies SharedLockable (std::shared_lock).
class RWMutex {
public:
    RWMutex() noexcept = default;
    RWMutex(const RWMutex&) = delete;
    RWMutex& operator=(const RWMutex&) = delete;

    void lock() noexcept;
    void unlock() noexcept;
    void lock_shared() noexcept;
    void unlock_shared() noexcept;

    // Go spelling.
    void Lock() noexcept { lock(); }
    void Unlock() noexcept { unlock(); }
    void RLock() noexcept { lock_shared(); }
    void RUnlock() noexcept { unlock_shared(); }

private:
    static constexpr int32_t max_readers = 1 << 30;

    Mutex w_;
    std::atomic<uint32_t> writer_sem_{0};
    std::atomic<uint32_t> reader_sem_{0};
    std::atomic<int32_t> reader_count_{0};
    std::atomic<int32_t> reader_wait_{0};
};

// sync.WaitGroup
class WaitGroup {
public:
    WaitGroup() noexcept = default;
    WaitGroup(const WaitGroup&) = delete;
    WaitGroup& operator=(const WaitGroup&) = delete;

    void Add(int delta) noexcept;
    void Done() noexcept { Add(-1); }
    void Wait() noexcept;

    // wg.Go(f): runs f on a new fiber, tracked by the group (Go 1.25).
    template <typename F>
    void Go(F&& f);

private:
    std::atomic<uint64_t> state_{0};  // high 32 bits: counter, low: waiters
    std::atomic<uint32_t> sema_{0};
};

// sync.Cond: fibers wait for a condition guarded by l. Wait() unlocks l,
// parks, and locks l again before returning; wake-ups may be spurious, so
// wait in a loop that checks the condition.
class Cond {
public:
    explicit Cond(Mutex* l) noexcept : L(l) {}
    Cond(const Cond&) = delete;
    Cond& operator=(const Cond&) = delete;

    Mutex* L;

    void Wait() noexcept;
    // Wakes one waiter.
    void Signal() noexcept;
    // Wakes all waiters.
    void Broadcast() noexcept;

private:
    struct waiter {
        std::atomic<uint32_t> sema{0};
        waiter* next = nullptr;
    };
    Mutex m_;
    waiter* head_ = nullptr;
    waiter* tail_ = nullptr;
};

// sync.Once
class Once {
public:
    // once.Do(f): calls f if Do hasn't been called before.
    template <typename F>
    void Do(F&& f) {
        if (done_.load(std::memory_order_acquire) == 0) call_slow(std::forward<F>(f));
    }

private:
    template <typename F>
    void call_slow(F&& f) {
        m_.lock();
        if (done_.load(std::memory_order_relaxed) == 0) {
            std::forward<F>(f)();
            done_.store(1, std::memory_order_release);
        }
        m_.unlock();
    }

    std::atomic<uint32_t> done_{0};
    Mutex m_;
};

}  // namespace gopp::sync

#include <gopp/runtime.hpp>

template <typename F>
void gopp::sync::WaitGroup::Go(F&& f) {
    Add(1);
    gopp::go([this, f = std::forward<F>(f)]() mutable {
        f();
        Done();
    });
}
