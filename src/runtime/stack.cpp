#include "stack.hpp"
#include "runtime.hpp"

#include <sys/mman.h>
#include <unistd.h>

#include <cstdlib>
#include <mutex>

namespace gopp::runtime {

namespace {

struct free_stack {
    free_stack* next;
};

std::size_t page_size() noexcept {
    static const std::size_t ps = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
    return ps;
}

// A small per-thread cache in front of a global list.
constexpr int cache_max = 16;

struct stack_cache {
    void* stacks[cache_max];
    int n = 0;
};

thread_local stack_cache tls_cache;

lock_t global_lock;
free_stack* global_free = nullptr;

}  // namespace

std::size_t stack_size() noexcept {
    static const std::size_t size = [] {
        std::size_t s = 256 * 1024;
        if (const char* e = std::getenv("GOPP_STACK_SIZE")) {
            long v = std::atol(e);
            if (v >= 16 * 1024) s = static_cast<std::size_t>(v);
        }
        std::size_t ps = page_size();
        return (s + ps - 1) / ps * ps;
    }();
    return size;
}

boost::context::stack_context stack_allocator::allocate() {
    std::size_t size = stack_size();
    void* base = nullptr;  // lowest usable address

    stack_cache& c = tls_cache;
    if (c.n > 0) {
        base = c.stacks[--c.n];
    } else {
        global_lock.lock();
        if (global_free) {
            base = global_free;
            global_free = global_free->next;
        }
        global_lock.unlock();
    }

    if (!base) {
        std::size_t guard = page_size();
        void* mem = mmap(nullptr, size + guard, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
        if (mem == MAP_FAILED) fatal("out of memory allocating fiber stack");
        mprotect(mem, guard, PROT_NONE);
        base = static_cast<char*>(mem) + guard;
    }

    boost::context::stack_context sctx;
    sctx.size = size;
    sctx.sp = static_cast<char*>(base) + size;
    return sctx;
}

void stack_allocator::deallocate(boost::context::stack_context& sctx) noexcept {
    void* base = static_cast<char*>(sctx.sp) - sctx.size;
    stack_cache& c = tls_cache;
    if (c.n < cache_max) {
        c.stacks[c.n++] = base;
        return;
    }
    auto* fs = static_cast<free_stack*>(base);
    global_lock.lock();
    fs->next = global_free;
    global_free = fs;
    global_lock.unlock();
}

}  // namespace gopp::runtime
