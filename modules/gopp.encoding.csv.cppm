// Module gopp.encoding.csv: <gopp/encoding/csv.hpp>. Written by generate.py.
module;
#define GOPP_MODULE
export module gopp.encoding.csv;

import std.compat;
export import gopp.bufio;
export import gopp.errors;
export import gopp.io;
export import gopp.ptr;
export import gopp.string;

export extern "C++" {
#include <gopp/encoding/csv.hpp>
}
