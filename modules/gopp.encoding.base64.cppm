// Module gopp.encoding.base64: <gopp/encoding/base64.hpp>. Written by generate.py.
module;
#define GOPP_MODULE
export module gopp.encoding.base64;

import std.compat;
export import gopp.errors;
export import gopp.io;
export import gopp.ptr;
export import gopp.slice;
export import gopp.string;

export extern "C++" {
#include <gopp/encoding/base64.hpp>
}
