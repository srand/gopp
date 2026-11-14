// After Go's context/context.Go.
#include <gopp/context.hpp>
#include <gopp/runtime.hpp>
#include <gopp/sync.hpp>

#include <mutex>
#include <unordered_set>

namespace gopp::context {

namespace {

struct deadline_exceeded_error final : errorInterface {
    string Error() const override { return string::fromStatic("context deadline exceeded"); }
    bool Timeout() const noexcept override { return true; }
};

}  // namespace

const error Canceled = errors::New("context canceled");
const error DeadlineExceeded = error(make_ptr<deadline_exceeded_error>());

namespace {

struct empty_ctx final : Context {
    chan<empty> Done() const override { return nullptr; }
    error Err() const override { return nullptr; }
};

class cancel_ctx;

// Finds the nearest cancel_ctx among parent and its ancestors.
cancel_ctx* parentCancelCtx(const ptr<Context>& parent);

class cancel_ctx : public Context {
public:
    explicit cancel_ctx(ptr<Context> parent) : parent_(std::move(parent)) {}

    std::optional<time::Time> Deadline() const override { return parent_->Deadline(); }
    ptr<object> Value(const void* key) const override { return parent_->Value(key); }

    chan<empty> Done() const override {
        std::lock_guard l(mu_);
        if (!done_) done_ = makeChan<empty>();
        return done_;
    }

    error Err() const override {
        std::lock_guard l(mu_);
        return err_;
    }

    error causeErr() const {
        std::lock_guard l(mu_);
        return cause_;
    }

    // Closes done, cancels the children and, with remove, detaches from the
    // parent.
    void cancel(bool remove_from_parent, error err, error cause) {
        if (!cause) cause = err;
        std::unordered_set<cancel_ctx*> children;
        {
            std::lock_guard l(mu_);
            if (err_) return;  // already canceled
            err_ = err;
            cause_ = cause;
            if (!done_) done_ = makeChan<empty>();
            done_.close();
            children.swap(children_);
        }
        for (cancel_ctx* c : children) {
            c->cancel(false, err, cause);
            c->releaseRef();
        }
        if (remove_from_parent) detach();
        onCancel();
    }

    // Arranges for this context to be canceled when parent is.
    void propagateCancel() {
        chan<empty> pd = parent_->Done();
        if (!pd) return;  // the parent is never canceled

        bool closed = false;
        empty e;
        if (pd.tryRecv(e, &closed) || closed) {
            cancel(false, parent_->Err(), Cause(parent_));
            return;
        }

        if (cancel_ctx* p = parentCancelCtx(parent_)) {
            std::unique_lock l(p->mu_);
            if (p->err_) {
                error perr = p->err_, pcause = p->cause_;
                l.unlock();
                cancel(false, perr, pcause);
            } else {
                addRef();  // the parent holds its children
                p->children_.insert(this);
                parent_cc_ = p;
            }
            return;
        }

        // A parent of another kind: watch it from a fiber.
        ptr<cancel_ctx> self(this);
        chan<empty> mine = Done();
        go([self, pd, mine] {
            select(recv(pd, [&] { self->cancel(false, self->parent_->Err(), Cause(self->parent_)); }),
                   recv(mine, [] {}));
        });
    }

protected:
    virtual void onCancel() {}

private:
    void detach() {
        cancel_ctx* p = parent_cc_;
        if (!p) return;
        bool removed = false;
        {
            std::lock_guard l(p->mu_);
            removed = p->children_.erase(this) > 0;
        }
        if (removed) releaseRef();
    }

    friend cancel_ctx* parentCancelCtx(const ptr<Context>& parent);

    ptr<Context> parent_;
    cancel_ctx* parent_cc_ = nullptr;
    mutable sync::Mutex mu_;
    mutable chan<empty> done_;
    std::unordered_set<cancel_ctx*> children_;
    error err_;
    error cause_;
};

cancel_ctx* parentCancelCtx(const ptr<Context>& parent) {
    // Go compares the done channels to tell whether the parent's done is
    // that of its cancel_ctx; a dynamic_cast serves the same purpose for
    // the context types defined here.
    return dynamic_cast<cancel_ctx*>(parent.get());
}

class timer_ctx final : public cancel_ctx {
public:
    timer_ctx(ptr<Context> parent, time::Time d) : cancel_ctx(std::move(parent)), deadline_(d) {}

    std::optional<time::Time> Deadline() const override { return deadline_; }

    void start(time::Duration dur) {
        ptr<timer_ctx> self(this);
        std::lock_guard l(tmu_);
        timer_ = time::AfterFunc(dur, [self] { self->cancel(true, DeadlineExceeded, nullptr); });
    }

protected:
    void onCancel() override {
        ptr<time::Timer> t;
        {
            std::lock_guard l(tmu_);
            t = std::move(timer_);
        }
        if (t) t->Stop();
    }

private:
    time::Time deadline_;
    sync::Mutex tmu_;
    ptr<time::Timer> timer_;
};

struct value_ctx final : Context {
    value_ctx(ptr<Context> p, const void* k, ptr<object> v) : parent(std::move(p)), key(k), val(std::move(v)) {}
    std::optional<time::Time> Deadline() const override { return parent->Deadline(); }
    chan<empty> Done() const override { return parent->Done(); }
    error Err() const override { return parent->Err(); }
    ptr<object> Value(const void* k) const override { return k == key ? val : parent->Value(k); }

    ptr<Context> parent;
    const void* key;
    ptr<object> val;
};

}  // namespace

ptr<Context> Background() {
    static Context* bg = [] {
        auto* c = new empty_ctx();
        c->addRef();  // never freed
        return c;
    }();
    return ptr<Context>(bg);
}

ptr<Context> TODO() { return Background(); }

std::pair<ptr<Context>, CancelFunc> WithCancel(ptr<Context> parent) {
    if (!parent) panic("cannot create context from nil parent");
    ptr<cancel_ctx> c = make_ptr<cancel_ctx>(std::move(parent));
    c->propagateCancel();
    return {c, [c] { c->cancel(true, Canceled, nullptr); }};
}

std::pair<ptr<Context>, CancelCauseFunc> WithCancelCause(ptr<Context> parent) {
    if (!parent) panic("cannot create context from nil parent");
    ptr<cancel_ctx> c = make_ptr<cancel_ctx>(std::move(parent));
    c->propagateCancel();
    return {c, [c](error cause) { c->cancel(true, Canceled, std::move(cause)); }};
}

std::pair<ptr<Context>, CancelFunc> WithDeadline(ptr<Context> parent, time::Time d) {
    if (!parent) panic("cannot create context from nil parent");
    if (auto cur = parent->Deadline(); cur && cur->Before(d)) return WithCancel(std::move(parent));
    ptr<timer_ctx> c = make_ptr<timer_ctx>(std::move(parent), d);
    c->propagateCancel();
    time::Duration dur = time::Until(d);
    if (dur.ns <= 0) {
        c->cancel(true, DeadlineExceeded, nullptr);
        return {c, [c] { c->cancel(false, Canceled, nullptr); }};
    }
    if (!c->Err()) c->start(dur);
    return {c, [c] { c->cancel(true, Canceled, nullptr); }};
}

std::pair<ptr<Context>, CancelFunc> WithTimeout(ptr<Context> parent, time::Duration timeout) {
    return WithDeadline(std::move(parent), time::Now().Add(timeout));
}

ptr<Context> WithValue(ptr<Context> parent, const void* key, ptr<object> val) {
    if (!parent) panic("cannot create context from nil parent");
    return make_ptr<value_ctx>(std::move(parent), key, std::move(val));
}

error Cause(const ptr<Context>& ctx) {
    if (auto* c = dynamic_cast<cancel_ctx*>(ctx.get())) return c->causeErr();
    return ctx->Err();
}

}  // namespace gopp::context
