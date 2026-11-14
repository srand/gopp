// Ported from Go 1.27.3's database/sql tests: fakedb_test.go (the fake
// driver), sql_test.go and convert_test.go, with Go's test names.
//
// Left out: tests of features C++ types make moot or go++ doesn't have
// (UUIDs, decimals, sql.Out, driver cursors in columns, Go's reflection on
// user-defined types, panics: go++'s panic aborts), the ScanColumn driver
// variant, and tests of Go internals (synctest timing, allocations).
#include "database/sql/internal.hpp"

#include <gopp/chan.hpp>
#include <gopp/database/sql.hpp>
#include <gopp/io.hpp>
#include <gopp/runtime.hpp>
#include <gopp/strings.hpp>
#include <gopp/sync.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <map>
#include <set>

using namespace gopp;
using sql::driver::Value;

namespace gopp::sql {

// Go's in-package tests read DB internals; these do it through here.
struct testAccess {
    static std::vector<ptr<detail::driverConn>> freeConns(DB& db) {
        std::lock_guard l(db.mu_);
        return db.freeConn_;
    }
    static int numOpen(DB& db) {
        std::lock_guard l(db.mu_);
        return db.numOpen_;
    }
    static std::size_t numDeps(DB& db) {
        std::lock_guard l(db.mu_);
        return db.dep_.size();
    }
    static ptr<driver::Connector> connector(DB& db) { return db.connector_; }
    static ptr<driver::Conn> ci(Conn& c) { return c.dc_ ? c.dc_->ci : nullptr; }
    static bool connReleased(Conn& c) { return !c.dc_; }
    static bool stmtClosed(Stmt& s) {
        std::lock_guard l(s.mu_);
        return s.closed_;
    }
    static std::size_t cssSize(Stmt& s) {
        std::lock_guard l(s.mu_);
        return s.css_.size();
    }
    static void clearCss(Stmt& s) {
        std::lock_guard l(s.mu_);
        s.css_.clear();
    }
    static ptr<driver::Tx> txi(Tx& tx) { return tx.txi_; }
    static bool rowsClosed(Rows& r) {
        r.closemu_.RLock();
        bool c = r.closed_;
        r.closemu_.RUnlock();
        return c;
    }
};

}  // namespace gopp::sql

using sql::testAccess;

namespace {

namespace driver = sql::driver;

// ---------------------------------------------------------------------------
// fakedb_test.go: a fake driver whose "SQL" is pipe-separated commands:
//
//   WIPE
//   CREATE|<tablename>|<col>=<type>,<col>=<type>,...
//   INSERT|<tablename>|col=val,col2=val2,col3=?
//   SELECT|<tablename>|projectcol1,projectcol2|filtercol=?,filtercol2=?
//   SELECT|<tablename>|projectcol1,projectcol2|filtercol=?param1,filtercol2=?param2
//
// Any of these may be preceded by WAIT|<duration>|, to sleep first.

struct fakeError final : errorInterface {
    string Message;
    gopp::error Wrapped;
    fakeError(string m, gopp::error w) : Message(std::move(m)), Wrapped(std::move(w)) {}
    string Error() const override { return Message; }
    const errorInterface* Unwrap() const noexcept override { return Wrapped.get(); }
};

gopp::error fakeErr(std::string_view msg, gopp::error wrapped) { return make_ptr<fakeError>(string(msg), std::move(wrapped)); }

gopp::error errf(std::string msg) { return errors::New(string("fakedb: " + msg)); }

std::vector<std::string> split(std::string_view s, char sep) {
    std::vector<std::string> out;
    std::size_t start = 0;
    for (;;) {
        auto i = s.find(sep, start);
        out.emplace_back(s.substr(start, i == s.npos ? s.npos : i - start));
        if (i == s.npos) return out;
        start = i + 1;
    }
}

struct table final : object {
    sync::Mutex mu;
    std::vector<std::string> colname;
    std::vector<std::string> coltype;
    std::vector<std::vector<Value>> rows;
    int columnIndex(std::string_view name) const {
        for (std::size_t i = 0; i < colname.size(); i++) {
            if (colname[i] == name) return static_cast<int>(i);
        }
        return -1;
    }
};

struct fakeDb final : object {
    std::string name;
    sync::Mutex mu;
    std::map<std::string, ptr<table>> tables;
    bool badConn = false;

    void wipe() {
        std::lock_guard l(mu);
        tables.clear();
    }
    gopp::error createTable(const std::string& name, std::vector<std::string> names, std::vector<std::string> types) {
        std::lock_guard l(mu);
        if (tables.contains(name)) return errf("table " + sql::detail::quote(name).str() + " already exists");
        auto t = make_ptr<table>();
        t->colname = std::move(names);
        t->coltype = std::move(types);
        tables[name] = t;
        return nullptr;
    }
    ptr<table> getTable(const std::string& name) {
        auto it = tables.find(name);
        return it == tables.end() ? nullptr : it->second;
    }
    std::optional<std::string> columnType(const std::string& t, const std::string& column) {
        std::lock_guard l(mu);
        auto tb = getTable(t);
        if (!tb) return std::nullopt;
        int i = tb->columnIndex(column);
        if (i < 0) return std::nullopt;
        return tb->coltype[static_cast<std::size_t>(i)];
    }
};

// Hooks, as in fakedb_test.go.
std::function<gopp::error()> hookOpenErr;
std::function<bool()> hookPrepareBadConn, hookExecBadConn, hookQueryBadConn, hookCommitBadConn,
    hookRollbackBadConn;
std::function<gopp::error(std::span<Value>)> rowsCursorNextHook;
std::function<void(gopp::error)> hookPostCloseConn;
bool strictClose = false;

const gopp::error errClosed = errors::New("fakedb: statement has been closed");
const gopp::error errFakeConnSessionDirty = errors::New("fakedb: session is dirty");

struct fakeTx;
struct fakeStmt;

struct fakeConn final : driver::Conn,
                         driver::ConnPrepareContext,
                         driver::ExecerContext,
                         driver::QueryerContext,
                         driver::SessionResetter,
                         driver::Validator {
    ptr<fakeDb> db;
    fakeTx* currTx = nullptr;
    sync::Mutex mu;
    int stmtsMade = 0, stmtsClosed = 0, numPrepare = 0;
    bool bad = false, stickyBad = false, skipDirtySession = false, dirtySession = false;
    std::function<void(ptr<context::Context>)> waiter;

    void incr(int& v) {
        std::lock_guard l(mu);
        v++;
    }

    bool isBad() {
        if (stickyBad) return true;
        if (bad) {
            if (!db) return false;
            // Alternate between bad and good.
            db->badConn = !db->badConn;
            return db->badConn;
        }
        return false;
    }

    bool isDirtyAndMark() {
        if (skipDirtySession) return false;
        if (currTx) {
            dirtySession = true;
            return false;
        }
        if (dirtySession) return true;
        dirtySession = true;
        return false;
    }

    std::pair<ptr<driver::Tx>, gopp::error> Begin() override;

    gopp::error ResetSession(ptr<context::Context>) override {
        dirtySession = false;
        currTx = nullptr;
        if (isBad()) return fakeErr("Reset Session: bad conn", driver::ErrBadConn);
        return nullptr;
    }

    bool IsValid() override { return !isBad(); }

    gopp::error closeImpl();
    gopp::error Close() override {
        gopp::error err = closeImpl();
        if (err && strictClose) ADD_FAILURE() << "failed to close a test fakeConn: " << err;
        if (hookPostCloseConn) hookPostCloseConn(err);
        return err;
    }

    std::pair<ptr<driver::Result>, gopp::error> ExecContext(ptr<context::Context>, std::string_view,
                                                            std::span<const driver::NamedValue>) override {
        return {nullptr, driver::ErrSkip};
    }
    std::pair<ptr<driver::Rows>, gopp::error> QueryContext(ptr<context::Context>, std::string_view,
                                                           std::span<const driver::NamedValue>) override {
        return {nullptr, driver::ErrSkip};
    }

    std::pair<ptr<driver::Stmt>, gopp::error> Prepare(std::string_view query) override {
        return PrepareContext(context::Background(), query);
    }
    std::pair<ptr<driver::Stmt>, gopp::error> PrepareContext(ptr<context::Context> ctx, std::string_view query) override;

    std::pair<ptr<fakeStmt>, gopp::error> prepareSelect(ptr<fakeStmt> stmt, std::vector<std::string> parts);
    std::pair<ptr<fakeStmt>, gopp::error> prepareCreate(ptr<fakeStmt> stmt, std::vector<std::string> parts);
    std::pair<ptr<fakeStmt>, gopp::error> prepareInsert(ptr<fakeStmt> stmt, std::vector<std::string> parts);
};

struct fakeDriver final : driver::Driver {
    sync::Mutex mu;
    int openCount = 0, closeCount = 0;
    std::map<std::string, ptr<fakeDb>> dbs;

    ptr<fakeDb> getDb(const std::string& name) {
        std::lock_guard l(mu);
        auto& db = dbs[name];
        if (!db) {
            db = make_ptr<fakeDb>();
            db->name = name;
        }
        return db;
    }

    std::pair<ptr<driver::Conn>, gopp::error> Open(std::string_view dsn) override {
        if (hookOpenErr) {
            if (gopp::error err = hookOpenErr()) return {nullptr, err};
        }
        auto parts = split(dsn, ';');
        auto db = getDb(parts[0]);
        {
            std::lock_guard l(mu);
            openCount++;
        }
        auto conn = make_ptr<fakeConn>();
        conn->db = db;
        if (parts.size() >= 2 && parts[1] == "badConn") conn->bad = true;
        return {conn, nullptr};
    }
};

ptr<fakeDriver>& fdriver() {
    static auto* d = new ptr<fakeDriver>(make_ptr<fakeDriver>());
    return *d;
}

gopp::error fakeConn::closeImpl() {
    if (currTx) return errors::New("fakedb: can't close fakeConn; in a Transaction");
    if (!db) return errors::New("fakedb: can't close fakeConn; already closed");
    if (stmtsMade > stmtsClosed) return errors::New("fakedb: can't close; dangling statement(s)");
    db = nullptr;
    std::lock_guard l(fdriver()->mu);
    fdriver()->closeCount++;
    return nullptr;
}

struct fakeTx final : driver::Tx {
    fakeConn* c;
    explicit fakeTx(fakeConn* conn) : c(conn) {}
    gopp::error Commit() override {
        c->currTx = nullptr;
        if (hookCommitBadConn && hookCommitBadConn()) return fakeErr("Commit: Hook Bad Conn", driver::ErrBadConn);
        return nullptr;
    }
    gopp::error Rollback() override {
        c->currTx = nullptr;
        if (hookRollbackBadConn && hookRollbackBadConn()) return fakeErr("Rollback: Hook Bad Conn", driver::ErrBadConn);
        return nullptr;
    }
};

std::pair<ptr<driver::Tx>, gopp::error> fakeConn::Begin() {
    if (isBad()) return {nullptr, fakeErr("", driver::ErrBadConn)};
    if (currTx) return {nullptr, errors::New("fakedb: already in a transaction")};
    auto tx = make_ptr<fakeTx>(this);
    currTx = tx.get();
    return {tx, nullptr};
}

// fakeDriverString: strings and bytes stay; others are printed.
struct fakeDriverString final : driver::ValueConverter {
    std::pair<Value, gopp::error> ConvertValue(const Value& v) const override {
        if (std::holds_alternative<string>(v) || std::holds_alternative<slice<byte>>(v)) return {v, nullptr};
        return {driver::Format(v), nullptr};
    }
};

struct anyTypeConverter final : driver::ValueConverter {
    std::pair<Value, gopp::error> ConvertValue(const Value& v) const override { return {v, nullptr}; }
};

const driver::ValueConverter& converterForType(const std::string& typ) {
    static const driver::Null nullBool(driver::Bool);
    static const driver::NotNull notNullDefault(driver::DefaultParameterConverter);
    static const driver::Null nullDefault(driver::DefaultParameterConverter);
    static const fakeDriverString fds;
    static const driver::NotNull notNullString(fds);
    static const driver::Null nullString(fds);
    static const anyTypeConverter anyConv;
    if (typ == "bool") return driver::Bool;
    if (typ == "nullbool") return nullBool;
    if (typ == "byte" || typ == "int16" || typ == "int64" || typ == "float64" || typ == "datetime") return notNullDefault;
    if (typ == "int32") return driver::Int32;
    if (typ == "nullbyte" || typ == "nullint32" || typ == "nullint16" || typ == "nullint64" || typ == "nullfloat64" ||
        typ == "nulldatetime")
        return nullDefault;
    if (typ == "string") return notNullString;
    if (typ == "nullstring") return nullString;
    if (typ == "any") return anyConv;
    panic("invalid fakedb column type of " + typ);
}

string colTypeToScanType(const std::string& typ) {
    static const std::map<std::string, std::string> m = {
        {"bool", "bool"},           {"nullbool", "sql.NullBool"}, {"int16", "int16"},
        {"nullint16", "sql.NullInt16"}, {"int32", "int32"},       {"nullint32", "sql.NullInt32"},
        {"string", "string"},       {"nullstring", "sql.NullString"}, {"int64", "int64"},
        {"nullint64", "sql.NullInt64"}, {"float64", "float64"},   {"nullfloat64", "sql.NullFloat64"},
        {"datetime", "time.Time"},  {"any", "interface {}"},
    };
    return string(m.at(typ));
}

struct boundCol {
    std::string column, placeholder;
    int ordinal;
};

struct colValue {
    bool placeholder = false;
    std::string ph;  // "?" or "?name"
    Value v;
};

struct rowsCursor final : driver::Rows, driver::RowsNextResultSet, driver::RowsColumnTypeScanType {
    std::vector<std::vector<std::string>> cols, colType;
    std::size_t posSet = 0;
    int posRow = -1;
    std::vector<std::vector<std::vector<Value>>> rows;
    bool closed = false;
    int errPos = -1;
    gopp::error err;
    std::vector<slice<byte>> driverOwnedMemory;  // invalidated after each row
    gopp::error closeErr;

    void invalidateDriverOwnedMemory() {
        for (auto& b : driverOwnedMemory) {
            for (auto& c : b) c = 'x';
        }
        driverOwnedMemory.clear();
    }

    gopp::error Close() override {
        invalidateDriverOwnedMemory();
        closed = true;
        return closeErr;
    }
    std::vector<string> Columns() override {
        std::vector<string> out;
        for (auto& c : cols[posSet]) out.emplace_back(c);
        return out;
    }
    string ColumnTypeScanType(int index) override {
        return colTypeToScanType(colType[posSet][static_cast<std::size_t>(index)]);
    }
    gopp::error Next(std::span<Value> dest) override {
        if (rowsCursorNextHook) return rowsCursorNextHook(dest);
        if (closed) return errors::New("fakedb: cursor is closed");
        posRow++;
        if (posRow == errPos) return err;
        if (posRow >= static_cast<int>(rows[posSet].size())) return io::EOF_;
        // Bytes are the driver's memory: valid until the next row.
        invalidateDriverOwnedMemory();
        auto& row = rows[posSet][static_cast<std::size_t>(posRow)];
        for (std::size_t i = 0; i < row.size(); i++) {
            Value v = row[i];
            if (auto* b = std::get_if<slice<byte>>(&v)) {
                slice<byte> c = slice<byte>::of(b->span());
                driverOwnedMemory.push_back(c);
                v = c;
            }
            dest[i] = v;
        }
        return nullptr;
    }
    bool HasNextResultSet() override { return posSet < rows.size() - 1; }
    gopp::error NextResultSet() override {
        if (HasNextResultSet()) {
            posSet++;
            posRow = -1;
            return nullptr;
        }
        return io::EOF_;
    }
};

struct fakeStmt final : driver::Stmt, driver::StmtExecContext, driver::StmtQueryContext, driver::ColumnConverter {
    ptr<fakeConn> c;
    std::string q, cmd, tbl;
    time::Duration wait;
    ptr<fakeStmt> next;  // for multiple results
    bool closed = false;
    std::vector<std::string> colName, colType;
    std::vector<colValue> colValue_;
    int placeholders = 0;
    std::vector<boundCol> whereCol;
    std::vector<const driver::ValueConverter*> placeholderConverter;

    const driver::ValueConverter& Converter(int idx) override {
        if (placeholderConverter.empty()) return driver::DefaultParameterConverter;
        return *placeholderConverter[static_cast<std::size_t>(idx)];
    }

    gopp::error Close() override {
        if (!c) panic("nil conn in fakeStmt.Close");
        if (!c->db) panic("in fakeStmt.Close, conn's db is nil (already closed)");
        if (!closed) {
            c->incr(c->stmtsClosed);
            closed = true;
        }
        if (next) next->Close();
        return nullptr;
    }

    int NumInput() override { return placeholders; }

    std::pair<ptr<driver::Result>, gopp::error> Exec(std::span<const Value>) override {
        panic("Using ExecContext");
    }
    std::pair<ptr<driver::Rows>, gopp::error> Query(std::span<const Value>) override { panic("Use QueryContext"); }

    std::pair<ptr<driver::Result>, gopp::error> ExecContext(ptr<context::Context> ctx,
                                                            std::span<const driver::NamedValue> args) override {
        if (closed) return {nullptr, errClosed};
        if (c->stickyBad || (hookExecBadConn && hookExecBadConn())) {
            return {nullptr, fakeErr("Exec: Sticky Bad", driver::ErrBadConn)};
        }
        if (c->isDirtyAndMark()) return {nullptr, errFakeConnSessionDirty};
        if (wait.ns > 0) time::Sleep(wait);
        if (gopp::error err = ctx->Err()) return {nullptr, err};
        auto db = c->db;
        if (cmd == "WIPE") {
            db->wipe();
            return {driver::ResultNoRows, nullptr};
        }
        if (cmd == "CREATE") {
            if (gopp::error err = db->createTable(tbl, colName, colType)) return {nullptr, err};
            return {driver::ResultNoRows, nullptr};
        }
        if (cmd == "INSERT") return execInsert(args, true);
        if (cmd == "NOSERT") return execInsert(args, false);
        return {nullptr, errors::New("fakedb: unimplemented statement Exec command type of " + sql::detail::quote(cmd))};
    }

    static Value valueFromPlaceholderName(std::span<const driver::NamedValue> args, std::string_view name) {
        for (auto& a : args) {
            if (a.Name == name) return a.Value;
        }
        return nullptr;
    }

    std::pair<ptr<driver::Result>, gopp::error> execInsert(std::span<const driver::NamedValue> args, bool doInsert) {
        auto db = c->db;
        if (static_cast<int>(args.size()) != placeholders) panic("error in pkg db; should only get here if size is correct");
        ptr<table> t;
        {
            std::lock_guard l(db->mu);
            t = db->getTable(tbl);
        }
        if (!t) return {nullptr, errors::New("fakedb: table " + sql::detail::quote(tbl) + " doesn't exist")};
        std::lock_guard l(t->mu);
        std::vector<Value> cols(t->colname.size(), nullptr);
        std::size_t argPos = 0;
        for (std::size_t n = 0; n < colName.size(); n++) {
            int colidx = t->columnIndex(colName[n]);
            if (colidx == -1) {
                return {nullptr, errors::New("fakedb: column " + sql::detail::quote(colName[n]) +
                                             " doesn't exist or dropped since prepared statement was created")};
            }
            Value val;
            if (colValue_[n].placeholder) {
                if (colValue_[n].ph == "?") val = args[argPos].Value;
                else val = valueFromPlaceholderName(args, std::string_view(colValue_[n].ph).substr(1));
                argPos++;
            } else {
                val = colValue_[n].v;
            }
            cols[static_cast<std::size_t>(colidx)] = val;
        }
        if (doInsert) t->rows.push_back(std::move(cols));
        return {driver::RowsAffected(1), nullptr};
    }

    std::pair<ptr<driver::Rows>, gopp::error> QueryContext(ptr<context::Context> ctx,
                                                           std::span<const driver::NamedValue> args) override {
        if (closed) return {nullptr, errClosed};
        if (c->stickyBad || (hookQueryBadConn && hookQueryBadConn())) {
            return {nullptr, fakeErr("Query: Sticky Bad", driver::ErrBadConn)};
        }
        if (c->isDirtyAndMark()) return {nullptr, errFakeConnSessionDirty};
        auto db = c->db;
        if (static_cast<int>(args.size()) != placeholders) panic("error in pkg db; should only get here if size is correct");
        auto cursor = make_ptr<rowsCursor>();
        for (fakeStmt* s = this; s; s = s->next.get()) {
            ptr<table> t;
            {
                std::lock_guard l(db->mu);
                t = db->getTable(s->tbl);
            }
            if (!t) return {nullptr, errors::New("fakedb: table " + sql::detail::quote(s->tbl) + " doesn't exist")};
            if (s->tbl == "magicquery" && s->whereCol.size() == 2 && s->whereCol[0].column == "op" &&
                s->whereCol[1].column == "millis") {
                if (driver::Format(args[0].Value) == "sleep") {
                    time::Sleep(time::Duration(std::get<int64>(args[1].Value) * time::Millisecond.ns));
                }
            }
            if (s->tbl == "tx_status" && s->colName[0] == "tx_status") {
                auto tc = make_ptr<rowsCursor>();
                tc->rows = {{{string(c->currTx ? "transaction" : "autocommit")}}};
                tc->cols = {{"tx_status"}};
                tc->colType = {{"string"}};
                return {tc, nullptr};
            }
            std::lock_guard l(t->mu);
            std::map<std::string, int> colIdx;
            for (auto& name : s->colName) {
                int idx = t->columnIndex(name);
                if (idx == -1) return {nullptr, errors::New("fakedb: unknown column name " + sql::detail::quote(name))};
                colIdx[name] = idx;
            }
            std::vector<std::vector<Value>> mrows;
            for (auto& trow : t->rows) {
                bool match = true;
                for (auto& wcol : s->whereCol) {
                    int idx = t->columnIndex(wcol.column);
                    if (idx == -1) return {nullptr, errors::New("fakedb: invalid where clause column")};
                    Value tcol = trow[static_cast<std::size_t>(idx)];
                    if (auto* b = std::get_if<slice<byte>>(&tcol)) tcol = string(asStringView(b->span()));
                    Value argValue;
                    if (wcol.placeholder == "?") argValue = args[static_cast<std::size_t>(wcol.ordinal - 1)].Value;
                    else argValue = valueFromPlaceholderName(args, std::string_view(wcol.placeholder).substr(1));
                    if (driver::Format(tcol) != driver::Format(argValue)) {
                        match = false;
                        break;
                    }
                }
                if (!match) continue;
                std::vector<Value> mrow;
                for (auto& name : s->colName) mrow.push_back(trow[static_cast<std::size_t>(colIdx[name])]);
                mrows.push_back(std::move(mrow));
            }
            std::vector<std::string> ct;
            for (auto& column : s->colName) ct.push_back(t->coltype[static_cast<std::size_t>(t->columnIndex(column))]);
            cursor->rows.push_back(std::move(mrows));
            cursor->cols.push_back(s->colName);
            cursor->colType.push_back(std::move(ct));
        }
        return {cursor, nullptr};
    }
};

std::pair<ptr<fakeStmt>, gopp::error> fakeConn::prepareSelect(ptr<fakeStmt> stmt, std::vector<std::string> parts) {
    if (parts.size() != 3) {
        stmt->Close();
        return {nullptr, errf("invalid SELECT syntax with " + std::to_string(parts.size()) + " parts; want 3")};
    }
    stmt->tbl = parts[0];
    stmt->colName = split(parts[1], ',');
    auto specs = split(parts[2], ',');
    for (std::size_t n = 0; n < specs.size(); n++) {
        if (specs[n].empty()) continue;
        auto nameVal = split(specs[n], '=');
        if (nameVal.size() != 2) {
            stmt->Close();
            return {nullptr, errf("SELECT on table " + sql::detail::quote(stmt->tbl).str() + " has invalid column spec")};
        }
        if (!db->columnType(stmt->tbl, nameVal[0])) {
            stmt->Close();
            return {nullptr, errf("SELECT on table " + sql::detail::quote(stmt->tbl).str() + " references non-existent column " +
                                  sql::detail::quote(nameVal[0]).str())};
        }
        if (!nameVal[1].starts_with("?")) {
            stmt->Close();
            return {nullptr, errf("SELECT on table has pre-bound value for where column; need a question mark")};
        }
        stmt->placeholders++;
        stmt->whereCol.push_back({nameVal[0], nameVal[1], stmt->placeholders});
    }
    return {stmt, nullptr};
}

std::pair<ptr<fakeStmt>, gopp::error> fakeConn::prepareCreate(ptr<fakeStmt> stmt, std::vector<std::string> parts) {
    if (parts.size() != 2) {
        stmt->Close();
        return {nullptr, errf("invalid CREATE syntax with " + std::to_string(parts.size()) + " parts; want 2")};
    }
    stmt->tbl = parts[0];
    for (auto& spec : split(parts[1], ',')) {
        auto nt = split(spec, '=');
        if (nt.size() != 2) {
            stmt->Close();
            return {nullptr, errf("CREATE table has invalid column spec")};
        }
        stmt->colName.push_back(nt[0]);
        stmt->colType.push_back(nt[1]);
    }
    return {stmt, nullptr};
}

std::pair<ptr<fakeStmt>, gopp::error> fakeConn::prepareInsert(ptr<fakeStmt> stmt, std::vector<std::string> parts) {
    if (parts.size() != 2) {
        stmt->Close();
        return {nullptr, errf("invalid INSERT syntax with " + std::to_string(parts.size()) + " parts; want 2")};
    }
    stmt->tbl = parts[0];
    for (auto& spec : split(parts[1], ',')) {
        auto nv = split(spec, '=');
        if (nv.size() != 2) {
            stmt->Close();
            return {nullptr, errf("INSERT table has invalid column spec")};
        }
        auto ctype = db->columnType(stmt->tbl, nv[0]);
        if (!ctype) {
            stmt->Close();
            return {nullptr, errf("INSERT table " + sql::detail::quote(stmt->tbl).str() + " references non-existent column " +
                                  sql::detail::quote(nv[0]).str())};
        }
        stmt->colName.push_back(nv[0]);
        colValue cv;
        if (!nv[1].starts_with("?")) {
            if (*ctype == "string" || *ctype == "blob") {
                cv.v = slice<byte>::of(as_bytes(nv[1]));
            } else if (*ctype == "int32") {
                auto [i, err] = strconv::Atoi(nv[1]);
                if (err) {
                    stmt->Close();
                    return {nullptr, errf("invalid conversion to int32 from " + sql::detail::quote(nv[1]).str())};
                }
                cv.v = static_cast<int64>(i);
            } else {
                stmt->Close();
                return {nullptr, errf("unsupported conversion for pre-bound parameter " + sql::detail::quote(nv[1]).str() +
                                      " to type " + sql::detail::quote(*ctype).str())};
            }
        } else {
            stmt->placeholders++;
            stmt->placeholderConverter.push_back(&converterForType(*ctype));
            cv.placeholder = true;
            cv.ph = nv[1];
        }
        stmt->colValue_.push_back(std::move(cv));
    }
    return {stmt, nullptr};
}

std::pair<ptr<driver::Stmt>, gopp::error> fakeConn::PrepareContext(ptr<context::Context> ctx, std::string_view query) {
    numPrepare++;
    if (!db) panic("nil c.db");
    if (stickyBad || (hookPrepareBadConn && hookPrepareBadConn())) {
        return {nullptr, fakeErr("Prepare: Sticky Bad", driver::ErrBadConn)};
    }
    ptr<fakeStmt> first, prev;
    for (auto& q : split(query, ';')) {
        auto parts = split(q, '|');
        auto stmt = make_ptr<fakeStmt>();
        stmt->q = q;
        stmt->c = ptr<fakeConn>(this);
        if (parts.size() >= 3 && parts[0] == "WAIT") {
            auto [d, err] = time::ParseDuration(parts[1]);
            if (err) return {nullptr, errf("expected section after WAIT to be a duration")};
            stmt->wait = d;
            parts.erase(parts.begin(), parts.begin() + 2);
        }
        stmt->cmd = parts[0];
        parts.erase(parts.begin());
        if (waiter) {
            waiter(ctx);
            if (gopp::error err = ctx->Err()) return {nullptr, err};
        }
        if (stmt->wait.ns > 0) {
            auto t = time::NewTimer(stmt->wait);
            gopp::error cerr;
            select(recv(t->C, [] {}), recv(ctx->Done(), [&] { cerr = ctx->Err(); }));
            if (cerr) {
                t->Stop();
                return {nullptr, cerr};
            }
        }
        incr(stmtsMade);
        gopp::error err;
        if (stmt->cmd == "WIPE") {
        } else if (stmt->cmd == "SELECT") {
            std::tie(stmt, err) = prepareSelect(stmt, parts);
        } else if (stmt->cmd == "CREATE") {
            std::tie(stmt, err) = prepareCreate(stmt, parts);
        } else if (stmt->cmd == "INSERT" || stmt->cmd == "NOSERT") {
            std::tie(stmt, err) = prepareInsert(stmt, parts);
        } else {
            stmt->Close();
            return {nullptr, errf("unsupported command type " + sql::detail::quote(stmt->cmd).str())};
        }
        if (err) return {nullptr, err};
        if (!first) first = stmt;
        if (prev) prev->next = stmt;
        prev = stmt;
    }
    return {first, nullptr};
}

struct fakeConnector final : driver::Connector, io::Closer {
    std::string name;
    std::function<void(ptr<context::Context>)> waiter;
    bool closed = false;
    std::pair<ptr<driver::Conn>, gopp::error> Connect(ptr<context::Context>) override {
        auto [conn, err] = fdriver()->Open(name);
        if (err) return {nullptr, err};
        dynamic_cast<fakeConn&>(*conn).waiter = waiter;
        return {conn, nullptr};
    }
    ptr<driver::Driver> Driver() override { return fdriver(); }
    gopp::error Close() override {
        if (closed) return errors::New("fakedb: connector is closed");
        closed = true;
        return nullptr;
    }
};

struct fakeDriverCtx final : driver::Driver, driver::DriverContext {
    std::pair<ptr<driver::Conn>, gopp::error> Open(std::string_view name) override { return fdriver()->Open(name); }
    std::pair<ptr<driver::Connector>, gopp::error> OpenConnector(std::string_view name) override {
        auto c = make_ptr<fakeConnector>();
        c->name = std::string(name);
        return {c, nullptr};
    }
};

// basicConn and basicStmt (sql_test.go): only the required interfaces.
struct basicStmt final : driver::Stmt {
    ptr<fakeStmt> st;
    std::pair<std::vector<driver::NamedValue>, gopp::error> valuesToNamedValues(std::span<const Value> args) {
        std::vector<driver::NamedValue> nv;
        for (std::size_t i = 0; i < args.size(); i++) {
            auto [val, err] = st->Converter(static_cast<int>(i)).ConvertValue(args[i]);
            if (err) {
                return {{}, sql::detail::wrapf("sql: converting argument $" + strconv::Itoa(static_cast<int64>(i + 1)) +
                                                   " type: " + err.Error(),
                                               err)};
            }
            nv.push_back({{}, static_cast<int>(i + 1), val});
        }
        return {nv, nullptr};
    }
    gopp::error Close() override { return st->Close(); }
    int NumInput() override { return st->NumInput(); }
    std::pair<ptr<driver::Result>, gopp::error> Exec(std::span<const Value> args) override {
        auto [nvs, err] = valuesToNamedValues(args);
        if (err) return {nullptr, err};
        return st->ExecContext(context::Background(), nvs);
    }
    std::pair<ptr<driver::Rows>, gopp::error> Query(std::span<const Value> args) override {
        auto [nvs, err] = valuesToNamedValues(args);
        if (err) return {nullptr, err};
        return st->QueryContext(context::Background(), nvs);
    }
};

struct basicConn final : driver::Conn {
    ptr<fakeConn> fc;
    std::pair<ptr<driver::Stmt>, gopp::error> Prepare(std::string_view query) override {
        auto [stmt, err] = fc->PrepareContext(context::Background(), query);
        if (err) return {nullptr, err};
        auto bs = make_ptr<basicStmt>();
        bs->st = dynamic_pointer_cast<fakeStmt>(stmt);
        return {bs, nullptr};
    }
    std::pair<ptr<driver::Tx>, gopp::error> Begin() override { return fc->Begin(); }
    gopp::error Close() override { return fc->Close(); }
};

struct basicConnector final : driver::Connector {
    std::string name;
    std::pair<ptr<driver::Conn>, gopp::error> Connect(ptr<context::Context> ctx) override {
        auto [conn, err] = fdriver()->Open(name);
        if (err) return {nullptr, err};
        auto fc = dynamic_pointer_cast<fakeConn>(conn);
        fc->skipDirtySession = true;  // the Conn won't implement ResetSession
        auto bc = make_ptr<basicConn>();
        bc->fc = fc;
        return {bc, nullptr};
    }
    ptr<driver::Driver> Driver() override { return fdriver(); }
};

fakeConn* getFakeConn(const ptr<driver::Conn>& c) {
    if (auto* fc = dynamic_cast<fakeConn*>(c.get())) return fc;
    if (auto* bc = dynamic_cast<basicConn*>(c.get())) return bc->fc.get();
    return nullptr;
}

const std::string fakeDbName = "foo";
const time::Time chrisBirthday = time::Time::Unix(123456789, 0);

[[maybe_unused]] const bool registered = [] {
    sql::Register("test", fdriver());
    return true;
}();

// ---------------------------------------------------------------------------
// sql_test.go helpers

template <typename F>
bool waitFor(F cond, time::Duration timeout = 5 * time::Second) {
    time::Time deadline = time::Now().Add(timeout);
    while (!cond()) {
        if (time::Now().After(deadline)) return false;
        time::Sleep(time::Millisecond);
    }
    return true;
}

template <typename... A>
void exec(sql::DB& db, std::string_view query, const A&... args) {
    auto [res, err] = db.Exec(query, args...);
    ASSERT_FALSE(err) << "Exec of " << query << ": " << err;
}

void closeDb(sql::DB& db) {
    // The fake driver's databases are global, as in Go, where the next test
    // wipes them; wiping here keeps each test's objects its own.
    db.Exec("WIPE");
    for (auto& dc : testAccess::freeConns(db)) {
        std::lock_guard l(dc->mu);
        EXPECT_EQ(dc->openStmt.size(), 0u) << "while closing db, a free conn had open stmts";
    }
    hookPostCloseConn = [](gopp::error err) { EXPECT_FALSE(err) << "Error closing fakeConn: " << err; };
    gopp::error err = db.Close();
    EXPECT_FALSE(err) << "error closing DB: " << err;
    // Connections close asynchronously; wait for them to finish doing so.
    EXPECT_TRUE(waitFor([&] { return testAccess::numOpen(db) == 0; }))
        << testAccess::numOpen(db) << " connections still open after closing DB";
    hookPostCloseConn = nullptr;
}

void populate(sql::DB& db, std::string_view name) {
    if (name == "people") {
        exec(db, "CREATE|people|name=string,age=int32,photo=blob,dead=bool,bdate=datetime");
        exec(db, "INSERT|people|name=Alice,age=?,photo=APHOTO", 1);
        exec(db, "INSERT|people|name=Bob,age=?,photo=BPHOTO", 2);
        exec(db, "INSERT|people|name=Chris,age=?,photo=CPHOTO,bdate=?", 3, chrisBirthday);
    } else if (name == "magicquery") {
        exec(db, "CREATE|magicquery|op=string,millis=int32");
        exec(db, "INSERT|magicquery|op=sleep,millis=10");
    } else if (name == "tx_status") {
        exec(db, "CREATE|tx_status|tx_status=string");
        exec(db, "INSERT|tx_status|tx_status=invalid");
    } else {
        FAIL() << "unknown database name " << name;
    }
}

ptr<sql::DB> newTestDb(std::string_view name = "") {
    auto fc = make_ptr<fakeConnector>();
    fc->name = fakeDbName;
    auto db = sql::OpenDB(fc);
    auto [res, err] = db->Exec("WIPE");
    EXPECT_FALSE(err) << "exec wipe: " << err;
    if (!name.empty()) populate(*db, name);
    return db;
}

int numPrepares(sql::DB& db) {
    auto fcs = testAccess::freeConns(db);
    EXPECT_EQ(fcs.size(), 1u) << "free conns";
    if (fcs.empty()) return 0;
    return getFakeConn(fcs[0]->ci)->numPrepare;
}

std::size_t numFreeConns(sql::DB& db) { return testAccess::freeConns(db).size(); }

void clearAllConns(sql::DB& db) {
    db.SetMaxIdleConns(0);
    EXPECT_EQ(numFreeConns(db), 0u);
    EXPECT_TRUE(waitFor([&] { return testAccess::numDeps(db) == 0; })) << "number of dependencies = " << testAccess::numDeps(db);
}

// testDatabase: runs f against the basic driver (no optional interfaces)
// and the default fake driver.
void testDatabase(const std::function<void(sql::DB&)>& f, bool requireDefault = false) {
    for (std::string variant : {"basic", "default"}) {
        if (requireDefault && variant == "basic") continue;
        SCOPED_TRACE(variant);
        ptr<driver::Connector> c;
        if (variant == "basic") {
            auto bc = make_ptr<basicConnector>();
            bc->name = fakeDbName;
            c = bc;
        } else {
            auto fc = make_ptr<fakeConnector>();
            fc->name = fakeDbName;
            c = fc;
        }
        auto db = sql::OpenDB(c);
        auto [res, err] = db->Exec("WIPE");
        ASSERT_FALSE(err) << "exec wipe: " << err;
        f(*db);
        closeDb(*db);
        // A test may close the DB itself: wipe through a new one.
        auto w = sql::OpenDB(c);
        w->Exec("WIPE");
        w->Close();
    }
}

struct personRow {
    int age = 0;
    string name;
    bool operator==(const personRow&) const = default;
};

}  // namespace

// ---------------------------------------------------------------------------
// sql_test.go

TEST(Sql, Drivers) {
    auto all = sql::Drivers();
    EXPECT_TRUE(std::is_sorted(all.begin(), all.end()));
    EXPECT_TRUE(std::find(all.begin(), all.end(), "test") != all.end());
}

TEST(Sql, OpenDB) {
    auto fc = make_ptr<fakeConnector>();
    fc->name = fakeDbName;
    auto db = sql::OpenDB(fc);
    EXPECT_EQ(db->Driver(), fdriver());
    db->Close();
}

TEST(Sql, Query) {
    testDatabase([](sql::DB& db) {
        populate(db, "people");
        int prepares0 = numPrepares(db);
        auto [rows, err] = db.Query("SELECT|people|age,name|");
        ASSERT_FALSE(err) << "Query: " << err;
        std::vector<personRow> got;
        while (rows->Next()) {
            personRow r;
            ASSERT_FALSE(rows->Scan(&r.age, &r.name));
            got.push_back(r);
        }
        ASSERT_FALSE(rows->Err());
        std::vector<personRow> want = {{1, "Alice"}, {2, "Bob"}, {3, "Chris"}};
        EXPECT_EQ(got, want);
        // The final Next, which hit EOF, closed the rows and freed the conn.
        EXPECT_EQ(numFreeConns(db), 1u);
        EXPECT_EQ(numPrepares(db) - prepares0, 1);
        rows->Close();
    });
}

TEST(Sql, QueryContext) {
    testDatabase([](sql::DB& db) {
        populate(db, "people");
        int prepares0 = numPrepares(db);
        auto [ctx, cancel] = context::WithCancel(context::Background());
        auto [rows, err] = db.QueryContext(ctx, "SELECT|people|age,name|");
        ASSERT_FALSE(err) << "Query: " << err;
        std::vector<personRow> got;
        int index = 0;
        while (rows->Next()) {
            if (index == 2) {
                cancel();
                ASSERT_TRUE(waitFor([&] { return testAccess::rowsClosed(*rows); }));
            }
            personRow r;
            if (gopp::error serr = rows->Scan(&r.age, &r.name)) {
                if (index == 2) break;
                FAIL() << "Scan: " << serr;
            }
            got.push_back(r);
            index++;
        }
        EXPECT_EQ(ctx->Err().Error(), "context canceled");
        std::vector<personRow> want = {{1, "Alice"}, {2, "Bob"}};
        EXPECT_EQ(got, want);
        EXPECT_TRUE(waitFor([&] { return numFreeConns(db) == 1; }));
        EXPECT_EQ(numPrepares(db) - prepares0, 1);
    });
}

TEST(Sql, TxContextWait) {
    testDatabase([](sql::DB& db) {
        populate(db, "people");
        auto [ctx, cancel] = context::WithCancel(context::Background());
        auto [tx, err] = db.BeginTx(ctx, nullptr);
        ASSERT_FALSE(err);
        go([cancel] {
            time::Sleep(15 * time::Millisecond);
            cancel();
        });
        // The prepare waits up to a second, unless the context ends first.
        auto [rows, qerr] = tx->QueryContext(ctx, "WAIT|1s|SELECT|people|age,name|");
        ASSERT_TRUE(qerr);
        EXPECT_EQ(qerr.Error(), "context canceled");
        // The transaction rolled back when its context ended.
        EXPECT_TRUE(waitFor([&] { return tx->Commit() == sql::ErrTxDone; }));
        EXPECT_TRUE(waitFor([&] { return testAccess::numOpen(db) == static_cast<int>(numFreeConns(db)); }));
    });
}

TEST(Sql, QueryRow) {
    testDatabase([](sql::DB& db) {
        populate(db, "people");
        string name;
        int age;
        time::Time birthday;
        gopp::error err = db.QueryRow("SELECT|people|age,name|age=?", 3)->Scan(&age);
        EXPECT_TRUE(err && strings::Contains(err.Error(), "expected 2 destination arguments")) << err;

        err = db.QueryRow("SELECT|people|bdate|age=?", 3)->Scan(&birthday);
        EXPECT_FALSE(err) << err;
        EXPECT_TRUE(birthday.Equal(chrisBirthday));

        ASSERT_FALSE(db.QueryRow("SELECT|people|age,name|age=?", 2)->Scan(&age, &name));
        EXPECT_EQ(name, "Bob");
        EXPECT_EQ(age, 2);

        ASSERT_FALSE(db.QueryRow("SELECT|people|age,name|name=?", "Alice")->Scan(&age, &name));
        EXPECT_EQ(name, "Alice");
        EXPECT_EQ(age, 1);

        slice<byte> photo;
        ASSERT_FALSE(db.QueryRow("SELECT|people|photo|name=?", "Alice")->Scan(&photo));
        EXPECT_EQ(asStringView(photo.span()), "APHOTO");
    });
}

TEST(Sql, RowErr) {
    testDatabase([](sql::DB& db) {
        populate(db, "people");
        auto row = db.QueryRowContext(context::Background(), "SELECT|people|bdate|age=?", 3);
        EXPECT_FALSE(row->Err());
        row->Scan();
        auto [ctx, cancel] = context::WithCancel(context::Background());
        cancel();
        gopp::error err = db.QueryRowContext(ctx, "SELECT|people|bdate|age=?", 3)->Err();
        EXPECT_TRUE(err && strings::Contains(err.Error(), "context canceled")) << err;
    });
}

TEST(Sql, TxRollbackCommitErr) {
    testDatabase([](sql::DB& db) {
        populate(db, "people");
        auto [tx, err] = db.Begin();
        ASSERT_FALSE(err);
        EXPECT_FALSE(tx->Rollback());
        EXPECT_EQ(tx->Commit(), sql::ErrTxDone);
        auto [tx2, err2] = db.Begin();
        ASSERT_FALSE(err2);
        EXPECT_FALSE(tx2->Commit());
        EXPECT_EQ(tx2->Rollback(), sql::ErrTxDone);
    });
}

TEST(Sql, StatementErrorAfterClose) {
    testDatabase([](sql::DB& db) {
        populate(db, "people");
        auto [stmt, err] = db.Prepare("SELECT|people|age|name=?");
        ASSERT_FALSE(err) << "Prepare: " << err;
        ASSERT_FALSE(stmt->Close());
        string name;
        EXPECT_TRUE(stmt->QueryRow("foo")->Scan(&name));
    });
}

TEST(Sql, StatementQueryRow) {
    testDatabase([](sql::DB& db) {
        populate(db, "people");
        auto [stmt, err] = db.Prepare("SELECT|people|age|name=?");
        ASSERT_FALSE(err) << "Prepare: " << err;
        int age;
        std::pair<const char*, int> tests[] = {{"Alice", 1}, {"Bob", 2}, {"Chris", 3}};
        for (auto [name, want] : tests) {
            gopp::error serr = stmt->QueryRow(name)->Scan(&age);
            EXPECT_FALSE(serr) << name << ": " << serr;
            EXPECT_EQ(age, want);
        }
        stmt->Close();
    });
}

TEST(Sql, StatementClose) {
    // A sticky error is what Close returns.
    ptr<sql::Stmt> s;
    {
        // A Tx statement for a closed transaction: Stmt with stickyErr.
        auto db = newTestDb("people");
        auto [tx, err] = db->Begin();
        ASSERT_FALSE(err);
        tx->Rollback();
        auto [stmt, perr] = db->Prepare("SELECT|people|age|");
        ASSERT_FALSE(perr);
        s = tx->Stmt(stmt);
        EXPECT_EQ(s->Close(), sql::ErrTxDone) << "stickyErr not propagated";
        stmt->Close();
        closeDb(*db);
    }
}

TEST(Sql, Exec) {
    testDatabase([](sql::DB& db) {
        exec(db, "CREATE|t1|name=string,age=int32,dead=bool");
        auto [stmt, err] = db.Prepare("INSERT|t1|name=?,age=?");
        ASSERT_FALSE(err) << err;
        auto check = [&](std::string want, auto... args) {
            auto [res, xerr] = stmt->Exec(args...);
            EXPECT_EQ(xerr ? xerr.Error().str() : std::string(), want);
        };
        // Okay:
        check("", "Brad", 31);
        check("", "Brad", int64(31));
        check("", "Bob", "32");
        check("", 7, 9);
        // Invalid conversions:
        check("sql: converting argument $2 type: sql/driver: value 4294967295 overflows int32", "Brad", int64(0xFFFFFFFF));
        check("sql: converting argument $2 type: sql/driver: value \"strconv fail\" can't be converted to int32", "Brad",
              "strconv fail");
        // Wrong number of args:
        check("sql: expected 2 arguments, got 0");
        check("sql: expected 2 arguments, got 3", 1, 2, 3);
        stmt->Close();
    });
}

TEST(Sql, TxPrepare) {
    testDatabase([](sql::DB& db) {
        exec(db, "CREATE|t1|name=string,age=int32,dead=bool");
        auto [tx, err] = db.Begin();
        ASSERT_FALSE(err) << "Begin = " << err;
        auto [stmt, perr] = tx->Prepare("INSERT|t1|name=?,age=?");
        ASSERT_FALSE(perr) << perr;
        auto [res, xerr] = stmt->Exec("Bobby", 7);
        ASSERT_FALSE(xerr) << "Exec = " << xerr;
        ASSERT_FALSE(tx->Commit());
        // Commit closed the statement.
        EXPECT_TRUE(testAccess::stmtClosed(*stmt)) << "Stmt not closed after Commit";
        stmt->Close();
    });
}

TEST(Sql, TxStmt) {
    testDatabase([](sql::DB& db) {
        exec(db, "CREATE|t1|name=string,age=int32,dead=bool");
        auto [stmt, err] = db.Prepare("INSERT|t1|name=?,age=?");
        ASSERT_FALSE(err) << err;
        auto [tx, berr] = db.Begin();
        ASSERT_FALSE(berr);
        auto txs = tx->Stmt(stmt);
        auto [res, xerr] = txs->Exec("Bobby", 7);
        ASSERT_FALSE(xerr) << "Exec = " << xerr;
        ASSERT_FALSE(tx->Commit());
        EXPECT_TRUE(testAccess::stmtClosed(*txs)) << "Stmt not closed after Commit";
        txs->Close();
        stmt->Close();
    });
}

TEST(Sql, TxStmtPreparedOnce) {
    testDatabase([](sql::DB& db) {
        exec(db, "CREATE|t1|name=string,age=int32");
        int prepares0 = numPrepares(db);
        // db.Prepare increments numPrepares.
        auto [stmt, err] = db.Prepare("INSERT|t1|name=?,age=?");
        ASSERT_FALSE(err);
        auto [tx, berr] = db.Begin();
        ASSERT_FALSE(berr);
        auto txs1 = tx->Stmt(stmt);
        auto txs2 = tx->Stmt(stmt);
        ASSERT_FALSE(txs1->Exec("Go", 7).second);
        txs1->Close();
        ASSERT_FALSE(txs2->Exec("Gopher", 8).second);
        txs2->Close();
        ASSERT_FALSE(tx->Commit());
        EXPECT_EQ(numPrepares(db) - prepares0, 1);
        stmt->Close();
    });
}

TEST(Sql, TxQuery) {
    testDatabase([](sql::DB& db) {
        exec(db, "CREATE|t1|name=string,age=int32,dead=bool");
        exec(db, "INSERT|t1|name=Alice");
        auto [tx, err] = db.Begin();
        ASSERT_FALSE(err);
        auto [r, qerr] = tx->Query("SELECT|t1|name|");
        ASSERT_FALSE(qerr) << qerr;
        ASSERT_TRUE(r->Next()) << r->Err();
        string x;
        EXPECT_FALSE(r->Scan(&x));
        r->Close();
        tx->Rollback();
    });
}

TEST(Sql, TxQueryInvalid) {
    testDatabase([](sql::DB& db) {
        auto [tx, err] = db.Begin();
        ASSERT_FALSE(err);
        auto [r, qerr] = tx->Query("SELECT|t1|name|");
        EXPECT_TRUE(qerr) << "Error expected";
        tx->Rollback();
    });
}

TEST(Sql, ConnQuery) {
    testDatabase([](sql::DB& db) {
        populate(db, "people");
        auto ctx = context::Background();
        auto [conn, err] = db.Conn(ctx);
        ASSERT_FALSE(err);
        getFakeConn(testAccess::ci(*conn))->skipDirtySession = true;
        string name;
        ASSERT_FALSE(conn->QueryRowContext(ctx, "SELECT|people|name|age=?", 3)->Scan(&name));
        EXPECT_EQ(name, "Chris");
        EXPECT_FALSE(conn->PingContext(ctx));
        conn->Close();
    });
}

TEST(Sql, ConnRaw) {
    testDatabase([](sql::DB& db) {
        populate(db, "people");
        auto ctx = context::Background();
        auto [conn, err] = db.Conn(ctx);
        ASSERT_FALSE(err);
        getFakeConn(testAccess::ci(*conn))->skipDirtySession = true;
        bool sawFunc = false;
        gopp::error rerr = conn->Raw([&](driver::Conn& dc) -> gopp::error {
            sawFunc = true;
            EXPECT_TRUE(dynamic_cast<fakeConn*>(&dc) || dynamic_cast<basicConn*>(&dc));
            return nullptr;
        });
        EXPECT_FALSE(rerr);
        EXPECT_TRUE(sawFunc) << "Raw func not called";
        conn->Close();
    });
}

TEST(Sql, ConnTx) {
    testDatabase([](sql::DB& db) {
        populate(db, "people");
        auto ctx = context::Background();
        auto [conn, err] = db.Conn(ctx);
        ASSERT_FALSE(err);
        getFakeConn(testAccess::ci(*conn))->skipDirtySession = true;
        auto [tx, berr] = conn->BeginTx(ctx, nullptr);
        ASSERT_FALSE(berr);
        auto [res, xerr] = tx->ExecContext(ctx, "INSERT|people|name=?,age=?,photo=APHOTO", "Nancy", 33);
        ASSERT_FALSE(xerr) << xerr;
        ASSERT_FALSE(tx->Commit());
        string selectName;
        ASSERT_FALSE(conn->QueryRowContext(ctx, "SELECT|people|name|age=?", 33)->Scan(&selectName));
        EXPECT_EQ(selectName, "Nancy");
        conn->Close();
    });
}

TEST(Sql, ConnIsValid) {
    testDatabase(
        [](sql::DB& db) {
            populate(db, "people");
            db.SetMaxOpenConns(1);
            auto [c, err] = db.Conn(context::Background());
            ASSERT_FALSE(err);
            c->Raw([](driver::Conn& raw) -> gopp::error {
                getFakeConn(ptr<driver::Conn>(&raw))->stickyBad = true;
                return nullptr;
            });
            c->Close();
            auto fcs = testAccess::freeConns(db);
            EXPECT_FALSE(!fcs.empty() && getFakeConn(fcs[0]->ci)->stickyBad)
                << "bad connection returned to pool; expected bad connection to be discarded";
        },
        true);
}

TEST(Sql, QueryNamedArg) {
    testDatabase(
        [](sql::DB& db) {
            populate(db, "people");
            int prepares0 = numPrepares(db);
            // The name and age parameters match on name, not position.
            auto [rows, err] = db.Query("SELECT|people|age,name|name=?name,age=?age", sql::Named("age", 2),
                                        sql::Named("name", "Bob"));
            ASSERT_FALSE(err) << err;
            std::vector<personRow> got;
            while (rows->Next()) {
                personRow r;
                ASSERT_FALSE(rows->Scan(&r.age, &r.name));
                got.push_back(r);
            }
            ASSERT_FALSE(rows->Err());
            EXPECT_EQ(got, (std::vector<personRow>{{2, "Bob"}}));
            EXPECT_EQ(numFreeConns(db), 1u);
            EXPECT_EQ(numPrepares(db) - prepares0, 1);
        },
        true);
}

TEST(Sql, MultiResultSetQuery) {
    testDatabase([](sql::DB& db) {
        populate(db, "people");
        int prepares0 = numPrepares(db);
        auto [rows, err] = db.Query("SELECT|people|age,name|;SELECT|people|name|");
        ASSERT_FALSE(err) << err;
        std::vector<personRow> got1;
        while (rows->Next()) {
            personRow r;
            ASSERT_FALSE(rows->Scan(&r.age, &r.name));
            got1.push_back(r);
        }
        ASSERT_FALSE(rows->Err());
        EXPECT_EQ(got1, (std::vector<personRow>{{1, "Alice"}, {2, "Bob"}, {3, "Chris"}}));
        EXPECT_TRUE(rows->NextResultSet()) << "expected another result set";
        std::vector<string> got2;
        while (rows->Next()) {
            string name;
            ASSERT_FALSE(rows->Scan(&name));
            got2.push_back(name);
        }
        ASSERT_FALSE(rows->Err());
        EXPECT_EQ(got2, (std::vector<string>{"Alice", "Bob", "Chris"}));
        EXPECT_FALSE(rows->NextResultSet()) << "expected no more result sets";
        EXPECT_TRUE(waitFor([&] { return numFreeConns(db) == 1; }));
        EXPECT_EQ(numPrepares(db) - prepares0, 1);
    });
}

TEST(Sql, RowsColumns) {
    testDatabase([](sql::DB& db) {
        populate(db, "people");
        auto [rows, err] = db.Query("SELECT|people|age,name|");
        ASSERT_FALSE(err);
        auto [cols, cerr] = rows->Columns();
        ASSERT_FALSE(cerr);
        EXPECT_EQ(cols, (std::vector<string>{"age", "name"}));
        EXPECT_FALSE(rows->Close());
    });
}

TEST(Sql, RowsColumnTypes) {
    testDatabase([](sql::DB& db) {
        populate(db, "people");
        auto [rows, err] = db.Query("SELECT|people|age,name|");
        ASSERT_FALSE(err);
        auto [tt, terr] = rows->ColumnTypes();
        ASSERT_FALSE(terr);
        // Go's ScanType is a reflect.Type; here, its name.
        ASSERT_EQ(tt.size(), 2u);
        EXPECT_EQ(tt[0]->Name(), "age");
        EXPECT_EQ(tt[0]->ScanType(), "int32");
        EXPECT_EQ(tt[1]->ScanType(), "string");
        int ct = 0;
        while (rows->Next()) {
            int32 age;
            string name;
            ASSERT_FALSE(rows->Scan(&age, &name));
            if (ct == 1) {
                EXPECT_EQ(age, 2);
                EXPECT_EQ(name, "Bob");
            }
            ct++;
        }
        EXPECT_EQ(ct, 3);
        EXPECT_FALSE(rows->Close());
    });
}

TEST(Sql, CloseStmtBeforeRows) {
    testDatabase([](sql::DB& db) {
        populate(db, "people");
        auto [s, err] = db.Prepare("SELECT|people|name|");
        ASSERT_FALSE(err);
        auto [r, qerr] = s->Query();
        ASSERT_FALSE(qerr);
        ASSERT_FALSE(s->Close());
        r->Close();
    });
}

TEST(Sql, NullByteSlice) {
    testDatabase([](sql::DB& db) {
        exec(db, "CREATE|t|id=int32,name=nullstring");
        exec(db, "INSERT|t|id=10,name=?", nullptr);
        slice<byte> name;
        ASSERT_FALSE(db.QueryRow("SELECT|t|name|id=?", 10)->Scan(&name));
        EXPECT_TRUE(name.isNil()) << "name []byte should be nil for null column value";
        exec(db, "INSERT|t|id=11,name=?", "bob");
        ASSERT_FALSE(db.QueryRow("SELECT|t|name|id=?", 11)->Scan(&name));
        EXPECT_EQ(asStringView(name.span()), "bob");
    });
}

TEST(Sql, PointerParamsAndScans) {
    testDatabase([](sql::DB& db) {
        exec(db, "CREATE|t|id=int32,name=nullstring");
        std::string bob = "bob";
        const std::string* name = &bob;
        exec(db, "INSERT|t|id=10,name=?", name);
        name = nullptr;
        exec(db, "INSERT|t|id=20,name=?", name);
        // Go's **string is std::optional<T> here.
        std::optional<string> got;
        ASSERT_FALSE(db.QueryRow("SELECT|t|name|id=?", 10)->Scan(&got));
        EXPECT_TRUE(got && *got == "bob");
        ASSERT_FALSE(db.QueryRow("SELECT|t|name|id=?", 20)->Scan(&got));
        EXPECT_FALSE(got) << "id 20 = " << *got << "; want nil";
    });
}

namespace {

// nullTestRun: a nullable column type and its Null type.
template <typename N, typename P>
struct nullTestRow {
    std::variant<N, P, std::nullptr_t> nullParam;
    std::variant<N, P, std::nullptr_t> notNullParam;
    N scanNullVal;
};

template <typename N, typename P>
void nullTestRun(sql::DB& db, std::string nullType, std::string notNullType, std::array<nullTestRow<N, P>, 6> rows,
                   const std::function<bool(const N&, const N&)>& eq) {
    exec(db, "CREATE|t|id=int32,name=string,nullf=" + nullType + ",notnullf=" + notNullType);
    auto arg = [](const std::variant<N, P, std::nullptr_t>& v) {
        return std::visit([](const auto& x) { return sql::toArg(x); }, v);
    };
    auto value = [&](const std::variant<N, P, std::nullptr_t>& v) {
        sql::detail::arg a = arg(v);
        return driver::Value(a.value);
    };
    // Inserts with db.Exec:
    exec(db, "INSERT|t|id=?,name=?,nullf=?,notnullf=?", 1, "alice", value(rows[0].nullParam), value(rows[0].notNullParam));
    exec(db, "INSERT|t|id=?,name=?,nullf=?,notnullf=?", 2, "bob", value(rows[1].nullParam), value(rows[1].notNullParam));
    // Inserts with a prepared statement:
    auto [stmt, err] = db.Prepare("INSERT|t|id=?,name=?,nullf=?,notnullf=?");
    ASSERT_FALSE(err) << "prepare: " << err;
    EXPECT_FALSE(stmt->Exec(3, "chris", value(rows[2].nullParam), value(rows[2].notNullParam)).second);
    EXPECT_FALSE(stmt->Exec(4, "dave", value(rows[3].nullParam), value(rows[3].notNullParam)).second);
    EXPECT_FALSE(stmt->Exec(5, "eleanor", value(rows[4].nullParam), value(rows[4].notNullParam)).second);
    // Can't put null val into non-null col.
    EXPECT_TRUE(stmt->Exec(6, "bob", value(rows[5].nullParam), value(rows[5].notNullParam)).second)
        << "expected error inserting nil val with prepared statement Exec";
    for (int i = 0; i < 5; i++) {
        int id = i + 1;
        N bindVal;
        gopp::error serr = db.QueryRow("SELECT|t|nullf|id=?", id)->Scan(&bindVal);
        EXPECT_FALSE(serr) << "id=" << id << " Scan: " << serr;
        EXPECT_TRUE(eq(bindVal, rows[static_cast<std::size_t>(i)].scanNullVal)) << "id=" << id;
    }
    stmt->Close();
}

}  // namespace

TEST(Sql, NullStringParam) {
    using N = sql::NullString;
    testDatabase([](sql::DB& db) {
        nullTestRun<N, string>(db, "nullstring", "string",
                                 {{{N{"aqua", true}, string(""), N{"aqua", true}},
                                   {N{"brown", false}, string(""), N{"", false}},
                                   {string("chartreuse"), string(""), N{"chartreuse", true}},
                                   {N{"darkred", true}, string(""), N{"darkred", true}},
                                   {N{"eel", false}, string(""), N{"", false}},
                                   {string("foo"), N{"black", false}, N{}}}},
                                 [](const N& a, const N& b) { return a.String == b.String && a.Valid == b.Valid; });
    });
}

TEST(Sql, NullInt64Param) {
    using N = sql::NullInt64;
    testDatabase([](sql::DB& db) {
        nullTestRun<N, int64>(db, "nullint64", "int64",
                                {{{N{31, true}, int64(1), N{31, true}},
                                  {N{-22, false}, int64(1), N{0, false}},
                                  {int64(22), int64(1), N{22, true}},
                                  {N{33, true}, int64(1), N{33, true}},
                                  {N{222, false}, int64(1), N{0, false}},
                                  {int64(0), N{31, false}, N{}}}},
                                [](const N& a, const N& b) { return a.Int64 == b.Int64 && a.Valid == b.Valid; });
    });
}

TEST(Sql, NullInt32Param) {
    using N = sql::NullInt32;
    testDatabase([](sql::DB& db) {
        nullTestRun<N, int32>(db, "nullint32", "int32",
                                {{{N{31, true}, int32(1), N{31, true}},
                                  {N{-22, false}, int32(1), N{0, false}},
                                  {int32(22), int32(1), N{22, true}},
                                  {N{33, true}, int32(1), N{33, true}},
                                  {N{222, false}, int32(1), N{0, false}},
                                  {int32(0), N{31, false}, N{}}}},
                                [](const N& a, const N& b) { return a.Int32 == b.Int32 && a.Valid == b.Valid; });
    });
}

TEST(Sql, NullFloat64Param) {
    using N = sql::NullFloat64;
    testDatabase([](sql::DB& db) {
        nullTestRun<N, double>(db, "nullfloat64", "float64",
                                 {{{N{31.2, true}, 1.0, N{31.2, true}},
                                   {N{13.1, false}, 1.0, N{0, false}},
                                   {-22.9, 1.0, N{-22.9, true}},
                                   {N{33.81, true}, 1.0, N{33.81, true}},
                                   {N{222, false}, 1.0, N{0, false}},
                                   {10.0, N{31.2, false}, N{}}}},
                                 [](const N& a, const N& b) { return a.Float64 == b.Float64 && a.Valid == b.Valid; });
    });
}

TEST(Sql, NullBoolParam) {
    using N = sql::NullBool;
    testDatabase([](sql::DB& db) {
        nullTestRun<N, bool>(db, "nullbool", "bool",
                               {{{N{false, true}, true, N{false, true}},
                                 {N{true, false}, false, N{false, false}},
                                 {true, true, N{true, true}},
                                 {N{true, true}, true, N{true, true}},
                                 {N{true, false}, true, N{false, false}},
                                 {true, N{true, false}, N{}}}},
                               [](const N& a, const N& b) { return a.Bool == b.Bool && a.Valid == b.Valid; });
    });
}

TEST(Sql, NullTimeParam) {
    using N = sql::NullTime;
    time::Time t0;
    time::Time t1 = time::Time::Unix(1000000, 0);
    time::Time t2 = time::Time::Unix(1000000000, 0);
    testDatabase([&](sql::DB& db) {
        nullTestRun<N, time::Time>(db, "nulldatetime", "datetime",
                                     {{{N{t1, true}, t2, N{t1, true}},
                                       {N{t1, false}, t2, N{t0, false}},
                                       {t1, t2, N{t1, true}},
                                       {N{t1, true}, t2, N{t1, true}},
                                       {N{t1, false}, t2, N{t0, false}},
                                       {t2, N{t1, false}, N{}}}},
                                     [](const N& a, const N& b) { return a.Time.Equal(b.Time) && a.Valid == b.Valid; });
    });
}

TEST(Sql, QueryRowNilScanDest) {
    testDatabase([](sql::DB& db) {
        populate(db, "people");
        string* name = nullptr;
        gopp::error err = db.QueryRow("SELECT|people|name|")->Scan(name);
        EXPECT_EQ(err ? err.Error().str() : "", "sql: Scan error on column index 0, name \"name\": destination pointer is nil");
    });
}

TEST(Sql, InvalidNilValues) {
    for (int which = 0; which < 2; which++) {
        auto db = newTestDb("people");
        auto ctx = context::Background();
        auto [conn, err] = db->Conn(ctx);
        ASSERT_FALSE(err);
        getFakeConn(testAccess::ci(*conn))->skipDirtySession = true;
        gopp::error serr;
        std::string want;
        if (which == 0) {
            time::Time date1;
            serr = conn->QueryRowContext(ctx, "SELECT|people|bdate|age=?", 1)->Scan(&date1);
            want = "sql: Scan error on column index 0, name \"bdate\": unsupported Scan, storing driver.Value type <nil> into type *time.Time";
        } else {
            int date2;
            serr = conn->QueryRowContext(ctx, "SELECT|people|bdate|age=?", 1)->Scan(&date2);
            want = "sql: Scan error on column index 0, name \"bdate\": converting NULL to int is unsupported";
        }
        ASSERT_TRUE(serr) << "expected error when querying nil column, but succeeded";
        EXPECT_EQ(serr.Error(), want);
        EXPECT_FALSE(conn->PingContext(ctx));
        conn->Close();
        closeDb(*db);
    }
}

TEST(Sql, MaxIdleConns) {
    testDatabase([](sql::DB& db) {
        populate(db, "people");
        auto [tx, err] = db.Begin();
        ASSERT_FALSE(err);
        tx->Commit();
        EXPECT_EQ(numFreeConns(db), 1u);
        db.SetMaxIdleConns(0);
        EXPECT_EQ(numFreeConns(db), 0u) << "freeConns after set to zero";
        auto [tx2, err2] = db.Begin();
        ASSERT_FALSE(err2);
        tx2->Commit();
        EXPECT_EQ(numFreeConns(db), 0u);
    });
}

TEST(Sql, MaxOpenConns) {
    testDatabase([](sql::DB& db) {
        populate(db, "magicquery");
        auto drv = fdriver();
        clearAllConns(db);
        int opens0, closes0;
        {
            std::lock_guard l(drv->mu);
            opens0 = drv->openCount;
            closes0 = drv->closeCount;
        }
        db.SetMaxIdleConns(10);
        db.SetMaxOpenConns(10);
        auto [stmt, err] = db.Prepare("SELECT|magicquery|op|op=?,millis=?");
        ASSERT_FALSE(err) << err;
        // 50 parallel slow queries, twice.
        constexpr int nquery = 50, sleepMillis = 25, nbatch = 2;
        std::atomic<int> errs{0};
        for (int batch = 0; batch < nbatch; batch++) {
            sync::WaitGroup wg;
            for (int i = 0; i < nquery; i++) {
                wg.Go([&] {
                    string op;
                    gopp::error qerr = stmt->QueryRow("sleep", sleepMillis)->Scan(&op);
                    if (qerr && qerr != sql::ErrNoRows) errs++;
                });
            }
            wg.Wait();
        }
        EXPECT_EQ(errs.load(), 0);
        EXPECT_TRUE(waitFor([&] { return numFreeConns(db) == 10; })) << "free conns = " << numFreeConns(db);
        EXPECT_LE(testAccess::numDeps(db), 20u);
        int opens, closes;
        {
            std::lock_guard l(drv->mu);
            opens = drv->openCount - opens0;
            closes = drv->closeCount - closes0;
        }
        EXPECT_LE(opens, 10) << "db connections opened; closes " << closes;
        ASSERT_FALSE(stmt->Close());
        EXPECT_EQ(numFreeConns(db), 10u);
        EXPECT_LE(testAccess::numDeps(db), 10u);
        db.SetMaxOpenConns(5);
        EXPECT_EQ(numFreeConns(db), 5u);
        EXPECT_LE(testAccess::numDeps(db), 5u);
        db.SetMaxOpenConns(0);
        EXPECT_EQ(numFreeConns(db), 5u);
        clearAllConns(db);
    });
}

TEST(Sql, PendingConnsAfterErr) {
    constexpr int maxOpen = 2, tryOpen = maxOpen * 2 + 2;
    auto [db, err] = sql::Open("test", fakeDbName);
    ASSERT_FALSE(err);
    db->SetMaxOpenConns(maxOpen);
    db->SetMaxIdleConns(0);
    const gopp::error errOffline = errors::New("db offline");
    auto errs = make_chan<gopp::error>(tryOpen);
    sync::WaitGroup opening;
    opening.Add(tryOpen);
    hookOpenErr = [&] {
        // Wait for all connections to enqueue.
        opening.Wait();
        return errOffline;
    };
    for (int i = 0; i < tryOpen; i++) {
        go([&, db] {
            opening.Done();  // one connection is in flight
            auto [res, xerr] = db->Exec("will never run");
            errs.send(xerr);
        });
    }
    opening.Wait();
    // All connections fail without deadlock. (One timer for all, stopped at
    // the end: time::After's would live on for 5s each.)
    auto timeout = time::NewTimer(5 * time::Second);
    for (int i = 0; i < tryOpen; i++) {
        gopp::error got;
        bool timedOut = false;
        select(recv(errs, [&](gopp::error e) { got = e; }), recv(timeout->C, [&] { timedOut = true; }));
        ASSERT_FALSE(timedOut) << "orphaned connection request(s)";
        EXPECT_EQ(got, errOffline) << got;
    }
    timeout->Stop();
    hookOpenErr = nullptr;
    closeDb(*db);
}

TEST(Sql, SingleOpenConn) {
    testDatabase([](sql::DB& db) {
        populate(db, "people");
        db.SetMaxOpenConns(1);
        auto [rows, err] = db.Query("SELECT|people|name|");
        ASSERT_FALSE(err);
        ASSERT_FALSE(rows->Close());
        // Shouldn't deadlock.
        auto [rows2, err2] = db.Query("SELECT|people|name|");
        ASSERT_FALSE(err2);
        ASSERT_FALSE(rows2->Close());
    });
}

TEST(Sql, Stats) {
    auto db = newTestDb("people");
    EXPECT_EQ(db->Stats().OpenConnections, 1);
    auto [tx, err] = db->Begin();
    ASSERT_FALSE(err);
    tx->Commit();
    closeDb(*db);
    EXPECT_EQ(db->Stats().OpenConnections, 0);
}

TEST(Sql, StmtCloseDeps) {
    testDatabase([](sql::DB& db) {
        populate(db, "magicquery");
        auto drv = fdriver();
        int openDelta0;
        {
            std::lock_guard l(drv->mu);
            openDelta0 = drv->openCount - drv->closeCount;
        }
        auto [stmt, err] = db.Prepare("SELECT|magicquery|op|op=?,millis=?");
        ASSERT_FALSE(err);
        constexpr int nquery = 50, sleepMillis = 25, nbatch = 2;
        for (int batch = 0; batch < nbatch; batch++) {
            sync::WaitGroup wg;
            for (int i = 0; i < nquery; i++) {
                wg.Go([&] {
                    string op;
                    gopp::error qerr = stmt->QueryRow("sleep", sleepMillis)->Scan(&op);
                    EXPECT_TRUE(!qerr || qerr == sql::ErrNoRows) << qerr;
                });
            }
            wg.Wait();
        }
        EXPECT_TRUE(waitFor([&] { return numFreeConns(db) == 2; })) << "free conns = " << numFreeConns(db);
        EXPECT_LE(testAccess::numDeps(db), 4u);
        int openDelta;
        {
            std::lock_guard l(drv->mu);
            openDelta = (drv->openCount - drv->closeCount) - openDelta0;
        }
        EXPECT_LE(openDelta, 2) << "db connections opened";
        EXPECT_LE(testAccess::cssSize(*stmt), static_cast<std::size_t>(nquery));
        ASSERT_FALSE(stmt->Close());
        EXPECT_EQ(numFreeConns(db), 2u);
        EXPECT_LE(testAccess::numDeps(db), 2u);
        clearAllConns(db);
    });
}

TEST(Sql, CloseConnBeforeStmts) {
    testDatabase([](sql::DB& db) {
        populate(db, "people");
        hookPostCloseConn = [](gopp::error err) { EXPECT_FALSE(err) << "Error closing fakeConn: " << err; };
        auto [stmt, err] = db.Prepare("SELECT|people|name|");
        ASSERT_FALSE(err);
        auto fcs = testAccess::freeConns(db);
        ASSERT_EQ(fcs.size(), 1u);
        auto dc = fcs[0];
        EXPECT_FALSE(dc->closed);
        EXPECT_EQ(dc->openStmt.size(), 1u);
        EXPECT_FALSE(db.Close());
        EXPECT_TRUE(dc->closed) << "after db.Close, driverConn should be closed";
        EXPECT_EQ(dc->openStmt.size(), 0u);
        EXPECT_FALSE(stmt->Close());
        EXPECT_TRUE(dc->closed);
        EXPECT_FALSE(dc->ci) << "after Stmt Close, driverConn's Conn interface should be nil";
        hookPostCloseConn = nullptr;
    });
}

TEST(Sql, RowsCloseOrder) {
    testDatabase([](sql::DB& db) {
        populate(db, "people");
        db.SetMaxIdleConns(0);
        strictClose = true;
        auto [rows, err] = db.Query("SELECT|people|age,name|");
        ASSERT_FALSE(err);
        EXPECT_FALSE(rows->Close());
        strictClose = false;
    });
}

TEST(Sql, ManyErrBadConn) {
    auto setup = [](std::function<void(sql::DB&)> first = nullptr) {
        auto db = newTestDb("people");
        if (first) first(*db);
        int nconn = 2 + 1;  // maxBadConnRetries + 1
        db->SetMaxIdleConns(nconn);
        db->SetMaxOpenConns(nconn);
        // Open enough connections.
        {
            std::vector<ptr<sql::Rows>> open;
            for (int i = 0; i < nconn; i++) {
                auto [rows, err] = db->Query("SELECT|people|age,name|");
                EXPECT_FALSE(err);
                open.push_back(rows);
            }
            for (auto& r : open) r->Close();
        }
        EXPECT_EQ(testAccess::numOpen(*db), nconn);
        auto fcs = testAccess::freeConns(*db);
        EXPECT_EQ(static_cast<int>(fcs.size()), nconn);
        for (auto& c : fcs) {
            std::lock_guard l(c->mu);
            getFakeConn(c->ci)->stickyBad = true;
        }
        return db;
    };

    {  // Query
        auto db = setup();
        auto [rows, err] = db->Query("SELECT|people|age,name|");
        ASSERT_FALSE(err) << err;
        ASSERT_FALSE(rows->Close());
        closeDb(*db);
    }
    {  // Exec
        auto db = setup();
        auto [res, err] = db->Exec("INSERT|people|name=Julia,age=19");
        ASSERT_FALSE(err) << err;
        closeDb(*db);
    }
    {  // Begin
        auto db = setup();
        auto [tx, err] = db->Begin();
        ASSERT_FALSE(err) << err;
        ASSERT_FALSE(tx->Rollback());
        closeDb(*db);
    }
    {  // Prepare
        auto db = setup();
        auto [stmt, err] = db->Prepare("SELECT|people|age,name|");
        ASSERT_FALSE(err) << err;
        ASSERT_FALSE(stmt->Close());
        closeDb(*db);
    }
    {  // Stmt.Exec
        ptr<sql::Stmt> stmt;
        auto db = setup([&](sql::DB& d) {
            gopp::error err;
            std::tie(stmt, err) = d.Prepare("INSERT|people|name=Julia,age=19");
            ASSERT_FALSE(err);
        });
        auto [res, err] = stmt->Exec();
        ASSERT_FALSE(err) << err;
        ASSERT_FALSE(stmt->Close());
        closeDb(*db);
    }
    {  // Stmt.Query
        ptr<sql::Stmt> stmt;
        auto db = setup([&](sql::DB& d) {
            gopp::error err;
            std::tie(stmt, err) = d.Prepare("SELECT|people|age,name|");
            ASSERT_FALSE(err);
        });
        auto [rows, err] = stmt->Query();
        ASSERT_FALSE(err) << err;
        ASSERT_FALSE(rows->Close());
        ASSERT_FALSE(stmt->Close());
        closeDb(*db);
    }
    {  // Conn
        auto db = setup();
        auto [conn, err] = db->Conn(context::Background());
        ASSERT_FALSE(err) << err;
        getFakeConn(testAccess::ci(*conn))->skipDirtySession = true;
        ASSERT_FALSE(conn->Close());
        closeDb(*db);
    }
    {  // Ping
        auto db = setup();
        ASSERT_FALSE(db->PingContext(context::Background()));
        closeDb(*db);
    }
}

TEST(Sql, ErrBadConnReconnect) {
    testDatabase([](sql::DB& db) {
        exec(db, "CREATE|t1|name=string,age=int32,dead=bool");
        auto simulateBadConn = [&](std::string name, std::function<bool()>& hook, const std::function<gopp::error()>& op) {
            bool broken = false, retried = false;
            int numOpen = testAccess::numOpen(db);
            // A broken connection on the first try.
            hook = [&] {
                if (!broken) {
                    broken = true;
                    return true;
                }
                retried = true;
                return false;
            };
            gopp::error err = op();
            EXPECT_FALSE(err) << name << ": " << err;
            EXPECT_TRUE(broken && retried) << name << ": Failed to simulate broken connection";
            hook = nullptr;
            EXPECT_EQ(numOpen, testAccess::numOpen(db)) << name << ": leaked connection(s)";
        };
        auto dbExec = [&] { return db.Exec("INSERT|t1|name=?,age=?,dead=?", "Gordon", 3, true).second; };
        simulateBadConn("db.Exec prepare", hookPrepareBadConn, dbExec);
        simulateBadConn("db.Exec exec", hookExecBadConn, dbExec);
        auto dbQuery = [&] {
            auto [rows, err] = db.Query("SELECT|t1|age,name|");
            if (!err) err = rows->Close();
            return err;
        };
        simulateBadConn("db.Query prepare", hookPrepareBadConn, dbQuery);
        simulateBadConn("db.Query query", hookQueryBadConn, dbQuery);
        simulateBadConn("db.Prepare", hookPrepareBadConn, [&] {
            auto [stmt, err] = db.Prepare("INSERT|t1|name=?,age=?,dead=?");
            if (err) return err;
            stmt->Close();
            return gopp::error();
        });
        auto [stmt1, err1] = db.Prepare("INSERT|t1|name=?,age=?,dead=?");
        ASSERT_FALSE(err1);
        testAccess::clearCss(*stmt1);  // must prepare again
        auto stmtExec = [&] { return stmt1->Exec("Gopher", 3, false).second; };
        simulateBadConn("stmt.Exec prepare", hookPrepareBadConn, stmtExec);
        simulateBadConn("stmt.Exec exec", hookExecBadConn, stmtExec);
        auto [stmt2, err2] = db.Prepare("SELECT|t1|age,name|");
        ASSERT_FALSE(err2);
        testAccess::clearCss(*stmt2);
        auto stmtQuery = [&] {
            auto [rows, err] = stmt2->Query();
            if (!err) err = rows->Close();
            return err;
        };
        simulateBadConn("stmt.Query prepare", hookPrepareBadConn, stmtQuery);
        simulateBadConn("stmt.Query exec", hookQueryBadConn, stmtQuery);
        stmt1->Close();
        stmt2->Close();
    });
}

TEST(Sql, TxEndBadConn) {
    testDatabase([](sql::DB& db) {
        db.SetMaxIdleConns(0);
        exec(db, "CREATE|t1|name=string,age=int32,dead=bool");
        db.SetMaxIdleConns(1);
        auto simulateBadConn = [&](std::string name, std::function<bool()>& hook, const std::function<gopp::error()>& op) {
            bool broken = false;
            int numOpen = testAccess::numOpen(db);
            hook = [&] {
                broken = true;
                return true;
            };
            gopp::error err = op();
            EXPECT_TRUE(errors::Is(err, driver::ErrBadConn)) << name << ": " << err;
            EXPECT_TRUE(broken) << name << ": Failed to simulate broken connection";
            hook = nullptr;
            EXPECT_TRUE(waitFor([&] { return numOpen == testAccess::numOpen(db); })) << name << ": leaked connection(s)";
        };
        auto dbExec = [&](bool commit) {
            return [&, commit] {
                auto [tx, err] = db.Begin();
                if (err) return err;
                if (gopp::error xerr = tx->Exec("INSERT|t1|name=?,age=?,dead=?", "Gordon", 3, true).second) return xerr;
                return commit ? tx->Commit() : tx->Rollback();
            };
        };
        simulateBadConn("db.Tx.Exec commit", hookCommitBadConn, dbExec(true));
        simulateBadConn("db.Tx.Exec rollback", hookRollbackBadConn, dbExec(false));
        auto dbQuery = [&](bool commit) {
            return [&, commit] {
                auto [tx, err] = db.Begin();
                if (err) return err;
                auto [rows, qerr] = tx->Query("SELECT|t1|age,name|");
                if (qerr) return qerr;
                if (gopp::error cerr = rows->Close()) return cerr;
                return commit ? tx->Commit() : tx->Rollback();
            };
        };
        simulateBadConn("db.Tx.Query commit", hookCommitBadConn, dbQuery(true));
        simulateBadConn("db.Tx.Query rollback", hookRollbackBadConn, dbQuery(false));
    });
}

TEST(Sql, TxErrBadConn) {
    auto [db, err] = sql::Open("test", fakeDbName + ";badConn");
    ASSERT_FALSE(err);
    ASSERT_FALSE(db->Exec("WIPE").second);
    exec(*db, "CREATE|t1|name=string,age=int32,dead=bool");
    auto [stmt, perr] = db->Prepare("INSERT|t1|name=?,age=?");
    ASSERT_FALSE(perr) << perr;
    auto [tx, berr] = db->Begin();
    ASSERT_FALSE(berr) << berr;
    auto txs = tx->Stmt(stmt);
    ASSERT_FALSE(txs->Exec("Bobby", 7).second);
    ASSERT_FALSE(tx->Commit());
    txs->Close();
    stmt->Close();
    closeDb(*db);
}

TEST(Sql, OpenConnector) {
    static const bool once = [] {
        sql::Register("testctx", make_ptr<fakeDriverCtx>());  // once, also with --gtest_repeat
        return true;
    }();
    (void)once;
    auto [db, err] = sql::Open("testctx", "people");
    ASSERT_FALSE(err);
    auto c = dynamic_pointer_cast<fakeConnector>(testAccess::connector(*db));
    ASSERT_TRUE(c) << "not using *fakeConnector";
    ASSERT_FALSE(db->Close());
    EXPECT_TRUE(c->closed) << "connector is not closed";
}

TEST(Sql, RowsScanProperlyWrapsErrors) {
    static const gopp::error errTestScanWrap = errors::New("errTestScanWrap");
    struct alwaysErrScanner {
        gopp::error Scan(const Value&) { return errTestScanWrap; }
    };
    testDatabase([](sql::DB& db) {
        populate(db, "people");
        auto [rows, err] = db.Query("SELECT|people|age|");
        ASSERT_FALSE(err);
        alwaysErrScanner res;
        while (rows->Next()) {
            gopp::error serr = rows->Scan(&res);
            ASSERT_TRUE(serr) << "expecting back an error";
            EXPECT_TRUE(errors::Is(serr, errTestScanWrap)) << serr;
            EXPECT_TRUE(strings::Contains(serr.Error(), errTestScanWrap.Error()));
        }
    });
}

TEST(Sql, DriverArgsWrapsErrors) {
    static const gopp::error errEmpty = errors::New("empty value");
    struct alwaysErrValuer {
        std::pair<driver::Value, gopp::error> Value() const { return {nullptr, errEmpty}; }
    };
    {
        auto db = newTestDb("people");
        auto [res, err] = db->Exec("INSERT|keys|dec1=?", alwaysErrValuer{});
        ASSERT_TRUE(err) << "expecting back an error";
        EXPECT_TRUE(errors::Is(err, errEmpty)) << err;
        EXPECT_TRUE(strings::Contains(err.Error(), errEmpty.Error()));
        closeDb(*db);
    }
    {
        auto db = newTestDb("people");
        auto [rows, err] = db->Query("INSERT|keys|dec1=?", alwaysErrValuer{});
        ASSERT_TRUE(err) << "expecting back an error";
        EXPECT_TRUE(errors::Is(err, errEmpty)) << err;
        closeDb(*db);
    }
}

TEST(Sql, Issue2542Deadlock) {
    testDatabase([](sql::DB& db) {
        populate(db, "people");
        db.Close();
        for (int i = 0; i < 2; i++) {
            auto [rows, err] = db.Query("SELECT|people|age,name|");
            EXPECT_TRUE(err) << "expected error";
        }
    });
}

TEST(Sql, QueryRowClosingStmt) {
    testDatabase([](sql::DB& db) {
        populate(db, "people");
        string name;
        int age;
        ASSERT_FALSE(db.QueryRow("SELECT|people|age,name|age=?", 3)->Scan(&age, &name));
        auto fcs = testAccess::freeConns(db);
        ASSERT_EQ(fcs.size(), 1u) << "expected 1 free conn";
        auto* fc = getFakeConn(fcs[0]->ci);
        EXPECT_EQ(fc->stmtsMade, fc->stmtsClosed) << "statement close mismatch";
    });
}

TEST(Sql, Issue6651) {
    testDatabase([](sql::DB& db) {
        populate(db, "people");
        string v;
        std::string want = "error in rows.Next";
        rowsCursorNextHook = [&](std::span<Value>) { return errors::New(string(want)); };
        gopp::error err = db.QueryRow("SELECT|people|name|")->Scan(&v);
        EXPECT_EQ(err ? err.Error().str() : "", want);
        rowsCursorNextHook = nullptr;

        want = "error in rows.Close";
        sql::detail::rowsCloseHook = [&](sql::Rows&, gopp::error& e) { e = errors::New(string(want)); };
        err = db.QueryRow("SELECT|people|name|")->Scan(&v);
        EXPECT_EQ(err ? err.Error().str() : "", want);
        sql::detail::rowsCloseHook = nullptr;
    });
}

TEST(Sql, Ping) {
    static const gopp::error pingError = errors::New("Ping failed");
    struct pingConn final : driver::Conn, driver::Pinger {
        bool* fails;
        std::pair<ptr<driver::Stmt>, gopp::error> Prepare(std::string_view) override { return {nullptr, errors::New("x")}; }
        gopp::error Close() override { return nullptr; }
        std::pair<ptr<driver::Tx>, gopp::error> Begin() override { return {nullptr, errors::New("x")}; }
        gopp::error Ping(ptr<context::Context>) override { return *fails ? pingError : nullptr; }
    };
    struct pingDriver final : driver::Driver {
        bool fails = false;
        std::pair<ptr<driver::Conn>, gopp::error> Open(std::string_view) override {
            auto c = make_ptr<pingConn>();
            c->fails = &fails;
            return {c, nullptr};
        }
    };
    static const ptr<pingDriver> drv = [] {
        auto d = make_ptr<pingDriver>();
        sql::Register("ping", d);  // once, also with --gtest_repeat
        return d;
    }();
    drv->fails = false;
    auto [db, err] = sql::Open("ping", "ignored");
    ASSERT_FALSE(err);
    EXPECT_FALSE(db->Ping());
    drv->fails = true;
    EXPECT_EQ(db->Ping(), pingError);
    db->Close();
}

TEST(Sql, NilErrorAfterClose) {
    testDatabase([](sql::DB& db) {
        populate(db, "people");
        // WithCancel matters: Rows watches cancelable contexts on a fiber.
        auto [ctx, cancel] = context::WithCancel(context::Background());
        auto [r, err] = db.QueryContext(ctx, "SELECT|people|name|");
        ASSERT_FALSE(err);
        ASSERT_FALSE(r->Close());
        time::Sleep(10 * time::Millisecond);
        EXPECT_FALSE(r->Err());
        cancel();
    });
}

TEST(Sql, RawBytesReuse) {
    testDatabase([](sql::DB& db) {
        populate(db, "people");
        sql::RawBytes raw;
        // This RawBytes aliases driver-owned memory.
        auto [rows, err] = db.Query("SELECT|people|name|");
        ASSERT_FALSE(err);
        rows->Next();
        rows->Scan(&raw);
        std::string name1(asStringView(raw.span()));
        rows->Close();
        // This one doesn't, and must not write to the memory above.
        std::tie(rows, err) = db.Query("SELECT|people|age|");
        ASSERT_FALSE(err);
        rows->Next();
        rows->Scan(&raw);
        rows->Close();
        // The first query again: nothing changed.
        std::tie(rows, err) = db.Query("SELECT|people|name|");
        ASSERT_FALSE(err);
        rows->Next();
        rows->Scan(&raw);
        std::string name2(asStringView(raw.span()));
        rows->Close();
        EXPECT_EQ(name1, name2);
    });
}

TEST(Sql, UnsupportedOptions) {
    testDatabase([](sql::DB& db) {
        populate(db, "people");
        sql::TxOptions opts{sql::LevelSerializable, true};
        auto [tx, err] = db.BeginTx(context::Background(), &opts);
        EXPECT_TRUE(err) << "expected error when using unsupported options, got nil";
    });
}

TEST(Sql, BogusPreboundParameters) {
    testDatabase([](sql::DB& db) {
        exec(db, "CREATE|t1|name=string,age=int32,dead=bool");
        auto [stmt, err] = db.Prepare("INSERT|t1|name=?,age=bogusconversion");
        ASSERT_TRUE(err) << "expected error";
        EXPECT_EQ(err.Error(), "fakedb: invalid conversion to int32 from \"bogusconversion\"");
    });
}

TEST(Sql, StatsMaxIdleClosedZero) {
    testDatabase([](sql::DB& db) {
        populate(db, "people");
        db.SetMaxOpenConns(1);
        db.SetMaxIdleConns(1);
        db.SetConnMaxLifetime(time::Duration{});
        auto pre = db.Stats();
        for (int i = 0; i < 10; i++) {
            auto [rows, err] = db.Query("SELECT|people|name|");
            ASSERT_FALSE(err);
            rows->Close();
        }
        auto st = db.Stats();
        EXPECT_EQ(st.MaxIdleClosed - pre.MaxIdleClosed, 0);
    });
}

TEST(Sql, StatsMaxIdleClosedTen) {
    testDatabase([](sql::DB& db) {
        populate(db, "people");
        db.SetMaxOpenConns(1);
        db.SetMaxIdleConns(0);
        db.SetConnMaxLifetime(time::Duration{});
        auto pre = db.Stats();
        for (int i = 0; i < 10; i++) {
            auto [rows, err] = db.Query("SELECT|people|name|");
            ASSERT_FALSE(err);
            rows->Close();
        }
        auto st = db.Stats();
        EXPECT_EQ(st.MaxIdleClosed - pre.MaxIdleClosed, 10);
    });
}

TEST(Sql, SimultaneousQueries) {
    testDatabase([](sql::DB& db) {
        populate(db, "people");
        auto [tx, err] = db.Begin();
        ASSERT_FALSE(err);
        auto [r1, err1] = tx->Query("SELECT|people|name|");
        ASSERT_FALSE(err1);
        auto [r2, err2] = tx->Query("SELECT|people|name|");
        ASSERT_FALSE(err2);
        r1->Close();
        r2->Close();
        tx->Rollback();
    });
}

TEST(Sql, ConnRequestSet) {
    sql::detail::connRequestSet s;
    EXPECT_EQ(s.Len(), 0u);
    EXPECT_FALSE(s.TakeRandom());
    // add-delete
    auto dh = s.Add(nullptr);
    EXPECT_EQ(s.Len(), 1u);
    EXPECT_TRUE(s.Delete(dh)) << "failed to delete";
    EXPECT_EQ(s.Len(), 0u);
    EXPECT_FALSE(s.Delete(dh)) << "delete worked twice";
    // take-before-delete
    auto ch1 = make_chan<sql::detail::connRequest>();
    dh = s.Add(ch1);
    auto got = s.TakeRandom();
    EXPECT_TRUE(got && *got == ch1);
    EXPECT_EQ(s.Len(), 0u);
    EXPECT_FALSE(s.Delete(dh)) << "unexpected delete after take";
    // get-take-many
    std::vector<chan<sql::detail::connRequest>> inOrder, backOut;
    for (int i = 0; i < 100; i++) {
        auto c = make_chan<sql::detail::connRequest>();
        s.Add(c);
        inOrder.push_back(c);
    }
    EXPECT_EQ(s.Len(), 100u);
    while (s.Len() > 0) backOut.push_back(*s.TakeRandom());
    EXPECT_NE(inOrder, backOut) << "wasn't random";
    // close-delete
    auto ch = make_chan<sql::detail::connRequest>();
    dh = s.Add(ch);
    s.CloseAndRemoveAll();
    EXPECT_EQ(s.Len(), 0u);
    EXPECT_FALSE(s.Delete(dh)) << "unexpected delete after CloseAndRemoveAll";
}

TEST(Sql, NullTypeScanNil) {
    sql::NullString ns{"preset", true};
    sql::NullInt64 n64{42, true};
    sql::NullInt32 n32{42, true};
    sql::NullFloat64 nf{3.14, true};
    sql::NullBool nb{true, true};
    sql::NullTime nt{time::Now(), true};
    EXPECT_FALSE(ns.Scan(nullptr));
    EXPECT_FALSE(n64.Scan(nullptr));
    EXPECT_FALSE(n32.Scan(nullptr));
    EXPECT_FALSE(nf.Scan(nullptr));
    EXPECT_FALSE(nb.Scan(nullptr));
    EXPECT_FALSE(nt.Scan(nullptr));
    EXPECT_FALSE(ns.Valid || n64.Valid || n32.Valid || nf.Valid || nb.Valid || nt.Valid) << "Scan(nil) left Valid=true";
}

TEST(Sql, NullTypeScanErrorConsistency) {
    // Issue #45662: Valid is false when Scan returns an error.
    auto check = [](auto n, Value in, bool wantErr) {
        gopp::error err = n.Scan(in);
        EXPECT_EQ(static_cast<bool>(err), wantErr) << err;
        EXPECT_EQ(n.Valid, !err) << "Valid must be the opposite of whether Scan failed";
    };
    slice<byte> bad = slice<byte>::of(as_bytes("not_a_number"));
    check(sql::NullInt32{}, bad, true);
    check(sql::NullInt64{}, bad, true);
    check(sql::NullFloat64{}, slice<byte>::of(as_bytes("not_a_float")), true);
    check(sql::NullBool{}, slice<byte>::of(as_bytes("not_a_bool")), true);
    check(sql::NullInt32{}, int64(42), false);
    check(sql::NullInt64{}, int64(42), false);
    check(sql::NullFloat64{}, 3.14, false);
    check(sql::NullBool{}, true, false);
    check(sql::NullString{}, string("hello"), false);
}

// ---------------------------------------------------------------------------
// convert_test.go

namespace {

struct conversionTest {
    Value s;
    std::function<gopp::error()> assign;  // convertAssign into the destination
    std::function<std::string()> check;   // "" if the result is right
    std::string wanterr;
};

}  // namespace

TEST(SqlConvert, Conversions) {
    string scanstr;
    slice<byte> scanbytes;
    sql::RawBytes scanraw;
    int scanint = 0;
    uint8 scanuint8 = 0;
    uint16 scanuint16 = 0;
    bool scanbool = false;
    float scanf32 = 0;
    double scanf64 = 0;
    time::Time scantime;
    std::optional<int64> scanptr;
    Value scaniface;
    time::Time someTime = time::Time::Unix(1000000, 0);

    auto str = [&](std::string want) {
        return conversionTest{nullptr, [&] { return gopp::error(); }, [&, want] { return scanstr == want ? "" : "got " + scanstr.str(); }, ""};
    };
    (void)str;
    std::vector<conversionTest> tests;
    auto toStr = [&](Value s, std::string want, std::string wanterr = "") {
        tests.push_back({s, [&, s] { return sql::convertAssign(&scanstr, s); },
                         [&, want] { return want.empty() || scanstr == want ? "" : "string " + scanstr.str(); }, wanterr});
    };
    auto toBytes = [&](Value s, std::optional<std::string> want) {
        tests.push_back({s, [&, s] { return sql::convertAssign(&scanbytes, s); }, [&, want] {
                             if (!want) return scanbytes.isNil() ? std::string() : "want nil bytes";
                             return asStringView(scanbytes.span()) == *want ? "" : "bytes " + std::string(asStringView(scanbytes.span()));
                         },
                         ""});
    };
    auto toRaw = [&](Value s, std::optional<std::string> want) {
        tests.push_back({s, [&, s] { return sql::convertAssign(&scanraw, s); }, [&, want] {
                             if (!want) return scanraw.isNil() ? std::string() : "want nil RawBytes";
                             return asStringView(scanraw.span()) == *want ? "" : "raw " + std::string(asStringView(scanraw.span()));
                         },
                         ""});
    };
    auto b = [](std::string_view s) { return Value(slice<byte>::of(as_bytes(s))); };

    // Exact conversions (destination pointer type matches source type).
    toStr(string("foo"), "foo");
    tests.push_back({int64(123), [&] { return sql::convertAssign(&scanint, Value(int64(123))); },
                     [&] { return scanint == 123 ? "" : "int"; }, ""});
    tests.push_back({someTime, [&] { return sql::convertAssign(&scantime, Value(someTime)); },
                     [&] { return scantime.Equal(someTime) ? "" : "time"; }, ""});
    // To strings.
    toStr(string("string"), "string");
    toStr(b("byteslice"), "byteslice");
    toStr(int64(123), "123");
    toStr(1.5, "1.5");
    // From time.Time.
    toStr(time::Time::Unix(1, 0).UTC(), "1970-01-01T00:00:01Z");
    toStr(time::Time::Unix(1453874597, 0).In(time::FixedZone("here", -3600 * 8)), "2016-01-26T22:03:17-08:00");
    toStr(time::Time::Unix(1, 2).UTC(), "1970-01-01T00:00:01.000000002Z");
    toBytes(time::Time::Unix(1, 2).UTC(), "1970-01-01T00:00:01.000000002Z");
    // To []byte.
    toBytes(nullptr, std::nullopt);
    toBytes(string("string"), "string");
    toBytes(b("byteslice"), "byteslice");
    toBytes(int64(123), "123");
    toBytes(1.5, "1.5");
    // To RawBytes.
    toRaw(nullptr, std::nullopt);
    toRaw(b("byteslice"), "byteslice");
    toRaw(string("string"), "string");
    toRaw(int64(123), "123");
    toRaw(1.5, "1.5");
    toRaw(time::Time::Unix(2, 5).UTC(), "1970-01-01T00:00:02.000000005Z");
    // Strings to integers.
    auto u8 = [&](Value s, int want, std::string wanterr = "") {
        tests.push_back({s, [&, s] { return sql::convertAssign(&scanuint8, s); },
                         [&, want] { return want == 0 || scanuint8 == want ? "" : "uint8 " + std::to_string(scanuint8); }, wanterr});
    };
    auto u16 = [&](Value s, int want, std::string wanterr = "") {
        tests.push_back({s, [&, s] { return sql::convertAssign(&scanuint16, s); },
                         [&, want] { return want == 0 || scanuint16 == want ? "" : "uint16 " + std::to_string(scanuint16); }, wanterr});
    };
    auto i = [&](Value s, int want, std::string wanterr = "") {
        tests.push_back({s, [&, s] { return sql::convertAssign(&scanint, s); },
                         [&, want] { return want == 0 || scanint == want ? "" : "int " + std::to_string(scanint); }, wanterr});
    };
    u8(string("255"), 255);
    u8(string("256"), 0, "converting driver.Value type string (\"256\") to a uint8: value out of range");
    u16(string("256"), 256);
    i(string("-1"), -1);
    i(string("foo"), 0, "converting driver.Value type string (\"foo\") to a int: invalid syntax");
    // int64 to smaller integers.
    u8(int64(5), 5);
    u8(int64(256), 0, "converting driver.Value type int64 (\"256\") to a uint8: value out of range");
    u16(int64(256), 256);
    u16(int64(65536), 0, "converting driver.Value type int64 (\"65536\") to a uint16: value out of range");
    // Bools.
    auto bo = [&](Value s, bool want, std::string wanterr = "") {
        tests.push_back({s, [&, s] { return sql::convertAssign(&scanbool, s); },
                         [&, want, wanterr] { return !wanterr.empty() || scanbool == want ? "" : "bool"; }, wanterr});
    };
    for (Value v : {Value(true), Value(string("True")), Value(string("TRUE")), Value(string("1")), Value(int64(1))}) bo(v, true);
    for (Value v : {Value(false), Value(string("false")), Value(string("FALSE")), Value(string("0")), Value(int64(0))}) bo(v, false);
    bo(string("yup"), false, "sql/driver: couldn't convert \"yup\" into type bool");
    bo(int64(2), false, "sql/driver: couldn't convert 2 into type bool");
    // Floats.
    auto f64 = [&](Value s, double want) {
        tests.push_back({s, [&, s] { return sql::convertAssign(&scanf64, s); },
                         [&, want] { return scanf64 == want ? "" : "float64"; }, ""});
    };
    auto f32 = [&](Value s, float want) {
        tests.push_back({s, [&, s] { return sql::convertAssign(&scanf32, s); },
                         [&, want] { return scanf32 == want ? "" : "float32"; }, ""});
    };
    f64(1.5, 1.5);
    f64(int64(1), 1);
    f32(1.5, 1.5f);
    f32(string("1.5"), 1.5f);
    f64(string("1.5"), 1.5);
    // Pointers (std::optional).
    tests.push_back({nullptr, [&] { return sql::convertAssign(&scanptr, Value(nullptr)); },
                     [&] { return scanptr ? "want nil" : ""; }, ""});
    tests.push_back({int64(42), [&] { return sql::convertAssign(&scanptr, Value(int64(42))); },
                     [&] { return scanptr && *scanptr == 42 ? "" : "want pointer to 42"; }, ""});
    // To interface{}.
    for (Value v : {Value(1.5), Value(int64(1)), Value(string("str")), b("byteslice"), Value(true), Value(nullptr)}) {
        tests.push_back({v, [&, v] { return sql::convertAssign(&scaniface, v); }, [&, v] {
                             if (!driver::Equal(scaniface, v)) return std::string("interface ") + driver::Format(scaniface).str();
                             // A copy, not the source's bytes.
                             if (auto* sb = std::get_if<slice<byte>>(&v); sb && !sb->empty() &&
                                                                     std::get<slice<byte>>(scaniface).data() == sb->data()) {
                                 return std::string("copy into interface{} didn't copy []byte data");
                             }
                             return std::string();
                         },
                         ""});
    }

    for (std::size_t n = 0; n < tests.size(); n++) {
        auto& ct = tests[n];
        gopp::error err = ct.assign();
        std::string errstr = err ? err.Error().str() : "";
        EXPECT_EQ(errstr, ct.wanterr) << "convertAssign #" << n << " for " << driver::Format(ct.s) << " ("
                                      << driver::TypeName(ct.s) << ")";
        if (ct.wanterr.empty()) {
            EXPECT_EQ(ct.check(), "") << "convertAssign #" << n << " for " << driver::Format(ct.s);
        }
    }
}

TEST(SqlConvert, NullString) {
    sql::NullString ns;
    sql::convertAssign(&ns, Value(slice<byte>::of(as_bytes("foo"))));
    EXPECT_TRUE(ns.Valid) << "expecting not null";
    EXPECT_EQ(ns.String, "foo");
    sql::convertAssign(&ns, Value(nullptr));
    EXPECT_FALSE(ns.Valid) << "expecting null on nil";
    EXPECT_EQ(ns.String, "");
}

TEST(SqlConvert, ValueConverters) {
    // NullString as an argument: its Value.
    auto a = sql::toArg(sql::NullString{"hi", true});
    EXPECT_TRUE(driver::Equal(a.value, string("hi")));
    auto b = sql::toArg(sql::NullString{"", false});
    EXPECT_TRUE(std::holds_alternative<std::nullptr_t>(b.value));
}

TEST(SqlConvert, DriverArgs) {
    // Valuer_V: a value-receiver Valuer.
    struct valuerV {
        string s;
        std::pair<driver::Value, gopp::error> Value() const { return {strings::ToUpper(s), nullptr}; }
    };
    const valuerV* nilValuerPtr = nullptr;
    const std::string* nilStrPtr = nullptr;
    struct test {
        std::vector<sql::detail::arg> args;
        Value want;
    };
    std::vector<test> tests = {
        {sql::detail::args(valuerV{"foo"}), string("FOO")},
        {sql::detail::args(nilValuerPtr), nullptr},
        {sql::detail::args("plain-str"), string("plain-str")},
        {sql::detail::args(nilStrPtr), nullptr},
    };
    struct stubStmt final : driver::Stmt {
        gopp::error Close() override { return nullptr; }
        int NumInput() override { return -1; }
        std::pair<ptr<driver::Result>, gopp::error> Exec(std::span<const Value>) override { return {}; }
        std::pair<ptr<driver::Rows>, gopp::error> Query(std::span<const Value>) override { return {}; }
    };
    for (std::size_t n = 0; n < tests.size(); n++) {
        auto dc = make_ptr<sql::detail::driverConn>();
        auto ds = make_ptr<sql::detail::driverStmt>();
        ds->dc = dc.get();
        ds->si = make_ptr<stubStmt>();
        auto [got, err] = sql::detail::driverArgsConnLocked(nullptr, ds.get(), tests[n].args);
        ASSERT_FALSE(err) << "test[" << n << "]: " << err;
        ASSERT_EQ(got.size(), 1u);
        EXPECT_EQ(got[0].Ordinal, 1);
        EXPECT_TRUE(driver::Equal(got[0].Value, tests[n].want)) << "test[" << n << "]: got " << driver::Format(got[0].Value);
    }
}

// types_test.go (database/sql/driver)
TEST(SqlDriver, ValueConverters) {
    struct test {
        const driver::ValueConverter& c;
        Value in, out;
        std::string err;
    };
    static const driver::NotNull notNullString(driver::String);
    static const driver::Null nullString(driver::String);
    std::vector<test> tests = {
        {driver::Bool, string("true"), true, ""},
        {driver::Bool, string("True"), true, ""},
        {driver::Bool, slice<byte>::of(as_bytes("t")), true, ""},
        {driver::Bool, true, true, ""},
        {driver::Bool, string("1"), true, ""},
        {driver::Bool, int64(1), true, ""},
        {driver::Bool, string("false"), false, ""},
        {driver::Bool, false, false, ""},
        {driver::Bool, string("0"), false, ""},
        {driver::Bool, int64(0), false, ""},
        {driver::Bool, string("foo"), nullptr, "sql/driver: couldn't convert \"foo\" into type bool"},
        {driver::Bool, int64(2), nullptr, "sql/driver: couldn't convert 2 into type bool"},
        {driver::Int32, int64(1), int64(1), ""},
        {driver::Int32, int64(1) << 31, nullptr, "sql/driver: value 2147483648 overflows int32"},
        {driver::Int32, string("1"), int64(1), ""},
        {driver::Int32, string("1.0"), nullptr, "sql/driver: value \"1.0\" can't be converted to int32"},
        {driver::String, int64(1), string("1"), ""},
        {driver::String, string("foo"), string("foo"), ""},
        {driver::String, 1.5, string("1.5"), ""},
        {driver::String, true, string("true"), ""},
        {notNullString, nullptr, nullptr, "nil value not allowed"},
        {nullString, nullptr, nullptr, ""},
        {driver::DefaultParameterConverter, nullptr, nullptr, ""},
        {driver::DefaultParameterConverter, int64(1), int64(1), ""},
        {driver::DefaultParameterConverter, string("foo"), string("foo"), ""},
    };
    for (std::size_t i = 0; i < tests.size(); i++) {
        auto& tt = tests[i];
        auto [out, err] = tt.c.ConvertValue(tt.in);
        EXPECT_EQ(err ? err.Error().str() : "", tt.err) << "test " << i;
        if (tt.err.empty()) {
            EXPECT_TRUE(driver::Equal(out, tt.out)) << "test " << i << ": got " << driver::Format(out);
        }
    }
}
