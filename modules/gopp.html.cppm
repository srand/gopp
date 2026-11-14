// Module gopp.html: <gopp/html.hpp>. Written by generate.py.
module;
#define GOPP_MODULE
export module gopp.html;

import std.compat;
export import gopp.string;

export extern "C++" {
#include <gopp/html.hpp>
}
