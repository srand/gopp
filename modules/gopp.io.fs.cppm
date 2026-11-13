// Module gopp.io.fs: <gopp/io/fs.hpp>. Written by generate.py.
module;
#define GOPP_MODULE
export module gopp.io.fs;

import std.compat;
export import gopp.base;
export import gopp.errors;
export import gopp.io;
export import gopp.ptr;
export import gopp.slice;
export import gopp.string;
export import gopp.time;

export extern "C++" {
#include <gopp/io/fs.hpp>
}
