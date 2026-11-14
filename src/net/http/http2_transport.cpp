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

struct wrappedUnusable : errorInterface {
    gopp::error cause;
    explicit wrappedUnusable(gopp::error c) : cause(std::move(c)) {}
    string Error() const override { return cause.Error(); }
    const errorInterface* Unwrap() const noexcept override { return internal::err_http2_conn_unusable.get(); }
};

// An error that marks the request as not processed by the server.
gopp::error unusable(gopp::error cause) { return make_ptr<wrappedUnusable>(std::move(cause)); }

class client_conn;

struct client_stream final : object {
    uint32 id = 0;
    outflow flow;  // what we may send
    inflow in;     // what the server may send
    ptr<pipe> body = make_ptr<pipe>();
    // The response header, or the error that ends the request first.
    chan<std::pair<ptr<Response>, gopp::error>> resp = makeChan<std::pair<ptr<Response>, gopp::error>>(1);
    bool got_headers = false;
    bool peer_done = false;  // END_STREAM or RST_STREAM received
    bool sent_end = false;   // we sent END_STREAM or RST_STREAM
    gopp::error abort_err;   // why the stream ended early
    chan<empty> done = makeChan<empty>();  // closed when the stream is forgotten
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
    void bodyRead(const ptr<client_stream>& cs, int n);
    void bodyClosed(const ptr<client_stream>& cs);

private:
    bool can_take_locked() const {
        return !closed_ && !go_away_ && streams_.size() + reserved_ < max_concurrent_ && next_id_ < 0x7fffffff;
    }
    gopp::error writeFrames(const std::string& frames);
    gopp::error writeHeaders(Request& req, client_stream& cs, bool endStream);
    gopp::error writeBody(Request& req, const ptr<client_stream>& cs);
    void forgetStream(const ptr<client_stream>& cs);
    void resetStream(const ptr<client_stream>& cs, ErrCode code);
    void readLoop();
    gopp::error processFrame(Frame& f);
    gopp::error processHeaders(Frame& f);
    gopp::error processData(Frame& f);
    void processSettings(const Frame& f);
    void abortStream(const ptr<client_stream>& cs, gopp::error err);
    void connFailed(gopp::error err);

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
        if (n > 0) cc_->bodyRead(cs_, static_cast<int>(n));
        return {n, err};
    }

    gopp::error Close() override {
        if (closed_) return nullptr;
        closed_ = true;
        cc_->bodyClosed(cs_);
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
    appendSettings(out, settings);
    appendWindowUpdate(out, 0, uint32(transport_default_conn_flow - int32(initial_window_size)));
    if (gopp::error err = writeFrames(out)) {
        connFailed(err);
        return;
    }
    ptr<client_conn> self(this);
    go([self] { self->readLoop(); });
}

gopp::error client_conn::writeFrames(const std::string& frames) {
    auto [n, err] = io::WriteString(*conn_, frames);
    if (err) conn_->Close();
    return err;
}

// encodeHeaders: the request's pseudo headers and fields.
gopp::error client_conn::writeHeaders(Request& req, client_stream& cs, bool endStream) {
    string host = req.Host.empty() ? req.URL.Host : req.Host;
    string method = req.Method.empty() ? string::fromStatic("GET") : req.Method;
    // The caller holds wmu_.
    hbuf_.clear();
    auto field = [&](std::string_view k, std::string_view v) { henc_.writeField({string(k), string(v)}); };
    field(":authority", host);
    field(":method", method);
    if (method != "CONNECT") {
        field(":path", req.URL.RequestURI());
        field(":scheme", req.URL.Scheme);
    }
    bool did_ua = false;
    for (auto& e : req.Header) {
        string k = lowerHeader(e.key);
        if (k == "host" || k == "content-length" || k == "connection" || k == "proxy-connection" ||
            k == "transfer-encoding" || k == "upgrade" || k == "keep-alive") {
            continue;
        }
        if (k == "te" && e.value != "trailers") continue;
        if (!validWireHeaderFieldName(k) || !internal::validHeaderFieldValue(e.value)) continue;
        if (k == "user-agent") did_ua = true;
        henc_.writeField({k, e.value});
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
        appendHeaders(out, cs.id, hbuf_, endStream, peer_max_frame_size_);
    }
    auto [n, err] = io::WriteString(*conn_, out);
    if (err) conn_->Close();
    return err;
}

gopp::error client_conn::writeBody(Request& req, const ptr<client_stream>& cs) {
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
            appendData(out, cs->id, last, asStringView(std::span<const byte>(buf.data() + off, k)));
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
        werr = writeHeaders(req, *cs, !has_body);
    }
    if (werr) {
        forgetStream(cs);
        return {nullptr, unusable(werr)};
    }
    if (has_body) {
        // The body goes out on its own fiber, so that a server that
        // answers before reading it all isn't kept waiting.
        ptr<client_conn> self(this);
        ptr<io::ReadCloser> body = req.Body;
        Request* r = &req;
        auto body_done = makeChan<gopp::error>(1);
        go([self, cs, r, body, body_done] {
            gopp::error err = self->writeBody(*r, cs);
            body->Close();
            if (err && err != cs->abort_err) self->resetStream(cs, ErrCode::cancel);
            body_done.send(err);
        });
        // Wait for the body while waiting for the response: the request
        // must outlive the body's fiber.
        std::pair<ptr<Response>, gopp::error> res;
        gopp::error berr;
        bool got_res = false, got_body = false;
        while (!got_res || !got_body) {
            chan<empty> ctxDone = ctx ? ctx->Done() : chan<empty>();
            select(recv(cs->resp, [&](std::pair<ptr<Response>, gopp::error> r2) { res = std::move(r2); got_res = true; }),
                   recv(got_body ? chan<gopp::error>() : body_done, [&](gopp::error e) { berr = std::move(e); got_body = true; }),
                   recv(got_res ? chan<empty>() : ctxDone, [&] {
                       resetStream(cs, ErrCode::cancel);
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
    chan<empty> ctxDone = ctx ? ctx->Done() : chan<empty>();
    select(recv(cs->resp, [&](std::pair<ptr<Response>, gopp::error> r2) { res = std::move(r2); }),
           recv(ctxDone, [&] {
               resetStream(cs, ErrCode::cancel);
               res = {nullptr, ctx->Err()};
           }),
           recv(timeout, [&] {
               resetStream(cs, ErrCode::cancel);
               res = {nullptr, err_timeout_headers};
           }));
    if (timer) timer->Stop();
    return res;
}

void client_conn::forgetStream(const ptr<client_stream>& cs) {
    std::lock_guard l(mu_);
    if (streams_.erase(cs->id)) {
        // Unread data still counts for the connection's window.
        if (int32 add = inflow_.add(static_cast<int32>(cs->body->len())); add > 0) {
            std::string out;
            appendWindowUpdate(out, 0, static_cast<uint32>(add));
            ptr<client_conn> self(this);
            go([self, out] {
                std::lock_guard l(self->wmu_);
                self->writeFrames(out);
            });
        }
        cs->done.close();
    }
    cond_.Broadcast();
}

void client_conn::resetStream(const ptr<client_stream>& cs, ErrCode code) {
    bool send = false;
    {
        std::lock_guard l(mu_);
        if (!cs->sent_end || !cs->peer_done) send = streams_.contains(cs->id) && !closed_;
        cs->sent_end = true;
        if (!cs->abort_err) cs->abort_err = streamError(cs->id, code);
        cond_.Broadcast();
    }
    if (send) {
        std::string out;
        appendRstStream(out, cs->id, code);
        std::lock_guard l(wmu_);
        writeFrames(out);
    }
    cs->body->breakWithError(err_request_canceled);
    forgetStream(cs);
}

void client_conn::bodyRead(const ptr<client_stream>& cs, int n) {
    std::string out;
    {
        std::lock_guard l(mu_);
        if (int32 add = inflow_.add(n); add > 0) appendWindowUpdate(out, 0, static_cast<uint32>(add));
        if (!cs->peer_done && streams_.contains(cs->id)) {
            if (int32 add = cs->in.add(n); add > 0) appendWindowUpdate(out, cs->id, static_cast<uint32>(add));
        }
    }
    if (!out.empty()) {
        std::lock_guard l(wmu_);
        writeFrames(out);
    }
}

void client_conn::bodyClosed(const ptr<client_stream>& cs) {
    bool done;
    {
        std::lock_guard l(mu_);
        done = cs->peer_done && cs->sent_end;
    }
    if (done) {
        forgetStream(cs);
        return;
    }
    // Closing early cancels the stream, as in Go.
    resetStream(cs, ErrCode::cancel);
}

void client_conn::abortStream(const ptr<client_stream>& cs, gopp::error err) {
    cs->peer_done = true;
    if (!cs->abort_err) cs->abort_err = err;
    if (!cs->got_headers) {
        cs->resp.trySend(std::pair<ptr<Response>, gopp::error>{nullptr, err});
    } else {
        cs->body->breakWithError(err);
    }
}

void client_conn::connFailed(gopp::error err) {
    std::lock_guard l(mu_);
    closed_ = true;
    if (!read_err_) read_err_ = err;
    for (auto& [id, cs] : streams_) {
        // Streams the server never saw, after a GOAWAY, may be retried.
        gopp::error e = go_away_ && id > go_away_last_ ? unusable(go_away_err_) : err;
        if (!cs->got_headers) {
            abortStream(cs, e);
        } else {
            cs->peer_done = true;
            cs->body->breakWithError(err == io::EOF_ ? io::ErrUnexpectedEOF : err);
        }
        cs->done.close();
    }
    streams_.clear();
    cond_.Broadcast();
}

void client_conn::readLoop() {
    ptr<client_conn> self(this);
    gopp::error err;
    for (;;) {
        auto [f, rerr] = fr_.readFrame();
        if (rerr) {
            if (auto* se = errors::As<StreamError>(rerr)) {
                // A malformed response: end the stream, keep the connection.
                ptr<client_stream> cs;
                {
                    std::lock_guard l(mu_);
                    if (auto it = streams_.find(se->streamId); it != streams_.end()) cs = it->second;
                }
                if (cs) {
                    {
                        std::lock_guard l(mu_);
                        abortStream(cs, rerr);
                    }
                    resetStream(cs, se->code);
                }
                continue;
            }
            err = rerr;
            break;
        }
        if (gopp::error perr = processFrame(f)) {
            err = perr;
            if (auto* ce = errors::As<ConnectionError>(perr)) {
                std::string out;
                appendGoaway(out, 0, ce->code);
                std::lock_guard l(wmu_);
                writeFrames(out);
            }
            break;
        }
    }
    {
        std::lock_guard l(mu_);
        if (go_away_ && go_away_err_ && (err == io::EOF_ || errors::Is(err, net::ErrClosed))) err = go_away_err_;
    }
    conn_->Close();
    connFailed(err ? err : err_client_conn_closed);
}

gopp::error client_conn::processFrame(Frame& f) {
    switch (f.type()) {
    case FrameType::headers: return processHeaders(f);
    case FrameType::data: return processData(f);
    case FrameType::settings:
        if (!f.isAck()) processSettings(f);
        return nullptr;
    case FrameType::window_update: {
        std::lock_guard l(mu_);
        if (f.streamId() == 0) {
            if (!flow_.add(static_cast<int32>(f.increment))) return connectionError(ErrCode::flow_control);
        } else if (auto it = streams_.find(f.streamId()); it != streams_.end()) {
            if (!it->second->flow.add(static_cast<int32>(f.increment))) return connectionError(ErrCode::flow_control);
        }
        cond_.Broadcast();
        return nullptr;
    }
    case FrameType::rst_stream: {
        ptr<client_stream> cs;
        {
            std::lock_guard l(mu_);
            auto it = streams_.find(f.streamId());
            if (it == streams_.end()) return nullptr;
            cs = it->second;
            cs->sent_end = true;
            abortStream(cs, make_ptr<StreamError>(f.streamId(), f.err_code, err_from_peer));
            cond_.Broadcast();
        }
        forgetStream(cs);
        return nullptr;
    }
    case FrameType::ping:
        if (!f.isAck()) {
            std::string out;
            appendPing(out, true, f.data);
            std::lock_guard l(wmu_);
            writeFrames(out);
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
                abortStream(cs, unusable(go_away_err_));
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
        return connectionError(ErrCode::protocol, "server sent PUSH_PROMISE with push disabled");
    default:
        return nullptr;
    }
}

void client_conn::processSettings(const Frame& f) {
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
        if (s.id == SettingID::header_table_size) henc_.setMaxDynamicTableSizeLimit(s.val);
    }
    std::string out;
    appendSettingsAck(out);
    writeFrames(out);
}

gopp::error client_conn::processHeaders(Frame& f) {
    ptr<client_stream> cs;
    {
        std::lock_guard l(mu_);
        auto it = streams_.find(f.streamId());
        if (it == streams_.end()) return nullptr;  // a canceled stream
        cs = it->second;
    }
    if (cs->got_headers) {
        // Trailers: must end the stream.
        if (!f.streamEnded()) {
            resetStream(cs, ErrCode::protocol);
            return nullptr;
        }
        std::lock_guard l(mu_);
        cs->peer_done = true;
        cs->body->closeWithError(io::EOF_);
        if (cs->sent_end) {
            streams_.erase(cs->id);
            cs->done.close();
            cond_.Broadcast();
        }
        return nullptr;
    }
    string status = f.pseudoValue("status");
    auto [code, cerr] = strconv::Atoi(status);
    if (status.empty() || cerr || code < 100 || code > 999) {
        {
            std::lock_guard l(mu_);
            abortStream(cs, errors::New("malformed response from server: missing or invalid status"));
        }
        resetStream(cs, ErrCode::protocol);
        return nullptr;
    }
    if (code >= 100 && code <= 199) return nullptr;  // informational; the response follows

    auto resp = make_ptr<Response>();
    resp->StatusCode = static_cast<int>(code);
    resp->Status = status + " " + string(StatusText(static_cast<int>(code)));
    resp->Proto = string::fromStatic("HTTP/2.0");
    resp->ProtoMajor = 2;
    resp->ProtoMinor = 0;
    resp->TLS = tls_;
    for (auto& hf : f.regularFields()) resp->Header.AddCanonical(CanonicalHeaderKey(hf.name), hf.value);
    resp->ContentLength = -1;
    if (string cl = resp->Header.Get("Content-Length"); !cl.empty()) {
        auto [n, err] = strconv::ParseInt(cl, 10, 63);
        if (!err && n >= 0) resp->ContentLength = n;
    } else if (f.streamEnded()) {
        resp->ContentLength = 0;
    }
    std::lock_guard l(mu_);
    cs->got_headers = true;
    if (f.streamEnded() || cs->head) {
        resp->Body = internal::noBody();
        if (cs->head && !resp->Header.Get("Content-Length").empty()) {
            // Keep the advertised length of a HEAD response.
        }
        if (f.streamEnded()) {
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
    cs->resp.trySend(std::pair<ptr<Response>, gopp::error>{resp, nullptr});
    return nullptr;
}

gopp::error client_conn::processData(Frame& f) {
    ptr<client_stream> cs;
    std::string out;
    {
        std::lock_guard l(mu_);
        auto it = streams_.find(f.streamId());
        if (it == streams_.end() || !it->second->got_headers || it->second->peer_done) {
            // A stream we're done with: keep the connection's window.
            if (!inflow_.take(f.header.length)) return connectionError(ErrCode::flow_control);
            if (int32 add = inflow_.add(static_cast<int32>(f.header.length)); add > 0) {
                appendWindowUpdate(out, 0, static_cast<uint32>(add));
            }
        } else {
            cs = it->second;
            if (!takeInflows(inflow_, cs->in, f.header.length)) return connectionError(ErrCode::flow_control);
            std::size_t len = f.data.size();
            if (len > 0 && cs->body->write(std::move(f.data))) {
                // The body was closed: return the connection's window.
                if (int32 add = inflow_.add(static_cast<int32>(len)); add > 0) {
                    appendWindowUpdate(out, 0, static_cast<uint32>(add));
                }
            }
            if (int32 pad = static_cast<int32>(f.header.length - len); pad > 0) {
                if (int32 add = inflow_.add(pad); add > 0) appendWindowUpdate(out, 0, static_cast<uint32>(add));
                if (int32 add = cs->in.add(pad); add > 0) appendWindowUpdate(out, cs->id, static_cast<uint32>(add));
            }
            if (f.streamEnded()) {
                cs->peer_done = true;
                cs->body->closeWithError(io::EOF_);
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
        writeFrames(out);
    }
    return nullptr;
}

}  // namespace

}  // namespace http2

ptr<internal::http2_client_conn> internal::newHttp2ClientConn(ptr<net::Conn> c, std::optional<tls::ConnectionState> tls_state,
                                                                 const http2_client_options& opts) {
    auto cc = make_ptr<http2::client_conn>(std::move(c), std::move(tls_state), opts);
    cc->start();
    return cc;
}

}  // namespace gopp::http
