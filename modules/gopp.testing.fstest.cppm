// Module gopp.testing.fstest: <gopp/testing/fstest.hpp>. Written by generate.py.
module;
#define GOPP_MODULE
export module gopp.testing.fstest;

import std.compat;
export import gopp.errors;
export import gopp.io.fs;
export import gopp.ptr;
export import gopp.slice;
export import gopp.string;
export import gopp.time;

export extern "C++" {
#include <gopp/testing/fstest.hpp>
}
