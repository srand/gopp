// Module gopp.crypto.md5: <gopp/crypto/md5.hpp>. Written by generate.py.
module;
#define GOPP_MODULE
export module gopp.crypto.md5;

import std.compat;
export import gopp.hash;
export import gopp.ptr;

export extern "C++" {
#include <gopp/crypto/md5.hpp>
}
