// Module gopp.crypto: <gopp/crypto.hpp>. Written by generate.py.
module;
#define GOPP_MODULE
export module gopp.crypto;

import std.compat;
export import gopp.base;
export import gopp.errors;
export import gopp.hash;
export import gopp.io;
export import gopp.ptr;
export import gopp.slice;
export import gopp.string;

export extern "C++" {
#include <gopp/crypto.hpp>
}
