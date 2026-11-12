// Module gopp.flag: <gopp/flag.hpp>. Written by generate.py.
module;
#define GOPP_MODULE
export module gopp.flag;

import std.compat;
export import gopp.errors;
export import gopp.string;
export import gopp.time;

export extern "C++" {
#include <gopp/flag.hpp>
}
