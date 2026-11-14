// Statements and rows, after mattn/go-sqlite3's sqlite3.go and
// sqlite3_type.go.
#include "internal.hpp"

#include <gopp/fmt.hpp>
#include <gopp/io.hpp>
#include <gopp/runtime.hpp>

#include <cctype>

namespace gopp::sqlite3::internal {

namespace {

constexpr const char* columnDate = "date";
constexpr const char* columnDatetime = "datetime";
constexpr const char* columnTimestamp = "timestamp";

const char placeHolder[1] = {0};

int bindText(sqlite3_stmt* s, int n, std::string_view v) {
    if (v.empty()) return sqlite3_bind_text64(s, n, placeHolder, 0, SQLITE_TRANSIENT, SQLITE_UTF8);
    return sqlite3_bind_text64(s, n, v.data(), v.size(), SQLITE_TRANSIENT, SQLITE_UTF8);
}

int bindValue(sqlite3_stmt* s, int n, const driver::Value& value) {
    switch (value.index()) {
    case 0: return sqlite3_bind_null(s, n);                                                    // nil
    case 1: return sqlite3_bind_int64(s, n, std::get<int64>(value));                           // int64
    case 2: return sqlite3_bind_double(s, n, std::get<double>(value));                         // float64
    case 3: return sqlite3_bind_int(s, n, std::get<bool>(value) ? 1 : 0);                      // bool
    case 4: {                                                                                  // []byte
        const slice<byte>& v = std::get<slice<byte>>(value);
        if (v.is_nil()) return sqlite3_bind_null(s, n);
        if (v.empty()) return sqlite3_bind_blob64(s, n, placeHolder, 0, SQLITE_TRANSIENT);
        return sqlite3_bind_blob64(s, n, v.data(), v.size(), SQLITE_TRANSIENT);
    }
    case 5: return bindText(s, n, std::get<string>(value).view());                             // string
    case 6: {                                                                                  // time.Time
        string b = std::get<time::Time>(value).Format(SQLiteTimestampFormats[0]);
        return bindText(s, n, b.view());
    }
    }
    return SQLITE_MISUSE;
}

std::string lower(std::string_view s) {
    std::string r(s);
    for (char& c : r) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return r;
}

const error errStmtConnClosed = errors::New("sqlite statement with already closed database connection");

}  // namespace

int blocking_step(sqlite3_stmt* s) {
    return runtime::blocking([&] { return sqlite3_step(s); });
}

std::vector<driver::NamedValue> valueToNamedValue(std::span<const driver::Value> args) {
    std::vector<driver::NamedValue> list(args.size());
    for (std::size_t i = 0; i < args.size(); i++) {
        list[i].Ordinal = static_cast<int>(i + 1);
        list[i].Value = args[i];
    }
    return list;
}

std::vector<driver::NamedValue> stmtArgs(std::span<const driver::NamedValue> args, std::size_t start, std::size_t na) {
    if (na == 0) return {};
    std::size_t end = start + na;
    // Named arguments are matched by name, not position: a statement in a
    // multi-statement query must see them even outside its positional
    // window.
    bool hasNamedOutside = false;
    for (std::size_t i = 0; i < args.size(); i++) {
        if (!args[i].Name.empty() && (i < start || i >= end)) {
            hasNamedOutside = true;
            break;
        }
    }
    if (start == 0 && !hasNamedOutside) return {args.begin() + start, args.begin() + end};
    std::vector<driver::NamedValue> out(args.begin() + start, args.begin() + end);
    for (std::size_t i = 0; i < args.size(); i++) {
        if (!args[i].Name.empty() && (i < start || i >= end)) out.push_back(args[i]);
    }
    for (std::size_t i = 0; i < out.size(); i++) out[i].Ordinal = static_cast<int>(i + 1);
    return out;
}

// ---------------------------------------------------------------------------
// Statements

stmt::stmt(ptr<conn> c, sqlite3_stmt* s, std::string tail) : c_(std::move(c)), s_(s), tail_(std::move(tail)) {
    if (s_) num_input_ = sqlite3_bind_parameter_count(s_);
}

stmt::~stmt() { Close(); }

error stmt::Close() {
    if (closed_) return nullptr;
    closed_ = true;
    sqlite3_stmt* s = s_;
    if (!s) return nullptr;
    s_ = nullptr;
    // After the connection's sqlite3_close_v2 the statement still has to be
    // finalized, which then closes the database.
    bool open = c_->db() != nullptr;
    int rv = sqlite3_finalize(s);
    if (!open) return errStmtConnClosed;
    if (rv != SQLITE_OK) return lastError(c_->db());
    return nullptr;
}

std::array<int, 3> stmt::bindNamedIndices(const std::string& name) {
    if (auto it = named_params_.find(name); it != named_params_.end()) return it->second;
    std::array<int, 3> indices{};
    const char prefixes[3] = {':', '@', '$'};
    for (int i = 0; i < 3; i++) {
        std::string pname = prefixes[i] + name;
        indices[i] = sqlite3_bind_parameter_index(s_, pname.c_str());
    }
    named_params_[name] = indices;
    return indices;
}

error stmt::bindError(const driver::Value& v) {
    if (error err = lastError(c_->db())) return err;
    return errors::New("sqlite3: unsupported bind type " + string(driver::TypeName(v)));
}

error stmt::bind(std::span<const driver::NamedValue> args) {
    int rv = sqlite3_reset(s_);
    sqlite3_clear_bindings(s_);
    if (rv != SQLITE_ROW && rv != SQLITE_OK && rv != SQLITE_DONE) return lastError(c_->db());

    bool hasNamed = false;
    for (const auto& arg : args) {
        if (!arg.Name.empty()) {
            hasNamed = true;
            break;
        }
    }
    if (!hasNamed) {
        for (const auto& arg : args) {
            if (bindValue(s_, arg.Ordinal, arg.Value) != SQLITE_OK) return bindError(arg.Value);
        }
        return nullptr;
    }
    for (const auto& arg : args) {
        if (arg.Name.empty()) {
            if (bindValue(s_, arg.Ordinal, arg.Value) != SQLITE_OK) return bindError(arg.Value);
            continue;
        }
        for (int idx : bindNamedIndices(std::string(arg.Name.view()))) {
            if (idx == 0) continue;
            if (bindValue(s_, idx, arg.Value) != SQLITE_OK) return bindError(arg.Value);
        }
    }
    return nullptr;
}

std::pair<ptr<driver::Result>, error> stmt::Exec(std::span<const driver::Value> args) {
    auto nv = valueToNamedValue(args);
    return exec(context::Background(), nv);
}

std::pair<ptr<driver::Rows>, error> stmt::Query(std::span<const driver::Value> args) {
    auto nv = valueToNamedValue(args);
    return query(context::Background(), nv);
}

std::pair<ptr<driver::Result>, error> stmt::ExecContext(ptr<context::Context> ctx, std::span<const driver::NamedValue> args) {
    return exec(ctx, args);
}

std::pair<ptr<driver::Rows>, error> stmt::QueryContext(ptr<context::Context> ctx, std::span<const driver::NamedValue> args) {
    return query(ctx, args);
}

std::pair<ptr<driver::Result>, error> stmt::exec(const ptr<context::Context>& ctx, std::span<const driver::NamedValue> args) {
    if (closed_ || !c_->db()) return {nullptr, errStmtConnClosed};
    if (error err = bind(args)) {
        sqlite3_reset(s_);
        sqlite3_clear_bindings(s_);
        return {nullptr, err};
    }
    canceler cancel(c_, ctx);
    if (error err = cancel.ctx_err()) return {nullptr, err};
    cancel.begin();
    int rv = blocking_step(s_);
    cancel.end();
    ::sqlite3* db = c_->db();
    int64 rowid = sqlite3_last_insert_rowid(db);
    int64 changes = sqlite3_changes64(db);
    if (rv != SQLITE_ROW && rv != SQLITE_OK && rv != SQLITE_DONE) {
        error err = lastError(db);
        sqlite3_reset(s_);
        sqlite3_clear_bindings(s_);
        // An interrupt for a canceled context reports the context's error.
        if (error cerr = cancel.ctx_err()) {
            if (auto* e = errors::As<Error_>(err); e && e->Code == ErrInterrupt) return {nullptr, cerr};
        }
        return {nullptr, err};
    }
    return {make_ptr<result>(rowid, changes), nullptr};
}

std::pair<ptr<driver::Rows>, error> stmt::query(const ptr<context::Context>& ctx, std::span<const driver::NamedValue> args) {
    if (closed_ || !c_->db()) return {nullptr, errStmtConnClosed};
    auto r = make_ptr<rows>(ptr<stmt>(this), cls, ctx);
    if (error err = r->first_step(args)) return {nullptr, err};
    return {r, nullptr};
}

// ---------------------------------------------------------------------------
// Rows

rows::rows(ptr<stmt> s, bool cls, ptr<context::Context> ctx) : s_(std::move(s)), cls_(cls), ctx_(std::move(ctx)) {}

rows::~rows() { Close(); }

error rows::first_step(std::span<const driver::NamedValue> args) {
    sqlite3_stmt* h = s_->handle();
    if (error err = s_->bind(args)) return err;
    cancel_ = std::make_unique<canceler>(s_->connection(), ctx_);
    if (error err = cancel_->ctx_err()) {
        cancel_.reset();
        return err;
    }
    int rv = step();
    if (error err = cancel_->ctx_err()) {
        cancel_.reset();
        sqlite3_reset(h);
        sqlite3_clear_bindings(h);
        return err;
    }
    if (rv != SQLITE_ROW && rv != SQLITE_DONE) {
        cancel_.reset();
        error err = lastError(s_->connection()->db());
        sqlite3_reset(h);
        sqlite3_clear_bindings(h);
        return err;
    }
    nc_ = sqlite3_column_count(h);
    pending_ = rv;
    return nullptr;
}

int rows::step() {
    cancel_->begin();
    int rv = blocking_step(s_->handle());
    cancel_->end();
    return rv;
}

std::vector<string> rows::Columns() {
    std::vector<string> cols;
    if (!s_ || s_->closed()) return cols;
    for (int i = 0; i < nc_; i++) cols.emplace_back(sqlite3_column_name(s_->handle(), i));
    return cols;
}

const std::vector<std::string>& rows::declTypes() {
    if (!have_decltype_ && s_ && s_->handle()) {
        decltype_.resize(nc_);
        for (int i = 0; i < nc_; i++) {
            const char* d = sqlite3_column_decltype(s_->handle(), i);
            decltype_[i] = lower(d ? d : "");
        }
        have_decltype_ = true;
    }
    return decltype_;
}

error rows::Close() {
    cancel_.reset();
    ptr<stmt> s = std::move(s_);
    if (!s || s->closed()) return nullptr;
    if (cls_) return s->Close();
    if (sqlite3_reset(s->handle()) != SQLITE_OK) return lastError(s->connection()->db());
    return nullptr;
}

error rows::Next(std::span<driver::Value> dest) {
    if (!s_ || s_->closed()) return io::EOF_;
    if (pending_ >= 0) {
        int rv = pending_;
        pending_ = -1;
        return readStepResult(dest, rv);
    }
    if (error err = cancel_->ctx_err()) return err;
    int rv = step();
    error err = readStepResult(dest, rv);
    if (error cerr = cancel_->ctx_err()) return cerr;
    return err;
}

error rows::readStepResult(std::span<driver::Value> dest, int rv) {
    if (rv == SQLITE_DONE) return io::EOF_;
    sqlite3_stmt* h = s_->handle();
    if (rv != SQLITE_ROW) {
        rv = sqlite3_reset(h);
        if (rv != SQLITE_OK) return lastError(s_->connection()->db());
        return nullptr;
    }
    const auto& decltypes = declTypes();
    const time::Location* loc = s_->connection()->loc();
    for (std::size_t i = 0; i < dest.size() && static_cast<int>(i) < nc_; i++) {
        int col = static_cast<int>(i);
        switch (sqlite3_column_type(h, col)) {
        case SQLITE_INTEGER: {
            int64 val = sqlite3_column_int64(h, col);
            const std::string& dt = decltypes[i];
            if (dt == columnTimestamp || dt == columnDatetime || dt == columnDate) {
                time::Time t;
                if (val > 1000000000000 || val < -1000000000000) {
                    val *= time::Millisecond.ns;  // convert ms to nsec
                    t = time::Unix(0, val);
                } else {
                    t = time::Unix(val, 0);
                }
                t = t.UTC();
                if (loc) t = t.In(loc);
                dest[i] = t;
            } else if (dt == "boolean") {
                dest[i] = val > 0;
            } else {
                dest[i] = val;
            }
            break;
        }
        case SQLITE_FLOAT:
            dest[i] = sqlite3_column_double(h, col);
            break;
        case SQLITE_BLOB: {
            const void* p = sqlite3_column_blob(h, col);
            int n = sqlite3_column_bytes(h, col);
            slice<byte> b = slice<byte>::make(0, static_cast<std::size_t>(n));  // not nil: []byte{} for an empty blob
            if (p && n > 0) {
                b = append_span(std::move(b), std::span<const byte>(static_cast<const byte*>(p), static_cast<std::size_t>(n)));
            }
            dest[i] = std::move(b);
            break;
        }
        case SQLITE_NULL:
            dest[i] = nullptr;
            break;
        case SQLITE_TEXT: {
            const char* p = reinterpret_cast<const char*>(sqlite3_column_text(h, col));
            int n = sqlite3_column_bytes(h, col);
            std::string_view s(p ? p : "", static_cast<std::size_t>(n));
            const std::string& dt = decltypes[i];
            if (dt == columnTimestamp || dt == columnDatetime || dt == columnDate) {
                time::Time t;
                if (s.ends_with('Z')) s.remove_suffix(1);
                bool ok = false;
                for (std::string_view format : SQLiteTimestampFormats) {
                    auto [tv, err] = time::ParseInLocation(format, s, time::UTC);
                    if (!err) {
                        t = tv;
                        ok = true;
                        break;
                    }
                }
                if (!ok) t = time::Time{};
                if (loc) t = t.In(loc);
                dest[i] = t;
            } else {
                dest[i] = string(s);
            }
            break;
        }
        }
    }
    return nullptr;
}

string rows::ColumnTypeDatabaseTypeName(int i) {
    const char* d = s_ ? sqlite3_column_decltype(s_->handle(), i) : nullptr;
    return string(d ? d : "");
}

std::pair<bool, bool> rows::ColumnTypeNullable(int) { return {true, true}; }

string rows::ColumnTypeScanType(int i) {
    // mattn's scanType and databaseTypeConvSqlite.
    const char* d = s_ ? sqlite3_column_decltype(s_->handle(), i) : nullptr;
    std::string t(d ? d : "");
    for (char& c : t) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    auto contains = [&](std::string_view sub) { return t.find(sub) != std::string::npos; };
    if (contains("INT")) return string::from_static("sql.NullInt64");
    if (t == "CLOB" || t == "TEXT" || contains("CHAR")) return string::from_static("sql.NullString");
    if (t == "BLOB") return string::from_static("sql.RawBytes");
    if (t == "REAL" || t == "FLOAT" || contains("DOUBLE")) return string::from_static("sql.NullFloat64");
    if (t == "DATE" || t == "DATETIME" || t == "TIMESTAMP") return string::from_static("sql.NullTime");
    if (t == "NUMERIC" || contains("DECIMAL")) return string::from_static("sql.NullFloat64");
    if (t == "BOOLEAN") return string::from_static("sql.NullBool");
    return string::from_static("*interface {}");
}

}  // namespace gopp::sqlite3::internal
