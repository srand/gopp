// Module gopp.net.netip: <gopp/net/netip.hpp>. Written by generate.py.
module;
#define GOPP_MODULE
export module gopp.net.netip;

import std.compat;
export import gopp.errors;
export import gopp.slice;
export import gopp.string;

export extern "C++" {
#include <gopp/net/netip.hpp>
}
