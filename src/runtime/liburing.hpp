// <liburing.h>, which go++'s sources include through here: its headers use
// GNU extensions (anonymous structs, zero-length arrays, compound literals)
// that -pedantic rejects.
#pragma once

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#include <liburing.h>
#pragma GCC diagnostic pop
