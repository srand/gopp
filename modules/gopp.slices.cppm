// Module gopp.slices: <gopp/slices.hpp>. Written by generate.py.
module;
#define GOPP_MODULE
export module gopp.slices;

import std.compat;
export import gopp.base;
export import gopp.cmp;
export import gopp.slice;

export extern "C++" {
#include <gopp/slices.hpp>
}
