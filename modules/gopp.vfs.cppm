// Module gopp.vfs: <gopp/vfs.hpp>. Written by generate.py.
module;
#define GOPP_MODULE
export module gopp.vfs;

import std.compat;
export import gopp.base;
export import gopp.errors;
export import gopp.io;
export import gopp.io.fs;
export import gopp.os;
export import gopp.path.filepath;
export import gopp.ptr;
export import gopp.regexp;
export import gopp.slice;
export import gopp.string;
export import gopp.time;

export extern "C++" {
#include <gopp/vfs.hpp>
}
