// Module gopp.cmd: <gopp/cmd.hpp>. Written by generate.py.
module;
#define GOPP_MODULE
export module gopp.cmd;

import std.compat;
export import gopp.context;
export import gopp.errors;
export import gopp.io;
export import gopp.pflag;
export import gopp.ptr;
export import gopp.string;
export import gopp.text.tmpl;

export extern "C++" {
#include <gopp/cmd.hpp>
}
