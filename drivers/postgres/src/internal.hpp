#pragma once

#include <gopp/context.hpp>
#include <gopp/libpq.hpp>

#include <libpq-fe.h>

#include <string>
#include <vector>

namespace gopp::libpq::internal {
namespace driver = sql::driver;

error resultError(PGresult* result);
std::vector<std::string> parameterValues(std::span<const driver::NamedValue> args,
                                         std::vector<const char*>& values);
std::vector<Oid> parameterTypes(std::span<const driver::NamedValue> args);
PGresult* execParams(PGconn* conn, std::string_view query, std::span<const driver::NamedValue> args);

class conn;

class stmt final : public driver::Stmt, public driver::StmtExecContext, public driver::StmtQueryContext {
public:
    stmt(ptr<conn> c, std::string query) : c_(std::move(c)), query_(std::move(query)) {}
    error Close() override { closed_ = true; return nullptr; }
    int NumInput() override { return -1; }
    std::pair<ptr<driver::Result>, error> Exec(std::span<const driver::Value> args) override;
    std::pair<ptr<driver::Rows>, error> Query(std::span<const driver::Value> args) override;
    std::pair<ptr<driver::Result>, error> ExecContext(ptr<context::Context>, std::span<const driver::NamedValue> args) override;
    std::pair<ptr<driver::Rows>, error> QueryContext(ptr<context::Context>, std::span<const driver::NamedValue> args) override;
private:
    ptr<conn> c_;
    std::string query_;
    bool closed_ = false;
};

class conn final : public driver::Conn,
                   public driver::ConnPrepareContext,
                   public driver::ConnBeginTx,
                   public driver::ExecerContext,
                   public driver::QueryerContext,
                   public driver::Pinger {
public:
    explicit conn(PGconn* c) : c_(c) {}
    ~conn() override;
    std::pair<ptr<driver::Stmt>, error> Prepare(std::string_view query) override;
    std::pair<ptr<driver::Stmt>, error> PrepareContext(ptr<context::Context>, std::string_view query) override;
    error Close() override;
    std::pair<ptr<driver::Tx>, error> Begin() override;
    std::pair<ptr<driver::Tx>, error> BeginTx(ptr<context::Context>, driver::TxOptions) override;
    std::pair<ptr<driver::Result>, error> ExecContext(ptr<context::Context>, std::string_view,
                                                       std::span<const driver::NamedValue>) override;
    std::pair<ptr<driver::Rows>, error> QueryContext(ptr<context::Context>, std::string_view,
                                                      std::span<const driver::NamedValue>) override;
    error Ping(ptr<context::Context>) override;
    PGconn* handle() const { return c_; }
private:
    PGconn* c_ = nullptr;
};

class rows final : public driver::Rows, public driver::RowsColumnTypeDatabaseTypeName {
public:
    explicit rows(PGresult* r) : r_(r), n_(PQntuples(r)) {}
    ~rows() override { Close(); }
    std::vector<string> Columns() override;
    error Close() override;
    error Next(std::span<driver::Value> dest) override;
    string ColumnTypeDatabaseTypeName(int index) override;
private:
    PGresult* r_ = nullptr;
    int n_ = 0;
    int row_ = 0;
};

struct result final : driver::Result {
    int64 affected;
    explicit result(int64 n) : affected(n) {}
    std::pair<int64, error> LastInsertId() override;
    std::pair<int64, error> RowsAffected() override { return {affected, nullptr}; }
};

struct tx final : driver::Tx {
    ptr<conn> c;
    bool done = false;
    explicit tx(ptr<conn> conn) : c(std::move(conn)) {}
    error Commit() override;
    error Rollback() override;
};

std::vector<driver::NamedValue> positional(std::span<const driver::Value> args);
}  // namespace gopp::libpq::internal
