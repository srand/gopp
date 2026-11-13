// Module gopp.maps: <gopp/maps.hpp>. Written by generate.py.
module;
#define GOPP_MODULE
export module gopp.maps;

import std.compat;
export import gopp.base;

export extern "C++" {
#include <gopp/maps.hpp>
}
