// Module gopp.os: <gopp/os.hpp>. Written by generate.py.
module;
#define GOPP_MODULE
export module gopp.os;

import std.compat;
export import gopp.io;
export import gopp.string;

export extern "C++" {
#include <gopp/os.hpp>
}
