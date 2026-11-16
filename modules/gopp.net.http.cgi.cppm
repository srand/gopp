// Module gopp.net.http.cgi: <gopp/net/http/cgi.hpp>. Written by generate.py.
module;
#define GOPP_MODULE
export module gopp.net.http.cgi;

import std.compat;
export import gopp.io;
export import gopp.log;
export import gopp.net.http;
export import gopp.ptr;
export import gopp.slice;
export import gopp.string;

export extern "C++" {
#include <gopp/net/http/cgi.hpp>
}
