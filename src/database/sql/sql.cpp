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

using detail::connReuseStrategy;
using detail::driverConn;
using detail::driverStmt;
using detail::releaseFn;

namespace {

sync::RWMutex driversMu;
std::map<std::string, ptr<driver::Driver>, std::less<>>& drivers() {
    static auto* m = new std::map<std::string, ptr<driver::Driver>, std::less<>>();
    return *m;
}

const error errDbClosed = errors::New("sql: database is closed");
const error errRowsClosed = errors::New("sql: Rows are closed");
const error errNoRowsAvailable = errors::New("sql: no Rows available");

constexpr int defaultMaxIdleConns = 2;
constexpr int maxBadConnRetries = 2;

template <typename F>
void withLock(driverConn& dc, F&& f) {
    std::lock_guard l(dc);
    f();
}

// Whether ctx is done, without blocking (select with a default case).
error ctxDone(const ptr<context::Context>& ctx) { return ctx ? ctx->Err() : nullptr; }

// ---------------------------------------------------------------------------
// ctxutil.go

std::pair<std::vector<driver::Value>, error> namedValueToValue(std::span<const driver::NamedValue> named) {
    std::vector<driver::Value> dargs;
    dargs.reserve(named.size());
    for (auto& p : named) {
        if (!p.Name.empty()) return {{}, errors::New("sql: driver does not support the use of Named Parameters")};
        dargs.push_back(p.Value);
    }
    return {std::move(dargs), nullptr};
}

std::pair<ptr<driver::Stmt>, error> ctxDriverPrepare(const ptr<context::Context>& ctx, driver::Conn& ci,
                                                       std::string_view query) {
    if (auto* c = dynamic_cast<driver::ConnPrepareContext*>(&ci)) return c->PrepareContext(ctx, query);
    auto [si, err] = ci.Prepare(query);
    if (!err) {
        if (error cerr = ctxDone(ctx)) {
            si->Close();
            return {nullptr, cerr};
        }
    }
    return {si, err};
}

std::pair<ptr<driver::Result>, error> ctxDriverExec(const ptr<context::Context>& ctx, driver::ExecerContext* execerCtx,
                                                      driver::Execer* execer, std::string_view query,
                                                      std::span<const driver::NamedValue> nvdargs) {
    if (execerCtx) return execerCtx->ExecContext(ctx, query, nvdargs);
    auto [dargs, err] = namedValueToValue(nvdargs);
    if (err) return {nullptr, err};
    if (error cerr = ctxDone(ctx)) return {nullptr, cerr};
    return execer->Exec(query, dargs);
}

std::pair<ptr<driver::Rows>, error> ctxDriverQuery(const ptr<context::Context>& ctx, driver::QueryerContext* queryerCtx,
                                                     driver::Queryer* queryer, std::string_view query,
                                                     std::span<const driver::NamedValue> nvdargs) {
    if (queryerCtx) return queryerCtx->QueryContext(ctx, query, nvdargs);
    auto [dargs, err] = namedValueToValue(nvdargs);
    if (err) return {nullptr, err};
    if (error cerr = ctxDone(ctx)) return {nullptr, cerr};
    return queryer->Query(query, dargs);
}

std::pair<ptr<driver::Result>, error> ctxDriverStmtExec(const ptr<context::Context>& ctx, driver::Stmt& si,
                                                           std::span<const driver::NamedValue> nvdargs) {
    if (auto* s = dynamic_cast<driver::StmtExecContext*>(&si)) return s->ExecContext(ctx, nvdargs);
    auto [dargs, err] = namedValueToValue(nvdargs);
    if (err) return {nullptr, err};
    if (error cerr = ctxDone(ctx)) return {nullptr, cerr};
    return si.Exec(dargs);
}

std::pair<ptr<driver::Rows>, error> ctxDriverStmtQuery(const ptr<context::Context>& ctx, driver::Stmt& si,
                                                          std::span<const driver::NamedValue> nvdargs) {
    if (auto* s = dynamic_cast<driver::StmtQueryContext*>(&si)) return s->QueryContext(ctx, nvdargs);
    auto [dargs, err] = namedValueToValue(nvdargs);
    if (err) return {nullptr, err};
    if (error cerr = ctxDone(ctx)) return {nullptr, cerr};
    return si.Query(dargs);
}

std::pair<ptr<driver::Tx>, error> ctxDriverBegin(const ptr<context::Context>& ctx, const TxOptions* opts, driver::Conn& ci) {
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
        if (error cerr = ctxDone(ctx)) {
            txi->Rollback();
            return {nullptr, cerr};
        }
    }
    return {txi, err};
}

// The Result of a driver, called under its connection's lock.
struct driverResult final : Result {
    ptr<driverConn> dc;
    ptr<driver::Result> resi;
    driverResult(ptr<driverConn> c, ptr<driver::Result> r) : dc(std::move(c)), resi(std::move(r)) {}
    std::pair<int64, error> LastInsertId() override {
        std::lock_guard l(*dc);
        return resi->LastInsertId();
    }
    std::pair<int64, error> RowsAffected() override {
        std::lock_guard l(*dc);
        return resi->RowsAffected();
    }
};

std::pair<ptr<Result>, error> resultFromStatement(const ptr<context::Context>& ctx, driver::Conn& ci,
                                                    const ptr<driverStmt>& ds, std::span<const detail::arg> args) {
    std::lock_guard l(*ds);
    auto [dargs, err] = detail::driverArgsConnLocked(&ci, ds.get(), args);
    if (err) return {nullptr, err};
    auto [resi, rerr] = ctxDriverStmtExec(ctx, *ds->si, dargs);
    if (rerr) return {nullptr, rerr};
    return {make_ptr<driverResult>(ptr<driverConn>(ds->dc), resi), nullptr};
}

std::pair<ptr<driver::Rows>, error> rowsiFromStatement(const ptr<context::Context>& ctx, driver::Conn& ci,
                                                         const ptr<driverStmt>& ds, std::span<const detail::arg> args) {
    std::lock_guard l(*ds);
    auto [dargs, err] = detail::driverArgsConnLocked(&ci, ds.get(), args);
    if (err) return {nullptr, err};
    return ctxDriverStmtQuery(ctx, *ds->si, dargs);
}

}  // namespace

// ---------------------------------------------------------------------------
// Drivers

void Register(std::string_view name, ptr<driver::Driver> d) {
    std::lock_guard l(driversMu);
    if (!d) panic("sql: Register driver is nil");
    if (drivers().contains(name)) panic("sql: Register called twice for driver " + std::string(name));
    drivers().emplace(std::string(name), std::move(d));
}

std::vector<string> Drivers() {
    std::shared_lock l(driversMu);
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

std::function<void(Rows&, error&)> rowsCloseHook;

void closingMutex::RLock() {
    if (TryRLock()) return;
    std::lock_guard l(mu_);
    for (;;) {
        if (TryRLock()) return;
        read_.Wait();
    }
}

void closingMutex::RUnlock() {
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

void closingMutex::Lock() {
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

void closingMutex::Unlock() {
    std::lock_guard l(mu_);
    int64 x = -1;
    if (!state_.compare_exchange_strong(x, 0)) panic("unlock of unlocked mutex");
    read_.Broadcast();
    write_.Broadcast();
}

bool closingMutex::TryRLock() {
    for (;;) {
        int64 x = state_.load();
        if (x < 0 || x == 1) return false;
        if (state_.compare_exchange_strong(x, x + 2)) return true;
    }
}

// ---------------------------------------------------------------------------
// connRequestSet

connRequestSet::handle connRequestSet::Add(chan<connRequest> c) {
    auto idx = std::make_shared<int>(static_cast<int>(s_.size()));
    s_.emplace_back(std::move(c), idx);
    return idx;
}

bool connRequestSet::Delete(const handle& h) {
    int idx = *h;
    if (idx < 0) return false;
    deleteIndex(static_cast<std::size_t>(idx));
    return true;
}

void connRequestSet::deleteIndex(std::size_t idx) {
    *s_[idx].second = -1;
    if (idx < s_.size() - 1) {
        *s_.back().second = static_cast<int>(idx);
        s_[idx] = std::move(s_.back());
    }
    s_.pop_back();
}

std::optional<chan<connRequest>> connRequestSet::TakeRandom() {
    if (s_.empty()) return std::nullopt;
    thread_local std::minstd_rand rng(std::random_device{}());
    std::size_t pick = rng() % s_.size();
    chan<connRequest> c = s_[pick].first;
    deleteIndex(pick);
    return c;
}

void connRequestSet::CloseAndRemoveAll() {
    for (auto& [c, idx] : s_) {
        *idx = -1;
        c.close();
    }
    s_.clear();
}

// ---------------------------------------------------------------------------
// driverConn, driverStmt

void driverConn::releaseConn(error err) { db->putConn(ptr<driverConn>(this), std::move(err), true); }

void driverConn::removeOpenStmt(driverStmt* ds) {
    std::lock_guard l(mu);
    openStmt.erase(ds);
}

bool driverConn::expired(time::Duration timeout) const {
    if (timeout.ns <= 0) return false;
    return createdAt.Add(timeout).Before(time::Now());
}

error driverConn::resetSession(ptr<context::Context> ctx) {
    std::lock_guard l(mu);
    if (!needReset) return nullptr;
    if (auto* cr = dynamic_cast<driver::SessionResetter*>(ci.get())) return cr->ResetSession(std::move(ctx));
    return nullptr;
}

bool driverConn::validateConnection(bool needsReset) {
    std::lock_guard l(mu);
    if (needsReset) needReset = true;
    if (auto* cv = dynamic_cast<driver::Validator*>(ci.get())) return cv->IsValid();
    return true;
}

std::pair<ptr<driverStmt>, error> driverConn::prepareLocked(ptr<context::Context> ctx, bool grabbed,
                                                               std::string_view query) {
    auto [si, err] = ctxDriverPrepare(ctx, *ci, query);
    if (err) return {nullptr, err};
    auto ds = make_ptr<driverStmt>();
    ds->dc = this;
    ds->si = si;
    // Statements of a Tx or Conn close with them, not with the connection.
    if (grabbed) return {ds, nullptr};
    openStmt[ds.get()] = ds;
    return {ds, nullptr};
}

std::function<error()> driverConn::closeDbLocked() {
    std::lock_guard l(mu);
    if (closed) return [] { return errors::New("sql: duplicate driverConn close"); };
    closed = true;
    return db->removeDepLocked(this, this);
}

error driverConn::Close() {
    {
        std::lock_guard l(mu);
        if (closed) return errors::New("sql: duplicate driverConn close");
        closed = true;
    }
    std::function<error()> fn;
    {
        std::lock_guard l(db->mu_);
        dbmuClosed = true;
        fn = db->removeDepLocked(this, this);
    }
    return fn();
}

error driverConn::finalClose() {
    ptr<driverConn> self(this);
    std::vector<ptr<driverStmt>> stmts;
    {
        std::lock_guard l(mu);
        for (auto& [k, ds] : openStmt) stmts.push_back(ds);
        openStmt.clear();
    }
    for (auto& ds : stmts) ds->Close();
    error err;
    {
        std::lock_guard l(mu);
        finalClosed = true;
        err = ci->Close();
        ci = nullptr;
    }
    {
        std::lock_guard l(db->mu_);
        db->numOpen_--;
        db->maybeOpenNewConnections();
    }
    db->numClosed_.fetch_add(1);
    return err;
}

error driverStmt::Close() {
    std::lock_guard l(*dc);
    if (closed) return closeErr;
    closed = true;
    closeErr = si->Close();
    return closeErr;
}

}  // namespace detail

// ---------------------------------------------------------------------------
// DB: dependencies

void DB::addDep(detail::finalCloser* x, ptr<object> keep, const void* dep) {
    std::lock_guard l(mu_);
    addDepLocked(x, std::move(keep), dep);
}

void DB::addDepLocked(detail::finalCloser* x, ptr<object> keep, const void* dep) {
    auto& e = dep_[x];
    if (!e.keep) e.keep = std::move(keep);
    e.deps.insert(dep);
}

error DB::removeDep(detail::finalCloser* x, const void* dep) {
    std::function<error()> fn;
    {
        std::lock_guard l(mu_);
        fn = removeDepLocked(x, dep);
    }
    return fn();
}

std::function<error()> DB::removeDepLocked(detail::finalCloser* x, const void* dep) {
    auto it = dep_.find(x);
    if (it == dep_.end()) panic("unpaired removeDep: no deps");
    if (it->second.deps.erase(dep) == 0) panic("unpaired removeDep: no such dep");
    if (!it->second.deps.empty()) return [] { return error(); };
    ptr<object> keep = std::move(it->second.keep);
    dep_.erase(it);
    return [x, keep] { return x->finalClose(); };
}

// ---------------------------------------------------------------------------
// DB: opening and closing

namespace {

// A Connector for a driver and a data source name.
struct dsnConnector final : driver::Connector {
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
    db->openerCh_ = make_chan<empty>(1);
    db->stop_ = cancel;
    go([db, ctx] { db->connectionOpener(ctx); });
    return db;
}

std::pair<ptr<DB>, error> Open(std::string_view driverName, std::string_view dataSourceName) {
    ptr<driver::Driver> d;
    {
        std::shared_lock l(driversMu);
        auto it = drivers().find(driverName);
        if (it == drivers().end()) {
            return {nullptr, errors::New("sql: unknown driver " + detail::quote(driverName) + " (forgotten import?)")};
        }
        d = it->second;
    }
    if (auto* dc = dynamic_cast<driver::DriverContext*>(d.get())) {
        auto [connector, err] = dc->OpenConnector(dataSourceName);
        if (err) return {nullptr, err};
        return {OpenDB(connector), nullptr};
    }
    auto c = make_ptr<dsnConnector>();
    c->dsn = string(dataSourceName);
    c->drv = d;
    return {OpenDB(c), nullptr};
}

error DB::pingDc(ptr<context::Context> ctx, const ptr<driverConn>& dc, const releaseFn& release) {
    error err;
    if (auto* pinger = dynamic_cast<driver::Pinger*>(dc->ci.get())) {
        withLock(*dc, [&] { err = pinger->Ping(ctx); });
    }
    release(err);
    return err;
}

error DB::PingContext(ptr<context::Context> ctx) {
    ptr<driverConn> dc;
    error err = retry([&](connReuseStrategy strategy) {
        auto [c, e] = conn(ctx, strategy);
        dc = c;
        return e;
    });
    if (err) return err;
    return pingDc(ctx, dc, [dc](error e) { dc->releaseConn(std::move(e)); });
}

error DB::Ping() { return PingContext(context::Background()); }

error DB::Close() {
    std::vector<std::function<error()>> fns;
    {
        std::lock_guard l(mu_);
        if (closed_) return nullptr;  // idempotent
        if (cleanerCh_) cleanerCh_.close();
        for (auto& dc : freeConn_) fns.push_back(dc->closeDbLocked());
        freeConn_.clear();
        closed_ = true;
        connRequests_.CloseAndRemoveAll();
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

int DB::maxIdleConnsLocked() const {
    int n = maxIdleCount_;
    if (n == 0) return defaultMaxIdleConns;
    if (n < 0) return 0;
    return n;
}

time::Duration DB::shortestIdleTimeLocked() const {
    if (maxIdleTime_.ns <= 0) return maxLifetime_;
    if (maxLifetime_.ns <= 0) return maxIdleTime_;
    return std::min(maxIdleTime_, maxLifetime_);
}

void DB::SetMaxIdleConns(int n) {
    std::vector<ptr<driverConn>> closing;
    {
        std::lock_guard l(mu_);
        maxIdleCount_ = n > 0 ? n : -1;
        // Make sure maxIdle doesn't exceed maxOpen.
        if (maxOpen_ > 0 && maxIdleConnsLocked() > maxOpen_) maxIdleCount_ = maxOpen_;
        int maxIdle = maxIdleConnsLocked();
        if (static_cast<int>(freeConn_.size()) > maxIdle) {
            closing.assign(freeConn_.begin() + maxIdle, freeConn_.end());
            freeConn_.resize(static_cast<std::size_t>(maxIdle));
        }
        maxIdleClosed_ += static_cast<int64>(closing.size());
    }
    for (auto& c : closing) c->Close();
}

void DB::SetMaxOpenConns(int n) {
    bool syncMaxIdle;
    {
        std::lock_guard l(mu_);
        maxOpen_ = n < 0 ? 0 : n;
        syncMaxIdle = maxOpen_ > 0 && maxIdleConnsLocked() > maxOpen_;
    }
    if (syncMaxIdle) SetMaxIdleConns(n);
}

void DB::SetConnMaxLifetime(time::Duration d) {
    if (d.ns < 0) d = {};
    std::lock_guard l(mu_);
    // Wake the cleaner to apply a shorter lifetime.
    if (d.ns > 0 && d < shortestIdleTimeLocked() && cleanerCh_) cleanerCh_.trySend(empty{});
    maxLifetime_ = d;
    startCleanerLocked();
}

void DB::SetConnMaxIdleTime(time::Duration d) {
    if (d.ns < 0) d = {};
    std::lock_guard l(mu_);
    if (d.ns > 0 && d < shortestIdleTimeLocked() && cleanerCh_) cleanerCh_.trySend(empty{});
    maxIdleTime_ = d;
    startCleanerLocked();
}

void DB::startCleanerLocked() {
    if ((maxLifetime_.ns > 0 || maxIdleTime_.ns > 0) && numOpen_ > 0 && !cleanerCh_) {
        cleanerCh_ = make_chan<empty>(1);
        ptr<DB> self(this);
        time::Duration d = shortestIdleTimeLocked();
        go([self, d] { self->connectionCleaner(d); });
    }
}

void DB::connectionCleaner(time::Duration d) {
    const time::Duration minInterval = time::Second;
    if (d < minInterval) d = minInterval;
    ptr<time::Timer> t = time::NewTimer(d);
    chan<empty> cleaner;
    {
        std::lock_guard l(mu_);
        cleaner = cleanerCh_;
    }
    for (;;) {
        select(recv(t->C, [] {}), recv(cleaner, [] {}));  // maxLifetime changed or the db closed
        std::vector<ptr<driverConn>> closing;
        {
            std::lock_guard l(mu_);
            d = shortestIdleTimeLocked();
            if (closed_ || numOpen_ == 0 || d.ns <= 0) {
                cleanerCh_ = nullptr;
                return;
            }
            std::tie(d, closing) = connectionCleanerRunLocked(d);
        }
        for (auto& c : closing) c->Close();
        if (d < minInterval) d = minInterval;
        if (!t->Stop()) {
            time::Time v;
            t->C.tryRecv(v);
        }
        t->Reset(d);
    }
}

std::pair<time::Duration, std::vector<ptr<driverConn>>> DB::connectionCleanerRunLocked(time::Duration d) {
    int64 idleClosing = 0;
    std::vector<ptr<driverConn>> closing;
    if (maxIdleTime_.ns > 0) {
        // Connections returned before idleSince have been idle too long;
        // freeConn_ is ordered by returnedAt.
        time::Time idleSince = time::Now().Add(-maxIdleTime_);
        for (std::size_t i = freeConn_.size(); i-- > 0;) {
            if (freeConn_[i]->returnedAt.Before(idleSince)) {
                closing.assign(freeConn_.begin(), freeConn_.begin() + static_cast<std::ptrdiff_t>(i + 1));
                freeConn_.erase(freeConn_.begin(), freeConn_.begin() + static_cast<std::ptrdiff_t>(i + 1));
                idleClosing = static_cast<int64>(closing.size());
                maxIdleTimeClosed_ += idleClosing;
                break;
            }
        }
        if (!freeConn_.empty()) {
            if (time::Duration d2 = freeConn_[0]->returnedAt.Sub(idleSince); d2 < d) d = d2;
        }
    }
    if (maxLifetime_.ns > 0) {
        time::Time expiredSince = time::Now().Add(-maxLifetime_);
        for (std::size_t i = 0; i < freeConn_.size(); i++) {
            auto& c = freeConn_[i];
            if (c->createdAt.Before(expiredSince)) {
                closing.push_back(c);
                freeConn_.erase(freeConn_.begin() + static_cast<std::ptrdiff_t>(i));
                i--;
            } else if (time::Duration d2 = c->createdAt.Sub(expiredSince); d2 < d) {
                d = d2;
            }
        }
        maxLifetimeClosed_ += static_cast<int64>(closing.size()) - idleClosing;
    }
    return {d, std::move(closing)};
}

DBStats DB::Stats() {
    int64 wait = waitDuration_.load();
    std::lock_guard l(mu_);
    DBStats s;
    s.MaxOpenConnections = maxOpen_;
    s.Idle = static_cast<int>(freeConn_.size());
    s.OpenConnections = numOpen_;
    s.InUse = numOpen_ - static_cast<int>(freeConn_.size());
    s.WaitCount = waitCount_;
    s.WaitDuration = time::Duration(wait);
    s.MaxIdleClosed = maxIdleClosed_;
    s.MaxIdleTimeClosed = maxIdleTimeClosed_;
    s.MaxLifetimeClosed = maxLifetimeClosed_;
    return s;
}

// Asks the opener for connections the waiting requests need, within
// maxOpen_.
void DB::maybeOpenNewConnections() {
    int numRequests = static_cast<int>(connRequests_.Len());
    if (maxOpen_ > 0) numRequests = std::min(numRequests, maxOpen_ - numOpen_);
    for (; numRequests > 0; numRequests--) {
        numOpen_++;  // optimistically
        if (closed_) return;
        pendingOpens_++;
        openerCh_.trySend(empty{});
    }
}

void DB::connectionOpener(ptr<context::Context> ctx) {
    for (;;) {
        bool done = false;
        select(recv(ctx->Done(), [&] { done = true; }), recv(openerCh_, [] {}));
        if (done) return;
        for (;;) {
            {
                std::lock_guard l(mu_);
                if (pendingOpens_ == 0) break;
                pendingOpens_--;
            }
            openNewConnection(ctx);
        }
    }
}

void DB::openNewConnection(ptr<context::Context> ctx) {
    // numOpen_ was incremented for this connection already.
    auto [ci, err] = connector_->Connect(ctx);
    std::lock_guard l(mu_);
    if (closed_) {
        if (!err) ci->Close();
        numOpen_--;
        return;
    }
    if (err) {
        numOpen_--;
        putConnDbLocked(nullptr, err);
        maybeOpenNewConnections();
        return;
    }
    auto dc = make_ptr<driverConn>();
    dc->db = this;
    dc->createdAt = dc->returnedAt = time::Now();
    dc->ci = ci;
    if (putConnDbLocked(dc, nullptr)) {
        addDepLocked(dc.get(), dc, dc.get());
    } else {
        numOpen_--;
        ci->Close();
    }
}

// Returns a connection: a cached one, a new one, or one released by
// another fiber while this one waits (maxOpen_ reached).
std::pair<ptr<driverConn>, error> DB::conn(ptr<context::Context> ctx, connReuseStrategy strategy) {
    mu_.lock();
    if (closed_) {
        mu_.unlock();
        return {nullptr, errDbClosed};
    }
    if (error err = ctxDone(ctx)) {
        mu_.unlock();
        return {nullptr, err};
    }
    time::Duration lifetime = maxLifetime_;

    // Prefer a free connection, the newest.
    if (strategy == detail::cachedOrNewConn && !freeConn_.empty()) {
        ptr<driverConn> c = std::move(freeConn_.back());
        freeConn_.pop_back();
        c->inUse = true;
        if (c->expired(lifetime)) {
            maxLifetimeClosed_++;
            mu_.unlock();
            c->Close();
            return {nullptr, driver::ErrBadConn};
        }
        mu_.unlock();
        // Reset the session before use.
        if (error err = c->resetSession(ctx); errors::Is(err, driver::ErrBadConn)) {
            c->Close();
            return {nullptr, err};
        }
        return {c, nullptr};
    }

    // Out of connections: wait for one to be released.
    if (maxOpen_ > 0 && numOpen_ >= maxOpen_) {
        auto req = make_chan<detail::connRequest>(1);
        auto delHandle = connRequests_.Add(req);
        waitCount_++;
        mu_.unlock();
        time::Time waitStart = time::Now();

        std::optional<std::pair<detail::connRequest, bool>> got;
        error ctxErr;
        select(recv(ctx->Done(), [&] { ctxErr = ctx->Err(); }),
               recv(req, [&](detail::connRequest r, bool ok) { got.emplace(std::move(r), ok); }));
        waitDuration_.fetch_add(time::Since(waitStart).ns);
        if (!got) {
            // Remove the request so that no one sends to it; if someone did
            // already, put the connection back.
            bool deleted;
            {
                std::lock_guard l(mu_);
                deleted = connRequests_.Delete(delHandle);
            }
            if (!deleted) {
                detail::connRequest r;
                bool closed = false;
                if (req.tryRecv(r, &closed) && !closed && r.conn) putConn(r.conn, r.err, false);
            }
            return {nullptr, ctxErr};
        }
        auto& [ret, ok] = *got;
        if (!ok) return {nullptr, errDbClosed};
        // Only check if the connection is expired if the strategy is
        // cachedOrNewConn; when the strategy is alwaysNewConn, a bad
        // connection may be returned once more.
        if (strategy == detail::cachedOrNewConn && !ret.err && ret.conn->expired(lifetime)) {
            {
                std::lock_guard l(mu_);
                maxLifetimeClosed_++;
            }
            ret.conn->Close();
            return {nullptr, driver::ErrBadConn};
        }
        if (!ret.conn) return {nullptr, ret.err};
        if (error err = ret.conn->resetSession(ctx); errors::Is(err, driver::ErrBadConn)) {
            ret.conn->Close();
            return {nullptr, err};
        }
        return {ret.conn, ret.err};
    }

    numOpen_++;  // optimistically
    mu_.unlock();
    auto [ci, err] = connector_->Connect(ctx);
    if (err) {
        std::lock_guard l(mu_);
        numOpen_--;  // correct for the optimism
        maybeOpenNewConnections();
        return {nullptr, err};
    }
    std::lock_guard l(mu_);
    auto dc = make_ptr<driverConn>();
    dc->db = this;
    dc->createdAt = dc->returnedAt = time::Now();
    dc->ci = ci;
    dc->inUse = true;
    addDepLocked(dc.get(), dc, dc.get());
    return {dc, nullptr};
}

void DB::noteUnusedDriverStatement(const ptr<driverConn>& c, const ptr<driverStmt>& ds) {
    std::lock_guard l(mu_);
    if (c->inUse) {
        c->onPut.push_back([ds] { ds->Close(); });
    } else {
        bool fc;
        {
            std::lock_guard cl(c->mu);
            fc = c->finalClosed;
        }
        if (!fc) ds->Close();
    }
}

void DB::putConn(ptr<driverConn> dc, error err, bool resetSession) {
    if (!errors::Is(err, driver::ErrBadConn)) {
        if (!dc->validateConnection(resetSession)) err = driver::ErrBadConn;
    }
    mu_.lock();
    if (!dc->inUse) {
        mu_.unlock();
        panic("sql: connection returned that was never out");
    }
    if (!errors::Is(err, driver::ErrBadConn) && dc->expired(maxLifetime_)) {
        maxLifetimeClosed_++;
        err = driver::ErrBadConn;
    }
    dc->inUse = false;
    dc->returnedAt = time::Now();
    auto onPut = std::move(dc->onPut);
    dc->onPut.clear();
    for (auto& fn : onPut) fn();

    if (errors::Is(err, driver::ErrBadConn)) {
        // Don't reuse bad connections. Since the conn is considered bad
        // and is being discarded, treat it as closed; don't decrement the
        // open count here, final_close will.
        maybeOpenNewConnections();
        mu_.unlock();
        dc->Close();
        return;
    }
    bool added = putConnDbLocked(dc, nullptr);
    mu_.unlock();
    if (!added) dc->Close();
}

// Hands dc (or err) to a waiting request, or keeps dc free. False if dc
// should be closed.
bool DB::putConnDbLocked(const ptr<driverConn>& dc, error err) {
    if (closed_) return false;
    if (maxOpen_ > 0 && numOpen_ > maxOpen_) return false;
    if (auto req = connRequests_.TakeRandom()) {
        if (!err) dc->inUse = true;
        req->send(detail::connRequest{dc, err});
        return true;
    }
    if (!err && !closed_) {
        if (maxIdleConnsLocked() > static_cast<int>(freeConn_.size())) {
            freeConn_.push_back(dc);
            startCleanerLocked();
            return true;
        }
        maxIdleClosed_++;
    }
    return false;
}

// Runs fn with cached connections, then with a new one, while it fails
// with ErrBadConn.
error DB::retry(const std::function<error(connReuseStrategy)>& fn) {
    for (int i = 0; i < maxBadConnRetries; i++) {
        error err = fn(detail::cachedOrNewConn);
        if (!err || !errors::Is(err, driver::ErrBadConn)) return err;
    }
    return fn(detail::alwaysNewConn);
}

// ---------------------------------------------------------------------------
// DB: statements and queries

std::pair<ptr<sql::Stmt>, error> DB::PrepareContext(ptr<context::Context> ctx, std::string_view query) {
    ptr<sql::Stmt> stmt;
    error err = retry([&](connReuseStrategy strategy) {
        auto [dc, e] = conn(ctx, strategy);
        if (e) return e;
        auto [s, perr] = prepareDc(ctx, dc, [dc](error e) { dc->releaseConn(std::move(e)); }, nullptr, nullptr, query);
        stmt = s;
        return perr;
    });
    return {stmt, err};
}

std::pair<ptr<sql::Stmt>, error> DB::Prepare(std::string_view query) { return PrepareContext(context::Background(), query); }

std::pair<ptr<sql::Stmt>, error> DB::prepareDc(ptr<context::Context> ctx, const ptr<driverConn>& dc, releaseFn release,
                                                detail::stmtConnGrabber* cg, ptr<object> cgKeep, std::string_view query) {
    ptr<driverStmt> ds;
    error err;
    withLock(*dc, [&] { std::tie(ds, err) = dc->prepareLocked(ctx, cg != nullptr, query); });
    release(err);
    if (err) return {nullptr, err};
    // A statement on a Tx or Conn stays on its connection; others note
    // where they are prepared and prepare again where needed.
    ptr<sql::Stmt> stmt(new sql::Stmt());
    stmt->db_ = ptr<DB>(this);
    stmt->query_ = string(query);
    stmt->cg_ = cg;
    stmt->cgKeep_ = std::move(cgKeep);
    stmt->cgds_ = ds;
    if (!cg) {
        stmt->css_.push_back({dc, ds});
        stmt->lastNumClosed_ = numClosed_.load();
        addDep(stmt.get(), stmt, stmt.get());
    }
    return {stmt, nullptr};
}

std::pair<ptr<Result>, error> DB::execArgs(ptr<context::Context> ctx, std::string_view query, std::vector<detail::arg> args) {
    ptr<Result> res;
    error err = retry([&](connReuseStrategy strategy) {
        auto [dc, e] = conn(ctx, strategy);
        if (e) return e;
        auto [r, xerr] = execDc(ctx, dc, [dc](error e) { dc->releaseConn(std::move(e)); }, query, args);
        res = r;
        return xerr;
    });
    return {res, err};
}

std::pair<ptr<Result>, error> DB::execDc(ptr<context::Context> ctx, const ptr<driverConn>& dc, releaseFn release,
                                          std::string_view query, std::span<const detail::arg> args) {
    ptr<Result> res;
    error err;
    struct releaser {
        releaseFn& release;
        error& err;
        ~releaser() { release(err); }
    } r{release, err};

    auto* execerCtx = dynamic_cast<driver::ExecerContext*>(dc->ci.get());
    driver::Execer* execer = execerCtx ? nullptr : dynamic_cast<driver::Execer*>(dc->ci.get());
    if (execerCtx || execer) {
        ptr<driver::Result> resi;
        withLock(*dc, [&] {
            auto [nvdargs, aerr] = detail::driverArgsConnLocked(dc->ci.get(), nullptr, args);
            err = aerr;
            if (err) return;
            std::tie(resi, err) = ctxDriverExec(ctx, execerCtx, execer, query, nvdargs);
        });
        if (err != driver::ErrSkip) {
            if (err) return {nullptr, err};
            return {make_ptr<driverResult>(dc, resi), nullptr};
        }
    }
    // No fast path: a prepared statement.
    ptr<driver::Stmt> si;
    withLock(*dc, [&] { std::tie(si, err) = ctxDriverPrepare(ctx, *dc->ci, query); });
    if (err) return {nullptr, err};
    auto ds = make_ptr<driverStmt>();
    ds->dc = dc.get();
    ds->si = si;
    std::tie(res, err) = resultFromStatement(ctx, *dc->ci, ds, args);
    ds->Close();
    if (err) return {nullptr, err};
    return {res, nullptr};
}

std::pair<ptr<Rows>, error> DB::queryArgs(ptr<context::Context> ctx, std::string_view query, std::vector<detail::arg> args) {
    ptr<Rows> rows;
    error err = retry([&](connReuseStrategy strategy) {
        auto [dc, e] = conn(ctx, strategy);
        if (e) return e;
        auto [r, qerr] = queryDc(ctx, nullptr, dc, [dc](error e) { dc->releaseConn(std::move(e)); }, query, args);
        rows = r;
        return qerr;
    });
    return {rows, err};
}

// Runs a query on dc; release is called when the Rows close, or now on an
// error.
std::pair<ptr<Rows>, error> DB::queryDc(ptr<context::Context> ctx, ptr<context::Context> txctx, const ptr<driverConn>& dc,
                                         releaseFn release, std::string_view query, std::span<const detail::arg> args) {
    auto* queryerCtx = dynamic_cast<driver::QueryerContext*>(dc->ci.get());
    driver::Queryer* queryer = queryerCtx ? nullptr : dynamic_cast<driver::Queryer*>(dc->ci.get());
    if (queryerCtx || queryer) {
        ptr<driver::Rows> rowsi;
        error err;
        withLock(*dc, [&] {
            auto [nvdargs, aerr] = detail::driverArgsConnLocked(dc->ci.get(), nullptr, args);
            err = aerr;
            if (err) return;
            std::tie(rowsi, err) = ctxDriverQuery(ctx, queryerCtx, queryer, query, nvdargs);
        });
        if (err != driver::ErrSkip) {
            if (err) {
                release(err);
                return {nullptr, err};
            }
            ptr<Rows> rows(new Rows());
            rows->dc_ = dc;
            rows->releaseConn_ = std::move(release);
            rows->rowsi_ = rowsi;
            rows->initContextClose(ctx, txctx);
            return {rows, nullptr};
        }
    }
    ptr<driver::Stmt> si;
    error err;
    withLock(*dc, [&] { std::tie(si, err) = ctxDriverPrepare(ctx, *dc->ci, query); });
    if (err) {
        release(err);
        return {nullptr, err};
    }
    auto ds = make_ptr<driverStmt>();
    ds->dc = dc.get();
    ds->si = si;
    auto [rowsi, rerr] = rowsiFromStatement(ctx, *dc->ci, ds, args);
    if (rerr) {
        ds->Close();
        release(rerr);
        return {nullptr, rerr};
    }
    // The statement closes with the rows.
    ptr<Rows> rows(new Rows());
    rows->dc_ = dc;
    rows->releaseConn_ = std::move(release);
    rows->rowsi_ = rowsi;
    rows->closeStmt_ = ds;
    rows->initContextClose(ctx, txctx);
    return {rows, nullptr};
}

ptr<Row> DB::queryRowArgs(ptr<context::Context> ctx, std::string_view query, std::vector<detail::arg> args) {
    auto [rows, err] = queryArgs(std::move(ctx), query, std::move(args));
    return make_ptr<Row>(rows, err);
}

std::pair<ptr<sql::Tx>, error> DB::BeginTx(ptr<context::Context> ctx, const TxOptions* opts) {
    ptr<sql::Tx> tx;
    error err = retry([&](connReuseStrategy strategy) {
        auto [dc, e] = conn(ctx, strategy);
        if (e) return e;
        auto [t, berr] = beginDc(ctx, dc, [dc](error e) { dc->releaseConn(std::move(e)); }, opts);
        tx = t;
        return berr;
    });
    return {tx, err};
}

std::pair<ptr<sql::Tx>, error> DB::Begin() { return BeginTx(context::Background(), nullptr); }

std::pair<ptr<sql::Tx>, error> DB::beginDc(ptr<context::Context> ctx, const ptr<driverConn>& dc, releaseFn release,
                                            const TxOptions* opts) {
    ptr<driver::Tx> txi;
    bool keepConnOnRollback = false;
    error err;
    withLock(*dc, [&] {
        bool hasSessionResetter = dynamic_cast<driver::SessionResetter*>(dc->ci.get()) != nullptr;
        bool hasValidator = dynamic_cast<driver::Validator*>(dc->ci.get()) != nullptr;
        keepConnOnRollback = hasSessionResetter && hasValidator;
        std::tie(txi, err) = ctxDriverBegin(ctx, opts, *dc->ci);
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
    tx->releaseConn_ = std::move(release);
    tx->txi_ = txi;
    tx->cancel_ = cancel;
    tx->keepConnOnRollback_ = keepConnOnRollback;
    tx->ctx_ = tctx;
    go([tx] { tx->awaitDone(); });
    return {tx, nullptr};
}

ptr<driver::Driver> DB::Driver() { return connector_->Driver(); }

std::pair<ptr<sql::Conn>, error> DB::Conn(ptr<context::Context> ctx) {
    ptr<driverConn> dc;
    error err = retry([&](connReuseStrategy strategy) {
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

std::tuple<ptr<driverConn>, releaseFn, error> Conn::grabConn(ptr<context::Context>) {
    if (done_.load()) return {nullptr, nullptr, ErrConnDone};
    closemu_.RLock();
    ptr<sql::Conn> self(this);
    return {dc_, [self](error err) { self->closemuRunlockCondReleaseConn(std::move(err)); }, nullptr};
}

error Conn::PingContext(ptr<context::Context> ctx) {
    auto [dc, release, err] = grabConn(ctx);
    if (err) return err;
    return db_->pingDc(ctx, dc, release);
}

std::pair<ptr<Result>, error> Conn::execArgs(ptr<context::Context> ctx, std::string_view query, std::vector<detail::arg> args) {
    auto [dc, release, err] = grabConn(ctx);
    if (err) return {nullptr, err};
    return db_->execDc(ctx, dc, release, query, args);
}

std::pair<ptr<Rows>, error> Conn::queryArgs(ptr<context::Context> ctx, std::string_view query, std::vector<detail::arg> args) {
    auto [dc, release, err] = grabConn(ctx);
    if (err) return {nullptr, err};
    return db_->queryDc(ctx, nullptr, dc, release, query, args);
}

ptr<Row> Conn::queryRowArgs(ptr<context::Context> ctx, std::string_view query, std::vector<detail::arg> args) {
    auto [rows, err] = queryArgs(std::move(ctx), query, std::move(args));
    return make_ptr<Row>(rows, err);
}

std::pair<ptr<sql::Stmt>, error> Conn::PrepareContext(ptr<context::Context> ctx, std::string_view query) {
    auto [dc, release, err] = grabConn(ctx);
    if (err) return {nullptr, err};
    return db_->prepareDc(ctx, dc, release, this, ptr<object>(static_cast<object*>(this)), query);
}

error Conn::Raw(const std::function<error(driver::Conn&)>& f) {
    auto [dc, release, err] = grabConn(nullptr);
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
    auto [dc, release, err] = grabConn(ctx);
    if (err) return {nullptr, err};
    return db_->beginDc(ctx, dc, release, opts);
}

void Conn::closemuRunlockCondReleaseConn(error err) {
    closemu_.RUnlock();
    if (errors::Is(err, driver::ErrBadConn)) close(err);
}

error Conn::close(error err) {
    bool expected = false;
    if (!done_.compare_exchange_strong(expected, true)) return ErrConnDone;
    // Lock around releasing the driver connection to ensure all queries
    // have been stopped before doing so.
    std::lock_guard l(closemu_);
    dc_->releaseConn(err);
    dc_ = nullptr;
    return err;
}

error Conn::Close() { return close(nullptr); }

// ---------------------------------------------------------------------------
// Tx

Tx::~Tx() = default;

void Tx::awaitDone() {
    // Wait for either the transaction to be committed or rolled back, or
    // for the associated context to be closed.
    ctx_->Done().recv();
    // Discard and close the connection used to ensure the transaction is
    // closed and the resources are released, unless the driver can reset
    // the session.
    rollback(!keepConnOnRollback_);
}

void Tx::close(error err) {
    releaseConn_(std::move(err));
    dc_ = nullptr;
    txi_ = nullptr;
    // The Tx's statements refer to it (to grab its connection); drop them,
    // closed or not, so that the two don't keep each other alive. Go's GC
    // frees the pair; statements of a done Tx are unusable anyway.
    std::vector<ptr<sql::Stmt>> stmts;
    {
        std::lock_guard l(stmtsMu_);
        stmts.swap(stmts_);
    }
}

std::tuple<ptr<driverConn>, releaseFn, error> Tx::grabConn(ptr<context::Context> ctx) {
    if (error err = ctxDone(ctx)) return {nullptr, nullptr, err};
    // closemu.RLock must come before the check for isDone to prevent the
    // Tx from closing while a query is executing.
    closemu_.RLock();
    if (isDone()) {
        closemu_.RUnlock();
        return {nullptr, nullptr, ErrTxDone};
    }
    ptr<sql::Tx> self(this);
    return {dc_, [self](error) { self->closemu_.RUnlock(); }, nullptr};
}

void Tx::closePrepared() {
    std::lock_guard l(stmtsMu_);
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
    withLock(*dc_, [&] { err = txi_->Commit(); });
    if (!errors::Is(err, driver::ErrBadConn)) closePrepared();
    close(err);
    return err;
}

error Tx::rollback(bool discardConn) {
    bool expected = false;
    if (!done_.compare_exchange_strong(expected, true)) return ErrTxDone;
    cancel_();
    closemu_.Lock();
    closemu_.Unlock();
    error err;
    withLock(*dc_, [&] { err = txi_->Rollback(); });
    if (!errors::Is(err, driver::ErrBadConn)) closePrepared();
    if (discardConn) err = driver::ErrBadConn;
    close(err);
    return err;
}

error Tx::Rollback() { return rollback(false); }

std::pair<ptr<sql::Stmt>, error> Tx::PrepareContext(ptr<context::Context> ctx, std::string_view query) {
    auto [dc, release, err] = grabConn(ctx);
    if (err) return {nullptr, err};
    auto [stmt, perr] = db_->prepareDc(ctx, dc, release, this, ptr<object>(static_cast<object*>(this)), query);
    if (perr) return {nullptr, perr};
    std::lock_guard l(stmtsMu_);
    stmts_.push_back(stmt);
    return {stmt, nullptr};
}

std::pair<ptr<sql::Stmt>, error> Tx::Prepare(std::string_view query) { return PrepareContext(context::Background(), query); }

ptr<sql::Stmt> Tx::StmtContext(ptr<context::Context> ctx, const ptr<sql::Stmt>& stmt) {
    auto sticky = [](error err) {
        ptr<sql::Stmt> s(new sql::Stmt());
        s->stickyErr_ = std::move(err);
        return s;
    };
    auto [dc, release, err] = grabConn(ctx);
    if (err) return sticky(err);
    struct releaser {
        releaseFn& r;
        ~releaser() { r(nullptr); }
    } rel{release};
    if (db_ != stmt->db_) return sticky(errors::New("sql: Tx.Stmt: statement from different database used"));
    ptr<driver::Stmt> si;
    ptr<sql::Stmt> parentStmt;
    stmt->mu_.lock();
    if (stmt->closed_ || stmt->cg_) {
        // The statement is closed or belongs to a Tx or Conn: prepare it
        // again on this transaction's connection.
        stmt->mu_.unlock();
        withLock(*dc, [&] { std::tie(si, err) = ctxDriverPrepare(ctx, *dc->ci, stmt->query_); });
        if (err) return sticky(err);
    } else {
        stmt->removeClosedStmtLocked();
        // Reuse a statement prepared on this connection, if any.
        for (auto& v : stmt->css_) {
            if (v.dc == dc) {
                si = v.ds->si;
                break;
            }
        }
        stmt->mu_.unlock();
        if (!si) {
            ptr<driverStmt> ds;
            withLock(*dc, [&] { std::tie(ds, err) = stmt->prepareOnConnLocked(ctx, dc); });
            if (err) return sticky(err);
            si = ds->si;
        }
        parentStmt = stmt;
    }
    ptr<sql::Stmt> txs(new sql::Stmt());
    txs->db_ = db_;
    txs->cg_ = this;
    txs->cgKeep_ = ptr<object>(static_cast<object*>(this));
    txs->cgds_ = make_ptr<driverStmt>();
    txs->cgds_->dc = dc.get();
    txs->cgds_->si = si;
    txs->parentStmt_ = parentStmt;
    txs->query_ = stmt->query_;
    if (parentStmt) db_->addDep(parentStmt.get(), parentStmt, txs.get());
    std::lock_guard l(stmtsMu_);
    stmts_.push_back(txs);
    return txs;
}

ptr<sql::Stmt> Tx::Stmt(const ptr<sql::Stmt>& stmt) { return StmtContext(context::Background(), stmt); }

std::pair<ptr<Result>, error> Tx::execArgs(ptr<context::Context> ctx, std::string_view query, std::vector<detail::arg> args) {
    auto [dc, release, err] = grabConn(ctx);
    if (err) return {nullptr, err};
    return db_->execDc(ctx, dc, release, query, args);
}

std::pair<ptr<Rows>, error> Tx::queryArgs(ptr<context::Context> ctx, std::string_view query, std::vector<detail::arg> args) {
    auto [dc, release, err] = grabConn(ctx);
    if (err) return {nullptr, err};
    return db_->queryDc(ctx, ctx_, dc, release, query, args);
}

ptr<Row> Tx::queryRowArgs(ptr<context::Context> ctx, std::string_view query, std::vector<detail::arg> args) {
    auto [rows, err] = queryArgs(std::move(ctx), query, std::move(args));
    return make_ptr<Row>(rows, err);
}

// ---------------------------------------------------------------------------
// Stmt

Stmt::~Stmt() = default;

std::pair<ptr<Result>, error> Stmt::execArgs(ptr<context::Context> ctx, std::vector<detail::arg> args) {
    closemu_.RLock();
    struct unlocker {
        detail::closingMutex& m;
        ~unlocker() { m.RUnlock(); }
    } u{closemu_};
    ptr<Result> res;
    error err = db_->retry([&](connReuseStrategy strategy) {
        auto [dc, release, ds, e] = connStmt(ctx, strategy);
        if (e) return e;
        auto [r, xerr] = resultFromStatement(ctx, *dc->ci, ds, args);
        res = r;
        release(xerr);
        return xerr;
    });
    return {res, err};
}

void Stmt::removeClosedStmtLocked() {
    // Clean the connections that closed since the last time, when enough
    // did.
    std::size_t t = std::min<std::size_t>(css_.size() / 2 + 1, 10);
    uint64 dbClosed = db_->numClosed_.load();
    if (dbClosed - lastNumClosed_ < t) return;
    {
        std::lock_guard l(db_->mu_);
        std::erase_if(css_, [](const detail::connStmt& v) { return v.dc->dbmuClosed; });
    }
    lastNumClosed_ = dbClosed;
}

std::tuple<ptr<driverConn>, releaseFn, ptr<driverStmt>, error> Stmt::connStmt(ptr<context::Context> ctx,
                                                                                   connReuseStrategy strategy) {
    if (stickyErr_) return {nullptr, nullptr, nullptr, stickyErr_};
    mu_.lock();
    if (closed_) {
        mu_.unlock();
        return {nullptr, nullptr, nullptr, errors::New("sql: statement is closed")};
    }
    // In a transaction or connection, there is one connection to use.
    if (cg_) {
        mu_.unlock();
        auto [dc, release, err] = cg_->grabConn(ctx);  // blocks, waiting for the connection
        if (err) return {nullptr, nullptr, nullptr, err};
        return {dc, release, cgds_, nullptr};
    }
    removeClosedStmtLocked();
    mu_.unlock();

    auto [dc, err] = db_->conn(ctx, strategy);
    if (err) return {nullptr, nullptr, nullptr, err};
    releaseFn release = [dc](error e) { dc->releaseConn(std::move(e)); };
    mu_.lock();
    for (auto& v : css_) {
        if (v.dc == dc) {
            mu_.unlock();
            return {dc, release, v.ds, nullptr};
        }
    }
    mu_.unlock();
    // No statement on this connection yet: prepare one.
    ptr<driverStmt> ds;
    withLock(*dc, [&] { std::tie(ds, err) = prepareOnConnLocked(ctx, dc); });
    if (err) {
        dc->releaseConn(err);
        return {nullptr, nullptr, nullptr, err};
    }
    return {dc, release, ds, nullptr};
}

std::pair<ptr<driverStmt>, error> Stmt::prepareOnConnLocked(ptr<context::Context> ctx, const ptr<driverConn>& dc) {
    auto [si, err] = dc->prepareLocked(ctx, cg_ != nullptr, query_);
    if (err) return {nullptr, err};
    std::lock_guard l(mu_);
    css_.push_back({dc, si});
    return {si, nullptr};
}

std::pair<ptr<Rows>, error> Stmt::queryArgs(ptr<context::Context> ctx, std::vector<detail::arg> args) {
    closemu_.RLock();
    struct unlocker {
        detail::closingMutex& m;
        ~unlocker() { m.RUnlock(); }
    } u{closemu_};
    ptr<Rows> rows;
    error err = db_->retry([&](connReuseStrategy strategy) {
        auto [dc, release, ds, e] = connStmt(ctx, strategy);
        if (e) return e;
        auto [rowsi, qerr] = rowsiFromStatement(ctx, *dc->ci, ds, args);
        if (!qerr) {
            // The rows depend on the statement, which can't close until
            // they do.
            ptr<Rows> r(new Rows());
            r->dc_ = dc;
            r->rowsi_ = rowsi;
            ptr<sql::Stmt> self(this);
            db_->addDep(this, self, r.get());
            const void* rp = r.get();
            r->releaseConn_ = [release, self, rp](error e) {
                release(std::move(e));
                self->db_->removeDep(self.get(), rp);
            };
            ptr<context::Context> txctx;
            if (cg_) txctx = cg_->txCtx();
            r->initContextClose(ctx, txctx);
            rows = r;
            return error();
        }
        release(qerr);
        return qerr;
    });
    return {rows, err};
}

ptr<Row> Stmt::queryRowArgs(ptr<context::Context> ctx, std::vector<detail::arg> args) {
    auto [rows, err] = queryArgs(std::move(ctx), std::move(args));
    if (err) return make_ptr<Row>(nullptr, err);
    return make_ptr<Row>(rows, nullptr);
}

error Stmt::Close() {
    std::lock_guard l(closemu_);
    if (stickyErr_) return stickyErr_;
    ptr<driverStmt> txds;
    {
        std::lock_guard ml(mu_);
        if (closed_) return nullptr;
        closed_ = true;
        txds = std::move(cgds_);
    }
    if (!cg_) return db_->removeDep(this, this);
    if (parentStmt_) {
        // A Tx statement of a DB statement: the driver statement belongs
        // to the parent.
        return db_->removeDep(parentStmt_.get(), this);
    }
    return txds->Close();
}

error Stmt::finalClose() {
    std::lock_guard l(mu_);
    for (auto& v : css_) {
        db_->noteUnusedDriverStatement(v.dc, v.ds);
        v.dc->removeOpenStmt(v.ds.get());
    }
    css_.clear();
    return nullptr;
}

// ---------------------------------------------------------------------------
// Rows

Rows::~Rows() = default;

error Rows::lasterrOrErrLocked(error err) {
    if (lasterr_ && lasterr_ != io::EOF_) return lasterr_;
    return err;
}

void Rows::initContextClose(ptr<context::Context> ctx, ptr<context::Context> txctx) {
    if (!ctx->Done() && (!txctx || !txctx->Done())) return;
    auto [closectx, cancel] = context::WithCancel(ctx);
    cancel_ = cancel;
    ptr<Rows> self(this);
    go([self, ctx, txctx, closectx] { self->awaitDone(ctx, txctx, closectx); });
}

// Closes the rows when a context is done, unless they close first
// (closectx).
void Rows::awaitDone(ptr<context::Context> ctx, ptr<context::Context> txctx, ptr<context::Context> closectx) {
    chan<empty> txctxDone = txctx ? txctx->Done() : chan<empty>();
    auto setDone = [&](error err) {
        std::lock_guard l(contextDoneMu_);
        contextDone_ = std::move(err);
        hasContextDone_.store(true);
    };
    select(recv(ctx->Done(), [&] { setDone(ctx->Err()); }), recv(txctxDone, [&] { setDone(txctx->Err()); }),
           recv(closectx->Done(), [] {}));
    close(ctx->Err());
}

bool Rows::Next() {
    // If the user's calling Next, they're done with their previous row's
    // Scan results (any RawBytes memory), so we can release the read lock.
    closemuRunlockIfHeldByScan();
    if (hasContextDone_.load()) return false;
    bool doClose, ok;
    {
        closemu_.RLock();
        struct unlocker {
            detail::closingMutex& m;
            ~unlocker() { m.RUnlock(); }
        } u{closemu_};
        std::tie(doClose, ok) = nextLocked();
    }
    if (doClose) Close();
    if (doClose && !ok) hitEof_ = true;
    return ok;
}

std::pair<bool, bool> Rows::nextLocked() {
    if (closed_) return {false, false};
    // Lock the driver connection before calling the driver interface
    // rowsi to prevent a Tx from rolling back the connection at the same
    // time.
    std::lock_guard l(*dc_);
    if (!nextCalled_) {
        numCols_ = rowsi_->Columns().size();
        nextCalled_ = true;
    }
    if (lastcols_.size() != numCols_) lastcols_.assign(numCols_, nullptr);
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
    closemuRunlockIfHeldByScan();
    bool doClose = false;
    bool result;
    {
        closemu_.RLock();
        struct unlocker {
            detail::closingMutex& m;
            ~unlocker() { m.RUnlock(); }
        } u{closemu_};
        if (closed_) return false;
        lastcols_.clear();
        nextCalled_ = false;
        auto* nrs = dynamic_cast<driver::RowsNextResultSet*>(rowsi_.get());
        if (!nrs) {
            doClose = true;
            result = false;
        } else {
            std::lock_guard l(*dc_);
            lasterr_ = nrs->NextResultSet();
            if (lasterr_) {
                doClose = true;
                result = false;
            } else {
                result = true;
            }
        }
    }
    if (doClose) Close();
    return result;
}

error Rows::Err() {
    // Return any context error that might have occurred while iterating
    // over the rows; it takes precedence unless the end was reached.
    if (!hitEof_ && hasContextDone_.load()) {
        std::lock_guard l(contextDoneMu_);
        return contextDone_;
    }
    closemu_.RLock();
    error err = lasterrOrErrLocked(nullptr);
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
        detail::closingMutex& m;
        ~unlocker() { m.RUnlock(); }
    } u{closemu_};
    if (closed_) return {{}, lasterrOrErrLocked(errRowsClosed)};
    if (!rowsi_) return {{}, lasterrOrErrLocked(errNoRowsAvailable)};
    std::lock_guard l(*dc_);
    return {rowsi_->Columns(), nullptr};
}

std::pair<std::vector<ptr<ColumnType>>, error> Rows::ColumnTypes() {
    closemu_.RLock();
    struct unlocker {
        detail::closingMutex& m;
        ~unlocker() { m.RUnlock(); }
    } u{closemu_};
    if (closed_) return {{}, lasterrOrErrLocked(errRowsClosed)};
    if (!rowsi_) return {{}, lasterrOrErrLocked(errNoRowsAvailable)};
    std::lock_guard l(*dc_);
    auto names = rowsi_->Columns();
    std::vector<ptr<ColumnType>> list;
    for (std::size_t i = 0; i < names.size(); i++) {
        int idx = static_cast<int>(i);
        auto ci = make_ptr<ColumnType>();
        ci->name_ = names[i];
        if (auto* p = dynamic_cast<driver::RowsColumnTypeScanType*>(rowsi_.get())) ci->scanType_ = p->ColumnTypeScanType(idx);
        else ci->scanType_ = string::fromStatic("interface {}");
        if (auto* p = dynamic_cast<driver::RowsColumnTypeDatabaseTypeName*>(rowsi_.get())) {
            ci->databaseType_ = p->ColumnTypeDatabaseTypeName(idx);
        }
        if (auto* p = dynamic_cast<driver::RowsColumnTypeLength*>(rowsi_.get())) {
            std::tie(ci->length_, ci->hasLength_) = p->ColumnTypeLength(idx);
        }
        if (auto* p = dynamic_cast<driver::RowsColumnTypeNullable*>(rowsi_.get())) {
            std::tie(ci->nullable_, ci->hasNullable_) = p->ColumnTypeNullable(idx);
        }
        if (auto* p = dynamic_cast<driver::RowsColumnTypePrecisionScale*>(rowsi_.get())) {
            std::tie(ci->precision_, ci->scale_, ci->hasPrecisionScale_) = p->ColumnTypePrecisionScale(idx);
        }
        list.push_back(ci);
    }
    return {std::move(list), nullptr};
}

error Rows::scan(std::span<detail::dest> dest) {
    if (closemuScanHold_) {
        // This should only be possible if the user calls Scan twice in a
        // row without calling Next.
        return errors::New("sql: Scan called without calling Next (closemuScanHold)");
    }
    closemu_.RLock();
    raw_ = raw_.Sub(0, 0);
    error err = scanLocked(dest);
    bool hasRaw = std::any_of(dest.begin(), dest.end(), [](const detail::dest& d) { return d.rawBytes; });
    if (!err && hasRaw) {
        // RawBytes point into memory the driver owns until the next call;
        // keep the rows from closing until then.
        closemuScanHold_ = true;
    } else {
        closemu_.RUnlock();
    }
    return err;
}

error Rows::scanLocked(std::span<detail::dest> dest) {
    if (lasterr_ && lasterr_ != io::EOF_) return lasterr_;
    if (closed_) return lasterrOrErrLocked(errRowsClosed);
    if (!nextCalled_) return errors::New("sql: Scan called without calling Next");
    if (dest.size() != numCols_) {
        return errors::New("sql: expected " + strconv::Itoa(static_cast<int64>(numCols_)) + " destination arguments in Scan, not " +
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

void Rows::closemuRunlockIfHeldByScan() {
    if (closemuScanHold_) {
        closemuScanHold_ = false;
        closemu_.RUnlock();
    }
}

error Rows::Close() {
    // If the user's calling Close, they're done with their previous row's
    // Scan results (any RawBytes memory), so we can release the read lock
    // that would be preventing that close.
    closemuRunlockIfHeldByScan();
    return close(nullptr);
}

error Rows::close(error err) {
    std::lock_guard l(closemu_);
    if (closed_) return nullptr;
    closed_ = true;
    if (!lasterr_) lasterr_ = err;
    withLock(*dc_, [&] { err = rowsi_->Close(); });
    if (detail::rowsCloseHook) detail::rowsCloseHook(*this, err);
    if (cancel_) cancel_();
    if (closeStmt_) closeStmt_->Close();
    releaseConn_(err);
    lasterr_ = lasterrOrErrLocked(err);
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
        if (d.rawBytes) return errors::New("sql: RawBytes isn't allowed on Row.Scan");
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
