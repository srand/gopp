// Module gopp.chan: <gopp/chan.hpp>. Written by generate.py.
module;
#define GOPP_MODULE
export module gopp.chan;

import std.compat;
export import gopp.base;

export extern "C++" {
#include <gopp/chan.hpp>
}
