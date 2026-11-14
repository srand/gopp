// Ported from github.com/mattn/go-sqlite3's tests (MIT; see
// drivers/sqlite3/LICENSE): sqlite3_test.go, error_test.go,
// sqlite3_exec_trailing_test.go and sqlite3_context_test.go, with their
// test names.
//
// Left out: the tests of features go++'s driver doesn't have (functions,
// aggregators, collations, hooks, authorizers, file controls, backups,
// extensions, user authentication, the statement cache and
// ConnectHook), and TestBindErrorPaths' unsupported-type cases, which
// driver::Value's types make impossible. The context tests use a long
// recursive query where mattn's register a Go function to control it.
#include "go_helper.hpp"
#include "internal.hpp"

#include <gopp/context.hpp>
#include <gopp/database/sql.hpp>
#include <gopp/fmt.hpp>
#include <gopp/io.hpp>
#include <gopp/net/url.hpp>
#include <gopp/os.hpp>
#include <gopp/runtime.hpp>
#include <gopp/sqlite3.hpp>
#include <gopp/sync.hpp>
#include <gopp/time.hpp>

#include <gtest/gtest.h>

#include <random>
#include <string>
#include <vector>

using namespace gopp;
using sql::driver::Value;
namespace driver = gopp::sql::driver;

namespace {

std::string S(const string& s) { return std::string(s.view()); }
std::string S(const error& e) { return e ? std::string(e.Error().view()) : "<nil>"; }

// TempFilename: a new empty file's name.
std::string TempFilename() {
    auto [f, err] = os::CreateTemp("", "go-sqlite3-test-");
    if (err) ADD_FAILURE() << S(err);
    std::string name(f->Name().view());
    f->Close();
    return name;
}

// Removes the file when it goes out of scope (defer os.Remove).
struct remove_after {
    std::string name;
    ~remove_after() { os::Remove(name); }
};

ptr<sql::DB> mustOpen(const std::string& dsn) {
    auto [db, err] = sql::Open("sqlite3", dsn);
    if (err) ADD_FAILURE() << "Failed to open database: " << S(err);
    return db;
}

struct close_after {
    ptr<sql::DB> db;
    ~close_after() {
        if (db) db->Close();
    }
};

// The open driver connection mattn's tests use directly.
ptr<sqlite3::internal::conn> openConn() {
    sqlite3::SQLiteDriver d;
    auto [c, err] = d.Open(":memory:");
    if (err) ADD_FAILURE() << "open SQLite connection: " << S(err);
    return ptr<sqlite3::internal::conn>(dynamic_cast<sqlite3::internal::conn*>(c.get()));
}

std::string doTestOpen(const std::string& option, error& err) {
    std::string tempFilename = TempFilename();
    remove_after rm{tempFilename};
    auto [db, oerr] = sql::Open("sqlite3", tempFilename + option);
    if (oerr) {
        err = oerr;
        return "Failed to open database:";
    }
    close_after c{db};
    if ((err = db->Ping())) return "ping error:";
    db->Exec("drop table foo");
    if (auto [_, cerr] = db->Exec("create table foo (id integer)"); cerr) {
        err = cerr;
        return "Failed to create table:";
    }
    auto [st, serr] = os::Stat(tempFilename);
    if (serr || st->IsDir()) return "Failed to create ./foo.db";
    return "";
}

}  // namespace

TEST(GoSQLite3, Open) {
    std::pair<const char*, bool> cases[] = {
        {"", true},
        {"?_txlock=immediate", true},
        {"?_txlock=deferred", true},
        {"?_txlock=exclusive", true},
        {"?_txlock=bogus", false},
    };
    for (auto [option, expectedPass] : cases) {
        error err;
        std::string result = doTestOpen(option, err);
        if (result.empty()) {
            ASSERT_TRUE(expectedPass) << "_txlock error not caught at dbOpen with option: " << option;
        } else if (expectedPass) {
            FAIL() << result << " " << S(err);
        }
    }
}

TEST(GoSQLite3, OpenWithVFS) {
    std::string filename = "TestOpenWithVFS.sqlite";
    os::Remove(filename);
    remove_after rm{filename};
    {
        auto db = mustOpen("file:" + filename + "?vfs=hello");
        EXPECT_TRUE(db->Ping()) << "Failed to open: a vfs named hello shouldn't exist";
        db->Close();
    }
    auto db = mustOpen("file:" + filename + "?vfs=unix-none");
    EXPECT_FALSE(db->Ping()) << "Failed to ping";
    db->Close();
}

TEST(GoSQLite3, OpenNoCreate) {
    std::string filename = "TestOpenNoCreate.sqlite";
    os::Remove(filename);
    remove_after rm{filename};

    // https://www.sqlite.org/c3ref/open.html
    // SQLITE_OPEN_READWRITE: the database is opened for reading and writing
    // if possible, or reading only if the file is write protected by the
    // operating system. In either case the database must already exist,
    // otherwise an error is returned.
    auto [db, err] = sql::Open("sqlite3", "file:" + filename + "?mode=rw");
    if (!err) {
        err = db->Ping();
        db->Close();
        ASSERT_TRUE(err) << "expected error from Open or Ping";
    }
    auto* sqlErr = errors::As<sqlite3::Error>(err);
    ASSERT_TRUE(sqlErr) << "expected sqlite3.Error, but got " << S(err);
    EXPECT_EQ(sqlErr->Code, sqlite3::ErrCantOpen) << "expected SQLITE_CANTOPEN, but got " << S(err);
    // make sure database file truly was not created
    EXPECT_TRUE(os::IsNotExist(os::Stat(filename).second)) << "expected database file to not exist";

    // verify that it works if the mode is "rwc" instead
    auto db2 = mustOpen("file:" + filename + "?mode=rwc");
    close_after c{db2};
    ASSERT_FALSE(db2->Ping());
    // make sure database file truly was created
    EXPECT_FALSE(os::Stat(filename).second) << "expected database file to exist";
}

TEST(GoSQLite3, Readonly) {
    std::string tempFilename = TempFilename();
    remove_after rm{tempFilename};
    auto db1 = mustOpen("file:" + tempFilename);
    close_after c1{db1};
    db1->Exec("CREATE TABLE test (x int, y float)");
    auto db2 = mustOpen("file:" + tempFilename + "?mode=ro");
    close_after c2{db2};
    auto [_, err] = db2->Exec("INSERT INTO test VALUES (1, 3.14)");
    EXPECT_TRUE(err) << "didn't expect INSERT into read-only database to work";
}

TEST(GoSQLite3, ForeignKeys) {
    std::pair<const char*, bool> cases[] = {{"?_foreign_keys=1", true}, {"?_foreign_keys=0", false}};
    for (auto [option, want] : cases) {
        std::string fname = TempFilename();
        remove_after rm{fname};
        std::string uri = "file:" + fname + option;
        auto db = mustOpen(uri);
        bool enabled = false;
        error err = db->QueryRow("PRAGMA foreign_keys;")->Scan(&enabled);
        db->Close();
        EXPECT_FALSE(err) << "query foreign_keys for " << uri << ": " << S(err);
        EXPECT_EQ(enabled, want) << "\"PRAGMA foreign_keys;\" for " << uri;
    }
}

TEST(GoSQLite3, DeferredForeignKey) {
    std::string fname = TempFilename();
    remove_after rm{fname};
    auto db = mustOpen("file:" + fname + "?_foreign_keys=1");
    close_after c{db};
    EXPECT_FALSE(db->Exec("CREATE TABLE bar (id INTEGER PRIMARY KEY)").second);
    EXPECT_FALSE(db->Exec("CREATE TABLE foo (bar_id INTEGER, FOREIGN KEY(bar_id) REFERENCES bar(id) DEFERRABLE "
                          "INITIALLY DEFERRED)")
                     .second);
    auto [tx, err] = db->Begin();
    ASSERT_FALSE(err) << "Failed to begin transaction: " << S(err);
    EXPECT_FALSE(tx->Exec("INSERT INTO foo (bar_id) VALUES (123)").second) << "Failed to insert row";
    EXPECT_TRUE(tx->Commit()) << "Expected an error";
    auto [tx2, err2] = db->Begin();
    EXPECT_FALSE(err2) << "Failed to begin transaction: " << S(err2);
    if (tx2) tx2->Rollback();
}

TEST(GoSQLite3, RecursiveTriggers) {
    std::pair<const char*, bool> cases[] = {{"?_recursive_triggers=1", true}, {"?_recursive_triggers=0", false}};
    for (auto [option, want] : cases) {
        std::string fname = TempFilename();
        remove_after rm{fname};
        std::string uri = "file:" + fname + option;
        auto db = mustOpen(uri);
        bool enabled = false;
        error err = db->QueryRow("PRAGMA recursive_triggers;")->Scan(&enabled);
        db->Close();
        EXPECT_FALSE(err) << S(err);
        EXPECT_EQ(enabled, want) << "\"PRAGMA recursive_triggers;\" for " << uri;
    }
}

TEST(GoSQLite3, Close) {
    std::string tempFilename = TempFilename();
    remove_after rm{tempFilename};
    auto db = mustOpen(tempFilename);
    db->Exec("drop table foo");
    ASSERT_FALSE(db->Exec("create table foo (id integer)").second);
    auto [stmt, err] = db->Prepare("select id from foo where id = ?");
    ASSERT_FALSE(err) << "Failed to select records: " << S(err);
    db->Close();
    auto [_, eerr] = stmt->Exec(1);
    EXPECT_TRUE(eerr) << "Failed to operate closed statement";
    stmt->Close();  // Go's GC collects it
}

TEST(GoSQLite3, Insert) {
    std::string tempFilename = TempFilename();
    remove_after rm{tempFilename};
    auto db = mustOpen(tempFilename);
    close_after c{db};
    db->Exec("drop table foo");
    ASSERT_FALSE(db->Exec("create table foo (id integer)").second);
    auto [res, err] = db->Exec("insert into foo(id) values(123)");
    ASSERT_FALSE(err) << "Failed to insert record: " << S(err);
    EXPECT_EQ(res->RowsAffected().first, 1);
    auto [rows, qerr] = db->Query("select id from foo");
    ASSERT_FALSE(qerr) << "Failed to select records: " << S(qerr);
    rows->Next();
    int result = 0;
    rows->Scan(&result);
    rows->Close();
    EXPECT_EQ(result, 123);
}

TEST(GoSQLite3, Upsert) {
    auto [_, n, __] = sqlite3::Version();
    if (n < 3024000) GTEST_SKIP() << "UPSERT requires sqlite3 >= 3.24.0";
    std::string tempFilename = TempFilename();
    remove_after rm{tempFilename};
    auto db = mustOpen(tempFilename);
    close_after c{db};
    db->Exec("drop table foo");
    ASSERT_FALSE(db->Exec("create table foo (name string primary key, counter integer)").second);
    for (int i = 0; i < 10; i++) {
        auto [res, err] =
            db->Exec("insert into foo(name, counter) values('key', 1) on conflict (name) do update set counter=counter+1");
        ASSERT_FALSE(err) << "Failed to upsert record: " << S(err);
        EXPECT_EQ(res->RowsAffected().first, 1);
    }
    auto [rows, err] = db->Query("select name, counter from foo");
    ASSERT_FALSE(err) << S(err);
    rows->Next();
    string resultName;
    int resultCounter = 0;
    rows->Scan(&resultName, &resultCounter);
    rows->Close();
    EXPECT_EQ(S(resultName), "key");
    EXPECT_EQ(resultCounter, 10);
}

TEST(GoSQLite3, Update) {
    std::string tempFilename = TempFilename();
    remove_after rm{tempFilename};
    auto db = mustOpen(tempFilename);
    close_after c{db};
    db->Exec("drop table foo");
    ASSERT_FALSE(db->Exec("create table foo (id integer)").second);
    auto [res, err] = db->Exec("insert into foo(id) values(123)");
    ASSERT_FALSE(err) << S(err);
    int64 expected = res->LastInsertId().first;
    EXPECT_EQ(res->RowsAffected().first, 1);
    auto [res2, err2] = db->Exec("update foo set id = 234");
    ASSERT_FALSE(err2) << S(err2);
    EXPECT_EQ(res2->LastInsertId().first, expected) << "last Id";
    EXPECT_EQ(res2->RowsAffected().first, 1);
    auto [rows, qerr] = db->Query("select id from foo");
    ASSERT_FALSE(qerr);
    rows->Next();
    int result = 0;
    rows->Scan(&result);
    rows->Close();
    EXPECT_EQ(result, 234);
}

TEST(GoSQLite3, Delete) {
    std::string tempFilename = TempFilename();
    remove_after rm{tempFilename};
    auto db = mustOpen(tempFilename);
    close_after c{db};
    db->Exec("drop table foo");
    ASSERT_FALSE(db->Exec("create table foo (id integer)").second);
    auto [res, err] = db->Exec("insert into foo(id) values(123)");
    ASSERT_FALSE(err) << S(err);
    int64 expected = res->LastInsertId().first;
    EXPECT_EQ(res->RowsAffected().first, 1);
    auto [res2, err2] = db->Exec("delete from foo where id = 123");
    ASSERT_FALSE(err2) << S(err2);
    EXPECT_EQ(res2->LastInsertId().first, expected);
    EXPECT_EQ(res2->RowsAffected().first, 1);
    auto [rows, qerr] = db->Query("select id from foo");
    ASSERT_FALSE(qerr);
    EXPECT_FALSE(rows->Next()) << "Fetched row but expected not rows";
    rows->Close();
}

TEST(GoSQLite3, BooleanRoundtrip) {
    std::string tempFilename = TempFilename();
    remove_after rm{tempFilename};
    auto db = mustOpen(tempFilename);
    close_after c{db};
    db->Exec("DROP TABLE foo");
    ASSERT_FALSE(db->Exec("CREATE TABLE foo(id INTEGER, value BOOL)").second);
    ASSERT_FALSE(db->Exec("INSERT INTO foo(id, value) VALUES(1, ?)", true).second);
    ASSERT_FALSE(db->Exec("INSERT INTO foo(id, value) VALUES(2, ?)", false).second);
    auto [rows, err] = db->Query("SELECT id, value FROM foo");
    ASSERT_FALSE(err) << S(err);
    while (rows->Next()) {
        int id = 0;
        bool value = false;
        if (auto serr = rows->Scan(&id, &value)) {
            ADD_FAILURE() << "Unable to scan results: " << S(serr);
            continue;
        }
        if (id == 1) {
            EXPECT_TRUE(value) << "Value for id 1 should be true, not false";
        } else if (id == 2) {
            EXPECT_FALSE(value) << "Value for id 2 should be false, not true";
        }
    }
    rows->Close();
}

namespace {

std::string tz_offset(const time::Time& t) { return S(t.Format("-07:00")); }

}  // namespace

TEST(GoSQLite3, Timestamp) {
    std::string tempFilename = TempFilename();
    remove_after rm{tempFilename};
    auto db = mustOpen(tempFilename);
    close_after c{db};
    db->Exec("DROP TABLE foo");
    ASSERT_FALSE(db->Exec("CREATE TABLE foo(id INTEGER, ts timeSTAMP, dt DATETIME)").second);

    time::Time timestamp1 = time::Date(2012, time::April, 6, 22, 50, 0, 0, time::UTC);
    time::Time timestamp2 = time::Date(2006, time::January, 2, 15, 4, 5, 123456789, time::UTC);
    time::Time timestamp3 = time::Date(2012, time::November, 4, 0, 0, 0, 0, time::UTC);
    const time::Location* tzTest = time::FixedZone("TEST", -9 * 3600 - 13 * 60);
    auto str = [](const char* s) { return Value(string(s)); };
    auto fmt = [](const time::Time& t, const char* layout) { return Value(t.Format(layout)); };
    struct {
        Value value;
        time::Time expected;
    } tests[] = {
        {str("nonsense"), time::Time{}},
        {str("0000-00-00 00:00:00"), time::Time{}},
        {Value(time::Time{}.Unix()), time::Time{}},
        {Value(timestamp1), timestamp1},
        {Value(timestamp2.Unix()), timestamp2.Truncate(time::Second)},
        {Value(timestamp2.UnixNano() / time::Millisecond.ns), timestamp2.Truncate(time::Millisecond)},
        {Value(timestamp1.In(tzTest)), timestamp1.In(tzTest)},
        {fmt(timestamp1, "2006-01-02 15:04:05.000"), timestamp1},
        {fmt(timestamp1, "2006-01-02T15:04:05.000"), timestamp1},
        {fmt(timestamp1, "2006-01-02 15:04:05"), timestamp1},
        {fmt(timestamp1, "2006-01-02T15:04:05"), timestamp1},
        {Value(timestamp2), timestamp2},
        {str("2006-01-02 15:04:05.123456789"), timestamp2},
        {str("2006-01-02T15:04:05.123456789"), timestamp2},
        {str("2006-01-02T05:51:05.123456789-09:13"), timestamp2.In(tzTest)},
        {str("2012-11-04"), timestamp3},
        {str("2012-11-04 00:00"), timestamp3},
        {str("2012-11-04 00:00:00"), timestamp3},
        {str("2012-11-04 00:00:00.000"), timestamp3},
        {str("2012-11-04T00:00"), timestamp3},
        {str("2012-11-04T00:00:00"), timestamp3},
        {str("2012-11-04T00:00:00.000"), timestamp3},
        {str("2006-01-02T15:04:05.123456789Z"), timestamp2},
        {str("2012-11-04Z"), timestamp3},
        {str("2012-11-04 00:00Z"), timestamp3},
        {str("2012-11-04 00:00:00Z"), timestamp3},
        {str("2012-11-04 00:00:00.000Z"), timestamp3},
        {str("2012-11-04T00:00Z"), timestamp3},
        {str("2012-11-04T00:00:00Z"), timestamp3},
        {str("2012-11-04T00:00:00.000Z"), timestamp3},
    };
    constexpr int n = sizeof(tests) / sizeof(tests[0]);
    for (int i = 0; i < n; i++) {
        auto [_, err] = db->Exec("INSERT INTO foo(id, ts, dt) VALUES(?, ?, ?)", i, tests[i].value, tests[i].value);
        ASSERT_FALSE(err) << "Failed to insert timestamp: " << S(err);
    }

    auto [rows, err] = db->Query("SELECT id, ts, dt FROM foo ORDER BY id ASC");
    ASSERT_FALSE(err) << S(err);
    int seen = 0;
    while (rows->Next()) {
        int id = 0;
        time::Time ts, dt;
        if (auto serr = rows->Scan(&id, &ts, &dt)) {
            ADD_FAILURE() << "Unable to scan results: " << S(serr);
            continue;
        }
        if (id < 0 || id >= n) {
            ADD_FAILURE() << "Bad row id: " << id;
            continue;
        }
        seen++;
        EXPECT_TRUE(tests[id].expected.Equal(ts)) << "Timestamp value for id " << id << " should be "
                                                  << S(tests[id].expected.String()) << ", not " << S(ts.String());
        EXPECT_TRUE(tests[id].expected.Equal(dt)) << "Datetime value for id " << id << " should be "
                                                  << S(tests[id].expected.String()) << ", not " << S(dt.String());
        EXPECT_EQ(tz_offset(tests[id].expected), tz_offset(ts)) << "Timezone for id " << id;
        EXPECT_EQ(tz_offset(tests[id].expected), tz_offset(dt)) << "Timezone for id " << id;
    }
    rows->Close();
    EXPECT_EQ(seen, n);
}

TEST(GoSQLite3, Boolean) {
    std::string tempFilename = TempFilename();
    remove_after rm{tempFilename};
    auto db = mustOpen(tempFilename);
    close_after c{db};
    ASSERT_FALSE(db->Exec("CREATE TABLE foo(id INTEGER, fbool BOOLEAN)").second);
    bool bool1 = true;
    ASSERT_FALSE(db->Exec("INSERT INTO foo(id, fbool) VALUES(1, ?)", bool1).second);
    bool bool2 = false;
    ASSERT_FALSE(db->Exec("INSERT INTO foo(id, fbool) VALUES(2, ?)", bool2).second);
    ASSERT_FALSE(db->Exec("INSERT INTO foo(id, fbool) VALUES(3, ?)", "nonsense").second);

    for (bool want : {bool1, bool2}) {
        auto [rows, err] = db->Query("SELECT id, fbool FROM foo where fbool = ?", want);
        ASSERT_FALSE(err) << S(err);
        int counter = 0, id = 0;
        bool fbool = !want;
        while (rows->Next()) {
            ASSERT_FALSE(rows->Scan(&id, &fbool));
            counter++;
        }
        rows->Close();
        EXPECT_EQ(counter, 1);
        EXPECT_EQ(fbool, want);
    }

    auto [rows, err] = db->Query("SELECT id, fbool FROM foo where id=?;", 3);
    ASSERT_FALSE(err) << S(err);
    rows->Next();
    int id = 0;
    bool fbool = false;
    EXPECT_TRUE(rows->Scan(&id, &fbool)) << "Expected error from \"nonsense\" bool";
    rows->Close();
}

TEST(GoSQLite3, Float32) {
    std::string tempFilename = TempFilename();
    remove_after rm{tempFilename};
    auto db = mustOpen(tempFilename);
    close_after c{db};
    ASSERT_FALSE(db->Exec("CREATE TABLE foo(id INTEGER)").second);
    ASSERT_FALSE(db->Exec("INSERT INTO foo(id) VALUES(null)").second);
    auto [rows, err] = db->Query("SELECT id FROM foo");
    ASSERT_FALSE(err);
    ASSERT_TRUE(rows->Next());
    Value id = int64(1);
    ASSERT_FALSE(rows->Scan(&id));
    rows->Close();
    EXPECT_TRUE(std::holds_alternative<std::nullptr_t>(id)) << "Expected nil but not";
}

TEST(GoSQLite3, Null) {
    std::string tempFilename = TempFilename();
    remove_after rm{tempFilename};
    auto db = mustOpen(tempFilename);
    close_after c{db};
    auto [rows, err] = db->Query("SELECT 3.141592");
    ASSERT_FALSE(err);
    ASSERT_TRUE(rows->Next());
    Value v;
    ASSERT_FALSE(rows->Scan(&v));
    rows->Close();
    ASSERT_TRUE(std::holds_alternative<double>(v)) << "Expected float but not";
    EXPECT_EQ(std::get<double>(v), 3.141592);
}

TEST(GoSQLite3, Transaction) {
    std::string tempFilename = TempFilename();
    remove_after rm{tempFilename};
    auto db = mustOpen(tempFilename);
    close_after c{db};
    ASSERT_FALSE(db->Exec("CREATE TABLE foo(id INTEGER)").second);

    auto [tx, err] = db->Begin();
    ASSERT_FALSE(err) << S(err);
    ASSERT_FALSE(tx->Exec("INSERT INTO foo(id) VALUES(1)").second);
    auto [rows, qerr] = tx->Query("SELECT id from foo");
    ASSERT_FALSE(qerr) << S(qerr);
    ASSERT_FALSE(tx->Rollback());
    EXPECT_FALSE(rows->Next()) << "Unable to query results";

    auto [tx2, err2] = db->Begin();
    ASSERT_FALSE(err2);
    ASSERT_FALSE(tx2->Exec("INSERT INTO foo(id) VALUES(1)").second);
    ASSERT_FALSE(tx2->Commit());
    auto [rows2, qerr2] = tx2->Query("SELECT id from foo");
    EXPECT_TRUE(qerr2) << "Expected failure to query";
}

TEST(GoSQLite3, WAL) {
    std::string tempFilename = TempFilename();
    remove_after rm{tempFilename};
    auto db = mustOpen(tempFilename);
    close_after c{db};
    ASSERT_FALSE(db->Exec("PRAGMA journal_mode=WAL;").second);
    ASSERT_FALSE(db->Exec("PRAGMA locking_mode=EXCLUSIVE;").second);
    ASSERT_FALSE(db->Exec("CREATE TABLE test (id SERIAL, user TEXT NOT NULL, name TEXT NOT NULL);").second);
    ASSERT_FALSE(db->Exec("INSERT INTO test (user, name) VALUES ('user','name');").second);

    // Exclusive mode, so that a fresh connection can't read.
    auto [trans, err] = db->Begin();
    ASSERT_FALSE(err) << S(err);
    auto [s, perr] = trans->Prepare("INSERT INTO test (user, name) VALUES (?, ?);");
    ASSERT_FALSE(perr) << S(perr);
    int count = 0;
    ASSERT_FALSE(trans->QueryRow("SELECT count(user) FROM test;")->Scan(&count));
    ASSERT_FALSE(s->Exec("bbbb", "aaaa").second);
    ASSERT_FALSE(s->Close());
    ASSERT_FALSE(trans->Commit());
}

TEST(GoSQLite3, TimezoneConversion) {
    const char* zones[] = {"UTC", "US/Central", "US/Pacific", "Local"};
    for (const char* tz : zones) {
        auto [loc, lerr] = time::LoadLocation(tz);
        if (lerr) {
            // Systems that split off tzdata-legacy lack the US/ links.
            std::string_view z = tz;
            std::tie(loc, lerr) = time::LoadLocation(z == "US/Central" ? "America/Chicago" : "America/Los_Angeles");
            ASSERT_FALSE(lerr) << S(lerr);
            tz = z == "US/Central" ? "America/Chicago" : "America/Los_Angeles";
        }
        std::string tempFilename = TempFilename();
        remove_after rm{tempFilename};
        auto db = mustOpen(tempFilename + "?_loc=" + S(url::QueryEscape(tz)));
        close_after c{db};
        db->Exec("DROP TABLE foo");
        ASSERT_FALSE(db->Exec("CREATE TABLE foo(id INTEGER, ts TIMESTAMP, dt DATETIME)").second);

        time::Time timestamp1 = time::Date(2012, time::April, 6, 22, 50, 0, 0, time::UTC);
        time::Time timestamp2 = time::Date(2006, time::January, 2, 15, 4, 5, 123456789, time::UTC);
        time::Time timestamp3 = time::Date(2012, time::November, 4, 0, 0, 0, 0, time::UTC);
        auto str = [](const char* s) { return Value(string(s)); };
        auto fmt = [](const time::Time& t, const char* layout) { return Value(t.Format(layout)); };
        struct {
            Value value;
            time::Time expected;
        } tests[] = {
            {str("nonsense"), time::Time{}.In(loc)},
            {str("0000-00-00 00:00:00"), time::Time{}.In(loc)},
            {Value(timestamp1), timestamp1.In(loc)},
            {Value(timestamp1.Unix()), timestamp1.In(loc)},
            {Value(timestamp1.In(time::FixedZone("TEST", -7 * 3600))), timestamp1.In(loc)},
            {fmt(timestamp1, "2006-01-02 15:04:05.000"), timestamp1.In(loc)},
            {fmt(timestamp1, "2006-01-02T15:04:05.000"), timestamp1.In(loc)},
            {fmt(timestamp1, "2006-01-02 15:04:05"), timestamp1.In(loc)},
            {fmt(timestamp1, "2006-01-02T15:04:05"), timestamp1.In(loc)},
            {Value(timestamp2), timestamp2.In(loc)},
            {str("2006-01-02 15:04:05.123456789"), timestamp2.In(loc)},
            {str("2006-01-02T15:04:05.123456789"), timestamp2.In(loc)},
            {str("2012-11-04"), timestamp3.In(loc)},
            {str("2012-11-04 00:00"), timestamp3.In(loc)},
            {str("2012-11-04 00:00:00"), timestamp3.In(loc)},
            {str("2012-11-04 00:00:00.000"), timestamp3.In(loc)},
            {str("2012-11-04T00:00"), timestamp3.In(loc)},
            {str("2012-11-04T00:00:00"), timestamp3.In(loc)},
            {str("2012-11-04T00:00:00.000"), timestamp3.In(loc)},
        };
        constexpr int n = sizeof(tests) / sizeof(tests[0]);
        for (int i = 0; i < n; i++) {
            ASSERT_FALSE(db->Exec("INSERT INTO foo(id, ts, dt) VALUES(?, ?, ?)", i, tests[i].value, tests[i].value).second);
        }
        auto [rows, err] = db->Query("SELECT id, ts, dt FROM foo ORDER BY id ASC");
        ASSERT_FALSE(err) << S(err);
        int seen = 0;
        while (rows->Next()) {
            int id = 0;
            time::Time ts, dt;
            if (auto serr = rows->Scan(&id, &ts, &dt)) {
                ADD_FAILURE() << "Unable to scan results: " << S(serr);
                continue;
            }
            if (id < 0 || id >= n) continue;
            seen++;
            EXPECT_TRUE(tests[id].expected.Equal(ts)) << tz << " id " << id << ": " << S(ts.String());
            EXPECT_TRUE(tests[id].expected.Equal(dt)) << tz << " id " << id << ": " << S(dt.String());
            EXPECT_EQ(S(tests[id].expected.Location()->String()), S(ts.Location()->String())) << tz << " id " << id;
            EXPECT_EQ(S(tests[id].expected.Location()->String()), S(dt.Location()->String())) << tz << " id " << id;
        }
        rows->Close();
        EXPECT_EQ(seen, n) << tz;
    }
}

TEST(GoSQLite3, Execer) {
    std::string tempFilename = TempFilename();
    remove_after rm{tempFilename};
    auto db = mustOpen(tempFilename);
    close_after c{db};
    auto [_, err] = db->Exec(R"(
       create table foo (id integer); -- one comment
       insert into foo(id) values(?);
       insert into foo(id) values(?);
       insert into foo(id) values(?); -- another comment
       )",
                             1, 2, 3);
    EXPECT_FALSE(err) << "Failed to call db.Exec: " << S(err);
}

TEST(GoSQLite3, Queryer) {
    std::string tempFilename = TempFilename();
    remove_after rm{tempFilename};
    auto db = mustOpen(tempFilename);
    close_after c{db};
    EXPECT_FALSE(db->Exec("\n\t\tcreate table foo (id integer);\n\t").second);
    EXPECT_FALSE(db->Exec(R"(
		insert into foo(id) values(?);
		insert into foo(id) values(?);
		insert into foo(id) values(?);
	)",
                          3, 2, 1)
                     .second);
    auto [rows, err] = db->Query("\n\t\tselect id from foo order by id;\n\t");
    ASSERT_FALSE(err) << S(err);
    int n = 0;
    while (rows->Next()) {
        int id = 0;
        EXPECT_FALSE(rows->Scan(&id));
        EXPECT_EQ(id, n + 1) << "Failed to db.Query: not matched results";
        n++;
    }
    EXPECT_FALSE(rows->Err()) << "Post-scan failed: " << S(rows->Err());
    rows->Close();
    EXPECT_EQ(n, 3);
}

TEST(GoSQLite3, Stress) {
    std::string tempFilename = TempFilename();
    remove_after rm{tempFilename};
    {
        auto db = mustOpen(tempFilename);
        db->Exec("CREATE TABLE foo (id int);");
        db->Exec("INSERT INTO foo VALUES(1);");
        db->Exec("INSERT INTO foo VALUES(2);");
        db->Close();
    }
    for (int i = 0; i < 1000; i++) {  // mattn's 10000, a tenth here
        auto db = mustOpen(tempFilename);
        for (int j = 0; j < 3; j++) {
            auto [rows, err] = db->Query("select * from foo where id=1;");
            ASSERT_FALSE(err) << "Failed to call db.Query: " << S(err);
            while (rows->Next()) {
                int v = 0;
                EXPECT_FALSE(rows->Scan(&v));
            }
            EXPECT_FALSE(rows->Err());
            rows->Close();
        }
        db->Close();
    }
}

TEST(GoSQLite3, DateTimeLocal) {
    const char* zone = "Asia/Tokyo";
    std::string tempFilename = TempFilename();
    remove_after rm{tempFilename};
    {
        auto db = mustOpen(tempFilename + "?_loc=" + zone);
        db->Exec("CREATE TABLE foo (dt datetime);");
        db->Exec("INSERT INTO foo VALUES('2015-03-05 15:16:17');");
        time::Time d;
        ASSERT_FALSE(db->QueryRow("select * from foo")->Scan(&d));
        EXPECT_FALSE(d.Hour() == 15 || S(d.String()).find("JST") == std::string::npos)
            << "Result should have timezone " << S(d.String());
        db->Close();
    }
    {
        auto db = mustOpen(tempFilename);
        time::Time d;
        ASSERT_FALSE(db->QueryRow("select * from foo")->Scan(&d));
        EXPECT_FALSE(d.UTC().Hour() != 15 || S(d.String()).find("UTC") == std::string::npos)
            << "Result should not have timezone " << zone << " " << S(d.String());
        ASSERT_FALSE(db->Exec("DELETE FROM foo").second);
        auto [dt, err] = time::Parse("2006/1/2 15/4/5 -0700 MST", "2015/3/5 15/16/17 +0900 JST");
        ASSERT_FALSE(err) << S(err);
        db->Exec("INSERT INTO foo VALUES(?);", dt);
        db->Close();
    }
    auto db = mustOpen(tempFilename + "?_loc=" + zone);
    close_after c{db};
    time::Time d;
    ASSERT_FALSE(db->QueryRow("select * from foo")->Scan(&d));
    EXPECT_FALSE(d.Hour() != 15 || S(d.String()).find("JST") == std::string::npos)
        << "Result should have timezone " << zone << " " << S(d.String());
}

TEST(GoSQLite3, Version) {
    auto [s, n, id] = sqlite3::Version();
    EXPECT_FALSE(s.empty() || n == 0 || id.empty()) << "Version failed " << S(s) << ", " << n << ", " << S(id);
}

TEST(GoSQLite3, StringContainingZero) {
    std::string tempFilename = TempFilename();
    remove_after rm{tempFilename};
    auto db = mustOpen(tempFilename);
    close_after c{db};
    EXPECT_FALSE(db->Exec("\n\tcreate table foo (id integer, name, extra text);\n\t").second);
    const string text(std::string_view("foo\0bar", 7));
    EXPECT_FALSE(db->Exec("insert into foo(id, name, extra) values($1, $2, $2)", 1, text).second);
    auto row = db->QueryRow("select id, extra from foo where id = $1 and extra = $2", 1, text);
    int id = 0;
    string extra;
    EXPECT_FALSE(row->Scan(&id, &extra));
    EXPECT_TRUE(id == 1 && extra.view() == text.view()) << "Failed to db.QueryRow: not matched results";
}

namespace {

constexpr std::string_view CurrentTimeStamp = "2006-01-02 15:04:05";

// mattn's TimeStamp: a Scanner and Valuer of the CURRENT_TIMESTAMP format.
struct TimeStamp {
    time::Time* Time;
    error Scan(const Value& value) const {
        if (auto* s = std::get_if<string>(&value)) {
            auto [t, err] = time::Parse(CurrentTimeStamp, s->view());
            *Time = t;
            return err;
        }
        if (auto* b = std::get_if<slice<byte>>(&value)) {
            auto [t, err] = time::Parse(CurrentTimeStamp, std::string_view(reinterpret_cast<const char*>(b->data()), b->size()));
            *Time = t;
            return err;
        }
        return errors::New("invalid type for current_timestamp");
    }
};

}  // namespace

TEST(GoSQLite3, DateTimeNow) {
    std::string tempFilename = TempFilename();
    remove_after rm{tempFilename};
    auto db = mustOpen(tempFilename);
    close_after c{db};
    time::Time d;
    TimeStamp ts{&d};
    EXPECT_FALSE(db->QueryRow("SELECT datetime('now')")->Scan(&ts)) << "Failed to scan datetime";
}

TEST(GoSQLite3, BindErrorPaths) {
    auto c = openConn();
    ASSERT_FALSE(c->exec(context::Background(), "CREATE TABLE t (v)", {}).second);
    // Binding a positional argument at an index past the statement's
    // parameters surfaces SQLite's error (SQLITE_RANGE), not a type error.
    auto [stmt, err] = c->prepare("INSERT INTO t VALUES (?)");
    ASSERT_FALSE(err) << S(err);
    std::vector<driver::NamedValue> args = {{.Name = {}, .Ordinal = 2, .Value = int64(1)}};
    error berr = stmt->bind(args);
    auto* serr = errors::As<sqlite3::Error>(berr);
    EXPECT_TRUE(serr && serr->Code == sqlite3::ErrRange) << "out-of-range bind: got " << S(berr) << ", want SQLITE_RANGE error";
    stmt->Close();
    c->Close();
}

TEST(GoSQLite3, DeclTypes) {
    auto c = openConn();
    ASSERT_FALSE(c->exec(context::Background(), "create table foo (id integer not null primary key, name text)", {}).second);
    ASSERT_FALSE(c->exec(context::Background(), "insert into foo(name) values('bar')", {}).second);
    auto [rs, err] = c->query(context::Background(), "select * from foo", {});
    ASSERT_FALSE(err) << S(err);
    auto declTypes = dynamic_cast<sqlite3::internal::rows*>(rs.get())->DeclTypes();
    EXPECT_EQ(declTypes, (std::vector<std::string>{"integer", "text"}));
    rs->Close();
    c->Close();
}

TEST(GoSQLite3, Pinger) {
    auto db = mustOpen(":memory:");
    ASSERT_FALSE(db->Ping());
    db->Close();
    EXPECT_TRUE(db->Ping()) << "Should be closed";
}

TEST(GoSQLite3, NonColumnString) {
    auto db = mustOpen(":memory:");
    close_after c{db};
    Value x;
    ASSERT_FALSE(db->QueryRow("SELECT 'hello'")->Scan(&x));
    ASSERT_TRUE(std::holds_alternative<string>(x)) << "non-column string must return string but got "
                                                   << driver::TypeName(x);
    EXPECT_EQ(S(std::get<string>(x)), "hello");
}

TEST(GoSQLite3, NilAndEmptyBytes) {
    auto db = mustOpen(":memory:");
    close_after c{db};
    struct {
        std::string name;
        std::string columnType;
        bool actualNil;  // insert nil itself, not a nil []byte
        slice<byte> insertBytes;
        bool expectNil;
    } tests[] = {
        {"actual nil blob", "blob", true, slice<byte>{}, true},
        {"referenced nil blob", "blob", false, slice<byte>{}, true},
        {"empty blob", "blob", false, slice<byte>::make(0), false},
        {"actual nil text", "text", true, slice<byte>{}, true},
        {"referenced nil text", "text", false, slice<byte>{}, true},
        {"empty text", "text", false, slice<byte>::make(0), false},
    };
    int tsti = 0;
    for (const auto& tst : tests) {
        ASSERT_FALSE(db->Exec(fmt::Sprint("create table tbl{} (txt {})", tsti, tst.columnType).str()).second) << tst.name;
        std::string ins = fmt::Sprint("insert into tbl{} (txt) values (?)", tsti).str();
        if (tst.actualNil) {
            ASSERT_FALSE(db->Exec(ins, nullptr).second) << tst.name;
        } else {
            ASSERT_FALSE(db->Exec(ins, tst.insertBytes).second) << tst.name;
        }
        auto [rows, err] = db->Query(fmt::Sprint("select txt from tbl{}", tsti).str());
        ASSERT_FALSE(err) << tst.name;
        ASSERT_TRUE(rows->Next()) << tst.name << " no rows";
        slice<byte> scanBytes = slice<byte>::make(1);
        ASSERT_FALSE(rows->Scan(&scanBytes)) << tst.name;
        ASSERT_FALSE(rows->Err()) << tst.name;
        rows->Close();
        if (tst.expectNil) {
            EXPECT_TRUE(scanBytes.isNil()) << tst.name;
        } else {
            EXPECT_TRUE(!scanBytes.isNil() && scanBytes.empty()) << tst.name;
        }
        tsti++;
    }
}

TEST(GoSQLite3, InsertNilByteSlice) {
    auto db = mustOpen(":memory:");
    close_after c{db};
    ASSERT_FALSE(db->Exec("create table blob_not_null (b blob not null)").second);
    slice<byte> nilSlice;
    EXPECT_TRUE(db->Exec("insert into blob_not_null (b) values (?)", nilSlice).second)
        << "didn't expect INSERT to 'not null' column with a nil []byte slice to work";
    slice<byte> zeroLenSlice = slice<byte>::make(0);
    EXPECT_FALSE(db->Exec("insert into blob_not_null (b) values (?)", zeroLenSlice).second)
        << "failed to insert zero-length slice";
}

TEST(GoSQLite3, NamedParam) {
    std::string tempFilename = TempFilename();
    remove_after rm{tempFilename};
    auto db = mustOpen(tempFilename);
    close_after c{db};
    db->Exec("drop table foo");
    ASSERT_FALSE(db->Exec("create table foo (id integer, name text, amount integer)").second);
    auto [_, err] = db->Exec("insert into foo(id, name, amount) values(:id, @name, $amount)", sql::Named("bar", 42),
                             sql::Named("baz", "quux"), sql::Named("amount", 123), sql::Named("corge", "waldo"),
                             sql::Named("id", 2), sql::Named("name", "grault"));
    ASSERT_FALSE(err) << "Failed to insert record with named parameters: " << S(err);
    auto [rows, qerr] = db->Query("select id, name, amount from foo");
    ASSERT_FALSE(qerr);
    rows->Next();
    int id = 0, amount = 0;
    string name;
    rows->Scan(&id, &name, &amount);
    rows->Close();
    EXPECT_TRUE(id == 2 && name.view() == "grault" && amount == 123) << id << " " << S(name) << " " << amount;
}

TEST(GoSQLite3, NamedParamClearBindings) {
    std::string tempFilename = TempFilename();
    remove_after rm{tempFilename};
    auto db = mustOpen(tempFilename);
    close_after c{db};
    ASSERT_FALSE(db->Exec("create table foo (x integer, y integer, z text)").second);
    ASSERT_FALSE(db->Exec("insert into foo(x, y, z) values($x, $y, $z)", sql::Named("x", 1), sql::Named("y", 2),
                          sql::Named("z", "three"))
                     .second);
    ASSERT_FALSE(db->Exec("insert into foo(x, y, z) values($x, $y, $z)", sql::Named("x", 10), sql::Named("y", nullptr),
                          sql::Named("z", nullptr))
                     .second);
    int x = 0;
    sql::NullInt64 y, z;
    ASSERT_FALSE(db->QueryRow("select x, y, z from foo where x = 10")->Scan(&x, &y, &z));
    EXPECT_FALSE(y.Valid) << "Expected y to be NULL, got " << y.Int64;
    EXPECT_FALSE(z.Valid) << "Expected z to be NULL, got " << z.Int64;
}

TEST(GoSQLite3, NotEnoughArgsErrorMessage) {
    auto db = mustOpen(":memory:");
    close_after c{db};
    const std::string want = "not enough args to execute query: want 1 got 0";
    // exec
    EXPECT_EQ(S(db->Exec("SELECT ?; SELECT ?", "hello").second), want);
    // exec without args
    EXPECT_EQ(S(db->Exec("SELECT ?").second), want);
    // query
    EXPECT_EQ(S(db->Query("SELECT ?; SELECT ?", "hello").second), want);
}

TEST(GoSQLite3, ExecWithoutArgsStopsAtMissingArgs) {
    auto db = mustOpen(":memory:");
    close_after c{db};
    db->SetMaxOpenConns(1);
    ASSERT_FALSE(db->Exec("CREATE TABLE t (x INTEGER)").second);
    EXPECT_TRUE(db->Exec("INSERT INTO t VALUES (7); INSERT INTO t VALUES (?); INSERT INTO t VALUES (9)").second);
    string rows;
    ASSERT_FALSE(db->QueryRow("SELECT group_concat(coalesce(x, 'NULL')) FROM t")->Scan(&rows));
    EXPECT_EQ(S(rows), "7") << "want only the statement before the missing argument";
}

TEST(GoSQLite3, QueryCommentOnly) {
    auto db = mustOpen(":memory:");
    close_after c{db};
    const char* cases[] = {"", "   ", "-- comment", "---- comment\n", "/* block */"};
    for (const char* q : cases) {
        int x = 0;
        EXPECT_EQ(db->QueryRow(q)->Scan(&x), sql::ErrNoRows) << "QueryRow(" << q << ")";
        auto [rows, err] = db->Query(q);
        if (err) {
            ADD_FAILURE() << "Query(" << q << "): unexpected error: " << S(err);
            continue;
        }
        EXPECT_FALSE(rows->Next()) << "Query(" << q << "): expected no rows";
        rows->Close();
        EXPECT_FALSE(db->Exec(q).second) << "Exec(" << q << ")";
    }
}

// ---------------------------------------------------------------------------
// TestSuite

namespace {

class GoSQLite3Suite : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        tempFilename = TempFilename();
        auto [d, err] = sql::Open("sqlite3", tempFilename + "?_busy_timeout=99999");
        ASSERT_FALSE(err) << S(err);
        db = d;
        // The pool's connection, made here, not inside the first test, and
        // only one kept idle after tests that use several at once (for the
        // leak check, which counts idle connections a test leaves).
        db->SetMaxIdleConns(1);
        ASSERT_FALSE(db->Ping());
    }
    static void TearDownTestSuite() {
        db->Close();
        db = nullptr;
        os::Remove(tempFilename);
    }
    void SetUp() override { tearDown(); }

    static ptr<sql::Result> mustExec(std::string_view q, auto&&... args) {
        auto [res, err] = db->Exec(q, args...);
        if (err) ADD_FAILURE() << "Error running " << q << ": " << S(err);
        return res;
    }
    static void tearDown() {
        for (const char* tbl : {"foo", "bar", "t", "bench"}) mustExec(std::string("drop table if exists ") + tbl);
    }

    static inline ptr<sql::DB> db;
    static inline std::string tempFilename;
};

std::string hex(const slice<byte>& b) {
    std::string s;
    for (byte c : b) s += std::format("{:02x}", c);
    return s;
}

slice<byte> bytes_of(std::initializer_list<int> v) {
    slice<byte> b;
    for (int c : v) b = append(std::move(b), static_cast<byte>(c));
    return b;
}

}  // namespace

TEST_F(GoSQLite3Suite, Result) {
    mustExec("create temporary table test (id integer primary key autoincrement, name varchar(10))");
    for (int i = 1; i < 3; i++) {
        auto r = mustExec("insert into test (name) values (?)", fmt::Sprint("row {}", i));
        auto [n, err] = r->RowsAffected();
        ASSERT_FALSE(err);
        EXPECT_EQ(n, 1);
        auto [id, err2] = r->LastInsertId();
        ASSERT_FALSE(err2);
        EXPECT_EQ(id, i);
    }
    EXPECT_TRUE(db->Exec("error!").second) << "expected error";
}

TEST_F(GoSQLite3Suite, Blobs) {
    slice<byte> blob = bytes_of({0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15});
    mustExec("create table foo (id integer primary key, bar blob[16])");
    mustExec("insert into foo (id, bar) values(?,?)", 0, blob);

    std::string want = hex(blob);
    slice<byte> b = slice<byte>::make(16);
    error err = db->QueryRow("select bar from foo where id = ?", 0)->Scan(&b);
    EXPECT_FALSE(err) << "[]byte scan: " << S(err);
    EXPECT_EQ(hex(b), want);

    string got;
    err = db->QueryRow("select bar from foo where id = ?", 0)->Scan(&got);
    EXPECT_FALSE(err) << "string scan: " << S(err);
    EXPECT_EQ(got.view(), std::string_view(reinterpret_cast<const char*>(blob.data()), blob.size()));
}

TEST_F(GoSQLite3Suite, MultiBlobs) {
    mustExec("create table foo (id integer primary key, bar blob[16])");
    slice<byte> blob0 = bytes_of({0, 1, 2, 3, 4, 5, 6, 7});
    mustExec("insert into foo (id, bar) values(?,?)", 0, blob0);
    slice<byte> blob1 = bytes_of({0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15});
    mustExec("insert into foo (id, bar) values(?,?)", 1, blob1);

    auto [r, err] = db->Query("select bar from foo order by id");
    ASSERT_FALSE(err) << S(err);
    ASSERT_TRUE(r->Next()) << "expected one rows";
    slice<byte> b0 = slice<byte>::make(8);
    ASSERT_FALSE(r->Scan(&b0));
    ASSERT_TRUE(r->Next()) << "expected one rows";
    slice<byte> b1 = slice<byte>::make(16);
    ASSERT_FALSE(r->Scan(&b1));
    r->Close();
    EXPECT_EQ(hex(b0), hex(blob0));
    EXPECT_EQ(hex(b1), hex(blob1));
}

TEST_F(GoSQLite3Suite, NullZeroLengthBlobs) {
    mustExec("create table foo (id integer primary key, bar blob[16])");
    mustExec("insert into foo (id, bar) values(?,?)", 0, nullptr);
    mustExec("insert into foo (id, bar) values(?,?)", 1, slice<byte>::make(0));

    slice<byte> b0 = slice<byte>::make(1);
    ASSERT_FALSE(db->QueryRow("select bar from foo where id=0")->Scan(&b0));
    EXPECT_TRUE(b0.isNil()) << "for id=0, got " << hex(b0) << "; want nil";

    slice<byte> b1;
    ASSERT_FALSE(db->QueryRow("select bar from foo where id=1")->Scan(&b1));
    EXPECT_FALSE(b1.isNil()) << "for id=1, got nil; want zero-length slice";
    EXPECT_TRUE(b1.empty()) << "for id=1, got " << hex(b1) << "; want zero-length slice";
}

TEST_F(GoSQLite3Suite, ManyQueryRow) {
    mustExec("create table foo (id integer primary key, name varchar(50))");
    mustExec("insert into foo (id, name) values(?,?)", 1, "bob");
    string name;
    for (int i = 0; i < 10000; i++) {
        error err = db->QueryRow("select name from foo where id = ?", 1)->Scan(&name);
        ASSERT_TRUE(!err && name.view() == "bob") << "on query " << i << ": err=" << S(err) << ", name=" << S(name);
    }
}

TEST_F(GoSQLite3Suite, TxQuery) {
    auto [tx, err] = db->Begin();
    ASSERT_FALSE(err) << S(err);
    ASSERT_FALSE(tx->Exec("create table foo (id integer primary key, name varchar(50))").second);
    ASSERT_FALSE(tx->Exec("insert into foo (id, name) values(?,?)", 1, "bob").second);
    auto [r, qerr] = tx->Query("select name from foo where id = ?", 1);
    ASSERT_FALSE(qerr) << S(qerr);
    ASSERT_TRUE(r->Next()) << "expected one rows";
    string name;
    EXPECT_FALSE(r->Scan(&name));
    r->Close();
    tx->Rollback();
}

TEST_F(GoSQLite3Suite, PreparedStmt) {
    mustExec("CREATE TABLE t (count INT)");
    auto [sel, err] = db->Prepare("SELECT count FROM t ORDER BY count DESC");
    ASSERT_FALSE(err) << "prepare 1: " << S(err);
    auto [ins, err2] = db->Prepare("INSERT INTO t (count) VALUES (?)");
    ASSERT_FALSE(err2) << "prepare 2: " << S(err2);
    for (int n = 1; n <= 3; n++) ASSERT_FALSE(ins->Exec(n).second) << "insert(" << n << ")";

    constexpr int nRuns = 10;
    sync::WaitGroup wg;
    std::atomic<int> failures{0};
    for (int i = 0; i < nRuns; i++) {
        wg.Go([&, i] {
            std::mt19937 rng(i);
            for (int j = 0; j < 10; j++) {
                int count = 0;
                if (error e = sel->QueryRow()->Scan(&count); e && e != sql::ErrNoRows) {
                    ADD_FAILURE() << "Query: " << S(e);
                    failures++;
                    return;
                }
                if (auto [_, e] = ins->Exec(static_cast<int>(rng() % 100)); e) {
                    ADD_FAILURE() << "Insert: " << S(e);
                    failures++;
                    return;
                }
            }
        });
    }
    wg.Wait();
    EXPECT_EQ(failures.load(), 0);
    sel->Close();
    ins->Close();
}

TEST_F(GoSQLite3Suite, ExecEmptyQuery) {
    auto [res, err] = db->Exec(" -- this is just a comment ");
    ASSERT_FALSE(err) << "empty query err: " << S(err);
    EXPECT_FALSE(res->LastInsertId().second);
    EXPECT_FALSE(res->RowsAffected().second);
}

// ---------------------------------------------------------------------------
// error_test.go

TEST(GoSQLite3, SimpleError) {
    std::string e = S(sqlite3::ErrError.Error());
    EXPECT_TRUE(e == "SQL logic error or missing database" || e == "SQL logic error") << "wrong error code: " << e;
}

namespace {

struct temp_dir {
    std::string name;
    temp_dir() {
        auto [n, err] = os::MkdirTemp("", "sqlite3");
        if (err) ADD_FAILURE() << S(err);
        name = S(n);
    }
    ~temp_dir() { os::RemoveAll(name); }
};

}  // namespace

TEST(GoSQLite3, CorruptDbErrors) {
    temp_dir dir;
    std::string dbFileName = dir.name + "/test.db";
    auto [f, err] = os::Create(dbFileName);
    ASSERT_FALSE(err);
    f->Write(bytes_of({1, 2, 3, 4, 5}));
    f->Close();

    auto [db, oerr] = sql::Open("sqlite3", dbFileName);
    error e = oerr;
    if (!e) e = db->Exec("drop table foo").second;
    auto* sqliteErr = errors::As<sqlite3::Error>(e);
    ASSERT_TRUE(sqliteErr) << S(e);
    EXPECT_EQ(sqliteErr->Code, sqlite3::ErrNotADB) << "wrong error code for corrupted DB";
    EXPECT_FALSE(e.Error().empty()) << "wrong error string for corrupted DB";
    if (db) db->Close();
}

TEST(GoSQLite3, SqlLogicErrors) {
    temp_dir dir;
    auto db = mustOpen(dir.name + "/test.db");
    close_after c{db};
    EXPECT_FALSE(db->Exec("CREATE TABLE Foo (id INTEGER PRIMARY KEY)").second);
    EXPECT_EQ(S(db->Exec("CREATE TABLE Foo (id INTEGER PRIMARY KEY)").second), "table Foo already exists");
}

namespace {

ptr<sql::DB> errorsDB(const temp_dir& dir) {
    auto db = mustOpen(dir.name + "/test.db");
    EXPECT_FALSE(db->Exec("PRAGMA foreign_keys=ON;").second);
    EXPECT_FALSE(db->Exec(R"(CREATE TABLE Foo (
		id INTEGER PRIMARY KEY AUTOINCREMENT,
		value INTEGER NOT NULL,
		ref INTEGER NULL REFERENCES Foo (id),
		UNIQUE(value)
	);)")
                     .second);
    return db;
}

}  // namespace

TEST(GoSQLite3, ExtendedErrorCodes_ForeignKey) {
    temp_dir dir;
    auto db = errorsDB(dir);
    close_after c{db};
    auto [_, err] = db->Exec("INSERT INTO Foo (ref, value) VALUES (100, 100);");
    ASSERT_TRUE(err) << "No error!";
    auto* sqliteErr = errors::As<sqlite3::Error>(err);
    ASSERT_TRUE(sqliteErr);
    EXPECT_EQ(sqliteErr->Code, sqlite3::ErrConstraint) << "Wrong basic error code";
    EXPECT_EQ(sqliteErr->ExtendedCode, sqlite3::ErrConstraintForeignKey) << "Wrong extended error code";
}

TEST(GoSQLite3, ExtendedErrorCodes_NotNull) {
    temp_dir dir;
    auto db = errorsDB(dir);
    close_after c{db};
    auto [res, err] = db->Exec("INSERT INTO Foo (value) VALUES (100);");
    ASSERT_FALSE(err) << "Creating first row: " << S(err);
    auto [id, ierr] = res->LastInsertId();
    ASSERT_FALSE(ierr);
    auto [_, err2] = db->Exec("INSERT INTO Foo (ref) VALUES (?);", id);
    ASSERT_TRUE(err2) << "No error!";
    auto* sqliteErr = errors::As<sqlite3::Error>(err2);
    ASSERT_TRUE(sqliteErr);
    EXPECT_EQ(sqliteErr->Code, sqlite3::ErrConstraint);
    EXPECT_EQ(sqliteErr->ExtendedCode, sqlite3::ErrConstraintNotNull);
}

TEST(GoSQLite3, ExtendedErrorCodes_Unique) {
    temp_dir dir;
    auto db = errorsDB(dir);
    close_after c{db};
    auto [res, err] = db->Exec("INSERT INTO Foo (value) VALUES (100);");
    ASSERT_FALSE(err) << "Creating first row: " << S(err);
    auto [id, ierr] = res->LastInsertId();
    ASSERT_FALSE(ierr);
    auto [_, err2] = db->Exec("INSERT INTO Foo (ref, value) VALUES (?, 100);", id);
    ASSERT_TRUE(err2) << "No error!";
    auto* sqliteErr = errors::As<sqlite3::Error>(err2);
    ASSERT_TRUE(sqliteErr);
    EXPECT_EQ(sqliteErr->Code, sqlite3::ErrConstraint);
    EXPECT_EQ(sqliteErr->ExtendedCode, sqlite3::ErrConstraintUnique);
    EXPECT_EQ(S(sqliteErr->Code.Extend(3).Error()), "constraint failed");
}

TEST(GoSQLite3, Error_SystemErrno) {
    auto [_, n, __] = sqlite3::Version();
    if (n < 3012000) GTEST_SKIP() << "sqlite3_system_errno requires sqlite3 >= 3.12.0";
    // open a non-existent database in read-only mode so we get an IO error.
    auto db = mustOpen("file:nonexistent.db?mode=ro");
    close_after c{db};
    error err = db->Ping();
    ASSERT_TRUE(err) << "expected error pinging read-only non-existent database, but got nil";
    auto* serr = errors::As<sqlite3::Error>(err);
    ASSERT_TRUE(serr) << "expected error to be of type Error, but got " << S(err);
    ASSERT_NE(serr->SystemErrno, 0) << "expected SystemErrno to be set";
    EXPECT_EQ(serr->SystemErrno, ENOENT) << "expected SystemErrno to be a not exists error";
}

// ---------------------------------------------------------------------------
// sqlite3_exec_trailing_test.go

TEST(GoSQLite3, ExecTrailingCommentOrSemicolonKeepsResult) {
    auto db = mustOpen(":memory:");
    close_after c{db};
    db->SetMaxOpenConns(1);
    ASSERT_FALSE(db->Exec("CREATE TABLE t (id INTEGER PRIMARY KEY, v TEXT)").second);
    ASSERT_FALSE(db->Exec("INSERT INTO t (id, v) VALUES (1, 'a')").second);

    struct tcase {
        std::string name;
        std::string query;
        std::vector<Value> args;
        int64 affected;
        bool hasID;
    };
    std::vector<tcase> cases = {
        {"update", "UPDATE t SET v = 'b' WHERE id = 1", {}, 1, false},
        {"update trailing comment", "UPDATE t SET v = 'c' WHERE id = 1; -- mark as c", {}, 1, false},
        {"update args", "UPDATE t SET v = ? WHERE id = ?", {string("d"), int64(1)}, 1, false},
        {"update args trailing comment", "UPDATE t SET v = ? WHERE id = ?; -- mark as e", {string("e"), int64(1)}, 1, false},
        {"insert", "INSERT INTO t (v) VALUES ('f')", {}, 1, true},
        {"insert extra semicolon", "INSERT INTO t (v) VALUES ('g');;", {}, 1, true},
        {"insert args trailing comment", "INSERT INTO t (v) VALUES (?);\n-- end of script", {string("h")}, 1, true},
    };
    auto run = [&](const std::function<std::pair<ptr<sql::Result>, error>(const tcase&)>& exec) {
        for (const auto& tc : cases) {
            auto [res, err] = exec(tc);
            if (err) {
                ADD_FAILURE() << tc.name << ": Exec(" << tc.query << ") error: " << S(err);
                continue;
            }
            EXPECT_EQ(res->RowsAffected().first, tc.affected) << tc.name;
            int64 id = res->LastInsertId().first;
            if (tc.hasID) {
                int64 maxID = 0;
                ASSERT_FALSE(db->QueryRow("SELECT max(id) FROM t")->Scan(&maxID));
                EXPECT_EQ(id, maxID) << tc.name;
            }
        }
    };
    auto exec_with = [&](const ptr<context::Context>& ctx, const tcase& tc) {
        if (tc.args.empty()) return db->ExecContext(ctx, tc.query);
        if (tc.args.size() == 1) return db->ExecContext(ctx, tc.query, tc.args[0]);
        return db->ExecContext(ctx, tc.query, tc.args[0], tc.args[1]);
    };
    run([&](const tcase& tc) { return exec_with(context::Background(), tc); });  // exec
    auto [ctx, cancel] = context::WithCancel(context::Background());
    run([&](const tcase& tc) { return exec_with(ctx, tc); });  // execContext
    cancel();
}

// ---------------------------------------------------------------------------
// sqlite3_context_test.go, with a long recursive query in place of the
// function-controlled one

namespace {

constexpr time::Duration contextTestTimeout = 5 * time::Second;

// Runs until interrupted (as mattn's query does while its function says
// to go on).
constexpr std::string_view contextTestLongQuery = R"(
	WITH RECURSIVE numbers(value) AS (
		VALUES(0)
		UNION ALL
		SELECT value + 1 FROM numbers
		WHERE value < ?
	)
	SELECT sum(value) FROM numbers)";

std::vector<driver::NamedValue> contextTestQueryArgs(int64 maxRows) {
    return {{.Name = {}, .Ordinal = 1, .Value = maxRows}};
}

constexpr int64 contextTestMaxRows = int64(1) << 60;

// Receives from c within d (a stopped timer, not time::After, so that
// nothing outlives the test for the leak check); false on timeout.
bool recvWithin(const chan<error>& c, time::Duration d, error* out) {
    auto t = time::NewTimer(d);
    bool got = false;
    select(recv(c,
                [&](error e) {
                    got = true;
                    *out = e;
                }),
           recv(t->C, [](time::Time) {}));
    t->Stop();
    return got;
}

}  // namespace

TEST(GoSQLite3, RowsContextCancelDuringStep) {
    for (bool laterRow : {false, true}) {
        std::string query(contextTestLongQuery);
        if (laterRow) query = "SELECT 1 UNION ALL SELECT (" + query + ")";
        auto conn = openConn();
        auto [ctx, cancel] = context::WithCancel(context::Background());
        auto nextDone = makeChan<error>(1);
        go([=] {
            auto args = contextTestQueryArgs(contextTestMaxRows);
            auto [rows, err] = conn->QueryContext(ctx, query, args);
            if (err) {
                nextDone.send(err);
                return;
            }
            std::vector<Value> dest(1);
            if (laterRow) {
                if (error e = rows->Next(dest)) {
                    rows->Close();
                    nextDone.send(e);
                    return;
                }
            }
            error e = rows->Next(dest);
            rows->Close();
            nextDone.send(e);
        });
        // The query is running (the first row of laterRow comes at once).
        time::Sleep(50 * time::Millisecond);
        cancel();
        error err;
        bool got = recvWithin(nextDone, contextTestTimeout, &err);
        EXPECT_TRUE(got) << "laterRow=" << laterRow << ": Next did not return after cancellation";
        if (got) {
            EXPECT_TRUE(errors::Is(err, context::Canceled))
                << "laterRow=" << laterRow << ": Next error = " << S(err) << ", want context.Canceled";
        }
        conn->Close();
    }
}

TEST(GoSQLite3, RowsContextCancelBetweenRows) {
    auto conn = openConn();
    auto [ctx, cancel] = context::WithCancel(context::Background());
    auto [rows, err] = conn->QueryContext(ctx, "SELECT 1 UNION ALL SELECT 2", {});
    ASSERT_FALSE(err) << "query: " << S(err);
    std::vector<Value> values(1);
    ASSERT_FALSE(rows->Next(values)) << "first Next";
    cancel();
    error e = rows->Next(values);
    EXPECT_TRUE(errors::Is(e, context::Canceled)) << "second Next error = " << S(e) << ", want context.Canceled";
    rows->Close();
    conn->Close();
}

// A cancellation that comes after its rows have closed must not interrupt
// the statement's next use (mattn's
// TestRowsLateInterruptDoesNotAffectReusedStatement, through the context).
TEST(GoSQLite3, RowsLateInterruptDoesNotAffectReusedStatement) {
    auto conn = openConn();
    auto [stmt, err] = conn->prepare("SELECT 1 UNION ALL SELECT 2");
    ASSERT_FALSE(err) << S(err);
    auto [oldCtx, cancelOld] = context::WithCancel(context::Background());
    auto [oldRows, qerr] = stmt->QueryContext(oldCtx, {});
    ASSERT_FALSE(qerr) << S(qerr);
    std::vector<Value> dest(1);
    ASSERT_FALSE(oldRows->Next(dest));
    ASSERT_FALSE(oldRows->Close());

    auto [newRows, qerr2] = stmt->QueryContext(context::Background(), {});
    ASSERT_FALSE(qerr2) << S(qerr2);
    cancelOld();  // late: the old rows are closed
    time::Sleep(10 * time::Millisecond);
    EXPECT_FALSE(newRows->Next(dest)) << "replacement query was interrupted";
    EXPECT_FALSE(newRows->Next(dest)) << "replacement query was interrupted";
    newRows->Close();
    stmt->Close();
    conn->Close();
}

TEST(GoSQLite3, RowsContextCancelAndClose) {
    auto conn = openConn();
    for (int i = 0; i < 100; i++) {
        auto [ctx, cancel] = context::WithCancel(context::Background());
        auto [rows, err] = conn->QueryContext(ctx, "SELECT 1 UNION ALL SELECT 2", {});
        ASSERT_FALSE(err) << "query cancellable rows: " << S(err);
        std::vector<Value> dest(1);
        ASSERT_FALSE(rows->Next(dest)) << "read cancellable rows";

        auto start = makeChan<bool>();
        auto closeDone = makeChan<error>(1);
        auto cancelDone = makeChan<bool>(1);
        go([=] {
            start.recv2();
            closeDone.send(rows->Close());
        });
        go([=] {
            start.recv2();
            cancel();
            cancelDone.send(true);
        });
        start.close();
        EXPECT_FALSE(closeDone.recv()) << "close cancellable rows";
        cancelDone.recv();

        auto [reused, err2] = conn->QueryContext(context::Background(), "SELECT 1", {});
        ASSERT_FALSE(err2) << "query reused connection: " << S(err2);
        ASSERT_FALSE(reused->Next(dest)) << "read reused connection";
        ASSERT_FALSE(reused->Close()) << "close reused rows";
    }
    conn->Close();
}

TEST(GoSQLite3, DatabaseSQLRowsContextCancelAndReuse) {
    auto db = mustOpen(":memory:");
    close_after c{db};
    auto [sqlConn, err] = db->Conn(context::Background());
    ASSERT_FALSE(err) << S(err);
    auto [ctx, cancel] = context::WithCancel(context::Background());
    auto nextDone = makeChan<error>(1);
    go([=] {
        auto [rows, qerr] = sqlConn->QueryContext(ctx, contextTestLongQuery, contextTestMaxRows);
        if (qerr) {
            nextDone.send(qerr);
            return;
        }
        if (rows->Next()) {
            rows->Close();
            nextDone.send(errors::New("Next returned a row after cancellation"));
            return;
        }
        error e = rows->Err();
        if (error ce = rows->Close(); !e) e = ce;
        nextDone.send(e);
    });
    time::Sleep(50 * time::Millisecond);
    cancel();
    error e;
    ASSERT_TRUE(recvWithin(nextDone, contextTestTimeout, &e)) << "Next did not return after cancellation";
    EXPECT_TRUE(errors::Is(e, context::Canceled)) << "Next error = " << S(e) << ", want context.Canceled";
    int v = 0;
    ASSERT_FALSE(sqlConn->QueryRowContext(context::Background(), "SELECT 1")->Scan(&v)) << "query reused connection";
    EXPECT_EQ(v, 1);
    sqlConn->Close();
}

// go++'s own: ExecContext is interrupted too, and reports the context's
// error.
TEST(GoppSQLite3, ExecContextCancel) {
    auto db = mustOpen(":memory:");
    close_after c{db};
    ASSERT_FALSE(db->Exec("CREATE TABLE t (v INTEGER)").second);
    auto [ctx, cancel] = context::WithTimeout(context::Background(), 50 * time::Millisecond);
    auto start = time::Now();
    auto [_, err] = db->ExecContext(ctx, std::string("INSERT INTO t ") + std::string(contextTestLongQuery), contextTestMaxRows);
    EXPECT_TRUE(errors::Is(err, context::DeadlineExceeded)) << S(err);
    EXPECT_LT(time::Since(start).ns, (2 * time::Second).ns);
    cancel();
}

// go++'s own: a long SQLite call doesn't stall other fibers, as it runs as
// a blocking call. A fresh process with one P and no preemption, where it
// would stall them until it ended.
namespace {

const bool sqlite_blocking_helper = go_helper::registerHelper("sqlite-blocking", [](const std::vector<std::string>&) {
    auto [db, err] = sql::Open("sqlite3", ":memory:");
    if (err) os::Exit(2);
    auto started = makeChan<bool>(1);
    go([db, started] {
        started.send(true);
        // About half a second of computing in SQLite.
        int64 n = 0;
        db->QueryRow(std::string(contextTestLongQuery), int64(20000000))->Scan(&n);
    });
    started.recv();
    auto t0 = time::Now();
    time::Sleep(10 * time::Millisecond);
    os::Exit(time::Since(t0) < 200 * time::Millisecond ? 0 : 1);
});

}  // namespace

TEST(GoppSQLite3, LongQueryDoesntStallFibers) {
    auto [ctx, cancel] = context::WithTimeout(context::Background(), 20 * time::Second);
    auto cmd = go_helper::helperCommandContext(ctx, "sqlite-blocking");
    cmd->Env = append(cmd->Environ(), string("GOMAXPROCS=1"), string("GODEBUG=asyncpreemptoff=1"));
    EXPECT_FALSE(cmd->Run()) << "a sleeping fiber waited for the query";
    cancel();
}
