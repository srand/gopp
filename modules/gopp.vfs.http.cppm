// Module gopp.vfs.http: <gopp/vfs/http.hpp>. Written by generate.py.
module;
#define GOPP_MODULE
export module gopp.vfs.http;

import std.compat;
export import gopp.errors;
export import gopp.io.fs;
export import gopp.net.http;
export import gopp.ptr;
export import gopp.string;
export import gopp.time;
export import gopp.vfs;

export extern "C++" {
#include <gopp/vfs/http.hpp>
}
