# SQLite driver for database/sql

## Intent

go++'s `database/sql` is a port of Go's pool, statements and transactions,
but so far it has only run against Go's fake test driver. A real driver
lets people build a service on go++ end to end, and it backs up the claim
that C++ applications can be easy to write. SQLite needs no server, so it
is the first one.

## Goals

- `sql::Open("sqlite3", "app.db?_foreign_keys=1")` works the way it does with
  mattn/go-sqlite3: same driver name, same DSN options, same type
  conversions, same error values. Go code that uses that driver ports line
  by line.
- Long SQLite calls don't stall other fibers: the P is handed on while
  SQLite runs, as Go does for a cgo call.
- Canceling a query's context interrupts it (`sqlite3_interrupt`), as in
  mattn.
- A small example: an HTTP + JSON CRUD service over SQLite.
- Tests ported from mattn/go-sqlite3's own tests (MIT), keeping their names.

## Non-goals (could come later)

- mattn's extras: SQL functions and aggregators in C++, the backup API,
  update/commit/rollback hooks, authorizers, loadable extensions, the
  `sqlite_userauth` build, virtual tables.
- A C++ module (`modules/*.cppm`) for the driver.
- An io_uring VFS. SQLite does its own file I/O on the calling thread.

## Anti-goals

- Programs that don't use SQLite must not link it or get bigger.
- A fiber must never park inside a C++ static initializer (see commit
  6a13def): any lazy setup goes under `sync::Once`.

## Approaches

- **Wrap the C library, after mattn/go-sqlite3 (chosen).** Link jolt's
  `sqlite` package (3.54.1, static) and implement the `driver` interfaces
  over `sqlite3*` and `sqlite3_stmt*`. mattn wraps the same C API, so its
  semantics and tests carry over directly.
- *After modernc.org/sqlite.* That's a C-to-Go translation of SQLite, with
  the driver name `sqlite` and `_pragma=` DSN options. Wrapping C ourselves
  makes mattn's API and tests the natural fit. Rejected.
- *A driver with the extras (functions, backup, hooks).* About twice the
  work. The user chose the plain driver first.

## Decided

- Its own jolt library task, `gopp/sqlite3`, requiring `gopp` and `sqlite`.
  Header `<gopp/sqlite3.hpp>`, namespace `gopp::sqlite3`, after Go's
  package name.
- Registration under "sqlite3" happens from the header (an inline variable
  initialized in each program that includes it), so a static library can't
  drop it. This is what Go's `import _ "github.com/mattn/go-sqlite3"` does.
- Each SQLite call that can do I/O or wait (`sqlite3_open_v2`, `prepare`,
  `step`, `exec`, `close`, `reset` and the busy wait inside them) runs under
  `runtime::blocking`. Column reads and binds don't.
- Connections open with `SQLITE_OPEN_FULLMUTEX` unless `_mutex=no`, since a
  `driver::Conn` can move between threads between calls.
- The DSN keeps mattn's syntax: a file name or `file:` URI, with mattn's
  `_` options (`_busy_timeout`/`_timeout` defaulting to 5000 ms,
  `_foreign_keys`, `_journal_mode`, `_txlock`, `_loc`, `_synchronous`,
  `_locking_mode`, `_mutex`, `_cache_size`, `_recursive_triggers`,
  `_case_sensitive_like`, `_defer_foreign_keys`, `_ignore_check_constraints`,
  `_query_only`, `_secure_delete`, `cache`, `mode`, ...). Anything else is
  passed to SQLite as part of the URI.
- Types follow mattn: `INTEGER` → int64, `REAL` → double, `TEXT` → string,
  `BLOB` → bytes, `NULL` → nil. Columns declared `DATE`, `DATETIME` or
  `TIMESTAMP` parse to `time::Time` using mattn's `SQLiteTimestampFormats`.
  `BOOLEAN` columns read as bool. Times are written in mattn's format.
- Errors are `sqlite3::Error` with `Code`, `ExtendedCode` and the message,
  plus mattn's `ErrNo`/`ErrNoExtended` constants (`ErrConstraint`,
  `ErrConstraintUnique`, ...), so `errors::As` and `==` comparisons port.
- Interfaces implemented: `Driver`, `DriverContext`/`Connector`, `Conn`
  with `ConnBeginTx`, `ConnPrepareContext`, `ExecerContext`,
  `QueryerContext`, `Pinger` and `SessionResetter`; `Stmt` with the context
  variants; `Rows` with `ColumnTypeDatabaseTypeName`, `ColumnTypeNullable`
  and `ColumnTypeScanType`; `Result`; `Tx`. `Exec` runs a script of several
  statements, as mattn's does.
- Example: `examples/todo`, a JSON CRUD API over SQLite, with structs
  mapped by reflection.

## Left to the builder

- How context cancellation is watched: a fiber per cancellable query, as
  mattn uses a goroutine, or a `context::AfterFunc` added to the context
  package (Go 1.21's API, cheaper).
- Which of mattn's tests to port, as long as open/close, CRUD, types (bool,
  float, null, blobs, strings with NULs, times and time zones),
  transactions, named and numbered parameters, multi-statement Exec,
  cancellation, busy/locking with WAL, decltypes and errors are covered.
- Whether `runtime::blocking` around each `step` costs enough to matter.
  Measure a large scan, and fall back to wrapping only calls that can block
  if it does.
- README updates: the package table, the Status section and the test count.
