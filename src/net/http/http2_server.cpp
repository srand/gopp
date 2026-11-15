// The HTTP/2 server, after Go's net/http/internal/http2/server.Go.
//
// As in Go, one fiber owns the connection's state (the serve loop). A
// reader fiber reads frames and hands them to it; a writer fiber writes
// what it batches up; handlers run on their own fibers and ask it to
// write their responses over channels. Writes are batched: while the
// writer is busy, frames queue up, and the next write sends them all.
#include "ascii.hpp"
#include "http2.hpp"
#include "httpsfv.hpp"

#include <gopp/runtime.hpp>
#include <gopp/strings.hpp>

#include <algorithm>
#include <cstring>
#include <exception>
#include <map>
#include <optional>
#include <unordered_map>

namespace gopp::http {

namespace http2 {

const gopp::error ErrRecursivePush = errors::New("http2: recursive push not allowed");
const gopp::error ErrPushLimitReached = errors::New("http2: push would exceed peer's SETTINGS_MAX_CONCURRENT_STREAMS");

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

// An RFC 9218 priority: the urgency, 0 (most urgent) to 7, and whether the
// response can be used incrementally, as its parts arrive.
struct rfc9218Priority {
    uint8 urgency = 3;
    bool incremental = true;
};

// defaultRFC9218Priority: u=3, non-incremental as RFC 9218 has it, but
// incremental (round-robin, the historical behavior) unless the client has
// shown it knows RFC 9218 and no intermediary is involved.
rfc9218Priority defaultRFC9218Priority(bool canUseDefault) { return {3, !canUseDefault}; }

// parseRFC9218Priority: a Priority header or PRIORITY_UPDATE value
// ("u=1, i").
std::pair<rfc9218Priority, bool> parseRFC9218Priority(std::string_view s, bool canUseDefault) {
    rfc9218Priority p = defaultRFC9218Priority(canUseDefault);
    bool ok = httpsfv::ParseDictionary(s, [&](std::string_view key, std::string_view val, std::string_view) {
        if (key == "u") {
            if (auto [u, uok] = httpsfv::ParseInteger(val); uok && u >= 0 && u <= 7) p.urgency = static_cast<uint8>(u);
        } else if (key == "i") {
            if (auto [i, iok] = httpsfv::ParseBoolean(val); iok) p.incremental = i;
        }
    });
    if (!ok) return {defaultRFC9218Priority(canUseDefault), false};
    return {p, true};
}

// The write scheduler of streams' frames (writesched_priority_rfc9218.go):
// the most urgent first; of one urgency, non-incremental streams one at a
// time to completion, and incremental ones round-robin, alternating
// between the two kinds. Streams are kept in rings, one per urgency and
// kind. With every stream at u=3,i it's a round-robin scheduler.
class writeScheduler {
public:
    void open(uint32 id, rfc9218Priority p) {
        if (id == bufId_) {
            bufId_ = 0;
            p = bufPriority_;
        }
        streams_[id] = node{0, 0, p};
        link(id);
    }

    void close(uint32 id) {
        auto it = streams_.find(id);
        if (it == streams_.end()) return;
        unlink(id);
        streams_.erase(it);
    }

    // A PRIORITY_UPDATE; kept for the stream if it isn't open yet.
    void adjust(uint32 id, rfc9218Priority p) {
        auto it = streams_.find(id);
        if (it == streams_.end()) {
            bufId_ = id;
            bufPriority_ = p;
            return;
        }
        unlink(id);
        it->second.p = p;
        link(id);
    }

    void clear() {
        streams_.clear();
        for (auto& u : heads_) u[0] = u[1] = 0;
        bufId_ = 0;
    }

    // Offers streams, in the order to serve them, to emit(id), which
    // writes one frame of the stream if it can; false if no stream could.
    template <typename F>
    bool pop(F&& emit) {
        // Of one urgency, incremental and non-incremental streams take
        // turns: half the bandwidth to the incremental ones together, half
        // to the first non-incremental one.
        prioritizeIncremental_ = !prioritizeIncremental_;
        for (int u = 0; u < 8; u++) {
            for (int k = 0; k < 2; k++) {
                int i = prioritizeIncremental_ ? (k + 1) % 2 : k;
                uint32 q = heads_[u][i];
                if (!q) continue;
                for (;;) {
                    if (emit(q)) {
                        // Incremental streams round-robin; a non-incremental
                        // one stays first until it has nothing to send, so
                        // that a blocked stream doesn't hold up the others.
                        heads_[u][i] = i == 1 ? streams_[q].next : q;
                        return true;
                    }
                    q = streams_[q].next;
                    if (q == heads_[u][i]) break;
                }
            }
        }
        return false;
    }

private:
    struct node {
        uint32 prev, next;  // in the ring of its urgency and kind
        rfc9218Priority p;
    };

    // Adds id at the end of its ring.
    void link(uint32 id) {
        node& n = streams_[id];
        uint32& head = heads_[n.p.urgency][n.p.incremental];
        if (!head) {
            head = n.prev = n.next = id;
            return;
        }
        node& h = streams_[head];
        n.prev = h.prev;
        n.next = head;
        streams_[n.prev].next = id;
        h.prev = id;
    }

    void unlink(uint32 id) {
        node& n = streams_[id];
        uint32& head = heads_[n.p.urgency][n.p.incremental];
        if (n.next == id) {
            head = 0;  // the only stream
            return;
        }
        streams_[n.prev].next = n.next;
        streams_[n.next].prev = n.prev;
        if (head == id) head = n.next;
    }

    std::unordered_map<uint32, node> streams_;
    uint32 heads_[8][2] = {};  // a stream of each ring, 0 for none
    bool prioritizeIncremental_ = false;
    // The latest PRIORITY_UPDATE for a stream that isn't open yet.
    uint32 bufId_ = 0;
    rfc9218Priority bufPriority_;
};

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
    std::vector<string> trailerKeys;  // the trailers the request declares
    Header trailer;                   // the trailers received
    bool resetQueued = false;   // a RST_STREAM is on its way
    chan<empty> cw;              // closed when the stream closes
    gopp::error closeErr;       // why it closed; set before cw closes
    ptr<time::Timer> readDeadline;
    ptr<time::Timer> writeDeadline;
};

// A write a handler asks for (FrameWriteRequest).
struct writeRequest {
    enum kindT { headers, data, rst, pushPromise };
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
    gopp::error cause;  // why, for the handler's later writes
    // pushPromise: the promised request (and header)
    string method, scheme, authority, path;
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
    // Pusher::Push, once the request is checked.
    gopp::error startPush(writeRequest wr);
    // ResponseController's deadlines: from now on, a deadline of the
    // request body's reads, or of the response's writes (zero for none).
    void setStreamDeadline(const ptr<stream>& st, time::Time t, bool write);
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
    ptr<stream> newStream(uint32 id, streamState st, rfc9218Priority priority);
    gopp::error processPriorityUpdate(const Frame& f);
    void queuePush(writeRequest wr);
    std::pair<uint32, gopp::error> allocatePromisedId(const writeRequest& wr);
    void encodePushPromise(const writeRequest& wr);
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
    uint32 curPushedStreams_ = 0;
    uint32 maxPushPromiseId_ = 0;
    bool pushEnabled_ = true;               // the client's SETTINGS_ENABLE_PUSH
    uint32 clientMaxStreams_ = 0xffffffff;  // the client's SETTINGS_MAX_CONCURRENT_STREAMS
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
    writeScheduler sched_;
    bool priorityAware_ = false;    // the client has sent a Priority header or PRIORITY_UPDATE
    bool hasIntermediary_ = false;  // a request came through a proxy
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
class responseWriter final : public ResponseWriter, public Flusher, public Pusher, public internal::responseControl {
public:
    responseWriter(ptr<serverConn> sc, ptr<stream> st, Request* req) : sc_(std::move(sc)), st_(std::move(st)), req_(req) {}

    gopp::error Push(std::string_view target, const PushOptions* opts) override;

    gopp::error setReadDeadline(time::Time t) override {
        // A deadline past takes effect now, so that the reads after this
        // fail.
        if (!t.IsZero() && t.Before(time::Now())) {
            if (st_->body) st_->body->closeWithError(net::ErrDeadlineExceeded);
            return nullptr;
        }
        sc_->setStreamDeadline(st_, t, false);
        return nullptr;
    }
    gopp::error setWriteDeadline(time::Time t) override {
        // A deadline past resets the stream now: the reset goes before the
        // writes after this, which then fail.
        if (!t.IsZero() && t.Before(time::Now())) {
            writeRequest wr;
            wr.kind = writeRequest::rst;
            wr.st = st_;
            wr.code = ErrCode::internal;
            wr.cause = net::ErrDeadlineExceeded;
            sc_->writeFromHandler(std::move(wr));
            return nullptr;
        }
        sc_->setStreamDeadline(st_, t, true);
        return nullptr;
    }
    // HTTP/2 is always full duplex.
    gopp::error enableFullDuplex() override { return nullptr; }

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

    gopp::error flushError() override {
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

    // A trailer to send, if it may be one.
    void declareTrailer(std::string_view k) {
        string key = CanonicalHeaderKey(k);
        if (!internal::validTrailerHeader(key)) {
            internal::logf("http2: ignoring invalid trailer " + std::string(strconv::Quote(key).view()));
            return;
        }
        if (std::find(trailers_.begin(), trailers_.end(), key) == trailers_.end()) trailers_.push_back(std::move(key));
    }

    // The TrailerPrefix keys, declared as trailers under their own names.
    void promoteUndeclaredTrailers() {
        std::vector<std::pair<string, string>> promoted;
        for (auto& e : handlerHeader_) {
            if (e.key.starts_with(TrailerPrefix)) promoted.emplace_back(string(e.key.view().substr(TrailerPrefix.size())), e.value);
        }
        for (auto& [k, _] : promoted) {
            declareTrailer(k);
            handlerHeader_.Del(k);
        }
        for (auto& [k, v] : promoted) handlerHeader_.Add(k, v);
        std::sort(trailers_.begin(), trailers_.end());
    }

    bool hasNonemptyTrailers() const {
        for (auto& k : trailers_) {
            if (handlerHeader_.Has(k)) return true;
        }
        return false;
    }

    // writeChunk: sends the header with the first chunk, then the data,
    // and the trailers once the handler is done.
    void writeChunk(std::string_view p) {
        if (err_) return;
        if (!wroteHeader_) WriteHeader(StatusOK);
        if (handlerDone_) promoteUndeclaredTrailers();
        bool isHead = req_->Method == "HEAD";
        if (!sentHeader_) {
            sentHeader_ = true;
            for (auto& k : internal::declaredTrailers(snapHeader_)) declareTrailer(k);
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
            bool endStream = (handlerDone_ && trailers_.empty() && p.empty()) || isHead;
            wr.endStream = endStream;
            wr.header = std::move(snapHeader_);
            // The header goes out before any data, as both queue on the
            // stream in order; no need to wait for it.
            if (gopp::error err = sc_->writeFromHandler(std::move(wr))) {
                fail(err);
                return;
            }
            if (endStream) return;
        }
        if (isHead) return;
        if (p.empty() && !handlerDone_) return;
        // Trailers end the stream, if the handler gave them values.
        bool trailers = handlerDone_ && hasNonemptyTrailers();
        bool endStream = handlerDone_ && !trailers;
        if (!p.empty() || endStream) {
            writeRequest wr;
            wr.kind = writeRequest::data;
            wr.st = st_;
            wr.bytes = string(p);
            wr.endStream = endStream;
            if (handlerDone_) {
                // The last chunk: the handler need not wait for it.
                if (gopp::error err = sc_->writeFromHandler(std::move(wr))) fail(err);
            } else if (gopp::error err = sc_->writeAndWait(std::move(wr))) {
                fail(err);
            }
        }
        if (trailers && !err_) {
            writeRequest wr;
            wr.kind = writeRequest::headers;
            wr.st = st_;
            wr.status = 0;
            for (auto& k : trailers_) {
                for (auto& v : handlerHeader_.Values(k)) wr.header.AddCanonical(k, v);
            }
            wr.endStream = true;
            if (gopp::error err = sc_->writeFromHandler(std::move(wr))) fail(err);
        }
    }

    // A write failed: on a closed stream, the error is why it closed
    // (chunkWriter.Write).
    void fail(gopp::error err) {
        if (err == errStreamClosed && st_->closeErr) err = st_->closeErr;
        err_ = std::move(err);
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
    std::vector<string> trailers_;  // declared, sorted once the handler is done
};

gopp::error responseWriter::Push(std::string_view target, const PushOptions* opts) {
    // No pushes from a pushed stream: PUSH_PROMISE goes on streams the
    // client opened.
    if (st_->id % 2 == 0) return ErrRecursivePush;
    PushOptions o = opts ? *opts : PushOptions{};
    if (o.Method.view().empty()) o.Method = string::fromStatic("GET");
    string wantScheme = string::fromStatic(req_->TLS ? "https" : "http");

    auto [u, err] = url::Parse(target);
    if (err) return err;
    if (u.Scheme.view().empty()) {
        if (!target.starts_with("/")) {
            return errors::New("target must be an absolute URL or an absolute path: " + strconv::Quote(target));
        }
        u.Scheme = wantScheme;
        u.Host = req_->Host;
    } else {
        if (u.Scheme != wantScheme) {
            return errors::New("cannot push URL with scheme " + strconv::Quote(u.Scheme) + " from request with scheme " +
                               strconv::Quote(wantScheme));
        }
        if (u.Host.view().empty()) return errors::New("URL must have a host");
    }
    for (auto& e : o.Header) {
        if (e.key.starts_with(":")) return errors::New("promised request headers cannot include pseudo header " + strconv::Quote(e.key));
        // Meaningful only with a body, which promised requests can't have
        // (8.4); and the URL has the host.
        for (std::string_view k : {"content-length", "content-encoding", "trailer", "te", "expect", "host"}) {
            if (ascii::EqualFold(e.key, k)) return errors::New("promised request headers cannot include " + strconv::Quote(e.key));
        }
    }
    for (std::string_view k : {"Connection", "Keep-Alive", "Proxy-Connection", "Transfer-Encoding", "Upgrade"}) {
        if (o.Header.Has(k)) return errors::New("request header " + strconv::Quote(k) + " is not valid in HTTP/2");
    }
    // Promised requests must be cacheable and safe: GET or HEAD (8.4).
    if (o.Method != "GET" && o.Method != "HEAD") return errors::New("method " + strconv::Quote(o.Method) + " must be GET or HEAD");

    writeRequest wr;
    wr.kind = writeRequest::pushPromise;
    wr.st = st_;
    wr.method = o.Method;
    wr.scheme = u.Scheme;
    wr.authority = u.Host;
    wr.path = u.RequestURI();
    wr.header = std::move(o.Header);
    return sc_->startPush(std::move(wr));
}

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

void serverConn::setStreamDeadline(const ptr<stream>& st, time::Time t, bool write) {
    ptr<serverConn> self(this);
    sendServeMsg([self, st, t, write] {
        // A closed stream's timers are gone.
        auto it = self->streams_.find(st->id);
        if (it == self->streams_.end() || it->second != st) return;
        ptr<time::Timer>& timer = write ? st->writeDeadline : st->readDeadline;
        if (timer) {
            if (!timer->Stop()) return;  // it fired already
            timer = nullptr;
        }
        if (t.IsZero()) return;
        uint32 id = st->id;
        // A read deadline ends the body's reads; a write deadline resets
        // the stream.
        std::function<void()> expire = write ? std::function<void()>([self, id] {
            if (self->streams_.contains(id)) self->resetStream(id, ErrCode::internal, net::ErrDeadlineExceeded);
        })
                                             : std::function<void()>([st] {
                                                   if (st->body) st->body->closeWithError(net::ErrDeadlineExceeded);
                                               });
        time::Duration d = t.Sub(time::Now());
        if (d.ns <= 0) {
            expire();
            return;
        }
        timer = time::AfterFunc(d, [self, expire] { self->sendServeMsg(expire); });
    });
}

gopp::error serverConn::startPush(writeRequest wr) {
    ptr<stream> st = wr.st;
    chan<gopp::error> done = make_chan<gopp::error>(1);
    wr.done = done;
    ptr<serverConn> self(this);
    std::function<void()> fn = [self, wr] { self->queuePush(wr); };
    gopp::error err;
    select(send(serveMsgCh_, std::move(fn), [] {}), recv(doneServing_, [&] { err = errClientDisconnected; }),
           recv(st->cw, [&] { err = errStreamClosed; }));
    if (err) return err;
    select(recv(done, [&](gopp::error e) { err = std::move(e); }), recv(doneServing_, [&] { err = errClientDisconnected; }),
           recv(st->cw, [&] { err = errStreamClosed; }));
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
        // Both schedulers ignore RFC 7540's priorities.
        {SettingID::noRfc7540Priorities, 1},
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
    sched_.clear();
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
    case FrameType::priorityUpdate: return processPriorityUpdate(f);
    default:
        return nullptr;  // unknown frame types are ignored
    }
}

gopp::error serverConn::processPriorityUpdate(const Frame& f) {
    priorityAware_ = true;
    if (opts_.DisableClientPriority) return nullptr;
    auto [p, ok] = parseRFC9218Priority(f.data, priorityAware_);
    if (!ok) return streamError(f.promiseId, ErrCode::protocol);
    sched_.adjust(f.promiseId, p);
    return nullptr;
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
    case SettingID::enablePush: pushEnabled_ = s.val != 0; break;
    case SettingID::maxConcurrentStreams: clientMaxStreams_ = s.val; break;
    default: break;  // MAX_HEADER_LIST_SIZE, ...
    }
    return nullptr;
}

std::pair<streamState, ptr<stream>> serverConn::state(uint32 id) {
    if (auto it = streams_.find(id); it != streams_.end()) return {it->second->state, it->second};
    // Opening a stream implicitly closes the idle ones below it (5.1.1).
    if (id % 2 == 1 && id <= maxClientStreamId_) return {streamState::closed, nullptr};
    if (id % 2 == 0 && id != 0 && id <= maxPushPromiseId_) return {streamState::closed, nullptr};
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
    } else if (!st.trailer.empty() && st.req) {
        // The handler's read of the end sets the declared trailers
        // (copyTrailersToHandlerRequest).
        Header t;
        for (auto& k : st.trailerKeys) {
            for (auto& v : st.trailer.Values(k)) t.AddCanonical(k, v);
        }
        st.body->closeWithErrorAndCode(io::EOF_, [sink = detail::trailerSink::of(*st.req), t = std::move(t)] { sink->set(t); });
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
    // The response's priority (RFC 9218). A request through an
    // intermediary gets u=3,i, so that the clients behind a proxy share
    // fairly; until a client shows it knows RFC 9218, its streams are
    // incremental, round-robin as before.
    rfc9218Priority priority = defaultRFC9218Priority(priorityAware_ && !hasIntermediary_);
    if (!opts_.DisableClientPriority && !hasIntermediary_) {
        std::string_view header;
        bool aware = priorityAware_, intermediary = false;
        for (auto& hf : f.regularFields()) {
            if (hf.name == "priority") {
                header = hf.value;
                aware = true;
            }
            if (hf.name == "via" || hf.name == "forwarded" || hf.name == "x-forwarded-for") intermediary = true;
        }
        priority = parseRFC9218Priority(header, aware && !intermediary).first;
        hasIntermediary_ = intermediary;
        if (aware) priorityAware_ = true;
    }
    ptr<stream> st = newStream(id, f.streamEnded() ? streamState::halfClosedRemote : streamState::open, priority);
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
    if (!st.trailerKeys.empty()) {
        for (auto& hf : f.regularFields()) {
            string key = CanonicalHeaderKey(hf.name);
            if (!internal::validTrailerHeader(key)) return streamError(st.id, ErrCode::protocol);
            st.trailer.AddCanonical(std::move(key), hf.value);
        }
    }
    endStream(st);
    return nullptr;
}

ptr<stream> serverConn::newStream(uint32 id, streamState state, rfc9218Priority priority) {
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
    // Without client priorities, every stream is u=3,i: round-robin.
    sched_.open(id, opts_.DisableClientPriority ? rfc9218Priority{} : priority);
    if (id % 2 == 0) curPushedStreams_++;
    else curClientStreams_++;
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
    st.trailerKeys = internal::declaredTrailers(req->Header);
    req->Header.Del("Trailer");
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
    // No more pushes.
    pushEnabled_ = false;
    goAway(ErrCode::no);
    return nullptr;
}

void serverConn::closeStream(const ptr<stream>& st, gopp::error err) {
    if (st->state == streamState::idle || st->state == streamState::closed) panic("http2: can't close stream in this state");
    st->state = streamState::closed;
    if (st->id % 2 == 0) curPushedStreams_--;
    else curClientStreams_--;
    streams_.erase(st->id);
    queues_.erase(st->id);
    sched_.close(st->id);
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
        if (streams_.contains(wr.st->id)) resetStream(wr.st->id, wr.code, wr.cause);
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

void serverConn::queuePush(writeRequest wr) {
    // PUSH_PROMISE only on a stream the client opened that is open or
    // half-closed (remote) (6.6).
    if (wr.st->state != streamState::open && wr.st->state != streamState::halfClosedRemote) {
        wr.done.trySend(gopp::error(errStreamClosed));
        return;
    }
    if (!pushEnabled_) {
        wr.done.trySend(gopp::error(ErrNotSupported));
        return;
    }
    queueWrite(std::move(wr));
}

std::pair<uint32, gopp::error> serverConn::allocatePromisedId(const writeRequest& wr) {
    // The client may have turned pushes off since.
    if (!pushEnabled_) return {0, ErrNotSupported};
    if (curPushedStreams_ + 1 > clientMaxStreams_) return {0, ErrPushLimitReached};
    // Server streams have even ids (5.1.1); out of them, no more pushes.
    if (maxPushPromiseId_ + 2 >= 1u << 31) return {0, ErrPushLimitReached};
    maxPushPromiseId_ += 2;
    uint32 promisedId = maxPushPromiseId_;
    // The stream should start reserved (local) and become half-closed
    // (remote) with its HEADERS; it starts half-closed, as Go's does.
    ptr<stream> st = newStream(promisedId, streamState::halfClosedRemote, defaultRFC9218Priority(priorityAware_ && !hasIntermediary_));
    auto req = make_ptr<Request>();
    req->Method = wr.method;
    auto [u, err] = url::ParseRequestURI(wr.path);
    req->URL = std::move(u);
    req->RequestURI = wr.path;
    req->Host = wr.authority;
    req->Header = wr.header;  // a copy: the handler may change it while the promise is written
    req->Proto = string::fromStatic("HTTP/2.0");
    req->ProtoMajor = 2;
    req->ProtoMinor = 0;
    req->RemoteAddr = remoteAddr_;
    req->TLS = tlsState_;
    req->Body = internal::noBody();
    req->Context();
    st->req = req;
    scheduleHandler(st, std::move(req), 0, {});
    return {promisedId, nullptr};
}

void serverConn::encodePushPromise(const writeRequest& wr) {
    hbuf_.clear();
    auto field = [&](std::string_view k, std::string_view v) { henc_.writeField({string(k), string(v)}); };
    field(":method", wr.method);
    field(":scheme", wr.scheme);
    field(":authority", wr.authority);
    field(":path", wr.path);
    // The header, sorted by key, as Go's encodeHeaders writes it.
    std::vector<const Header::entry*> fields;
    for (auto& e : wr.header) fields.push_back(&e);
    std::stable_sort(fields.begin(), fields.end(), [](auto* a, auto* b) { return lowerHeader(a->key) < lowerHeader(b->key); });
    for (auto* e : fields) {
        string k = lowerHeader(e->key);
        if (!validWireHeaderFieldName(k) || !internal::validHeaderFieldValue(e->value)) continue;
        henc_.writeField({k, e->value});
    }
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

    // Stream frames, in the scheduler's order, one at a time, until the
    // batch is full or nothing can be sent.
    if (!inGoAway_ || goAwayCode_ == ErrCode::no) {
        uint32 frameMax = std::min(peerMaxFrameSize_, maxDataFrameSize);
        // Writes one frame of stream id, if it has one it can send.
        auto emit = [&](uint32 id) -> bool {
            auto it = queues_.find(id);
            if (it == queues_.end()) return false;
            auto& q = it->second;
            writeRequest& wr = q.front();
            ptr<stream> st = wr.st;
            bool popped = false;
            if (wr.kind == writeRequest::pushPromise) {
                // The promised stream's id is taken now, so that promises
                // go out in the order of their ids.
                auto [promisedId, err] = allocatePromisedId(wr);
                if (err) {
                    if (wr.done) wr.done.trySend(gopp::error(err));
                } else {
                    encodePushPromise(wr);
                    appendPushPromise(batch_, id, promisedId, hbuf_, peerMaxFrameSize_);
                    inFlight_.push_back({st, false, false, nullptr, std::move(wr.done)});
                }
                popped = true;
            } else if (wr.kind == writeRequest::headers) {
                encodeHeaders(wr);
                bool end = wr.endStream && (wr.status == 0 || wr.status >= 200);  // final or trailers
                appendHeaders(batch_, id, hbuf_, end, peerMaxFrameSize_);
                inFlight_.push_back({st, end, false, nullptr, std::move(wr.done)});
                popped = true;
            } else {
                std::size_t remain = wr.bytes.size() - wr.off;
                int32 avail = st->flow.available();
                if (remain != 0 && avail <= 0) return false;  // blocked by flow control
                std::size_t k = std::min<std::size_t>({remain, static_cast<std::size_t>(std::max(avail, 0)), frameMax});
                bool last = k == remain;
                appendData(batch_, id, last && wr.endStream, wr.bytes.view().substr(wr.off, k));
                st->flow.take(static_cast<int32>(k));
                wr.off += k;
                if (last) {
                    inFlight_.push_back({st, wr.endStream, false, nullptr, std::move(wr.done)});
                    popped = true;
                }
            }
            if (popped) {
                q.pop_front();
                if (q.empty()) queues_.erase(it);
            }
            return true;
        };
        while (batch_.size() < maxWriteBatch && !queues_.empty() && sched_.pop(emit)) {
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
