// Module gopp.path: <gopp/path.hpp>. Written by generate.py.
module;
#define GOPP_MODULE
export module gopp.path;

import std.compat;
export import gopp.errors;
export import gopp.string;

export extern "C++" {
#include <gopp/path.hpp>
}
