// The database/sql/driver package: the interfaces a database driver
// implements for database/sql, after Go's database/sql/driver.
//
// Drivers implement Driver and Conn, and the optional interfaces
// (ExecerContext, ConnBeginTx, SessionResetter, ...) as extra bases;
// database/sql finds them with dynamic_cast, as Go does with type
// assertions.
#pragma once

// The module interfaces (modules/) import these instead.
#ifndef GOPP_MODULE
#include <gopp/context.hpp>
#include <gopp/errors.hpp>
#include <gopp/ptr.hpp>
#include <gopp/slice.hpp>
#include <gopp/string.hpp>
#include <gopp/time.hpp>

#include <cstddef>
#include <span>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>
#endif

namespace gopp::sql::driver {

// driver.Value: what drivers handle. Go's nil, int64, float64, bool,
// []byte, string and time.Time.
using Value = std::variant<std::nullptr_t, int64, double, bool, slice<byte>, string, time::Time>;

// The Go type name of a Value: "<nil>", "int64", "float64", "bool",
// "[]uint8", "string", "time.Time" (for errors in Go's words).
std::string_view TypeName(const Value& v) noexcept;
// The value as Go's %v prints it.
string Format(const Value& v);
// Go's == on values ([]byte compares by content here, as Go can't).
bool Equal(const Value& a, const Value& b) noexcept;

// driver.NamedValue: an argument with its name (empty for positional
// ones) and 1-based position.
struct NamedValue {
    string Name;
    int Ordinal = 0;
    driver::Value Value;
};

struct Conn;
struct Connector;
struct Driver;
struct Result;
struct Rows;
struct Stmt;
struct Tx;

// driver.Driver
struct Driver : public virtual object {
    virtual std::pair<ptr<Conn>, error> Open(std::string_view name) = 0;
};

// driver.DriverContext: a driver that parses the name once.
struct DriverContext : public virtual object {
    virtual std::pair<ptr<Connector>, error> OpenConnector(std::string_view name) = 0;
};

// driver.Connector: a configured driver.
struct Connector : public virtual object {
    virtual std::pair<ptr<Conn>, error> Connect(ptr<context::Context> ctx) = 0;
    virtual ptr<driver::Driver> Driver() = 0;
};

extern const error ErrSkip;            // "driver: skip fast-path; continue as if unimplemented"
extern const error ErrBadConn;         // "driver: bad connection"
extern const error ErrRemoveArgument;  // "driver: remove argument from query"

// driver.Pinger
struct Pinger : public virtual object {
    virtual error Ping(ptr<context::Context> ctx) = 0;
};

// driver.Execer
struct Execer : public virtual object {
    virtual std::pair<ptr<Result>, error> Exec(std::string_view query, std::span<const Value> args) = 0;
};

// driver.ExecerContext
struct ExecerContext : public virtual object {
    virtual std::pair<ptr<Result>, error> ExecContext(ptr<context::Context> ctx, std::string_view query,
                                                      std::span<const NamedValue> args) = 0;
};

// driver.Queryer
struct Queryer : public virtual object {
    virtual std::pair<ptr<Rows>, error> Query(std::string_view query, std::span<const Value> args) = 0;
};

// driver.QueryerContext
struct QueryerContext : public virtual object {
    virtual std::pair<ptr<Rows>, error> QueryContext(ptr<context::Context> ctx, std::string_view query,
                                                     std::span<const NamedValue> args) = 0;
};

// driver.Conn: a connection, used by one fiber at a time.
struct Conn : public virtual object {
    virtual std::pair<ptr<Stmt>, error> Prepare(std::string_view query) = 0;
    virtual error Close() = 0;
    virtual std::pair<ptr<Tx>, error> Begin() = 0;
};

// driver.ConnPrepareContext
struct ConnPrepareContext : public virtual object {
    virtual std::pair<ptr<Stmt>, error> PrepareContext(ptr<context::Context> ctx, std::string_view query) = 0;
};

// driver.IsolationLevel: sql::IsolationLevel's values.
using IsolationLevel = int;

// driver.TxOptions
struct TxOptions {
    IsolationLevel Isolation = 0;
    bool ReadOnly = false;
};

// driver.ConnBeginTx
struct ConnBeginTx : public virtual object {
    virtual std::pair<ptr<Tx>, error> BeginTx(ptr<context::Context> ctx, TxOptions opts) = 0;
};

// driver.SessionResetter: called before a pooled connection is reused.
struct SessionResetter : public virtual object {
    virtual error ResetSession(ptr<context::Context> ctx) = 0;
};

// driver.Validator: whether a connection may go back to the pool.
struct Validator : public virtual object {
    virtual bool IsValid() = 0;
};

// driver.Result
struct Result : public virtual object {
    virtual std::pair<int64, error> LastInsertId() = 0;
    virtual std::pair<int64, error> RowsAffected() = 0;
};

// driver.Stmt: a prepared statement, bound to its connection.
struct Stmt : public virtual object {
    virtual error Close() = 0;
    // The number of placeholders, or -1 if the driver doesn't know.
    virtual int NumInput() = 0;
    virtual std::pair<ptr<Result>, error> Exec(std::span<const Value> args) = 0;
    virtual std::pair<ptr<Rows>, error> Query(std::span<const Value> args) = 0;
};

// driver.StmtExecContext
struct StmtExecContext : public virtual object {
    virtual std::pair<ptr<Result>, error> ExecContext(ptr<context::Context> ctx, std::span<const NamedValue> args) = 0;
};

// driver.StmtQueryContext
struct StmtQueryContext : public virtual object {
    virtual std::pair<ptr<Rows>, error> QueryContext(ptr<context::Context> ctx, std::span<const NamedValue> args) = 0;
};

// driver.NamedValueChecker: checks and converts arguments; may return
// ErrSkip (use the default checks) or ErrRemoveArgument.
struct NamedValueChecker : public virtual object {
    virtual error CheckNamedValue(NamedValue& nv) = 0;
};

// driver.ValueConverter
struct ValueConverter {
    virtual ~ValueConverter() = default;
    virtual std::pair<Value, error> ConvertValue(const Value& v) const = 0;
};

// driver.ColumnConverter (deprecated in Go for NamedValueChecker): a
// statement's converter for argument idx. Go's method is named like the
// interface, which C++ reserves for constructors: here it is Converter.
struct ColumnConverter : public virtual object {
    virtual const ValueConverter& Converter(int idx) = 0;
};

// driver.Rows: a cursor. Next fills dest (sized to Columns()) and returns
// io::EOF_ at the end.
struct Rows : public virtual object {
    virtual std::vector<string> Columns() = 0;
    virtual error Close() = 0;
    virtual error Next(std::span<Value> dest) = 0;
};

// driver.RowsNextResultSet
struct RowsNextResultSet : public virtual object {
    virtual bool HasNextResultSet() = 0;
    virtual error NextResultSet() = 0;
};

// driver.RowsColumnTypeScanType: Go returns a reflect.Type; here, the Go
// name of the type to scan into ("int64", "sql.NullString", ...).
struct RowsColumnTypeScanType : public virtual object {
    virtual string ColumnTypeScanType(int index) = 0;
};

// driver.RowsColumnTypeDatabaseTypeName
struct RowsColumnTypeDatabaseTypeName : public virtual object {
    virtual string ColumnTypeDatabaseTypeName(int index) = 0;
};

// driver.RowsColumnTypeLength
struct RowsColumnTypeLength : public virtual object {
    virtual std::pair<int64, bool> ColumnTypeLength(int index) = 0;
};

// driver.RowsColumnTypeNullable
struct RowsColumnTypeNullable : public virtual object {
    virtual std::pair<bool, bool> ColumnTypeNullable(int index) = 0;
};

// driver.RowsColumnTypePrecisionScale
struct RowsColumnTypePrecisionScale : public virtual object {
    virtual std::tuple<int64, int64, bool> ColumnTypePrecisionScale(int index) = 0;
};

// driver.Tx
struct Tx : public virtual object {
    virtual error Commit() = 0;
    virtual error Rollback() = 0;
};

// driver.RowsAffected(n): a Result for statements that report only the
// rows they affected.
ptr<Result> RowsAffected(int64 n);
// driver.ResultNoRows: for statements without results, such as DDL.
extern const ptr<Result> ResultNoRows;

// driver.Valuer: a type that converts itself to a Value, for arguments.
// database/sql also accepts any type with a const Value() member returning
// std::pair<driver::Value, error>, like the Null types.
struct Valuer {
    virtual ~Valuer() = default;
    virtual std::pair<driver::Value, error> Value() const = 0;
};

// The converters of the driver package.
extern const ValueConverter& Bool;    // driver.Bool
extern const ValueConverter& Int32;   // driver.Int32
extern const ValueConverter& String;  // driver.String
// driver.DefaultParameterConverter. Arguments become Values when they are
// passed (C++ types are known then), so this keeps a Value as it is.
extern const ValueConverter& DefaultParameterConverter;

// driver.Null: nil stays nil; other values go to Converter.
struct Null final : ValueConverter {
    const ValueConverter& Converter;
    explicit Null(const ValueConverter& c) : Converter(c) {}
    std::pair<driver::Value, error> ConvertValue(const driver::Value& v) const override;
};

// driver.NotNull: nil is an error; other values go to Converter.
struct NotNull final : ValueConverter {
    const ValueConverter& Converter;
    explicit NotNull(const ValueConverter& c) : Converter(c) {}
    std::pair<driver::Value, error> ConvertValue(const driver::Value& v) const override;
};

// driver.IsValue: every Value is one; for symmetry with Go.
inline bool IsValue(const Value&) noexcept { return true; }

}  // namespace gopp::sql::driver
