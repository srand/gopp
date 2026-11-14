// Module gopp.regexp.syntax: <gopp/regexp/syntax.hpp>. Written by generate.py.
module;
#define GOPP_MODULE
export module gopp.regexp.syntax;

import std.compat;
export import gopp.errors;
export import gopp.ptr;
export import gopp.string;

export extern "C++" {
#include <gopp/regexp/syntax.hpp>
}
