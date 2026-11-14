// Module gopp.unicode.utf8: <gopp/unicode/utf8.hpp>. Written by generate.py.
module;
#define GOPP_MODULE
export module gopp.unicode.utf8;

import std.compat;
export import gopp.base;
export import gopp.slice;

export extern "C++" {
#include <gopp/unicode/utf8.hpp>
}
