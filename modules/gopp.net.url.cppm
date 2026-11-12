// Module gopp.net.url: <gopp/net/url.hpp>. Written by generate.py.
module;
#define GOPP_MODULE
export module gopp.net.url;

import std.compat;
export import gopp.errors;
export import gopp.string;

export extern "C++" {
#include <gopp/net/url.hpp>
}
