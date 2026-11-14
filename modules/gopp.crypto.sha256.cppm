// Module gopp.crypto.sha256: <gopp/crypto/sha256.hpp>. Written by generate.py.
module;
#define GOPP_MODULE
export module gopp.crypto.sha256;

import std.compat;
export import gopp.hash;
export import gopp.ptr;

export extern "C++" {
#include <gopp/crypto/sha256.hpp>
}
