#include "stack.hpp"
#include "runtime.hpp"

#include <sys/mman.h>
#include <unistd.h>

#include <cstdlib>
#include <mutex>

namespace gopp::runtime {

namespace {

struct freeStack {
    freeStack* next;
};

std::size_t pageSize() noexcept {
    static const std::size_t ps = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
    return ps;
}

// A small per-thread cache in front of a global list.
constexpr int cacheMax = 16;

struct stackCache {
    void* stacks[cacheMax];
    int n = 0;
};

thread_local stackCache tlsCache;

lockT globalLock;
freeStack* globalFree = nullptr;

}  // namespace

std::size_t stackSize() noexcept {
    static const std::size_t size = [] {
        std::size_t s = 256 * 1024;
        if (const char* e = std::getenv("GOPP_STACK_SIZE")) {
            long v = std::atol(e);
            if (v >= 16 * 1024) s = static_cast<std::size_t>(v);
        }
        std::size_t ps = pageSize();
        return (s + ps - 1) / ps * ps;
    }();
    return size;
}

boost::context::stack_context stackAllocator::allocate() {
    std::size_t size = stackSize();
    void* base = nullptr;  // lowest usable address

    stackCache& c = tlsCache;
    if (c.n > 0) {
        base = c.stacks[--c.n];
    } else {
        globalLock.lock();
        if (globalFree) {
            base = globalFree;
            globalFree = globalFree->next;
        }
        globalLock.unlock();
    }

    if (!base) {
        std::size_t guard = pageSize();
        void* mem = mmap(nullptr, size + guard, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
        if (mem == MAP_FAILED) fatal("out of memory allocating fiber stack");
        mprotect(mem, guard, PROT_NONE);
        base = static_cast<char*>(mem) + guard;
    }

    boost::context::stack_context sctx;
    sctx.size = size;
    sctx.sp = static_cast<char*>(base) + size;
    if (gp) {
        gp->stackLo = reinterpret_cast<uintptr_t>(base);
        gp->stackHi = reinterpret_cast<uintptr_t>(sctx.sp);
    }
    return sctx;
}

void stackAllocator::deallocate(boost::context::stack_context& sctx) noexcept {
    void* base = static_cast<char*>(sctx.sp) - sctx.size;
    stackCache& c = tlsCache;
    if (c.n < cacheMax) {
        c.stacks[c.n++] = base;
        return;
    }
    auto* fs = static_cast<freeStack*>(base);
    globalLock.lock();
    fs->next = globalFree;
    globalFree = fs;
    globalLock.unlock();
}

}  // namespace gopp::runtime
