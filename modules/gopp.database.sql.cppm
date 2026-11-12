// Module gopp.database.sql: <gopp/database/sql.hpp>. Written by generate.py.
module;
#define GOPP_MODULE
export module gopp.database.sql;

import std.compat;
export import gopp.chan;
export import gopp.context;
export import gopp.database.sql.driver;
export import gopp.errors;
export import gopp.sync;
export import gopp.time;

export extern "C++" {
#include <gopp/database/sql.hpp>
}
