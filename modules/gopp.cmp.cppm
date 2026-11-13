// Module gopp.cmp: <gopp/cmp.hpp>. Written by generate.py.
module;
#define GOPP_MODULE
export module gopp.cmp;

import std.compat;
export import gopp.base;

export extern "C++" {
#include <gopp/cmp.hpp>
}
