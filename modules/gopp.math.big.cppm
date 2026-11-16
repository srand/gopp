// Module gopp.math.big: <gopp/math/big.hpp>. Written by generate.py.
module;
#define GOPP_MODULE
export module gopp.math.big;

import std.compat;
export import gopp.base;
export import gopp.ptr;
export import gopp.slice;
export import gopp.string;

export extern "C++" {
#include <gopp/math/big.hpp>
}
