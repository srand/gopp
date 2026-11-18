// A PostgreSQL driver for database/sql, implemented with libpq.
//
//     #include <gopp/database/sql.hpp>
//     #include <gopp/libpq.hpp>  // registers the "postgres" driver
//     auto [db, err] = sql::Open("postgres", "host=localhost dbname=app");
#pragma once

#include <gopp/database/sql.hpp>
#include <gopp/database/sql/driver.hpp>
#include <gopp/errors.hpp>

namespace gopp::libpq {

// Error: an error returned by PostgreSQL, including its SQLSTATE when set.
struct Error_ final : errorInterface {
    string Code;
    explicit Error_(string message, string code = {}) : Code(std::move(code)), msg_(std::move(message)) {}
    string Error() const override { return msg_; }
private:
    string msg_;
};
using Error = Error_;

// PostgreSQLDriver: the driver registered as "postgres".
struct PostgreSQLDriver final : sql::driver::Driver {
    std::pair<ptr<sql::driver::Conn>, error> Open(std::string_view dsn) override;
};

namespace detail {
bool registerDriver() noexcept;
inline const bool registered = registerDriver();
}  // namespace detail

}  // namespace gopp::libpq
