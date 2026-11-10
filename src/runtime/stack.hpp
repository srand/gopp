// Fiber stacks: mmap'd with a guard page below, and cached for reuse.
//
// Go grows stacks; fibers can't, so each gets a fixed virtual size
// (GOPP_STACK_SIZE, default 256 KiB). Only the pages a fiber touches use
// memory.
#pragma once

#include <boost/context/stack_context.hpp>

#include <cstddef>

namespace gopp::runtime {

std::size_t stack_size() noexcept;

struct stack_allocator {
    boost::context::stack_context allocate();
    void deallocate(boost::context::stack_context& sctx) noexcept;
};

}  // namespace gopp::runtime
