// Module gopp.string: <gopp/string.hpp>. Written by generate.py.
module;
#define GOPP_MODULE
export module gopp.string;

import std.compat;
export import gopp.base;

export extern "C++" {
#include <gopp/string.hpp>
}
