// Module gopp.encoding.binary: <gopp/encoding/binary.hpp>. Written by generate.py.
module;
#define GOPP_MODULE
export module gopp.encoding.binary;

import std.compat;
export import gopp.errors;
export import gopp.io;
export import gopp.slice;
export import gopp.string;

export extern "C++" {
#include <gopp/encoding/binary.hpp>
}
