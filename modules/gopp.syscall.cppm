// Module gopp.syscall: <gopp/syscall.hpp>. Written by generate.py.
module;
#define GOPP_MODULE
export module gopp.syscall;

import std.compat;
export import gopp.errors;
export import gopp.string;

export extern "C++" {
#include <gopp/syscall.hpp>
}
