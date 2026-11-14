// Module gopp.net.textproto: <gopp/net/textproto.hpp>. Written by generate.py.
module;
#define GOPP_MODULE
export module gopp.net.textproto;

import std.compat;
export import gopp.bufio;
export import gopp.errors;
export import gopp.io;
export import gopp.ptr;
export import gopp.slice;
export import gopp.string;

export extern "C++" {
#include <gopp/net/textproto.hpp>
}
