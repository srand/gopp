// After Go's database/sql: sql.go, ctxutil.go and closemu.go.
//
// The structure follows Go's: DB keeps a pool of driverConns guarded by
// db.mu, each driverConn its own mutex for driver calls, and objects that
// depend on others (a Stmt on its connections, Rows on a Stmt) are tracked
// in DB.dep so that drivers' objects close only when nothing uses them.
#include "internal.hpp"

#include <gopp/io.hpp>
#include <gopp/runtime.hpp>
#include <gopp/strings.hpp>

#include <algorithm>
#include <exception>
#include <map>
#include <mutex>
#include <random>
#include <shared_mutex>

namespace gopp::sql {

using detail::conn_reuse_strategy;
using detail::driver_conn;
using detail::driver_stmt;
using detail::release_fn;

namespace {

sync::RWMutex drivers_mu;
std::map<std::string, ptr<driver::Driver>, std::less<>>& drivers() {
    static auto* m = new std::map<std::string, ptr<driver::Driver>, std::less<>>();
    return *m;
}

const error err_db_closed = errors::New("sql: database is closed");
const error err_rows_closed = errors::New("sql: Rows are closed");
const error err_no_rows_available = errors::New("sql: no Rows available");

constexpr int default_max_idle_conns = 2;
constexpr int max_bad_conn_retries = 2;

template <typename F>
void with_lock(driver_conn& dc, F&& f) {
    std::lock_guard l(dc);
    f();
}

// Whether ctx is done, without blocking (select with a default case).
error ctx_done(const ptr<context::Context>& ctx) { return ctx ? ctx->Err() : nullptr; }

// ---------------------------------------------------------------------------
// ctxutil.go

std::pair<std::vector<driver::Value>, error> named_value_to_value(std::span<const driver::NamedValue> named) {
    std::vector<driver::Value> dargs;
    dargs.reserve(named.size());
    for (auto& p : named) {
        if (!p.Name.empty()) return {{}, errors::New("sql: driver does not support the use of Named Parameters")};
        dargs.push_back(p.Value);
    }
    return {std::move(dargs), nullptr};
}

std::pair<ptr<driver::Stmt>, error> ctx_driver_prepare(const ptr<context::Context>& ctx, driver::Conn& ci,
                                                       std::string_view query) {
    if (auto* c = dynamic_cast<driver::ConnPrepareContext*>(&ci)) return c->PrepareContext(ctx, query);
    auto [si, err] = ci.Prepare(query);
    if (!err) {
        if (error cerr = ctx_done(ctx)) {
            si->Close();
            return {nullptr, cerr};
        }
    }
    return {si, err};
}

std::pair<ptr<driver::Result>, error> ctx_driver_exec(const ptr<context::Context>& ctx, driver::ExecerContext* execer_ctx,
                                                      driver::Execer* execer, std::string_view query,
                                                      std::span<const driver::NamedValue> nvdargs) {
    if (execer_ctx) return execer_ctx->ExecContext(ctx, query, nvdargs);
    auto [dargs, err] = named_value_to_value(nvdargs);
    if (err) return {nullptr, err};
    if (error cerr = ctx_done(ctx)) return {nullptr, cerr};
    return execer->Exec(query, dargs);
}

std::pair<ptr<driver::Rows>, error> ctx_driver_query(const ptr<context::Context>& ctx, driver::QueryerContext* queryer_ctx,
                                                     driver::Queryer* queryer, std::string_view query,
                                                     std::span<const driver::NamedValue> nvdargs) {
    if (queryer_ctx) return queryer_ctx->QueryContext(ctx, query, nvdargs);
    auto [dargs, err] = named_value_to_value(nvdargs);
    if (err) return {nullptr, err};
    if (error cerr = ctx_done(ctx)) return {nullptr, cerr};
    return queryer->Query(query, dargs);
}

std::pair<ptr<driver::Result>, error> ctx_driver_stmt_exec(const ptr<context::Context>& ctx, driver::Stmt& si,
                                                           std::span<const driver::NamedValue> nvdargs) {
    if (auto* s = dynamic_cast<driver::StmtExecContext*>(&si)) return s->ExecContext(ctx, nvdargs);
    auto [dargs, err] = named_value_to_value(nvdargs);
    if (err) return {nullptr, err};
    if (error cerr = ctx_done(ctx)) return {nullptr, cerr};
    return si.Exec(dargs);
}

std::pair<ptr<driver::Rows>, error> ctx_driver_stmt_query(const ptr<context::Context>& ctx, driver::Stmt& si,
                                                          std::span<const driver::NamedValue> nvdargs) {
    if (auto* s = dynamic_cast<driver::StmtQueryContext*>(&si)) return s->QueryContext(ctx, nvdargs);
    auto [dargs, err] = named_value_to_value(nvdargs);
    if (err) return {nullptr, err};
    if (error cerr = ctx_done(ctx)) return {nullptr, cerr};
    return si.Query(dargs);
}

std::pair<ptr<driver::Tx>, error> ctx_driver_begin(const ptr<context::Context>& ctx, const TxOptions* opts, driver::Conn& ci) {
    if (auto* c = dynamic_cast<driver::ConnBeginTx*>(&ci)) {
        driver::TxOptions dopts;
        if (opts) {
            dopts.Isolation = opts->Isolation;
            dopts.ReadOnly = opts->ReadOnly;
        }
        return c->BeginTx(ctx, dopts);
    }
    if (opts) {
        if (opts->Isolation != LevelDefault) {
            return {nullptr, errors::New("sql: driver does not support non-default isolation level")};
        }
        if (opts->ReadOnly) return {nullptr, errors::New("sql: driver does not support read-only transactions")};
    }
    auto [txi, err] = ci.Begin();
    if (!err) {
        if (error cerr = ctx_done(ctx)) {
            txi->Rollback();
            return {nullptr, cerr};
        }
    }
    return {txi, err};
}

// The Result of a driver, called under its connection's lock.
struct driver_result final : Result {
    ptr<driver_conn> dc;
    ptr<driver::Result> resi;
    driver_result(ptr<driver_conn> c, ptr<driver::Result> r) : dc(std::move(c)), resi(std::move(r)) {}
    std::pair<int64, error> LastInsertId() override {
        std::lock_guard l(*dc);
        return resi->LastInsertId();
    }
    std::pair<int64, error> RowsAffected() override {
        std::lock_guard l(*dc);
        return resi->RowsAffected();
    }
};

std::pair<ptr<Result>, error> result_from_statement(const ptr<context::Context>& ctx, driver::Conn& ci,
                                                    const ptr<driver_stmt>& ds, std::span<const detail::arg> args) {
    std::lock_guard l(*ds);
    auto [dargs, err] = detail::driver_args_conn_locked(&ci, ds.get(), args);
    if (err) return {nullptr, err};
    auto [resi, rerr] = ctx_driver_stmt_exec(ctx, *ds->si, dargs);
    if (rerr) return {nullptr, rerr};
    return {make_ptr<driver_result>(ptr<driver_conn>(ds->dc), resi), nullptr};
}

std::pair<ptr<driver::Rows>, error> rowsi_from_statement(const ptr<context::Context>& ctx, driver::Conn& ci,
                                                         const ptr<driver_stmt>& ds, std::span<const detail::arg> args) {
    std::lock_guard l(*ds);
    auto [dargs, err] = detail::driver_args_conn_locked(&ci, ds.get(), args);
    if (err) return {nullptr, err};
    return ctx_driver_stmt_query(ctx, *ds->si, dargs);
}

}  // namespace

// ---------------------------------------------------------------------------
// Drivers

void Register(std::string_view name, ptr<driver::Driver> d) {
    std::lock_guard l(drivers_mu);
    if (!d) panic("sql: Register driver is nil");
    if (drivers().contains(name)) panic("sql: Register called twice for driver " + std::string(name));
    drivers().emplace(std::string(name), std::move(d));
}

std::vector<string> Drivers() {
    std::shared_lock l(drivers_mu);
    std::vector<string> names;
    for (auto& [k, v] : drivers()) names.emplace_back(k);
    return names;  // the map is sorted
}

const error ErrNoRows = errors::New("sql: no rows in result set");
const error ErrConnDone = errors::New("sql: connection is already closed");
const error ErrTxDone = errors::New("sql: transaction has already been committed or rolled back");

// ---------------------------------------------------------------------------
// closingMutex

namespace detail {

std::function<void(Rows&, error&)> rows_close_hook;

void closing_mutex::RLock() {
    if (TryRLock()) return;
    std::lock_guard l(mu_);
    for (;;) {
        if (TryRLock()) return;
        read_.Wait();
    }
}

void closing_mutex::RUnlock() {
    for (;;) {
        int64 x = state_.load();
        if (x < 2) panic("runlock of un-rlocked mutex");
        if (state_.compare_exchange_strong(x, x - 2)) {
            if (x - 2 == 1) {
                // The last reader leaves a waiting writer.
                std::lock_guard l(mu_);
                write_.Broadcast();
            }
            return;
        }
    }
}

void closing_mutex::Lock() {
    std::lock_guard l(mu_);
    for (;;) {
        int64 x = state_.load();
        if (x == 0 || x == 1) {
            if (state_.compare_exchange_strong(x, -1)) return;
            continue;
        }
        // Mark a waiting writer (the low bit) and wait for the readers.
        if ((x & 1) == 0 && !state_.compare_exchange_strong(x, x | 1)) continue;
        write_.Wait();
    }
}

void closing_mutex::Unlock() {
    std::lock_guard l(mu_);
    int64 x = -1;
    if (!state_.compare_exchange_strong(x, 0)) panic("unlock of unlocked mutex");
    read_.Broadcast();
    write_.Broadcast();
}

bool closing_mutex::TryRLock() {
    for (;;) {
        int64 x = state_.load();
        if (x < 0 || x == 1) return false;
        if (state_.compare_exchange_strong(x, x + 2)) return true;
    }
}

// ---------------------------------------------------------------------------
// connRequestSet

conn_request_set::handle conn_request_set::Add(chan<conn_request> c) {
    auto idx = std::make_shared<int>(static_cast<int>(s_.size()));
    s_.emplace_back(std::move(c), idx);
    return idx;
}

bool conn_request_set::Delete(const handle& h) {
    int idx = *h;
    if (idx < 0) return false;
    delete_index(static_cast<std::size_t>(idx));
    return true;
}

void conn_request_set::delete_index(std::size_t idx) {
    *s_[idx].second = -1;
    if (idx < s_.size() - 1) {
        *s_.back().second = static_cast<int>(idx);
        s_[idx] = std::move(s_.back());
    }
    s_.pop_back();
}

std::optional<chan<conn_request>> conn_request_set::TakeRandom() {
    if (s_.empty()) return std::nullopt;
    thread_local std::minstd_rand rng(std::random_device{}());
    std::size_t pick = rng() % s_.size();
    chan<conn_request> c = s_[pick].first;
    delete_index(pick);
    return c;
}

void conn_request_set::CloseAndRemoveAll() {
    for (auto& [c, idx] : s_) {
        *idx = -1;
        c.close();
    }
    s_.clear();
}

// ---------------------------------------------------------------------------
// driverConn, driverStmt

void driver_conn::release_conn(error err) { db->put_conn(ptr<driver_conn>(this), std::move(err), true); }

void driver_conn::remove_open_stmt(driver_stmt* ds) {
    std::lock_guard l(mu);
    open_stmt.erase(ds);
}

bool driver_conn::expired(time::Duration timeout) const {
    if (timeout.ns <= 0) return false;
    return created_at.Add(timeout).Before(time::Now());
}

error driver_conn::reset_session(ptr<context::Context> ctx) {
    std::lock_guard l(mu);
    if (!need_reset) return nullptr;
    if (auto* cr = dynamic_cast<driver::SessionResetter*>(ci.get())) return cr->ResetSession(std::move(ctx));
    return nullptr;
}

bool driver_conn::validate_connection(bool needs_reset) {
    std::lock_guard l(mu);
    if (needs_reset) need_reset = true;
    if (auto* cv = dynamic_cast<driver::Validator*>(ci.get())) return cv->IsValid();
    return true;
}

std::pair<ptr<driver_stmt>, error> driver_conn::prepare_locked(ptr<context::Context> ctx, bool grabbed,
                                                               std::string_view query) {
    auto [si, err] = ctx_driver_prepare(ctx, *ci, query);
    if (err) return {nullptr, err};
    auto ds = make_ptr<driver_stmt>();
    ds->dc = this;
    ds->si = si;
    // Statements of a Tx or Conn close with them, not with the connection.
    if (grabbed) return {ds, nullptr};
    open_stmt[ds.get()] = ds;
    return {ds, nullptr};
}

std::function<error()> driver_conn::close_db_locked() {
    std::lock_guard l(mu);
    if (closed) return [] { return errors::New("sql: duplicate driverConn close"); };
    closed = true;
    return db->remove_dep_locked(this, this);
}

error driver_conn::Close() {
    {
        std::lock_guard l(mu);
        if (closed) return errors::New("sql: duplicate driverConn close");
        closed = true;
    }
    std::function<error()> fn;
    {
        std::lock_guard l(db->mu_);
        dbmu_closed = true;
        fn = db->remove_dep_locked(this, this);
    }
    return fn();
}

error driver_conn::final_close() {
    ptr<driver_conn> self(this);
    std::vector<ptr<driver_stmt>> stmts;
    {
        std::lock_guard l(mu);
        for (auto& [k, ds] : open_stmt) stmts.push_back(ds);
        open_stmt.clear();
    }
    for (auto& ds : stmts) ds->Close();
    error err;
    {
        std::lock_guard l(mu);
        final_closed = true;
        err = ci->Close();
        ci = nullptr;
    }
    {
        std::lock_guard l(db->mu_);
        db->num_open_--;
        db->maybe_open_new_connections();
    }
    db->num_closed_.fetch_add(1);
    return err;
}

error driver_stmt::Close() {
    std::lock_guard l(*dc);
    if (closed) return close_err;
    closed = true;
    close_err = si->Close();
    return close_err;
}

}  // namespace detail

// ---------------------------------------------------------------------------
// DB: dependencies

void DB::add_dep(detail::final_closer* x, ptr<object> keep, const void* dep) {
    std::lock_guard l(mu_);
    add_dep_locked(x, std::move(keep), dep);
}

void DB::add_dep_locked(detail::final_closer* x, ptr<object> keep, const void* dep) {
    auto& e = dep_[x];
    if (!e.keep) e.keep = std::move(keep);
    e.deps.insert(dep);
}

error DB::remove_dep(detail::final_closer* x, const void* dep) {
    std::function<error()> fn;
    {
        std::lock_guard l(mu_);
        fn = remove_dep_locked(x, dep);
    }
    return fn();
}

std::function<error()> DB::remove_dep_locked(detail::final_closer* x, const void* dep) {
    auto it = dep_.find(x);
    if (it == dep_.end()) panic("unpaired removeDep: no deps");
    if (it->second.deps.erase(dep) == 0) panic("unpaired removeDep: no such dep");
    if (!it->second.deps.empty()) return [] { return error(); };
    ptr<object> keep = std::move(it->second.keep);
    dep_.erase(it);
    return [x, keep] { return x->final_close(); };
}

// ---------------------------------------------------------------------------
// DB: opening and closing

namespace {

// A Connector for a driver and a data source name.
struct dsn_connector final : driver::Connector {
    string dsn;
    ptr<driver::Driver> drv;
    std::pair<ptr<driver::Conn>, error> Connect(ptr<context::Context>) override { return drv->Open(dsn); }
    ptr<driver::Driver> Driver() override { return drv; }
};

}  // namespace

DB::DB(ptr<driver::Connector> c) : connector_(std::move(c)) {}

DB::~DB() = default;

ptr<DB> OpenDB(ptr<driver::Connector> c) {
    auto [ctx, cancel] = context::WithCancel(context::Background());
    auto db = make_ptr<DB>(std::move(c));
    db->opener_ch_ = make_chan<empty>(1);
    db->stop_ = cancel;
    go([db, ctx] { db->connection_opener(ctx); });
    return db;
}

std::pair<ptr<DB>, error> Open(std::string_view driver_name, std::string_view data_source_name) {
    ptr<driver::Driver> d;
    {
        std::shared_lock l(drivers_mu);
        auto it = drivers().find(driver_name);
        if (it == drivers().end()) {
            return {nullptr, errors::New("sql: unknown driver " + detail::quote(driver_name) + " (forgotten import?)")};
        }
        d = it->second;
    }
    if (auto* dc = dynamic_cast<driver::DriverContext*>(d.get())) {
        auto [connector, err] = dc->OpenConnector(data_source_name);
        if (err) return {nullptr, err};
        return {OpenDB(connector), nullptr};
    }
    auto c = make_ptr<dsn_connector>();
    c->dsn = string(data_source_name);
    c->drv = d;
    return {OpenDB(c), nullptr};
}

error DB::ping_dc(ptr<context::Context> ctx, const ptr<driver_conn>& dc, const release_fn& release) {
    error err;
    if (auto* pinger = dynamic_cast<driver::Pinger*>(dc->ci.get())) {
        with_lock(*dc, [&] { err = pinger->Ping(ctx); });
    }
    release(err);
    return err;
}

error DB::PingContext(ptr<context::Context> ctx) {
    ptr<driver_conn> dc;
    error err = retry([&](conn_reuse_strategy strategy) {
        auto [c, e] = conn(ctx, strategy);
        dc = c;
        return e;
    });
    if (err) return err;
    return ping_dc(ctx, dc, [dc](error e) { dc->release_conn(std::move(e)); });
}

error DB::Ping() { return PingContext(context::Background()); }

error DB::Close() {
    std::vector<std::function<error()>> fns;
    {
        std::lock_guard l(mu_);
        if (closed_) return nullptr;  // idempotent
        if (cleaner_ch_) cleaner_ch_.close();
        for (auto& dc : free_conn_) fns.push_back(dc->close_db_locked());
        free_conn_.clear();
        closed_ = true;
        conn_requests_.CloseAndRemoveAll();
    }
    error err;
    for (auto& fn : fns) {
        if (error e = fn()) err = e;
    }
    stop_();
    if (auto* c = dynamic_cast<io::Closer*>(connector_.get())) {
        if (error e = c->Close()) err = e;
    }
    return err;
}

int DB::max_idle_conns_locked() const {
    int n = max_idle_count_;
    if (n == 0) return default_max_idle_conns;
    if (n < 0) return 0;
    return n;
}

time::Duration DB::shortest_idle_time_locked() const {
    if (max_idle_time_.ns <= 0) return max_lifetime_;
    if (max_lifetime_.ns <= 0) return max_idle_time_;
    return std::min(max_idle_time_, max_lifetime_);
}

void DB::SetMaxIdleConns(int n) {
    std::vector<ptr<driver_conn>> closing;
    {
        std::lock_guard l(mu_);
        max_idle_count_ = n > 0 ? n : -1;
        // Make sure maxIdle doesn't exceed maxOpen.
        if (max_open_ > 0 && max_idle_conns_locked() > max_open_) max_idle_count_ = max_open_;
        int max_idle = max_idle_conns_locked();
        if (static_cast<int>(free_conn_.size()) > max_idle) {
            closing.assign(free_conn_.begin() + max_idle, free_conn_.end());
            free_conn_.resize(static_cast<std::size_t>(max_idle));
        }
        max_idle_closed_ += static_cast<int64>(closing.size());
    }
    for (auto& c : closing) c->Close();
}

void DB::SetMaxOpenConns(int n) {
    bool sync_max_idle;
    {
        std::lock_guard l(mu_);
        max_open_ = n < 0 ? 0 : n;
        sync_max_idle = max_open_ > 0 && max_idle_conns_locked() > max_open_;
    }
    if (sync_max_idle) SetMaxIdleConns(n);
}

void DB::SetConnMaxLifetime(time::Duration d) {
    if (d.ns < 0) d = {};
    std::lock_guard l(mu_);
    // Wake the cleaner to apply a shorter lifetime.
    if (d.ns > 0 && d < shortest_idle_time_locked() && cleaner_ch_) cleaner_ch_.try_send(empty{});
    max_lifetime_ = d;
    start_cleaner_locked();
}

void DB::SetConnMaxIdleTime(time::Duration d) {
    if (d.ns < 0) d = {};
    std::lock_guard l(mu_);
    if (d.ns > 0 && d < shortest_idle_time_locked() && cleaner_ch_) cleaner_ch_.try_send(empty{});
    max_idle_time_ = d;
    start_cleaner_locked();
}

void DB::start_cleaner_locked() {
    if ((max_lifetime_.ns > 0 || max_idle_time_.ns > 0) && num_open_ > 0 && !cleaner_ch_) {
        cleaner_ch_ = make_chan<empty>(1);
        ptr<DB> self(this);
        time::Duration d = shortest_idle_time_locked();
        go([self, d] { self->connection_cleaner(d); });
    }
}

void DB::connection_cleaner(time::Duration d) {
    const time::Duration min_interval = time::Second;
    if (d < min_interval) d = min_interval;
    ptr<time::Timer> t = time::NewTimer(d);
    chan<empty> cleaner;
    {
        std::lock_guard l(mu_);
        cleaner = cleaner_ch_;
    }
    for (;;) {
        select(recv(t->C, [] {}), recv(cleaner, [] {}));  // maxLifetime changed or the db closed
        std::vector<ptr<driver_conn>> closing;
        {
            std::lock_guard l(mu_);
            d = shortest_idle_time_locked();
            if (closed_ || num_open_ == 0 || d.ns <= 0) {
                cleaner_ch_ = nullptr;
                return;
            }
            std::tie(d, closing) = connection_cleaner_run_locked(d);
        }
        for (auto& c : closing) c->Close();
        if (d < min_interval) d = min_interval;
        if (!t->Stop()) {
            time::Time v;
            t->C.try_recv(v);
        }
        t->Reset(d);
    }
}

std::pair<time::Duration, std::vector<ptr<driver_conn>>> DB::connection_cleaner_run_locked(time::Duration d) {
    int64 idle_closing = 0;
    std::vector<ptr<driver_conn>> closing;
    if (max_idle_time_.ns > 0) {
        // Connections returned before idle_since have been idle too long;
        // free_conn_ is ordered by returnedAt.
        time::Time idle_since = time::Now().Add(-max_idle_time_);
        for (std::size_t i = free_conn_.size(); i-- > 0;) {
            if (free_conn_[i]->returned_at.Before(idle_since)) {
                closing.assign(free_conn_.begin(), free_conn_.begin() + static_cast<std::ptrdiff_t>(i + 1));
                free_conn_.erase(free_conn_.begin(), free_conn_.begin() + static_cast<std::ptrdiff_t>(i + 1));
                idle_closing = static_cast<int64>(closing.size());
                max_idle_time_closed_ += idle_closing;
                break;
            }
        }
        if (!free_conn_.empty()) {
            if (time::Duration d2 = free_conn_[0]->returned_at.Sub(idle_since); d2 < d) d = d2;
        }
    }
    if (max_lifetime_.ns > 0) {
        time::Time expired_since = time::Now().Add(-max_lifetime_);
        for (std::size_t i = 0; i < free_conn_.size(); i++) {
            auto& c = free_conn_[i];
            if (c->created_at.Before(expired_since)) {
                closing.push_back(c);
                free_conn_.erase(free_conn_.begin() + static_cast<std::ptrdiff_t>(i));
                i--;
            } else if (time::Duration d2 = c->created_at.Sub(expired_since); d2 < d) {
                d = d2;
            }
        }
        max_lifetime_closed_ += static_cast<int64>(closing.size()) - idle_closing;
    }
    return {d, std::move(closing)};
}

DBStats DB::Stats() {
    int64 wait = wait_duration_.load();
    std::lock_guard l(mu_);
    DBStats s;
    s.MaxOpenConnections = max_open_;
    s.Idle = static_cast<int>(free_conn_.size());
    s.OpenConnections = num_open_;
    s.InUse = num_open_ - static_cast<int>(free_conn_.size());
    s.WaitCount = wait_count_;
    s.WaitDuration = time::Duration(wait);
    s.MaxIdleClosed = max_idle_closed_;
    s.MaxIdleTimeClosed = max_idle_time_closed_;
    s.MaxLifetimeClosed = max_lifetime_closed_;
    return s;
}

// Asks the opener for connections the waiting requests need, within
// max_open_.
void DB::maybe_open_new_connections() {
    int num_requests = static_cast<int>(conn_requests_.Len());
    if (max_open_ > 0) num_requests = std::min(num_requests, max_open_ - num_open_);
    for (; num_requests > 0; num_requests--) {
        num_open_++;  // optimistically
        if (closed_) return;
        pending_opens_++;
        opener_ch_.try_send(empty{});
    }
}

void DB::connection_opener(ptr<context::Context> ctx) {
    for (;;) {
        bool done = false;
        select(recv(ctx->Done(), [&] { done = true; }), recv(opener_ch_, [] {}));
        if (done) return;
        for (;;) {
            {
                std::lock_guard l(mu_);
                if (pending_opens_ == 0) break;
                pending_opens_--;
            }
            open_new_connection(ctx);
        }
    }
}

void DB::open_new_connection(ptr<context::Context> ctx) {
    // num_open_ was incremented for this connection already.
    auto [ci, err] = connector_->Connect(ctx);
    std::lock_guard l(mu_);
    if (closed_) {
        if (!err) ci->Close();
        num_open_--;
        return;
    }
    if (err) {
        num_open_--;
        put_conn_db_locked(nullptr, err);
        maybe_open_new_connections();
        return;
    }
    auto dc = make_ptr<driver_conn>();
    dc->db = this;
    dc->created_at = dc->returned_at = time::Now();
    dc->ci = ci;
    if (put_conn_db_locked(dc, nullptr)) {
        add_dep_locked(dc.get(), dc, dc.get());
    } else {
        num_open_--;
        ci->Close();
    }
}

// Returns a connection: a cached one, a new one, or one released by
// another fiber while this one waits (max_open_ reached).
std::pair<ptr<driver_conn>, error> DB::conn(ptr<context::Context> ctx, conn_reuse_strategy strategy) {
    mu_.lock();
    if (closed_) {
        mu_.unlock();
        return {nullptr, err_db_closed};
    }
    if (error err = ctx_done(ctx)) {
        mu_.unlock();
        return {nullptr, err};
    }
    time::Duration lifetime = max_lifetime_;

    // Prefer a free connection, the newest.
    if (strategy == detail::cached_or_new_conn && !free_conn_.empty()) {
        ptr<driver_conn> c = std::move(free_conn_.back());
        free_conn_.pop_back();
        c->in_use = true;
        if (c->expired(lifetime)) {
            max_lifetime_closed_++;
            mu_.unlock();
            c->Close();
            return {nullptr, driver::ErrBadConn};
        }
        mu_.unlock();
        // Reset the session before use.
        if (error err = c->reset_session(ctx); errors::Is(err, driver::ErrBadConn)) {
            c->Close();
            return {nullptr, err};
        }
        return {c, nullptr};
    }

    // Out of connections: wait for one to be released.
    if (max_open_ > 0 && num_open_ >= max_open_) {
        auto req = make_chan<detail::conn_request>(1);
        auto del_handle = conn_requests_.Add(req);
        wait_count_++;
        mu_.unlock();
        time::Time wait_start = time::Now();

        std::optional<std::pair<detail::conn_request, bool>> got;
        error ctx_err;
        select(recv(ctx->Done(), [&] { ctx_err = ctx->Err(); }),
               recv(req, [&](detail::conn_request r, bool ok) { got.emplace(std::move(r), ok); }));
        wait_duration_.fetch_add(time::Since(wait_start).ns);
        if (!got) {
            // Remove the request so that no one sends to it; if someone did
            // already, put the connection back.
            bool deleted;
            {
                std::lock_guard l(mu_);
                deleted = conn_requests_.Delete(del_handle);
            }
            if (!deleted) {
                detail::conn_request r;
                bool closed = false;
                if (req.try_recv(r, &closed) && !closed && r.conn) put_conn(r.conn, r.err, false);
            }
            return {nullptr, ctx_err};
        }
        auto& [ret, ok] = *got;
        if (!ok) return {nullptr, err_db_closed};
        // Only check if the connection is expired if the strategy is
        // cached_or_new_conn; when the strategy is always_new_conn, a bad
        // connection may be returned once more.
        if (strategy == detail::cached_or_new_conn && !ret.err && ret.conn->expired(lifetime)) {
            {
                std::lock_guard l(mu_);
                max_lifetime_closed_++;
            }
            ret.conn->Close();
            return {nullptr, driver::ErrBadConn};
        }
        if (!ret.conn) return {nullptr, ret.err};
        if (error err = ret.conn->reset_session(ctx); errors::Is(err, driver::ErrBadConn)) {
            ret.conn->Close();
            return {nullptr, err};
        }
        return {ret.conn, ret.err};
    }

    num_open_++;  // optimistically
    mu_.unlock();
    auto [ci, err] = connector_->Connect(ctx);
    if (err) {
        std::lock_guard l(mu_);
        num_open_--;  // correct for the optimism
        maybe_open_new_connections();
        return {nullptr, err};
    }
    std::lock_guard l(mu_);
    auto dc = make_ptr<driver_conn>();
    dc->db = this;
    dc->created_at = dc->returned_at = time::Now();
    dc->ci = ci;
    dc->in_use = true;
    add_dep_locked(dc.get(), dc, dc.get());
    return {dc, nullptr};
}

void DB::note_unused_driver_statement(const ptr<driver_conn>& c, const ptr<driver_stmt>& ds) {
    std::lock_guard l(mu_);
    if (c->in_use) {
        c->on_put.push_back([ds] { ds->Close(); });
    } else {
        bool fc;
        {
            std::lock_guard cl(c->mu);
            fc = c->final_closed;
        }
        if (!fc) ds->Close();
    }
}

void DB::put_conn(ptr<driver_conn> dc, error err, bool reset_session) {
    if (!errors::Is(err, driver::ErrBadConn)) {
        if (!dc->validate_connection(reset_session)) err = driver::ErrBadConn;
    }
    mu_.lock();
    if (!dc->in_use) {
        mu_.unlock();
        panic("sql: connection returned that was never out");
    }
    if (!errors::Is(err, driver::ErrBadConn) && dc->expired(max_lifetime_)) {
        max_lifetime_closed_++;
        err = driver::ErrBadConn;
    }
    dc->in_use = false;
    dc->returned_at = time::Now();
    auto on_put = std::move(dc->on_put);
    dc->on_put.clear();
    for (auto& fn : on_put) fn();

    if (errors::Is(err, driver::ErrBadConn)) {
        // Don't reuse bad connections. Since the conn is considered bad
        // and is being discarded, treat it as closed; don't decrement the
        // open count here, final_close will.
        maybe_open_new_connections();
        mu_.unlock();
        dc->Close();
        return;
    }
    bool added = put_conn_db_locked(dc, nullptr);
    mu_.unlock();
    if (!added) dc->Close();
}

// Hands dc (or err) to a waiting request, or keeps dc free. False if dc
// should be closed.
bool DB::put_conn_db_locked(const ptr<driver_conn>& dc, error err) {
    if (closed_) return false;
    if (max_open_ > 0 && num_open_ > max_open_) return false;
    if (auto req = conn_requests_.TakeRandom()) {
        if (!err) dc->in_use = true;
        req->send(detail::conn_request{dc, err});
        return true;
    }
    if (!err && !closed_) {
        if (max_idle_conns_locked() > static_cast<int>(free_conn_.size())) {
            free_conn_.push_back(dc);
            start_cleaner_locked();
            return true;
        }
        max_idle_closed_++;
    }
    return false;
}

// Runs fn with cached connections, then with a new one, while it fails
// with ErrBadConn.
error DB::retry(const std::function<error(conn_reuse_strategy)>& fn) {
    for (int i = 0; i < max_bad_conn_retries; i++) {
        error err = fn(detail::cached_or_new_conn);
        if (!err || !errors::Is(err, driver::ErrBadConn)) return err;
    }
    return fn(detail::always_new_conn);
}

// ---------------------------------------------------------------------------
// DB: statements and queries

std::pair<ptr<sql::Stmt>, error> DB::PrepareContext(ptr<context::Context> ctx, std::string_view query) {
    ptr<sql::Stmt> stmt;
    error err = retry([&](conn_reuse_strategy strategy) {
        auto [dc, e] = conn(ctx, strategy);
        if (e) return e;
        auto [s, perr] = prepare_dc(ctx, dc, [dc](error e) { dc->release_conn(std::move(e)); }, nullptr, nullptr, query);
        stmt = s;
        return perr;
    });
    return {stmt, err};
}

std::pair<ptr<sql::Stmt>, error> DB::Prepare(std::string_view query) { return PrepareContext(context::Background(), query); }

std::pair<ptr<sql::Stmt>, error> DB::prepare_dc(ptr<context::Context> ctx, const ptr<driver_conn>& dc, release_fn release,
                                                detail::stmt_conn_grabber* cg, ptr<object> cg_keep, std::string_view query) {
    ptr<driver_stmt> ds;
    error err;
    with_lock(*dc, [&] { std::tie(ds, err) = dc->prepare_locked(ctx, cg != nullptr, query); });
    release(err);
    if (err) return {nullptr, err};
    // A statement on a Tx or Conn stays on its connection; others note
    // where they are prepared and prepare again where needed.
    ptr<sql::Stmt> stmt(new sql::Stmt());
    stmt->db_ = ptr<DB>(this);
    stmt->query_ = string(query);
    stmt->cg_ = cg;
    stmt->cg_keep_ = std::move(cg_keep);
    stmt->cgds_ = ds;
    if (!cg) {
        stmt->css_.push_back({dc, ds});
        stmt->last_num_closed_ = num_closed_.load();
        add_dep(stmt.get(), stmt, stmt.get());
    }
    return {stmt, nullptr};
}

std::pair<ptr<Result>, error> DB::exec_args(ptr<context::Context> ctx, std::string_view query, std::vector<detail::arg> args) {
    ptr<Result> res;
    error err = retry([&](conn_reuse_strategy strategy) {
        auto [dc, e] = conn(ctx, strategy);
        if (e) return e;
        auto [r, xerr] = exec_dc(ctx, dc, [dc](error e) { dc->release_conn(std::move(e)); }, query, args);
        res = r;
        return xerr;
    });
    return {res, err};
}

std::pair<ptr<Result>, error> DB::exec_dc(ptr<context::Context> ctx, const ptr<driver_conn>& dc, release_fn release,
                                          std::string_view query, std::span<const detail::arg> args) {
    ptr<Result> res;
    error err;
    struct releaser {
        release_fn& release;
        error& err;
        ~releaser() { release(err); }
    } r{release, err};

    auto* execer_ctx = dynamic_cast<driver::ExecerContext*>(dc->ci.get());
    driver::Execer* execer = execer_ctx ? nullptr : dynamic_cast<driver::Execer*>(dc->ci.get());
    if (execer_ctx || execer) {
        ptr<driver::Result> resi;
        with_lock(*dc, [&] {
            auto [nvdargs, aerr] = detail::driver_args_conn_locked(dc->ci.get(), nullptr, args);
            err = aerr;
            if (err) return;
            std::tie(resi, err) = ctx_driver_exec(ctx, execer_ctx, execer, query, nvdargs);
        });
        if (err != driver::ErrSkip) {
            if (err) return {nullptr, err};
            return {make_ptr<driver_result>(dc, resi), nullptr};
        }
    }
    // No fast path: a prepared statement.
    ptr<driver::Stmt> si;
    with_lock(*dc, [&] { std::tie(si, err) = ctx_driver_prepare(ctx, *dc->ci, query); });
    if (err) return {nullptr, err};
    auto ds = make_ptr<driver_stmt>();
    ds->dc = dc.get();
    ds->si = si;
    std::tie(res, err) = result_from_statement(ctx, *dc->ci, ds, args);
    ds->Close();
    if (err) return {nullptr, err};
    return {res, nullptr};
}

std::pair<ptr<Rows>, error> DB::query_args(ptr<context::Context> ctx, std::string_view query, std::vector<detail::arg> args) {
    ptr<Rows> rows;
    error err = retry([&](conn_reuse_strategy strategy) {
        auto [dc, e] = conn(ctx, strategy);
        if (e) return e;
        auto [r, qerr] = query_dc(ctx, nullptr, dc, [dc](error e) { dc->release_conn(std::move(e)); }, query, args);
        rows = r;
        return qerr;
    });
    return {rows, err};
}

// Runs a query on dc; release is called when the Rows close, or now on an
// error.
std::pair<ptr<Rows>, error> DB::query_dc(ptr<context::Context> ctx, ptr<context::Context> txctx, const ptr<driver_conn>& dc,
                                         release_fn release, std::string_view query, std::span<const detail::arg> args) {
    auto* queryer_ctx = dynamic_cast<driver::QueryerContext*>(dc->ci.get());
    driver::Queryer* queryer = queryer_ctx ? nullptr : dynamic_cast<driver::Queryer*>(dc->ci.get());
    if (queryer_ctx || queryer) {
        ptr<driver::Rows> rowsi;
        error err;
        with_lock(*dc, [&] {
            auto [nvdargs, aerr] = detail::driver_args_conn_locked(dc->ci.get(), nullptr, args);
            err = aerr;
            if (err) return;
            std::tie(rowsi, err) = ctx_driver_query(ctx, queryer_ctx, queryer, query, nvdargs);
        });
        if (err != driver::ErrSkip) {
            if (err) {
                release(err);
                return {nullptr, err};
            }
            ptr<Rows> rows(new Rows());
            rows->dc_ = dc;
            rows->release_conn_ = std::move(release);
            rows->rowsi_ = rowsi;
            rows->init_context_close(ctx, txctx);
            return {rows, nullptr};
        }
    }
    ptr<driver::Stmt> si;
    error err;
    with_lock(*dc, [&] { std::tie(si, err) = ctx_driver_prepare(ctx, *dc->ci, query); });
    if (err) {
        release(err);
        return {nullptr, err};
    }
    auto ds = make_ptr<driver_stmt>();
    ds->dc = dc.get();
    ds->si = si;
    auto [rowsi, rerr] = rowsi_from_statement(ctx, *dc->ci, ds, args);
    if (rerr) {
        ds->Close();
        release(rerr);
        return {nullptr, rerr};
    }
    // The statement closes with the rows.
    ptr<Rows> rows(new Rows());
    rows->dc_ = dc;
    rows->release_conn_ = std::move(release);
    rows->rowsi_ = rowsi;
    rows->close_stmt_ = ds;
    rows->init_context_close(ctx, txctx);
    return {rows, nullptr};
}

ptr<Row> DB::query_row_args(ptr<context::Context> ctx, std::string_view query, std::vector<detail::arg> args) {
    auto [rows, err] = query_args(std::move(ctx), query, std::move(args));
    return make_ptr<Row>(rows, err);
}

std::pair<ptr<sql::Tx>, error> DB::BeginTx(ptr<context::Context> ctx, const TxOptions* opts) {
    ptr<sql::Tx> tx;
    error err = retry([&](conn_reuse_strategy strategy) {
        auto [dc, e] = conn(ctx, strategy);
        if (e) return e;
        auto [t, berr] = begin_dc(ctx, dc, [dc](error e) { dc->release_conn(std::move(e)); }, opts);
        tx = t;
        return berr;
    });
    return {tx, err};
}

std::pair<ptr<sql::Tx>, error> DB::Begin() { return BeginTx(context::Background(), nullptr); }

std::pair<ptr<sql::Tx>, error> DB::begin_dc(ptr<context::Context> ctx, const ptr<driver_conn>& dc, release_fn release,
                                            const TxOptions* opts) {
    ptr<driver::Tx> txi;
    bool keep_conn_on_rollback = false;
    error err;
    with_lock(*dc, [&] {
        bool has_session_resetter = dynamic_cast<driver::SessionResetter*>(dc->ci.get()) != nullptr;
        bool has_validator = dynamic_cast<driver::Validator*>(dc->ci.get()) != nullptr;
        keep_conn_on_rollback = has_session_resetter && has_validator;
        std::tie(txi, err) = ctx_driver_begin(ctx, opts, *dc->ci);
    });
    if (err) {
        release(err);
        return {nullptr, err};
    }
    // Schedule the transaction to roll back when the context is canceled.
    auto [tctx, cancel] = context::WithCancel(ctx);
    ptr<sql::Tx> tx(new sql::Tx());
    tx->db_ = ptr<DB>(this);
    tx->dc_ = dc;
    tx->release_conn_ = std::move(release);
    tx->txi_ = txi;
    tx->cancel_ = cancel;
    tx->keep_conn_on_rollback_ = keep_conn_on_rollback;
    tx->ctx_ = tctx;
    go([tx] { tx->await_done(); });
    return {tx, nullptr};
}

ptr<driver::Driver> DB::Driver() { return connector_->Driver(); }

std::pair<ptr<sql::Conn>, error> DB::Conn(ptr<context::Context> ctx) {
    ptr<driver_conn> dc;
    error err = retry([&](conn_reuse_strategy strategy) {
        auto [c, e] = conn(ctx, strategy);
        dc = c;
        return e;
    });
    if (err) return {nullptr, err};
    return {ptr<sql::Conn>(new sql::Conn(ptr<DB>(this), dc)), nullptr};
}

// ---------------------------------------------------------------------------
// Conn

Conn::~Conn() = default;

std::tuple<ptr<driver_conn>, release_fn, error> Conn::grab_conn(ptr<context::Context>) {
    if (done_.load()) return {nullptr, nullptr, ErrConnDone};
    closemu_.RLock();
    ptr<sql::Conn> self(this);
    return {dc_, [self](error err) { self->closemu_runlock_cond_release_conn(std::move(err)); }, nullptr};
}

error Conn::PingContext(ptr<context::Context> ctx) {
    auto [dc, release, err] = grab_conn(ctx);
    if (err) return err;
    return db_->ping_dc(ctx, dc, release);
}

std::pair<ptr<Result>, error> Conn::exec_args(ptr<context::Context> ctx, std::string_view query, std::vector<detail::arg> args) {
    auto [dc, release, err] = grab_conn(ctx);
    if (err) return {nullptr, err};
    return db_->exec_dc(ctx, dc, release, query, args);
}

std::pair<ptr<Rows>, error> Conn::query_args(ptr<context::Context> ctx, std::string_view query, std::vector<detail::arg> args) {
    auto [dc, release, err] = grab_conn(ctx);
    if (err) return {nullptr, err};
    return db_->query_dc(ctx, nullptr, dc, release, query, args);
}

ptr<Row> Conn::query_row_args(ptr<context::Context> ctx, std::string_view query, std::vector<detail::arg> args) {
    auto [rows, err] = query_args(std::move(ctx), query, std::move(args));
    return make_ptr<Row>(rows, err);
}

std::pair<ptr<sql::Stmt>, error> Conn::PrepareContext(ptr<context::Context> ctx, std::string_view query) {
    auto [dc, release, err] = grab_conn(ctx);
    if (err) return {nullptr, err};
    return db_->prepare_dc(ctx, dc, release, this, ptr<object>(static_cast<object*>(this)), query);
}

error Conn::Raw(const std::function<error(driver::Conn&)>& f) {
    auto [dc, release, err] = grab_conn(nullptr);
    if (err) return err;
    // A panic in f makes the connection bad.
    dc->mu.lock();
    try {
        err = f(*dc->ci);
    } catch (...) {
        dc->mu.unlock();
        release(driver::ErrBadConn);
        throw;
    }
    dc->mu.unlock();
    release(err);
    return err;
}

std::pair<ptr<sql::Tx>, error> Conn::BeginTx(ptr<context::Context> ctx, const TxOptions* opts) {
    auto [dc, release, err] = grab_conn(ctx);
    if (err) return {nullptr, err};
    return db_->begin_dc(ctx, dc, release, opts);
}

void Conn::closemu_runlock_cond_release_conn(error err) {
    closemu_.RUnlock();
    if (errors::Is(err, driver::ErrBadConn)) close(err);
}

error Conn::close(error err) {
    bool expected = false;
    if (!done_.compare_exchange_strong(expected, true)) return ErrConnDone;
    // Lock around releasing the driver connection to ensure all queries
    // have been stopped before doing so.
    std::lock_guard l(closemu_);
    dc_->release_conn(err);
    dc_ = nullptr;
    return err;
}

error Conn::Close() { return close(nullptr); }

// ---------------------------------------------------------------------------
// Tx

Tx::~Tx() = default;

void Tx::await_done() {
    // Wait for either the transaction to be committed or rolled back, or
    // for the associated context to be closed.
    ctx_->Done().recv();
    // Discard and close the connection used to ensure the transaction is
    // closed and the resources are released, unless the driver can reset
    // the session.
    rollback(!keep_conn_on_rollback_);
}

void Tx::close(error err) {
    release_conn_(std::move(err));
    dc_ = nullptr;
    txi_ = nullptr;
    // The Tx's statements refer to it (to grab its connection); drop them,
    // closed or not, so that the two don't keep each other alive. Go's GC
    // frees the pair; statements of a done Tx are unusable anyway.
    std::vector<ptr<sql::Stmt>> stmts;
    {
        std::lock_guard l(stmts_mu_);
        stmts.swap(stmts_);
    }
}

std::tuple<ptr<driver_conn>, release_fn, error> Tx::grab_conn(ptr<context::Context> ctx) {
    if (error err = ctx_done(ctx)) return {nullptr, nullptr, err};
    // closemu.RLock must come before the check for is_done to prevent the
    // Tx from closing while a query is executing.
    closemu_.RLock();
    if (is_done()) {
        closemu_.RUnlock();
        return {nullptr, nullptr, ErrTxDone};
    }
    ptr<sql::Tx> self(this);
    return {dc_, [self](error) { self->closemu_.RUnlock(); }, nullptr};
}

void Tx::close_prepared() {
    std::lock_guard l(stmts_mu_);
    for (auto& s : stmts_) s->Close();
}

error Tx::Commit() {
    // Check context first to avoid transaction leak.
    if (error err = ctx_->Err()) {
        if (done_.load()) return ErrTxDone;
        return err;
    }
    bool expected = false;
    if (!done_.compare_exchange_strong(expected, true)) return ErrTxDone;
    // Cancel the Tx to release any active R-closemu locks.
    cancel_();
    closemu_.Lock();
    closemu_.Unlock();
    error err;
    with_lock(*dc_, [&] { err = txi_->Commit(); });
    if (!errors::Is(err, driver::ErrBadConn)) close_prepared();
    close(err);
    return err;
}

error Tx::rollback(bool discard_conn) {
    bool expected = false;
    if (!done_.compare_exchange_strong(expected, true)) return ErrTxDone;
    cancel_();
    closemu_.Lock();
    closemu_.Unlock();
    error err;
    with_lock(*dc_, [&] { err = txi_->Rollback(); });
    if (!errors::Is(err, driver::ErrBadConn)) close_prepared();
    if (discard_conn) err = driver::ErrBadConn;
    close(err);
    return err;
}

error Tx::Rollback() { return rollback(false); }

std::pair<ptr<sql::Stmt>, error> Tx::PrepareContext(ptr<context::Context> ctx, std::string_view query) {
    auto [dc, release, err] = grab_conn(ctx);
    if (err) return {nullptr, err};
    auto [stmt, perr] = db_->prepare_dc(ctx, dc, release, this, ptr<object>(static_cast<object*>(this)), query);
    if (perr) return {nullptr, perr};
    std::lock_guard l(stmts_mu_);
    stmts_.push_back(stmt);
    return {stmt, nullptr};
}

std::pair<ptr<sql::Stmt>, error> Tx::Prepare(std::string_view query) { return PrepareContext(context::Background(), query); }

ptr<sql::Stmt> Tx::StmtContext(ptr<context::Context> ctx, const ptr<sql::Stmt>& stmt) {
    auto sticky = [](error err) {
        ptr<sql::Stmt> s(new sql::Stmt());
        s->sticky_err_ = std::move(err);
        return s;
    };
    auto [dc, release, err] = grab_conn(ctx);
    if (err) return sticky(err);
    struct releaser {
        release_fn& r;
        ~releaser() { r(nullptr); }
    } rel{release};
    if (db_ != stmt->db_) return sticky(errors::New("sql: Tx.Stmt: statement from different database used"));
    ptr<driver::Stmt> si;
    ptr<sql::Stmt> parent_stmt;
    stmt->mu_.lock();
    if (stmt->closed_ || stmt->cg_) {
        // The statement is closed or belongs to a Tx or Conn: prepare it
        // again on this transaction's connection.
        stmt->mu_.unlock();
        with_lock(*dc, [&] { std::tie(si, err) = ctx_driver_prepare(ctx, *dc->ci, stmt->query_); });
        if (err) return sticky(err);
    } else {
        stmt->remove_closed_stmt_locked();
        // Reuse a statement prepared on this connection, if any.
        for (auto& v : stmt->css_) {
            if (v.dc == dc) {
                si = v.ds->si;
                break;
            }
        }
        stmt->mu_.unlock();
        if (!si) {
            ptr<driver_stmt> ds;
            with_lock(*dc, [&] { std::tie(ds, err) = stmt->prepare_on_conn_locked(ctx, dc); });
            if (err) return sticky(err);
            si = ds->si;
        }
        parent_stmt = stmt;
    }
    ptr<sql::Stmt> txs(new sql::Stmt());
    txs->db_ = db_;
    txs->cg_ = this;
    txs->cg_keep_ = ptr<object>(static_cast<object*>(this));
    txs->cgds_ = make_ptr<driver_stmt>();
    txs->cgds_->dc = dc.get();
    txs->cgds_->si = si;
    txs->parent_stmt_ = parent_stmt;
    txs->query_ = stmt->query_;
    if (parent_stmt) db_->add_dep(parent_stmt.get(), parent_stmt, txs.get());
    std::lock_guard l(stmts_mu_);
    stmts_.push_back(txs);
    return txs;
}

ptr<sql::Stmt> Tx::Stmt(const ptr<sql::Stmt>& stmt) { return StmtContext(context::Background(), stmt); }

std::pair<ptr<Result>, error> Tx::exec_args(ptr<context::Context> ctx, std::string_view query, std::vector<detail::arg> args) {
    auto [dc, release, err] = grab_conn(ctx);
    if (err) return {nullptr, err};
    return db_->exec_dc(ctx, dc, release, query, args);
}

std::pair<ptr<Rows>, error> Tx::query_args(ptr<context::Context> ctx, std::string_view query, std::vector<detail::arg> args) {
    auto [dc, release, err] = grab_conn(ctx);
    if (err) return {nullptr, err};
    return db_->query_dc(ctx, ctx_, dc, release, query, args);
}

ptr<Row> Tx::query_row_args(ptr<context::Context> ctx, std::string_view query, std::vector<detail::arg> args) {
    auto [rows, err] = query_args(std::move(ctx), query, std::move(args));
    return make_ptr<Row>(rows, err);
}

// ---------------------------------------------------------------------------
// Stmt

Stmt::~Stmt() = default;

std::pair<ptr<Result>, error> Stmt::exec_args(ptr<context::Context> ctx, std::vector<detail::arg> args) {
    closemu_.RLock();
    struct unlocker {
        detail::closing_mutex& m;
        ~unlocker() { m.RUnlock(); }
    } u{closemu_};
    ptr<Result> res;
    error err = db_->retry([&](conn_reuse_strategy strategy) {
        auto [dc, release, ds, e] = conn_stmt(ctx, strategy);
        if (e) return e;
        auto [r, xerr] = result_from_statement(ctx, *dc->ci, ds, args);
        res = r;
        release(xerr);
        return xerr;
    });
    return {res, err};
}

void Stmt::remove_closed_stmt_locked() {
    // Clean the connections that closed since the last time, when enough
    // did.
    std::size_t t = std::min<std::size_t>(css_.size() / 2 + 1, 10);
    uint64 db_closed = db_->num_closed_.load();
    if (db_closed - last_num_closed_ < t) return;
    {
        std::lock_guard l(db_->mu_);
        std::erase_if(css_, [](const detail::conn_stmt& v) { return v.dc->dbmu_closed; });
    }
    last_num_closed_ = db_closed;
}

std::tuple<ptr<driver_conn>, release_fn, ptr<driver_stmt>, error> Stmt::conn_stmt(ptr<context::Context> ctx,
                                                                                   conn_reuse_strategy strategy) {
    if (sticky_err_) return {nullptr, nullptr, nullptr, sticky_err_};
    mu_.lock();
    if (closed_) {
        mu_.unlock();
        return {nullptr, nullptr, nullptr, errors::New("sql: statement is closed")};
    }
    // In a transaction or connection, there is one connection to use.
    if (cg_) {
        mu_.unlock();
        auto [dc, release, err] = cg_->grab_conn(ctx);  // blocks, waiting for the connection
        if (err) return {nullptr, nullptr, nullptr, err};
        return {dc, release, cgds_, nullptr};
    }
    remove_closed_stmt_locked();
    mu_.unlock();

    auto [dc, err] = db_->conn(ctx, strategy);
    if (err) return {nullptr, nullptr, nullptr, err};
    release_fn release = [dc](error e) { dc->release_conn(std::move(e)); };
    mu_.lock();
    for (auto& v : css_) {
        if (v.dc == dc) {
            mu_.unlock();
            return {dc, release, v.ds, nullptr};
        }
    }
    mu_.unlock();
    // No statement on this connection yet: prepare one.
    ptr<driver_stmt> ds;
    with_lock(*dc, [&] { std::tie(ds, err) = prepare_on_conn_locked(ctx, dc); });
    if (err) {
        dc->release_conn(err);
        return {nullptr, nullptr, nullptr, err};
    }
    return {dc, release, ds, nullptr};
}

std::pair<ptr<driver_stmt>, error> Stmt::prepare_on_conn_locked(ptr<context::Context> ctx, const ptr<driver_conn>& dc) {
    auto [si, err] = dc->prepare_locked(ctx, cg_ != nullptr, query_);
    if (err) return {nullptr, err};
    std::lock_guard l(mu_);
    css_.push_back({dc, si});
    return {si, nullptr};
}

std::pair<ptr<Rows>, error> Stmt::query_args(ptr<context::Context> ctx, std::vector<detail::arg> args) {
    closemu_.RLock();
    struct unlocker {
        detail::closing_mutex& m;
        ~unlocker() { m.RUnlock(); }
    } u{closemu_};
    ptr<Rows> rows;
    error err = db_->retry([&](conn_reuse_strategy strategy) {
        auto [dc, release, ds, e] = conn_stmt(ctx, strategy);
        if (e) return e;
        auto [rowsi, qerr] = rowsi_from_statement(ctx, *dc->ci, ds, args);
        if (!qerr) {
            // The rows depend on the statement, which can't close until
            // they do.
            ptr<Rows> r(new Rows());
            r->dc_ = dc;
            r->rowsi_ = rowsi;
            ptr<sql::Stmt> self(this);
            db_->add_dep(this, self, r.get());
            const void* rp = r.get();
            r->release_conn_ = [release, self, rp](error e) {
                release(std::move(e));
                self->db_->remove_dep(self.get(), rp);
            };
            ptr<context::Context> txctx;
            if (cg_) txctx = cg_->tx_ctx();
            r->init_context_close(ctx, txctx);
            rows = r;
            return error();
        }
        release(qerr);
        return qerr;
    });
    return {rows, err};
}

ptr<Row> Stmt::query_row_args(ptr<context::Context> ctx, std::vector<detail::arg> args) {
    auto [rows, err] = query_args(std::move(ctx), std::move(args));
    if (err) return make_ptr<Row>(nullptr, err);
    return make_ptr<Row>(rows, nullptr);
}

error Stmt::Close() {
    std::lock_guard l(closemu_);
    if (sticky_err_) return sticky_err_;
    ptr<driver_stmt> txds;
    {
        std::lock_guard ml(mu_);
        if (closed_) return nullptr;
        closed_ = true;
        txds = std::move(cgds_);
    }
    if (!cg_) return db_->remove_dep(this, this);
    if (parent_stmt_) {
        // A Tx statement of a DB statement: the driver statement belongs
        // to the parent.
        return db_->remove_dep(parent_stmt_.get(), this);
    }
    return txds->Close();
}

error Stmt::final_close() {
    std::lock_guard l(mu_);
    for (auto& v : css_) {
        db_->note_unused_driver_statement(v.dc, v.ds);
        v.dc->remove_open_stmt(v.ds.get());
    }
    css_.clear();
    return nullptr;
}

// ---------------------------------------------------------------------------
// Rows

Rows::~Rows() = default;

error Rows::lasterr_or_err_locked(error err) {
    if (lasterr_ && lasterr_ != io::EOF_) return lasterr_;
    return err;
}

void Rows::init_context_close(ptr<context::Context> ctx, ptr<context::Context> txctx) {
    if (!ctx->Done() && (!txctx || !txctx->Done())) return;
    auto [closectx, cancel] = context::WithCancel(ctx);
    cancel_ = cancel;
    ptr<Rows> self(this);
    go([self, ctx, txctx, closectx] { self->await_done(ctx, txctx, closectx); });
}

// Closes the rows when a context is done, unless they close first
// (closectx).
void Rows::await_done(ptr<context::Context> ctx, ptr<context::Context> txctx, ptr<context::Context> closectx) {
    chan<empty> txctx_done = txctx ? txctx->Done() : chan<empty>();
    auto set_done = [&](error err) {
        std::lock_guard l(context_done_mu_);
        context_done_ = std::move(err);
        has_context_done_.store(true);
    };
    select(recv(ctx->Done(), [&] { set_done(ctx->Err()); }), recv(txctx_done, [&] { set_done(txctx->Err()); }),
           recv(closectx->Done(), [] {}));
    close(ctx->Err());
}

bool Rows::Next() {
    // If the user's calling Next, they're done with their previous row's
    // Scan results (any RawBytes memory), so we can release the read lock.
    closemu_runlock_if_held_by_scan();
    if (has_context_done_.load()) return false;
    bool do_close, ok;
    {
        closemu_.RLock();
        struct unlocker {
            detail::closing_mutex& m;
            ~unlocker() { m.RUnlock(); }
        } u{closemu_};
        std::tie(do_close, ok) = next_locked();
    }
    if (do_close) Close();
    if (do_close && !ok) hit_eof_ = true;
    return ok;
}

std::pair<bool, bool> Rows::next_locked() {
    if (closed_) return {false, false};
    // Lock the driver connection before calling the driver interface
    // rowsi to prevent a Tx from rolling back the connection at the same
    // time.
    std::lock_guard l(*dc_);
    if (!next_called_) {
        num_cols_ = rowsi_->Columns().size();
        next_called_ = true;
    }
    if (lastcols_.size() != num_cols_) lastcols_.assign(num_cols_, nullptr);
    lasterr_ = rowsi_->Next(lastcols_);
    if (lasterr_) {
        // Close the connection if there is a driver error.
        if (lasterr_ != io::EOF_) return {true, false};
        auto* nrs = dynamic_cast<driver::RowsNextResultSet*>(rowsi_.get());
        if (!nrs) return {true, false};
        // The driver is at the end of the current result set. Test to see
        // if there is another result set after the current one. Only close
        // Rows if there is no further result set to read.
        return {!nrs->HasNextResultSet(), false};
    }
    return {false, true};
}

bool Rows::NextResultSet() {
    closemu_runlock_if_held_by_scan();
    bool do_close = false;
    bool result;
    {
        closemu_.RLock();
        struct unlocker {
            detail::closing_mutex& m;
            ~unlocker() { m.RUnlock(); }
        } u{closemu_};
        if (closed_) return false;
        lastcols_.clear();
        next_called_ = false;
        auto* nrs = dynamic_cast<driver::RowsNextResultSet*>(rowsi_.get());
        if (!nrs) {
            do_close = true;
            result = false;
        } else {
            std::lock_guard l(*dc_);
            lasterr_ = nrs->NextResultSet();
            if (lasterr_) {
                do_close = true;
                result = false;
            } else {
                result = true;
            }
        }
    }
    if (do_close) Close();
    return result;
}

error Rows::Err() {
    // Return any context error that might have occurred while iterating
    // over the rows; it takes precedence unless the end was reached.
    if (!hit_eof_ && has_context_done_.load()) {
        std::lock_guard l(context_done_mu_);
        return context_done_;
    }
    closemu_.RLock();
    error err = lasterr_or_err_locked(nullptr);
    closemu_.RUnlock();
    return err;
}

RawBytes Rows::setrawbuf(slice<byte> b) {
    std::size_t off = raw_.len();
    raw_ = std::move(b);
    return RawBytes(raw_.Sub(off));
}

std::pair<std::vector<string>, error> Rows::Columns() {
    closemu_.RLock();
    struct unlocker {
        detail::closing_mutex& m;
        ~unlocker() { m.RUnlock(); }
    } u{closemu_};
    if (closed_) return {{}, lasterr_or_err_locked(err_rows_closed)};
    if (!rowsi_) return {{}, lasterr_or_err_locked(err_no_rows_available)};
    std::lock_guard l(*dc_);
    return {rowsi_->Columns(), nullptr};
}

std::pair<std::vector<ptr<ColumnType>>, error> Rows::ColumnTypes() {
    closemu_.RLock();
    struct unlocker {
        detail::closing_mutex& m;
        ~unlocker() { m.RUnlock(); }
    } u{closemu_};
    if (closed_) return {{}, lasterr_or_err_locked(err_rows_closed)};
    if (!rowsi_) return {{}, lasterr_or_err_locked(err_no_rows_available)};
    std::lock_guard l(*dc_);
    auto names = rowsi_->Columns();
    std::vector<ptr<ColumnType>> list;
    for (std::size_t i = 0; i < names.size(); i++) {
        int idx = static_cast<int>(i);
        auto ci = make_ptr<ColumnType>();
        ci->name_ = names[i];
        if (auto* p = dynamic_cast<driver::RowsColumnTypeScanType*>(rowsi_.get())) ci->scan_type_ = p->ColumnTypeScanType(idx);
        else ci->scan_type_ = string::from_static("interface {}");
        if (auto* p = dynamic_cast<driver::RowsColumnTypeDatabaseTypeName*>(rowsi_.get())) {
            ci->database_type_ = p->ColumnTypeDatabaseTypeName(idx);
        }
        if (auto* p = dynamic_cast<driver::RowsColumnTypeLength*>(rowsi_.get())) {
            std::tie(ci->length_, ci->has_length_) = p->ColumnTypeLength(idx);
        }
        if (auto* p = dynamic_cast<driver::RowsColumnTypeNullable*>(rowsi_.get())) {
            std::tie(ci->nullable_, ci->has_nullable_) = p->ColumnTypeNullable(idx);
        }
        if (auto* p = dynamic_cast<driver::RowsColumnTypePrecisionScale*>(rowsi_.get())) {
            std::tie(ci->precision_, ci->scale_, ci->has_precision_scale_) = p->ColumnTypePrecisionScale(idx);
        }
        list.push_back(ci);
    }
    return {std::move(list), nullptr};
}

error Rows::scan(std::span<detail::dest> dest) {
    if (closemu_scan_hold_) {
        // This should only be possible if the user calls Scan twice in a
        // row without calling Next.
        return errors::New("sql: Scan called without calling Next (closemuScanHold)");
    }
    closemu_.RLock();
    raw_ = raw_.Sub(0, 0);
    error err = scan_locked(dest);
    bool has_raw = std::any_of(dest.begin(), dest.end(), [](const detail::dest& d) { return d.raw_bytes; });
    if (!err && has_raw) {
        // RawBytes point into memory the driver owns until the next call;
        // keep the rows from closing until then.
        closemu_scan_hold_ = true;
    } else {
        closemu_.RUnlock();
    }
    return err;
}

error Rows::scan_locked(std::span<detail::dest> dest) {
    if (lasterr_ && lasterr_ != io::EOF_) return lasterr_;
    if (closed_) return lasterr_or_err_locked(err_rows_closed);
    if (!next_called_) return errors::New("sql: Scan called without calling Next");
    if (dest.size() != num_cols_) {
        return errors::New("sql: expected " + strconv::Itoa(static_cast<int64>(num_cols_)) + " destination arguments in Scan, not " +
                           strconv::Itoa(static_cast<int64>(dest.size())));
    }
    for (std::size_t i = 0; i < lastcols_.size(); i++) {
        if (error err = dest[i].assign(dest[i].p, lastcols_[i], this)) {
            auto cols = rowsi_->Columns();
            return detail::wrapf("sql: Scan error on column index " + strconv::Itoa(static_cast<int64>(i)) + ", name " +
                                     detail::quote(cols[i]) + ": " + err.Error(),
                                 err);
        }
    }
    return nullptr;
}

void Rows::closemu_runlock_if_held_by_scan() {
    if (closemu_scan_hold_) {
        closemu_scan_hold_ = false;
        closemu_.RUnlock();
    }
}

error Rows::Close() {
    // If the user's calling Close, they're done with their previous row's
    // Scan results (any RawBytes memory), so we can release the read lock
    // that would be preventing that close.
    closemu_runlock_if_held_by_scan();
    return close(nullptr);
}

error Rows::close(error err) {
    std::lock_guard l(closemu_);
    if (closed_) return nullptr;
    closed_ = true;
    if (!lasterr_) lasterr_ = err;
    with_lock(*dc_, [&] { err = rowsi_->Close(); });
    if (detail::rows_close_hook) detail::rows_close_hook(*this, err);
    if (cancel_) cancel_();
    if (close_stmt_) close_stmt_->Close();
    release_conn_(err);
    lasterr_ = lasterr_or_err_locked(err);
    return err;
}

// ---------------------------------------------------------------------------
// Row

error Row::scan(std::span<detail::dest> dest) {
    if (err_) return err_;
    struct closer {
        ptr<Rows>& r;
        ~closer() { r->Close(); }
    } c{rows_};
    for (auto& d : dest) {
        if (d.raw_bytes) return errors::New("sql: RawBytes isn't allowed on Row.Scan");
    }
    if (!rows_->Next()) {
        if (error err = rows_->Err()) return err;
        return ErrNoRows;
    }
    if (error err = rows_->scan(dest)) return err;
    // Make sure the query can be processed to completion with no errors.
    return rows_->Close();
}

}  // namespace gopp::sql
