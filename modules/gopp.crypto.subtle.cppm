// Module gopp.crypto.subtle: <gopp/crypto/subtle.hpp>. Written by generate.py.
module;
#define GOPP_MODULE
export module gopp.crypto.subtle;

import std.compat;
export import gopp.base;

export extern "C++" {
#include <gopp/crypto/subtle.hpp>
}
