// Module gopp.sync: <gopp/sync.hpp>. Written by generate.py.
module;
#define GOPP_MODULE
export module gopp.sync;

import std.compat;
export import gopp.base;
export import gopp.runtime;

export extern "C++" {
#include <gopp/sync.hpp>
}
