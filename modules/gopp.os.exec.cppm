// Module gopp.os.exec: <gopp/os/exec.hpp>. Written by generate.py.
module;
#define GOPP_MODULE
export module gopp.os.exec;

import std.compat;
export import gopp.chan;
export import gopp.context;
export import gopp.errors;
export import gopp.io;
export import gopp.os;
export import gopp.ptr;
export import gopp.slice;
export import gopp.string;
export import gopp.syscall;
export import gopp.time;

export extern "C++" {
#include <gopp/os/exec.hpp>
}
