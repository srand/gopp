// Channels, after Go's runtime/chan.Go and runtime/select.Go.
//
// Invariants (from chan.Go): at least one of sendq and recvq is empty,
// except for an unbuffered channel with a single fiber blocked on it for
// both sending and receiving through select. For buffered channels,
// qcount > 0 implies recvq is empty, and qcount < dataqsiz implies sendq is
// empty.
#include "runtime.hpp"

#include <gopp/chan.hpp>

#include <algorithm>
#include <cstring>
#include <new>

namespace gopp::runtime {

struct sudog {
    G* g = nullptr;
    sudog* next = nullptr;
    sudog* prev = nullptr;
    void* elem = nullptr;  // value to send, or storage to receive into
    bool is_select = false;
    bool success = false;  // whether communication happened (false: closed)
    sudog* waitlink = nullptr;  // G's list of sudogs in a select
    gopp::detail::hchan* c = nullptr;
};

}  // namespace gopp::runtime

namespace gopp::detail {

using runtime::G;
using runtime::lock_t;
using runtime::sudog;

namespace {

struct waitq {
    sudog* first = nullptr;
    sudog* last = nullptr;

    void enqueue(sudog* s) noexcept {
        s->next = nullptr;
        s->prev = last;
        if (last) last->next = s;
        else first = s;
        last = s;
    }

    sudog* dequeue() noexcept {
        for (;;) {
            sudog* s = first;
            if (!s) return nullptr;
            first = s->next;
            if (first) first->prev = nullptr;
            else last = nullptr;
            s->next = s->prev = nullptr;
            // A select may have been won by another case already.
            if (s->is_select) {
                uint32_t z = 0;
                if (!s->g->select_done.compare_exchange_strong(z, 1)) continue;
            }
            return s;
        }
    }

    void remove(sudog* s) noexcept {
        sudog* x = s->prev;
        sudog* y = s->next;
        if (x) {
            if (y) {
                x->next = y;
                y->prev = x;
            } else {
                x->next = nullptr;
                last = x;
            }
        } else if (y) {
            y->prev = nullptr;
            first = y;
        } else if (first == s) {
            first = last = nullptr;
        }
        s->next = s->prev = nullptr;
    }
};

}  // namespace

struct hchan {
    std::atomic<unsigned> refs{1};
    const elem_ops* ops;
    std::atomic<std::size_t> qcount{0};
    std::size_t dataqsiz;
    unsigned char* buf = nullptr;
    std::size_t sendx = 0;
    std::size_t recvx = 0;
    waitq recvq;
    waitq sendq;
    std::atomic<uint32_t> closed{0};
    lock_t lock;

    void* slot(std::size_t i) noexcept { return buf + i * stride(); }
    std::size_t stride() const noexcept { return (ops->size + ops->align - 1) / ops->align * ops->align; }

    bool full() const noexcept {
        if (dataqsiz == 0) return recvq.first == nullptr;
        return qcount.load(std::memory_order_relaxed) == dataqsiz;
    }
    bool empty() const noexcept {
        if (dataqsiz == 0) return sendq.first == nullptr;
        return qcount.load(std::memory_order_relaxed) == 0;
    }
};

namespace {

bool chanparkcommit(G*, void* arg) noexcept {
    static_cast<lock_t*>(arg)->unlock();
    return true;
}

[[noreturn]] void block_forever(runtime::waitreason reason) {
    runtime::park(nullptr, nullptr, reason);
    runtime::fatal("unreachable: resumed a fiber blocked forever");
}

// Hands ep to the receiver sg, which is dequeued from c, then unlocks.
template <typename Unlock>
void send_to(hchan* c, sudog* sg, void* ep, Unlock unlock) noexcept {
    if (sg->elem) {
        c->ops->move_construct(sg->elem, ep);
        sg->elem = nullptr;
    }
    G* gp = sg->g;
    unlock();
    gp->param = sg;
    sg->success = true;
    runtime::ready(gp);
}

// Takes a value from the sender sg, which is dequeued from c, then unlocks.
template <typename Unlock>
void recv_from(hchan* c, sudog* sg, void* ep, Unlock unlock) noexcept {
    if (c->dataqsiz == 0) {
        if (ep) c->ops->move_construct(ep, sg->elem);
    } else {
        // The queue is full: take its head and put the sender's value at
        // its tail, which is the same slot.
        void* qp = c->slot(c->recvx);
        if (ep) c->ops->move_construct(ep, qp);
        c->ops->destroy(qp);
        c->ops->move_construct(qp, sg->elem);
        c->recvx++;
        if (c->recvx == c->dataqsiz) c->recvx = 0;
        c->sendx = c->recvx;
    }
    sg->elem = nullptr;
    G* gp = sg->g;
    unlock();
    gp->param = sg;
    sg->success = true;
    runtime::ready(gp);
}

}  // namespace

hchan* chan_make(const elem_ops* ops, std::size_t size) {
    auto* c = new hchan();
    c->ops = ops;
    c->dataqsiz = size;
    if (size > 0) {
        c->buf = static_cast<unsigned char*>(::operator new(size * c->stride(), std::align_val_t(std::max<std::size_t>(ops->align, alignof(std::max_align_t)))));
    }
    return c;
}

void chan_retain(hchan* c) noexcept { c->refs.fetch_add(1, std::memory_order_relaxed); }

void chan_release(hchan* c) noexcept {
    if (c->refs.fetch_sub(1, std::memory_order_acq_rel) != 1) return;
    std::size_t n = c->qcount.load();
    for (std::size_t i = 0, x = c->recvx; i < n; i++) {
        c->ops->destroy(c->slot(x));
        if (++x == c->dataqsiz) x = 0;
    }
    if (c->buf) ::operator delete(c->buf, std::align_val_t(std::max<std::size_t>(c->ops->align, alignof(std::max_align_t))));
    delete c;
}

bool chan_send(hchan* c, void* ep, bool block) {
    if (!c) {
        if (!block) return false;
        block_forever(runtime::waitreason::chan_send_nil);
    }
    // Fast path: a full open channel fails a non-blocking send.
    if (!block && c->closed.load(std::memory_order_acquire) == 0 && c->full()) return false;

    c->lock.lock();
    if (c->closed.load(std::memory_order_relaxed) != 0) {
        c->lock.unlock();
        panic("send on closed channel");
    }
    if (sudog* sg = c->recvq.dequeue()) {
        send_to(c, sg, ep, [c] { c->lock.unlock(); });
        return true;
    }
    if (c->qcount.load(std::memory_order_relaxed) < c->dataqsiz) {
        c->ops->move_construct(c->slot(c->sendx), ep);
        if (++c->sendx == c->dataqsiz) c->sendx = 0;
        c->qcount.fetch_add(1, std::memory_order_relaxed);
        c->lock.unlock();
        return true;
    }
    if (!block) {
        c->lock.unlock();
        return false;
    }

    G* gp = runtime::getg();
    sudog mysg;
    mysg.g = gp;
    mysg.elem = ep;
    mysg.c = c;
    gp->waiting = &mysg;
    gp->param = nullptr;
    c->sendq.enqueue(&mysg);
    runtime::park(chanparkcommit, &c->lock, runtime::waitreason::chan_send);

    gp = runtime::getg();
    gp->waiting = nullptr;
    gp->param = nullptr;
    if (!mysg.success) panic("send on closed channel");
    return true;
}

recv_result chan_recv(hchan* c, void* ep, bool block) {
    if (!c) {
        if (!block) return {false, false};
        block_forever(runtime::waitreason::chan_receive_nil);
    }
    // Fast path: an empty channel fails a non-blocking receive.
    if (!block && c->empty()) {
        if (c->closed.load(std::memory_order_acquire) == 0) return {false, false};
        if (c->empty()) return {true, false};
    }

    c->lock.lock();
    if (c->closed.load(std::memory_order_relaxed) != 0) {
        if (c->qcount.load(std::memory_order_relaxed) == 0) {
            c->lock.unlock();
            return {true, false};
        }
    } else if (sudog* sg = c->sendq.dequeue()) {
        recv_from(c, sg, ep, [c] { c->lock.unlock(); });
        return {true, true};
    }
    if (c->qcount.load(std::memory_order_relaxed) > 0) {
        void* qp = c->slot(c->recvx);
        c->ops->move_construct(ep, qp);
        c->ops->destroy(qp);
        if (++c->recvx == c->dataqsiz) c->recvx = 0;
        c->qcount.fetch_sub(1, std::memory_order_relaxed);
        c->lock.unlock();
        return {true, true};
    }
    if (!block) {
        c->lock.unlock();
        return {false, false};
    }

    G* gp = runtime::getg();
    sudog mysg;
    mysg.g = gp;
    mysg.elem = ep;
    mysg.c = c;
    gp->waiting = &mysg;
    gp->param = nullptr;
    c->recvq.enqueue(&mysg);
    runtime::park(chanparkcommit, &c->lock, runtime::waitreason::chan_receive);

    gp = runtime::getg();
    gp->waiting = nullptr;
    gp->param = nullptr;
    return {true, mysg.success};
}

void chan_close(hchan* c) {
    if (!c) panic("close of nil channel");
    c->lock.lock();
    if (c->closed.load(std::memory_order_relaxed) != 0) {
        c->lock.unlock();
        panic("close of closed channel");
    }
    c->closed.store(1, std::memory_order_release);

    runtime::glist list;
    while (sudog* sg = c->recvq.dequeue()) {
        sg->elem = nullptr;  // left unconstructed: the receiver gets the zero value
        sg->success = false;
        sg->g->param = sg;
        list.push_back(sg->g);
    }
    while (sudog* sg = c->sendq.dequeue()) {
        sg->elem = nullptr;
        sg->success = false;
        sg->g->param = sg;
        list.push_back(sg->g);
    }
    c->lock.unlock();
    while (G* gp = list.pop()) runtime::ready(gp);
}

std::size_t chan_len(hchan* c) noexcept { return c->qcount.load(std::memory_order_relaxed); }
std::size_t chan_cap(hchan* c) noexcept { return c->dataqsiz; }

// ---------------------------------------------------------------------------
// select

namespace {

void sellock(scase* cases, const uint16_t* lockorder, int n) noexcept {
    hchan* last = nullptr;
    for (int i = 0; i < n; i++) {
        hchan* c = cases[lockorder[i]].c;
        if (c != last) {
            c->lock.lock();
            last = c;
        }
    }
}

void selunlock(scase* cases, const uint16_t* lockorder, int n) noexcept {
    // Unlock in reverse order, once per channel.
    for (int i = n - 1; i >= 0; i--) {
        hchan* c = cases[lockorder[i]].c;
        if (i > 0 && c == cases[lockorder[i - 1]].c) continue;
        c->lock.unlock();
    }
}

struct selpark_arg {
    scase* cases;
    const uint16_t* lockorder;
    int n;
};

bool selparkcommit(G*, void* arg) noexcept {
    auto* a = static_cast<selpark_arg*>(arg);
    selunlock(a->cases, a->lockorder, a->n);
    return true;
}

}  // namespace

select_result selectgo(scase* cases, int nsends, int nrecvs, bool block) {
    int ncases = nsends + nrecvs;
    uint16_t pollorder[64 + 1];
    uint16_t lockorder[64 + 1];
    if (ncases > 64) panic("select: too many cases");

    // Random poll order, skipping nil channels.
    int norder = 0;
    for (int i = 0; i < ncases; i++) {
        if (!cases[i].c) continue;
        int j = static_cast<int>(runtime::fastrand() % static_cast<uint32_t>(norder + 1));
        pollorder[norder] = pollorder[j];
        pollorder[j] = static_cast<uint16_t>(i);
        norder++;
    }
    if (norder == 0) {
        if (!block) return {-1, false};
        block_forever(runtime::waitreason::select_no_cases);
    }

    // Lock order: by channel address, to avoid deadlock between selects.
    std::copy(pollorder, pollorder + norder, lockorder);
    std::sort(lockorder, lockorder + norder, [cases](uint16_t a, uint16_t b) {
        return std::less<hchan*>()(cases[a].c, cases[b].c);
    });

    sellock(cases, lockorder, norder);

    int casi = -1;
    scase* cas = nullptr;
    sudog* sg = nullptr;
    hchan* c = nullptr;
    bool recv_ok = false;

    // Pass 1: look for something that can proceed now.
    for (int k = 0; k < norder; k++) {
        casi = pollorder[k];
        cas = &cases[casi];
        c = cas->c;
        if (casi >= nsends) {
            if ((sg = c->sendq.dequeue())) {
                recv_from(c, sg, cas->elem, [&] { selunlock(cases, lockorder, norder); });
                return {casi, true};
            }
            if (c->qcount.load(std::memory_order_relaxed) > 0) {
                void* qp = c->slot(c->recvx);
                c->ops->move_construct(cas->elem, qp);
                c->ops->destroy(qp);
                if (++c->recvx == c->dataqsiz) c->recvx = 0;
                c->qcount.fetch_sub(1, std::memory_order_relaxed);
                selunlock(cases, lockorder, norder);
                return {casi, true};
            }
            if (c->closed.load(std::memory_order_relaxed) != 0) {
                selunlock(cases, lockorder, norder);
                return {casi, false};
            }
        } else {
            if (c->closed.load(std::memory_order_relaxed) != 0) {
                selunlock(cases, lockorder, norder);
                panic("send on closed channel");
            }
            if ((sg = c->recvq.dequeue())) {
                send_to(c, sg, cas->elem, [&] { selunlock(cases, lockorder, norder); });
                return {casi, false};
            }
            if (c->qcount.load(std::memory_order_relaxed) < c->dataqsiz) {
                c->ops->move_construct(c->slot(c->sendx), cas->elem);
                if (++c->sendx == c->dataqsiz) c->sendx = 0;
                c->qcount.fetch_add(1, std::memory_order_relaxed);
                selunlock(cases, lockorder, norder);
                return {casi, false};
            }
        }
    }

    if (!block) {
        selunlock(cases, lockorder, norder);
        return {-1, false};
    }

    // Pass 2: enqueue on all channels.
    G* gp = runtime::getg();
    sudog sgs[64];
    gp->waiting = nullptr;
    gp->select_done.store(0);
    sudog** nextp = &gp->waiting;
    for (int k = 0; k < norder; k++) {
        int ci = lockorder[k];
        sudog* s = &sgs[k];
        *s = sudog{};
        s->g = gp;
        s->is_select = true;
        s->elem = cases[ci].elem;
        s->c = cases[ci].c;
        *nextp = s;
        nextp = &s->waitlink;
        if (ci < nsends) cases[ci].c->sendq.enqueue(s);
        else cases[ci].c->recvq.enqueue(s);
    }

    gp->param = nullptr;
    selpark_arg arg{cases, lockorder, norder};
    runtime::park(selparkcommit, &arg, runtime::waitreason::select);

    gp = runtime::getg();
    sellock(cases, lockorder, norder);
    gp->select_done.store(0);
    sg = static_cast<sudog*>(gp->param);
    gp->param = nullptr;

    // Pass 3: dequeue from the channels that didn't win.
    casi = -1;
    cas = nullptr;
    bool case_success = false;
    sudog* sglist = gp->waiting;
    gp->waiting = nullptr;
    for (int k = 0; k < norder; k++) {
        int ci = lockorder[k];
        scase* k_cas = &cases[ci];
        if (sg == sglist) {
            casi = ci;
            cas = k_cas;
            case_success = sglist->success;
        } else if (ci < nsends) {
            k_cas->c->sendq.remove(sglist);
        } else {
            k_cas->c->recvq.remove(sglist);
        }
        sglist = sglist->waitlink;
    }
    if (!cas) runtime::fatal("selectgo: bad wakeup");

    if (casi < nsends) {
        if (!case_success) {
            selunlock(cases, lockorder, norder);
            panic("send on closed channel");
        }
    } else {
        recv_ok = case_success;
    }
    selunlock(cases, lockorder, norder);
    return {casi, recv_ok};
}

}  // namespace gopp::detail
