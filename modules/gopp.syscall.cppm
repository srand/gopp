// Module gopp.syscall: <gopp/syscall.hpp>. Written by generate.py.
module;
#define GOPP_MODULE
export module gopp.syscall;

import std.compat;
export import gopp.base;
export import gopp.errors;
export import gopp.slice;
export import gopp.string;
export import gopp.sync;

export extern "C++" {
#include <gopp/syscall.hpp>
}
