// Module gopp.bytes: <gopp/bytes.hpp>. Written by generate.py.
module;
#define GOPP_MODULE
export module gopp.bytes;

import std.compat;
export import gopp.errors;
export import gopp.io;
export import gopp.ptr;
export import gopp.slice;
export import gopp.string;
export import gopp.unicode;

export extern "C++" {
#include <gopp/bytes.hpp>
}
