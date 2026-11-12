// Module gopp.context: <gopp/context.hpp>. Written by generate.py.
module;
#define GOPP_MODULE
export module gopp.context;

import std.compat;
export import gopp.chan;
export import gopp.errors;
export import gopp.ptr;
export import gopp.time;

export extern "C++" {
#include <gopp/context.hpp>
}
