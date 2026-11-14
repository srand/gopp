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

const gopp::error internal::errHttp2ConnUnusable = errors::New("http2: client connection is unusable");

namespace http2 {

namespace {

// Go's Transport defaults.
constexpr int32 transportDefaultConnFlow = 1 << 30;
constexpr int32 transportDefaultStreamFlow = 4 << 20;
constexpr uint32 defaultMaxHeaderListSize = 10 << 20;
constexpr uint32 initialMaxConcurrentStreams = 100;
constexpr std::size_t maxBodyChunk = 16 << 10;

const gopp::error errClientConnClosed = errors::New("http2: client conn is closed");
const gopp::error errFromPeer = errors::New("received from peer");
const gopp::error errRequestCanceled = errors::New("net/http: request canceled");
const gopp::error errTimeoutHeaders = errors::New("http2: timeout awaiting response headers");
const gopp::error errClosedBody = errors::New("http2: response body closed");

struct wrappedUnusable : errorInterface {
    gopp::error cause;
    explicit wrappedUnusable(gopp::error c) : cause(std::move(c)) {}
    string Error() const override { return cause.Error(); }
    const errorInterface* Unwrap() const noexcept override { return internal::errHttp2ConnUnusable.get(); }
};

// An error that marks the request as not processed by the server.
gopp::error unusable(gopp::error cause) { return make_ptr<wrappedUnusable>(std::move(cause)); }

class clientConn;

struct clientStream final : object {
    uint32 id = 0;
    outflow flow;  // what we may send
    inflow in;     // what the server may send
    ptr<pipe> body = make_ptr<pipe>();
    // The response header, or the error that ends the request first.
    chan<std::pair<ptr<Response>, gopp::error>> resp = make_chan<std::pair<ptr<Response>, gopp::error>>(1);
    bool gotHeaders = false;
    bool peerDone = false;  // END_STREAM or RST_STREAM received
    bool sentEnd = false;   // we sent END_STREAM or RST_STREAM
    gopp::error abortErr;   // why the stream ended early
    chan<empty> done = make_chan<empty>();  // closed when the stream is forgotten
    bool head = false;
};

class clientConn final : public internal::http2ClientConn {
public:
    clientConn(ptr<net::Conn> c, std::optional<tls::ConnectionState> tlsState, const internal::http2ClientOptions& opts)
        : conn_(std::move(c)),
          br_(make_ptr<bufio::Reader>(conn_, 64 << 10)),
          fr_(br_),
          hdec_(initialHeaderTableSize, nullptr),
          henc_(&hbuf_),
          tls_(std::move(tlsState)),
          opts_(opts) {
        fr_.readMetaHeaders = &hdec_;
        fr_.maxHeaderListSize = defaultMaxHeaderListSize;
        flow_.add(initialWindowSize);
        inflow_.init(transportDefaultConnFlow);
    }

    void start();

    bool canTakeNewRequest() override {
        std::lock_guard l(mu_);
        return canTakeLocked();
    }

    std::pair<ptr<Response>, gopp::error> roundTrip(Request& req, const ptr<context::Context>& ctx) override;

    bool closed() override {
        std::lock_guard l(mu_);
        return closed_ || goAway_;
    }

    bool closeIfIdle() override {
        {
            std::lock_guard l(mu_);
            if (!streams_.empty() || reserved_ > 0) return false;
        }
        close();
        return true;
    }

    void close() override { conn_->Close(); }

    // Response body side.
    void bodyRead(const ptr<clientStream>& cs, int n);
    void bodyClosed(const ptr<clientStream>& cs);

private:
    bool canTakeLocked() const {
        return !closed_ && !goAway_ && streams_.size() + reserved_ < maxConcurrent_ && nextId_ < 0x7fffffff;
    }
    gopp::error writeFrames(const std::string& frames);
    gopp::error writeHeaders(Request& req, clientStream& cs, bool endStream);
    gopp::error writeBody(Request& req, const ptr<clientStream>& cs);
    void forgetStream(const ptr<clientStream>& cs);
    void resetStream(const ptr<clientStream>& cs, ErrCode code);
    void readLoop();
    gopp::error processFrame(Frame& f);
    gopp::error processHeaders(Frame& f);
    gopp::error processData(Frame& f);
    void processSettings(const Frame& f);
    void abortStream(const ptr<clientStream>& cs, gopp::error err);
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
    internal::http2ClientOptions opts_;

    sync::Mutex mu_;
    sync::Cond cond_{&mu_};
    std::unordered_map<uint32, ptr<clientStream>> streams_;
    uint32 nextId_ = 1;
    std::size_t reserved_ = 0;
    uint32 maxConcurrent_ = initialMaxConcurrentStreams;
    uint32 peerMaxFrameSize_ = initialMaxFrameSize;
    int32 initialWindow_ = initialWindowSize;
    outflow flow_;
    inflow inflow_;
    bool closed_ = false;
    bool goAway_ = false;
    uint32 goAwayLast_ = 0;
    gopp::error goAwayErr_;
    gopp::error readErr_;
};

// The body of a response.
class clientBody final : public io::ReadCloser {
public:
    clientBody(ptr<clientConn> cc, ptr<clientStream> cs) : cc_(std::move(cc)), cs_(std::move(cs)) {}
    ~clientBody() override {
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
    ptr<clientConn> cc_;
    ptr<clientStream> cs_;
    bool closed_ = false;
};

void clientConn::start() {
    std::string out(clientPreface);
    Setting settings[] = {
        {SettingID::enablePush, 0},
        {SettingID::initialWindowSize, uint32(transportDefaultStreamFlow)},
        {SettingID::maxHeaderListSize, defaultMaxHeaderListSize},
    };
    appendSettings(out, settings);
    appendWindowUpdate(out, 0, uint32(transportDefaultConnFlow - int32(initialWindowSize)));
    if (gopp::error err = writeFrames(out)) {
        connFailed(err);
        return;
    }
    ptr<clientConn> self(this);
    go([self] { self->readLoop(); });
}

gopp::error clientConn::writeFrames(const std::string& frames) {
    auto [n, err] = io::WriteString(*conn_, frames);
    if (err) conn_->Close();
    return err;
}

// encodeHeaders: the request's pseudo headers and fields.
gopp::error clientConn::writeHeaders(Request& req, clientStream& cs, bool endStream) {
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
    bool didUa = false;
    for (auto& e : req.Header) {
        string k = lowerHeader(e.key);
        if (k == "host" || k == "content-length" || k == "connection" || k == "proxy-connection" ||
            k == "transfer-encoding" || k == "upgrade" || k == "keep-alive") {
            continue;
        }
        if (k == "te" && e.value != "trailers") continue;
        if (!validWireHeaderFieldName(k) || !internal::validHeaderFieldValue(e.value)) continue;
        if (k == "user-agent") didUa = true;
        henc_.writeField({k, e.value});
    }
    int64 cl = req.Body ? req.ContentLength : 0;
    if (cl > 0 || (cl == 0 && (method == "POST" || method == "PUT" || method == "PATCH"))) {
        field("content-length", strconv::Itoa(cl));
    }
    if (!didUa) field("user-agent", "gopp-http-client/2.0");
    if (hbuf_.size() > defaultMaxHeaderListSize) return errors::New("http2: request header list larger than peer's advertised limit");
    std::string out;
    {
        std::lock_guard l2(mu_);
        appendHeaders(out, cs.id, hbuf_, endStream, peerMaxFrameSize_);
    }
    auto [n, err] = io::WriteString(*conn_, out);
    if (err) conn_->Close();
    return err;
}

gopp::error clientConn::writeBody(Request& req, const ptr<clientStream>& cs) {
    std::vector<byte> buf(maxBodyChunk);
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
                while (want > 0 && !cs->abortErr && !closed_ && cs->flow.available() <= 0) cond_.Wait();
                if (cs->abortErr) return cs->abortErr;
                if (closed_) return readErr_ ? readErr_ : errClientConnClosed;
                k = std::min<std::size_t>({want, static_cast<std::size_t>(std::max(cs->flow.available(), 0)), peerMaxFrameSize_});
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
                cs->sentEnd = true;
                if (cs->peerDone) {
                    streams_.erase(cs->id);
                    cond_.Broadcast();
                }
                return nullptr;
            }
        }
    }
}

std::pair<ptr<Response>, gopp::error> clientConn::roundTrip(Request& req, const ptr<context::Context>& ctx) {
    auto cs = make_ptr<clientStream>();
    cs->head = req.Method == "HEAD";
    {
        // Wait for a stream slot, and reserve it.
        std::lock_guard l(mu_);
        for (;;) {
            if (closed_ || goAway_) return {nullptr, unusable(errClientConnClosed)};
            if (streams_.size() + reserved_ < maxConcurrent_) break;
            cond_.Wait();
        }
        reserved_++;
    }
    bool hasBody = req.Body && req.ContentLength != 0;
    gopp::error werr;
    {
        // Stream ids must reach the server in order: take the id under the
        // write lock, as Go does.
        std::lock_guard w(wmu_);
        {
            std::lock_guard l(mu_);
            reserved_--;
            if (closed_ || goAway_) {
                cond_.Broadcast();
                return {nullptr, unusable(errClientConnClosed)};
            }
            cs->id = nextId_;
            nextId_ += 2;
            cs->flow.conn = &flow_;
            cs->flow.add(initialWindow_);
            cs->in.init(transportDefaultStreamFlow);
            // Without a body, the HEADERS end the request: mark it sent now,
            // so that a response that arrives before this fiber runs again
            // finds the stream complete and removes it.
            cs->sentEnd = !hasBody;
            streams_[cs->id] = cs;
        }
        werr = writeHeaders(req, *cs, !hasBody);
    }
    if (werr) {
        forgetStream(cs);
        return {nullptr, unusable(werr)};
    }
    if (hasBody) {
        // The body goes out on its own fiber, so that a server that
        // answers before reading it all isn't kept waiting.
        ptr<clientConn> self(this);
        ptr<io::ReadCloser> body = req.Body;
        Request* r = &req;
        auto bodyDone = make_chan<gopp::error>(1);
        go([self, cs, r, body, bodyDone] {
            gopp::error err = self->writeBody(*r, cs);
            body->Close();
            if (err && err != cs->abortErr) self->resetStream(cs, ErrCode::cancel);
            bodyDone.send(err);
        });
        // Wait for the body while waiting for the response: the request
        // must outlive the body's fiber.
        std::pair<ptr<Response>, gopp::error> res;
        gopp::error berr;
        bool gotRes = false, gotBody = false;
        while (!gotRes || !gotBody) {
            chan<empty> ctxDone = ctx ? ctx->Done() : chan<empty>();
            select(recv(cs->resp, [&](std::pair<ptr<Response>, gopp::error> r2) { res = std::move(r2); gotRes = true; }),
                   recv(gotBody ? chan<gopp::error>() : bodyDone, [&](gopp::error e) { berr = std::move(e); gotBody = true; }),
                   recv(gotRes ? chan<empty>() : ctxDone, [&] {
                       resetStream(cs, ErrCode::cancel);
                       res = {nullptr, ctx->Err()};
                       gotRes = true;
                   }));
            // A response that ends the stream early doesn't need the rest
            // of the body.
            if (gotRes && !gotBody && (res.second || cs->peerDone)) {
                std::lock_guard l(mu_);
                if (!cs->abortErr) cs->abortErr = res.second ? res.second : errClosedBody;
                cond_.Broadcast();
            }
        }
        if (res.second) return res;
        if (berr && berr != cs->abortErr && !res.first) return {nullptr, berr};
        return res;
    }

    std::pair<ptr<Response>, gopp::error> res;
    ptr<time::Timer> timer;
    chan<time::Time> timeout;
    if (opts_.responseHeaderTimeout.ns > 0) {
        timer = time::NewTimer(opts_.responseHeaderTimeout);
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
               res = {nullptr, errTimeoutHeaders};
           }));
    if (timer) timer->Stop();
    return res;
}

void clientConn::forgetStream(const ptr<clientStream>& cs) {
    std::lock_guard l(mu_);
    if (streams_.erase(cs->id)) {
        // Unread data still counts for the connection's window.
        if (int32 add = inflow_.add(static_cast<int32>(cs->body->len())); add > 0) {
            std::string out;
            appendWindowUpdate(out, 0, static_cast<uint32>(add));
            ptr<clientConn> self(this);
            go([self, out] {
                std::lock_guard l(self->wmu_);
                self->writeFrames(out);
            });
        }
        cs->done.close();
    }
    cond_.Broadcast();
}

void clientConn::resetStream(const ptr<clientStream>& cs, ErrCode code) {
    bool send = false;
    {
        std::lock_guard l(mu_);
        if (!cs->sentEnd || !cs->peerDone) send = streams_.contains(cs->id) && !closed_;
        cs->sentEnd = true;
        if (!cs->abortErr) cs->abortErr = streamError(cs->id, code);
        cond_.Broadcast();
    }
    if (send) {
        std::string out;
        appendRstStream(out, cs->id, code);
        std::lock_guard l(wmu_);
        writeFrames(out);
    }
    cs->body->breakWithError(errRequestCanceled);
    forgetStream(cs);
}

void clientConn::bodyRead(const ptr<clientStream>& cs, int n) {
    std::string out;
    {
        std::lock_guard l(mu_);
        if (int32 add = inflow_.add(n); add > 0) appendWindowUpdate(out, 0, static_cast<uint32>(add));
        if (!cs->peerDone && streams_.contains(cs->id)) {
            if (int32 add = cs->in.add(n); add > 0) appendWindowUpdate(out, cs->id, static_cast<uint32>(add));
        }
    }
    if (!out.empty()) {
        std::lock_guard l(wmu_);
        writeFrames(out);
    }
}

void clientConn::bodyClosed(const ptr<clientStream>& cs) {
    bool done;
    {
        std::lock_guard l(mu_);
        done = cs->peerDone && cs->sentEnd;
    }
    if (done) {
        forgetStream(cs);
        return;
    }
    // Closing early cancels the stream, as in Go.
    resetStream(cs, ErrCode::cancel);
}

void clientConn::abortStream(const ptr<clientStream>& cs, gopp::error err) {
    cs->peerDone = true;
    if (!cs->abortErr) cs->abortErr = err;
    if (!cs->gotHeaders) {
        cs->resp.trySend(std::pair<ptr<Response>, gopp::error>{nullptr, err});
    } else {
        cs->body->breakWithError(err);
    }
}

void clientConn::connFailed(gopp::error err) {
    std::lock_guard l(mu_);
    closed_ = true;
    if (!readErr_) readErr_ = err;
    for (auto& [id, cs] : streams_) {
        // Streams the server never saw, after a GOAWAY, may be retried.
        gopp::error e = goAway_ && id > goAwayLast_ ? unusable(goAwayErr_) : err;
        if (!cs->gotHeaders) {
            abortStream(cs, e);
        } else {
            cs->peerDone = true;
            cs->body->breakWithError(err == io::EOF_ ? io::ErrUnexpectedEOF : err);
        }
        cs->done.close();
    }
    streams_.clear();
    cond_.Broadcast();
}

void clientConn::readLoop() {
    ptr<clientConn> self(this);
    gopp::error err;
    for (;;) {
        auto [f, rerr] = fr_.readFrame();
        if (rerr) {
            if (auto* se = errors::As<StreamError>(rerr)) {
                // A malformed response: end the stream, keep the connection.
                ptr<clientStream> cs;
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
        if (goAway_ && goAwayErr_ && (err == io::EOF_ || errors::Is(err, net::ErrClosed))) err = goAwayErr_;
    }
    conn_->Close();
    connFailed(err ? err : errClientConnClosed);
}

gopp::error clientConn::processFrame(Frame& f) {
    switch (f.type()) {
    case FrameType::headers: return processHeaders(f);
    case FrameType::data: return processData(f);
    case FrameType::settings:
        if (!f.isAck()) processSettings(f);
        return nullptr;
    case FrameType::windowUpdate: {
        std::lock_guard l(mu_);
        if (f.streamId() == 0) {
            if (!flow_.add(static_cast<int32>(f.increment))) return connectionError(ErrCode::flowControl);
        } else if (auto it = streams_.find(f.streamId()); it != streams_.end()) {
            if (!it->second->flow.add(static_cast<int32>(f.increment))) return connectionError(ErrCode::flowControl);
        }
        cond_.Broadcast();
        return nullptr;
    }
    case FrameType::rstStream: {
        ptr<clientStream> cs;
        {
            std::lock_guard l(mu_);
            auto it = streams_.find(f.streamId());
            if (it == streams_.end()) return nullptr;
            cs = it->second;
            cs->sentEnd = true;
            abortStream(cs, make_ptr<StreamError>(f.streamId(), f.errCode, errFromPeer));
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
        std::vector<ptr<clientStream>> unprocessed;
        {
            std::lock_guard l(mu_);
            goAway_ = true;
            goAwayLast_ = f.lastStreamId;
            goAwayErr_ = make_ptr<GoAwayError>(f.lastStreamId, f.errCode, f.data);
            for (auto& [id, cs] : streams_) {
                if (id > f.lastStreamId) unprocessed.push_back(cs);
            }
            for (auto& cs : unprocessed) {
                abortStream(cs, unusable(goAwayErr_));
                streams_.erase(cs->id);
                cs->done.close();
            }
            cond_.Broadcast();
            // An idle connection is done; others end with their streams.
            if (streams_.empty() && reserved_ == 0) conn_->Close();
        }
        return nullptr;
    }
    case FrameType::pushPromise:
        return connectionError(ErrCode::protocol, "server sent PUSH_PROMISE with push disabled");
    default:
        return nullptr;
    }
}

void clientConn::processSettings(const Frame& f) {
    {
        std::lock_guard l(mu_);
        for (auto& s : f.settings) {
            switch (s.id) {
            case SettingID::maxFrameSize: peerMaxFrameSize_ = s.val; break;
            case SettingID::maxConcurrentStreams: maxConcurrent_ = s.val; break;
            case SettingID::initialWindowSize: {
                int32 delta = static_cast<int32>(s.val) - initialWindow_;
                for (auto& [id, cs] : streams_) cs->flow.add(delta);
                initialWindow_ = static_cast<int32>(s.val);
                break;
            }
            default: break;
            }
        }
        cond_.Broadcast();
    }
    std::lock_guard l(wmu_);
    for (auto& s : f.settings) {
        if (s.id == SettingID::headerTableSize) henc_.setMaxDynamicTableSizeLimit(s.val);
    }
    std::string out;
    appendSettingsAck(out);
    writeFrames(out);
}

gopp::error clientConn::processHeaders(Frame& f) {
    ptr<clientStream> cs;
    {
        std::lock_guard l(mu_);
        auto it = streams_.find(f.streamId());
        if (it == streams_.end()) return nullptr;  // a canceled stream
        cs = it->second;
    }
    if (cs->gotHeaders) {
        // Trailers: must end the stream.
        if (!f.streamEnded()) {
            resetStream(cs, ErrCode::protocol);
            return nullptr;
        }
        std::lock_guard l(mu_);
        cs->peerDone = true;
        cs->body->closeWithError(io::EOF_);
        if (cs->sentEnd) {
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
    cs->gotHeaders = true;
    if (f.streamEnded() || cs->head) {
        resp->Body = internal::noBody();
        if (cs->head && !resp->Header.Get("Content-Length").empty()) {
            // Keep the advertised length of a HEAD response.
        }
        if (f.streamEnded()) {
            cs->peerDone = true;
            if (cs->sentEnd) {
                streams_.erase(cs->id);
                cs->done.close();
                cond_.Broadcast();
            }
        }
    } else {
        resp->Body = make_ptr<clientBody>(ptr<clientConn>(this), cs);
    }
    cs->resp.trySend(std::pair<ptr<Response>, gopp::error>{resp, nullptr});
    return nullptr;
}

gopp::error clientConn::processData(Frame& f) {
    ptr<clientStream> cs;
    std::string out;
    {
        std::lock_guard l(mu_);
        auto it = streams_.find(f.streamId());
        if (it == streams_.end() || !it->second->gotHeaders || it->second->peerDone) {
            // A stream we're done with: keep the connection's window.
            if (!inflow_.take(f.header.length)) return connectionError(ErrCode::flowControl);
            if (int32 add = inflow_.add(static_cast<int32>(f.header.length)); add > 0) {
                appendWindowUpdate(out, 0, static_cast<uint32>(add));
            }
        } else {
            cs = it->second;
            if (!takeInflows(inflow_, cs->in, f.header.length)) return connectionError(ErrCode::flowControl);
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
                cs->peerDone = true;
                cs->body->closeWithError(io::EOF_);
                if (cs->sentEnd) {
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

ptr<internal::http2ClientConn> internal::newHttp2ClientConn(ptr<net::Conn> c, std::optional<tls::ConnectionState> tlsState,
                                                                 const http2ClientOptions& opts) {
    auto cc = make_ptr<http2::clientConn>(std::move(c), std::move(tlsState), opts);
    cc->start();
    return cc;
}

}  // namespace gopp::http
