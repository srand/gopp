// Module gopp.sort: <gopp/sort.hpp>. Written by generate.py.
module;
#define GOPP_MODULE
export module gopp.sort;

import std.compat;
export import gopp.base;
export import gopp.slices;
export import gopp.string;

export extern "C++" {
#include <gopp/sort.hpp>
}
