// Module gopp.unicode: <gopp/unicode.hpp>. Written by generate.py.
module;
#define GOPP_MODULE
export module gopp.unicode;

import std.compat;
export import gopp.base;

export extern "C++" {
#include <gopp/unicode.hpp>
}
