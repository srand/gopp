// The sqlite3 driver's internals: mattn/go-sqlite3's SQLiteConn,
// SQLiteStmt, SQLiteRows, SQLiteTx and SQLiteResult.
#pragma once

#include <gopp/context.hpp>
#include <gopp/sqlite3.hpp>
#include <gopp/time.hpp>

#include <sqlite3.h>

#include <array>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace gopp::sqlite3::internal {

namespace driver = sql::driver;

// Go's lastError: the connection's last error, or nullptr if it has none.
error lastError(::sqlite3* db);

class stmt;

// SQLiteConn: one SQLite connection. database/sql uses it from one fiber
// at a time; the fiber that watches a context for cancellation calls
// interrupt from another.
class conn final : public driver::Conn,
                   public driver::ConnBeginTx,
                   public driver::ConnPrepareContext,
                   public driver::ExecerContext,
                   public driver::QueryerContext,
                   public driver::Pinger {
public:
    conn(::sqlite3* db, const time::Location* loc, std::string txlock) : db_(db), loc_(loc), txlock_(std::move(txlock)) {}
    ~conn() override;

    std::pair<ptr<driver::Stmt>, error> Prepare(std::string_view query) override;
    error Close() override;
    std::pair<ptr<driver::Tx>, error> Begin() override;
    std::pair<ptr<driver::Tx>, error> BeginTx(ptr<context::Context> ctx, driver::TxOptions opts) override;
    std::pair<ptr<driver::Stmt>, error> PrepareContext(ptr<context::Context> ctx, std::string_view query) override;
    std::pair<ptr<driver::Result>, error> ExecContext(ptr<context::Context> ctx, std::string_view query,
                                                      std::span<const driver::NamedValue> args) override;
    std::pair<ptr<driver::Rows>, error> QueryContext(ptr<context::Context> ctx, std::string_view query,
                                                     std::span<const driver::NamedValue> args) override;
    error Ping(ptr<context::Context> ctx) override;

    // Go's exec, query and prepare.
    std::pair<ptr<driver::Result>, error> exec(const ptr<context::Context>& ctx, std::string_view query,
                                               std::span<const driver::NamedValue> args);
    std::pair<ptr<driver::Rows>, error> query(const ptr<context::Context>& ctx, std::string_view query,
                                              std::span<const driver::NamedValue> args);
    std::pair<ptr<stmt>, error> prepare(std::string_view query);

    // The handle, nullptr once closed. Only the fiber using the connection
    // reads it without mu_.
    ::sqlite3* db() const noexcept { return db_; }
    const time::Location* loc() const noexcept { return loc_; }

    // Cancellation: an operation that a context may cancel runs between
    // begin_op and end_op, and interrupt(id) interrupts it only while it
    // runs, so that a late cancellation can't hit the next statement.
    uint64_t new_op_id() noexcept;
    void begin_op(uint64_t id) noexcept;
    void end_op() noexcept;
    void interrupt(uint64_t id) noexcept;

private:
    ::sqlite3* db_;
    const time::Location* loc_;
    std::string txlock_;
    std::mutex mu_;  // guards db_ for interrupt, and active_op_
    uint64_t active_op_ = 0;
    uint64_t next_op_ = 1;
};

// Watches a context while a statement runs: when it is done, interrupts
// the connection's operation. A fiber does the watching, as a goroutine
// does in mattn's driver; nothing is started for contexts that can't be
// canceled.
class canceler {
public:
    canceler(ptr<conn> c, ptr<context::Context> ctx);
    ~canceler();
    canceler(const canceler&) = delete;
    canceler& operator=(const canceler&) = delete;

    // Around each SQLite step.
    void begin() noexcept;
    void end() noexcept;
    // ctx's error, or nullptr.
    error ctx_err() const;
    bool active() const noexcept { return id_ != 0; }

private:
    ptr<conn> c_;
    ptr<context::Context> ctx_;
    uint64_t id_ = 0;
    chan<bool> done_;
};

// SQLiteStmt: a prepared statement and the rest of its query (tail).
class stmt final : public driver::Stmt, public driver::StmtExecContext, public driver::StmtQueryContext {
public:
    stmt(ptr<conn> c, sqlite3_stmt* s, std::string tail);
    ~stmt() override;

    error Close() override;
    int NumInput() override { return num_input_; }
    std::pair<ptr<driver::Result>, error> Exec(std::span<const driver::Value> args) override;
    std::pair<ptr<driver::Rows>, error> Query(std::span<const driver::Value> args) override;
    std::pair<ptr<driver::Result>, error> ExecContext(ptr<context::Context> ctx,
                                                      std::span<const driver::NamedValue> args) override;
    std::pair<ptr<driver::Rows>, error> QueryContext(ptr<context::Context> ctx,
                                                     std::span<const driver::NamedValue> args) override;

    std::pair<ptr<driver::Result>, error> exec(const ptr<context::Context>& ctx, std::span<const driver::NamedValue> args);
    std::pair<ptr<driver::Rows>, error> query(const ptr<context::Context>& ctx, std::span<const driver::NamedValue> args);
    // Binds args after resetting the statement.
    error bind(std::span<const driver::NamedValue> args);

    sqlite3_stmt* handle() const noexcept { return s_; }
    const std::string& tail() const noexcept { return tail_; }
    bool closed() const noexcept { return closed_; }
    const ptr<conn>& connection() const noexcept { return c_; }
    // Set when the statement belongs to a Query of the connection, so that
    // closing its rows closes it.
    bool cls = false;

private:
    // The indices of :name, @name and $name.
    std::array<int, 3> bindNamedIndices(const std::string& name);
    error bindError(const driver::Value& v);

    ptr<conn> c_;
    sqlite3_stmt* s_;
    std::string tail_;
    bool closed_ = false;
    int num_input_ = 0;
    std::map<std::string, std::array<int, 3>> named_params_;
};

// SQLiteRows: the rows of a query. The first step runs in Query, so that
// errors show there.
class rows final : public driver::Rows,
                   public driver::RowsColumnTypeDatabaseTypeName,
                   public driver::RowsColumnTypeNullable,
                   public driver::RowsColumnTypeScanType {
public:
    rows(ptr<stmt> s, bool cls, ptr<context::Context> ctx);
    ~rows() override;

    std::vector<string> Columns() override;
    error Close() override;
    error Next(std::span<driver::Value> dest) override;
    string ColumnTypeDatabaseTypeName(int index) override;
    std::pair<bool, bool> ColumnTypeNullable(int index) override;
    string ColumnTypeScanType(int index) override;

    // Binds args and takes the first step; the error if it failed.
    error first_step(std::span<const driver::NamedValue> args);
    // mattn's SQLiteRows.DeclTypes: the declared column types, in lower
    // case.
    std::vector<std::string> DeclTypes() { return declTypes(); }

private:
    int step();
    error readStepResult(std::span<driver::Value> dest, int rv);
    const std::vector<std::string>& declTypes();

    ptr<stmt> s_;
    bool cls_;
    ptr<context::Context> ctx_;
    std::unique_ptr<canceler> cancel_;
    int nc_ = 0;
    int pending_ = -1;  // the result of the first step, until Next reads it
    std::vector<std::string> decltype_;
    bool have_decltype_ = false;
};

// SQLiteResult
struct result final : driver::Result {
    int64 id = 0, changes = 0;
    result(int64 i, int64 c) : id(i), changes(c) {}
    std::pair<int64, error> LastInsertId() override { return {id, nullptr}; }
    std::pair<int64, error> RowsAffected() override { return {changes, nullptr}; }
};

// SQLiteTx
struct tx final : driver::Tx {
    ptr<conn> c;
    explicit tx(ptr<conn> conn) : c(std::move(conn)) {}
    error Commit() override;
    error Rollback() override;
};

// Go's stmtArgs: the na arguments of the statement starting at start, and
// the named arguments from elsewhere, renumbered.
std::vector<driver::NamedValue> stmtArgs(std::span<const driver::NamedValue> args, std::size_t start, std::size_t na);

// Go's valueToNamedValue.
std::vector<driver::NamedValue> valueToNamedValue(std::span<const driver::Value> args);

// SQLite calls that can wait or do I/O, as blocking calls.
int blocking_step(sqlite3_stmt* s);

}  // namespace gopp::sqlite3::internal
