// After Go's context/context.Go.
#include <gopp/context.hpp>
#include <gopp/runtime.hpp>
#include <gopp/sync.hpp>

#include <mutex>
#include <unordered_set>

namespace gopp::context {

namespace {

struct deadlineExceededError final : errorInterface {
    string Error() const override { return string::fromStatic("context deadline exceeded"); }
    bool Timeout() const noexcept override { return true; }
};

}  // namespace

const error Canceled = errors::New("context canceled");
const error DeadlineExceeded = error(make_ptr<deadlineExceededError>());

namespace {

struct emptyCtx final : Context {
    chan<empty> Done() const override { return nullptr; }
    error Err() const override { return nullptr; }
};

class cancelCtx;

// Finds the nearest cancelCtx among parent and its ancestors.
cancelCtx* parentCancelCtx(const ptr<Context>& parent);

class cancelCtx : public Context {
public:
    explicit cancelCtx(ptr<Context> parent) : parent_(std::move(parent)) {}

    std::optional<time::Time> Deadline() const override { return parent_->Deadline(); }
    ptr<object> Value(const void* key) const override { return parent_->Value(key); }

    chan<empty> Done() const override {
        std::lock_guard l(mu_);
        if (!done_) done_ = make_chan<empty>();
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
    void cancel(bool removeFromParent, error err, error cause) {
        if (!cause) cause = err;
        std::unordered_set<cancelCtx*> children;
        {
            std::lock_guard l(mu_);
            if (err_) return;  // already canceled
            err_ = err;
            cause_ = cause;
            if (!done_) done_ = make_chan<empty>();
            done_.close();
            children.swap(children_);
        }
        for (cancelCtx* c : children) {
            c->cancel(false, err, cause);
            c->releaseRef();
        }
        if (removeFromParent) detach();
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

        if (cancelCtx* p = parentCancelCtx(parent_)) {
            std::unique_lock l(p->mu_);
            if (p->err_) {
                error perr = p->err_, pcause = p->cause_;
                l.unlock();
                cancel(false, perr, pcause);
            } else {
                addRef();  // the parent holds its children
                p->children_.insert(this);
                parentCc_ = p;
            }
            return;
        }

        // A parent of another kind: watch it from a fiber.
        ptr<cancelCtx> self(this);
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
        cancelCtx* p = parentCc_;
        if (!p) return;
        bool removed = false;
        {
            std::lock_guard l(p->mu_);
            removed = p->children_.erase(this) > 0;
        }
        if (removed) releaseRef();
    }

    friend cancelCtx* parentCancelCtx(const ptr<Context>& parent);

    ptr<Context> parent_;
    cancelCtx* parentCc_ = nullptr;
    mutable sync::Mutex mu_;
    mutable chan<empty> done_;
    std::unordered_set<cancelCtx*> children_;
    error err_;
    error cause_;
};

cancelCtx* parentCancelCtx(const ptr<Context>& parent) {
    // Go compares the done channels to tell whether the parent's done is
    // that of its cancelCtx; a dynamic_cast serves the same purpose for
    // the context types defined here.
    return dynamic_cast<cancelCtx*>(parent.get());
}

class timerCtx final : public cancelCtx {
public:
    timerCtx(ptr<Context> parent, time::Time d) : cancelCtx(std::move(parent)), deadline_(d) {}

    std::optional<time::Time> Deadline() const override { return deadline_; }

    void start(time::Duration dur) {
        ptr<timerCtx> self(this);
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

struct valueCtx final : Context {
    valueCtx(ptr<Context> p, const void* k, ptr<object> v) : parent(std::move(p)), key(k), val(std::move(v)) {}
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
        auto* c = new emptyCtx();
        c->addRef();  // never freed
        return c;
    }();
    return ptr<Context>(bg);
}

ptr<Context> TODO() { return Background(); }

std::pair<ptr<Context>, CancelFunc> WithCancel(ptr<Context> parent) {
    if (!parent) panic("cannot create context from nil parent");
    ptr<cancelCtx> c = make_ptr<cancelCtx>(std::move(parent));
    c->propagateCancel();
    return {c, [c] { c->cancel(true, Canceled, nullptr); }};
}

std::pair<ptr<Context>, CancelCauseFunc> WithCancelCause(ptr<Context> parent) {
    if (!parent) panic("cannot create context from nil parent");
    ptr<cancelCtx> c = make_ptr<cancelCtx>(std::move(parent));
    c->propagateCancel();
    return {c, [c](error cause) { c->cancel(true, Canceled, std::move(cause)); }};
}

std::pair<ptr<Context>, CancelFunc> WithDeadline(ptr<Context> parent, time::Time d) {
    if (!parent) panic("cannot create context from nil parent");
    if (auto cur = parent->Deadline(); cur && cur->Before(d)) return WithCancel(std::move(parent));
    ptr<timerCtx> c = make_ptr<timerCtx>(std::move(parent), d);
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
    return make_ptr<valueCtx>(std::move(parent), key, std::move(val));
}

error Cause(const ptr<Context>& ctx) {
    if (auto* c = dynamic_cast<cancelCtx*>(ctx.get())) return c->causeErr();
    return ctx->Err();
}

}  // namespace gopp::context
