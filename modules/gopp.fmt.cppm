// Module gopp.fmt: <gopp/fmt.hpp>. Written by generate.py.
module;
#define GOPP_MODULE
export module gopp.fmt;

import std.compat;
export import gopp.errors;
export import gopp.io;
export import gopp.string;

export extern "C++" {
#include <gopp/fmt.hpp>
}
