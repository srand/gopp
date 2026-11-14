// io.Pipe, ported from Go 1.27.3's io/pipe.go.
#include <gopp/chan.hpp>
#include <gopp/io.hpp>
#include <gopp/sync.hpp>

#include <mutex>

namespace gopp::io {

namespace detail {

// Go's onceError: keeps the first error stored.
class onceError {
public:
    void Store(error err) {
        std::lock_guard l(mu_);
        if (err_) return;
        err_ = std::move(err);
    }
    error Load() {
        std::lock_guard l(mu_);
        return err_;
    }

private:
    sync::Mutex mu_;
    error err_;
};

struct pipe {
    sync::Mutex wrMu;  // serializes Write operations
    chan<std::span<const byte>> wrCh = makeChan<std::span<const byte>>();
    chan<int> rdCh = makeChan<int>();
    sync::Once once;  // protects closing done
    chan<empty> done = makeChan<empty>();
    onceError rerr;
    onceError werr;

    bool isDone() {
        bool closed = false;
        select(recv(done, [&](empty) { closed = true; }), defaultCase([] {}));
        return closed;
    }

    std::pair<isize, error> read(std::span<byte> b) {
        if (isDone()) return {0, readCloseError()};
        std::pair<isize, error> r{0, nullptr};
        select(recv(wrCh,
                    [&](std::span<const byte> bw) {
                        int nr = int(gopp::Copy(b, bw));
                        rdCh.send(nr);
                        r = {nr, nullptr};
                    }),
               recv(done, [&](empty) { r = {0, readCloseError()}; }));
        return r;
    }

    error closeRead(error err) {
        if (!err) err = ErrClosedPipe;
        rerr.Store(std::move(err));
        once.Do([&] { done.close(); });
        return nullptr;
    }

    std::pair<isize, error> write(std::span<const byte> b) {
        if (isDone()) return {0, writeCloseError()};
        std::lock_guard l(wrMu);
        isize n = 0;
        for (bool first = true; first || !b.empty(); first = false) {
            bool closed = false;
            select(send(wrCh, b,
                        [&] {
                            int nw = rdCh.recv();
                            b = b.subspan(std::size_t(nw));
                            n += nw;
                        }),
                   recv(done, [&](empty) { closed = true; }));
            if (closed) return {n, writeCloseError()};
        }
        return {n, nullptr};
    }

    error closeWrite(error err) {
        if (!err) err = EOF_;
        werr.Store(std::move(err));
        once.Do([&] { done.close(); });
        return nullptr;
    }

    // The error a read returns once the pipe is closed.
    error readCloseError() {
        auto re = rerr.Load();
        if (auto we = werr.Load(); !re && we) return we;
        return ErrClosedPipe;
    }

    // The error a write returns once the pipe is closed.
    error writeCloseError() {
        auto we = werr.Load();
        if (auto re = rerr.Load(); !we && re) return re;
        return ErrClosedPipe;
    }
};

}  // namespace detail

// A dropped half closes, so that the other side doesn't wait forever: a
// dropped writer as with ErrClosedPipe, not io::EOF_, so that readers don't
// take a cut-off stream for a complete one.
PipeReader::~PipeReader() { p_->closeRead(nullptr); }
PipeWriter::~PipeWriter() { p_->closeWrite(ErrClosedPipe); }

std::pair<isize, error> PipeReader::Read(std::span<byte> p) { return p_->read(p); }
error PipeReader::Close() { return CloseWithError(nullptr); }
error PipeReader::CloseWithError(error err) { return p_->closeRead(std::move(err)); }

std::pair<isize, error> PipeWriter::Write(std::span<const byte> p) { return p_->write(p); }
error PipeWriter::Close() { return CloseWithError(nullptr); }
error PipeWriter::CloseWithError(error err) { return p_->closeWrite(std::move(err)); }

std::pair<ptr<PipeReader>, ptr<PipeWriter>> Pipe() {
    auto p = std::make_shared<detail::pipe>();
    return {make_ptr<PipeReader>(p), make_ptr<PipeWriter>(p)};
}

}  // namespace gopp::io
