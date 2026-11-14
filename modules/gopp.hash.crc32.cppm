// Module gopp.hash.crc32: <gopp/hash/crc32.hpp>. Written by generate.py.
module;
#define GOPP_MODULE
export module gopp.hash.crc32;

import std.compat;
export import gopp.hash;
export import gopp.ptr;

export extern "C++" {
#include <gopp/hash/crc32.hpp>
}
