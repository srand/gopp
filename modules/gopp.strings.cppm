// Module gopp.strings: <gopp/strings.hpp>. Written by generate.py.
module;
#define GOPP_MODULE
export module gopp.strings;

import std.compat;
export import gopp.errors;
export import gopp.io;
export import gopp.slice;
export import gopp.string;

export extern "C++" {
#include <gopp/strings.hpp>
}
