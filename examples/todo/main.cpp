// todo: a JSON API for a to-do list, stored in SQLite.
//
//     $ todo [-addr :8080] [-db todo.db]
//     $ curl -d '{"title":"Buy milk"}' localhost:8080/todos
//     {"id":1,"title":"Buy milk","done":false,"created":"2026-11-13T21:04:05.123Z"}
//     $ curl localhost:8080/todos
//     $ curl -X PATCH -d '{"done":true}' localhost:8080/todos/1
//     $ curl -X DELETE localhost:8080/todos/1
//
// database/sql pools the SQLite connections, and every query runs with
// its request's context: a client that goes away cancels its query.
// Structs go to and from JSON by reflection, and created comes back from
// the DATETIME column as a time::Time. Ctrl-C shuts the server down
// gracefully.
#include <gopp/context.hpp>
#include <gopp/database/sql.hpp>
#include <gopp/encoding/json.hpp>
#include <gopp/errors.hpp>
#include <gopp/flag.hpp>
#include <gopp/fmt.hpp>
#include <gopp/net/http.hpp>
#include <gopp/os.hpp>
#include <gopp/os/signal.hpp>
#include <gopp/sqlite3.hpp>  // registers the "sqlite3" driver
#include <gopp/strings.hpp>
#include <gopp/syscall.hpp>
#include <gopp/time.hpp>

#include <optional>
#include <vector>

using namespace gopp;
namespace signal = os::signal;

struct Todo {
    [[=json::tag("id")]] int64 ID = 0;
    [[=json::tag("title")]] string Title;
    [[=json::tag("done")]] bool Done = false;
    [[=json::tag("created")]] time::Time Created;
};

// The body of a PATCH: only the fields it has change.
struct TodoPatch {
    [[=json::tag("title")]] std::optional<string> Title;
    [[=json::tag("done")]] std::optional<bool> Done;
};

class Server {
public:
    explicit Server(ptr<sql::DB> db) : db_(std::move(db)) {}

    void Routes(http::ServeMux& mux) {
        mux.HandleFunc("GET /todos", [this](http::ResponseWriter& w, http::Request& r) { list(w, r); });
        mux.HandleFunc("POST /todos", [this](http::ResponseWriter& w, http::Request& r) { create(w, r); });
        mux.HandleFunc("GET /todos/{id}", [this](http::ResponseWriter& w, http::Request& r) { get(w, r); });
        mux.HandleFunc("PATCH /todos/{id}", [this](http::ResponseWriter& w, http::Request& r) { patch(w, r); });
        mux.HandleFunc("DELETE /todos/{id}", [this](http::ResponseWriter& w, http::Request& r) { remove(w, r); });
    }

private:
    void list(http::ResponseWriter& w, http::Request& r) {
        auto [rows, err] = db_->QueryContext(r.Context(), "SELECT id, title, done, created FROM todos ORDER BY id");
        if (err) return fail(w, err);
        std::vector<Todo> todos;
        while (rows->Next()) {
            Todo t;
            if (auto err = rows->Scan(&t.ID, &t.Title, &t.Done, &t.Created)) return fail(w, err);
            todos.push_back(std::move(t));
        }
        if (auto err = rows->Err()) return fail(w, err);
        reply(w, http::StatusOK, todos);
    }

    void create(http::ResponseWriter& w, http::Request& r) {
        Todo t;
        if (auto err = json::NewDecoder(*r.Body).Decode(&t); err || strings::TrimSpace(t.Title).empty()) {
            return http::Error(w, "want {\"title\": \"...\"}", http::StatusBadRequest);
        }
        t.Created = time::Now().UTC().Truncate(time::Millisecond);
        auto [res, err] = db_->ExecContext(r.Context(), "INSERT INTO todos (title, done, created) VALUES (?, ?, ?)",
                                           t.Title, t.Done, t.Created);
        if (err) return fail(w, err);
        t.ID = res->LastInsertId().first;
        w.Header().Set("Location", fmt::Sprint("/todos/{}", t.ID));
        reply(w, http::StatusCreated, t);
    }

    void get(http::ResponseWriter& w, http::Request& r) {
        auto [t, err] = find(r);
        if (err) return fail(w, err);
        reply(w, http::StatusOK, t);
    }

    void patch(http::ResponseWriter& w, http::Request& r) {
        TodoPatch p;
        if (auto err = json::NewDecoder(*r.Body).Decode(&p)) {
            return http::Error(w, "want {\"title\": \"...\", \"done\": true}", http::StatusBadRequest);
        }
        // Read and update in one transaction, so that two PATCHes can't
        // interleave.
        auto [tx, err] = db_->BeginTx(r.Context(), {});
        if (err) return fail(w, err);
        auto [t, ferr] = find(r, tx.get());
        if (ferr) {
            tx->Rollback();
            return fail(w, ferr);
        }
        if (p.Title) t.Title = *p.Title;
        if (p.Done) t.Done = *p.Done;
        if (auto [_, err] = tx->ExecContext(r.Context(), "UPDATE todos SET title = ?, done = ? WHERE id = ?", t.Title,
                                            t.Done, t.ID);
            err) {
            tx->Rollback();
            return fail(w, err);
        }
        if (auto err = tx->Commit()) return fail(w, err);
        reply(w, http::StatusOK, t);
    }

    void remove(http::ResponseWriter& w, http::Request& r) {
        auto [id, perr] = strconv::ParseInt(r.PathValue("id"), 10, 64);
        if (perr) return http::NotFound(w, r);
        auto [res, err] = db_->ExecContext(r.Context(), "DELETE FROM todos WHERE id = ?", id);
        if (err) return fail(w, err);
        if (res->RowsAffected().first == 0) return http::NotFound(w, r);
        w.WriteHeader(http::StatusNoContent);
    }

    // The to-do of the request's {id}, through tx if it isn't nullptr.
    std::pair<Todo, error> find(http::Request& r, sql::Tx* tx = nullptr) {
        Todo t;
        auto [id, perr] = strconv::ParseInt(r.PathValue("id"), 10, 64);
        if (perr) return {t, sql::ErrNoRows};
        const char* q = "SELECT id, title, done, created FROM todos WHERE id = ?";
        auto row = tx ? tx->QueryRowContext(r.Context(), q, id) : db_->QueryRowContext(r.Context(), q, id);
        return {t, row->Scan(&t.ID, &t.Title, &t.Done, &t.Created)};
    }

    template <typename T>
    static void reply(http::ResponseWriter& w, int status, const T& v) {
        w.Header().Set("Content-Type", "application/json");
        w.WriteHeader(status);
        json::NewEncoder(w).Encode(v);
    }

    static void fail(http::ResponseWriter& w, const error& err) {
        if (errors::Is(err, sql::ErrNoRows)) return http::Error(w, "no such to-do", http::StatusNotFound);
        if (errors::Is(err, context::Canceled)) return;  // the client went away
        fmt::Println(os::Stderr, "todo: {}", err);
        http::Error(w, "internal error", http::StatusInternalServerError);
    }

    ptr<sql::DB> db_;
};

int main(int argc, char** argv) {
    string addr = ":8080", dbfile = "todo.db";
    flag::StringVar(&addr, "addr", addr, "address to listen on");
    flag::StringVar(&dbfile, "db", dbfile, "the SQLite database file");
    flag::Parse(argc, argv);

    // WAL lets readers and the writer work at once; foreign keys and a busy
    // timeout are mattn's options, as in Go.
    auto [db, err] = sql::Open("sqlite3", fmt::Sprint("{}?_journal_mode=WAL&_foreign_keys=1&_busy_timeout=5000", dbfile));
    if (err) {
        fmt::Println(os::Stderr, "todo: {}", err);
        return 1;
    }
    if (auto [_, err] = db->Exec(R"(CREATE TABLE IF NOT EXISTS todos (
            id INTEGER PRIMARY KEY,
            title TEXT NOT NULL,
            done BOOLEAN NOT NULL DEFAULT 0,
            created DATETIME NOT NULL))");
        err) {
        fmt::Println(os::Stderr, "todo: {}", err);
        return 1;
    }

    Server s(db);
    auto mux = http::NewServeMux();
    s.Routes(*mux);
    http::Server srv{.Addr = addr, .Handler = mux};
    auto served = make_chan<error>(1);
    go([&srv, served] { served.send(srv.ListenAndServe()); });
    fmt::Println("todo: listening on {}, database {}", addr, dbfile);

    auto [ctx, stop] = signal::NotifyContext(context::Background(), os::Interrupt, syscall::SIGTERM_);
    error serve_err;
    select(recv(served, [&](error e) { serve_err = e; }), recv(ctx->Done(), [] {}));
    stop();
    if (serve_err) {
        fmt::Println(os::Stderr, "todo: {}", serve_err);
        return 1;
    }
    auto [sctx, cancel] = context::WithTimeout(context::Background(), 5 * time::Second);
    srv.Shutdown(sctx);
    cancel();
    served.recv();
    db->Close();
    return 0;
}
