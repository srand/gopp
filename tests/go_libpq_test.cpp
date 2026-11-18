#include "go_helper.hpp"

#include <gopp/database/sql.hpp>
#include <gopp/libpq.hpp>
#include <gopp/os.hpp>
#include <gopp/time.hpp>

#include <gtest/gtest.h>

#include <algorithm>

using namespace gopp;

namespace {
std::string postgresDSN() { return std::string(os::Getenv("GOPP_POSTGRES_DSN").view()); }
std::string errString(const error& e) { return e ? std::string(e.Error().view()) : "<nil>"; }

}

TEST(GoLibpq, RegistersAndReportsConnectionErrors) {
    auto drivers = sql::Drivers();
    EXPECT_TRUE(std::find(drivers.begin(), drivers.end(), "postgres") != drivers.end());
    auto [db, err] = sql::Open("postgres", "host='unterminated");
    if (db && !err) { err = db->Ping(); db->Close(); }
    EXPECT_TRUE(err);
    EXPECT_TRUE(db);  // database/sql opens the first connection lazily.
}

TEST(GoLibpq, QueryValuesAndTransactions) {
    if (postgresDSN().empty()) GTEST_SKIP() << "set GOPP_POSTGRES_DSN to run PostgreSQL integration tests";
    auto [db, openErr] = sql::Open("postgres", postgresDSN());
    ASSERT_FALSE(openErr) << "open PostgreSQL: " << errString(openErr);
    struct closeDB { ptr<sql::DB> db; ~closeDB() { db->Close(); } } close{db};
    ASSERT_FALSE(db->Ping()) << "ping PostgreSQL failed";

    int64 number = 0;
    string label;
    bool flag = false;
    double real = 0;
    error err = db->QueryRow("SELECT $1, $2, $3, $4", int64{42}, "go++", true, 3.141592653589793)->Scan(&number, &label, &flag, &real);
    ASSERT_FALSE(err) << errString(err);
    EXPECT_EQ(number, 42);
    EXPECT_EQ(label, "go++");
    EXPECT_TRUE(flag);
    EXPECT_EQ(real, 3.141592653589793);
    time::Time timestamp;
    err = db->QueryRow("SELECT TIMESTAMPTZ '2001-02-03 04:05:06+00'")->Scan(&timestamp);
    ASSERT_FALSE(err) << errString(err);
    EXPECT_EQ(timestamp.Year(), 2001);

    auto [tx, terr] = db->Begin();
    ASSERT_FALSE(terr) << errString(terr);
    ASSERT_FALSE(tx->Exec("CREATE TEMP TABLE gopp_libpq_test (v bigint)").second);
    auto [res, ierr] = tx->Exec("INSERT INTO gopp_libpq_test VALUES ($1)", int64{77});
    ASSERT_FALSE(ierr) << errString(ierr);
    EXPECT_EQ(res->RowsAffected().first, 1);
    int64 got = 0;
    ASSERT_FALSE(tx->QueryRow("SELECT v FROM gopp_libpq_test")->Scan(&got));
    EXPECT_EQ(got, 77);
    ASSERT_FALSE(tx->Rollback());
}

TEST(GoLibpq, SQLStateOnError) {
    if (postgresDSN().empty()) GTEST_SKIP() << "set GOPP_POSTGRES_DSN to run PostgreSQL integration tests";
    auto [db, openErr] = sql::Open("postgres", postgresDSN());
    ASSERT_FALSE(openErr) << "open PostgreSQL: " << errString(openErr);
    struct closeDB { ptr<sql::DB> db; ~closeDB() { db->Close(); } } close{db};
    error err = db->QueryRow("SELECT * FROM gopp_table_that_does_not_exist")->Err();
    ASSERT_TRUE(err);
    auto* pqerr = errors::As<libpq::Error>(err);
    ASSERT_TRUE(pqerr) << errString(err);
    EXPECT_EQ(pqerr->Code, "42P01");
}
