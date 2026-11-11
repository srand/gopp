// The HTTP/2 client, after Go's net/http/internal/http2/transport.go.
//
// A client connection multiplexes requests as streams. A read loop fiber
// reads frames and hands them to the streams; a round trip writes its
// HEADERS and DATA itself, under the write lock, waiting on the
// connection's condition variable for flow control and for a free stream
// slot. Response bodies are pipes that the read loop fills.
#include "http2.hpp"

#include <gopp/runtime.hpp>
#include <gopp/strings.hpp>

#include <unordered_map>

namespace gopp::http {

const gopp::error internal::err_http2_conn_unusable = errors::New("http2: client connection is unusable");

namespace http2 {

namespace {

// Go's Transport defaults.
constexpr int32 transport_default_conn_flow = 1 << 30;
constexpr int32 transport_default_stream_flow = 4 << 20;
constexpr uint32 default_max_header_list_size = 10 << 20;
constexpr uint32 initial_max_concurrent_streams = 100;
constexpr std::size_t max_body_chunk = 16 << 10;

const gopp::error err_client_conn_closed = errors::New("http2: client conn is closed");
const gopp::error err_from_peer = errors::New("received from peer");
const gopp::error err_request_canceled = errors::New("net/http: request canceled");
const gopp::error err_timeout_headers = errors::New("http2: timeout awaiting response headers");
const gopp::error err_closed_body = errors::New("http2: response body closed");

struct wrapped_unusable : error_interface {
    gopp::error cause;
    explicit wrapped_unusable(gopp::error c) : cause(std::move(c)) {}
    string Error() const override { return cause.Error(); }
    const error_interface* Unwrap() const noexcept override { return internal::err_http2_conn_unusable.get(); }
};

// An error that marks the request as not processed by the server.
gopp::error unusable(gopp::error cause) { return make_ptr<wrapped_unusable>(std::move(cause)); }

class client_conn;

struct client_stream final : object {
    uint32 id = 0;
    outflow flow;  // what we may send
    inflow in;     // what the server may send
    ptr<pipe> body = make_ptr<pipe>();
    // The response header, or the error that ends the request first.
    chan<std::pair<ptr<Response>, gopp::error>> resp = make_chan<std::pair<ptr<Response>, gopp::error>>(1);
    bool got_headers = false;
    bool peer_done = false;  // END_STREAM or RST_STREAM received
    bool sent_end = false;   // we sent END_STREAM or RST_STREAM
    gopp::error abort_err;   // why the stream ended early
    chan<empty> done = make_chan<empty>();  // closed when the stream is forgotten
    bool head = false;
};

class client_conn final : public internal::http2_client_conn {
public:
    client_conn(ptr<net::Conn> c, std::optional<tls::ConnectionState> tls_state, const internal::http2_client_options& opts)
        : conn_(std::move(c)),
          br_(make_ptr<bufio::Reader>(conn_, 64 << 10)),
          fr_(br_),
          hdec_(initial_header_table_size, nullptr),
          henc_(&hbuf_),
          tls_(std::move(tls_state)),
          opts_(opts) {
        fr_.read_meta_headers = &hdec_;
        fr_.max_header_list_size = default_max_header_list_size;
        flow_.add(initial_window_size);
        inflow_.init(transport_default_conn_flow);
    }

    void start();

    bool can_take_new_request() override {
        std::lock_guard l(mu_);
        return can_take_locked();
    }

    std::pair<ptr<Response>, gopp::error> round_trip(Request& req, const ptr<context::Context>& ctx) override;

    bool closed() override {
        std::lock_guard l(mu_);
        return closed_ || go_away_;
    }

    bool close_if_idle() override {
        {
            std::lock_guard l(mu_);
            if (!streams_.empty() || reserved_ > 0) return false;
        }
        close();
        return true;
    }

    void close() override { conn_->Close(); }

    // Response body side.
    void body_read(const ptr<client_stream>& cs, int n);
    void body_closed(const ptr<client_stream>& cs);

private:
    bool can_take_locked() const {
        return !closed_ && !go_away_ && streams_.size() + reserved_ < max_concurrent_ && next_id_ < 0x7fffffff;
    }
    gopp::error write_frames(const std::string& frames);
    gopp::error write_headers(Request& req, client_stream& cs, bool end_stream);
    gopp::error write_body(Request& req, const ptr<client_stream>& cs);
    void forget_stream(const ptr<client_stream>& cs);
    void reset_stream(const ptr<client_stream>& cs, ErrCode code);
    void read_loop();
    gopp::error process_frame(Frame& f);
    gopp::error process_headers(Frame& f);
    gopp::error process_data(Frame& f);
    void process_settings(const Frame& f);
    void abort_stream(const ptr<client_stream>& cs, gopp::error err);
    void conn_failed(gopp::error err);

    ptr<net::Conn> conn_;
    ptr<bufio::Reader> br_;
    Framer fr_;
    hpack::Decoder hdec_;

    // Writes: the HPACK encoder's state must follow the frame order.
    sync::Mutex wmu_;
    std::string hbuf_;
    hpack::Encoder henc_;
    std::optional<tls::ConnectionState> tls_;
    internal::http2_client_options opts_;

    sync::Mutex mu_;
    sync::Cond cond_{&mu_};
    std::unordered_map<uint32, ptr<client_stream>> streams_;
    uint32 next_id_ = 1;
    std::size_t reserved_ = 0;
    uint32 max_concurrent_ = initial_max_concurrent_streams;
    uint32 peer_max_frame_size_ = initial_max_frame_size;
    int32 initial_window_ = initial_window_size;
    outflow flow_;
    inflow inflow_;
    bool closed_ = false;
    bool go_away_ = false;
    uint32 go_away_last_ = 0;
    gopp::error go_away_err_;
    gopp::error read_err_;
};

// The body of a response.
class client_body final : public io::ReadCloser {
public:
    client_body(ptr<client_conn> cc, ptr<client_stream> cs) : cc_(std::move(cc)), cs_(std::move(cs)) {}
    ~client_body() override {
        if (!closed_) Close();
    }

    std::pair<isize, gopp::error> Read(std::span<byte> p) override {
        if (closed_) return {0, ErrBodyReadAfterClose};
        auto [n, err] = cs_->body->Read(p);
        if (n > 0) cc_->body_read(cs_, static_cast<int>(n));
        return {n, err};
    }

    gopp::error Close() override {
        if (closed_) return nullptr;
        closed_ = true;
        cc_->body_closed(cs_);
        return nullptr;
    }

private:
    ptr<client_conn> cc_;
    ptr<client_stream> cs_;
    bool closed_ = false;
};

void client_conn::start() {
    std::string out(client_preface);
    Setting settings[] = {
        {SettingID::enable_push, 0},
        {SettingID::initial_window_size, uint32(transport_default_stream_flow)},
        {SettingID::max_header_list_size, default_max_header_list_size},
    };
    append_settings(out, settings);
    append_window_update(out, 0, uint32(transport_default_conn_flow - int32(initial_window_size)));
    if (gopp::error err = write_frames(out)) {
        conn_failed(err);
        return;
    }
    ptr<client_conn> self(this);
    go([self] { self->read_loop(); });
}

gopp::error client_conn::write_frames(const std::string& frames) {
    auto [n, err] = io::WriteString(*conn_, frames);
    if (err) conn_->Close();
    return err;
}

// encodeHeaders: the request's pseudo headers and fields.
gopp::error client_conn::write_headers(Request& req, client_stream& cs, bool end_stream) {
    string host = req.Host.empty() ? req.URL.Host : req.Host;
    string method = req.Method.empty() ? string::from_static("GET") : req.Method;
    // The caller holds wmu_.
    hbuf_.clear();
    auto field = [&](std::string_view k, std::string_view v) { henc_.write_field({string(k), string(v)}); };
    field(":authority", host);
    field(":method", method);
    if (method != "CONNECT") {
        field(":path", req.URL.RequestURI());
        field(":scheme", req.URL.Scheme);
    }
    bool did_ua = false;
    for (auto& e : req.Header) {
        string k = lower_header(e.key);
        if (k == "host" || k == "content-length" || k == "connection" || k == "proxy-connection" ||
            k == "transfer-encoding" || k == "upgrade" || k == "keep-alive") {
            continue;
        }
        if (k == "te" && e.value != "trailers") continue;
        if (!valid_wire_header_field_name(k) || !internal::valid_header_field_value(e.value)) continue;
        if (k == "user-agent") did_ua = true;
        henc_.write_field({k, e.value});
    }
    int64 cl = req.Body ? req.ContentLength : 0;
    if (cl > 0 || (cl == 0 && (method == "POST" || method == "PUT" || method == "PATCH"))) {
        field("content-length", strconv::Itoa(cl));
    }
    if (!did_ua) field("user-agent", "gopp-http-client/2.0");
    if (hbuf_.size() > default_max_header_list_size) return errors::New("http2: request header list larger than peer's advertised limit");
    std::string out;
    {
        std::lock_guard l2(mu_);
        append_headers(out, cs.id, hbuf_, end_stream, peer_max_frame_size_);
    }
    auto [n, err] = io::WriteString(*conn_, out);
    if (err) conn_->Close();
    return err;
}

gopp::error client_conn::write_body(Request& req, const ptr<client_stream>& cs) {
    std::vector<byte> buf(max_body_chunk);
    for (;;) {
        auto [n, rerr] = req.Body->Read(buf);
        if (rerr && rerr != io::EOF_) return rerr;
        std::size_t off = 0;
        bool eof = rerr == io::EOF_;
        // Send what was read, as flow control allows.
        while (off < static_cast<std::size_t>(n) || (eof && off == static_cast<std::size_t>(n))) {
            std::size_t k = 0;
            {
                std::lock_guard l(mu_);
                std::size_t want = static_cast<std::size_t>(n) - off;
                while (want > 0 && !cs->abort_err && !closed_ && cs->flow.available() <= 0) cond_.Wait();
                if (cs->abort_err) return cs->abort_err;
                if (closed_) return read_err_ ? read_err_ : err_client_conn_closed;
                k = std::min<std::size_t>({want, static_cast<std::size_t>(std::max(cs->flow.available(), 0)), peer_max_frame_size_});
                cs->flow.take(static_cast<int32>(k));
            }
            bool last = eof && off + k == static_cast<std::size_t>(n);
            std::string out;
            append_data(out, cs->id, last, as_string_view(std::span<const byte>(buf.data() + off, k)));
            {
                std::lock_guard l(wmu_);
                if (auto [w, err] = io::WriteString(*conn_, out); err) {
                    conn_->Close();
                    return err;
                }
            }
            off += k;
            if (last) {
                std::lock_guard l(mu_);
                cs->sent_end = true;
                if (cs->peer_done) {
                    streams_.erase(cs->id);
                    cond_.Broadcast();
                }
                return nullptr;
            }
        }
    }
}

std::pair<ptr<Response>, gopp::error> client_conn::round_trip(Request& req, const ptr<context::Context>& ctx) {
    auto cs = make_ptr<client_stream>();
    cs->head = req.Method == "HEAD";
    {
        // Wait for a stream slot, and reserve it.
        std::lock_guard l(mu_);
        for (;;) {
            if (closed_ || go_away_) return {nullptr, unusable(err_client_conn_closed)};
            if (streams_.size() + reserved_ < max_concurrent_) break;
            cond_.Wait();
        }
        reserved_++;
    }
    bool has_body = req.Body && req.ContentLength != 0;
    gopp::error werr;
    {
        // Stream ids must reach the server in order: take the id under the
        // write lock, as Go does.
        std::lock_guard w(wmu_);
        {
            std::lock_guard l(mu_);
            reserved_--;
            if (closed_ || go_away_) {
                cond_.Broadcast();
                return {nullptr, unusable(err_client_conn_closed)};
            }
            cs->id = next_id_;
            next_id_ += 2;
            cs->flow.conn = &flow_;
            cs->flow.add(initial_window_);
            cs->in.init(transport_default_stream_flow);
            // Without a body, the HEADERS end the request: mark it sent now,
            // so that a response that arrives before this fiber runs again
            // finds the stream complete and removes it.
            cs->sent_end = !has_body;
            streams_[cs->id] = cs;
        }
        werr = write_headers(req, *cs, !has_body);
    }
    if (werr) {
        forget_stream(cs);
        return {nullptr, unusable(werr)};
    }
    if (has_body) {
        // The body goes out on its own fiber, so that a server that
        // answers before reading it all isn't kept waiting.
        ptr<client_conn> self(this);
        ptr<io::ReadCloser> body = req.Body;
        Request* r = &req;
        auto body_done = make_chan<gopp::error>(1);
        go([self, cs, r, body, body_done] {
            gopp::error err = self->write_body(*r, cs);
            body->Close();
            if (err && err != cs->abort_err) self->reset_stream(cs, ErrCode::cancel);
            body_done.send(err);
        });
        // Wait for the body while waiting for the response: the request
        // must outlive the body's fiber.
        std::pair<ptr<Response>, gopp::error> res;
        gopp::error berr;
        bool got_res = false, got_body = false;
        while (!got_res || !got_body) {
            chan<empty> ctx_done = ctx ? ctx->Done() : chan<empty>();
            select(recv(cs->resp, [&](std::pair<ptr<Response>, gopp::error> r2) { res = std::move(r2); got_res = true; }),
                   recv(got_body ? chan<gopp::error>() : body_done, [&](gopp::error e) { berr = std::move(e); got_body = true; }),
                   recv(got_res ? chan<empty>() : ctx_done, [&] {
                       reset_stream(cs, ErrCode::cancel);
                       res = {nullptr, ctx->Err()};
                       got_res = true;
                   }));
            // A response that ends the stream early doesn't need the rest
            // of the body.
            if (got_res && !got_body && (res.second || cs->peer_done)) {
                std::lock_guard l(mu_);
                if (!cs->abort_err) cs->abort_err = res.second ? res.second : err_closed_body;
                cond_.Broadcast();
            }
        }
        if (res.second) return res;
        if (berr && berr != cs->abort_err && !res.first) return {nullptr, berr};
        return res;
    }

    std::pair<ptr<Response>, gopp::error> res;
    ptr<time::Timer> timer;
    chan<time::Time> timeout;
    if (opts_.response_header_timeout.ns > 0) {
        timer = time::NewTimer(opts_.response_header_timeout);
        timeout = timer->C;
    }
    chan<empty> ctx_done = ctx ? ctx->Done() : chan<empty>();
    select(recv(cs->resp, [&](std::pair<ptr<Response>, gopp::error> r2) { res = std::move(r2); }),
           recv(ctx_done, [&] {
               reset_stream(cs, ErrCode::cancel);
               res = {nullptr, ctx->Err()};
           }),
           recv(timeout, [&] {
               reset_stream(cs, ErrCode::cancel);
               res = {nullptr, err_timeout_headers};
           }));
    if (timer) timer->Stop();
    return res;
}

void client_conn::forget_stream(const ptr<client_stream>& cs) {
    std::lock_guard l(mu_);
    if (streams_.erase(cs->id)) {
        // Unread data still counts for the connection's window.
        if (int32 add = inflow_.add(static_cast<int32>(cs->body->len())); add > 0) {
            std::string out;
            append_window_update(out, 0, static_cast<uint32>(add));
            ptr<client_conn> self(this);
            go([self, out] {
                std::lock_guard l(self->wmu_);
                self->write_frames(out);
            });
        }
        cs->done.close();
    }
    cond_.Broadcast();
}

void client_conn::reset_stream(const ptr<client_stream>& cs, ErrCode code) {
    bool send = false;
    {
        std::lock_guard l(mu_);
        if (!cs->sent_end || !cs->peer_done) send = streams_.contains(cs->id) && !closed_;
        cs->sent_end = true;
        if (!cs->abort_err) cs->abort_err = stream_error(cs->id, code);
        cond_.Broadcast();
    }
    if (send) {
        std::string out;
        append_rst_stream(out, cs->id, code);
        std::lock_guard l(wmu_);
        write_frames(out);
    }
    cs->body->break_with_error(err_request_canceled);
    forget_stream(cs);
}

void client_conn::body_read(const ptr<client_stream>& cs, int n) {
    std::string out;
    {
        std::lock_guard l(mu_);
        if (int32 add = inflow_.add(n); add > 0) append_window_update(out, 0, static_cast<uint32>(add));
        if (!cs->peer_done && streams_.contains(cs->id)) {
            if (int32 add = cs->in.add(n); add > 0) append_window_update(out, cs->id, static_cast<uint32>(add));
        }
    }
    if (!out.empty()) {
        std::lock_guard l(wmu_);
        write_frames(out);
    }
}

void client_conn::body_closed(const ptr<client_stream>& cs) {
    bool done;
    {
        std::lock_guard l(mu_);
        done = cs->peer_done && cs->sent_end;
    }
    if (done) {
        forget_stream(cs);
        return;
    }
    // Closing early cancels the stream, as in Go.
    reset_stream(cs, ErrCode::cancel);
}

void client_conn::abort_stream(const ptr<client_stream>& cs, gopp::error err) {
    cs->peer_done = true;
    if (!cs->abort_err) cs->abort_err = err;
    if (!cs->got_headers) {
        cs->resp.try_send(std::pair<ptr<Response>, gopp::error>{nullptr, err});
    } else {
        cs->body->break_with_error(err);
    }
}

void client_conn::conn_failed(gopp::error err) {
    std::lock_guard l(mu_);
    closed_ = true;
    if (!read_err_) read_err_ = err;
    for (auto& [id, cs] : streams_) {
        // Streams the server never saw, after a GOAWAY, may be retried.
        gopp::error e = go_away_ && id > go_away_last_ ? unusable(go_away_err_) : err;
        if (!cs->got_headers) {
            abort_stream(cs, e);
        } else {
            cs->peer_done = true;
            cs->body->break_with_error(err == io::EOF_ ? io::ErrUnexpectedEOF : err);
        }
        cs->done.close();
    }
    streams_.clear();
    cond_.Broadcast();
}

void client_conn::read_loop() {
    ptr<client_conn> self(this);
    gopp::error err;
    for (;;) {
        auto [f, rerr] = fr_.read_frame();
        if (rerr) {
            if (auto* se = errors::As<StreamError>(rerr)) {
                // A malformed response: end the stream, keep the connection.
                ptr<client_stream> cs;
                {
                    std::lock_guard l(mu_);
                    if (auto it = streams_.find(se->stream_id); it != streams_.end()) cs = it->second;
                }
                if (cs) {
                    {
                        std::lock_guard l(mu_);
                        abort_stream(cs, rerr);
                    }
                    reset_stream(cs, se->code);
                }
                continue;
            }
            err = rerr;
            break;
        }
        if (gopp::error perr = process_frame(f)) {
            err = perr;
            if (auto* ce = errors::As<ConnectionError>(perr)) {
                std::string out;
                append_goaway(out, 0, ce->code);
                std::lock_guard l(wmu_);
                write_frames(out);
            }
            break;
        }
    }
    {
        std::lock_guard l(mu_);
        if (go_away_ && go_away_err_ && (err == io::EOF_ || errors::Is(err, net::ErrClosed))) err = go_away_err_;
    }
    conn_->Close();
    conn_failed(err ? err : err_client_conn_closed);
}

gopp::error client_conn::process_frame(Frame& f) {
    switch (f.type()) {
    case FrameType::headers: return process_headers(f);
    case FrameType::data: return process_data(f);
    case FrameType::settings:
        if (!f.is_ack()) process_settings(f);
        return nullptr;
    case FrameType::window_update: {
        std::lock_guard l(mu_);
        if (f.stream_id() == 0) {
            if (!flow_.add(static_cast<int32>(f.increment))) return connection_error(ErrCode::flow_control);
        } else if (auto it = streams_.find(f.stream_id()); it != streams_.end()) {
            if (!it->second->flow.add(static_cast<int32>(f.increment))) return connection_error(ErrCode::flow_control);
        }
        cond_.Broadcast();
        return nullptr;
    }
    case FrameType::rst_stream: {
        ptr<client_stream> cs;
        {
            std::lock_guard l(mu_);
            auto it = streams_.find(f.stream_id());
            if (it == streams_.end()) return nullptr;
            cs = it->second;
            cs->sent_end = true;
            abort_stream(cs, make_ptr<StreamError>(f.stream_id(), f.err_code, err_from_peer));
            cond_.Broadcast();
        }
        forget_stream(cs);
        return nullptr;
    }
    case FrameType::ping:
        if (!f.is_ack()) {
            std::string out;
            append_ping(out, true, f.data);
            std::lock_guard l(wmu_);
            write_frames(out);
        }
        return nullptr;
    case FrameType::goaway: {
        std::vector<ptr<client_stream>> unprocessed;
        {
            std::lock_guard l(mu_);
            go_away_ = true;
            go_away_last_ = f.last_stream_id;
            go_away_err_ = make_ptr<GoAwayError>(f.last_stream_id, f.err_code, f.data);
            for (auto& [id, cs] : streams_) {
                if (id > f.last_stream_id) unprocessed.push_back(cs);
            }
            for (auto& cs : unprocessed) {
                abort_stream(cs, unusable(go_away_err_));
                streams_.erase(cs->id);
                cs->done.close();
            }
            cond_.Broadcast();
            // An idle connection is done; others end with their streams.
            if (streams_.empty() && reserved_ == 0) conn_->Close();
        }
        return nullptr;
    }
    case FrameType::push_promise:
        return connection_error(ErrCode::protocol, "server sent PUSH_PROMISE with push disabled");
    default:
        return nullptr;
    }
}

void client_conn::process_settings(const Frame& f) {
    {
        std::lock_guard l(mu_);
        for (auto& s : f.settings) {
            switch (s.id) {
            case SettingID::max_frame_size: peer_max_frame_size_ = s.val; break;
            case SettingID::max_concurrent_streams: max_concurrent_ = s.val; break;
            case SettingID::initial_window_size: {
                int32 delta = static_cast<int32>(s.val) - initial_window_;
                for (auto& [id, cs] : streams_) cs->flow.add(delta);
                initial_window_ = static_cast<int32>(s.val);
                break;
            }
            default: break;
            }
        }
        cond_.Broadcast();
    }
    std::lock_guard l(wmu_);
    for (auto& s : f.settings) {
        if (s.id == SettingID::header_table_size) henc_.set_max_dynamic_table_size_limit(s.val);
    }
    std::string out;
    append_settings_ack(out);
    write_frames(out);
}

gopp::error client_conn::process_headers(Frame& f) {
    ptr<client_stream> cs;
    {
        std::lock_guard l(mu_);
        auto it = streams_.find(f.stream_id());
        if (it == streams_.end()) return nullptr;  // a canceled stream
        cs = it->second;
    }
    if (cs->got_headers) {
        // Trailers: must end the stream.
        if (!f.stream_ended()) {
            reset_stream(cs, ErrCode::protocol);
            return nullptr;
        }
        std::lock_guard l(mu_);
        cs->peer_done = true;
        cs->body->close_with_error(io::EOF_);
        if (cs->sent_end) {
            streams_.erase(cs->id);
            cs->done.close();
            cond_.Broadcast();
        }
        return nullptr;
    }
    string status = f.pseudo_value("status");
    auto [code, cerr] = strconv::Atoi(status);
    if (status.empty() || cerr || code < 100 || code > 999) {
        {
            std::lock_guard l(mu_);
            abort_stream(cs, errors::New("malformed response from server: missing or invalid status"));
        }
        reset_stream(cs, ErrCode::protocol);
        return nullptr;
    }
    if (code >= 100 && code <= 199) return nullptr;  // informational; the response follows

    auto resp = make_ptr<Response>();
    resp->StatusCode = static_cast<int>(code);
    resp->Status = status + " " + string(StatusText(static_cast<int>(code)));
    resp->Proto = string::from_static("HTTP/2.0");
    resp->ProtoMajor = 2;
    resp->ProtoMinor = 0;
    resp->TLS = tls_;
    for (auto& hf : f.regular_fields()) resp->Header.AddCanonical(CanonicalHeaderKey(hf.name), hf.value);
    resp->ContentLength = -1;
    if (string cl = resp->Header.Get("Content-Length"); !cl.empty()) {
        auto [n, err] = strconv::ParseInt(cl, 10, 63);
        if (!err && n >= 0) resp->ContentLength = n;
    } else if (f.stream_ended()) {
        resp->ContentLength = 0;
    }
    std::lock_guard l(mu_);
    cs->got_headers = true;
    if (f.stream_ended() || cs->head) {
        resp->Body = internal::no_body();
        if (cs->head && !resp->Header.Get("Content-Length").empty()) {
            // Keep the advertised length of a HEAD response.
        }
        if (f.stream_ended()) {
            cs->peer_done = true;
            if (cs->sent_end) {
                streams_.erase(cs->id);
                cs->done.close();
                cond_.Broadcast();
            }
        }
    } else {
        resp->Body = make_ptr<client_body>(ptr<client_conn>(this), cs);
    }
    cs->resp.try_send(std::pair<ptr<Response>, gopp::error>{resp, nullptr});
    return nullptr;
}

gopp::error client_conn::process_data(Frame& f) {
    ptr<client_stream> cs;
    std::string out;
    {
        std::lock_guard l(mu_);
        auto it = streams_.find(f.stream_id());
        if (it == streams_.end() || !it->second->got_headers || it->second->peer_done) {
            // A stream we're done with: keep the connection's window.
            if (!inflow_.take(f.header.length)) return connection_error(ErrCode::flow_control);
            if (int32 add = inflow_.add(static_cast<int32>(f.header.length)); add > 0) {
                append_window_update(out, 0, static_cast<uint32>(add));
            }
        } else {
            cs = it->second;
            if (!take_inflows(inflow_, cs->in, f.header.length)) return connection_error(ErrCode::flow_control);
            std::size_t len = f.data.size();
            if (len > 0 && cs->body->write(std::move(f.data))) {
                // The body was closed: return the connection's window.
                if (int32 add = inflow_.add(static_cast<int32>(len)); add > 0) {
                    append_window_update(out, 0, static_cast<uint32>(add));
                }
            }
            if (int32 pad = static_cast<int32>(f.header.length - len); pad > 0) {
                if (int32 add = inflow_.add(pad); add > 0) append_window_update(out, 0, static_cast<uint32>(add));
                if (int32 add = cs->in.add(pad); add > 0) append_window_update(out, cs->id, static_cast<uint32>(add));
            }
            if (f.stream_ended()) {
                cs->peer_done = true;
                cs->body->close_with_error(io::EOF_);
                if (cs->sent_end) {
                    streams_.erase(cs->id);
                    cs->done.close();
                    cond_.Broadcast();
                }
            }
        }
    }
    if (!out.empty()) {
        std::lock_guard l(wmu_);
        write_frames(out);
    }
    return nullptr;
}

}  // namespace

}  // namespace http2

ptr<internal::http2_client_conn> internal::new_http2_client_conn(ptr<net::Conn> c, std::optional<tls::ConnectionState> tls_state,
                                                                 const http2_client_options& opts) {
    auto cc = make_ptr<http2::client_conn>(std::move(c), std::move(tls_state), opts);
    cc->start();
    return cc;
}

}  // namespace gopp::http
