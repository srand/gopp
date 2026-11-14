// Module gopp.crypto.sha1: <gopp/crypto/sha1.hpp>. Written by generate.py.
module;
#define GOPP_MODULE
export module gopp.crypto.sha1;

import std.compat;
export import gopp.hash;
export import gopp.ptr;

export extern "C++" {
#include <gopp/crypto/sha1.hpp>
}
