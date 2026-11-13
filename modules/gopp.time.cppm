// Module gopp.time: <gopp/time.hpp>. Written by generate.py.
module;
#define GOPP_MODULE
export module gopp.time;

import std.compat;
export import gopp.base;
export import gopp.chan;
export import gopp.errors;
export import gopp.ptr;
export import gopp.slice;
export import gopp.string;

export extern "C++" {
#include <gopp/time.hpp>
}
