// Module gopp.slice: <gopp/slice.hpp>. Written by generate.py.
module;
#define GOPP_MODULE
export module gopp.slice;

import std.compat;
export import gopp.base;
export import gopp.string;

export extern "C++" {
#include <gopp/slice.hpp>
}
