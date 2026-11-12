// Module gopp.bufio: <gopp/bufio.hpp>. Written by generate.py.
module;
#define GOPP_MODULE
export module gopp.bufio;

import std.compat;
export import gopp.io;

export extern "C++" {
#include <gopp/bufio.hpp>
}
