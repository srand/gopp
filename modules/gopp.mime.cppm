// Module gopp.mime: <gopp/mime.hpp>. Written by generate.py.
module;
#define GOPP_MODULE
export module gopp.mime;

import std.compat;
export import gopp.errors;
export import gopp.string;

export extern "C++" {
#include <gopp/mime.hpp>
}
