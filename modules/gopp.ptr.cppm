// Module gopp.ptr: <gopp/ptr.hpp>. Written by generate.py.
module;
#define GOPP_MODULE
export module gopp.ptr;

import std.compat;

export extern "C++" {
#include <gopp/ptr.hpp>
}
