// Module gopp.os: <gopp/os.hpp>. Written by generate.py.
module;
#define GOPP_MODULE
export module gopp.os;

import std.compat;
export import gopp.base;
export import gopp.errors;
export import gopp.io;
export import gopp.io.fs;
export import gopp.ptr;
export import gopp.slice;
export import gopp.string;
export import gopp.time;

export extern "C++" {
#include <gopp/os.hpp>
}
