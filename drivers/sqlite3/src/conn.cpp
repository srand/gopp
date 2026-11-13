// The driver and its connections, after mattn/go-sqlite3's sqlite3.go,
// sqlite3_sql.go and error.go.
#include "internal.hpp"

#include <gopp/chan.hpp>
#include <gopp/fmt.hpp>
#include <gopp/net/url.hpp>
#include <gopp/runtime.hpp>
#include <gopp/strings.hpp>
#include <gopp/syscall.hpp>

#include <atomic>
#include <cctype>

namespace gopp::sqlite3 {

using namespace internal;

// ---------------------------------------------------------------------------
// Errors

string ErrNo::Error() const { return Error_(*this, ErrNoExtended(), 0, string()).Error(); }

string ErrNoExtended::Error() const { return Error_(ErrNo(value & 0xff), *this, 0, string()).Error(); }

string Error_::Error() const {
    string str = !msg_.empty() ? msg_ : string(sqlite3_errstr(Code.value));
    if (SystemErrno != 0) str = str + ": " + syscall::Errno(SystemErrno).Error();
    return str;
}

namespace internal {

error lastError(::sqlite3* db) {
    int rv = sqlite3_errcode(db);  // returns SQLITE_NOMEM if db == nullptr
    if (rv == SQLITE_OK) return nullptr;
    int extrv = sqlite3_extended_errcode(db);  // returns SQLITE_NOMEM if db == nullptr
    string errStr(sqlite3_errmsg(db));         // returns "out of memory" if db == nullptr
    int systemErrno = 0;
    if (rv == SQLITE_CANTOPEN || (rv == SQLITE_IOERR && extrv != SQLITE_IOERR_NOMEM)) {
        systemErrno = sqlite3_system_errno(db);
    }
    return make_ptr<Error_>(ErrNo(rv), ErrNoExtended(extrv), systemErrno, errStr);
}

}  // namespace internal

// ---------------------------------------------------------------------------
// Package-level

const std::vector<std::string_view> SQLiteTimestampFormats = {
    // By default, store timestamps with whatever timezone they come with.
    // When parsed, they will be returned with the same timezone.
    "2006-01-02 15:04:05.999999999-07:00",
    "2006-01-02T15:04:05.999999999-07:00",
    "2006-01-02 15:04:05.999999999",
    "2006-01-02T15:04:05.999999999",
    "2006-01-02 15:04:05",
    "2006-01-02T15:04:05",
    "2006-01-02 15:04",
    "2006-01-02T15:04",
    "2006-01-02",
};

std::tuple<string, int, string> Version() {
    return {string(sqlite3_libversion()), sqlite3_libversion_number(), string(sqlite3_sourceid())};
}

bool detail::register_driver() noexcept {
    // Every program that includes the header runs this; register once.
    static std::atomic<bool> done{false};
    if (!done.exchange(true)) sql::Register("sqlite3", make_ptr<SQLiteDriver>());
    return true;
}

// ---------------------------------------------------------------------------
// Open

namespace {

std::string lower(std::string_view s) {
    std::string r(s);
    for (char& c : r) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return r;
}

std::string upper(std::string_view s) {
    std::string r(s);
    for (char& c : r) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return r;
}

// A boolean option: 0 or 1, or -1 when the value isn't one.
int parse_bool(const std::string& v) {
    std::string l = lower(v);
    if (l == "0" || l == "no" || l == "false" || l == "off") return 0;
    if (l == "1" || l == "yes" || l == "true" || l == "on") return 1;
    return -1;
}

error errorf(std::string msg) { return errors::New(string(msg)); }

// sqlite3_exec as a blocking call.
error exec_pragma(::sqlite3* db, const std::string& s) {
    int rv = runtime::blocking([&] { return sqlite3_exec(db, s.c_str(), nullptr, nullptr, nullptr); });
    if (rv != SQLITE_OK) return lastError(db);
    return nullptr;
}

}  // namespace

std::pair<ptr<sql::driver::Conn>, error> SQLiteDriver::Open(std::string_view dsn_view) {
    if (sqlite3_threadsafe() == 0) return {nullptr, errorf("sqlite library was not compiled for thread-safe operation")};

    std::string dsn(dsn_view);
    const time::Location* loc = nullptr;
    int mutex = SQLITE_OPEN_FULLMUTEX;
    std::string txlock = "BEGIN";

    int autoVacuum = -1;
    int64 busyTimeout = 5000;
    int caseSensitiveLike = -1;
    int deferForeignKeys = -1;
    int foreignKeys = -1;
    int ignoreCheckConstraints = -1;
    std::string journalMode;
    std::string lockingMode = "NORMAL";
    int queryOnly = -1;
    int recursiveTriggers = -1;
    std::string secureDelete = "DEFAULT";
    std::string synchronousMode = "NORMAL";
    int writableSchema = -1;
    std::string vfsName;
    bool haveCacheSize = false;
    int64 cacheSize = 0;

    auto pos = dsn.find('?');
    if (pos != std::string::npos && pos >= 1) {
        auto [params, err] = url::ParseQuery(std::string_view(dsn).substr(pos + 1));
        if (err) return {nullptr, err};
        // The value of the first key of keys that params has ("" if none),
        // and whether it had one: the later aliases win, as in mattn's.
        auto get = [&](std::initializer_list<const char*> keys) {
            std::string pkey;
            for (const char* k : keys) {
                if (params.Has(k)) pkey = k;
            }
            return pkey.empty() ? std::string() : std::string(params.Get(pkey).view());
        };

        // _loc
        if (std::string val = get({"_loc"}); !val.empty()) {
            if (lower(val) == "auto") {
                loc = time::Local;
            } else {
                auto [l, lerr] = time::LoadLocation(val);
                if (lerr) return {nullptr, errorf("Invalid _loc: " + val + ": " + std::string(lerr.Error().view()))};
                loc = l;
            }
        }

        // _mutex
        if (std::string val = get({"_mutex"}); !val.empty()) {
            std::string l = lower(val);
            if (l == "no") mutex = SQLITE_OPEN_NOMUTEX;
            else if (l == "full") mutex = SQLITE_OPEN_FULLMUTEX;
            else return {nullptr, errorf("Invalid _mutex: " + val)};
        }

        // _txlock
        if (std::string val = get({"_txlock"}); !val.empty()) {
            std::string l = lower(val);
            if (l == "immediate") txlock = "BEGIN IMMEDIATE";
            else if (l == "exclusive") txlock = "BEGIN EXCLUSIVE";
            else if (l == "deferred") txlock = "BEGIN";
            else return {nullptr, errorf("Invalid _txlock: " + val)};
        }

        // Auto Vacuum (_vacuum)
        if (std::string val = get({"_auto_vacuum", "_vacuum"}); !val.empty()) {
            std::string l = lower(val);
            if (l == "0" || l == "none") autoVacuum = 0;
            else if (l == "1" || l == "full") autoVacuum = 1;
            else if (l == "2" || l == "incremental") autoVacuum = 2;
            else {
                return {nullptr,
                        errorf("Invalid _auto_vacuum: " + val + ", expecting value of '0 NONE 1 FULL 2 INCREMENTAL'")};
            }
        }

        // Busy Timeout (_busy_timeout)
        if (std::string val = get({"_busy_timeout", "_timeout"}); !val.empty()) {
            auto [iv, perr] = strconv::ParseInt(val, 10, 64);
            if (perr) return {nullptr, errorf("Invalid _busy_timeout: " + val + ": " + std::string(perr.Error().view()))};
            busyTimeout = iv;
        }

        auto boolean = [&](std::initializer_list<const char*> keys, const char* name, int& out) -> error {
            std::string val = get(keys);
            if (val.empty()) return nullptr;
            out = parse_bool(val);
            if (out < 0) {
                return errorf(std::string("Invalid ") + name + ": " + val +
                              ", expecting boolean value of '0 1 false true no yes off on'");
            }
            return nullptr;
        };
        if (auto e = boolean({"_case_sensitive_like", "_cslike"}, "_case_sensitive_like", caseSensitiveLike)) return {nullptr, e};
        if (auto e = boolean({"_defer_foreign_keys", "_defer_fk"}, "_defer_foreign_keys", deferForeignKeys)) return {nullptr, e};
        if (auto e = boolean({"_foreign_keys", "_fk"}, "_foreign_keys", foreignKeys)) return {nullptr, e};
        if (auto e = boolean({"_ignore_check_constraints"}, "_ignore_check_constraints", ignoreCheckConstraints)) {
            return {nullptr, e};
        }

        // Journal Mode (_journal)
        if (std::string val = get({"_journal_mode", "_journal"}); !val.empty()) {
            std::string u = upper(val);
            if (u == "DELETE" || u == "TRUNCATE" || u == "PERSIST" || u == "MEMORY" || u == "OFF") {
                journalMode = u;
            } else if (u == "WAL") {
                journalMode = u;
                // For WAL Mode set Synchronous Mode to 'NORMAL'.
                // See https://www.sqlite.org/pragma.html#pragma_synchronous
                synchronousMode = "NORMAL";
            } else {
                return {nullptr,
                        errorf("Invalid _journal: " + val + ", expecting value of 'DELETE TRUNCATE PERSIST MEMORY WAL OFF'")};
            }
        }

        // Locking Mode (_locking)
        if (std::string val = get({"_locking_mode", "_locking"}); !val.empty()) {
            std::string u = upper(val);
            if (u == "NORMAL" || u == "EXCLUSIVE") lockingMode = u;
            else return {nullptr, errorf("Invalid _locking_mode: " + val + ", expecting value of 'NORMAL EXCLUSIVE")};
        }

        if (auto e = boolean({"_query_only"}, "_query_only", queryOnly)) return {nullptr, e};
        if (auto e = boolean({"_recursive_triggers", "_rt"}, "_recursive_triggers", recursiveTriggers)) return {nullptr, e};

        // Secure Delete (_secure_delete)
        if (std::string val = get({"_secure_delete"}); !val.empty()) {
            std::string l = lower(val);
            if (int b = parse_bool(val); b >= 0) secureDelete = b ? "ON" : "OFF";
            else if (l == "fast") secureDelete = "FAST";
            else {
                return {nullptr, errorf("Invalid _secure_delete: " + val +
                                        ", expecting boolean value of '0 1 false true no yes off on fast'")};
            }
        }

        // Synchronous Mode (_synchronous)
        if (std::string val = get({"_synchronous", "_sync"}); !val.empty()) {
            std::string u = upper(val);
            if (u == "0" || u == "OFF" || u == "1" || u == "NORMAL" || u == "2" || u == "FULL" || u == "3" || u == "EXTRA") {
                synchronousMode = u;
            } else {
                return {nullptr,
                        errorf("Invalid _synchronous: " + val + ", expecting value of '0 OFF 1 NORMAL 2 FULL 3 EXTRA'")};
            }
        }

        if (auto e = boolean({"_writable_schema"}, "_writable_schema", writableSchema)) return {nullptr, e};

        // Cache size (_cache_size)
        if (std::string val = get({"_cache_size"}); !val.empty()) {
            auto [iv, perr] = strconv::ParseInt(val, 10, 64);
            if (perr) return {nullptr, errorf("Invalid _cache_size: " + val + ": " + std::string(perr.Error().view()))};
            haveCacheSize = true;
            cacheSize = iv;
        }

        if (std::string val = get({"vfs"}); !val.empty()) vfsName = val;

        if (!dsn.starts_with("file:")) dsn = dsn.substr(0, pos);
    }

    ::sqlite3* db = nullptr;
    int rv = runtime::blocking([&] {
        return sqlite3_open_v2(dsn.c_str(), &db, mutex | SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI,
                               vfsName.empty() ? nullptr : vfsName.c_str());
    });
    if (rv != 0) {
        // Save off the error _before_ closing the database. This is
        // important in the case that the database file is not found.
        error err = lastError(db);
        if (db) runtime::blocking([&] { return sqlite3_close_v2(db); });
        return {nullptr, err};
    }
    if (!db) return {nullptr, errorf("sqlite succeeded without returning a database")};

    auto c = make_ptr<conn>(db, loc, txlock);
    auto fail = [&](error err) -> std::pair<ptr<sql::driver::Conn>, error> {
        c->Close();
        return {nullptr, err};
    };

    // Busy timeout
    if (auto err = exec_pragma(db, fmt::Sprint("PRAGMA busy_timeout = {};", busyTimeout).str())) return fail(err);
    // Auto Vacuum: must be set before any tables are created.
    if (autoVacuum > -1) {
        if (auto err = exec_pragma(db, fmt::Sprint("PRAGMA auto_vacuum = {};", autoVacuum).str())) return fail(err);
    }
    if (caseSensitiveLike > -1) {
        if (auto err = exec_pragma(db, fmt::Sprint("PRAGMA case_sensitive_like = {};", caseSensitiveLike).str())) {
            return fail(err);
        }
    }
    if (deferForeignKeys > -1) {
        if (auto err = exec_pragma(db, fmt::Sprint("PRAGMA defer_foreign_keys = {};", deferForeignKeys).str())) {
            return fail(err);
        }
    }
    if (foreignKeys > -1) {
        if (auto err = exec_pragma(db, fmt::Sprint("PRAGMA foreign_keys = {};", foreignKeys).str())) return fail(err);
    }
    if (ignoreCheckConstraints > -1) {
        if (auto err =
                exec_pragma(db, fmt::Sprint("PRAGMA ignore_check_constraints = {};", ignoreCheckConstraints).str())) {
            return fail(err);
        }
    }
    if (!journalMode.empty()) {
        if (auto err = exec_pragma(db, "PRAGMA journal_mode = " + journalMode + ";")) return fail(err);
    }
    if (auto err = exec_pragma(db, "PRAGMA locking_mode = " + lockingMode + ";")) return fail(err);
    if (queryOnly > -1) {
        if (auto err = exec_pragma(db, fmt::Sprint("PRAGMA query_only = {};", queryOnly).str())) return fail(err);
    }
    if (recursiveTriggers > -1) {
        if (auto err = exec_pragma(db, fmt::Sprint("PRAGMA recursive_triggers = {};", recursiveTriggers).str())) {
            return fail(err);
        }
    }
    if (secureDelete != "DEFAULT") {
        if (auto err = exec_pragma(db, "PRAGMA secure_delete = " + secureDelete + ";")) return fail(err);
    }
    if (auto err = exec_pragma(db, "PRAGMA synchronous = " + synchronousMode + ";")) return fail(err);
    if (writableSchema > -1) {
        if (auto err = exec_pragma(db, fmt::Sprint("PRAGMA writable_schema = {};", writableSchema).str())) return fail(err);
    }
    if (haveCacheSize) {
        if (auto err = exec_pragma(db, fmt::Sprint("PRAGMA cache_size = {};", cacheSize).str())) return fail(err);
    }
    return {c, nullptr};
}

// ---------------------------------------------------------------------------
// Connections

namespace internal {

conn::~conn() { Close(); }

error conn::Close() {
    ::sqlite3* db;
    {
        std::lock_guard l(mu_);
        db = db_;
        if (!db) return nullptr;
    }
    // sqlite3_close_v2 leaves statements not yet finalized valid; the
    // database closes when the last one is.
    int rv = runtime::blocking([&] { return sqlite3_close_v2(db); });
    if (rv != SQLITE_OK) return lastError(db);
    std::lock_guard l(mu_);
    db_ = nullptr;
    return nullptr;
}

error conn::Ping(ptr<context::Context>) {
    if (!db_) return driver::ErrBadConn;
    return nullptr;
}

std::pair<ptr<driver::Tx>, error> conn::Begin() { return BeginTx(context::Background(), {}); }

std::pair<ptr<driver::Tx>, error> conn::BeginTx(ptr<context::Context> ctx, driver::TxOptions) {
    if (auto [_, err] = exec(ctx, txlock_, {}); err) return {nullptr, err};
    return {make_ptr<tx>(ptr<conn>(this)), nullptr};
}

std::pair<ptr<driver::Stmt>, error> conn::Prepare(std::string_view q) {
    auto [s, err] = prepare(q);
    if (err) return {nullptr, err};
    return {s, nullptr};
}

std::pair<ptr<driver::Stmt>, error> conn::PrepareContext(ptr<context::Context>, std::string_view q) { return Prepare(q); }

std::pair<ptr<driver::Result>, error> conn::ExecContext(ptr<context::Context> ctx, std::string_view q,
                                                        std::span<const driver::NamedValue> args) {
    return exec(ctx, q, args);
}

std::pair<ptr<driver::Rows>, error> conn::QueryContext(ptr<context::Context> ctx, std::string_view q,
                                                       std::span<const driver::NamedValue> args) {
    return query(ctx, q, args);
}

namespace {

const error errConnClosed = errors::New("sqlite connection is already closed");

std::string trim_space(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return std::string(s);
}

error not_enough_args(std::size_t want, std::size_t got) {
    return errors::New(fmt::Sprint("not enough args to execute query: want {} got {}", want, got));
}

}  // namespace

std::pair<ptr<stmt>, error> conn::prepare(std::string_view q) {
    if (!db_) return {nullptr, errConnClosed};
    sqlite3_stmt* s = nullptr;
    const char* tail = nullptr;
    std::string query(q);
    int rv = runtime::blocking(
        [&] { return sqlite3_prepare_v2(db_, query.c_str(), static_cast<int>(query.size()), &s, &tail); });
    if (rv != SQLITE_OK) return {nullptr, lastError(db_)};
    std::string t;
    if (tail && *tail != '\0') t = trim_space(std::string_view(tail, query.c_str() + query.size() - tail));
    return {make_ptr<stmt>(ptr<conn>(this), s, std::move(t)), nullptr};
}

std::pair<ptr<driver::Result>, error> conn::exec(const ptr<context::Context>& ctx, std::string_view q,
                                                 std::span<const driver::NamedValue> args) {
    std::string query(q);
    std::size_t start = 0;
    ptr<driver::Result> res;
    for (;;) {
        auto [s, err] = prepare(query);
        if (err) return {nullptr, err};
        if (s->handle()) {
            std::size_t na = static_cast<std::size_t>(s->NumInput());
            if (args.size() - start < na) {
                s->Close();
                return {nullptr, not_enough_args(na, args.size() - start)};
            }
            auto sargs = stmtArgs(args, start, na);
            auto [r, eerr] = s->exec(ctx, sargs);
            if (eerr && eerr != driver::ErrSkip) {
                s->Close();
                return {nullptr, eerr};
            }
            res = r;
            start += na;
        }
        std::string tail = s->tail();
        s->Close();
        if (tail.empty()) {
            if (!res) res = make_ptr<result>(0, 0);
            return {res, nullptr};
        }
        query = tail;
    }
}

std::pair<ptr<driver::Rows>, error> conn::query(const ptr<context::Context>& ctx, std::string_view q,
                                                std::span<const driver::NamedValue> args) {
    std::string query(q);
    std::size_t start = 0;
    for (;;) {
        auto [s, err] = prepare(query);
        if (err) return {nullptr, err};
        s->cls = true;
        if (!s->handle()) {
            // A query of only whitespace or comments.
            std::string tail = s->tail();
            s->Close();
            if (tail.empty()) return {make_ptr<rows>(nullptr, true, ctx), nullptr};
            query = tail;
            continue;
        }
        std::size_t na = static_cast<std::size_t>(s->NumInput());
        if (args.size() - start < na) {
            s->Close();
            return {nullptr, not_enough_args(na, args.size() - start)};
        }
        auto sargs = stmtArgs(args, start, na);
        auto [r, qerr] = s->query(ctx, sargs);
        if (qerr && qerr != driver::ErrSkip) {
            s->Close();
            return {r, qerr};
        }
        start += na;
        std::string tail = s->tail();
        if (tail.empty()) return {r, nullptr};
        r->Close();
        s->Close();
        query = tail;
    }
}

uint64_t conn::new_op_id() noexcept {
    std::lock_guard l(mu_);
    return next_op_++;
}

void conn::begin_op(uint64_t id) noexcept {
    std::lock_guard l(mu_);
    active_op_ = id;
}

void conn::end_op() noexcept {
    std::lock_guard l(mu_);
    active_op_ = 0;
}

void conn::interrupt(uint64_t id) noexcept {
    std::lock_guard l(mu_);
    if (active_op_ == id && db_) sqlite3_interrupt(db_);
}

// ---------------------------------------------------------------------------
// Cancellation

canceler::canceler(ptr<conn> c, ptr<context::Context> ctx) : c_(std::move(c)), ctx_(std::move(ctx)) {
    if (!ctx_ || !ctx_->Done()) return;
    id_ = c_->new_op_id();
    done_ = make_chan<bool>();
    go([c = c_, ctx = ctx_, done = done_, id = id_] {
        select(recv(ctx->Done(), [&] { c->interrupt(id); }), recv(done, [] {}));
    });
}

canceler::~canceler() {
    if (id_) done_.close();
}

void canceler::begin() noexcept {
    if (!id_) return;
    c_->begin_op(id_);
    // A cancellation just before begin found no operation to interrupt.
    if (ctx_->Err()) c_->interrupt(id_);
}

void canceler::end() noexcept {
    if (id_) c_->end_op();
}

error canceler::ctx_err() const { return id_ ? ctx_->Err() : nullptr; }

// ---------------------------------------------------------------------------
// Transactions

error tx::Commit() {
    auto [_, err] = c->exec(context::Background(), "COMMIT", {});
    if (err) {
        // sqlite3 may leave the transaction open in this scenario. However,
        // database/sql considers the transaction complete once we return
        // from Commit() - we must clean up to honour its semantics. We
        // don't know if the ROLLBACK is strictly necessary, but according
        // to sqlite's docs, there is no harm in calling ROLLBACK
        // unnecessarily.
        c->exec(context::Background(), "ROLLBACK", {});
    }
    return err;
}

error tx::Rollback() {
    auto [_, err] = c->exec(context::Background(), "ROLLBACK", {});
    return err;
}

}  // namespace internal

}  // namespace gopp::sqlite3
