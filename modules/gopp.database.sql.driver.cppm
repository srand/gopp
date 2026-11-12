// Module gopp.database.sql.driver: <gopp/database/sql/driver.hpp>. Written by generate.py.
module;
#define GOPP_MODULE
export module gopp.database.sql.driver;

import std.compat;
export import gopp.context;
export import gopp.errors;
export import gopp.ptr;
export import gopp.slice;
export import gopp.string;
export import gopp.time;

export extern "C++" {
#include <gopp/database/sql/driver.hpp>
}
