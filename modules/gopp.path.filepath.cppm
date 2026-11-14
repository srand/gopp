// Module gopp.path.filepath: <gopp/path/filepath.hpp>. Written by generate.py.
module;
#define GOPP_MODULE
export module gopp.path.filepath;

import std.compat;
export import gopp.errors;
export import gopp.io.fs;
export import gopp.path;
export import gopp.slice;
export import gopp.string;

export extern "C++" {
#include <gopp/path/filepath.hpp>
}
