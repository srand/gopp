// Module gopp.container.heap: <gopp/container/heap.hpp>. Written by generate.py.
module;
#define GOPP_MODULE
export module gopp.container.heap;

import std.compat;
export import gopp.sort;

export extern "C++" {
#include <gopp/container/heap.hpp>
}
