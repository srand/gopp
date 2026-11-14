// Module gopp.html.tmpl: <gopp/html/template.hpp>. Written by generate.py.
module;
#define GOPP_MODULE
export module gopp.html.tmpl;

import std.compat;
export import gopp.errors;
export import gopp.io;
export import gopp.ptr;
export import gopp.string;
export import gopp.sync;
export import gopp.text.tmpl;
export import gopp.text.tmpl.parse;

export extern "C++" {
#include <gopp/html/template.hpp>
}
