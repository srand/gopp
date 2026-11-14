// Module gopp.compress.flate: <gopp/compress/flate.hpp>. Written by generate.py.
module;
#define GOPP_MODULE
export module gopp.compress.flate;

import std.compat;
export import gopp.errors;
export import gopp.io;
export import gopp.ptr;
export import gopp.string;

export extern "C++" {
#include <gopp/compress/flate.hpp>
}
