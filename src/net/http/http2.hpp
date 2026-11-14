// HTTP/2 framing, errors, flow control and body pipes, shared by the
// server and the client. After Go's net/http/internal/http2: frame.Go,
// errors.Go, flow.Go, pipe.Go and http2.Go.
#pragma once

#include "hpack.hpp"
#include "internal.hpp"

#include <gopp/chan.hpp>
#include <gopp/sync.hpp>

#include <deque>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace gopp::http::http2 {

// The client connection preface.
inline constexpr std::string_view client_preface = internal::http2_preface;

// RFC 9113 6.5.2 initial values.
inline constexpr uint32 initial_window_size = 65535;
inline constexpr uint32 initial_max_frame_size = 16384;
inline constexpr uint32 initial_header_table_size = 4096;
inline constexpr uint32 max_frame_size_limit = (1 << 24) - 1;
inline constexpr int32 max_window_size = 0x7fffffff;
inline constexpr std::size_t frame_header_len = 9;

// ---------------------------------------------------------------------------
// Frame types, flags, settings

enum class FrameType : uint8 {
    data = 0x0,
    headers = 0x1,
    priority = 0x2,
    rst_stream = 0x3,
    settings = 0x4,
    push_promise = 0x5,
    ping = 0x6,
    goaway = 0x7,
    window_update = 0x8,
    continuation = 0x9,
};

// "DATA", "HEADERS", ...; "UNKNOWN_FRAME_TYPE_n" for others.
string to_string(FrameType t);

namespace flags {
inline constexpr uint8 endStream = 0x1;  // DATA, HEADERS
inline constexpr uint8 ack = 0x1;         // SETTINGS, PING
inline constexpr uint8 end_headers = 0x4; // HEADERS, PUSH_PROMISE, CONTINUATION
inline constexpr uint8 padded = 0x8;      // DATA, HEADERS, PUSH_PROMISE
inline constexpr uint8 priority = 0x20;   // HEADERS
}  // namespace flags

enum class ErrCode : uint32 {
    no = 0x0,
    protocol = 0x1,
    internal = 0x2,
    flow_control = 0x3,
    settings_timeout = 0x4,
    stream_closed = 0x5,
    frame_size = 0x6,
    refused_stream = 0x7,
    cancel = 0x8,
    compression = 0x9,
    connect = 0xa,
    enhance_your_calm = 0xb,
    inadequate_security = 0xc,
    http_1_1_required = 0xd,
};

// "NO_ERROR", "PROTOCOL_ERROR", ...
string to_string(ErrCode c);

enum class SettingID : uint16 {
    header_table_size = 0x1,
    enable_push = 0x2,
    max_concurrent_streams = 0x3,
    initial_window_size = 0x4,
    max_frame_size = 0x5,
    max_header_list_size = 0x6,
    enable_connect_protocol = 0x8,
    no_rfc7540_priorities = 0x9,
};

string to_string(SettingID s);

struct Setting {
    SettingID id;
    uint32 val;
    // A connection error if the value is out of range (RFC 9113 6.5.2).
    gopp::error Valid() const;
    friend bool operator==(const Setting&, const Setting&) = default;
};

// ---------------------------------------------------------------------------
// Errors

// http2.ConnectionError: the connection must end with GOAWAY and this code.
struct ConnectionError : errorInterface {
    ErrCode code;
    string reason;  // for logs; not sent
    explicit ConnectionError(ErrCode c, string r = {}) : code(c), reason(std::move(r)) {}
    string Error() const override;
};

// http2.StreamError: the stream must end with RST_STREAM and this code.
struct StreamError : errorInterface {
    uint32 streamId;
    ErrCode code;
    gopp::error cause;
    StreamError(uint32 id, ErrCode c, gopp::error e = nullptr) : streamId(id), code(c), cause(std::move(e)) {}
    string Error() const override;
    const errorInterface* Unwrap() const noexcept override { return cause.get(); }
};

gopp::error connectionError(ErrCode code, std::string_view reason = {});
gopp::error streamError(uint32 id, ErrCode code, gopp::error cause = nullptr);

// A GOAWAY from the peer, as the client reports it.
struct GoAwayError : errorInterface {
    uint32 last_stream_id;
    ErrCode code;
    string debug_data;
    GoAwayError(uint32 id, ErrCode c, string d) : last_stream_id(id), code(c), debug_data(std::move(d)) {}
    string Error() const override;
};

extern const gopp::error err_frame_too_large;  // "http2: frame too large"

// ---------------------------------------------------------------------------
// Frames

struct FrameHeader {
    FrameType type{};
    uint8 flags = 0;
    uint32 length = 0;  // of the payload
    uint32 streamId = 0;
    bool has(uint8 f) const noexcept { return (flags & f) == f; }
    string String() const;  // "[FrameHeader DATA flags=END_STREAM stream=1 len=5]"
};

struct PriorityParam {
    uint32 stream_dep = 0;
    bool exclusive = false;
    uint8 weight = 0;  // the weight minus one
    bool IsZero() const noexcept { return stream_dep == 0 && !exclusive && weight == 0; }
};

// A frame as read. The fields that are set depend on the type.
struct Frame {
    FrameHeader header;
    // DATA: the data without padding. HEADERS, PUSH_PROMISE, CONTINUATION:
    // the header block fragment. PING: the 8 bytes. GOAWAY: debug data.
    // Unknown types: the payload.
    string data;
    PriorityParam priority;          // HEADERS with the PRIORITY flag; PRIORITY
    ErrCode err_code{};              // RST_STREAM, GOAWAY
    uint32 last_stream_id = 0;       // GOAWAY
    uint32 promise_id = 0;           // PUSH_PROMISE
    uint32 increment = 0;            // WINDOW_UPDATE
    std::vector<Setting> settings;   // SETTINGS

    // A HEADERS frame read with its CONTINUATION frames and decoded
    // (MetaHeadersFrame).
    bool is_meta = false;
    std::vector<hpack::HeaderField> fields;  // pseudo fields first
    // The header list exceeded the limit and was not fully decoded.
    bool truncated = false;

    FrameType type() const noexcept { return header.type; }
    uint32 streamId() const noexcept { return header.streamId; }
    bool streamEnded() const noexcept { return header.has(flags::endStream); }
    bool headersEnded() const noexcept { return header.has(flags::end_headers); }
    bool isAck() const noexcept { return header.has(flags::ack); }
    bool hasPriority() const noexcept { return header.has(flags::priority); }

    // The value of the pseudo header field ":"+name, or "".
    string pseudoValue(std::string_view name) const;
    std::span<const hpack::HeaderField> pseudoFields() const;
    std::span<const hpack::HeaderField> regularFields() const;
    // The value of a setting, if present.
    std::optional<uint32> setting(SettingID id) const;
};

// Reads frames from a connection (the reading half of Go's Framer).
class Framer {
public:
    explicit Framer(ptr<bufio::Reader> r) : r_(std::move(r)) {}

    // The largest frame payload accepted: our SETTINGS_MAX_FRAME_SIZE.
    void setMaxReadFrameSize(uint32 v) noexcept { max_read_size_ = v > max_frame_size_limit ? max_frame_size_limit : v; }
    // When set, HEADERS frames are read with their CONTINUATION frames and
    // decoded with this decoder (Framer.ReadMetaHeaders).
    hpack::Decoder* read_meta_headers = nullptr;
    // Our SETTINGS_MAX_HEADER_LIST_SIZE; 16 MiB if 0.
    uint32 max_header_list_size = 0;

    // Reads the next frame. A ConnectionError or StreamError for protocol
    // violations; err_frame_too_large; or the network's error.
    std::pair<Frame, gopp::error> readFrame();
    // Details of the last connection error, for logs.
    gopp::error errorDetail() const { return err_detail_; }

private:
    std::pair<Frame, gopp::error> Parse(const FrameHeader& fh, string payload);
    gopp::error checkFrameOrder(const FrameHeader& fh);
    gopp::error connError(ErrCode code, std::string_view reason);
    gopp::error readMetaFrame(Frame& hf);
    uint32 maxHeaderList() const noexcept { return max_header_list_size ? max_header_list_size : 16 << 20; }

    ptr<bufio::Reader> r_;
    uint32 max_read_size_ = initial_max_frame_size;
    uint32 last_header_stream_ = 0;
    FrameType last_frame_type_{};
    gopp::error err_detail_;
};

// Frame writers: append a frame to out (the writing half of Go's Framer).
void appendFrameHeader(std::string& out, FrameType type, uint8 flags, uint32 streamId, std::size_t length);
void appendData(std::string& out, uint32 streamId, bool endStream, std::string_view data);
// A header block as HEADERS and as many CONTINUATION frames as
// max_frame_size requires.
void appendHeaders(std::string& out, uint32 streamId, std::string_view block, bool endStream, uint32 max_frame_size);
void appendSettings(std::string& out, std::span<const Setting> settings);
void appendSettingsAck(std::string& out);
void appendPing(std::string& out, bool ack, std::string_view data8);
void appendGoaway(std::string& out, uint32 max_stream_id, ErrCode code, std::string_view debug = {});
void appendWindowUpdate(std::string& out, uint32 streamId, uint32 incr);
void appendRstStream(std::string& out, uint32 streamId, ErrCode code);
void appendPriority(std::string& out, uint32 streamId, const PriorityParam& p);

// Header field validity on the wire: names are lower case tokens.
bool validWireHeaderFieldName(std::string_view v) noexcept;
// Headers that are connection-specific in HTTP/1 and invalid in HTTP/2.
bool isConnectionHeader(std::string_view canonical_key) noexcept;
// The lower-case wire name of a canonical header key.
string lowerHeader(std::string_view key);
// Whether the value of a request header may be sent: Go drops some.
bool validPseudoPath(std::string_view v) noexcept;

// ---------------------------------------------------------------------------
// Flow control (flow.Go)

// The window for sending on a stream, limited by the connection's too.
struct outflow {
    int32 n = 0;
    outflow* conn = nullptr;  // nil for the connection's own window

    int32 available() const noexcept { return conn && conn->n < n ? conn->n : n; }
    void take(int32 k) noexcept {
        n -= k;
        if (conn) conn->n -= k;
    }
    // Adds a WINDOW_UPDATE or SETTINGS change; false on overflow.
    bool add(int32 k) noexcept {
        int64 sum = int64(n) + k;
        if (sum > max_window_size) return false;
        n = static_cast<int32>(sum);
        return true;
    }
};

// The window for receiving: what the peer may send, and what the reader
// consumed that we haven't announced yet.
struct inflow {
    int32 avail = 0;
    int32 unsent = 0;

    void init(int32 n) noexcept { avail = n; }
    // Returns the WINDOW_UPDATE to send for n consumed bytes, or 0 while
    // the update would be small (inflowMinRefresh).
    int32 add(int32 n) noexcept;
    // Accounts for a received frame; false if the peer overran the window.
    bool take(uint32 n) noexcept;
};

// Takes n from both windows, or neither.
bool takeInflows(inflow& f1, inflow& f2, uint32 n) noexcept;

// ---------------------------------------------------------------------------
// pipe (pipe.Go): a body buffer between the connection, which writes
// received DATA into it without blocking, and a reader that blocks.

class pipe final : public object {
public:
    // Bytes buffered, or discarded by break_with_error: the connection
    // returns their flow control when the stream closes.
    std::size_t len();
    // Reads, blocking until data or an error is available.
    std::pair<isize, gopp::error> Read(std::span<byte> p);
    // Buffers data; an error if the pipe was closed.
    gopp::error write(string data);
    // The reader gets err once the buffer is drained (io::EOF_ for a body
    // that ended). Later errors are ignored.
    void closeWithError(gopp::error err);
    // The reader gets err now; buffered data is discarded.
    void breakWithError(gopp::error err);
    // Like close_with_error, and runs fn once the reader sees the error,
    // before the read returns (for trailers).
    void closeWithErrorAndCode(gopp::error err, std::function<void()> fn);
    // The error the reader gets, once set.
    gopp::error err();
    // Closed when an error is set.
    chan<empty> done();

private:
    void closeDoneLocked();
    sync::Mutex mu_;
    sync::Cond c_{&mu_};
    std::deque<string> b_;
    std::size_t front_off_ = 0;
    std::size_t len_ = 0;
    std::size_t unread_ = 0;  // discarded by break_with_error
    gopp::error err_;        // for the reader once the buffer is drained
    gopp::error break_err_;  // for the reader now
    std::function<void()> read_fn_;
    chan<empty> donec_;
    bool done_closed_ = false;
};

extern const gopp::error err_closed_pipe_write;  // "write on closed buffer"

}  // namespace gopp::http::http2
