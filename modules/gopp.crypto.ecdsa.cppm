// Module gopp.crypto.ecdsa: <gopp/crypto/ecdsa.hpp>. Written by generate.py.
module;
#define GOPP_MODULE
export module gopp.crypto.ecdsa;

import std.compat;
export import gopp.base;
export import gopp.crypto;
export import gopp.crypto.elliptic;
export import gopp.errors;
export import gopp.io;
export import gopp.math.big;
export import gopp.ptr;
export import gopp.slice;

export extern "C++" {
#include <gopp/crypto/ecdsa.hpp>
}
