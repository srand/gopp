// Module gopp.crypto.hmac: <gopp/crypto/hmac.hpp>. Written by generate.py.
module;
#define GOPP_MODULE
export module gopp.crypto.hmac;

import std.compat;
export import gopp.hash;
export import gopp.ptr;

export extern "C++" {
#include <gopp/crypto/hmac.hpp>
}
