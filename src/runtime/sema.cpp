// Semaphores for sync.Mutex and friends, after Go's runtime/sema.Go.
//
// Waiters are kept in a table of buckets hashed by address. Go keeps a
// treap per bucket; a list is simpler and fine while few addresses share a
// bucket.
#include "runtime.hpp"

namespace gopp::runtime {

namespace {

struct semwaiter {
    G* g;
    std::atomic<uint32_t>* addr;
    semwaiter* next;
    semwaiter* prev;
    bool ticket;
};

struct alignas(64) semroot {
    lockT lock;
    std::atomic<uint32_t> nwait{0};
    semwaiter* head = nullptr;
    semwaiter* tail = nullptr;

    void queue(semwaiter* s, bool lifo) noexcept {
        if (lifo) {
            s->prev = nullptr;
            s->next = head;
            if (head) head->prev = s;
            else tail = s;
            head = s;
        } else {
            s->next = nullptr;
            s->prev = tail;
            if (tail) tail->next = s;
            else head = s;
            tail = s;
        }
    }

    semwaiter* dequeue(std::atomic<uint32_t>* addr) noexcept {
        for (semwaiter* s = head; s; s = s->next) {
            if (s->addr != addr) continue;
            if (s->prev) s->prev->next = s->next;
            else head = s->next;
            if (s->next) s->next->prev = s->prev;
            else tail = s->prev;
            return s;
        }
        return nullptr;
    }
};

constexpr int semtabsize = 251;
semroot semtable[semtabsize];

semroot& rootOf(std::atomic<uint32_t>* addr) noexcept {
    return semtable[(reinterpret_cast<uintptr_t>(addr) >> 3) % semtabsize];
}

bool cansemacquire(std::atomic<uint32_t>* addr) noexcept {
    uint32_t v = addr->load(std::memory_order_relaxed);
    while (v != 0) {
        if (addr->compare_exchange_weak(v, v - 1, std::memory_order_acquire, std::memory_order_relaxed)) return true;
    }
    return false;
}

}  // namespace

void semacquire(std::atomic<uint32_t>* addr, bool lifo, waitreason reason) noexcept {
    if (cansemacquire(addr)) return;
    semroot& root = rootOf(addr);
    semwaiter s{getg(), addr, nullptr, nullptr, false};
    for (;;) {
        root.lock.lock();
        root.nwait.fetch_add(1);
        if (cansemacquire(addr)) {
            root.nwait.fetch_sub(1);
            root.lock.unlock();
            return;
        }
        s.ticket = false;
        root.queue(&s, lifo);
        parkUnlock(&root.lock, reason);
        if (s.ticket || cansemacquire(addr)) return;
    }
}

void semrelease(std::atomic<uint32_t>* addr, bool handoff) noexcept {
    semroot& root = rootOf(addr);
    addr->fetch_add(1, std::memory_order_release);
    if (root.nwait.load() == 0) return;
    root.lock.lock();
    if (root.nwait.load() == 0) {
        root.lock.unlock();
        return;
    }
    semwaiter* s = root.dequeue(addr);
    if (s) root.nwait.fetch_sub(1);
    root.lock.unlock();
    if (s) {
        if (handoff && cansemacquire(addr)) s->ticket = true;
        G* gp = s->g;  // s lives on the waiter's stack: don't touch it after ready
        ready(gp);
    }
}

}  // namespace gopp::runtime
