// Module gopp.sync.atomic: <gopp/sync/atomic.hpp>. Written by generate.py.
module;
#define GOPP_MODULE
export module gopp.sync.atomic;

import std.compat;
export import gopp.base;
export import gopp.ptr;

export extern "C++" {
#include <gopp/sync/atomic.hpp>
}
