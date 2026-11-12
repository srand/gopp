// Module gopp.text.template_: <gopp/text/template.hpp>. Written by generate.py.
module;
#define GOPP_MODULE
export module gopp.text.template_;

import std.compat;
export import gopp.encoding.json;
export import gopp.errors;
export import gopp.io;
export import gopp.ptr;
export import gopp.slice;
export import gopp.string;
export import gopp.sync;
export import gopp.text.template_.parse;

export extern "C++" {
#include <gopp/text/template.hpp>
}
