// Module gopp.compress.gzip: <gopp/compress/gzip.hpp>. Written by generate.py.
module;
#define GOPP_MODULE
export module gopp.compress.gzip;

import std.compat;
export import gopp.bufio;
export import gopp.compress.flate;
export import gopp.errors;
export import gopp.io;
export import gopp.ptr;
export import gopp.slice;
export import gopp.string;
export import gopp.time;

export extern "C++" {
#include <gopp/compress/gzip.hpp>
}
