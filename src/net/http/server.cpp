// The HTTP/1.x server, after Go's net/http/server.Go.
//
// Each connection runs on its own fiber: read a request, call the handler,
// finish the response, repeat while the connection can be reused.
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
#include <unordered_set>

namespace gopp::http {

namespace {

constexpr std::size_t buffer_before_chunking = 2048;
constexpr std::size_t sniff_len = 512;  // as DetectContentType reads
constexpr int64 max_post_handler_read_bytes = 256 << 10;

enum class conn_state : int { fresh, active, idle, closed };

using internal::logf;

}  // namespace

void internal::logf(std::string_view msg) {
    // As Go's log package writes it, in local time.
    string ts = time::Now().Format("2006/01/02 15:04:05");
    std::fprintf(stderr, "%.*s %.*s\n", static_cast<int>(ts.size()), ts.data(), static_cast<int>(msg.size()), msg.data());
}

namespace {

bool body_allowed_for_status(int status) {
    if (status >= 100 && status <= 199) return false;
    if (status == 204 || status == 304) return false;
    return true;
}

bool temporary_accept_error(const gopp::error& err) {
    if (auto* e = errors::As<net::Errno>(err)) {
        switch (e->Code) {
        case EMFILE: case ENFILE: case ENOBUFS: case ENOMEM: case ECONNABORTED: case EAGAIN: case EINTR: case ECONNRESET:
            return true;
        }
    }
    return false;
}

void append_status_line(std::string& out, bool is11, int code) {
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
class conn_writer final : public io::Writer {
public:
    explicit conn_writer(conn* c) : c_(c) {}
    std::pair<isize, gopp::error> Write(std::span<const byte> p) override;

private:
    conn* c_;
};

// Sends "100 Continue" when the handler first reads the body.
class expect_continue_reader final : public io::ReadCloser {
public:
    expect_continue_reader(conn* c, ptr<io::ReadCloser> body) : c_(c), body_(std::move(body)) {}
    std::pair<isize, gopp::error> Read(std::span<byte> p) override;
    gopp::error Close() override { return body_->Close(); }
    bool saw_eof() const noexcept { return saw_eof_; }
    const ptr<io::ReadCloser>& inner() const noexcept { return body_; }

private:
    conn* c_;
    ptr<io::ReadCloser> body_;
    bool sent_ = false;
    bool saw_eof_ = false;
};

class response;

}  // namespace

namespace detail {

struct server_state {
    sync::Mutex mu;  // fiber-aware: held while closing listeners and connections, which may park
    std::vector<ptr<net::Listener>> listeners;
    std::unordered_set<conn*> conns;
    std::atomic<bool> shutting_down{false};
};

}  // namespace detail

namespace {

class conn final : public object {
public:
    conn(const Server& srv, std::shared_ptr<detail::server_state> st, ptr<net::Conn> rwc)
        : handler_(srv.Handler),
          read_timeout_(srv.ReadTimeout),
          read_header_timeout_(srv.ReadHeaderTimeout.ns ? srv.ReadHeaderTimeout : srv.ReadTimeout),
          write_timeout_(srv.WriteTimeout),
          idle_timeout_(srv.IdleTimeout.ns ? srv.IdleTimeout : srv.ReadTimeout),
          max_header_bytes_(srv.MaxHeaderBytes > 0 ? srv.MaxHeaderBytes : DefaultMaxHeaderBytes),
          disable_keep_alives_(srv.DisableKeepAlives),
          state_(std::move(st)),
          rwc(std::move(rwc)),
          bufr(make_ptr<bufio::Reader>(this->rwc, 4096)),
          bufw(make_ptr<conn_writer>(this), 4096),
          protocols_(srv.Protocols.value_or(DefaultProtocols())) {
        if (!handler_) handler_ = ptr<Handler>(&DefaultServeMux());
    }

    void serve();

    // The settings HTTP/2 connections share with HTTP/1.
    const internal::server_options* this_server_options() {
        opts_.ReadTimeout = read_timeout_;
        opts_.WriteTimeout = write_timeout_;
        opts_.IdleTimeout = idle_timeout_;
        opts_.MaxHeaderBytes = max_header_bytes_;
        opts_.DisableKeepAlives = disable_keep_alives_;
        opts_.state = state_;
        opts_.set_active = [this](bool active) {
            st_.store(active ? conn_state::active : conn_state::idle, std::memory_order_relaxed);
        };
        opts_.on_shutdown = [this](std::function<void()> fn) {
            std::lock_guard l(state_->mu);
            h2_shutdown = std::move(fn);
        };
        return &opts_;
    }
    internal::server_options opts_;

    bool keep_alives_enabled() const {
        return !disable_keep_alives_ && !state_->shutting_down.load(std::memory_order_relaxed);
    }

    conn_state state() const noexcept { return st_.load(std::memory_order_relaxed); }

    ptr<Handler> handler_;
    time::Duration read_timeout_;
    time::Duration read_header_timeout_;
    time::Duration write_timeout_;
    time::Duration idle_timeout_;
    int64 max_header_bytes_;
    bool disable_keep_alives_;
    std::shared_ptr<detail::server_state> state_;

    ptr<net::Conn> rwc;
    string RemoteAddr;
    ptr<bufio::Reader> bufr;
    bufio::Writer bufw;
    gopp::error werr;
    Request* cur_req = nullptr;
    std::atomic<conn_state> st_{conn_state::fresh};
    Protocols protocols_;
    std::optional<tls::ConnectionState> tls_state;
    // For an HTTP/2 connection: starts its graceful shutdown. Guarded by
    // state_->mu.
    std::function<void()> h2_shutdown;

private:
    void write_error_response(const gopp::error& err);
    void close_write_and_wait();
};

std::pair<isize, gopp::error> conn_writer::Write(std::span<const byte> p) {
    auto [n, err] = c_->rwc->Write(p);
    if (err && !c_->werr) {
        c_->werr = err;
        if (c_->cur_req) c_->cur_req->CancelContext();
    }
    return {n, err};
}

std::pair<isize, gopp::error> expect_continue_reader::Read(std::span<byte> p) {
    if (!sent_) {
        sent_ = true;
        c_->bufw.WriteString("HTTP/1.1 100 Continue\r\n\r\n");
        c_->bufw.Flush();
    }
    auto [n, err] = body_->Read(p);
    if (err == io::EOF_) saw_eof_ = true;
    return {n, err};
}

// The ResponseWriter of a request. Writes go through a 2048-byte buffer,
// so that small responses get a Content-Length and a sniffed Content-Type,
// then through the chunk writer (chunked encoding when the length isn't
// known), into the connection's 4 KiB buffer.
class response final : public ResponseWriter, public Flusher, public io::ReaderFrom {
public:
    response(conn* c, Request* req) : c_(c), req_(req) {
        wants_10_keep_alive_ = !req->ProtoAtLeast(1, 1) && internal::has_token(req->Header.Get("Connection"), "keep-alive");
        wants_close_ = internal::has_token(req->Header.Get("Connection"), "close");
    }

    http::Header& Header() override { return handler_header_; }

    void WriteHeader(int code) override {
        if (wrote_header_) {
            logf("http: superfluous response.WriteHeader call");
            return;
        }
        if (code < 100 || code > 999) panic(fmt_code("invalid WriteHeader code ", code));
        if (code >= 100 && code <= 199 && code != 101) {
            // Informational: send it now; the final status follows.
            std::string out;
            append_status_line(out, req_->ProtoAtLeast(1, 1), code);
            handler_header_.WriteTo(out);
            out.append("\r\n");
            c_->bufw.WriteString(out);
            c_->bufw.Flush();
            return;
        }
        wrote_header_ = true;
        status_ = code;
        string cl = handler_header_.Get("Content-Length");
        if (!cl.empty()) {
            auto [v, err] = strconv::ParseInt(cl, 10, 64);
            if (!err && v >= 0) {
                content_length_ = v;
            } else {
                logf("http: invalid Content-Length of " + std::string(strconv::Quote(cl).view()));
                handler_header_.Del("Content-Length");
            }
        }
    }

    std::pair<isize, gopp::error> Write(std::span<const byte> p) override {
        if (!wrote_header_) WriteHeader(StatusOK);
        if (p.empty()) return {0, nullptr};
        if (!body_allowed_for_status(status_)) return {0, ErrBodyNotAllowed};
        written_ += static_cast<int64>(p.size());
        if (content_length_ != -1 && written_ > content_length_) return {0, ErrContentLength};
        isize total = static_cast<isize>(p.size());
        while (p.size() > buffer_before_chunking - n_) {
            if (n_ == 0) {
                auto [m, err] = cw_write(p);
                return {err ? m : total, err};
            }
            std::size_t k = buffer_before_chunking - n_;
            std::memcpy(buf_ + n_, p.data(), k);
            n_ += k;
            p = p.subspan(k);
            if (auto err = flush_buf()) return {total - static_cast<isize>(p.size()), err};
        }
        std::memcpy(buf_ + n_, p.data(), p.size());
        n_ += p.size();
        return {total, nullptr};
    }

    void Flush() override {
        if (!wrote_header_) WriteHeader(StatusOK);
        flush_buf();
        if (!cw_wrote_) cw_write_header({});
        c_->bufw.Flush();
    }

    // response.ReadFrom: io::ReaderFrom, so that io::Copy and io::CopyN
    // hand the body to the connection's ReadFrom once the header is out: a
    // file goes to a TCP connection with sendfile(2).
    std::pair<int64, gopp::error> ReadFrom(io::Reader& src) override {
        struct writer_only final : io::Writer {
            response& w;
            explicit writer_only(response& w_) : w(w_) {}
            std::pair<isize, gopp::error> Write(std::span<const byte> p) override { return w.Write(p); }
        } wo(*this);

        auto* rf = dynamic_cast<io::ReaderFrom*>(c_->rwc.get());
        if (!rf) return io::Copy(wo, src);  // TLS

        // Copy the first sniff_len bytes first: the header isn't sent
        // before the source has data (Go issue 5660), and they're there for
        // sniffing the Content-Type.
        int64 n = 0;
        if (!cw_wrote_) {
            byte buf[sniff_len];
            std::size_t got = 0;
            while (got < sniff_len) {
                auto [k, rerr] = src.Read(std::span<byte>(buf + got, sniff_len - got));
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
        flush_buf();
        if (!cw_wrote_) cw_write_header({});
        c_->bufw.Flush();

        if (!chunking_ && body_allowed_for_status(status_) && req_->Method != "HEAD") {
            auto [m, err] = rf->ReadFrom(src);
            n += m;
            written_ += m;
            if (err && !c_->werr) {
                // As conn_writer does for the other writes.
                c_->werr = err;
                if (c_->cur_req) c_->cur_req->CancelContext();
            }
            return {n, err};
        }
        auto [m, err] = io::Copy(wo, src);
        return {n + m, err};
    }

    void finish_request() {
        handler_done_ = true;
        if (!wrote_header_) WriteHeader(StatusOK);
        flush_buf();
        cw_close();
        c_->bufw.Flush();
        // Close the body so that its unread rest is discarded.
        req_->Body->Close();
    }

    bool should_reuse_connection() const {
        if (close_after_reply_) return false;
        if (req_->Method != "HEAD" && content_length_ != -1 && body_allowed_for_status(status_) &&
            content_length_ != written_)
            return false;  // wrote less than declared
        if (c_->werr) return false;
        if (closed_request_body_early()) return false;
        return true;
    }

private:
    static string fmt_code(std::string_view msg, int code) { return string(msg) + strconv::Itoa(code); }

    internal::body_reader* request_body() const {
        io::ReadCloser* b = req_->Body.get();
        if (auto* ecr = dynamic_cast<expect_continue_reader*>(b)) b = ecr->inner().get();
        return dynamic_cast<internal::body_reader*>(b);
    }

public:
    bool closed_request_body_early() const {
        internal::body_reader* b = request_body();
        return b && b->closed() && (!b->saw_eof() || b->close_failed());
    }

private:

    gopp::error flush_buf() {
        if (n_ == 0) return nullptr;
        auto [m, err] = cw_write(std::span<const byte>(buf_, n_));
        n_ = 0;
        return err;
    }

    // chunkWriter.Write
    std::pair<isize, gopp::error> cw_write(std::span<const byte> p) {
        if (!cw_wrote_) cw_write_header(p);
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
    void cw_close() {
        if (!cw_wrote_) cw_write_header({});
        if (chunking_) c_->bufw.WriteString("0\r\n\r\n");
    }

    // chunkWriter.writeHeader: decides on framing and writes the header.
    // p is the first part of the body, for sniffing and, once the handler
    // is done, the Content-Length.
    void cw_write_header(std::span<const byte> p) {
        if (cw_wrote_) return;
        cw_wrote_ = true;

        http::Header& header = handler_header_;
        bool is_head = req_->Method == "HEAD";
        bool keep_alives = c_->keep_alives_enabled();
        string te = header.Get("Transfer-Encoding");
        bool has_te = !te.empty();
        string set_cl, set_ct, set_conn, set_te;

        // A finished handler that set no Content-Length: we know it now.
        if (handler_done_ && body_allowed_for_status(status_) && !header.Has("Content-Length") && !has_te &&
            (!is_head || !p.empty())) {
            content_length_ = static_cast<int64>(p.size());
            set_cl = strconv::Itoa(content_length_);
        }

        // HTTP/1.0 keep-alive needs a known length.
        if (wants_10_keep_alive_ && keep_alives) {
            bool sent_length = !header.Get("Content-Length").empty() || !set_cl.empty();
            if (sent_length && header.Get("Connection") == "keep-alive") close_after_reply_ = false;
        }
        bool has_cl = content_length_ != -1;
        if (wants_10_keep_alive_ && (is_head || has_cl || !body_allowed_for_status(status_))) {
            if (!header.Has("Connection")) set_conn = string::from_static("keep-alive");
        } else if (!req_->ProtoAtLeast(1, 1) || wants_close_) {
            close_after_reply_ = true;
        }
        if (header.Get("Connection") == "close" || !keep_alives) close_after_reply_ = true;

        // A client waiting for 100 Continue that we never read from.
        if (auto* ecr = dynamic_cast<expect_continue_reader*>(req_->Body.get()); ecr && !ecr->saw_eof()) {
            close_after_reply_ = true;
        }

        // Discard a small unread request body, so that the connection can
        // be reused and clients that write their whole request before
        // reading the response don't deadlock.
        if (req_->ContentLength != 0 && !close_after_reply_) {
            if (internal::body_reader* b = request_body()) {
                bool discard = false, too_big = false;
                if (b->closed()) {
                    if (!b->saw_eof()) close_after_reply_ = true;
                } else if (b->unread_size() >= max_post_handler_read_bytes) {
                    too_big = true;
                } else {
                    discard = true;
                }
                if (discard) {
                    gopp::error err = b->Close();
                    if (err || b->close_failed()) close_after_reply_ = true;
                }
                if (too_big) {
                    close_after_reply_ = true;
                    header.Del("Connection");
                    set_conn = string::from_static("close");
                }
            }
        }

        int code = status_;
        if (body_allowed_for_status(code)) {
            bool have_type = header.Has("Content-Type");
            bool has_ce = !header.Get("Content-Encoding").empty();
            if (!has_ce && !have_type && !has_te && !p.empty()) set_ct = DetectContentType(p);
        } else {
            if (code == 304) header.Del("Content-Type");
            header.Del("Content-Length");
            header.Del("Transfer-Encoding");
        }
        bool set_date = !header.Has("Date");

        if (has_cl && has_te && te != "identity") {
            header.Del("Content-Length");
            has_cl = false;
        }
        if (is_head || !body_allowed_for_status(code) || code == StatusNoContent) {
            header.Del("Transfer-Encoding");
        } else if (has_cl) {
            header.Del("Transfer-Encoding");
        } else if (req_->ProtoAtLeast(1, 1)) {
            if (has_te && te == "identity") {
                chunking_ = false;
                close_after_reply_ = true;
                header.Del("Transfer-Encoding");
            } else {
                // Unknown length: chunked encoding keeps the connection.
                chunking_ = true;
                set_te = string::from_static("chunked");
                if (has_te && te == "chunked") header.Del("Transfer-Encoding");
            }
        } else {
            // HTTP/1.0 without a length: the end of the body is the end of
            // the connection.
            close_after_reply_ = true;
            header.Del("Transfer-Encoding");
        }
        if (chunking_) {
            header.Del("Content-Length");
            set_cl = {};
        }

        bool del_conn = close_after_reply_ && (!keep_alives || !internal::has_token(header.Get("Connection"), "close")) &&
                        code != StatusSwitchingProtocols;
        if (del_conn) {
            header.Del("Connection");
            if (req_->ProtoAtLeast(1, 1)) set_conn = string::from_static("close");
        }

        std::string out;
        out.reserve(256);
        append_status_line(out, req_->ProtoAtLeast(1, 1), code);
        header.WriteTo(out);
        auto extra = [&](std::string_view k, const string& v) {
            if (v.empty()) return;
            out.append(k);
            out.append(v.view());
            out.append("\r\n");
        };
        extra("Content-Type: ", set_ct);
        extra("Connection: ", set_conn);
        extra("Transfer-Encoding: ", set_te);
        if (set_date) {
            out.append("Date: ");
            internal::append_date(out);
            out.append("\r\n");
        }
        extra("Content-Length: ", set_cl);
        out.append("\r\n");
        c_->bufw.WriteString(out);
    }

    conn* c_;
    Request* req_;
    http::Header handler_header_;
    bool wrote_header_ = false;
    int status_ = 0;
    int64 content_length_ = -1;  // declared, -1 if unknown
    int64 written_ = 0;
    bool close_after_reply_ = false;
    bool wants_10_keep_alive_ = false;
    bool wants_close_ = false;
    bool handler_done_ = false;

    byte buf_[buffer_before_chunking];
    std::size_t n_ = 0;

    bool cw_wrote_ = false;
    bool chunking_ = false;
};

void conn::write_error_response(const gopp::error& err) {
    int code = 400;
    std::string text = "Bad Request";
    std::string detail;
    if (err == internal::err_header_too_large) {
        code = 431;
        text = "Request Header Fields Too Large";
    } else if (auto* se = errors::As<internal::status_error>(err)) {
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
    close_write_and_wait();
}

void conn::close_write_and_wait() {
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
            c->st_.store(conn_state::closed);
            std::lock_guard l(c->state_->mu);
            c->state_->conns.erase(c);
        }
    } closer{this};

    if (auto* tc = dynamic_cast<tls::Conn*>(rwc.get())) {
        // The handshake must finish within the time for a request header.
        time::Duration limit = read_header_timeout_.ns > 0 ? read_header_timeout_ : write_timeout_;
        if (limit.ns > 0) rwc->SetDeadline(time::Now().Add(limit));
        if (gopp::error err = tc->Handshake()) {
            if (err != io::EOF_ && !errors::Is(err, net::ErrClosed)) {
                logf("http: TLS handshake error from " + std::string(RemoteAddr.view()) + ": " +
                     std::string(err.Error().view()));
            }
            return;
        }
        if (limit.ns > 0) rwc->SetDeadline(time::Time{});
        tls_state = tc->ConnectionState();
        if (tls_state->NegotiatedProtocol == "h2" && protocols_.HTTP2() && internal::http2_enabled) {
            internal::serve_http2(rwc, bufr, handler_, *this_server_options(), tls_state);
            return;
        }
    } else if (protocols_.UnencryptedHTTP2() && internal::http2_enabled) {
        // HTTP/2 with prior knowledge: the client starts with the preface.
        rwc->SetReadDeadline(read_header_timeout_.ns > 0 ? time::Now().Add(read_header_timeout_) : time::Time{});
        auto [head, perr] = bufr->Peek(internal::http2_preface.size());
        if (!perr && as_string_view(head) == internal::http2_preface) {
            rwc->SetReadDeadline(time::Time{});
            internal::serve_http2(rwc, bufr, handler_, *this_server_options(), std::nullopt);
            return;
        }
        if (!protocols_.HTTP1()) return;
    }

    for (;;) {
        if (read_header_timeout_.ns > 0) rwc->SetReadDeadline(time::Now().Add(read_header_timeout_));
        auto [req, err] = internal::read_request(bufr, max_header_bytes_);
        if (err) {
            if (err == io::EOF_ || err == io::ErrUnexpectedEOF || errors::Is(err, net::ErrClosed)) return;
            if (err->Timeout() || errors::As<net::OpError>(err)) return;  // network read errors
            write_error_response(err);
            return;
        }
        st_.store(conn_state::active, std::memory_order_relaxed);
        if (read_timeout_.ns > 0 && read_header_timeout_.ns != read_timeout_.ns) {
            rwc->SetReadDeadline(time::Now().Add(read_timeout_));
        } else if (read_header_timeout_.ns > 0 && read_timeout_.ns == 0) {
            rwc->SetReadDeadline(time::Time{});
        }
        if (write_timeout_.ns > 0) rwc->SetWriteDeadline(time::Now().Add(write_timeout_));

        req->RemoteAddr = RemoteAddr;
        req->TLS = tls_state;

        string expect = req->Header.Get("Expect");
        if (!expect.empty()) {
            if (strings::EqualFold(expect, "100-continue") && req->ProtoAtLeast(1, 1) && req->ContentLength != 0) {
                req->Body = make_ptr<expect_continue_reader>(this, req->Body);
            } else if (!strings::EqualFold(expect, "100-continue")) {
                io::WriteString(*rwc, "HTTP/1.1 417 Expectation Failed\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                return;
            }
            req->Header.Del("Expect");
        }

        response w(this, req.get());
        cur_req = req.get();
        try {
            handler_->ServeHTTP(w, *req);
        } catch (const std::exception& e) {
            logf("http: panic serving " + std::string(RemoteAddr.view()) + ": " + e.what());
            req->CancelContext();
            cur_req = nullptr;
            return;  // like Go: drop the connection
        }
        req->CancelContext();
        w.finish_request();
        cur_req = nullptr;
        if (!w.should_reuse_connection()) {
            if (w.closed_request_body_early()) close_write_and_wait();
            return;
        }
        st_.store(conn_state::idle, std::memory_order_relaxed);
        if (!keep_alives_enabled()) return;
        if (idle_timeout_.ns > 0) {
            rwc->SetReadDeadline(time::Now().Add(idle_timeout_));
        }
        if (write_timeout_.ns > 0) rwc->SetWriteDeadline(time::Time{});
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// Server

std::shared_ptr<detail::server_state> Server::state() {
    static std::mutex create_mu;
    std::lock_guard l(create_mu);
    if (!state_) state_ = std::make_shared<detail::server_state>();
    return state_;
}

gopp::error Server::ListenAndServe() {
    auto st = state();
    if (st->shutting_down.load()) return ErrServerClosed;
    string a = Addr.empty() ? string::from_static(":http") : Addr;
    auto [ln, err] = net::Listen("tcp", a);
    if (err) return err;
    return Serve(ln);
}

gopp::error Server::Serve(ptr<net::Listener> l) {
    auto st = state();
    {
        std::lock_guard lk(st->mu);
        if (st->shutting_down.load()) return ErrServerClosed;
        st->listeners.push_back(l);
    }
    time::Duration delay{};
    for (;;) {
        auto [rw, err] = l->Accept();
        if (err) {
            if (st->shutting_down.load()) return ErrServerClosed;
            if (temporary_accept_error(err)) {
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
    st->shutting_down.store(true);
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
    st->shutting_down.store(true);
    gopp::error first;
    {
        std::lock_guard lk(st->mu);
        for (auto& l : st->listeners) {
            if (auto err = l->Close(); err && !first) first = err;
        }
        st->listeners.clear();
    }
    time::Duration interval = time::Millisecond;
    bool first_round = true;
    for (;;) {
        bool quiescent = true;
        {
            std::lock_guard lk(st->mu);
            for (conn* c : st->conns) {
                if (c->h2_shutdown) {
                    // HTTP/2 connections send GOAWAY and close themselves
                    // once their streams are done.
                    if (first_round) c->h2_shutdown();
                    quiescent = false;
                    continue;
                }
                conn_state s = c->state();
                if (s == conn_state::idle || s == conn_state::fresh) c->rwc->Close();
                else quiescent = false;
            }
            first_round = false;
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
std::pair<tls::Config, gopp::error> server_tls_config(const Server& srv, std::string_view cert_file,
                                                      std::string_view key_file) {
    tls::Config cfg = srv.TLSConfig.value_or(tls::Config{});
    cfg.ctx_ = nullptr;  // compiled anew with the changes below
    if (cfg.Certificates.empty() || !cert_file.empty() || !key_file.empty()) {
        auto [cert, err] = tls::LoadX509KeyPair(cert_file, key_file);
        if (err) return {cfg, err};
        cfg.Certificates.push_back(cert);
    }
    if (cfg.NextProtos.empty()) {
        Protocols p = srv.Protocols.value_or(DefaultProtocols());
        if (p.HTTP2() && internal::http2_enabled) cfg.NextProtos.push_back(string::from_static("h2"));
        if (p.HTTP1()) cfg.NextProtos.push_back(string::from_static("http/1.1"));
    }
    return {cfg, nullptr};
}

}  // namespace

gopp::error Server::ServeTLS(ptr<net::Listener> l, std::string_view cert_file, std::string_view key_file) {
    auto [cfg, err] = server_tls_config(*this, cert_file, key_file);
    if (err) return err;
    return Serve(tls::NewListener(std::move(l), cfg));
}

gopp::error Server::ListenAndServeTLS(std::string_view cert_file, std::string_view key_file) {
    auto st = state();
    if (st->shutting_down.load()) return ErrServerClosed;
    string a = Addr.empty() ? string::from_static(":https") : Addr;
    auto [ln, err] = net::Listen("tcp", a);
    if (err) return err;
    return ServeTLS(ln, cert_file, key_file);
}

gopp::error ListenAndServeTLS(string addr, std::string_view cert_file, std::string_view key_file, ptr<Handler> handler) {
    Server srv{.Addr = std::move(addr), .Handler = std::move(handler)};
    return srv.ListenAndServeTLS(cert_file, key_file);
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
