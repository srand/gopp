// The HTTP/2 server, after Go's net/http/internal/http2/server.Go.
//
// As in Go, one fiber owns the connection's state (the serve loop). A
// reader fiber reads frames and hands them to it; a writer fiber writes
// what it batches up; handlers run on their own fibers and ask it to
// write their responses over channels. Writes are batched: while the
// writer is busy, frames queue up, and the next write sends them all.
#include "ascii.hpp"
#include "http2.hpp"

#include <gopp/runtime.hpp>
#include <gopp/strings.hpp>

#include <cstring>
#include <exception>
#include <map>
#include <optional>
#include <unordered_map>

namespace gopp::http {

namespace http2 {

namespace {

// Defaults of Go's http2.Server.
constexpr uint32 defaultMaxReadFrameSize = 1 << 20;
constexpr uint32 defaultMaxStreams = 250;
constexpr int32 initialStreamRecvWindow = 1 << 20;  // MaxReceiveBufferPerStream
constexpr int32 initialConnRecvWindow = 1 << 20;    // MaxReceiveBufferPerConnection
constexpr std::size_t handlerChunkWriteSize = 4 << 10;
constexpr std::size_t maxWriteBatch = 64 << 10;
constexpr uint32 maxDataFrameSize = 16 << 10;
const time::Duration prefaceTimeout = 10 * time::Second;
const time::Duration firstSettingsTimeout = 2 * time::Second;
const time::Duration goAwayTimeout = time::Second;

const gopp::error errClientDisconnected = errors::New("client disconnected");
const gopp::error errClosedBody = errors::New("body closed by handler");
const gopp::error errHandlerComplete = errors::New("http2: request body closed due to handler exiting");
const gopp::error errStreamClosed = errors::New("http2: stream closed");
const gopp::error errTooMuchData = errors::New("http2: handler wrote more than declared Content-Length");

enum class streamState { idle, open, halfClosedLocal, halfClosedRemote, closed };

class serverConn;

struct stream final : object {
    uint32 id = 0;
    streamState state = streamState::idle;
    ptr<pipe> body;  // the request body; nil if there is none
    ptr<Request> req;
    int64 declBodyBytes = -1;  // the declared Content-Length; -1 if none
    int64 bodyBytes = 0;        // received so far
    outflow flow;                // what we may send
    inflow in;                   // what the peer may send
    bool gotTrailerHeader = false;
    bool resetQueued = false;   // a RST_STREAM is on its way
    chan<empty> cw;              // closed when the stream closes
    gopp::error closeErr;       // why it closed; set before cw closes
    ptr<time::Timer> readDeadline;
    ptr<time::Timer> writeDeadline;
};

// A write a handler asks for (FrameWriteRequest).
struct writeRequest {
    enum kindT { headers, data, rst };
    kindT kind = data;
    ptr<stream> st;
    // headers
    int status = 0;  // 0 for trailers
    Header header;
    string contentType, ContentLength, date;
    // data
    string bytes;
    std::size_t off = 0;
    bool endStream = false;
    // rst
    ErrCode code = ErrCode::internal;
    // Receives the result once the frame is written; nil if no one waits.
    chan<gopp::error> done;
};

struct readFrameResult {
    Frame f;
    gopp::error err;
};

struct bodyReadMsg {
    ptr<stream> st;
    int n;
};

// What to do once a batch is written (wroteFrame).
struct writeEffect {
    ptr<stream> st;
    bool endsStream = false;  // the frame had END_STREAM
    bool rst = false;          // a RST_STREAM: the stream is closed
    gopp::error rstErr;
    chan<gopp::error> done;
};

class serverConn final : public object {
public:
    serverConn(ptr<net::Conn> c, ptr<bufio::Reader> br, ptr<Handler> h, const internal::serverOptions& opts,
                std::optional<tls::ConnectionState> tlsState)
        : conn_(std::move(c)),
          br_(std::move(br)),
          handler_(std::move(h)),
          opts_(opts),
          tlsState_(std::move(tlsState)),
          remoteAddr_(conn_->RemoteAddr() ? conn_->RemoteAddr()->String() : string()),
          framer_(br_),
          hdec_(initialHeaderTableSize, nullptr),
          henc_(&hbuf_) {
        maxHeaderListSize_ = static_cast<uint32>(std::min<int64>(
            (opts_.MaxHeaderBytes > 0 ? opts_.MaxHeaderBytes : DefaultMaxHeaderBytes) + 10 * 32, 0xffffffffLL));
        framer_.readMetaHeaders = &hdec_;
        framer_.maxHeaderListSize = maxHeaderListSize_;
        framer_.setMaxReadFrameSize(defaultMaxReadFrameSize);
        flow_.add(initialWindowSize);
        inflow_.init(initialWindowSize);
        doneServing_ = make_chan<empty>();
        wantWriteCh_ = make_chan<writeRequest>(8);
        bodyReadCh_ = make_chan<bodyReadMsg>(8);
        serveMsgCh_ = make_chan<std::function<void()>>(8);
        readFrameCh_ = make_chan<readFrameResult>();
        writeCh_ = make_chan<std::string>(1);
        wroteCh_ = make_chan<gopp::error>(1);
    }

    void serve();

    // Handler side (any fiber).
    gopp::error writeFromHandler(writeRequest wr);
    gopp::error writeAndWait(writeRequest wr);
    void noteBodyReadFromHandler(const ptr<stream>& st, int n);
    void sendServeMsg(std::function<void()> fn);
    const chan<empty>& doneServing() const noexcept { return doneServing_; }

private:
    // Serve loop.
    bool processFrameFromReader(readFrameResult res);
    gopp::error processFrame(Frame& f);
    gopp::error processSettings(const Frame& f);
    gopp::error processSetting(const Setting& s);
    gopp::error processData(Frame& f);
    gopp::error processHeaders(Frame& f);
    gopp::error processTrailerHeaders(stream& st, const Frame& f);
    gopp::error processWindowUpdate(const Frame& f);
    gopp::error processResetStream(const Frame& f);
    gopp::error processGoaway(const Frame& f);
    std::pair<streamState, ptr<stream>> state(uint32 id);
    ptr<stream> newStream(uint32 id, streamState st);
    gopp::error NewRequest(stream& st, const Frame& f, ptr<Request>& out);
    void scheduleHandler(ptr<stream> st, ptr<Request> req, int statusOverride, string msg);
    void closeStream(const ptr<stream>& st, gopp::error err);
    void releaseStream(stream& st);
    void endStream(stream& st);
    void resetStream(uint32 id, ErrCode code, gopp::error cause = nullptr);
    void goAway(ErrCode code);
    void sendWindowUpdate(stream* st, int32 n);
    void noteBodyRead(const ptr<stream>& st, int n);
    void queueWrite(writeRequest wr);
    void scheduleFrameWrite();
    void encodeHeaders(const writeRequest& wr);
    void wroteBatch(gopp::error err);
    void setActive(bool active);
    void resetIdleTimer();

    ptr<net::Conn> conn_;
    ptr<bufio::Reader> br_;
    ptr<Handler> handler_;
    internal::serverOptions opts_;
    std::optional<tls::ConnectionState> tlsState_;
    string remoteAddr_;
    Framer framer_;
    hpack::Decoder hdec_;
    std::string hbuf_;  // the HPACK encoder's output
    hpack::Encoder henc_;

    chan<empty> doneServing_;
    chan<writeRequest> wantWriteCh_;
    chan<bodyReadMsg> bodyReadCh_;
    chan<std::function<void()>> serveMsgCh_;
    chan<readFrameResult> readFrameCh_;
    chan<std::string> writeCh_;
    chan<gopp::error> wroteCh_;

    // State owned by the serve loop.
    std::unordered_map<uint32, ptr<stream>> streams_;
    uint32 maxClientStreamId_ = 0;
    uint32 advMaxStreams_ = defaultMaxStreams;
    uint32 curClientStreams_ = 0;
    int unackedSettings_ = 0;
    bool sawFirstSettings_ = false;
    bool needToSendSettingsAck_ = false;
    outflow flow_;  // the connection's send window
    inflow inflow_;  // the connection's receive window
    int32 initialStreamSendWindow_ = initialWindowSize;
    uint32 peerMaxFrameSize_ = initialMaxFrameSize;
    uint32 maxHeaderListSize_;
    bool inGoAway_ = false;
    bool needToSendGoAway_ = false;
    ErrCode goAwayCode_ = ErrCode::no;
    ptr<time::Timer> shutdownTimer_;
    ptr<time::Timer> idleTimer_;
    bool active_ = false;
    bool exit_ = false;

    // The write scheduler: control frames first, then the streams' queues
    // in turn (Go's round-robin scheduler).
    std::string control_;
    std::vector<writeEffect> controlEffects_;
    std::map<uint32, std::deque<writeRequest>> queues_;
    uint32 nextQueue_ = 0;  // round-robin position
    bool writing_ = false;
    std::vector<writeEffect> inFlight_;
    std::string batch_;
};

// The request body as the handler reads it (requestBody).
class requestBody final : public io::ReadCloser {
public:
    requestBody(ptr<serverConn> sc, ptr<stream> st, ptr<pipe> p, bool needsContinue)
        : sc_(std::move(sc)), st_(std::move(st)), pipe_(std::move(p)), needsContinue_(needsContinue) {}

    std::pair<isize, gopp::error> Read(std::span<byte> p) override {
        if (needsContinue_) {
            needsContinue_ = false;
            writeRequest wr;
            wr.kind = writeRequest::headers;
            wr.st = st_;
            wr.status = 100;
            sc_->writeFromHandler(std::move(wr));
        }
        if (!pipe_ || sawEof_) return {0, io::EOF_};
        auto [n, err] = pipe_->Read(p);
        if (err == io::EOF_) sawEof_ = true;
        if (n > 0) sc_->noteBodyReadFromHandler(st_, static_cast<int>(n));
        return {n, err};
    }

    gopp::error Close() override {
        if (pipe_ && !closed_) pipe_->breakWithError(errClosedBody);
        closed_ = true;
        return nullptr;
    }

private:
    ptr<serverConn> sc_;
    ptr<stream> st_;
    ptr<pipe> pipe_;
    bool needsContinue_;
    bool sawEof_ = false;
    bool closed_ = false;
};

bool bodyAllowedForStatus(int status) {
    if (status >= 100 && status <= 199) return false;
    return status != 204 && status != 304;
}

// The ResponseWriter of a stream (responseWriter and responseWriterState).
// Writes collect in a 4 KiB buffer; a full buffer, flush() or the end of
// the handler sends it as a chunk: the header first, then DATA.
class responseWriter final : public ResponseWriter, public Flusher {
public:
    responseWriter(ptr<serverConn> sc, ptr<stream> st, Request* req) : sc_(std::move(sc)), st_(std::move(st)), req_(req) {}

    http::Header& Header() override { return handlerHeader_; }

    void WriteHeader(int code) override {
        if (wroteHeader_) {
            internal::logf("http2: superfluous response.WriteHeader call");
            return;
        }
        if (code < 100 || code > 999) panic("invalid WriteHeader code " + std::to_string(code));
        if (code >= 100 && code <= 199) {
            // Informational: send it now; the final status follows.
            writeRequest wr;
            wr.kind = writeRequest::headers;
            wr.st = st_;
            wr.status = code;
            wr.header = handlerHeader_;
            sc_->writeFromHandler(std::move(wr));
            return;
        }
        wroteHeader_ = true;
        status_ = code;
        snapHeader_ = handlerHeader_;
    }

    std::pair<isize, gopp::error> Write(std::span<const byte> p) override {
        if (!wroteHeader_) WriteHeader(StatusOK);
        if (!bodyAllowedForStatus(status_)) return {0, ErrBodyNotAllowed};
        wroteBytes_ += static_cast<int64>(p.size());
        if (sentContentLen_ != 0 && declaredLen_ >= 0 && wroteBytes_ > declaredLen_) return {0, errTooMuchData};
        isize total = static_cast<isize>(p.size());
        // bufio.Writer.Write
        while (p.size() > handlerChunkWriteSize - buf_.size() && !err_) {
            std::size_t k;
            if (buf_.empty()) {
                // Large write, empty buffer: write directly.
                k = p.size();
                writeChunk(asStringView(p));
            } else {
                k = handlerChunkWriteSize - buf_.size();
                buf_.append(asStringView(p.first(k)));
                flushBuf();
            }
            p = p.subspan(k);
        }
        if (err_) return {total - static_cast<isize>(p.size()), err_};
        buf_.append(asStringView(p));
        return {total, nullptr};
    }

    void Flush() override { flushError(); }

    gopp::error flushError() {
        if (!buf_.empty()) {
            flushBuf();
        } else {
            // The header and/or the final DATA frame must go out even
            // without data.
            writeChunk({});
            if (!err_) {
                bool closed = false;
                st_->cw.tryRecv(*newEmptySlot(), &closed);
                if (closed) err_ = st_->closeErr;
            }
        }
        return err_;
    }

    // The handler returned.
    void handlerDone() {
        handlerDone_ = true;
        flushError();
    }

private:
    static empty* newEmptySlot() {
        thread_local empty e;
        return &e;
    }

    void flushBuf() {
        std::string b = std::move(buf_);
        buf_.clear();
        writeChunk(b);
    }

    // writeChunk: sends the header with the first chunk, then the data.
    void writeChunk(std::string_view p) {
        if (err_) return;
        if (!wroteHeader_) WriteHeader(StatusOK);
        bool isHead = req_->Method == "HEAD";
        if (!sentHeader_) {
            sentHeader_ = true;
            writeRequest wr;
            wr.kind = writeRequest::headers;
            wr.st = st_;
            wr.status = status_;
            string clen = snapHeader_.Get("Content-Length");
            if (!clen.empty()) {
                snapHeader_.Del("Content-Length");
                auto [cl, err] = strconv::ParseInt(clen, 10, 63);
                if (!err && cl >= 0) {
                    declaredLen_ = cl;
                    sentContentLen_ = cl;
                } else {
                    clen = {};
                }
            }
            if (clen.empty() && handlerDone_ && bodyAllowedForStatus(status_) && (!p.empty() || !isHead)) {
                clen = strconv::Itoa(static_cast<int64>(p.size()));
            }
            wr.ContentLength = clen;
            bool hasCt = snapHeader_.Has("Content-Type");
            bool hasCe = !snapHeader_.Get("Content-Encoding").empty();
            if (!hasCe && !hasCt && bodyAllowedForStatus(status_) && !p.empty()) {
                wr.contentType = DetectContentType(std::span<const byte>(reinterpret_cast<const byte*>(p.data()), p.size()));
            }
            if (!snapHeader_.Has("Date")) {
                std::string d;
                internal::appendDate(d);
                wr.date = string(d);
            }
            if (snapHeader_.Has("Connection")) {
                string v = snapHeader_.Get("Connection");
                snapHeader_.Del("Connection");
                if (v == "close") sc_->sendServeMsg(nullptr);  // graceful shutdown
            }
            bool endStream = (handlerDone_ && p.empty()) || isHead;
            wr.endStream = endStream;
            wr.header = std::move(snapHeader_);
            // The header goes out before any data, as both queue on the
            // stream in order; no need to wait for it.
            if (gopp::error err = sc_->writeFromHandler(std::move(wr))) {
                err_ = err;
                return;
            }
            if (endStream) return;
        }
        if (isHead) return;
        if (p.empty() && !handlerDone_) return;
        writeRequest wr;
        wr.kind = writeRequest::data;
        wr.st = st_;
        wr.bytes = string(p);
        wr.endStream = handlerDone_;
        if (handlerDone_) {
            // The last chunk: the handler need not wait for it.
            if (gopp::error err = sc_->writeFromHandler(std::move(wr))) err_ = err;
            return;
        }
        if (gopp::error err = sc_->writeAndWait(std::move(wr))) err_ = err;
    }

    ptr<serverConn> sc_;
    ptr<stream> st_;
    Request* req_;
    http::Header handlerHeader_;
    http::Header snapHeader_;
    int status_ = 0;
    bool wroteHeader_ = false;
    bool sentHeader_ = false;
    bool handlerDone_ = false;
    int64 wroteBytes_ = 0;
    int64 declaredLen_ = -1;
    int64 sentContentLen_ = -1;
    std::string buf_;
    gopp::error err_;
};

// ---------------------------------------------------------------------------
// Handler side

gopp::error serverConn::writeFromHandler(writeRequest wr) {
    gopp::error err;
    select(send(wantWriteCh_, std::move(wr), [] {}), recv(doneServing_, [&] { err = errClientDisconnected; }));
    return err;
}

gopp::error serverConn::writeAndWait(writeRequest wr) {
    ptr<stream> st = wr.st;
    chan<gopp::error> done = make_chan<gopp::error>(1);
    wr.done = done;
    if (gopp::error err = writeFromHandler(std::move(wr))) return err;
    gopp::error err;
    select(recv(done, [&](gopp::error e) { err = std::move(e); }),
           recv(doneServing_, [&] { err = errClientDisconnected; }),
           recv(st->cw, [&] {
               // The stream closed; the write may have finished anyway.
               gopp::error e;
               if (done.tryRecv(e)) err = std::move(e);
               else err = errStreamClosed;
           }));
    return err;
}

void serverConn::noteBodyReadFromHandler(const ptr<stream>& st, int n) {
    select(send(bodyReadCh_, bodyReadMsg{st, n}, [] {}), recv(doneServing_, [] {}));
}

void serverConn::sendServeMsg(std::function<void()> fn) {
    if (!fn) {
        // A graceful shutdown.
        ptr<serverConn> self(this);
        fn = [self] { self->goAway(ErrCode::no); };
    }
    select(send(serveMsgCh_, std::move(fn), [] {}), recv(doneServing_, [] {}));
}

// ---------------------------------------------------------------------------
// The serve loop

void serverConn::serve() {
    ptr<serverConn> self(this);
    if (opts_.onShutdown) {
        opts_.onShutdown([self] {
            // Runs on the shutting down fiber, under the server's lock.
            std::function<void()> fn = [self] { self->goAway(ErrCode::no); };
            self->serveMsgCh_.trySend(std::move(fn));
        });
    }

    // Our SETTINGS and the larger connection window go out first.
    Setting settings[] = {
        {SettingID::maxFrameSize, defaultMaxReadFrameSize},
        {SettingID::maxConcurrentStreams, advMaxStreams_},
        {SettingID::maxHeaderListSize, maxHeaderListSize_},
        {SettingID::headerTableSize, initialHeaderTableSize},
        {SettingID::initialWindowSize, uint32(initialStreamRecvWindow)},
    };
    appendSettings(control_, settings);
    unackedSettings_++;
    if (int32 diff = initialConnRecvWindow - int32(initialWindowSize); diff > 0) sendWindowUpdate(nullptr, diff);

    // The writer.
    go([self] {
        for (;;) {
            auto [buf, ok] = self->writeCh_.recv2();
            if (!ok) return;
            auto [n, err] = io::WriteString(*self->conn_, buf);
            self->wroteCh_.send(err);
        }
    });
    scheduleFrameWrite();

    // The client preface, which an h2c server peeked at already.
    conn_->SetReadDeadline(time::Now().Add(prefaceTimeout));
    byte preface[24];
    if (auto [n, err] = io::ReadFull(*br_, std::span<byte>(preface, sizeof(preface))); err) {
        if (err != io::EOF_ && !errors::Is(err, net::ErrClosed)) {
            internal::logf("http2: server: error reading preface from client " + std::string(remoteAddr_.view()) + ": " +
                           std::string(err.Error().view()));
        }
        exit_ = true;
    } else if (asStringView(std::span<const byte>(preface, sizeof(preface))) != clientPreface) {
        internal::logf("http2: server: error reading preface from client " + std::string(remoteAddr_.view()) +
                       ": bogus greeting");
        exit_ = true;
    }
    conn_->SetReadDeadline(time::Time{});

    if (!exit_) {
        // The reader.
        go([self] {
            for (;;) {
                auto [f, err] = self->framer_.readFrame();
                // Only stream errors leave the connection readable.
                bool terminal = err && !errors::As<StreamError>(err);
                bool sent = false;
                select(send(self->readFrameCh_, readFrameResult{std::move(f), err}, [&] { sent = true; }),
                       recv(self->doneServing_, [] {}));
                if (!sent || terminal) return;
            }
        });
    }

    auto settingsTimer = time::AfterFunc(firstSettingsTimeout, [self] {
        self->sendServeMsg([self] {
            if (!self->sawFirstSettings_) self->exit_ = true;
        });
    });
    resetIdleTimer();

    while (!exit_) {
        select(recv(wantWriteCh_, [&](writeRequest wr) { queueWrite(std::move(wr)); }),
               recv(wroteCh_, [&](gopp::error err) { wroteBatch(std::move(err)); }),
               recv(readFrameCh_, [&](readFrameResult res) {
                   // Finish a write first: it may have closed a stream.
                   gopp::error werr;
                   if (writing_ && wroteCh_.tryRecv(werr)) wroteBatch(std::move(werr));
                   if (!processFrameFromReader(std::move(res))) exit_ = true;
               }),
               recv(bodyReadCh_, [&](bodyReadMsg m) { noteBodyRead(m.st, m.n); }),
               recv(serveMsgCh_, [&](std::function<void()> fn) {
                   if (fn) fn();
               }));
        if (exit_) break;
        scheduleFrameWrite();

        // After GOAWAY: close once the streams are done (or right away
        // for an error), giving the peer a moment to read it.
        bool sentGoAway = inGoAway_ && !needToSendGoAway_ && !writing_;
        bool gracefulDone = goAwayCode_ == ErrCode::no && streams_.empty();
        if (sentGoAway && !shutdownTimer_ && (goAwayCode_ != ErrCode::no || gracefulDone)) {
            shutdownTimer_ = time::AfterFunc(goAwayTimeout, [self] {
                self->sendServeMsg([self] { self->exit_ = true; });
            });
        }
    }

    // The timers' functions refer to this connection: drop them.
    settingsTimer->Stop();
    if (idleTimer_) idleTimer_->Stop();
    if (shutdownTimer_) shutdownTimer_->Stop();
    idleTimer_ = nullptr;
    shutdownTimer_ = nullptr;
    if (opts_.onShutdown) opts_.onShutdown(nullptr);
    doneServing_.close();
    conn_->Close();
    writeCh_.close();
    // Close the streams, so that their handlers see the connection end.
    auto streams = std::move(streams_);
    for (auto& [id, st] : streams) {
        st->state = streamState::closed;
        if (st->body) st->body->closeWithError(errClientDisconnected);
        st->closeErr = errClientDisconnected;
        releaseStream(*st);
        st->cw.close();
    }
    // Free the queued writes; their handlers see doneServing.
    queues_.clear();
    controlEffects_.clear();
    inFlight_.clear();
}

bool serverConn::processFrameFromReader(readFrameResult res) {
    gopp::error err = std::move(res.err);
    if (err) {
        if (err == errFrameTooLarge) {
            goAway(ErrCode::frameSize);
            return true;
        }
        bool clientGone = err == io::EOF_ || err == io::ErrUnexpectedEOF || errors::Is(err, net::ErrClosed) ||
                           errors::As<net::OpError>(err) || errors::As<net::Errno>(err);
        if (clientGone) return false;
    } else {
        err = processFrame(res.f);
        if (!err) return true;
    }
    if (auto* se = errors::As<StreamError>(err)) {
        resetStream(se->streamId, se->code, se->cause);
        return true;
    }
    if (auto* ce = errors::As<ConnectionError>(err)) {
        std::string why = ce->reason.empty() ? std::string(err.Error().view()) : std::string(ce->reason.view());
        if (gopp::error d = framer_.errorDetail(); d && ce->reason.empty()) why = std::string(d.Error().view());
        internal::logf("http2: server connection error from " + std::string(remoteAddr_.view()) + ": " + why);
        goAway(ce->code);
        return true;
    }
    internal::logf("http2: server closing client connection: " + std::string(err.Error().view()));
    return false;
}

gopp::error serverConn::processFrame(Frame& f) {
    // The first frame must be SETTINGS.
    if (!sawFirstSettings_) {
        if (f.type() != FrameType::settings) return connectionError(ErrCode::protocol, "first frame was not SETTINGS");
        sawFirstSettings_ = true;
    }
    // After a GOAWAY, ignore new streams (and everything after an error),
    // but keep the connection's flow control right.
    if (inGoAway_ && (goAwayCode_ != ErrCode::no || f.streamId() > maxClientStreamId_)) {
        if (f.type() == FrameType::data) {
            if (!inflow_.take(f.header.length)) return connectionError(ErrCode::flowControl);
            sendWindowUpdate(nullptr, static_cast<int32>(f.header.length));
        }
        return nullptr;
    }
    switch (f.type()) {
    case FrameType::settings: return processSettings(f);
    case FrameType::headers: return processHeaders(f);
    case FrameType::windowUpdate: return processWindowUpdate(f);
    case FrameType::ping:
        if (!f.isAck()) {
            if (inGoAway_ && goAwayCode_ != ErrCode::no) return nullptr;
            appendPing(control_, true, f.data);
        }
        return nullptr;
    case FrameType::data: return processData(f);
    case FrameType::rstStream: return processResetStream(f);
    case FrameType::priority:
        if (f.streamId() == f.priority.streamDep) return streamError(f.streamId(), ErrCode::protocol);
        return nullptr;
    case FrameType::goaway: return processGoaway(f);
    case FrameType::pushPromise:
        // A client must not push.
        return connectionError(ErrCode::protocol, "client sent PUSH_PROMISE");
    default:
        return nullptr;  // unknown frame types are ignored
    }
}

gopp::error serverConn::processSettings(const Frame& f) {
    if (f.isAck()) {
        unackedSettings_--;
        if (unackedSettings_ < 0) return connectionError(ErrCode::protocol, "unexpected SETTINGS ACK");
        return nullptr;
    }
    if (f.settings.size() > 100) return connectionError(ErrCode::protocol, "too many settings");
    for (std::size_t i = 0; i < f.settings.size(); i++) {
        for (std::size_t j = 0; j < i; j++) {
            if (f.settings[i].id == f.settings[j].id) return connectionError(ErrCode::protocol, "duplicate setting");
        }
    }
    for (auto& s : f.settings) {
        if (gopp::error err = processSetting(s)) return err;
    }
    needToSendSettingsAck_ = true;
    return nullptr;
}

gopp::error serverConn::processSetting(const Setting& s) {
    if (gopp::error err = s.Valid()) return err;
    switch (s.id) {
    case SettingID::headerTableSize: henc_.setMaxDynamicTableSize(s.val); break;
    case SettingID::initialWindowSize: {
        // Applies to all open streams, by the difference (6.9.2).
        int32 growth = static_cast<int32>(s.val) - initialStreamSendWindow_;
        initialStreamSendWindow_ = static_cast<int32>(s.val);
        for (auto& [id, st] : streams_) {
            if (!st->flow.add(growth)) return connectionError(ErrCode::flowControl);
        }
        break;
    }
    case SettingID::maxFrameSize: peerMaxFrameSize_ = s.val; break;
    default: break;  // ENABLE_PUSH, MAX_CONCURRENT_STREAMS (for pushes), MAX_HEADER_LIST_SIZE, ...
    }
    return nullptr;
}

std::pair<streamState, ptr<stream>> serverConn::state(uint32 id) {
    if (auto it = streams_.find(id); it != streams_.end()) return {it->second->state, it->second};
    // Opening a stream implicitly closes the idle ones below it (5.1.1).
    if (id % 2 == 1 && id <= maxClientStreamId_) return {streamState::closed, nullptr};
    if (id % 2 == 0 && id != 0) return {streamState::closed, nullptr};  // we never push
    return {streamState::idle, nullptr};
}

gopp::error serverConn::processData(Frame& f) {
    if (inGoAway_ && goAwayCode_ != ErrCode::no) return nullptr;
    uint32 id = f.streamId();
    auto [state, st] = this->state(id);
    if (id == 0 || state == streamState::idle) return connectionError(ErrCode::protocol, "DATA on an idle stream");
    if (!st || state != streamState::open || st->gotTrailerHeader || st->resetQueued) {
        // Not for an open stream: keep the connection's flow control
        // right, and tell the peer to stop.
        if (!inflow_.take(f.header.length)) return streamError(id, ErrCode::flowControl);
        sendWindowUpdate(nullptr, static_cast<int32>(f.header.length));
        if (st && st->resetQueued) return nullptr;
        return streamError(id, ErrCode::streamClosed);
    }
    if (!st->body) panic("http2: internal error: should have a body in this state");
    if (st->declBodyBytes != -1 && st->bodyBytes + static_cast<int64>(f.data.size()) > st->declBodyBytes) {
        if (!inflow_.take(f.header.length)) return streamError(id, ErrCode::flowControl);
        sendWindowUpdate(nullptr, static_cast<int32>(f.header.length));
        st->body->closeWithError(errors::New(std::format("sender tried to send more than declared Content-Length of {} bytes",
                                                           st->declBodyBytes)));
        return streamError(id, ErrCode::protocol);
    }
    if (f.header.length > 0) {
        if (!takeInflows(inflow_, st->in, f.header.length)) return streamError(id, ErrCode::flowControl);
        std::size_t len = f.data.size();
        if (len > 0) {
            st->bodyBytes += static_cast<int64>(len);
            if (st->body->write(std::move(f.data))) {
                // The handler closed the body: return the connection's
                // flow control for the discarded data.
                sendWindowUpdate(nullptr, static_cast<int32>(f.header.length));
                return nullptr;
            }
        }
        // Padding is refunded now; data is refunded as the handler reads.
        if (int32 pad = static_cast<int32>(f.header.length - len); pad > 0) {
            sendWindowUpdate(nullptr, pad);
            sendWindowUpdate(st.get(), pad);
        }
    }
    if (f.streamEnded()) endStream(*st);
    return nullptr;
}

void serverConn::endStream(stream& st) {
    if (st.declBodyBytes != -1 && st.declBodyBytes != st.bodyBytes) {
        st.body->closeWithError(errors::New(std::format("request declared a Content-Length of {} but only wrote {} bytes",
                                                          st.declBodyBytes, st.bodyBytes)));
    } else {
        st.body->closeWithError(io::EOF_);
    }
    st.state = streamState::halfClosedRemote;
}

gopp::error serverConn::processHeaders(Frame& f) {
    uint32 id = f.streamId();
    // Clients use odd stream ids (5.1.1).
    if (id % 2 != 1) return connectionError(ErrCode::protocol, "HEADERS on an even stream");
    if (auto it = streams_.find(id); it != streams_.end()) {
        stream& st = *it->second;
        if (st.resetQueued) return nullptr;
        if (st.state == streamState::halfClosedRemote) return streamError(id, ErrCode::streamClosed);
        return processTrailerHeaders(st, f);
    }
    // New streams must have increasing ids (5.1.1).
    if (id <= maxClientStreamId_) return connectionError(ErrCode::protocol, "HEADERS on a closed stream");
    maxClientStreamId_ = id;
    if (idleTimer_) idleTimer_->Stop();
    if (curClientStreams_ + 1 > advMaxStreams_) {
        if (unackedSettings_ == 0) return streamError(id, ErrCode::protocol);  // they should know better
        return streamError(id, ErrCode::refusedStream);                        // a race with our SETTINGS
    }
    ptr<stream> st = newStream(id, f.streamEnded() ? streamState::halfClosedRemote : streamState::open);
    if (f.hasPriority() && f.priority.streamDep == id) return streamError(id, ErrCode::protocol);
    ptr<Request> req;
    if (gopp::error err = NewRequest(*st, f, req)) return err;
    int status = 0;
    string msg;
    if (f.truncated) {
        status = StatusRequestHeaderFieldsTooLarge;
        msg = string::fromStatic("Request Header Fields Too Large");
    } else {
        // Connection-specific headers are invalid in HTTP/2 (8.2.2).
        for (std::string_view k : {"Connection", "Proxy-Connection", "Keep-Alive", "Transfer-Encoding", "Upgrade"}) {
            if (req->Header.Has(k)) {
                status = StatusBadRequest;
                msg = "request header " + strconv::Quote(k) + " is not valid in HTTP/2";
                break;
            }
        }
        auto te = req->Header.Values("Te");
        if (!status && !te.empty() && (te.size() > 1 || (te[0] != "trailers" && !te[0].empty()))) {
            status = StatusBadRequest;
            msg = string::fromStatic("request header \"TE\" may only be \"trailers\" in HTTP/2");
        }
    }
    if (opts_.ReadTimeout.ns > 0) {
        ptr<serverConn> self(this);
        ptr<stream> s = st;
        st->readDeadline = time::AfterFunc(opts_.ReadTimeout, [self, s] {
            self->sendServeMsg([s] {
                if (s->body) s->body->closeWithError(net::ErrDeadlineExceeded);
            });
        });
    }
    scheduleHandler(st, std::move(req), status, std::move(msg));
    return nullptr;
}

gopp::error serverConn::processTrailerHeaders(stream& st, const Frame& f) {
    if (st.gotTrailerHeader) return connectionError(ErrCode::protocol, "second trailer");
    st.gotTrailerHeader = true;
    if (!f.streamEnded()) return streamError(st.id, ErrCode::protocol);
    if (!f.pseudoFields().empty()) return streamError(st.id, ErrCode::protocol);
    endStream(st);
    return nullptr;
}

ptr<stream> serverConn::newStream(uint32 id, streamState state) {
    auto st = make_ptr<stream>();
    st->id = id;
    st->state = state;
    st->cw = make_chan<empty>();
    st->flow.conn = &flow_;
    st->flow.add(initialStreamSendWindow_);
    st->in.init(initialStreamRecvWindow);
    if (opts_.WriteTimeout.ns > 0) {
        ptr<serverConn> self(this);
        st->writeDeadline = time::AfterFunc(opts_.WriteTimeout, [self, id] {
            self->sendServeMsg([self, id] {
                if (self->streams_.contains(id)) self->resetStream(id, ErrCode::internal, net::ErrDeadlineExceeded);
            });
        });
    }
    streams_[id] = st;
    curClientStreams_++;
    if (streams_.size() == 1) setActive(true);
    return st;
}

gopp::error serverConn::NewRequest(stream& st, const Frame& f, ptr<Request>& out) {
    string method = f.pseudoValue("method");
    string scheme = f.pseudoValue("scheme");
    string authority = f.pseudoValue("authority");
    string path = f.pseudoValue("path");
    bool isConnect = method == "CONNECT";
    if (!f.pseudoValue("protocol").empty()) return streamError(st.id, ErrCode::protocol);  // no extended CONNECT
    if (isConnect) {
        if (!path.empty() || !scheme.empty() || authority.empty()) return streamError(st.id, ErrCode::protocol);
    } else if (method.empty() || path.empty() || (scheme != "https" && scheme != "http")) {
        return streamError(st.id, ErrCode::protocol);
    }

    auto req = make_ptr<Request>();
    // Cookies may come as separate fields; HTTP/1 has one header (8.2.3).
    std::string cookie;
    for (auto& hf : f.regularFields()) {
        if (hf.name == "cookie") {
            if (!cookie.empty()) cookie += "; ";
            cookie += hf.value.view();
            continue;
        }
        req->Header.AddCanonical(CanonicalHeaderKey(hf.name), hf.value);
    }
    if (!cookie.empty()) req->Header.AddCanonical(string::fromStatic("Cookie"), string(cookie));
    if (authority.empty()) authority = req->Header.Get("Host");

    if (isConnect) {
        req->URL.Host = authority;
        req->RequestURI = authority;
    } else {
        auto [u, err] = url::ParseRequestURI(path);
        if (err) return streamError(st.id, ErrCode::protocol);
        req->URL = std::move(u);
        req->RequestURI = path;
    }
    req->Method = method;
    req->Proto = string::fromStatic("HTTP/2.0");
    req->ProtoMajor = 2;
    req->ProtoMinor = 0;
    req->Host = authority;
    req->RemoteAddr = remoteAddr_;
    req->TLS = tlsState_;

    bool bodyOpen = !f.streamEnded();
    auto cls = req->Header.Values("Content-Length");
    if (!cls.empty()) {
        auto [cl, err] = strconv::ParseInt(cls[0], 10, 63);
        req->ContentLength = !err && cl >= 0 ? cl : 0;
        if (!bodyOpen && req->ContentLength != 0) return streamError(st.id, ErrCode::protocol);
        for (std::size_t i = 1; i < cls.size(); i++) {
            if (cls[i] != cls[0]) return streamError(st.id, ErrCode::protocol);
        }
        if (cls.size() > 1) req->Header.Set("Content-Length", cls[0]);
    }
    ptr<serverConn> self(this);
    ptr<stream> sp(&st);
    if (bodyOpen) {
        if (cls.empty()) req->ContentLength = -1;
        st.body = make_ptr<pipe>();
        st.declBodyBytes = req->ContentLength;
        bool needsContinue = ascii::EqualFold(req->Header.Get("Expect"), "100-continue");
        if (needsContinue) req->Header.Del("Expect");
        req->Body = make_ptr<requestBody>(self, sp, st.body, needsContinue);
    } else {
        req->Body = internal::noBody();
    }
    // The context is created now: the serve loop cancels it when the
    // stream ends, while the handler may be using it.
    req->Context();
    st.req = req;
    out = std::move(req);
    return nullptr;
}

void serverConn::scheduleHandler(ptr<stream> st, ptr<Request> req, int statusOverride, string msg) {
    ptr<serverConn> self(this);
    ptr<Handler> h = handler_;
    go([self, st, req, h, statusOverride, msg] {
        responseWriter rw(self, st, req.get());
        // A handler that throws, as a Go handler panics, resets the stream;
        // throwing ErrAbortHandler does so without a log.
        std::optional<std::string> panicked;
        try {
            if (statusOverride) {
                http::Error(rw, msg, statusOverride);
            } else {
                h->ServeHTTP(rw, *req);
            }
        } catch (const gopp::error& e) {
            panicked = e == ErrAbortHandler ? std::string() : std::string(e.Error().view());
        } catch (const std::exception& e) {
            panicked = e.what();
        }
        if (panicked) {
            if (!panicked->empty()) {
                internal::logf("http2: panic serving " + std::string(self->remoteAddr_.view()) + ": " + *panicked);
            }
            req->CancelContext();
            if (req->MultipartForm) req->MultipartForm->RemoveAll();
            writeRequest wr;
            wr.kind = writeRequest::rst;
            wr.st = st;
            wr.code = ErrCode::internal;
            self->writeFromHandler(std::move(wr));
            return;
        }
        req->CancelContext();
        rw.handlerDone();
        if (req->MultipartForm) req->MultipartForm->RemoveAll();
    });
}

gopp::error serverConn::processWindowUpdate(const Frame& f) {
    if (f.streamId() != 0) {
        auto [state, st] = this->state(f.streamId());
        if (state == streamState::idle) return connectionError(ErrCode::protocol, "WINDOW_UPDATE on an idle stream");
        if (!st) return nullptr;  // closed: ignore
        if (!st->flow.add(static_cast<int32>(f.increment))) return streamError(f.streamId(), ErrCode::flowControl);
    } else if (!flow_.add(static_cast<int32>(f.increment))) {
        return connectionError(ErrCode::flowControl, "connection window overflow");
    }
    return nullptr;
}

gopp::error serverConn::processResetStream(const Frame& f) {
    auto [state, st] = this->state(f.streamId());
    if (state == streamState::idle) return connectionError(ErrCode::protocol, "RST_STREAM on an idle stream");
    if (st) closeStream(st, streamError(f.streamId(), f.errCode));
    return nullptr;
}

gopp::error serverConn::processGoaway(const Frame& f) {
    if (f.errCode != ErrCode::no) {
        internal::logf("http2: received GOAWAY " + std::string(to_string(f.errCode).view()) + ", starting graceful shutdown");
    }
    goAway(ErrCode::no);
    return nullptr;
}

void serverConn::closeStream(const ptr<stream>& st, gopp::error err) {
    if (st->state == streamState::idle || st->state == streamState::closed) panic("http2: can't close stream in this state");
    st->state = streamState::closed;
    curClientStreams_--;
    streams_.erase(st->id);
    queues_.erase(st->id);
    if (streams_.empty()) {
        setActive(false);
        resetIdleTimer();
        if (opts_.DisableKeepAlives) goAway(ErrCode::no);
    }
    if (st->body) {
        // Return the connection's flow control for unread data.
        sendWindowUpdate(nullptr, static_cast<int32>(st->body->len()));
        st->body->closeWithError(err);
    }
    if (auto* se = errors::As<StreamError>(err)) err = se->cause ? se->cause : errStreamClosed;
    st->closeErr = err;
    releaseStream(*st);
    st->cw.close();
}

// Drops what a closed stream refers to: its request (whose body refers back
// to the stream) and its deadline timers (whose functions do too).
void serverConn::releaseStream(stream& st) {
    if (st.readDeadline) {
        st.readDeadline->Stop();
        st.readDeadline = nullptr;
    }
    if (st.writeDeadline) {
        st.writeDeadline->Stop();
        st.writeDeadline = nullptr;
    }
    if (st.req) {
        st.req->CancelContext();
        st.req = nullptr;
    }
}

void serverConn::resetStream(uint32 id, ErrCode code, gopp::error cause) {
    appendRstStream(control_, id, code);
    auto it = streams_.find(id);
    writeEffect e;
    e.rst = true;
    e.rstErr = streamError(id, code, std::move(cause));
    if (it != streams_.end()) {
        it->second->resetQueued = true;
        e.st = it->second;
    }
    controlEffects_.push_back(std::move(e));
}

void serverConn::goAway(ErrCode code) {
    if (inGoAway_) {
        if (goAwayCode_ == ErrCode::no) goAwayCode_ = code;
        return;
    }
    inGoAway_ = true;
    needToSendGoAway_ = true;
    goAwayCode_ = code;
}

void serverConn::sendWindowUpdate(stream* st, int32 n) {
    if (n <= 0) return;
    int32 send = st ? st->in.add(n) : inflow_.add(n);
    if (send == 0) return;
    appendWindowUpdate(control_, st ? st->id : 0, static_cast<uint32>(send));
}

void serverConn::noteBodyRead(const ptr<stream>& st, int n) {
    sendWindowUpdate(nullptr, n);
    if (st->state != streamState::halfClosedRemote && st->state != streamState::closed) {
        sendWindowUpdate(st.get(), n);
    }
}

void serverConn::setActive(bool active) {
    if (active_ == active) return;
    active_ = active;
    if (opts_.setActive) opts_.setActive(active);
}

void serverConn::resetIdleTimer() {
    if (opts_.IdleTimeout.ns <= 0 || !streams_.empty()) return;
    if (idleTimer_) {
        idleTimer_->Reset(opts_.IdleTimeout);
        return;
    }
    ptr<serverConn> self(this);
    idleTimer_ = time::AfterFunc(opts_.IdleTimeout, [self] {
        self->sendServeMsg([self] {
            if (self->streams_.empty()) self->goAway(ErrCode::no);
        });
    });
}

// ---------------------------------------------------------------------------
// Writing

void serverConn::queueWrite(writeRequest wr) {
    if (wr.kind == writeRequest::rst) {
        if (streams_.contains(wr.st->id)) resetStream(wr.st->id, wr.code);
        return;
    }
    auto it = streams_.find(wr.st->id);
    if (it == streams_.end() || it->second != wr.st || wr.st->state == streamState::closed ||
        wr.st->state == streamState::halfClosedLocal) {
        // The stream is gone: its handler sees cw closed.
        if (wr.done) wr.done.trySend(gopp::error(errStreamClosed));
        return;
    }
    queues_[wr.st->id].push_back(std::move(wr));
}

void serverConn::encodeHeaders(const writeRequest& wr) {
    hbuf_.clear();
    auto field = [&](std::string_view k, std::string_view v) { henc_.writeField({string(k), string(v)}); };
    if (wr.status) {
        char num[8];
        int len = std::snprintf(num, sizeof(num), "%d", wr.status);
        field(":status", std::string_view(num, static_cast<std::size_t>(len)));
    }
    for (auto& e : wr.header) {
        string k = lowerHeader(e.key);
        if (!validWireHeaderFieldName(k)) continue;
        // Connection-specific headers don't exist in HTTP/2 (8.2.2).
        if (k == "connection" || k == "proxy-connection" || k == "keep-alive" || k == "upgrade") continue;
        if (k == "transfer-encoding" && e.value != "trailers") continue;
        if (!internal::validHeaderFieldValue(e.value)) continue;
        henc_.writeField({k, e.value});
    }
    if (!wr.contentType.empty()) field("content-type", wr.contentType);
    if (!wr.ContentLength.empty()) field("content-length", wr.ContentLength);
    if (!wr.date.empty()) field("date", wr.date);
}

void serverConn::scheduleFrameWrite() {
    if (writing_ || exit_) return;
    batch_.clear();
    inFlight_.clear();
    if (needToSendGoAway_) {
        needToSendGoAway_ = false;
        appendGoaway(batch_, maxClientStreamId_, goAwayCode_);
    }
    if (needToSendSettingsAck_) {
        needToSendSettingsAck_ = false;
        appendSettingsAck(batch_);
    }
    batch_ += control_;
    control_.clear();
    for (auto& e : controlEffects_) inFlight_.push_back(std::move(e));
    controlEffects_.clear();

    // Stream frames, one per stream per round, until the batch is full
    // or nothing can be sent.
    if (!inGoAway_ || goAwayCode_ == ErrCode::no) {
        uint32 frameMax = std::min(peerMaxFrameSize_, maxDataFrameSize);
        bool progress = true;
        while (progress && batch_.size() < maxWriteBatch && !queues_.empty()) {
            progress = false;
            auto it = queues_.upper_bound(nextQueue_);
            for (std::size_t visited = 0, n = queues_.size(); visited < n && batch_.size() < maxWriteBatch; visited++) {
                if (it == queues_.end()) it = queues_.begin();
                auto& [id, q] = *it;
                writeRequest& wr = q.front();
                ptr<stream> st = wr.st;
                bool popped = false;
                if (wr.kind == writeRequest::headers) {
                    encodeHeaders(wr);
                    bool end = wr.endStream && wr.status >= 200;
                    appendHeaders(batch_, id, hbuf_, end, peerMaxFrameSize_);
                    inFlight_.push_back({st, end, false, nullptr, std::move(wr.done)});
                    popped = true;
                } else {
                    std::size_t remain = wr.bytes.size() - wr.off;
                    int32 avail = st->flow.available();
                    if (remain == 0 || avail > 0) {
                        std::size_t k = std::min<std::size_t>({remain, static_cast<std::size_t>(std::max(avail, 0)), frameMax});
                        bool last = k == remain;
                        appendData(batch_, id, last && wr.endStream, wr.bytes.view().substr(wr.off, k));
                        st->flow.take(static_cast<int32>(k));
                        wr.off += k;
                        if (last) {
                            inFlight_.push_back({st, wr.endStream, false, nullptr, std::move(wr.done)});
                            popped = true;
                        }
                        progress = true;
                    }
                }
                if (popped) {
                    progress = true;
                    q.pop_front();
                    nextQueue_ = id;
                    if (q.empty()) {
                        it = queues_.erase(it);
                        continue;
                    }
                }
                nextQueue_ = id;
                ++it;
            }
        }
    }
    if (batch_.empty()) return;
    writing_ = true;
    writeCh_.send(std::move(batch_));
    batch_.clear();
}

void serverConn::wroteBatch(gopp::error err) {
    writing_ = false;
    if (err) conn_->Close();
    auto effects = std::move(inFlight_);
    inFlight_.clear();
    for (auto& e : effects) {
        if (e.endsStream && e.st) {
            stream& st = *e.st;
            if (st.state == streamState::open) {
                // The handler is done but the client is still sending:
                // tell it to stop (as Go does, with NO_ERROR).
                st.state = streamState::halfClosedLocal;
                resetStream(st.id, ErrCode::no);
            } else if (st.state == streamState::halfClosedRemote) {
                closeStream(e.st, errHandlerComplete);
            }
        } else if (e.rst && e.st && e.st->state != streamState::closed) {
            closeStream(e.st, e.rstErr);
        }
        if (e.done) e.done.trySend(gopp::error(err));
    }
}

}  // namespace

}  // namespace http2

void internal::serveHttp2(const ptr<net::Conn>& c, const ptr<bufio::Reader>& br, const ptr<Handler>& handler,
                           const serverOptions& opts, std::optional<tls::ConnectionState> tlsState) {
    auto sc = make_ptr<http2::serverConn>(c, br, handler, opts, std::move(tlsState));
    sc->serve();
}

}  // namespace gopp::http
