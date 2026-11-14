// Module gopp.net.http: <gopp/net/http.hpp>. Written by generate.py.
module;
#define GOPP_MODULE
export module gopp.net.http;

import std.compat;
export import gopp.bufio;
export import gopp.context;
export import gopp.crypto.tls;
export import gopp.errors;
export import gopp.io;
export import gopp.io.fs;
export import gopp.mime.multipart;
export import gopp.net;
export import gopp.net.url;
export import gopp.ptr;
export import gopp.string;
export import gopp.time;

export extern "C++" {
#include <gopp/net/http.hpp>
}
