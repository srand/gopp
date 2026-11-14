// Module gopp.net.http.httputil: <gopp/net/http/httputil.hpp>. Written by generate.py.
module;
#define GOPP_MODULE
export module gopp.net.http.httputil;

import std.compat;
export import gopp.errors;
export import gopp.log;
export import gopp.net.http;
export import gopp.net.url;
export import gopp.ptr;
export import gopp.slice;
export import gopp.time;

export extern "C++" {
#include <gopp/net/http/httputil.hpp>
}
