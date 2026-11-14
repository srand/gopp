// Module gopp.hash.adler32: <gopp/hash/adler32.hpp>. Written by generate.py.
module;
#define GOPP_MODULE
export module gopp.hash.adler32;

import std.compat;
export import gopp.hash;
export import gopp.ptr;

export extern "C++" {
#include <gopp/hash/adler32.hpp>
}
