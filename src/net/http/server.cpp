// The HTTP/1.x server, after Go's net/http/server.Go.
//
// Each connection runs on its own fiber: read a request, call the handler,
// finish the response, repeat while the connection can be reused.
#include "ascii.hpp"
#include "internal.hpp"

#include <gopp/chan.hpp>
#include <gopp/runtime.hpp>
#include <gopp/strings.hpp>
#include <gopp/sync.hpp>

#include <cerrno>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <exception>
#include <mutex>
#include <optional>
#include <unordered_set>

namespace gopp::http {

namespace {

constexpr std::size_t bufferBeforeChunking = 2048;
constexpr std::size_t sniffLen = 512;  // as DetectContentType reads
constexpr int64 maxPostHandlerReadBytes = 256 << 10;

enum class connState : int { fresh, active, idle, closed };

using internal::logf;

}  // namespace

void internal::logf(std::string_view msg) {
    // As Go's log package writes it, in local time.
    string ts = time::Now().Format("2006/01/02 15:04:05");
    std::fprintf(stderr, "%.*s %.*s\n", static_cast<int>(ts.size()), ts.data(), static_cast<int>(msg.size()), msg.data());
}

namespace {

bool bodyAllowedForStatus(int status) {
    if (status >= 100 && status <= 199) return false;
    if (status == 204 || status == 304) return false;
    return true;
}

bool temporaryAcceptError(const gopp::error& err) {
    if (auto* e = errors::As<net::Errno>(err)) {
        switch (e->Code) {
        case EMFILE: case ENFILE: case ENOBUFS: case ENOMEM: case ECONNABORTED: case EAGAIN: case EINTR: case ECONNRESET:
            return true;
        }
    }
    return false;
}

void appendStatusLine(std::string& out, bool is11, int code) {
    out.append(is11 ? "HTTP/1.1 " : "HTTP/1.0 ");
    char num[12];
    auto r = std::to_chars(num, num + sizeof(num), code);
    out.append(num, r.ptr);
    out.push_back(' ');
    std::string_view text = StatusText(code);
    if (text.empty()) {
        out.append("status code ");
        out.append(num, r.ptr);
    } else {
        out.append(text);
    }
    out.append("\r\n");
}

class conn;

// Writes to the network connection, recording the first error and
// canceling the current request (Go's checkConnErrorWriter).
class connWriter final : public io::Writer {
public:
    explicit connWriter(conn* c) : c_(c) {}
    std::pair<isize, gopp::error> Write(std::span<const byte> p) override;

private:
    conn* c_;
};

// Sends "100 Continue" when the handler first reads the body.
class expectContinueReader final : public io::ReadCloser {
public:
    expectContinueReader(conn* c, ptr<io::ReadCloser> body) : c_(c), body_(std::move(body)) {}
    std::pair<isize, gopp::error> Read(std::span<byte> p) override;
    gopp::error Close() override { return body_->Close(); }
    bool sawEof() const noexcept { return sawEof_; }
    const ptr<io::ReadCloser>& inner() const noexcept { return body_; }

private:
    conn* c_;
    ptr<io::ReadCloser> body_;
    bool sent_ = false;
    bool sawEof_ = false;
};

class response;

}  // namespace

namespace detail {

struct serverState {
    sync::Mutex mu;  // fiber-aware: held while closing listeners and connections, which may park
    std::vector<ptr<net::Listener>> listeners;
    std::unordered_set<conn*> conns;
    std::atomic<bool> shuttingDown{false};
};

}  // namespace detail

namespace {

class conn final : public object {
public:
    conn(const Server& srv, std::shared_ptr<detail::serverState> st, ptr<net::Conn> rwc)
        : handler_(srv.Handler),
          readTimeout_(srv.ReadTimeout),
          readHeaderTimeout_(srv.ReadHeaderTimeout.ns ? srv.ReadHeaderTimeout : srv.ReadTimeout),
          writeTimeout_(srv.WriteTimeout),
          idleTimeout_(srv.IdleTimeout.ns ? srv.IdleTimeout : srv.ReadTimeout),
          maxHeaderBytes_(srv.MaxHeaderBytes > 0 ? srv.MaxHeaderBytes : DefaultMaxHeaderBytes),
          disableKeepAlives_(srv.DisableKeepAlives),
          state_(std::move(st)),
          rwc(std::move(rwc)),
          bufr(make_ptr<bufio::Reader>(this->rwc, 4096)),
          bufw(make_ptr<connWriter>(this), 4096),
          protocols_(srv.Protocols.value_or(DefaultProtocols())) {
        if (!handler_) handler_ = ptr<Handler>(&DefaultServeMux());
    }

    void serve();

    // The settings HTTP/2 connections share with HTTP/1.
    const internal::serverOptions* thisServerOptions() {
        opts_.ReadTimeout = readTimeout_;
        opts_.WriteTimeout = writeTimeout_;
        opts_.IdleTimeout = idleTimeout_;
        opts_.MaxHeaderBytes = maxHeaderBytes_;
        opts_.DisableKeepAlives = disableKeepAlives_;
        opts_.state = state_;
        opts_.setActive = [this](bool active) {
            st_.store(active ? connState::active : connState::idle, std::memory_order_relaxed);
        };
        opts_.onShutdown = [this](std::function<void()> fn) {
            std::lock_guard l(state_->mu);
            h2Shutdown = std::move(fn);
        };
        return &opts_;
    }
    internal::serverOptions opts_;

    bool keepAlivesEnabled() const {
        return !disableKeepAlives_ && !state_->shuttingDown.load(std::memory_order_relaxed);
    }

    connState state() const noexcept { return st_.load(std::memory_order_relaxed); }

    ptr<Handler> handler_;
    time::Duration readTimeout_;
    time::Duration readHeaderTimeout_;
    time::Duration writeTimeout_;
    time::Duration idleTimeout_;
    int64 maxHeaderBytes_;
    bool disableKeepAlives_;
    std::shared_ptr<detail::serverState> state_;

    ptr<net::Conn> rwc;
    string RemoteAddr;
    ptr<bufio::Reader> bufr;
    bufio::Writer bufw;
    gopp::error werr;
    Request* curReq = nullptr;
    std::atomic<connState> st_{connState::fresh};
    Protocols protocols_;
    std::optional<tls::ConnectionState> tlsState;
    // For an HTTP/2 connection: starts its graceful shutdown. Guarded by
    // state_->mu.
    std::function<void()> h2Shutdown;

private:
    void writeErrorResponse(const gopp::error& err);
    void closeWriteAndWait();
};

std::pair<isize, gopp::error> connWriter::Write(std::span<const byte> p) {
    auto [n, err] = c_->rwc->Write(p);
    if (err && !c_->werr) {
        c_->werr = err;
        if (c_->curReq) c_->curReq->CancelContext();
    }
    return {n, err};
}

std::pair<isize, gopp::error> expectContinueReader::Read(std::span<byte> p) {
    if (!sent_) {
        sent_ = true;
        c_->bufw.WriteString("HTTP/1.1 100 Continue\r\n\r\n");
        c_->bufw.Flush();
    }
    auto [n, err] = body_->Read(p);
    if (err == io::EOF_) sawEof_ = true;
    return {n, err};
}

// The ResponseWriter of a request. Writes go through a 2048-byte buffer,
// so that small responses get a Content-Length and a sniffed Content-Type,
// then through the chunk writer (chunked encoding when the length isn't
// known), into the connection's 4 KiB buffer.
class response final : public ResponseWriter, public Flusher, public io::ReaderFrom, public internal::requestTooLarger {
public:
    response(conn* c, Request* req) : c_(c), req_(req) {
        wants10KeepAlive_ = !req->ProtoAtLeast(1, 1) && internal::hasToken(req->Header.Get("Connection"), "keep-alive");
        wantsClose_ = internal::hasToken(req->Header.Get("Connection"), "close");
    }

    http::Header& Header() override { return handlerHeader_; }

    // A MaxBytesReader hit its limit: the rest of the body won't be read,
    // so the connection can't be reused.
    void requestTooLarge() override {
        closeAfterReply_ = true;
        if (!wroteHeader_) handlerHeader_.Set("Connection", "close");
    }

    void WriteHeader(int code) override {
        if (wroteHeader_) {
            logf("http: superfluous response.WriteHeader call");
            return;
        }
        if (code < 100 || code > 999) panic(fmtCode("invalid WriteHeader code ", code));
        if (code >= 100 && code <= 199 && code != 101) {
            // Informational: send it now; the final status follows.
            std::string out;
            appendStatusLine(out, req_->ProtoAtLeast(1, 1), code);
            handlerHeader_.WriteTo(out);
            out.append("\r\n");
            c_->bufw.WriteString(out);
            c_->bufw.Flush();
            return;
        }
        wroteHeader_ = true;
        status_ = code;
        string cl = handlerHeader_.Get("Content-Length");
        if (!cl.empty()) {
            auto [v, err] = strconv::ParseInt(cl, 10, 64);
            if (!err && v >= 0) {
                contentLength_ = v;
            } else {
                logf("http: invalid Content-Length of " + std::string(strconv::Quote(cl).view()));
                handlerHeader_.Del("Content-Length");
            }
        }
    }

    std::pair<isize, gopp::error> Write(std::span<const byte> p) override {
        if (!wroteHeader_) WriteHeader(StatusOK);
        if (p.empty()) return {0, nullptr};
        if (!bodyAllowedForStatus(status_)) return {0, ErrBodyNotAllowed};
        written_ += static_cast<int64>(p.size());
        if (contentLength_ != -1 && written_ > contentLength_) return {0, ErrContentLength};
        isize total = static_cast<isize>(p.size());
        while (p.size() > bufferBeforeChunking - n_) {
            if (n_ == 0) {
                auto [m, err] = cwWrite(p);
                return {err ? m : total, err};
            }
            std::size_t k = bufferBeforeChunking - n_;
            std::memcpy(buf_ + n_, p.data(), k);
            n_ += k;
            p = p.subspan(k);
            if (auto err = flushBuf()) return {total - static_cast<isize>(p.size()), err};
        }
        std::memcpy(buf_ + n_, p.data(), p.size());
        n_ += p.size();
        return {total, nullptr};
    }

    void Flush() override {
        if (!wroteHeader_) WriteHeader(StatusOK);
        flushBuf();
        if (!cwWrote_) cwWriteHeader({});
        c_->bufw.Flush();
    }

    // response.ReadFrom: io::ReaderFrom, so that io::Copy and io::CopyN
    // hand the body to the connection's ReadFrom once the header is out: a
    // file goes to a TCP connection with sendfile(2).
    std::pair<int64, gopp::error> ReadFrom(io::Reader& src) override {
        struct writerOnly final : io::Writer {
            response& w;
            explicit writerOnly(response& w_) : w(w_) {}
            std::pair<isize, gopp::error> Write(std::span<const byte> p) override { return w.Write(p); }
        } wo(*this);

        auto* rf = dynamic_cast<io::ReaderFrom*>(c_->rwc.get());
        if (!rf) return io::Copy(wo, src);  // TLS

        // Copy the first sniffLen bytes first: the header isn't sent
        // before the source has data (Go issue 5660), and they're there for
        // sniffing the Content-Type.
        int64 n = 0;
        if (!cwWrote_) {
            byte buf[sniffLen];
            std::size_t got = 0;
            while (got < sniffLen) {
                auto [k, rerr] = src.Read(std::span<byte>(buf + got, sniffLen - got));
                if (k > 0) {
                    auto [m, werr] = Write(std::span<const byte>(buf + got, static_cast<std::size_t>(k)));
                    n += m;
                    if (werr) return {n, werr};
                    got += static_cast<std::size_t>(k);
                }
                if (rerr == io::EOF_) return {n, nullptr};
                if (rerr) return {n, rerr};
            }
        }

        // Out with what's buffered and the header; chunking_ is decided now.
        flushBuf();
        if (!cwWrote_) cwWriteHeader({});
        c_->bufw.Flush();

        if (!chunking_ && bodyAllowedForStatus(status_) && req_->Method != "HEAD") {
            auto [m, err] = rf->ReadFrom(src);
            n += m;
            written_ += m;
            if (err && !c_->werr) {
                // As connWriter does for the other writes.
                c_->werr = err;
                if (c_->curReq) c_->curReq->CancelContext();
            }
            return {n, err};
        }
        auto [m, err] = io::Copy(wo, src);
        return {n + m, err};
    }

    void finishRequest() {
        handlerDone_ = true;
        if (!wroteHeader_) WriteHeader(StatusOK);
        flushBuf();
        cwClose();
        c_->bufw.Flush();
        // Close the body so that its unread rest is discarded.
        req_->Body->Close();
    }

    bool shouldReuseConnection() const {
        if (closeAfterReply_) return false;
        if (req_->Method != "HEAD" && contentLength_ != -1 && bodyAllowedForStatus(status_) &&
            contentLength_ != written_)
            return false;  // wrote less than declared
        if (c_->werr) return false;
        if (closedRequestBodyEarly()) return false;
        return true;
    }

private:
    static string fmtCode(std::string_view msg, int code) { return string(msg) + strconv::Itoa(code); }

    internal::bodyReader* requestBody() const {
        io::ReadCloser* b = req_->Body.get();
        if (auto* ecr = dynamic_cast<expectContinueReader*>(b)) b = ecr->inner().get();
        return dynamic_cast<internal::bodyReader*>(b);
    }

public:
    bool closedRequestBodyEarly() const {
        internal::bodyReader* b = requestBody();
        return b && b->closed() && (!b->sawEof() || b->closeFailed());
    }

private:

    gopp::error flushBuf() {
        if (n_ == 0) return nullptr;
        auto [m, err] = cwWrite(std::span<const byte>(buf_, n_));
        n_ = 0;
        return err;
    }

    // chunkWriter.Write
    std::pair<isize, gopp::error> cwWrite(std::span<const byte> p) {
        if (!cwWrote_) cwWriteHeader(p);
        if (req_->Method == "HEAD") return {static_cast<isize>(p.size()), nullptr};  // no body
        if (chunking_) {
            char hex[20];
            auto r = std::to_chars(hex, hex + 16, p.size(), 16);
            *r.ptr++ = '\r';
            *r.ptr++ = '\n';
            if (auto [n, err] = c_->bufw.WriteString(std::string_view(hex, static_cast<std::size_t>(r.ptr - hex))); err) {
                c_->rwc->Close();
                return {0, err};
            }
        }
        auto [n, err] = c_->bufw.Write(p);
        if (chunking_ && !err) err = c_->bufw.WriteString("\r\n").second;
        if (err) c_->rwc->Close();
        return {n, err};
    }

    // chunkWriter.Close: the last chunk.
    void cwClose() {
        if (!cwWrote_) cwWriteHeader({});
        if (chunking_) c_->bufw.WriteString("0\r\n\r\n");
    }

    // chunkWriter.writeHeader: decides on framing and writes the header.
    // p is the first part of the body, for sniffing and, once the handler
    // is done, the Content-Length.
    void cwWriteHeader(std::span<const byte> p) {
        if (cwWrote_) return;
        cwWrote_ = true;

        http::Header& header = handlerHeader_;
        bool isHead = req_->Method == "HEAD";
        bool keepAlives = c_->keepAlivesEnabled();
        string te = header.Get("Transfer-Encoding");
        bool hasTe = !te.empty();
        string setCl, setCt, setConn, setTe;

        // A finished handler that set no Content-Length: we know it now.
        if (handlerDone_ && bodyAllowedForStatus(status_) && !header.Has("Content-Length") && !hasTe &&
            (!isHead || !p.empty())) {
            contentLength_ = static_cast<int64>(p.size());
            setCl = strconv::Itoa(contentLength_);
        }

        // HTTP/1.0 keep-alive needs a known length.
        if (wants10KeepAlive_ && keepAlives) {
            bool sentLength = !header.Get("Content-Length").empty() || !setCl.empty();
            if (sentLength && header.Get("Connection") == "keep-alive") closeAfterReply_ = false;
        }
        bool hasCl = contentLength_ != -1;
        if (wants10KeepAlive_ && (isHead || hasCl || !bodyAllowedForStatus(status_))) {
            if (!header.Has("Connection")) setConn = string::fromStatic("keep-alive");
        } else if (!req_->ProtoAtLeast(1, 1) || wantsClose_) {
            closeAfterReply_ = true;
        }
        if (header.Get("Connection") == "close" || !keepAlives) closeAfterReply_ = true;

        // A client waiting for 100 Continue that we never read from.
        if (auto* ecr = dynamic_cast<expectContinueReader*>(req_->Body.get()); ecr && !ecr->sawEof()) {
            closeAfterReply_ = true;
        }

        // Discard a small unread request body, so that the connection can
        // be reused and clients that write their whole request before
        // reading the response don't deadlock.
        if (req_->ContentLength != 0 && !closeAfterReply_) {
            if (internal::bodyReader* b = requestBody()) {
                bool discard = false, tooBig = false;
                if (b->closed()) {
                    if (!b->sawEof()) closeAfterReply_ = true;
                } else if (b->unreadSize() >= maxPostHandlerReadBytes) {
                    tooBig = true;
                } else {
                    discard = true;
                }
                if (discard) {
                    gopp::error err = b->Close();
                    if (err || b->closeFailed()) closeAfterReply_ = true;
                }
                if (tooBig) {
                    closeAfterReply_ = true;
                    header.Del("Connection");
                    setConn = string::fromStatic("close");
                }
            }
        }

        int code = status_;
        if (bodyAllowedForStatus(code)) {
            bool haveType = header.Has("Content-Type");
            bool hasCe = !header.Get("Content-Encoding").empty();
            if (!hasCe && !haveType && !hasTe && !p.empty()) setCt = DetectContentType(p);
        } else {
            if (code == 304) header.Del("Content-Type");
            header.Del("Content-Length");
            header.Del("Transfer-Encoding");
        }
        bool setDate = !header.Has("Date");

        if (hasCl && hasTe && te != "identity") {
            header.Del("Content-Length");
            hasCl = false;
        }
        if (isHead || !bodyAllowedForStatus(code) || code == StatusNoContent) {
            header.Del("Transfer-Encoding");
        } else if (hasCl) {
            header.Del("Transfer-Encoding");
        } else if (req_->ProtoAtLeast(1, 1)) {
            if (hasTe && te == "identity") {
                chunking_ = false;
                closeAfterReply_ = true;
                header.Del("Transfer-Encoding");
            } else {
                // Unknown length: chunked encoding keeps the connection.
                chunking_ = true;
                setTe = string::fromStatic("chunked");
                if (hasTe && te == "chunked") header.Del("Transfer-Encoding");
            }
        } else {
            // HTTP/1.0 without a length: the end of the body is the end of
            // the connection.
            closeAfterReply_ = true;
            header.Del("Transfer-Encoding");
        }
        if (chunking_) {
            header.Del("Content-Length");
            setCl = {};
        }

        bool delConn = closeAfterReply_ && (!keepAlives || !internal::hasToken(header.Get("Connection"), "close")) &&
                        code != StatusSwitchingProtocols;
        if (delConn) {
            header.Del("Connection");
            if (req_->ProtoAtLeast(1, 1)) setConn = string::fromStatic("close");
        }

        std::string out;
        out.reserve(256);
        appendStatusLine(out, req_->ProtoAtLeast(1, 1), code);
        header.WriteTo(out);
        auto extra = [&](std::string_view k, const string& v) {
            if (v.empty()) return;
            out.append(k);
            out.append(v.view());
            out.append("\r\n");
        };
        extra("Content-Type: ", setCt);
        extra("Connection: ", setConn);
        extra("Transfer-Encoding: ", setTe);
        if (setDate) {
            out.append("Date: ");
            internal::appendDate(out);
            out.append("\r\n");
        }
        extra("Content-Length: ", setCl);
        out.append("\r\n");
        c_->bufw.WriteString(out);
    }

    conn* c_;
    Request* req_;
    http::Header handlerHeader_;
    bool wroteHeader_ = false;
    int status_ = 0;
    int64 contentLength_ = -1;  // declared, -1 if unknown
    int64 written_ = 0;
    bool closeAfterReply_ = false;
    bool wants10KeepAlive_ = false;
    bool wantsClose_ = false;
    bool handlerDone_ = false;

    byte buf_[bufferBeforeChunking];
    std::size_t n_ = 0;

    bool cwWrote_ = false;
    bool chunking_ = false;
};

void conn::writeErrorResponse(const gopp::error& err) {
    int code = 400;
    std::string text = "Bad Request";
    std::string detail;
    if (err == internal::errHeaderTooLarge) {
        code = 431;
        text = "Request Header Fields Too Large";
    } else if (auto* se = errors::As<internal::statusError>(err)) {
        code = se->code;
        text = std::string(StatusText(code));
        detail = std::string(se->text.view());
    }
    std::string out = "HTTP/1.1 " + std::to_string(code) + " " + text +
                      "\r\nContent-Type: text/plain; charset=utf-8\r\nConnection: close\r\n\r\n" + std::to_string(code) +
                      " " + text;
    if (!detail.empty()) out += ": " + detail;
    rwc->SetWriteDeadline(time::Now().Add(time::Second));
    io::WriteString(*rwc, out);
    closeWriteAndWait();
}

void conn::closeWriteAndWait() {
    // Closing with unread request bytes makes the kernel send RST, which
    // can destroy the response before the client reads it. Send FIN first
    // and give the client a moment (Go's rstAvoidanceDelay).
    bufw.Flush();
    if (auto* tcp = dynamic_cast<net::TCPConn*>(rwc.get())) tcp->CloseWrite();
    time::Sleep(500 * time::Millisecond);
}

void conn::serve() {
    if (auto ra = rwc->RemoteAddr()) RemoteAddr = ra->String();
    struct closer {
        conn* c;
        ~closer() {
            c->bufw.Flush();
            c->rwc->Close();
            c->st_.store(connState::closed);
            std::lock_guard l(c->state_->mu);
            c->state_->conns.erase(c);
        }
    } closer{this};

    if (auto* tc = dynamic_cast<tls::Conn*>(rwc.get())) {
        // The handshake must finish within the time for a request header.
        time::Duration limit = readHeaderTimeout_.ns > 0 ? readHeaderTimeout_ : writeTimeout_;
        if (limit.ns > 0) rwc->SetDeadline(time::Now().Add(limit));
        if (gopp::error err = tc->Handshake()) {
            if (err != io::EOF_ && !errors::Is(err, net::ErrClosed)) {
                logf("http: TLS handshake error from " + std::string(RemoteAddr.view()) + ": " +
                     std::string(err.Error().view()));
            }
            return;
        }
        if (limit.ns > 0) rwc->SetDeadline(time::Time{});
        tlsState = tc->ConnectionState();
        if (tlsState->NegotiatedProtocol == "h2" && protocols_.HTTP2() && internal::http2Enabled) {
            internal::serveHttp2(rwc, bufr, handler_, *thisServerOptions(), tlsState);
            return;
        }
    } else if (protocols_.UnencryptedHTTP2() && internal::http2Enabled) {
        // HTTP/2 with prior knowledge: the client starts with the preface.
        rwc->SetReadDeadline(readHeaderTimeout_.ns > 0 ? time::Now().Add(readHeaderTimeout_) : time::Time{});
        auto [head, perr] = bufr->Peek(internal::http2Preface.size());
        if (!perr && asStringView(head) == internal::http2Preface) {
            rwc->SetReadDeadline(time::Time{});
            internal::serveHttp2(rwc, bufr, handler_, *thisServerOptions(), std::nullopt);
            return;
        }
        if (!protocols_.HTTP1()) return;
    }

    for (;;) {
        if (readHeaderTimeout_.ns > 0) rwc->SetReadDeadline(time::Now().Add(readHeaderTimeout_));
        auto [req, err] = internal::readRequest(bufr, maxHeaderBytes_);
        if (err) {
            if (err == io::EOF_ || err == io::ErrUnexpectedEOF || errors::Is(err, net::ErrClosed)) return;
            if (err->Timeout() || errors::As<net::OpError>(err)) return;  // network read errors
            writeErrorResponse(err);
            return;
        }
        st_.store(connState::active, std::memory_order_relaxed);
        if (readTimeout_.ns > 0 && readHeaderTimeout_.ns != readTimeout_.ns) {
            rwc->SetReadDeadline(time::Now().Add(readTimeout_));
        } else if (readHeaderTimeout_.ns > 0 && readTimeout_.ns == 0) {
            rwc->SetReadDeadline(time::Time{});
        }
        if (writeTimeout_.ns > 0) rwc->SetWriteDeadline(time::Now().Add(writeTimeout_));

        req->RemoteAddr = RemoteAddr;
        req->TLS = tlsState;

        string expect = req->Header.Get("Expect");
        if (!expect.empty()) {
            if (ascii::EqualFold(expect, "100-continue") && req->ProtoAtLeast(1, 1) && req->ContentLength != 0) {
                req->Body = make_ptr<expectContinueReader>(this, req->Body);
            } else if (!ascii::EqualFold(expect, "100-continue")) {
                io::WriteString(*rwc, "HTTP/1.1 417 Expectation Failed\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                return;
            }
            req->Header.Del("Expect");
        }

        response w(this, req.get());
        curReq = req.get();
        // A handler that throws, as a Go handler panics, drops the
        // connection; throwing ErrAbortHandler does so without a log.
        std::optional<std::string> panicked;
        try {
            handler_->ServeHTTP(w, *req);
        } catch (const gopp::error& e) {
            panicked = e == ErrAbortHandler ? std::string() : std::string(e.Error().view());
        } catch (const std::exception& e) {
            panicked = e.what();
        }
        if (panicked) {
            if (!panicked->empty()) logf("http: panic serving " + std::string(RemoteAddr.view()) + ": " + *panicked);
            req->CancelContext();
            if (req->MultipartForm) req->MultipartForm->RemoveAll();
            curReq = nullptr;
            return;  // like Go: drop the connection
        }
        req->CancelContext();
        w.finishRequest();
        if (req->MultipartForm) req->MultipartForm->RemoveAll();
        curReq = nullptr;
        if (!w.shouldReuseConnection()) {
            if (w.closedRequestBodyEarly()) closeWriteAndWait();
            return;
        }
        st_.store(connState::idle, std::memory_order_relaxed);
        if (!keepAlivesEnabled()) return;
        if (idleTimeout_.ns > 0) {
            rwc->SetReadDeadline(time::Now().Add(idleTimeout_));
        }
        if (writeTimeout_.ns > 0) rwc->SetWriteDeadline(time::Time{});
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// Server

std::shared_ptr<detail::serverState> Server::state() {
    static std::mutex createMu;
    std::lock_guard l(createMu);
    if (!state_) state_ = std::make_shared<detail::serverState>();
    return state_;
}

gopp::error Server::ListenAndServe() {
    auto st = state();
    if (st->shuttingDown.load()) return ErrServerClosed;
    string a = Addr.empty() ? string::fromStatic(":http") : Addr;
    auto [ln, err] = net::Listen("tcp", a);
    if (err) return err;
    return Serve(ln);
}

gopp::error Server::Serve(ptr<net::Listener> l) {
    auto st = state();
    {
        std::lock_guard lk(st->mu);
        if (st->shuttingDown.load()) return ErrServerClosed;
        st->listeners.push_back(l);
    }
    time::Duration delay{};
    for (;;) {
        auto [rw, err] = l->Accept();
        if (err) {
            if (st->shuttingDown.load()) return ErrServerClosed;
            if (temporaryAcceptError(err)) {
                delay = delay.ns == 0 ? 5 * time::Millisecond : delay * 2;
                if (delay > time::Second) delay = time::Second;
                logf("http: Accept error: " + std::string(err.Error().view()) + "; retrying in " +
                     std::string(delay.String().view()));
                time::Sleep(delay);
                continue;
            }
            return err;
        }
        delay = {};
        auto c = make_ptr<conn>(*this, st, rw);
        {
            std::lock_guard lk(st->mu);
            st->conns.insert(c.get());
        }
        go([c] { c->serve(); });
    }
}

gopp::error Server::Close() {
    auto st = state();
    st->shuttingDown.store(true);
    std::lock_guard lk(st->mu);
    gopp::error first;
    for (auto& l : st->listeners) {
        if (auto err = l->Close(); err && !first) first = err;
    }
    st->listeners.clear();
    for (conn* c : st->conns) c->rwc->Close();
    return first;
}

gopp::error Server::Shutdown(ptr<context::Context> ctx) {
    auto st = state();
    st->shuttingDown.store(true);
    gopp::error first;
    {
        std::lock_guard lk(st->mu);
        for (auto& l : st->listeners) {
            if (auto err = l->Close(); err && !first) first = err;
        }
        st->listeners.clear();
    }
    time::Duration interval = time::Millisecond;
    bool firstRound = true;
    for (;;) {
        bool quiescent = true;
        {
            std::lock_guard lk(st->mu);
            for (conn* c : st->conns) {
                if (c->h2Shutdown) {
                    // HTTP/2 connections send GOAWAY and close themselves
                    // once their streams are done.
                    if (firstRound) c->h2Shutdown();
                    quiescent = false;
                    continue;
                }
                connState s = c->state();
                if (s == connState::idle || s == connState::fresh) c->rwc->Close();
                else quiescent = false;
            }
            firstRound = false;
            if (quiescent && st->conns.empty()) return first;
        }
        gopp::error done;
        select(recv(ctx->Done(), [&] { done = ctx->Err(); }), recv(time::After(interval), [] {}));
        if (done) return done;
        if (interval < 500 * time::Millisecond) interval = interval * 2;
    }
}

namespace {

// The TLS config of a server: TLSConfig with the certificate files added,
// and ALPN for the enabled protocols.
std::pair<tls::Config, gopp::error> serverTlsConfig(const Server& srv, std::string_view certFile,
                                                      std::string_view keyFile) {
    tls::Config cfg = srv.TLSConfig.value_or(tls::Config{});
    cfg.ctx_ = nullptr;  // compiled anew with the changes below
    if (cfg.Certificates.empty() || !certFile.empty() || !keyFile.empty()) {
        auto [cert, err] = tls::LoadX509KeyPair(certFile, keyFile);
        if (err) return {cfg, err};
        cfg.Certificates.push_back(cert);
    }
    if (cfg.NextProtos.empty()) {
        Protocols p = srv.Protocols.value_or(DefaultProtocols());
        if (p.HTTP2() && internal::http2Enabled) cfg.NextProtos.push_back(string::fromStatic("h2"));
        if (p.HTTP1()) cfg.NextProtos.push_back(string::fromStatic("http/1.1"));
    }
    return {cfg, nullptr};
}

}  // namespace

gopp::error Server::ServeTLS(ptr<net::Listener> l, std::string_view certFile, std::string_view keyFile) {
    auto [cfg, err] = serverTlsConfig(*this, certFile, keyFile);
    if (err) return err;
    return Serve(tls::NewListener(std::move(l), cfg));
}

gopp::error Server::ListenAndServeTLS(std::string_view certFile, std::string_view keyFile) {
    auto st = state();
    if (st->shuttingDown.load()) return ErrServerClosed;
    string a = Addr.empty() ? string::fromStatic(":https") : Addr;
    auto [ln, err] = net::Listen("tcp", a);
    if (err) return err;
    return ServeTLS(ln, certFile, keyFile);
}

gopp::error ListenAndServeTLS(string addr, std::string_view certFile, std::string_view keyFile, ptr<Handler> handler) {
    Server srv{.Addr = std::move(addr), .Handler = std::move(handler)};
    return srv.ListenAndServeTLS(certFile, keyFile);
}

string Protocols::String() const {
    std::string s = "{";
    auto add = [&](bool on, const char* name) {
        if (!on) return;
        if (s.size() > 1) s += ",";
        s += name;
    };
    add(HTTP1(), "HTTP1");
    add(HTTP2(), "HTTP2");
    add(UnencryptedHTTP2(), "UnencryptedHTTP2");
    return string(s + "}");
}

Protocols DefaultProtocols() noexcept {
    Protocols p;
    p.SetHTTP1(true);
    p.SetHTTP2(true);
    return p;
}

gopp::error ListenAndServe(string addr, ptr<Handler> handler) {
    Server srv{.Addr = std::move(addr), .Handler = std::move(handler)};
    return srv.ListenAndServe();
}

}  // namespace gopp::http
