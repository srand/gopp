#include "internal.hpp"

#include <gopp/io.hpp>
#include <gopp/runtime.hpp>

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <limits>
#include <optional>

namespace gopp::libpq::internal {
namespace {
error makeError(std::string message) { return errors::New(string("libpq: " + message)); }

std::string formatDouble(double value) {
    char buffer[128];
    auto [end, ec] = std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::general,
                                   std::numeric_limits<double>::max_digits10);
    return ec == std::errc{} ? std::string(buffer, end) : std::to_string(value);
}

error commandError(PGconn* c, std::string_view query, std::span<const driver::NamedValue> args) {
    PGresult* r = execParams(c, query, args);
    if (!r) return makeError(PQerrorMessage(c));
    error e = resultError(r);
    PQclear(r);
    return e;
}

std::string valueText(const driver::Value& v) {
    switch (v.index()) {
    case 0: return {};
    case 1: return std::to_string(std::get<int64>(v));
    case 2: return formatDouble(std::get<double>(v));
    case 3: return std::get<bool>(v) ? "true" : "false";
    case 4: {
        const auto& b = std::get<slice<byte>>(v);
        static constexpr char hex[] = "0123456789abcdef";
        std::string out = "\\x";
        out.reserve(4 + b.size() * 2);
        for (byte ch : b) { out.push_back(hex[ch >> 4]); out.push_back(hex[ch & 15]); }
        return out;
    }
    case 5: return std::string(std::get<string>(v).view());
    case 6: return std::string(std::get<time::Time>(v).Format("2006-01-02 15:04:05.999999999-07:00").view());
    }
    return {};
}

bool isNullValue(const driver::Value& v) {
    return v.index() == 0 || (v.index() == 4 && std::get<slice<byte>>(v).isNil());
}

template <typename T>
T parseNumber(const char* s, std::size_t n) {
    T v{};
    auto [end, ec] = std::from_chars(s, s + n, v);
    return ec == std::errc{} && end == s + n ? v : T{};
}

std::optional<time::Time> parseTime(Oid oid, std::string_view text) {
    std::string_view layouts[] = {
        oid == 1082 ? "2006-01-02" : "",
        "2006-01-02 15:04:05.999999999-07:00",
        "2006-01-02 15:04:05.999999999-07",
        "2006-01-02 15:04:05-07:00",
        "2006-01-02 15:04:05-07",
        "2006-01-02 15:04:05.999999999",
        "2006-01-02 15:04:05",
    };
    for (auto layout : layouts) {
        if (layout.empty()) continue;
        auto [t, err] = time::Parse(layout, text);
        if (!err) return t;
    }
    return std::nullopt;
}
}  // namespace

error resultError(PGresult* r) {
    if (!r) return errors::New("libpq: no result");
    ExecStatusType status = PQresultStatus(r);
    if (status == PGRES_COMMAND_OK || status == PGRES_TUPLES_OK || status == PGRES_EMPTY_QUERY) return nullptr;
    const char* message = PQresultErrorMessage(r);
    const char* state = PQresultErrorField(r, PG_DIAG_SQLSTATE);
    return make_ptr<Error_>(string(message ? message : "PostgreSQL error"), string(state ? state : ""));
}

std::vector<std::string> parameterValues(std::span<const driver::NamedValue> args, std::vector<const char*>& values) {
    std::vector<const driver::NamedValue*> ordered;
    ordered.reserve(args.size());
    for (const auto& a : args) ordered.push_back(&a);
    std::sort(ordered.begin(), ordered.end(), [](auto* a, auto* b) { return a->Ordinal < b->Ordinal; });
    std::vector<std::string> storage;
    storage.reserve(ordered.size());
    values.reserve(ordered.size());
    for (const auto* a : ordered) storage.push_back(valueText(a->Value));
    for (std::size_t i = 0; i < ordered.size(); ++i) values.push_back(isNullValue(ordered[i]->Value) ? nullptr : storage[i].c_str());
    return storage;
}

std::vector<Oid> parameterTypes(std::span<const driver::NamedValue> args) {
    std::vector<const driver::NamedValue*> ordered;
    ordered.reserve(args.size());
    for (const auto& a : args) ordered.push_back(&a);
    std::sort(ordered.begin(), ordered.end(), [](auto* a, auto* b) { return a->Ordinal < b->Ordinal; });
    std::vector<Oid> types;
    types.reserve(ordered.size());
    for (const auto* a : ordered) {
        switch (a->Value.index()) {
        case 0: types.push_back(0); break;
        case 1: types.push_back(20); break;  // int8
        case 2: types.push_back(701); break; // float8
        case 3: types.push_back(16); break;  // bool
        case 4: types.push_back(17); break;  // bytea
        case 5: types.push_back(25); break;  // text
        case 6: types.push_back(1184); break; // timestamptz
        }
    }
    return types;
}

PGresult* execParams(PGconn* c, std::string_view query, std::span<const driver::NamedValue> args) {
    std::vector<const char*> vals;
    auto storage = parameterValues(args, vals);
    auto types = parameterTypes(args);
    (void)storage;
    std::string q(query);
    return runtime::blocking([&] { return PQexecParams(c, q.c_str(), static_cast<int>(vals.size()), types.data(),
                                                          vals.data(), nullptr, nullptr, 0); });
}

std::vector<driver::NamedValue> positional(std::span<const driver::Value> args) {
    std::vector<driver::NamedValue> out;
    out.reserve(args.size());
    for (std::size_t i = 0; i < args.size(); ++i) out.push_back({{}, static_cast<int>(i + 1), args[i]});
    return out;
}

conn::~conn() { Close(); }
error conn::Close() {
    PGconn* c = c_; c_ = nullptr;
    if (c) runtime::blocking([&] { PQfinish(c); });
    return nullptr;
}
std::pair<ptr<driver::Stmt>, error> conn::Prepare(std::string_view q) { return PrepareContext(context::Background(), q); }
std::pair<ptr<driver::Stmt>, error> conn::PrepareContext(ptr<context::Context>, std::string_view q) {
    if (!c_) return {nullptr, makeError("connection is closed")};
    return {make_ptr<stmt>(ptr<conn>(this), std::string(q)), nullptr};
}
std::pair<ptr<driver::Tx>, error> conn::Begin() { return BeginTx(context::Background(), {}); }
std::pair<ptr<driver::Tx>, error> conn::BeginTx(ptr<context::Context>, driver::TxOptions opts) {
    if (opts.Isolation != 0) return {nullptr, makeError("isolation levels are not supported")};
    if (!c_) return {nullptr, makeError("connection is closed")};
    if (error e = commandError(c_, opts.ReadOnly ? "BEGIN READ ONLY" : "BEGIN", {})) return {nullptr, e};
    return {make_ptr<tx>(ptr<conn>(this)), nullptr};
}
std::pair<ptr<driver::Result>, error> conn::ExecContext(ptr<context::Context>, std::string_view q,
                                                        std::span<const driver::NamedValue> args) {
    if (!c_) return {nullptr, makeError("connection is closed")};
    PGresult* r = execParams(c_, q, args);
    if (!r) return {nullptr, makeError(PQerrorMessage(c_))};
    error e = resultError(r);
    if (e) { PQclear(r); return {nullptr, e}; }
    int64 n = parseNumber<int64>(PQcmdTuples(r), std::strlen(PQcmdTuples(r)));
    PQclear(r);
    return {make_ptr<result>(n), nullptr};
}
std::pair<ptr<driver::Rows>, error> conn::QueryContext(ptr<context::Context>, std::string_view q,
                                                        std::span<const driver::NamedValue> args) {
    if (!c_) return {nullptr, makeError("connection is closed")};
    PGresult* r = execParams(c_, q, args);
    if (!r) return {nullptr, makeError(PQerrorMessage(c_))};
    error e = resultError(r);
    if (e) { PQclear(r); return {nullptr, e}; }
    return {make_ptr<rows>(r), nullptr};
}
error conn::Ping(ptr<context::Context>) {
    if (!c_ || PQstatus(c_) != CONNECTION_OK) return makeError(c_ ? PQerrorMessage(c_) : "connection is closed");
    return commandError(c_, "SELECT 1", {});
}

std::pair<ptr<driver::Result>, error> stmt::Exec(std::span<const driver::Value> a) { return ExecContext(context::Background(), positional(a)); }
std::pair<ptr<driver::Rows>, error> stmt::Query(std::span<const driver::Value> a) { return QueryContext(context::Background(), positional(a)); }
std::pair<ptr<driver::Result>, error> stmt::ExecContext(ptr<context::Context> c, std::span<const driver::NamedValue> a) {
    if (closed_) return {nullptr, makeError("statement is closed")};
    return c_->ExecContext(std::move(c), query_, a);
}
std::pair<ptr<driver::Rows>, error> stmt::QueryContext(ptr<context::Context> c, std::span<const driver::NamedValue> a) {
    if (closed_) return {nullptr, makeError("statement is closed")};
    return c_->QueryContext(std::move(c), query_, a);
}

std::vector<string> rows::Columns() {
    std::vector<string> out;
    if (r_) for (int i = 0; i < PQnfields(r_); ++i) out.emplace_back(PQfname(r_, i));
    return out;
}
error rows::Close() { if (r_) { PQclear(r_); r_ = nullptr; } return nullptr; }
string rows::ColumnTypeDatabaseTypeName(int i) {
    if (!r_ || i < 0 || i >= PQnfields(r_)) return {};
    switch (PQftype(r_, i)) {
    case 16: return "BOOL";
    case 17: return "BYTEA";
    case 20: return "INT8";
    case 21: return "INT2";
    case 23: return "INT4";
    case 25: return "TEXT";
    case 700: return "FLOAT4";
    case 701: return "FLOAT8";
    case 1042: return "BPCHAR";
    case 1043: return "VARCHAR";
    case 1082: return "DATE";
    case 1083: return "TIME";
    case 1114: return "TIMESTAMP";
    case 1184: return "TIMESTAMPTZ";
    case 1700: return "NUMERIC";
    default: return {};
    }
}
error rows::Next(std::span<driver::Value> dest) {
    if (!r_ || row_ >= n_) return io::EOF_;
    for (std::size_t i = 0; i < dest.size() && static_cast<int>(i) < PQnfields(r_); ++i) {
        int col = static_cast<int>(i);
        if (PQgetisnull(r_, row_, col)) { dest[i] = nullptr; continue; }
        const char* s = PQgetvalue(r_, row_, col);
        std::size_t n = static_cast<std::size_t>(PQgetlength(r_, row_, col));
        switch (PQftype(r_, col)) {
        case 16: dest[i] = (n == 1 && s[0] == 't'); break;
        case 20: case 21: case 23: dest[i] = parseNumber<int64>(s, n); break;
        case 700: case 701: case 1700: dest[i] = parseNumber<double>(s, n); break;
        case 1082: case 1114: case 1184: {
            auto t = parseTime(PQftype(r_, col), std::string_view(s, n));
            dest[i] = t ? driver::Value(*t) : driver::Value(string(std::string_view(s, n)));
            break;
        }
        case 17: {
            std::size_t len = 0;
            unsigned char* b = PQunescapeBytea(reinterpret_cast<const unsigned char*>(s), &len);
            slice<byte> data = slice<byte>::make(len);
            if (len) std::memcpy(data.data(), b, len);
            PQfreemem(b); dest[i] = std::move(data); break;
        }
        default: dest[i] = string(std::string_view(s, n)); break;
        }
    }
    ++row_;
    return nullptr;
}

std::pair<int64, error> result::LastInsertId() { return {0, makeError("LastInsertId is not supported")}; }
error tx::Commit() { if (done) return makeError("transaction is already done"); done = true; return commandError(c->handle(), "COMMIT", {}); }
error tx::Rollback() { if (done) return makeError("transaction is already done"); done = true; return commandError(c->handle(), "ROLLBACK", {}); }
}  // namespace gopp::libpq::internal

namespace gopp::libpq {
using namespace internal;
namespace { error errorf(std::string m) { return errors::New(string("libpq: " + m)); } }
std::pair<ptr<sql::driver::Conn>, error> PostgreSQLDriver::Open(std::string_view dsn) {
    std::string s(dsn);
    PGconn* c = runtime::blocking([&] { return PQconnectdb(s.c_str()); });
    if (!c) return {nullptr, errorf("could not allocate connection")};
    if (PQstatus(c) != CONNECTION_OK) {
        std::string msg = PQerrorMessage(c);
        runtime::blocking([&] { PQfinish(c); });
        return {nullptr, errorf(msg)};
    }
    return {make_ptr<internal::conn>(c), nullptr};
}
bool detail::registerDriver() noexcept {
    static bool done = [] { sql::Register("postgres", make_ptr<PostgreSQLDriver>()); return true; }();
    return done;
}
}  // namespace gopp::libpq
