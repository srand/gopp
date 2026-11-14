// Module gopp.mime.multipart: <gopp/mime/multipart.hpp>. Written by generate.py.
module;
#define GOPP_MODULE
export module gopp.mime.multipart;

import std.compat;
export import gopp.bufio;
export import gopp.errors;
export import gopp.io;
export import gopp.net.textproto;
export import gopp.ptr;
export import gopp.slice;
export import gopp.string;

export extern "C++" {
#include <gopp/mime/multipart.hpp>
}
