// Module gopp.text.tmpl.parse: <gopp/text/template/parse.hpp>. Written by generate.py.
module;
#define GOPP_MODULE
export module gopp.text.tmpl.parse;

import std.compat;
export import gopp.errors;
export import gopp.ptr;
export import gopp.string;

export extern "C++" {
#include <gopp/text/template/parse.hpp>
}
