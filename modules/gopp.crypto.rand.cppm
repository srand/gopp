// Module gopp.crypto.rand: <gopp/crypto/rand.hpp>. Written by generate.py.
module;
#define GOPP_MODULE
export module gopp.crypto.rand;

import std.compat;
export import gopp.io;
export import gopp.string;

export extern "C++" {
#include <gopp/crypto/rand.hpp>
}
