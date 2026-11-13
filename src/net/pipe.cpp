// net.Pipe, ported from Go 1.27.3's net/pipe.go.
#include <gopp/chan.hpp>
#include <gopp/net.hpp>
#include <gopp/sync.hpp>

#include <atomic>
#include <mutex>

namespace gopp::net {

namespace {

bool isClosedChan(const chan<empty>& c) {
    bool closed = false;
    select(recv(c, [&](empty) { closed = true; }), default_case([] {}));
    return closed;
}

// Go's pipeDeadline: a channel closed when the deadline passes.
class pipeDeadline {
public:
    pipeDeadline() : cancel_(make_chan<empty>()) {}

    void set(time::Time t) {
        std::lock_guard l(mu_);
        if (timer_ && !timer_->Stop()) {
            cancel_.recv();  // wait for the timer callback to close cancel
        }
        timer_ = nullptr;
        bool closed = isClosedChan(cancel_);
        if (t.IsZero()) {
            // No deadline: an open channel.
            if (closed) cancel_ = make_chan<empty>();
            return;
        }
        if (time::Duration dur = time::Until(t); dur.ns > 0) {
            // A deadline in the future.
            if (closed) cancel_ = make_chan<empty>();
            auto c = cancel_;
            timer_ = time::AfterFunc(dur, [c] { c.close(); });
            return;
        }
        // A deadline in the past.
        if (!closed) cancel_.close();
    }

    chan<empty> wait() {
        std::lock_guard l(mu_);
        return cancel_;
    }

private:
    sync::Mutex mu_;  // guards timer and cancel
    ptr<time::Timer> timer_;
    chan<empty> cancel_;  // never nil
};

struct pipeAddr final : Addr {
    string Network() const override { return string::from_static("pipe"); }
    string String() const override { return string::from_static("pipe"); }
};

class pipe final : public Conn {
public:
    pipe(chan<std::span<const byte>> rdRx, chan<int> rdTx, chan<std::span<const byte>> wrTx, chan<int> wrRx,
         chan<empty> localDone, chan<empty> remoteDone)
        : rdRx_(rdRx), rdTx_(rdTx), wrTx_(wrTx), wrRx_(wrRx), localDone_(localDone), remoteDone_(remoteDone) {}

    // As a garbage-collected pipe in Go eventually is, a dropped one is
    // closed, so that its peer doesn't wait forever.
    ~pipe() override { Close(); }

    ptr<net::Addr> LocalAddr() const override { return make_ptr<pipeAddr>(); }
    ptr<net::Addr> RemoteAddr() const override { return make_ptr<pipeAddr>(); }

    std::pair<isize, error> Read(std::span<byte> b) override {
        auto [n, err] = read(b);
        if (err && err != io::EOF_ && err != io::ErrClosedPipe) err = opError("read", err);
        return {n, err};
    }

    std::pair<isize, error> Write(std::span<const byte> b) override {
        auto [n, err] = write(b);
        if (err && err != io::ErrClosedPipe) err = opError("write", err);
        return {n, err};
    }

    error SetDeadline(time::Time t) override {
        if (isClosedChan(localDone_) || isClosedChan(remoteDone_)) return io::ErrClosedPipe;
        readDeadline_.set(t);
        writeDeadline_.set(t);
        return nullptr;
    }

    error SetReadDeadline(time::Time t) override {
        if (isClosedChan(localDone_) || isClosedChan(remoteDone_)) return io::ErrClosedPipe;
        readDeadline_.set(t);
        return nullptr;
    }

    error SetWriteDeadline(time::Time t) override {
        if (isClosedChan(localDone_) || isClosedChan(remoteDone_)) return io::ErrClosedPipe;
        writeDeadline_.set(t);
        return nullptr;
    }

    error Close() override {
        if (!closed_.exchange(true)) localDone_.close();
        return nullptr;
    }

private:
    static error opError(std::string_view op, error err) {
        auto e = make_ptr<OpError>();
        e->Op = string(op);
        e->Net = string::from_static("pipe");
        e->Err = std::move(err);
        return e;
    }

    std::pair<isize, error> read(std::span<byte> b) {
        if (isClosedChan(localDone_)) return {0, io::ErrClosedPipe};
        if (isClosedChan(remoteDone_)) return {0, io::EOF_};
        if (isClosedChan(readDeadline_.wait())) return {0, os::ErrDeadlineExceeded};
        std::pair<isize, error> r{0, nullptr};
        select(recv(rdRx_,
                    [&](std::span<const byte> bw) {
                        int nr = int(Copy(b, bw));
                        rdTx_.send(nr);
                        r = {nr, nullptr};
                    }),
               recv(localDone_, [&](empty) { r = {0, io::ErrClosedPipe}; }),
               recv(remoteDone_, [&](empty) { r = {0, io::EOF_}; }),
               recv(readDeadline_.wait(), [&](empty) { r = {0, os::ErrDeadlineExceeded}; }));
        return r;
    }

    std::pair<isize, error> write(std::span<const byte> b) {
        if (isClosedChan(localDone_)) return {0, io::ErrClosedPipe};
        if (isClosedChan(remoteDone_)) return {0, io::ErrClosedPipe};
        if (isClosedChan(writeDeadline_.wait())) return {0, os::ErrDeadlineExceeded};
        std::lock_guard l(wrMu_);  // ensure entirety of b is written together
        isize n = 0;
        for (bool once = true; once || !b.empty(); once = false) {
            error err;
            bool stop = false;
            select(send(wrTx_, b,
                        [&] {
                            int nw = wrRx_.recv();
                            b = b.subspan(std::size_t(nw));
                            n += nw;
                        }),
                   recv(localDone_, [&](empty) { err = io::ErrClosedPipe, stop = true; }),
                   recv(remoteDone_, [&](empty) { err = io::ErrClosedPipe, stop = true; }),
                   recv(writeDeadline_.wait(), [&](empty) { err = os::ErrDeadlineExceeded, stop = true; }));
            if (stop) return {n, err};
        }
        return {n, nullptr};
    }

    sync::Mutex wrMu_;  // serializes writes
    chan<std::span<const byte>> rdRx_;
    chan<int> rdTx_;
    chan<std::span<const byte>> wrTx_;
    chan<int> wrRx_;
    std::atomic<bool> closed_{false};
    chan<empty> localDone_;
    chan<empty> remoteDone_;
    pipeDeadline readDeadline_;
    pipeDeadline writeDeadline_;
};

}  // namespace

std::pair<ptr<Conn>, ptr<Conn>> Pipe() {
    auto cb1 = make_chan<std::span<const byte>>(), cb2 = make_chan<std::span<const byte>>();
    auto cn1 = make_chan<int>(), cn2 = make_chan<int>();
    auto done1 = make_chan<empty>(), done2 = make_chan<empty>();
    auto p1 = make_ptr<pipe>(cb1, cn1, cb2, cn2, done1, done2);
    auto p2 = make_ptr<pipe>(cb2, cn2, cb1, cn1, done2, done1);
    return {p1, p2};
}

}  // namespace gopp::net
