// Module gopp.compress.zlib: <gopp/compress/zlib.hpp>. Written by generate.py.
module;
#define GOPP_MODULE
export module gopp.compress.zlib;

import std.compat;
export import gopp.compress.flate;
export import gopp.errors;
export import gopp.hash;
export import gopp.io;
export import gopp.ptr;
export import gopp.slice;

export extern "C++" {
#include <gopp/compress/zlib.hpp>
}
