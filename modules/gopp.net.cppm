// Module gopp.net: <gopp/net.hpp>. Written by generate.py.
module;
#define GOPP_MODULE
export module gopp.net;

import std.compat;
export import gopp.errors;
export import gopp.io;
export import gopp.ptr;
export import gopp.string;
export import gopp.time;

export extern "C++" {
#include <gopp/net.hpp>
}
