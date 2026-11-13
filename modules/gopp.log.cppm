// Module gopp.log: <gopp/log.hpp>. Written by generate.py.
module;
#define GOPP_MODULE
export module gopp.log;

import std.compat;
export import gopp.errors;
export import gopp.io;
export import gopp.ptr;
export import gopp.string;
export import gopp.sync;

export extern "C++" {
#include <gopp/log.hpp>
}
