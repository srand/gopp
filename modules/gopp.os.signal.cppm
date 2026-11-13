// Module gopp.os.signal: <gopp/os/signal.hpp>. Written by generate.py.
module;
#define GOPP_MODULE
export module gopp.os.signal;

import std.compat;
export import gopp.chan;
export import gopp.context;
export import gopp.os;

export extern "C++" {
#include <gopp/os/signal.hpp>
}
