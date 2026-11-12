// Module gopp.crypto.tls: <gopp/crypto/tls.hpp>. Written by generate.py.
module;
#define GOPP_MODULE
export module gopp.crypto.tls;

import std.compat;
export import gopp.errors;
export import gopp.net;
export import gopp.ptr;
export import gopp.string;

export extern "C++" {
#include <gopp/crypto/tls.hpp>
}
