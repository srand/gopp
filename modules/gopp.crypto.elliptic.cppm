// Module gopp.crypto.elliptic: <gopp/crypto/elliptic.hpp>. Written by generate.py.
module;
#define GOPP_MODULE
export module gopp.crypto.elliptic;

import std.compat;
export import gopp.base;
export import gopp.math.big;
export import gopp.ptr;
export import gopp.string;

export extern "C++" {
#include <gopp/crypto/elliptic.hpp>
}
