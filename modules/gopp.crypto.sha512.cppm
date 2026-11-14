// Module gopp.crypto.sha512: <gopp/crypto/sha512.hpp>. Written by generate.py.
module;
#define GOPP_MODULE
export module gopp.crypto.sha512;

import std.compat;
export import gopp.hash;
export import gopp.ptr;

export extern "C++" {
#include <gopp/crypto/sha512.hpp>
}
