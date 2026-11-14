// Module gopp.container.list: <gopp/container/list.hpp>. Written by generate.py.
module;
#define GOPP_MODULE
export module gopp.container.list;

import std.compat;
export import gopp.base;
export import gopp.ptr;

export extern "C++" {
#include <gopp/container/list.hpp>
}
