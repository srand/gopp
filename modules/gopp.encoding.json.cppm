// Module gopp.encoding.json: <gopp/encoding/json.hpp>. Written by generate.py.
module;
#define GOPP_MODULE
export module gopp.encoding.json;

import std.compat;
export import gopp.errors;
export import gopp.io;
export import gopp.ptr;
export import gopp.slice;
export import gopp.string;
export import gopp.time;

export extern "C++" {
#include <gopp/encoding/json.hpp>
}
