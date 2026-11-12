// Module gopp.errors: <gopp/errors.hpp>. Written by generate.py.
module;
#define GOPP_MODULE
export module gopp.errors;

import std.compat;
export import gopp.base;
export import gopp.ptr;
export import gopp.string;

export extern "C++" {
#include <gopp/errors.hpp>
}
