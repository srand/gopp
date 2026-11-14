// The sqlite3 package: a SQLite driver for database/sql, after
// github.com/mattn/go-sqlite3 (MIT; see drivers/sqlite3/LICENSE).
//
//     #include <gopp/database/sql.hpp>
//     #include <gopp/sqlite3.hpp>   // registers the "sqlite3" driver
//
//     auto [db, err] = sql::Open("sqlite3", "app.db?_foreign_keys=1");
//
// Including the header registers the driver, as Go's
// import _ "github.com/mattn/go-sqlite3" does.
//
// The data source name is a file name, ":memory:", or a "file:" URI, with
// mattn's options after a '?':
//
//     _busy_timeout, _timeout     milliseconds to wait on a locked database (5000)
//     _foreign_keys, _fk          PRAGMA foreign_keys (0/1, no/yes, false/true, off/on)
//     _journal_mode, _journal     DELETE, TRUNCATE, PERSIST, MEMORY, WAL, OFF
//     _txlock                     deferred (default), immediate, exclusive
//     _loc                        "auto" (time::Local) or a zone name: times read
//                                 from DATE/DATETIME/TIMESTAMP columns are in it
//     _mutex                      full (default) or no
//     _synchronous, _sync         OFF, NORMAL (default), FULL, EXTRA, or 0 to 3
//     _locking_mode, _locking     NORMAL (default), EXCLUSIVE
//     _auto_vacuum, _vacuum       none, full, incremental, or 0 to 2
//     _case_sensitive_like, _cslike, _defer_foreign_keys, _defer_fk,
//     _ignore_check_constraints, _query_only, _recursive_triggers, _rt,
//     _writable_schema            booleans, as _foreign_keys
//     _secure_delete              a boolean, or fast
//     _cache_size                 PRAGMA cache_size
//     vfs                         the SQLite VFS to use
//
// A "file:" URI keeps its query for SQLite (mode=ro, cache=shared, ...).
//
// Values: INTEGER columns read as int64, REAL as double, TEXT as string,
// BLOB as bytes and NULL as nil; columns declared DATE, DATETIME or
// TIMESTAMP read as time::Time (from text in SQLiteTimestampFormats, or
// integers of Unix seconds or milliseconds), and BOOLEAN ones as bool.
// time::Time arguments are written in SQLiteTimestampFormats[0].
//
// SQLite calls that can wait or do I/O run as blocking calls, handing the
// fiber's processor to others meanwhile, as Go does for cgo calls.
// Canceling a query's context interrupts it (sqlite3_interrupt).
#pragma once

#include <gopp/database/sql.hpp>
#include <gopp/database/sql/driver.hpp>
#include <gopp/errors.hpp>
#include <gopp/string.hpp>

#include <string_view>
#include <tuple>
#include <vector>

namespace gopp::sqlite3 {

struct ErrNoExtended;

// sqlite3.ErrNo: a primary SQLite result code.
struct ErrNo {
    int value = 0;

    constexpr ErrNo() noexcept = default;
    constexpr explicit ErrNo(int v) noexcept : value(v) {}
    // SQLite's description of the code ("constraint failed", ...).
    string Error() const;
    // The extended code (by << 8 | code).
    constexpr ErrNoExtended Extend(int by) const noexcept;
    friend constexpr bool operator==(ErrNo, ErrNo) noexcept = default;
};

// sqlite3.ErrNoExtended: an extended SQLite result code.
struct ErrNoExtended {
    int value = 0;

    constexpr ErrNoExtended() noexcept = default;
    constexpr explicit ErrNoExtended(int v) noexcept : value(v) {}
    string Error() const;
    friend constexpr bool operator==(ErrNoExtended, ErrNoExtended) noexcept = default;
};

constexpr ErrNoExtended ErrNo::Extend(int by) const noexcept { return ErrNoExtended(value | (by << 8)); }

// sqlite3.Error: an error from SQLite, with its codes and message. A class
// can't have a method named like it, so the class is Error_ and Error
// names it: errors::As<sqlite3::Error>(err) works as in Go.
struct Error_ final : errorInterface {
    ErrNo Code;                  // the primary result code
    ErrNoExtended ExtendedCode;  // the extended result code
    int SystemErrno = 0;         // the OS errno behind CANTOPEN and IOERR errors, if any

    Error_(ErrNo code, ErrNoExtended extended, int system_errno, string msg)
        : Code(code), ExtendedCode(extended), SystemErrno(system_errno), msg_(std::move(msg)) {}
    // sqlite3_errmsg's message, or the code's description, and the errno.
    string Error() const override;

private:
    string msg_;
};
using Error = Error_;

// The primary result codes.
inline constexpr ErrNo ErrError{1};        // SQL error or missing database
inline constexpr ErrNo ErrInternal{2};     // Internal logic error in SQLite
inline constexpr ErrNo ErrPerm{3};         // Access permission denied
inline constexpr ErrNo ErrAbort{4};        // Callback routine requested an abort
inline constexpr ErrNo ErrBusy{5};         // The database file is locked
inline constexpr ErrNo ErrLocked{6};       // A table in the database is locked
inline constexpr ErrNo ErrNomem{7};        // A malloc() failed
inline constexpr ErrNo ErrReadonly{8};     // Attempt to write a readonly database
inline constexpr ErrNo ErrInterrupt{9};    // Operation terminated by sqlite3_interrupt()
inline constexpr ErrNo ErrIoErr{10};       // Some kind of disk I/O error occurred
inline constexpr ErrNo ErrCorrupt{11};     // The database disk image is malformed
inline constexpr ErrNo ErrNotFound{12};    // Unknown opcode in sqlite3_file_control()
inline constexpr ErrNo ErrFull{13};        // Insertion failed because database is full
inline constexpr ErrNo ErrCantOpen{14};    // Unable to open the database file
inline constexpr ErrNo ErrProtocol{15};    // Database lock protocol error
inline constexpr ErrNo ErrEmpty{16};       // Database is empty
inline constexpr ErrNo ErrSchema{17};      // The database schema changed
inline constexpr ErrNo ErrTooBig{18};      // String or BLOB exceeds size limit
inline constexpr ErrNo ErrConstraint{19};  // Abort due to constraint violation
inline constexpr ErrNo ErrMismatch{20};    // Data type mismatch
inline constexpr ErrNo ErrMisuse{21};      // Library used incorrectly
inline constexpr ErrNo ErrNoLFS{22};       // Uses OS features not supported on host
inline constexpr ErrNo ErrAuth{23};        // Authorization denied
inline constexpr ErrNo ErrFormat{24};      // Auxiliary database format error
inline constexpr ErrNo ErrRange{25};       // 2nd parameter to sqlite3_bind out of range
inline constexpr ErrNo ErrNotADB{26};      // File opened that is not a database file
inline constexpr ErrNo ErrNotice{27};      // Notifications from sqlite3_log()
inline constexpr ErrNo ErrWarning{28};     // Warnings from sqlite3_log()

// The extended result codes.
inline constexpr ErrNoExtended ErrIoErrRead = ErrIoErr.Extend(1);
inline constexpr ErrNoExtended ErrIoErrShortRead = ErrIoErr.Extend(2);
inline constexpr ErrNoExtended ErrIoErrWrite = ErrIoErr.Extend(3);
inline constexpr ErrNoExtended ErrIoErrFsync = ErrIoErr.Extend(4);
inline constexpr ErrNoExtended ErrIoErrDirFsync = ErrIoErr.Extend(5);
inline constexpr ErrNoExtended ErrIoErrTruncate = ErrIoErr.Extend(6);
inline constexpr ErrNoExtended ErrIoErrFstat = ErrIoErr.Extend(7);
inline constexpr ErrNoExtended ErrIoErrUnlock = ErrIoErr.Extend(8);
inline constexpr ErrNoExtended ErrIoErrRDlock = ErrIoErr.Extend(9);
inline constexpr ErrNoExtended ErrIoErrDelete = ErrIoErr.Extend(10);
inline constexpr ErrNoExtended ErrIoErrBlocked = ErrIoErr.Extend(11);
inline constexpr ErrNoExtended ErrIoErrNoMem = ErrIoErr.Extend(12);
inline constexpr ErrNoExtended ErrIoErrAccess = ErrIoErr.Extend(13);
inline constexpr ErrNoExtended ErrIoErrCheckReservedLock = ErrIoErr.Extend(14);
inline constexpr ErrNoExtended ErrIoErrLock = ErrIoErr.Extend(15);
inline constexpr ErrNoExtended ErrIoErrClose = ErrIoErr.Extend(16);
inline constexpr ErrNoExtended ErrIoErrDirClose = ErrIoErr.Extend(17);
inline constexpr ErrNoExtended ErrIoErrSHMOpen = ErrIoErr.Extend(18);
inline constexpr ErrNoExtended ErrIoErrSHMSize = ErrIoErr.Extend(19);
inline constexpr ErrNoExtended ErrIoErrSHMLock = ErrIoErr.Extend(20);
inline constexpr ErrNoExtended ErrIoErrSHMMap = ErrIoErr.Extend(21);
inline constexpr ErrNoExtended ErrIoErrSeek = ErrIoErr.Extend(22);
inline constexpr ErrNoExtended ErrIoErrDeleteNoent = ErrIoErr.Extend(23);
inline constexpr ErrNoExtended ErrIoErrMMap = ErrIoErr.Extend(24);
inline constexpr ErrNoExtended ErrIoErrGetTempPath = ErrIoErr.Extend(25);
inline constexpr ErrNoExtended ErrIoErrConvPath = ErrIoErr.Extend(26);
inline constexpr ErrNoExtended ErrLockedSharedCache = ErrLocked.Extend(1);
inline constexpr ErrNoExtended ErrBusyRecovery = ErrBusy.Extend(1);
inline constexpr ErrNoExtended ErrBusySnapshot = ErrBusy.Extend(2);
inline constexpr ErrNoExtended ErrCantOpenNoTempDir = ErrCantOpen.Extend(1);
inline constexpr ErrNoExtended ErrCantOpenIsDir = ErrCantOpen.Extend(2);
inline constexpr ErrNoExtended ErrCantOpenFullPath = ErrCantOpen.Extend(3);
inline constexpr ErrNoExtended ErrCantOpenConvPath = ErrCantOpen.Extend(4);
inline constexpr ErrNoExtended ErrCorruptVTab = ErrCorrupt.Extend(1);
inline constexpr ErrNoExtended ErrReadonlyRecovery = ErrReadonly.Extend(1);
inline constexpr ErrNoExtended ErrReadonlyCantLock = ErrReadonly.Extend(2);
inline constexpr ErrNoExtended ErrReadonlyRollback = ErrReadonly.Extend(3);
inline constexpr ErrNoExtended ErrReadonlyDbMoved = ErrReadonly.Extend(4);
inline constexpr ErrNoExtended ErrAbortRollback = ErrAbort.Extend(2);
inline constexpr ErrNoExtended ErrConstraintCheck = ErrConstraint.Extend(1);
inline constexpr ErrNoExtended ErrConstraintCommitHook = ErrConstraint.Extend(2);
inline constexpr ErrNoExtended ErrConstraintForeignKey = ErrConstraint.Extend(3);
inline constexpr ErrNoExtended ErrConstraintFunction = ErrConstraint.Extend(4);
inline constexpr ErrNoExtended ErrConstraintNotNull = ErrConstraint.Extend(5);
inline constexpr ErrNoExtended ErrConstraintPrimaryKey = ErrConstraint.Extend(6);
inline constexpr ErrNoExtended ErrConstraintTrigger = ErrConstraint.Extend(7);
inline constexpr ErrNoExtended ErrConstraintUnique = ErrConstraint.Extend(8);
inline constexpr ErrNoExtended ErrConstraintVTab = ErrConstraint.Extend(9);
inline constexpr ErrNoExtended ErrConstraintRowID = ErrConstraint.Extend(10);
inline constexpr ErrNoExtended ErrNoticeRecoverWAL = ErrNotice.Extend(1);
inline constexpr ErrNoExtended ErrNoticeRecoverRollback = ErrNotice.Extend(2);
inline constexpr ErrNoExtended ErrWarningAutoIndex = ErrWarning.Extend(1);

// sqlite3.SQLiteTimestampFormats: the layouts DATE, DATETIME and TIMESTAMP
// columns are read with, in order; the first is how times are written.
extern const std::vector<std::string_view> SQLiteTimestampFormats;

// sqlite3.Version: the library's version string ("3.54.1"), number
// (3054001) and source ID.
std::tuple<string, int, string> Version();

// sqlite3.SQLiteDriver: the driver registered as "sqlite3".
struct SQLiteDriver final : sql::driver::Driver {
    std::pair<ptr<sql::driver::Conn>, error> Open(std::string_view dsn) override;
};

namespace detail {
// Registers the driver once; true.
bool registerDriver() noexcept;
// Initialized in every program that includes this header, so that a
// static library can't leave the driver out.
inline const bool registered = registerDriver();
}  // namespace detail

}  // namespace gopp::sqlite3
