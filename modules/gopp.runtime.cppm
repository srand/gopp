// Module gopp.runtime: <gopp/runtime.hpp>. Written by generate.py.
module;
#define GOPP_MODULE
export module gopp.runtime;

import std.compat;
export import gopp.base;

export extern "C++" {
#include <gopp/runtime.hpp>
}
