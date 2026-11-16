// Module gopp.crypto.ed25519: <gopp/crypto/ed25519.hpp>. Written by generate.py.
module;
#define GOPP_MODULE
export module gopp.crypto.ed25519;

import std.compat;
export import gopp.base;
export import gopp.crypto;
export import gopp.errors;
export import gopp.io;
export import gopp.ptr;
export import gopp.slice;

export extern "C++" {
#include <gopp/crypto/ed25519.hpp>
}
