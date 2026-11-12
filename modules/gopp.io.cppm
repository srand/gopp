// Module gopp.io: <gopp/io.hpp>. Written by generate.py.
module;
#define GOPP_MODULE
export module gopp.io;

import std.compat;
export import gopp.errors;
export import gopp.ptr;
export import gopp.slice;

export extern "C++" {
#include <gopp/io.hpp>
}
