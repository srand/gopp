// Module gopp.log.slog: <gopp/log/slog.hpp>. Written by generate.py.
module;
#define GOPP_MODULE
export module gopp.log.slog;

import std.compat;
export import gopp.context;
export import gopp.encoding.json;
export import gopp.errors;
export import gopp.io;
export import gopp.log;
export import gopp.ptr;
export import gopp.slice;
export import gopp.string;
export import gopp.text.tmpl;
export import gopp.time;

export extern "C++" {
#include <gopp/log/slog.hpp>
}
