// Module gopp.hash: <gopp/hash.hpp>. Written by generate.py.
module;
#define GOPP_MODULE
export module gopp.hash;

import std.compat;
export import gopp.io;
export import gopp.slice;

export extern "C++" {
#include <gopp/hash.hpp>
}
