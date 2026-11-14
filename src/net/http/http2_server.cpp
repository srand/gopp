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
#include <unordered_map>

namespace gopp::http {

namespace http2 {

namespace {

// Defaults of Go's http2.Server.
constexpr uint32 default_max_read_frame_size = 1 << 20;
constexpr uint32 default_max_streams = 250;
constexpr int32 initial_stream_recv_window = 1 << 20;  // MaxReceiveBufferPerStream
constexpr int32 initial_conn_recv_window = 1 << 20;    // MaxReceiveBufferPerConnection
constexpr std::size_t handler_chunk_write_size = 4 << 10;
constexpr std::size_t max_write_batch = 64 << 10;
constexpr uint32 max_data_frame_size = 16 << 10;
const time::Duration preface_timeout = 10 * time::Second;
const time::Duration first_settings_timeout = 2 * time::Second;
const time::Duration go_away_timeout = time::Second;

const gopp::error err_client_disconnected = errors::New("client disconnected");
const gopp::error err_closed_body = errors::New("body closed by handler");
const gopp::error err_handler_complete = errors::New("http2: request body closed due to handler exiting");
const gopp::error err_stream_closed = errors::New("http2: stream closed");
const gopp::error err_too_much_data = errors::New("http2: handler wrote more than declared Content-Length");

enum class stream_state { idle, open, half_closed_local, half_closed_remote, closed };

class server_conn;

struct stream final : object {
    uint32 id = 0;
    stream_state state = stream_state::idle;
    ptr<pipe> body;  // the request body; nil if there is none
    ptr<Request> req;
    int64 decl_body_bytes = -1;  // the declared Content-Length; -1 if none
    int64 body_bytes = 0;        // received so far
    outflow flow;                // what we may send
    inflow in;                   // what the peer may send
    bool got_trailer_header = false;
    bool reset_queued = false;   // a RST_STREAM is on its way
    chan<empty> cw;              // closed when the stream closes
    gopp::error close_err;       // why it closed; set before cw closes
    ptr<time::Timer> read_deadline;
    ptr<time::Timer> write_deadline;
};

// A write a handler asks for (FrameWriteRequest).
struct writeRequest {
    enum kind_t { headers, data, rst };
    kind_t kind = data;
    ptr<stream> st;
    // headers
    int status = 0;  // 0 for trailers
    Header header;
    string content_type, ContentLength, date;
    // data
    string bytes;
    std::size_t off = 0;
    bool endStream = false;
    // rst
    ErrCode code = ErrCode::internal;
    // Receives the result once the frame is written; nil if no one waits.
    chan<gopp::error> done;
};

struct read_frame_result {
    Frame f;
    gopp::error err;
};

struct body_read_msg {
    ptr<stream> st;
    int n;
};

// What to do once a batch is written (wroteFrame).
struct write_effect {
    ptr<stream> st;
    bool ends_stream = false;  // the frame had END_STREAM
    bool rst = false;          // a RST_STREAM: the stream is closed
    gopp::error rst_err;
    chan<gopp::error> done;
};

class server_conn final : public object {
public:
    server_conn(ptr<net::Conn> c, ptr<bufio::Reader> br, ptr<Handler> h, const internal::server_options& opts,
                std::optional<tls::ConnectionState> tls_state)
        : conn_(std::move(c)),
          br_(std::move(br)),
          handler_(std::move(h)),
          opts_(opts),
          tls_state_(std::move(tls_state)),
          remote_addr_(conn_->RemoteAddr() ? conn_->RemoteAddr()->String() : string()),
          framer_(br_),
          hdec_(initial_header_table_size, nullptr),
          henc_(&hbuf_) {
        max_header_list_size_ = static_cast<uint32>(std::min<int64>(
            (opts_.MaxHeaderBytes > 0 ? opts_.MaxHeaderBytes : DefaultMaxHeaderBytes) + 10 * 32, 0xffffffffLL));
        framer_.read_meta_headers = &hdec_;
        framer_.max_header_list_size = max_header_list_size_;
        framer_.setMaxReadFrameSize(default_max_read_frame_size);
        flow_.add(initial_window_size);
        inflow_.init(initial_window_size);
        done_serving_ = makeChan<empty>();
        want_write_ch_ = makeChan<writeRequest>(8);
        body_read_ch_ = makeChan<body_read_msg>(8);
        serve_msg_ch_ = makeChan<std::function<void()>>(8);
        read_frame_ch_ = makeChan<read_frame_result>();
        write_ch_ = makeChan<std::string>(1);
        wrote_ch_ = makeChan<gopp::error>(1);
    }

    void serve();

    // Handler side (any fiber).
    gopp::error writeFromHandler(writeRequest wr);
    gopp::error writeAndWait(writeRequest wr);
    void noteBodyReadFromHandler(const ptr<stream>& st, int n);
    void sendServeMsg(std::function<void()> fn);
    const chan<empty>& done_serving() const noexcept { return done_serving_; }

private:
    // Serve loop.
    bool processFrameFromReader(read_frame_result res);
    gopp::error processFrame(Frame& f);
    gopp::error processSettings(const Frame& f);
    gopp::error processSetting(const Setting& s);
    gopp::error processData(Frame& f);
    gopp::error processHeaders(Frame& f);
    gopp::error processTrailerHeaders(stream& st, const Frame& f);
    gopp::error processWindowUpdate(const Frame& f);
    gopp::error processResetStream(const Frame& f);
    gopp::error processGoaway(const Frame& f);
    std::pair<stream_state, ptr<stream>> state(uint32 id);
    ptr<stream> newStream(uint32 id, stream_state st);
    gopp::error NewRequest(stream& st, const Frame& f, ptr<Request>& out);
    void scheduleHandler(ptr<stream> st, ptr<Request> req, int status_override, string msg);
    void closeStream(const ptr<stream>& st, gopp::error err);
    void releaseStream(stream& st);
    void endStream(stream& st);
    void resetStream(uint32 id, ErrCode code, gopp::error cause = nullptr);
    void goAway(ErrCode code);
    void sendWindowUpdate(stream* st, int32 n);
    void noteBodyRead(const ptr<stream>& st, int n);
    void queueWrite(writeRequest wr);
    void scheduleFrameWrite();
    void encode_headers(const writeRequest& wr);
    void wroteBatch(gopp::error err);
    void setActive(bool active);
    void resetIdleTimer();

    ptr<net::Conn> conn_;
    ptr<bufio::Reader> br_;
    ptr<Handler> handler_;
    internal::server_options opts_;
    std::optional<tls::ConnectionState> tls_state_;
    string remote_addr_;
    Framer framer_;
    hpack::Decoder hdec_;
    std::string hbuf_;  // the HPACK encoder's output
    hpack::Encoder henc_;

    chan<empty> done_serving_;
    chan<writeRequest> want_write_ch_;
    chan<body_read_msg> body_read_ch_;
    chan<std::function<void()>> serve_msg_ch_;
    chan<read_frame_result> read_frame_ch_;
    chan<std::string> write_ch_;
    chan<gopp::error> wrote_ch_;

    // State owned by the serve loop.
    std::unordered_map<uint32, ptr<stream>> streams_;
    uint32 max_client_stream_id_ = 0;
    uint32 adv_max_streams_ = default_max_streams;
    uint32 cur_client_streams_ = 0;
    int unacked_settings_ = 0;
    bool saw_first_settings_ = false;
    bool need_to_send_settings_ack_ = false;
    outflow flow_;  // the connection's send window
    inflow inflow_;  // the connection's receive window
    int32 initial_stream_send_window_ = initial_window_size;
    uint32 peer_max_frame_size_ = initial_max_frame_size;
    uint32 max_header_list_size_;
    bool in_go_away_ = false;
    bool need_to_send_go_away_ = false;
    ErrCode go_away_code_ = ErrCode::no;
    ptr<time::Timer> shutdown_timer_;
    ptr<time::Timer> idle_timer_;
    bool active_ = false;
    bool exit_ = false;

    // The write scheduler: control frames first, then the streams' queues
    // in turn (Go's round-robin scheduler).
    std::string control_;
    std::vector<write_effect> control_effects_;
    std::map<uint32, std::deque<writeRequest>> queues_;
    uint32 next_queue_ = 0;  // round-robin position
    bool writing_ = false;
    std::vector<write_effect> in_flight_;
    std::string batch_;
};

// The request body as the handler reads it (requestBody).
class request_body final : public io::ReadCloser {
public:
    request_body(ptr<server_conn> sc, ptr<stream> st, ptr<pipe> p, bool needs_continue)
        : sc_(std::move(sc)), st_(std::move(st)), pipe_(std::move(p)), needs_continue_(needs_continue) {}

    std::pair<isize, gopp::error> Read(std::span<byte> p) override {
        if (needs_continue_) {
            needs_continue_ = false;
            writeRequest wr;
            wr.kind = writeRequest::headers;
            wr.st = st_;
            wr.status = 100;
            sc_->writeFromHandler(std::move(wr));
        }
        if (!pipe_ || saw_eof_) return {0, io::EOF_};
        auto [n, err] = pipe_->Read(p);
        if (err == io::EOF_) saw_eof_ = true;
        if (n > 0) sc_->noteBodyReadFromHandler(st_, static_cast<int>(n));
        return {n, err};
    }

    gopp::error Close() override {
        if (pipe_ && !closed_) pipe_->breakWithError(err_closed_body);
        closed_ = true;
        return nullptr;
    }

private:
    ptr<server_conn> sc_;
    ptr<stream> st_;
    ptr<pipe> pipe_;
    bool needs_continue_;
    bool saw_eof_ = false;
    bool closed_ = false;
};

bool bodyAllowedForStatus(int status) {
    if (status >= 100 && status <= 199) return false;
    return status != 204 && status != 304;
}

// The ResponseWriter of a stream (responseWriter and responseWriterState).
// Writes collect in a 4 KiB buffer; a full buffer, flush() or the end of
// the handler sends it as a chunk: the header first, then DATA.
class response_writer final : public ResponseWriter, public Flusher {
public:
    response_writer(ptr<server_conn> sc, ptr<stream> st, Request* req) : sc_(std::move(sc)), st_(std::move(st)), req_(req) {}

    http::Header& Header() override { return handler_header_; }

    void WriteHeader(int code) override {
        if (wrote_header_) {
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
            wr.header = handler_header_;
            sc_->writeFromHandler(std::move(wr));
            return;
        }
        wrote_header_ = true;
        status_ = code;
        snap_header_ = handler_header_;
    }

    std::pair<isize, gopp::error> Write(std::span<const byte> p) override {
        if (!wrote_header_) WriteHeader(StatusOK);
        if (!bodyAllowedForStatus(status_)) return {0, ErrBodyNotAllowed};
        wrote_bytes_ += static_cast<int64>(p.size());
        if (sent_content_len_ != 0 && declared_len_ >= 0 && wrote_bytes_ > declared_len_) return {0, err_too_much_data};
        isize total = static_cast<isize>(p.size());
        // bufio.Writer.Write
        while (p.size() > handler_chunk_write_size - buf_.size() && !err_) {
            std::size_t k;
            if (buf_.empty()) {
                // Large write, empty buffer: write directly.
                k = p.size();
                write_chunk(asStringView(p));
            } else {
                k = handler_chunk_write_size - buf_.size();
                buf_.append(asStringView(p.first(k)));
                flush_buf();
            }
            p = p.subspan(k);
        }
        if (err_) return {total - static_cast<isize>(p.size()), err_};
        buf_.append(asStringView(p));
        return {total, nullptr};
    }

    void Flush() override { flush_error(); }

    gopp::error flush_error() {
        if (!buf_.empty()) {
            flush_buf();
        } else {
            // The header and/or the final DATA frame must go out even
            // without data.
            write_chunk({});
            if (!err_) {
                bool closed = false;
                st_->cw.tryRecv(*new_empty_slot(), &closed);
                if (closed) err_ = st_->close_err;
            }
        }
        return err_;
    }

    // The handler returned.
    void handler_done() {
        handler_done_ = true;
        flush_error();
    }

private:
    static empty* new_empty_slot() {
        thread_local empty e;
        return &e;
    }

    void flush_buf() {
        std::string b = std::move(buf_);
        buf_.clear();
        write_chunk(b);
    }

    // writeChunk: sends the header with the first chunk, then the data.
    void write_chunk(std::string_view p) {
        if (err_) return;
        if (!wrote_header_) WriteHeader(StatusOK);
        bool is_head = req_->Method == "HEAD";
        if (!sent_header_) {
            sent_header_ = true;
            writeRequest wr;
            wr.kind = writeRequest::headers;
            wr.st = st_;
            wr.status = status_;
            string clen = snap_header_.Get("Content-Length");
            if (!clen.empty()) {
                snap_header_.Del("Content-Length");
                auto [cl, err] = strconv::ParseInt(clen, 10, 63);
                if (!err && cl >= 0) {
                    declared_len_ = cl;
                    sent_content_len_ = cl;
                } else {
                    clen = {};
                }
            }
            if (clen.empty() && handler_done_ && bodyAllowedForStatus(status_) && (!p.empty() || !is_head)) {
                clen = strconv::Itoa(static_cast<int64>(p.size()));
            }
            wr.ContentLength = clen;
            bool has_ct = snap_header_.Has("Content-Type");
            bool has_ce = !snap_header_.Get("Content-Encoding").empty();
            if (!has_ce && !has_ct && bodyAllowedForStatus(status_) && !p.empty()) {
                wr.content_type = DetectContentType(std::span<const byte>(reinterpret_cast<const byte*>(p.data()), p.size()));
            }
            if (!snap_header_.Has("Date")) {
                std::string d;
                internal::appendDate(d);
                wr.date = string(d);
            }
            if (snap_header_.Has("Connection")) {
                string v = snap_header_.Get("Connection");
                snap_header_.Del("Connection");
                if (v == "close") sc_->sendServeMsg(nullptr);  // graceful shutdown
            }
            bool endStream = (handler_done_ && p.empty()) || is_head;
            wr.endStream = endStream;
            wr.header = std::move(snap_header_);
            // The header goes out before any data, as both queue on the
            // stream in order; no need to wait for it.
            if (gopp::error err = sc_->writeFromHandler(std::move(wr))) {
                err_ = err;
                return;
            }
            if (endStream) return;
        }
        if (is_head) return;
        if (p.empty() && !handler_done_) return;
        writeRequest wr;
        wr.kind = writeRequest::data;
        wr.st = st_;
        wr.bytes = string(p);
        wr.endStream = handler_done_;
        if (handler_done_) {
            // The last chunk: the handler need not wait for it.
            if (gopp::error err = sc_->writeFromHandler(std::move(wr))) err_ = err;
            return;
        }
        if (gopp::error err = sc_->writeAndWait(std::move(wr))) err_ = err;
    }

    ptr<server_conn> sc_;
    ptr<stream> st_;
    Request* req_;
    http::Header handler_header_;
    http::Header snap_header_;
    int status_ = 0;
    bool wrote_header_ = false;
    bool sent_header_ = false;
    bool handler_done_ = false;
    int64 wrote_bytes_ = 0;
    int64 declared_len_ = -1;
    int64 sent_content_len_ = -1;
    std::string buf_;
    gopp::error err_;
};

// ---------------------------------------------------------------------------
// Handler side

gopp::error server_conn::writeFromHandler(writeRequest wr) {
    gopp::error err;
    select(send(want_write_ch_, std::move(wr), [] {}), recv(done_serving_, [&] { err = err_client_disconnected; }));
    return err;
}

gopp::error server_conn::writeAndWait(writeRequest wr) {
    ptr<stream> st = wr.st;
    chan<gopp::error> done = makeChan<gopp::error>(1);
    wr.done = done;
    if (gopp::error err = writeFromHandler(std::move(wr))) return err;
    gopp::error err;
    select(recv(done, [&](gopp::error e) { err = std::move(e); }),
           recv(done_serving_, [&] { err = err_client_disconnected; }),
           recv(st->cw, [&] {
               // The stream closed; the write may have finished anyway.
               gopp::error e;
               if (done.tryRecv(e)) err = std::move(e);
               else err = err_stream_closed;
           }));
    return err;
}

void server_conn::noteBodyReadFromHandler(const ptr<stream>& st, int n) {
    select(send(body_read_ch_, body_read_msg{st, n}, [] {}), recv(done_serving_, [] {}));
}

void server_conn::sendServeMsg(std::function<void()> fn) {
    if (!fn) {
        // A graceful shutdown.
        ptr<server_conn> self(this);
        fn = [self] { self->goAway(ErrCode::no); };
    }
    select(send(serve_msg_ch_, std::move(fn), [] {}), recv(done_serving_, [] {}));
}

// ---------------------------------------------------------------------------
// The serve loop

void server_conn::serve() {
    ptr<server_conn> self(this);
    if (opts_.on_shutdown) {
        opts_.on_shutdown([self] {
            // Runs on the shutting down fiber, under the server's lock.
            std::function<void()> fn = [self] { self->goAway(ErrCode::no); };
            self->serve_msg_ch_.trySend(std::move(fn));
        });
    }

    // Our SETTINGS and the larger connection window go out first.
    Setting settings[] = {
        {SettingID::max_frame_size, default_max_read_frame_size},
        {SettingID::max_concurrent_streams, adv_max_streams_},
        {SettingID::max_header_list_size, max_header_list_size_},
        {SettingID::header_table_size, initial_header_table_size},
        {SettingID::initial_window_size, uint32(initial_stream_recv_window)},
    };
    appendSettings(control_, settings);
    unacked_settings_++;
    if (int32 diff = initial_conn_recv_window - int32(initial_window_size); diff > 0) sendWindowUpdate(nullptr, diff);

    // The writer.
    go([self] {
        for (;;) {
            auto [buf, ok] = self->write_ch_.recv2();
            if (!ok) return;
            auto [n, err] = io::WriteString(*self->conn_, buf);
            self->wrote_ch_.send(err);
        }
    });
    scheduleFrameWrite();

    // The client preface, which an h2c server peeked at already.
    conn_->SetReadDeadline(time::Now().Add(preface_timeout));
    byte preface[24];
    if (auto [n, err] = io::ReadFull(*br_, std::span<byte>(preface, sizeof(preface))); err) {
        if (err != io::EOF_ && !errors::Is(err, net::ErrClosed)) {
            internal::logf("http2: server: error reading preface from client " + std::string(remote_addr_.view()) + ": " +
                           std::string(err.Error().view()));
        }
        exit_ = true;
    } else if (asStringView(std::span<const byte>(preface, sizeof(preface))) != client_preface) {
        internal::logf("http2: server: error reading preface from client " + std::string(remote_addr_.view()) +
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
                select(send(self->read_frame_ch_, read_frame_result{std::move(f), err}, [&] { sent = true; }),
                       recv(self->done_serving_, [] {}));
                if (!sent || terminal) return;
            }
        });
    }

    auto settings_timer = time::AfterFunc(first_settings_timeout, [self] {
        self->sendServeMsg([self] {
            if (!self->saw_first_settings_) self->exit_ = true;
        });
    });
    resetIdleTimer();

    while (!exit_) {
        select(recv(want_write_ch_, [&](writeRequest wr) { queueWrite(std::move(wr)); }),
               recv(wrote_ch_, [&](gopp::error err) { wroteBatch(std::move(err)); }),
               recv(read_frame_ch_, [&](read_frame_result res) {
                   // Finish a write first: it may have closed a stream.
                   gopp::error werr;
                   if (writing_ && wrote_ch_.tryRecv(werr)) wroteBatch(std::move(werr));
                   if (!processFrameFromReader(std::move(res))) exit_ = true;
               }),
               recv(body_read_ch_, [&](body_read_msg m) { noteBodyRead(m.st, m.n); }),
               recv(serve_msg_ch_, [&](std::function<void()> fn) {
                   if (fn) fn();
               }));
        if (exit_) break;
        scheduleFrameWrite();

        // After GOAWAY: close once the streams are done (or right away
        // for an error), giving the peer a moment to read it.
        bool sent_go_away = in_go_away_ && !need_to_send_go_away_ && !writing_;
        bool graceful_done = go_away_code_ == ErrCode::no && streams_.empty();
        if (sent_go_away && !shutdown_timer_ && (go_away_code_ != ErrCode::no || graceful_done)) {
            shutdown_timer_ = time::AfterFunc(go_away_timeout, [self] {
                self->sendServeMsg([self] { self->exit_ = true; });
            });
        }
    }

    // The timers' functions refer to this connection: drop them.
    settings_timer->Stop();
    if (idle_timer_) idle_timer_->Stop();
    if (shutdown_timer_) shutdown_timer_->Stop();
    idle_timer_ = nullptr;
    shutdown_timer_ = nullptr;
    if (opts_.on_shutdown) opts_.on_shutdown(nullptr);
    done_serving_.close();
    conn_->Close();
    write_ch_.close();
    // Close the streams, so that their handlers see the connection end.
    auto streams = std::move(streams_);
    for (auto& [id, st] : streams) {
        st->state = stream_state::closed;
        if (st->body) st->body->closeWithError(err_client_disconnected);
        st->close_err = err_client_disconnected;
        releaseStream(*st);
        st->cw.close();
    }
    // Free the queued writes; their handlers see done_serving.
    queues_.clear();
    control_effects_.clear();
    in_flight_.clear();
}

bool server_conn::processFrameFromReader(read_frame_result res) {
    gopp::error err = std::move(res.err);
    if (err) {
        if (err == err_frame_too_large) {
            goAway(ErrCode::frame_size);
            return true;
        }
        bool client_gone = err == io::EOF_ || err == io::ErrUnexpectedEOF || errors::Is(err, net::ErrClosed) ||
                           errors::As<net::OpError>(err) || errors::As<net::Errno>(err);
        if (client_gone) return false;
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
        internal::logf("http2: server connection error from " + std::string(remote_addr_.view()) + ": " + why);
        goAway(ce->code);
        return true;
    }
    internal::logf("http2: server closing client connection: " + std::string(err.Error().view()));
    return false;
}

gopp::error server_conn::processFrame(Frame& f) {
    // The first frame must be SETTINGS.
    if (!saw_first_settings_) {
        if (f.type() != FrameType::settings) return connectionError(ErrCode::protocol, "first frame was not SETTINGS");
        saw_first_settings_ = true;
    }
    // After a GOAWAY, ignore new streams (and everything after an error),
    // but keep the connection's flow control right.
    if (in_go_away_ && (go_away_code_ != ErrCode::no || f.streamId() > max_client_stream_id_)) {
        if (f.type() == FrameType::data) {
            if (!inflow_.take(f.header.length)) return connectionError(ErrCode::flow_control);
            sendWindowUpdate(nullptr, static_cast<int32>(f.header.length));
        }
        return nullptr;
    }
    switch (f.type()) {
    case FrameType::settings: return processSettings(f);
    case FrameType::headers: return processHeaders(f);
    case FrameType::window_update: return processWindowUpdate(f);
    case FrameType::ping:
        if (!f.isAck()) {
            if (in_go_away_ && go_away_code_ != ErrCode::no) return nullptr;
            appendPing(control_, true, f.data);
        }
        return nullptr;
    case FrameType::data: return processData(f);
    case FrameType::rst_stream: return processResetStream(f);
    case FrameType::priority:
        if (f.streamId() == f.priority.stream_dep) return streamError(f.streamId(), ErrCode::protocol);
        return nullptr;
    case FrameType::goaway: return processGoaway(f);
    case FrameType::push_promise:
        // A client must not push.
        return connectionError(ErrCode::protocol, "client sent PUSH_PROMISE");
    default:
        return nullptr;  // unknown frame types are ignored
    }
}

gopp::error server_conn::processSettings(const Frame& f) {
    if (f.isAck()) {
        unacked_settings_--;
        if (unacked_settings_ < 0) return connectionError(ErrCode::protocol, "unexpected SETTINGS ACK");
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
    need_to_send_settings_ack_ = true;
    return nullptr;
}

gopp::error server_conn::processSetting(const Setting& s) {
    if (gopp::error err = s.Valid()) return err;
    switch (s.id) {
    case SettingID::header_table_size: henc_.setMaxDynamicTableSize(s.val); break;
    case SettingID::initial_window_size: {
        // Applies to all open streams, by the difference (6.9.2).
        int32 growth = static_cast<int32>(s.val) - initial_stream_send_window_;
        initial_stream_send_window_ = static_cast<int32>(s.val);
        for (auto& [id, st] : streams_) {
            if (!st->flow.add(growth)) return connectionError(ErrCode::flow_control);
        }
        break;
    }
    case SettingID::max_frame_size: peer_max_frame_size_ = s.val; break;
    default: break;  // ENABLE_PUSH, MAX_CONCURRENT_STREAMS (for pushes), MAX_HEADER_LIST_SIZE, ...
    }
    return nullptr;
}

std::pair<stream_state, ptr<stream>> server_conn::state(uint32 id) {
    if (auto it = streams_.find(id); it != streams_.end()) return {it->second->state, it->second};
    // Opening a stream implicitly closes the idle ones below it (5.1.1).
    if (id % 2 == 1 && id <= max_client_stream_id_) return {stream_state::closed, nullptr};
    if (id % 2 == 0 && id != 0) return {stream_state::closed, nullptr};  // we never push
    return {stream_state::idle, nullptr};
}

gopp::error server_conn::processData(Frame& f) {
    if (in_go_away_ && go_away_code_ != ErrCode::no) return nullptr;
    uint32 id = f.streamId();
    auto [state, st] = this->state(id);
    if (id == 0 || state == stream_state::idle) return connectionError(ErrCode::protocol, "DATA on an idle stream");
    if (!st || state != stream_state::open || st->got_trailer_header || st->reset_queued) {
        // Not for an open stream: keep the connection's flow control
        // right, and tell the peer to stop.
        if (!inflow_.take(f.header.length)) return streamError(id, ErrCode::flow_control);
        sendWindowUpdate(nullptr, static_cast<int32>(f.header.length));
        if (st && st->reset_queued) return nullptr;
        return streamError(id, ErrCode::stream_closed);
    }
    if (!st->body) panic("http2: internal error: should have a body in this state");
    if (st->decl_body_bytes != -1 && st->body_bytes + static_cast<int64>(f.data.size()) > st->decl_body_bytes) {
        if (!inflow_.take(f.header.length)) return streamError(id, ErrCode::flow_control);
        sendWindowUpdate(nullptr, static_cast<int32>(f.header.length));
        st->body->closeWithError(errors::New(std::format("sender tried to send more than declared Content-Length of {} bytes",
                                                           st->decl_body_bytes)));
        return streamError(id, ErrCode::protocol);
    }
    if (f.header.length > 0) {
        if (!takeInflows(inflow_, st->in, f.header.length)) return streamError(id, ErrCode::flow_control);
        std::size_t len = f.data.size();
        if (len > 0) {
            st->body_bytes += static_cast<int64>(len);
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

void server_conn::endStream(stream& st) {
    if (st.decl_body_bytes != -1 && st.decl_body_bytes != st.body_bytes) {
        st.body->closeWithError(errors::New(std::format("request declared a Content-Length of {} but only wrote {} bytes",
                                                          st.decl_body_bytes, st.body_bytes)));
    } else {
        st.body->closeWithError(io::EOF_);
    }
    st.state = stream_state::half_closed_remote;
}

gopp::error server_conn::processHeaders(Frame& f) {
    uint32 id = f.streamId();
    // Clients use odd stream ids (5.1.1).
    if (id % 2 != 1) return connectionError(ErrCode::protocol, "HEADERS on an even stream");
    if (auto it = streams_.find(id); it != streams_.end()) {
        stream& st = *it->second;
        if (st.reset_queued) return nullptr;
        if (st.state == stream_state::half_closed_remote) return streamError(id, ErrCode::stream_closed);
        return processTrailerHeaders(st, f);
    }
    // New streams must have increasing ids (5.1.1).
    if (id <= max_client_stream_id_) return connectionError(ErrCode::protocol, "HEADERS on a closed stream");
    max_client_stream_id_ = id;
    if (idle_timer_) idle_timer_->Stop();
    if (cur_client_streams_ + 1 > adv_max_streams_) {
        if (unacked_settings_ == 0) return streamError(id, ErrCode::protocol);  // they should know better
        return streamError(id, ErrCode::refused_stream);                        // a race with our SETTINGS
    }
    ptr<stream> st = newStream(id, f.streamEnded() ? stream_state::half_closed_remote : stream_state::open);
    if (f.hasPriority() && f.priority.stream_dep == id) return streamError(id, ErrCode::protocol);
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
        ptr<server_conn> self(this);
        ptr<stream> s = st;
        st->read_deadline = time::AfterFunc(opts_.ReadTimeout, [self, s] {
            self->sendServeMsg([s] {
                if (s->body) s->body->closeWithError(net::ErrDeadlineExceeded);
            });
        });
    }
    scheduleHandler(st, std::move(req), status, std::move(msg));
    return nullptr;
}

gopp::error server_conn::processTrailerHeaders(stream& st, const Frame& f) {
    if (st.got_trailer_header) return connectionError(ErrCode::protocol, "second trailer");
    st.got_trailer_header = true;
    if (!f.streamEnded()) return streamError(st.id, ErrCode::protocol);
    if (!f.pseudoFields().empty()) return streamError(st.id, ErrCode::protocol);
    endStream(st);
    return nullptr;
}

ptr<stream> server_conn::newStream(uint32 id, stream_state state) {
    auto st = make_ptr<stream>();
    st->id = id;
    st->state = state;
    st->cw = makeChan<empty>();
    st->flow.conn = &flow_;
    st->flow.add(initial_stream_send_window_);
    st->in.init(initial_stream_recv_window);
    if (opts_.WriteTimeout.ns > 0) {
        ptr<server_conn> self(this);
        st->write_deadline = time::AfterFunc(opts_.WriteTimeout, [self, id] {
            self->sendServeMsg([self, id] {
                if (self->streams_.contains(id)) self->resetStream(id, ErrCode::internal, net::ErrDeadlineExceeded);
            });
        });
    }
    streams_[id] = st;
    cur_client_streams_++;
    if (streams_.size() == 1) setActive(true);
    return st;
}

gopp::error server_conn::NewRequest(stream& st, const Frame& f, ptr<Request>& out) {
    string method = f.pseudoValue("method");
    string scheme = f.pseudoValue("scheme");
    string authority = f.pseudoValue("authority");
    string path = f.pseudoValue("path");
    bool is_connect = method == "CONNECT";
    if (!f.pseudoValue("protocol").empty()) return streamError(st.id, ErrCode::protocol);  // no extended CONNECT
    if (is_connect) {
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

    if (is_connect) {
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
    req->RemoteAddr = remote_addr_;
    req->TLS = tls_state_;

    bool body_open = !f.streamEnded();
    auto cls = req->Header.Values("Content-Length");
    if (!cls.empty()) {
        auto [cl, err] = strconv::ParseInt(cls[0], 10, 63);
        req->ContentLength = !err && cl >= 0 ? cl : 0;
        if (!body_open && req->ContentLength != 0) return streamError(st.id, ErrCode::protocol);
        for (std::size_t i = 1; i < cls.size(); i++) {
            if (cls[i] != cls[0]) return streamError(st.id, ErrCode::protocol);
        }
        if (cls.size() > 1) req->Header.Set("Content-Length", cls[0]);
    }
    ptr<server_conn> self(this);
    ptr<stream> sp(&st);
    if (body_open) {
        if (cls.empty()) req->ContentLength = -1;
        st.body = make_ptr<pipe>();
        st.decl_body_bytes = req->ContentLength;
        bool needs_continue = ascii::EqualFold(req->Header.Get("Expect"), "100-continue");
        if (needs_continue) req->Header.Del("Expect");
        req->Body = make_ptr<request_body>(self, sp, st.body, needs_continue);
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

void server_conn::scheduleHandler(ptr<stream> st, ptr<Request> req, int status_override, string msg) {
    ptr<server_conn> self(this);
    ptr<Handler> h = handler_;
    go([self, st, req, h, status_override, msg] {
        response_writer rw(self, st, req.get());
        try {
            if (status_override) {
                http::Error(rw, msg, status_override);
            } else {
                h->ServeHTTP(rw, *req);
            }
        } catch (const std::exception& e) {
            internal::logf("http2: panic serving " + std::string(self->remote_addr_.view()) + ": " + e.what());
            req->CancelContext();
            writeRequest wr;
            wr.kind = writeRequest::rst;
            wr.st = st;
            wr.code = ErrCode::internal;
            self->writeFromHandler(std::move(wr));
            return;
        }
        req->CancelContext();
        rw.handler_done();
    });
}

gopp::error server_conn::processWindowUpdate(const Frame& f) {
    if (f.streamId() != 0) {
        auto [state, st] = this->state(f.streamId());
        if (state == stream_state::idle) return connectionError(ErrCode::protocol, "WINDOW_UPDATE on an idle stream");
        if (!st) return nullptr;  // closed: ignore
        if (!st->flow.add(static_cast<int32>(f.increment))) return streamError(f.streamId(), ErrCode::flow_control);
    } else if (!flow_.add(static_cast<int32>(f.increment))) {
        return connectionError(ErrCode::flow_control, "connection window overflow");
    }
    return nullptr;
}

gopp::error server_conn::processResetStream(const Frame& f) {
    auto [state, st] = this->state(f.streamId());
    if (state == stream_state::idle) return connectionError(ErrCode::protocol, "RST_STREAM on an idle stream");
    if (st) closeStream(st, streamError(f.streamId(), f.err_code));
    return nullptr;
}

gopp::error server_conn::processGoaway(const Frame& f) {
    if (f.err_code != ErrCode::no) {
        internal::logf("http2: received GOAWAY " + std::string(to_string(f.err_code).view()) + ", starting graceful shutdown");
    }
    goAway(ErrCode::no);
    return nullptr;
}

void server_conn::closeStream(const ptr<stream>& st, gopp::error err) {
    if (st->state == stream_state::idle || st->state == stream_state::closed) panic("http2: can't close stream in this state");
    st->state = stream_state::closed;
    cur_client_streams_--;
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
    if (auto* se = errors::As<StreamError>(err)) err = se->cause ? se->cause : err_stream_closed;
    st->close_err = err;
    releaseStream(*st);
    st->cw.close();
}

// Drops what a closed stream refers to: its request (whose body refers back
// to the stream) and its deadline timers (whose functions do too).
void server_conn::releaseStream(stream& st) {
    if (st.read_deadline) {
        st.read_deadline->Stop();
        st.read_deadline = nullptr;
    }
    if (st.write_deadline) {
        st.write_deadline->Stop();
        st.write_deadline = nullptr;
    }
    if (st.req) {
        st.req->CancelContext();
        st.req = nullptr;
    }
}

void server_conn::resetStream(uint32 id, ErrCode code, gopp::error cause) {
    appendRstStream(control_, id, code);
    auto it = streams_.find(id);
    write_effect e;
    e.rst = true;
    e.rst_err = streamError(id, code, std::move(cause));
    if (it != streams_.end()) {
        it->second->reset_queued = true;
        e.st = it->second;
    }
    control_effects_.push_back(std::move(e));
}

void server_conn::goAway(ErrCode code) {
    if (in_go_away_) {
        if (go_away_code_ == ErrCode::no) go_away_code_ = code;
        return;
    }
    in_go_away_ = true;
    need_to_send_go_away_ = true;
    go_away_code_ = code;
}

void server_conn::sendWindowUpdate(stream* st, int32 n) {
    if (n <= 0) return;
    int32 send = st ? st->in.add(n) : inflow_.add(n);
    if (send == 0) return;
    appendWindowUpdate(control_, st ? st->id : 0, static_cast<uint32>(send));
}

void server_conn::noteBodyRead(const ptr<stream>& st, int n) {
    sendWindowUpdate(nullptr, n);
    if (st->state != stream_state::half_closed_remote && st->state != stream_state::closed) {
        sendWindowUpdate(st.get(), n);
    }
}

void server_conn::setActive(bool active) {
    if (active_ == active) return;
    active_ = active;
    if (opts_.setActive) opts_.setActive(active);
}

void server_conn::resetIdleTimer() {
    if (opts_.IdleTimeout.ns <= 0 || !streams_.empty()) return;
    if (idle_timer_) {
        idle_timer_->Reset(opts_.IdleTimeout);
        return;
    }
    ptr<server_conn> self(this);
    idle_timer_ = time::AfterFunc(opts_.IdleTimeout, [self] {
        self->sendServeMsg([self] {
            if (self->streams_.empty()) self->goAway(ErrCode::no);
        });
    });
}

// ---------------------------------------------------------------------------
// Writing

void server_conn::queueWrite(writeRequest wr) {
    if (wr.kind == writeRequest::rst) {
        if (streams_.contains(wr.st->id)) resetStream(wr.st->id, wr.code);
        return;
    }
    auto it = streams_.find(wr.st->id);
    if (it == streams_.end() || it->second != wr.st || wr.st->state == stream_state::closed ||
        wr.st->state == stream_state::half_closed_local) {
        // The stream is gone: its handler sees cw closed.
        if (wr.done) wr.done.trySend(gopp::error(err_stream_closed));
        return;
    }
    queues_[wr.st->id].push_back(std::move(wr));
}

void server_conn::encode_headers(const writeRequest& wr) {
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
    if (!wr.content_type.empty()) field("content-type", wr.content_type);
    if (!wr.ContentLength.empty()) field("content-length", wr.ContentLength);
    if (!wr.date.empty()) field("date", wr.date);
}

void server_conn::scheduleFrameWrite() {
    if (writing_ || exit_) return;
    batch_.clear();
    in_flight_.clear();
    if (need_to_send_go_away_) {
        need_to_send_go_away_ = false;
        appendGoaway(batch_, max_client_stream_id_, go_away_code_);
    }
    if (need_to_send_settings_ack_) {
        need_to_send_settings_ack_ = false;
        appendSettingsAck(batch_);
    }
    batch_ += control_;
    control_.clear();
    for (auto& e : control_effects_) in_flight_.push_back(std::move(e));
    control_effects_.clear();

    // Stream frames, one per stream per round, until the batch is full
    // or nothing can be sent.
    if (!in_go_away_ || go_away_code_ == ErrCode::no) {
        uint32 frame_max = std::min(peer_max_frame_size_, max_data_frame_size);
        bool progress = true;
        while (progress && batch_.size() < max_write_batch && !queues_.empty()) {
            progress = false;
            auto it = queues_.upper_bound(next_queue_);
            for (std::size_t visited = 0, n = queues_.size(); visited < n && batch_.size() < max_write_batch; visited++) {
                if (it == queues_.end()) it = queues_.begin();
                auto& [id, q] = *it;
                writeRequest& wr = q.front();
                ptr<stream> st = wr.st;
                bool popped = false;
                if (wr.kind == writeRequest::headers) {
                    encode_headers(wr);
                    bool end = wr.endStream && wr.status >= 200;
                    appendHeaders(batch_, id, hbuf_, end, peer_max_frame_size_);
                    in_flight_.push_back({st, end, false, nullptr, std::move(wr.done)});
                    popped = true;
                } else {
                    std::size_t remain = wr.bytes.size() - wr.off;
                    int32 avail = st->flow.available();
                    if (remain == 0 || avail > 0) {
                        std::size_t k = std::min<std::size_t>({remain, static_cast<std::size_t>(std::max(avail, 0)), frame_max});
                        bool last = k == remain;
                        appendData(batch_, id, last && wr.endStream, wr.bytes.view().substr(wr.off, k));
                        st->flow.take(static_cast<int32>(k));
                        wr.off += k;
                        if (last) {
                            in_flight_.push_back({st, wr.endStream, false, nullptr, std::move(wr.done)});
                            popped = true;
                        }
                        progress = true;
                    }
                }
                if (popped) {
                    progress = true;
                    q.pop_front();
                    next_queue_ = id;
                    if (q.empty()) {
                        it = queues_.erase(it);
                        continue;
                    }
                }
                next_queue_ = id;
                ++it;
            }
        }
    }
    if (batch_.empty()) return;
    writing_ = true;
    write_ch_.send(std::move(batch_));
    batch_.clear();
}

void server_conn::wroteBatch(gopp::error err) {
    writing_ = false;
    if (err) conn_->Close();
    auto effects = std::move(in_flight_);
    in_flight_.clear();
    for (auto& e : effects) {
        if (e.ends_stream && e.st) {
            stream& st = *e.st;
            if (st.state == stream_state::open) {
                // The handler is done but the client is still sending:
                // tell it to stop (as Go does, with NO_ERROR).
                st.state = stream_state::half_closed_local;
                resetStream(st.id, ErrCode::no);
            } else if (st.state == stream_state::half_closed_remote) {
                closeStream(e.st, err_handler_complete);
            }
        } else if (e.rst && e.st && e.st->state != stream_state::closed) {
            closeStream(e.st, e.rst_err);
        }
        if (e.done) e.done.trySend(gopp::error(err));
    }
}

}  // namespace

}  // namespace http2

void internal::serveHttp2(const ptr<net::Conn>& c, const ptr<bufio::Reader>& br, const ptr<Handler>& handler,
                           const server_options& opts, std::optional<tls::ConnectionState> tls_state) {
    auto sc = make_ptr<http2::server_conn>(c, br, handler, opts, std::move(tls_state));
    sc->serve();
}

}  // namespace gopp::http
