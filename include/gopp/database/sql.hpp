// The database/sql package: a generic interface around SQL databases, with
// a connection pool, after Go's database/sql.
//
//     auto [db, err] = sql::Open("mydriver", "dsn");
//     if (err) return err;
//     auto [rows, qerr] = db->Query("SELECT name, age FROM people WHERE age > ?", 21);
//     if (qerr) return qerr;
//     while (rows->Next()) {
//         string name;
//         int64 age;
//         if (auto err = rows->Scan(&name, &age)) return err;
//     }
//     if (auto err = rows->Err()) return err;
//
// Arguments become driver::Values when they are passed: integers become
// int64, floating point float64, strings and bytes stay what they are, and
// types with a Value() member (driver::Valuer, the Null types) convert
// themselves. Scan stores into pointers to strings, bytes, RawBytes,
// integers, floating point, bool, time::Time, driver::Value (Go's any),
// std::optional<T> (Go's **T: nullopt for NULL) and Scanners, converting as
// Go's convertAssign does.
#pragma once

// The module interfaces (modules/) import these instead.
#ifndef GOPP_MODULE
#include <gopp/chan.hpp>
#include <gopp/context.hpp>
#include <gopp/database/sql/driver.hpp>
#include <gopp/errors.hpp>
#include <gopp/sync.hpp>
#include <gopp/time.hpp>

#include <array>
#include <atomic>
#include <concepts>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#endif

namespace gopp::sql {

class Conn;
class DB;
struct testAccess;  // for tests, like Go's in-package tests
class Row;
class Rows;
class Stmt;
class Tx;

// sql.Register: makes a driver available under name. Panics if name is
// taken or the driver is nil.
void Register(std::string_view name, ptr<driver::Driver> d);
// sql.Drivers: the registered names, sorted.
std::vector<string> Drivers();

// sql.IsolationLevel
class IsolationLevel {
public:
    constexpr IsolationLevel(int v = 0) noexcept : v_(v) {}
    constexpr operator int() const noexcept { return v_; }
    // "Default", "Read Uncommitted", ...
    string String() const;

private:
    int v_;
};

inline constexpr IsolationLevel LevelDefault{0};
inline constexpr IsolationLevel LevelReadUncommitted{1};
inline constexpr IsolationLevel LevelReadCommitted{2};
inline constexpr IsolationLevel LevelWriteCommitted{3};
inline constexpr IsolationLevel LevelRepeatableRead{4};
inline constexpr IsolationLevel LevelSnapshot{5};
inline constexpr IsolationLevel LevelSerializable{6};
inline constexpr IsolationLevel LevelLinearizable{7};

// sql.TxOptions
struct TxOptions {
    IsolationLevel Isolation = LevelDefault;
    bool ReadOnly = false;
};

// sql.RawBytes: bytes owned by the database, valid until the next call to
// Next, Scan or Close.
class RawBytes : public slice<byte> {
public:
    RawBytes() = default;
    RawBytes(slice<byte> b) : slice<byte>(std::move(b)) {}
};

// sql.Scanner: a type that scans itself from a column value. database/sql
// also accepts any type with an error Scan(const driver::Value&) member.
struct Scanner {
    virtual ~Scanner() = default;
    virtual error Scan(const driver::Value& src) = 0;
};

extern const error ErrNoRows;    // "sql: no rows in result set"
extern const error ErrConnDone;  // "sql: connection is already closed"
extern const error ErrTxDone;    // "sql: transaction has already been committed or rolled back"

namespace detail {

// An argument, converted to a driver::Value when it was passed.
struct arg {
    string name;
    driver::Value value;
    error err;  // a conversion error, reported when the query runs
};

template <typename T>
concept hasValueMethod = requires(const T& t) {
    { t.Value() } -> std::same_as<std::pair<driver::Value, error>>;
};

template <typename T>
concept hasScanMethod = requires(T& t, const driver::Value& v) {
    { t.Scan(v) } -> std::same_as<error>;
};

template <typename T>
struct isOptional : std::false_type {};
template <typename T>
struct isOptional<std::optional<T>> : std::true_type {};

template <typename T>
inline constexpr bool isStringLike = std::is_same_v<T, gopp::string> || std::is_same_v<T, std::string> ||
                                       std::is_same_v<T, std::string_view> || std::is_same_v<T, const char*> ||
                                       std::is_same_v<T, char*>;

extern const error errUint64HighBit;

}  // namespace detail

// sql.NamedArg, made with sql::Named.
struct NamedArg {
    string Name;
    detail::arg Arg;
};

// The Value of a C++ argument, as database/sql passes it to drivers.
template <typename T>
detail::arg toArg(const T& v) {
    using U = std::remove_cvref_t<T>;
    if constexpr (std::is_same_v<U, NamedArg>) {
        detail::arg a = v.Arg;
        a.name = v.Name;
        return a;
    } else if constexpr (std::is_same_v<U, driver::Value>) {
        return {{}, v, nullptr};
    } else if constexpr (detail::hasValueMethod<U>) {
        auto [val, err] = v.Value();
        return {{}, std::move(val), std::move(err)};
    } else if constexpr (std::is_base_of_v<driver::Valuer, U>) {
        auto [val, err] = v.Value();
        return {{}, std::move(val), std::move(err)};
    } else if constexpr (std::is_same_v<U, std::nullptr_t>) {
        return {};
    } else if constexpr (detail::isOptional<U>::value) {
        if (!v) return {};
        return toArg(*v);
    } else if constexpr (std::is_array_v<U> && std::is_same_v<std::remove_cv_t<std::remove_extent_t<U>>, char>) {
        return {{}, string(std::string_view(v)), nullptr};
    } else if constexpr (detail::isStringLike<U>) {
        return {{}, string(std::string_view(v)), nullptr};
    } else if constexpr (std::is_pointer_v<U>) {
        if (!v) return {};
        return toArg(*v);
    } else if constexpr (std::is_same_v<U, bool>) {
        return {{}, v, nullptr};
    } else if constexpr (std::is_enum_v<U>) {
        return {{}, static_cast<int64>(v), nullptr};
    } else if constexpr (std::is_integral_v<U> && std::is_unsigned_v<U>) {
        if constexpr (sizeof(U) >= 8) {
            if (static_cast<uint64>(v) >= uint64(1) << 63) return {{}, nullptr, detail::errUint64HighBit};
        }
        return {{}, static_cast<int64>(v), nullptr};
    } else if constexpr (std::is_integral_v<U>) {
        return {{}, static_cast<int64>(v), nullptr};
    } else if constexpr (std::is_floating_point_v<U>) {
        return {{}, static_cast<double>(v), nullptr};
    } else if constexpr (std::is_same_v<U, gopp::string>) {
        return {{}, v, nullptr};
    } else if constexpr (std::is_base_of_v<slice<byte>, U>) {
        return {{}, slice<byte>(v), nullptr};
    } else if constexpr (std::is_same_v<U, time::Time>) {
        return {{}, v, nullptr};
    } else if constexpr (std::is_same_v<U, time::Duration>) {
        return {{}, v.ns, nullptr};
    } else {
        static_assert(sizeof(U) == 0, "sql: unsupported argument type");
    }
}

// sql.Named: a named argument, for drivers with named placeholders.
template <typename T>
NamedArg Named(std::string_view name, const T& value) {
    return NamedArg{string(name), toArg(value)};
}

// ---------------------------------------------------------------------------
// The Null types: nullable column values, usable as arguments and Scan
// destinations.

namespace detail {
error convertString(const driver::Value& src, gopp::string& out);
error convertBool(const driver::Value& src, bool& out);
error convertInt(const driver::Value& src, int bits, std::string_view kind, int64& out);
error convertUint(const driver::Value& src, int bits, std::string_view kind, uint64& out);
error convertFloat(const driver::Value& src, int bits, std::string_view kind, double& out);
error convertTime(const driver::Value& src, time::Time& out);
}  // namespace detail

// sql.NullString
struct NullString {
    gopp::string String;
    bool Valid = false;  // whether String is not NULL
    error Scan(const driver::Value& value);
    std::pair<driver::Value, error> Value() const;
};

// sql.NullInt64
struct NullInt64 {
    int64 Int64 = 0;
    bool Valid = false;
    error Scan(const driver::Value& value);
    std::pair<driver::Value, error> Value() const;
};

// sql.NullInt32
struct NullInt32 {
    int32 Int32 = 0;
    bool Valid = false;
    error Scan(const driver::Value& value);
    std::pair<driver::Value, error> Value() const;
};

// sql.NullInt16
struct NullInt16 {
    int16 Int16 = 0;
    bool Valid = false;
    error Scan(const driver::Value& value);
    std::pair<driver::Value, error> Value() const;
};

// sql.NullByte
struct NullByte {
    byte Byte = 0;
    bool Valid = false;
    error Scan(const driver::Value& value);
    std::pair<driver::Value, error> Value() const;
};

// sql.NullFloat64
struct NullFloat64 {
    double Float64 = 0;
    bool Valid = false;
    error Scan(const driver::Value& value);
    std::pair<driver::Value, error> Value() const;
};

// sql.NullBool
struct NullBool {
    bool Bool = false;
    bool Valid = false;
    error Scan(const driver::Value& value);
    std::pair<driver::Value, error> Value() const;
};

// sql.NullTime
struct NullTime {
    time::Time Time;
    bool Valid = false;
    error Scan(const driver::Value& value);
    std::pair<driver::Value, error> Value() const;
};

template <typename T>
error convertAssign(T* dest, const driver::Value& src, Rows* rows = nullptr);

// sql.Null[T]
template <typename T>
struct Null {
    T V{};
    bool Valid = false;

    error Scan(const driver::Value& value) {
        if (std::holds_alternative<std::nullptr_t>(value)) {
            V = T{};
            Valid = false;
            return nullptr;
        }
        error err = convertAssign(&V, value);
        Valid = !err;
        return err;
    }

    std::pair<driver::Value, error> Value() const {
        if (!Valid) return {nullptr, nullptr};
        detail::arg a = toArg(V);
        return {std::move(a.value), std::move(a.err)};
    }
};

// ---------------------------------------------------------------------------
// Scan destinations

namespace detail {

// A Scan destination: a pointer and how to assign to it.
struct dest {
    void* p;
    error (*assign)(void* p, const driver::Value& src, Rows* rows);
    bool rawBytes;
};

extern const error errNilPtr;  // "destination pointer is nil"

error convertBytes(const driver::Value& src, slice<byte>& out);
error convertRawBytes(const driver::Value& src, RawBytes& out, Rows* rows);
error convertAny(const driver::Value& src, driver::Value& out);
error unsupportedScan(const driver::Value& src, std::string_view destType);

// Go's kind names and %T names of destination types.
template <typename T>
constexpr std::string_view intKind() {
    if constexpr (std::is_same_v<T, signed char> || std::is_same_v<T, char>) return "int8";
    else if constexpr (std::is_same_v<T, short>) return "int16";
    else if constexpr (std::is_same_v<T, int>) return "int";
    else if constexpr (std::is_same_v<T, long> || std::is_same_v<T, long long>) return "int64";
    else if constexpr (std::is_same_v<T, unsigned char>) return "uint8";
    else if constexpr (std::is_same_v<T, unsigned short>) return "uint16";
    else if constexpr (std::is_same_v<T, unsigned int>) return "uint";
    else return "uint64";
}

template <typename T>
std::string typeName() {
    if constexpr (std::is_same_v<T, bool>) return "bool";
    else if constexpr (std::is_integral_v<T>) return std::string(intKind<T>());
    else if constexpr (std::is_same_v<T, float>) return "float32";
    else if constexpr (std::is_same_v<T, double>) return "float64";
    else if constexpr (std::is_same_v<T, gopp::string> || std::is_same_v<T, std::string>) return "string";
    else if constexpr (std::is_same_v<T, RawBytes>) return "sql.RawBytes";
    else if constexpr (std::is_same_v<T, slice<byte>>) return "[]uint8";
    else if constexpr (std::is_same_v<T, time::Time>) return "time.Time";
    else if constexpr (std::is_same_v<T, driver::Value>) return "interface {}";
    else if constexpr (isOptional<T>::value) return "*" + typeName<typename T::value_type>();
    else return "unknown";
}

}  // namespace detail

// Go's convertAssign: stores src into *dest, converting.
template <typename T>
error convertAssign(T* d, const driver::Value& src, Rows* rows) {
    if (!d) return detail::errNilPtr;
    if constexpr (std::is_same_v<T, gopp::string>) {
        return detail::convertString(src, *d);
    } else if constexpr (std::is_same_v<T, std::string>) {
        gopp::string s;
        error err = detail::convertString(src, s);
        if (!err) *d = s.str();
        return err;
    } else if constexpr (std::is_same_v<T, RawBytes>) {
        return detail::convertRawBytes(src, *d, rows);
    } else if constexpr (std::is_same_v<T, slice<byte>>) {
        return detail::convertBytes(src, *d);
    } else if constexpr (std::is_same_v<T, bool>) {
        return detail::convertBool(src, *d);
    } else if constexpr (std::is_same_v<T, driver::Value>) {
        return detail::convertAny(src, *d);
    } else if constexpr (detail::hasScanMethod<T>) {
        return d->Scan(src);
    } else if constexpr (std::is_base_of_v<Scanner, T>) {
        return static_cast<Scanner&>(*d).Scan(src);
    } else if constexpr (std::is_same_v<T, time::Time>) {
        if (auto* t = std::get_if<time::Time>(&src)) {
            *d = *t;
            return nullptr;
        }
        return detail::unsupportedScan(src, "*time.Time");
    } else if constexpr (detail::isOptional<T>::value) {
        // Go's pointer destinations: nil for NULL, else a new value.
        if (std::holds_alternative<std::nullptr_t>(src)) {
            d->reset();
            return nullptr;
        }
        d->emplace();
        return convertAssign(&**d, src, rows);
    } else if constexpr (std::is_integral_v<T> && std::is_signed_v<T>) {
        if constexpr (std::is_same_v<T, int64>) {
            if (auto* i = std::get_if<int64>(&src)) {
                *d = *i;
                return nullptr;
            }
        }
        int64 v;
        if (error err = detail::convertInt(src, static_cast<int>(sizeof(T) * 8), detail::intKind<T>(), v)) return err;
        *d = static_cast<T>(v);
        return nullptr;
    } else if constexpr (std::is_integral_v<T>) {
        uint64 v;
        if (error err = detail::convertUint(src, static_cast<int>(sizeof(T) * 8), detail::intKind<T>(), v)) return err;
        *d = static_cast<T>(v);
        return nullptr;
    } else if constexpr (std::is_floating_point_v<T>) {
        if constexpr (std::is_same_v<T, double>) {
            if (auto* f = std::get_if<double>(&src)) {
                *d = *f;
                return nullptr;
            }
        }
        double v;
        constexpr int bits = sizeof(T) == 4 ? 32 : 64;
        if (error err = detail::convertFloat(src, bits, bits == 32 ? "float32" : "float64", v)) return err;
        *d = static_cast<T>(v);
        return nullptr;
    } else {
        static_assert(sizeof(T) == 0, "sql: unsupported Scan destination type");
    }
}

namespace detail {

template <typename T>
dest makeDest(T* p) {
    return dest{p,
                [](void* p, const driver::Value& src, Rows* rows) -> error {
                    return convertAssign(static_cast<T*>(p), src, rows);
                },
                std::is_same_v<T, RawBytes>};
}

template <typename... A>
std::vector<arg> args(const A&... a) {
    return {toArg(a)...};
}

// closingMutex: an RWMutex where readers have priority over the writer,
// so that reads are safely reentrant (closemu.go).
class closingMutex {
public:
    void RLock();
    void RUnlock();
    void Lock();
    void Unlock();
    bool TryRLock();
    // For std::lock_guard.
    void lock() { Lock(); }
    void unlock() { Unlock(); }

private:
    std::atomic<int64> state_{0};
    sync::Mutex mu_;
    sync::Cond read_{&mu_};
    sync::Cond write_{&mu_};
};

// What DB.dep tracks: objects closed when their last dependency goes.
struct finalCloser {
    virtual ~finalCloser() = default;
    virtual error finalClose() = 0;
};

struct driverStmt;

// A connection of the pool (driverConn).
struct driverConn final : public object, public finalCloser {
    DB* db = nullptr;
    time::Time createdAt;

    sync::Mutex mu;  // guards the following
    ptr<driver::Conn> ci;
    bool needReset = false;
    bool closed = false;
    bool finalClosed = false;
    std::unordered_map<driverStmt*, ptr<driverStmt>> openStmt;

    // Guarded by db->mu_.
    bool inUse = false;
    bool dbmuClosed = false;
    time::Time returnedAt;
    std::vector<std::function<void()>> onPut;

    void lock() { mu.lock(); }
    void unlock() { mu.unlock(); }

    void releaseConn(error err);
    void removeOpenStmt(driverStmt* ds);
    bool expired(time::Duration timeout) const;
    error resetSession(ptr<context::Context> ctx);
    bool validateConnection(bool needsReset);
    std::pair<ptr<driverStmt>, error> prepareLocked(ptr<context::Context> ctx, bool grabbed, std::string_view query);
    std::function<error()> closeDbLocked();
    error Close();
    error finalClose() override;
};

// A driver statement and the connection whose lock guards it (driverStmt).
struct driverStmt final : public object {
    driverConn* dc = nullptr;  // the Locker
    ptr<driver::Stmt> si;
    bool closed = false;
    error closeErr;

    void lock() { dc->lock(); }
    void unlock() { dc->unlock(); }
    error Close();
};

struct connRequest {
    ptr<driverConn> conn;
    error err;
};

// connRequestSet: waiters for a connection, taken at random.
class connRequestSet {
public:
    using handle = std::shared_ptr<int>;
    handle Add(chan<connRequest> c);
    bool Delete(const handle& h);
    std::optional<chan<connRequest>> TakeRandom();
    void CloseAndRemoveAll();
    std::size_t Len() const noexcept { return s_.size(); }

private:
    void deleteIndex(std::size_t idx);
    std::vector<std::pair<chan<connRequest>, handle>> s_;
};

using releaseFn = std::function<void(error)>;

// Test hook (rowsCloseHook): may change the error of a Rows close.
extern std::function<void(Rows&, error&)> rowsCloseHook;

// What Tx and Conn give a Stmt (stmtConnGrabber).
struct stmtConnGrabber {
    virtual ~stmtConnGrabber() = default;
    virtual std::tuple<ptr<driverConn>, releaseFn, error> grabConn(ptr<context::Context> ctx) = 0;
    virtual ptr<context::Context> txCtx() = 0;
};

struct connStmt {
    ptr<driverConn> dc;
    ptr<driverStmt> ds;
};

enum connReuseStrategy { alwaysNewConn, cachedOrNewConn };

}  // namespace detail

// sql.Result
struct Result : public virtual object {
    virtual std::pair<int64, error> LastInsertId() = 0;
    virtual std::pair<int64, error> RowsAffected() = 0;
};

// sql.DBStats
struct DBStats {
    int MaxOpenConnections = 0;
    int OpenConnections = 0;  // in use and idle
    int InUse = 0;
    int Idle = 0;
    int64 WaitCount = 0;
    time::Duration WaitDuration;
    int64 MaxIdleClosed = 0;
    int64 MaxIdleTimeClosed = 0;
    int64 MaxLifetimeClosed = 0;
};

// sql.ColumnType
class ColumnType final : public object {
public:
    string Name() const { return name_; }
    std::pair<int64, bool> Length() const { return {length_, hasLength_}; }
    std::tuple<int64, int64, bool> DecimalSize() const { return {precision_, scale_, hasPrecisionScale_}; }
    // The Go name of the type to scan into ("interface {}" if unknown).
    string ScanType() const { return scanType_; }
    std::pair<bool, bool> Nullable() const { return {nullable_, hasNullable_}; }
    string DatabaseTypeName() const { return databaseType_; }

private:
    friend class Rows;
    string name_;
    bool hasNullable_ = false, hasLength_ = false, hasPrecisionScale_ = false;
    bool nullable_ = false;
    int64 length_ = 0;
    string databaseType_;
    int64 precision_ = 0, scale_ = 0;
    string scanType_;
};

// sql.DB: a pool of connections, safe to use from many fibers.
class DB final : public object {
public:
    explicit DB(ptr<driver::Connector> c);
    ~DB() override;
    DB(const DB&) = delete;
    DB& operator=(const DB&) = delete;

    error PingContext(ptr<context::Context> ctx);
    error Ping();
    // Closes the idle connections and stops new queries; connections in
    // use close when they are released.
    error Close();

    void SetMaxIdleConns(int n);
    void SetMaxOpenConns(int n);
    void SetConnMaxLifetime(time::Duration d);
    void SetConnMaxIdleTime(time::Duration d);
    DBStats Stats();

    std::pair<ptr<sql::Stmt>, error> PrepareContext(ptr<context::Context> ctx, std::string_view query);
    std::pair<ptr<sql::Stmt>, error> Prepare(std::string_view query);

    template <typename... A>
    std::pair<ptr<Result>, error> ExecContext(ptr<context::Context> ctx, std::string_view query, const A&... args) {
        return execArgs(std::move(ctx), query, detail::args(args...));
    }
    template <typename... A>
    std::pair<ptr<Result>, error> Exec(std::string_view query, const A&... args) {
        return execArgs(context::Background(), query, detail::args(args...));
    }
    template <typename... A>
    std::pair<ptr<Rows>, error> QueryContext(ptr<context::Context> ctx, std::string_view query, const A&... args) {
        return queryArgs(std::move(ctx), query, detail::args(args...));
    }
    template <typename... A>
    std::pair<ptr<Rows>, error> Query(std::string_view query, const A&... args) {
        return queryArgs(context::Background(), query, detail::args(args...));
    }
    template <typename... A>
    ptr<Row> QueryRowContext(ptr<context::Context> ctx, std::string_view query, const A&... args) {
        return queryRowArgs(std::move(ctx), query, detail::args(args...));
    }
    template <typename... A>
    ptr<Row> QueryRow(std::string_view query, const A&... args) {
        return queryRowArgs(context::Background(), query, detail::args(args...));
    }

    // opts may be nil for the defaults.
    std::pair<ptr<sql::Tx>, error> BeginTx(ptr<context::Context> ctx, const TxOptions* opts);
    std::pair<ptr<sql::Tx>, error> Begin();
    ptr<driver::Driver> Driver();
    // A single connection, for session state; Close returns it to the pool.
    std::pair<ptr<sql::Conn>, error> Conn(ptr<context::Context> ctx);

    // The arguments as converted (template-free entry points).
    std::pair<ptr<Result>, error> execArgs(ptr<context::Context> ctx, std::string_view query, std::vector<detail::arg> args);
    std::pair<ptr<Rows>, error> queryArgs(ptr<context::Context> ctx, std::string_view query, std::vector<detail::arg> args);
    ptr<Row> queryRowArgs(ptr<context::Context> ctx, std::string_view query, std::vector<detail::arg> args);

private:
    friend struct sql::testAccess;
    friend class sql::Conn;
    friend class Rows;
    friend class sql::Stmt;
    friend class sql::Tx;
    friend struct detail::driverConn;
    friend ptr<DB> OpenDB(ptr<driver::Connector> c);

    int maxIdleConnsLocked() const;
    time::Duration shortestIdleTimeLocked() const;
    void startCleanerLocked();
    void connectionCleaner(time::Duration d);
    std::pair<time::Duration, std::vector<ptr<detail::driverConn>>> connectionCleanerRunLocked(time::Duration d);
    void maybeOpenNewConnections();
    void connectionOpener(ptr<context::Context> ctx);
    void openNewConnection(ptr<context::Context> ctx);
    std::pair<ptr<detail::driverConn>, error> conn(ptr<context::Context> ctx, detail::connReuseStrategy strategy);
    void noteUnusedDriverStatement(const ptr<detail::driverConn>& c, const ptr<detail::driverStmt>& ds);
    void putConn(ptr<detail::driverConn> dc, error err, bool resetSession);
    bool putConnDbLocked(const ptr<detail::driverConn>& dc, error err);
    error retry(const std::function<error(detail::connReuseStrategy)>& fn);
    error pingDc(ptr<context::Context> ctx, const ptr<detail::driverConn>& dc, const detail::releaseFn& release);
    std::pair<ptr<sql::Stmt>, error> prepareDc(ptr<context::Context> ctx, const ptr<detail::driverConn>& dc,
                                                detail::releaseFn release, detail::stmtConnGrabber* cg,
                                                ptr<object> cgKeep, std::string_view query);
    std::pair<ptr<Result>, error> execDc(ptr<context::Context> ctx, const ptr<detail::driverConn>& dc,
                                          detail::releaseFn release, std::string_view query,
                                          std::span<const detail::arg> args);
    std::pair<ptr<Rows>, error> queryDc(ptr<context::Context> ctx, ptr<context::Context> txctx,
                                         const ptr<detail::driverConn>& dc, detail::releaseFn release,
                                         std::string_view query, std::span<const detail::arg> args);
    std::pair<ptr<sql::Tx>, error> beginDc(ptr<context::Context> ctx, const ptr<detail::driverConn>& dc,
                                            detail::releaseFn release, const TxOptions* opts);
    void addDep(detail::finalCloser* x, ptr<object> keep, const void* dep);
    void addDepLocked(detail::finalCloser* x, ptr<object> keep, const void* dep);
    error removeDep(detail::finalCloser* x, const void* dep);
    std::function<error()> removeDepLocked(detail::finalCloser* x, const void* dep);

    std::atomic<int64> waitDuration_{0};
    ptr<driver::Connector> connector_;
    std::atomic<uint64> numClosed_{0};

    sync::Mutex mu_;  // guards the following
    std::vector<ptr<detail::driverConn>> freeConn_;  // oldest returned first
    detail::connRequestSet connRequests_;
    int numOpen_ = 0;     // opened and being opened
    int pendingOpens_ = 0;  // for the opener (Go's openerCh backlog)
    chan<empty> openerCh_;
    bool closed_ = false;
    struct depEntry {
        ptr<object> keep;
        std::unordered_set<const void*> deps;
    };
    std::unordered_map<detail::finalCloser*, depEntry> dep_;
    int maxIdleCount_ = 0;  // 0: the default (2); negative: 0
    int maxOpen_ = 0;        // <= 0: unlimited
    time::Duration maxLifetime_;
    time::Duration maxIdleTime_;
    chan<empty> cleanerCh_;
    int64 waitCount_ = 0;
    int64 maxIdleClosed_ = 0;
    int64 maxIdleTimeClosed_ = 0;
    int64 maxLifetimeClosed_ = 0;
    context::CancelFunc stop_;
};

// sql.Open: a DB for a registered driver. It doesn't connect yet; use Ping
// to check the data source.
std::pair<ptr<DB>, error> Open(std::string_view driverName, std::string_view dataSourceName);
// sql.OpenDB
ptr<DB> OpenDB(ptr<driver::Connector> c);

// sql.Conn: one connection of the pool.
class Conn final : public object, public detail::stmtConnGrabber {
public:
    ~Conn() override;

    error PingContext(ptr<context::Context> ctx);
    template <typename... A>
    std::pair<ptr<Result>, error> ExecContext(ptr<context::Context> ctx, std::string_view query, const A&... args) {
        return execArgs(std::move(ctx), query, detail::args(args...));
    }
    template <typename... A>
    std::pair<ptr<Rows>, error> QueryContext(ptr<context::Context> ctx, std::string_view query, const A&... args) {
        return queryArgs(std::move(ctx), query, detail::args(args...));
    }
    template <typename... A>
    ptr<Row> QueryRowContext(ptr<context::Context> ctx, std::string_view query, const A&... args) {
        return queryRowArgs(std::move(ctx), query, detail::args(args...));
    }
    std::pair<ptr<sql::Stmt>, error> PrepareContext(ptr<context::Context> ctx, std::string_view query);
    // Runs f with the driver's connection; a panic in f makes the
    // connection bad.
    error Raw(const std::function<error(driver::Conn&)>& f);
    std::pair<ptr<sql::Tx>, error> BeginTx(ptr<context::Context> ctx, const TxOptions* opts);
    // Returns the connection to the pool.
    error Close();

    std::pair<ptr<Result>, error> execArgs(ptr<context::Context> ctx, std::string_view query, std::vector<detail::arg> args);
    std::pair<ptr<Rows>, error> queryArgs(ptr<context::Context> ctx, std::string_view query, std::vector<detail::arg> args);
    ptr<Row> queryRowArgs(ptr<context::Context> ctx, std::string_view query, std::vector<detail::arg> args);

    std::tuple<ptr<detail::driverConn>, detail::releaseFn, error> grabConn(ptr<context::Context> ctx) override;
    ptr<context::Context> txCtx() override { return nullptr; }

private:
    friend struct sql::testAccess;
    friend class DB;
    Conn(ptr<DB> db, ptr<detail::driverConn> dc) : db_(std::move(db)), dc_(std::move(dc)) {}
    void closemuRunlockCondReleaseConn(error err);
    error close(error err);

    ptr<DB> db_;
    detail::closingMutex closemu_;
    ptr<detail::driverConn> dc_;
    std::atomic<bool> done_{false};
};

// sql.Tx: a transaction, on one connection until Commit or Rollback.
class Tx final : public object, public detail::stmtConnGrabber {
public:
    ~Tx() override;

    error Commit();
    error Rollback();
    std::pair<ptr<sql::Stmt>, error> PrepareContext(ptr<context::Context> ctx, std::string_view query);
    std::pair<ptr<sql::Stmt>, error> Prepare(std::string_view query);
    // A transaction-specific statement from an existing one.
    ptr<sql::Stmt> StmtContext(ptr<context::Context> ctx, const ptr<sql::Stmt>& stmt);
    ptr<sql::Stmt> Stmt(const ptr<sql::Stmt>& stmt);

    template <typename... A>
    std::pair<ptr<Result>, error> ExecContext(ptr<context::Context> ctx, std::string_view query, const A&... args) {
        return execArgs(std::move(ctx), query, detail::args(args...));
    }
    template <typename... A>
    std::pair<ptr<Result>, error> Exec(std::string_view query, const A&... args) {
        return execArgs(context::Background(), query, detail::args(args...));
    }
    template <typename... A>
    std::pair<ptr<Rows>, error> QueryContext(ptr<context::Context> ctx, std::string_view query, const A&... args) {
        return queryArgs(std::move(ctx), query, detail::args(args...));
    }
    template <typename... A>
    std::pair<ptr<Rows>, error> Query(std::string_view query, const A&... args) {
        return queryArgs(context::Background(), query, detail::args(args...));
    }
    template <typename... A>
    ptr<Row> QueryRowContext(ptr<context::Context> ctx, std::string_view query, const A&... args) {
        return queryRowArgs(std::move(ctx), query, detail::args(args...));
    }
    template <typename... A>
    ptr<Row> QueryRow(std::string_view query, const A&... args) {
        return queryRowArgs(context::Background(), query, detail::args(args...));
    }

    std::pair<ptr<Result>, error> execArgs(ptr<context::Context> ctx, std::string_view query, std::vector<detail::arg> args);
    std::pair<ptr<Rows>, error> queryArgs(ptr<context::Context> ctx, std::string_view query, std::vector<detail::arg> args);
    ptr<Row> queryRowArgs(ptr<context::Context> ctx, std::string_view query, std::vector<detail::arg> args);

    std::tuple<ptr<detail::driverConn>, detail::releaseFn, error> grabConn(ptr<context::Context> ctx) override;
    ptr<context::Context> txCtx() override { return ctx_; }

private:
    friend struct sql::testAccess;
    friend class DB;
    Tx() = default;
    void awaitDone();
    bool isDone() const { return done_.load(); }
    void close(error err);
    void closePrepared();
    error rollback(bool discardConn);

    ptr<DB> db_;
    detail::closingMutex closemu_;
    ptr<detail::driverConn> dc_;
    ptr<driver::Tx> txi_;
    detail::releaseFn releaseConn_;
    std::atomic<bool> done_{false};
    bool keepConnOnRollback_ = false;
    sync::Mutex stmtsMu_;
    std::vector<ptr<sql::Stmt>> stmts_;
    context::CancelFunc cancel_;
    ptr<context::Context> ctx_;
};

// sql.Stmt: a prepared statement, usable from many fibers. It is prepared
// again on other connections as needed.
class Stmt final : public object, public detail::finalCloser {
public:
    ~Stmt() override;

    template <typename... A>
    std::pair<ptr<Result>, error> ExecContext(ptr<context::Context> ctx, const A&... args) {
        return execArgs(std::move(ctx), detail::args(args...));
    }
    template <typename... A>
    std::pair<ptr<Result>, error> Exec(const A&... args) {
        return execArgs(context::Background(), detail::args(args...));
    }
    template <typename... A>
    std::pair<ptr<Rows>, error> QueryContext(ptr<context::Context> ctx, const A&... args) {
        return queryArgs(std::move(ctx), detail::args(args...));
    }
    template <typename... A>
    std::pair<ptr<Rows>, error> Query(const A&... args) {
        return queryArgs(context::Background(), detail::args(args...));
    }
    template <typename... A>
    ptr<Row> QueryRowContext(ptr<context::Context> ctx, const A&... args) {
        return queryRowArgs(std::move(ctx), detail::args(args...));
    }
    template <typename... A>
    ptr<Row> QueryRow(const A&... args) {
        return queryRowArgs(context::Background(), detail::args(args...));
    }
    error Close();

    std::pair<ptr<Result>, error> execArgs(ptr<context::Context> ctx, std::vector<detail::arg> args);
    std::pair<ptr<Rows>, error> queryArgs(ptr<context::Context> ctx, std::vector<detail::arg> args);
    ptr<Row> queryRowArgs(ptr<context::Context> ctx, std::vector<detail::arg> args);

private:
    friend struct sql::testAccess;
    friend class DB;
    friend class Tx;
    friend class Rows;
    Stmt() = default;
    error finalClose() override;
    void removeClosedStmtLocked();
    std::tuple<ptr<detail::driverConn>, detail::releaseFn, ptr<detail::driverStmt>, error> connStmt(
        ptr<context::Context> ctx, detail::connReuseStrategy strategy);
    std::pair<ptr<detail::driverStmt>, error> prepareOnConnLocked(ptr<context::Context> ctx,
                                                                      const ptr<detail::driverConn>& dc);

    ptr<DB> db_;
    string query_;
    error stickyErr_;
    detail::closingMutex closemu_;
    detail::stmtConnGrabber* cg_ = nullptr;  // a Tx or Conn
    ptr<object> cgKeep_;                      // keeps cg_ alive
    ptr<detail::driverStmt> cgds_;
    ptr<sql::Stmt> parentStmt_;
    sync::Mutex mu_;  // guards the following
    bool closed_ = false;
    std::vector<detail::connStmt> css_;
    uint64 lastNumClosed_ = 0;
};

// sql.Rows: the result of a query. Next before every row, even the first.
class Rows final : public object {
public:
    ~Rows() override;

    bool Next();
    bool NextResultSet();
    error Err();
    std::pair<std::vector<string>, error> Columns();
    std::pair<std::vector<ptr<ColumnType>>, error> ColumnTypes();
    error Close();

    template <typename... T>
    error Scan(T*... dest) {
        std::array<detail::dest, sizeof...(T)> ds = {detail::makeDest(dest)...};
        return scan(ds);
    }
    error scan(std::span<detail::dest> dest);

    // For RawBytes destinations (rawbuf and setrawbuf in Go).
    slice<byte> rawbuf() const { return raw_; }
    RawBytes setrawbuf(slice<byte> b);

private:
    friend struct sql::testAccess;
    friend class DB;
    friend class Stmt;
    friend class Row;
    Rows() = default;
    error lasterrOrErrLocked(error err);
    void initContextClose(ptr<context::Context> ctx, ptr<context::Context> txctx);
    void awaitDone(ptr<context::Context> ctx, ptr<context::Context> txctx, ptr<context::Context> closectx);
    std::pair<bool, bool> nextLocked();
    error scanLocked(std::span<detail::dest> dest);
    void closemuRunlockIfHeldByScan();
    error close(error err);

    ptr<detail::driverConn> dc_;
    detail::releaseFn releaseConn_;
    ptr<driver::Rows> rowsi_;
    context::CancelFunc cancel_;
    ptr<detail::driverStmt> closeStmt_;
    sync::Mutex contextDoneMu_;
    std::atomic<bool> hasContextDone_{false};
    error contextDone_;
    detail::closingMutex closemu_;
    error lasterr_;
    bool closed_ = false;
    bool closemuScanHold_ = false;
    bool hitEof_ = false;
    bool nextCalled_ = false;
    std::vector<driver::Value> lastcols_;
    std::size_t numCols_ = 0;
    slice<byte> raw_;
};

// sql.Row: the result of QueryRow.
class Row final : public object {
public:
    // Scans the first row into dest and discards the rest; ErrNoRows if
    // there is none.
    template <typename... T>
    error Scan(T*... dest) {
        std::array<detail::dest, sizeof...(T)> ds = {detail::makeDest(dest)...};
        return scan(ds);
    }
    error scan(std::span<detail::dest> dest);
    error Err() const { return err_; }

    Row(ptr<Rows> rows, error err) : err_(std::move(err)), rows_(std::move(rows)) {}

private:
    error err_;
    ptr<Rows> rows_;
};

}  // namespace gopp::sql
