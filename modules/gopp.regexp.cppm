// Module gopp.regexp: <gopp/regexp.hpp>. Written by generate.py.
module;
#define GOPP_MODULE
export module gopp.regexp;

import std.compat;
export import gopp.errors;
export import gopp.io;
export import gopp.ptr;
export import gopp.regexp.syntax;
export import gopp.slice;
export import gopp.string;

export extern "C++" {
#include <gopp/regexp.hpp>
}
