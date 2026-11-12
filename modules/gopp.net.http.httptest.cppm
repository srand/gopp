// Module gopp.net.http.httptest: <gopp/net/http/httptest.hpp>. Written by generate.py.
module;
#define GOPP_MODULE
export module gopp.net.http.httptest;

import std.compat;
export import gopp.chan;
export import gopp.net.http;

export extern "C++" {
#include <gopp/net/http/httptest.hpp>
}
