// After Go's net/http/internal/http2: frame.Go, errors.Go, flow.Go and
// pipe.Go.
#include "http2.hpp"

#include <gopp/strings.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <format>
#include <mutex>

namespace gopp::http::http2 {

namespace {

uint32 read_u32(const char* p) noexcept {
    auto b = reinterpret_cast<const unsigned char*>(p);
    return uint32(b[0]) << 24 | uint32(b[1]) << 16 | uint32(b[2]) << 8 | uint32(b[3]);
}

void put_u32(std::string& out, uint32 v) {
    char b[4] = {static_cast<char>(v >> 24), static_cast<char>(v >> 16), static_cast<char>(v >> 8), static_cast<char>(v)};
    out.append(b, 4);
}

const gopp::error err_pseudo_after_regular = errors::New("pseudo header field after regular");
const gopp::error err_mix_pseudo_header_types = errors::New("mix of request and response pseudo headers");

gopp::error header_field_name_error(std::string_view name) {
    return errors::New("invalid header field name " + strconv::Quote(name));
}

gopp::error header_field_value_error(std::string_view name) {
    return errors::New("invalid header field value for " + strconv::Quote(name));
}

}  // namespace

const gopp::error err_frame_too_large = errors::New("http2: frame too large");
const gopp::error err_closed_pipe_write = errors::New("write on closed buffer");

// ---------------------------------------------------------------------------
// Names

string to_string(FrameType t) {
    static const char* names[] = {"DATA",   "HEADERS", "PRIORITY", "RST_STREAM",    "SETTINGS",
                                  "PUSH_PROMISE", "PING",    "GOAWAY",   "WINDOW_UPDATE", "CONTINUATION"};
    auto i = static_cast<std::size_t>(t);
    if (i < std::size(names)) return string::from_static(names[i]);
    return "UNKNOWN_FRAME_TYPE_" + strconv::Itoa(static_cast<int64>(i));
}

string to_string(ErrCode c) {
    static const char* names[] = {"NO_ERROR",         "PROTOCOL_ERROR", "INTERNAL_ERROR",    "FLOW_CONTROL_ERROR",
                                  "SETTINGS_TIMEOUT", "STREAM_CLOSED",  "FRAME_SIZE_ERROR",  "REFUSED_STREAM",
                                  "CANCEL",           "COMPRESSION_ERROR", "CONNECT_ERROR",  "ENHANCE_YOUR_CALM",
                                  "INADEQUATE_SECURITY", "HTTP_1_1_REQUIRED"};
    auto i = static_cast<std::size_t>(c);
    if (i < std::size(names)) return string::from_static(names[i]);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "unknown error code 0x%x", static_cast<unsigned>(i));
    return string(buf);
}

string to_string(SettingID s) {
    switch (s) {
    case SettingID::header_table_size: return string::from_static("HEADER_TABLE_SIZE");
    case SettingID::enable_push: return string::from_static("ENABLE_PUSH");
    case SettingID::max_concurrent_streams: return string::from_static("MAX_CONCURRENT_STREAMS");
    case SettingID::initial_window_size: return string::from_static("INITIAL_WINDOW_SIZE");
    case SettingID::max_frame_size: return string::from_static("MAX_FRAME_SIZE");
    case SettingID::max_header_list_size: return string::from_static("MAX_HEADER_LIST_SIZE");
    case SettingID::enable_connect_protocol: return string::from_static("ENABLE_CONNECT_PROTOCOL");
    case SettingID::no_rfc7540_priorities: return string::from_static("NO_RFC7540_PRIORITIES");
    }
    return "UNKNOWN_SETTING_" + strconv::Itoa(static_cast<int64>(s));
}

gopp::error Setting::Valid() const {
    switch (id) {
    case SettingID::enable_push:
    case SettingID::enable_connect_protocol:
        if (val != 0 && val != 1) return connection_error(ErrCode::protocol);
        break;
    case SettingID::initial_window_size:
        if (val > uint32(max_window_size)) return connection_error(ErrCode::flow_control);
        break;
    case SettingID::max_frame_size:
        if (val < initial_max_frame_size || val > max_frame_size_limit) return connection_error(ErrCode::protocol);
        break;
    default:
        break;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Errors

string ConnectionError::Error() const { return "connection error: " + to_string(code); }

string StreamError::Error() const {
    string s = "stream error: stream ID " + strconv::Itoa(stream_id) + "; " + to_string(code);
    if (cause) s = s + "; " + cause.Error();
    return s;
}

string GoAwayError::Error() const {
    return "http2: server sent GOAWAY and closed the connection; LastStreamID=" + strconv::Itoa(last_stream_id) +
           ", ErrCode=" + to_string(code) + ", debug=" + strconv::Quote(debug_data);
}

gopp::error connection_error(ErrCode code, std::string_view reason) { return make_ptr<ConnectionError>(code, string(reason)); }

gopp::error stream_error(uint32 id, ErrCode code, gopp::error cause) {
    return make_ptr<StreamError>(id, code, std::move(cause));
}

// ---------------------------------------------------------------------------
// Frames

string FrameHeader::String() const {
    std::string s = "[FrameHeader " + std::string(http2::to_string(type).view());
    if (flags) {
        s += " flags=";
        bool first = true;
        auto add = [&](uint8 bit, const char* name) {
            if (!(flags & bit)) return;
            if (!first) s += "|";
            s += name;
            first = false;
        };
        switch (type) {
        case FrameType::data: add(flags::end_stream, "END_STREAM"); add(flags::padded, "PADDED"); break;
        case FrameType::headers:
            add(flags::end_stream, "END_STREAM");
            add(flags::end_headers, "END_HEADERS");
            add(flags::padded, "PADDED");
            add(flags::priority, "PRIORITY");
            break;
        case FrameType::settings: case FrameType::ping: add(flags::ack, "ACK"); break;
        case FrameType::push_promise: add(flags::end_headers, "END_HEADERS"); add(flags::padded, "PADDED"); break;
        case FrameType::continuation: add(flags::end_headers, "END_HEADERS"); break;
        default: break;
        }
        if (first) s += "0x" + std::to_string(flags);
    }
    if (stream_id) s += " stream=" + std::to_string(stream_id);
    s += " len=" + std::to_string(length) + "]";
    return string(s);
}

string Frame::pseudo_value(std::string_view name) const {
    for (auto& hf : pseudo_fields()) {
        if (hf.name.view().substr(1) == name) return hf.value;
    }
    return {};
}

std::span<const hpack::HeaderField> Frame::pseudo_fields() const {
    std::size_t i = 0;
    while (i < fields.size() && fields[i].is_pseudo()) i++;
    return std::span(fields).first(i);
}

std::span<const hpack::HeaderField> Frame::regular_fields() const {
    return std::span(fields).subspan(pseudo_fields().size());
}

std::optional<uint32> Frame::setting(SettingID id) const {
    for (auto& s : settings) {
        if (s.id == id) return s.val;
    }
    return std::nullopt;
}

gopp::error Framer::conn_error(ErrCode code, std::string_view reason) {
    err_detail_ = errors::New(string(reason));
    return connection_error(code, reason);
}

gopp::error Framer::check_frame_order(const FrameHeader& fh) {
    FrameType last = last_frame_type_;
    last_frame_type_ = fh.type;
    if (last_header_stream_ != 0) {
        if (fh.type != FrameType::continuation) {
            return conn_error(ErrCode::protocol, std::format("got {} for stream {}; expected CONTINUATION following {} for stream {}",
                                                             to_string(fh.type).view(), fh.stream_id, to_string(last).view(),
                                                             last_header_stream_));
        }
        if (fh.stream_id != last_header_stream_) {
            return conn_error(ErrCode::protocol, std::format("got CONTINUATION for stream {}; expected stream {}",
                                                             fh.stream_id, last_header_stream_));
        }
    } else if (fh.type == FrameType::continuation) {
        return conn_error(ErrCode::protocol, std::format("unexpected CONTINUATION for stream {}", fh.stream_id));
    }
    if (fh.type == FrameType::headers || fh.type == FrameType::continuation) {
        last_header_stream_ = fh.has(flags::end_headers) ? 0 : fh.stream_id;
    }
    return nullptr;
}

std::pair<Frame, gopp::error> Framer::read_frame() {
    char hb[frame_header_len];
    if (auto [n, err] = io::ReadFull(*r_, std::span<byte>(reinterpret_cast<byte*>(hb), sizeof(hb))); err) {
        return {Frame{}, err};
    }
    FrameHeader fh;
    fh.length = uint32(uint8(hb[0])) << 16 | uint32(uint8(hb[1])) << 8 | uint8(hb[2]);
    fh.type = static_cast<FrameType>(hb[3]);
    fh.flags = static_cast<uint8>(hb[4]);
    fh.stream_id = read_u32(hb + 5) & 0x7fffffff;
    if (fh.length > max_read_size_) return {Frame{}, err_frame_too_large};
    gopp::error rerr;
    string payload = string::build(fh.length, [&](char* p) {
        rerr = io::ReadFull(*r_, std::span<byte>(reinterpret_cast<byte*>(p), fh.length)).second;
    });
    if (rerr) return {Frame{}, rerr == io::EOF_ ? io::ErrUnexpectedEOF : rerr};
    auto [f, err] = Parse(fh, std::move(payload));
    if (err) {
        if (auto* ce = errors::As<ConnectionError>(err); ce && !ce->reason.empty()) err_detail_ = errors::New(ce->reason);
        return {Frame{}, err};
    }
    if (gopp::error oerr = check_frame_order(fh)) return {Frame{}, oerr};
    if (fh.type == FrameType::headers && read_meta_headers) {
        if (gopp::error merr = read_meta_frame(f)) return {std::move(f), merr};
    }
    return {std::move(f), nullptr};
}

std::pair<Frame, gopp::error> Framer::Parse(const FrameHeader& fh, string p) {
    Frame f;
    f.header = fh;
    auto fail = [](gopp::error e) { return std::pair<Frame, gopp::error>{Frame{}, std::move(e)}; };
    // Strips the pad length byte and the padding.
    auto unpad = [&](string& q, gopp::error& err, bool stream_err) {
        if (!fh.has(flags::padded)) return;
        if (q.empty()) {
            err = io::ErrUnexpectedEOF;
            return;
        }
        std::size_t pad = static_cast<uint8>(q[0]);
        q = q.slice(1);
        if (pad > q.size()) {
            err = stream_err ? stream_error(fh.stream_id, ErrCode::protocol)
                             : connection_error(ErrCode::protocol, "pad size larger than data payload");
            return;
        }
        q = q.slice(0, q.size() - pad);
    };
    switch (fh.type) {
    case FrameType::data: {
        if (fh.stream_id == 0) return fail(connection_error(ErrCode::protocol, "DATA frame with stream ID 0"));
        gopp::error err;
        unpad(p, err, false);
        if (err) return fail(err);
        f.data = std::move(p);
        break;
    }
    case FrameType::headers: {
        if (fh.stream_id == 0) return fail(connection_error(ErrCode::protocol, "HEADERS frame with stream ID 0"));
        std::size_t pad = 0;
        if (fh.has(flags::padded)) {
            if (p.empty()) return fail(io::ErrUnexpectedEOF);
            pad = static_cast<uint8>(p[0]);
            p = p.slice(1);
        }
        if (fh.has(flags::priority)) {
            if (p.size() < 5) return fail(io::ErrUnexpectedEOF);
            uint32 v = read_u32(p.data());
            f.priority.stream_dep = v & 0x7fffffff;
            f.priority.exclusive = v != f.priority.stream_dep;
            f.priority.weight = static_cast<uint8>(p[4]);
            p = p.slice(5);
        }
        if (pad > p.size()) return fail(stream_error(fh.stream_id, ErrCode::protocol));
        f.data = p.slice(0, p.size() - pad);
        break;
    }
    case FrameType::priority: {
        if (fh.stream_id == 0) return fail(connection_error(ErrCode::protocol, "PRIORITY frame with stream ID 0"));
        if (p.size() != 5) {
            return fail(connection_error(ErrCode::frame_size, std::format("PRIORITY frame payload size was {}; want 5", p.size())));
        }
        uint32 v = read_u32(p.data());
        f.priority.stream_dep = v & 0x7fffffff;
        f.priority.exclusive = v != f.priority.stream_dep;
        f.priority.weight = static_cast<uint8>(p[4]);
        break;
    }
    case FrameType::rst_stream:
        if (p.size() != 4) return fail(connection_error(ErrCode::frame_size));
        if (fh.stream_id == 0) return fail(connection_error(ErrCode::protocol));
        f.err_code = static_cast<ErrCode>(read_u32(p.data()));
        break;
    case FrameType::settings:
        if (fh.has(flags::ack) && fh.length > 0) return fail(connection_error(ErrCode::frame_size));
        if (fh.stream_id != 0) return fail(connection_error(ErrCode::protocol));
        if (p.size() % 6 != 0) return fail(connection_error(ErrCode::frame_size));
        for (std::size_t i = 0; i < p.size(); i += 6) {
            Setting s{static_cast<SettingID>(uint16(uint8(p[i])) << 8 | uint8(p[i + 1])), read_u32(p.data() + i + 2)};
            if (s.id == SettingID::initial_window_size && s.val > uint32(max_window_size)) {
                return fail(connection_error(ErrCode::flow_control));
            }
            f.settings.push_back(s);
        }
        break;
    case FrameType::push_promise: {
        if (fh.stream_id == 0) return fail(connection_error(ErrCode::protocol));
        gopp::error err;
        unpad(p, err, false);
        if (err) return fail(err);
        if (p.size() < 4) return fail(io::ErrUnexpectedEOF);
        f.promise_id = read_u32(p.data()) & 0x7fffffff;
        f.data = p.slice(4);
        break;
    }
    case FrameType::ping:
        if (p.size() != 8) return fail(connection_error(ErrCode::frame_size));
        if (fh.stream_id != 0) return fail(connection_error(ErrCode::protocol));
        f.data = std::move(p);
        break;
    case FrameType::goaway:
        if (fh.stream_id != 0) return fail(connection_error(ErrCode::protocol));
        if (p.size() < 8) return fail(connection_error(ErrCode::frame_size));
        f.last_stream_id = read_u32(p.data()) & 0x7fffffff;
        f.err_code = static_cast<ErrCode>(read_u32(p.data() + 4));
        f.data = p.slice(8);
        break;
    case FrameType::window_update:
        if (p.size() != 4) return fail(connection_error(ErrCode::frame_size));
        f.increment = read_u32(p.data()) & 0x7fffffff;
        if (f.increment == 0) {
            // A zero increment is a connection error on the connection,
            // a stream error on a stream.
            if (fh.stream_id == 0) return fail(connection_error(ErrCode::protocol));
            return fail(stream_error(fh.stream_id, ErrCode::protocol));
        }
        break;
    case FrameType::continuation:
        if (fh.stream_id == 0) return fail(connection_error(ErrCode::protocol, "CONTINUATION frame with stream ID 0"));
        f.data = std::move(p);
        break;
    default:
        f.data = std::move(p);  // unknown frame types are ignored
        break;
    }
    return {std::move(f), nullptr};
}

gopp::error Framer::read_meta_frame(Frame& mh) {
    struct budget {
        uint32 remain_size;
        int count = 0;
    };
    budget headers{max_header_list()}, trailers{max_header_list()};
    bool saw_regular = false;
    gopp::error invalid;
    hpack::Decoder& hdec = *read_meta_headers;
    hdec.set_emit_enabled(true);
    hdec.set_max_string_length(static_cast<int>(max_header_list()));
    hdec.set_emit_func([&](const hpack::HeaderField& hf) {
        if (!internal::valid_header_field_value(hf.value)) invalid = header_field_value_error(hf.name);
        if (hf.is_pseudo()) {
            if (saw_regular) invalid = err_pseudo_after_regular;
        } else {
            saw_regular = true;
            if (!valid_wire_header_field_name(hf.name)) invalid = header_field_name_error(hf.name);
        }
        if (invalid) {
            hdec.set_emit_enabled(false);
            return;
        }
        budget* b;
        uint32 size;
        if (hf.name == "trailer") {
            b = &trailers;
            int field_count = static_cast<int>(std::count(hf.value.begin(), hf.value.end(), ',')) + 1;
            b->count += field_count;
            size = static_cast<uint32>(hf.value.size() - field_count + 1) + uint32(field_count) * 32;
        } else {
            b = &headers;
            b->count++;
            size = hf.size();
        }
        if (size > b->remain_size) {
            hdec.set_emit_enabled(false);
            mh.truncated = true;
            headers.remain_size = trailers.remain_size = 0;
            return;
        }
        b->remain_size -= size;
        mh.fields.push_back(hf);
    });
    struct reset_emit {
        hpack::Decoder& d;
        ~reset_emit() { d.set_emit_func(nullptr); }
    } reset{hdec};

    mh.is_meta = true;
    Frame* hc = &mh;
    Frame cont;
    for (;;) {
        std::string_view frag = hc->data;
        // A fragment much larger than what we would accept: refuse to
        // decode it (and spend memory on it).
        if (static_cast<int64>(frag.size()) > 2 * (int64(headers.remain_size) + trailers.remain_size)) {
            return connection_error(ErrCode::protocol, "header list too large");
        }
        if (invalid) return connection_error(ErrCode::protocol);
        if (auto [n, err] = hdec.Write(std::span<const byte>(reinterpret_cast<const byte*>(frag.data()), frag.size())); err) {
            return connection_error(ErrCode::compression);
        }
        if (hc->headers_ended()) break;
        auto [f, err] = read_frame();
        if (err) return err;
        cont = std::move(f);  // a CONTINUATION, as check_frame_order made sure
        hc = &cont;
    }
    mh.data = {};
    if (hdec.Close()) return connection_error(ErrCode::compression);
    if (invalid) {
        err_detail_ = invalid;
        return stream_error(mh.stream_id(), ErrCode::protocol, invalid);
    }
    // checkPseudos
    bool is_request = false, is_response = false;
    auto pf = mh.pseudo_fields();
    for (std::size_t i = 0; i < pf.size(); i++) {
        std::string_view n = pf[i].name;
        if (n == ":method" || n == ":path" || n == ":scheme" || n == ":authority" || n == ":protocol") {
            is_request = true;
        } else if (n == ":status") {
            is_response = true;
        } else {
            gopp::error e = errors::New("invalid pseudo-header " + strconv::Quote(n));
            err_detail_ = e;
            return stream_error(mh.stream_id(), ErrCode::protocol, e);
        }
        for (std::size_t j = 0; j < i; j++) {
            if (pf[j].name == n) {
                gopp::error e = errors::New("duplicate pseudo-header " + strconv::Quote(n));
                err_detail_ = e;
                return stream_error(mh.stream_id(), ErrCode::protocol, e);
            }
        }
    }
    if (is_request && is_response) return stream_error(mh.stream_id(), ErrCode::protocol, err_mix_pseudo_header_types);
    return nullptr;
}

// ---------------------------------------------------------------------------
// Frame writers

void append_frame_header(std::string& out, FrameType type, uint8 fl, uint32 stream_id, std::size_t length) {
    char b[frame_header_len] = {static_cast<char>(length >> 16), static_cast<char>(length >> 8), static_cast<char>(length),
                                static_cast<char>(type),         static_cast<char>(fl),          0, 0, 0, 0};
    out.append(b, 5);
    put_u32(out, stream_id & 0x7fffffff);
}

void append_data(std::string& out, uint32 stream_id, bool end_stream, std::string_view data) {
    append_frame_header(out, FrameType::data, end_stream ? flags::end_stream : 0, stream_id, data.size());
    out.append(data);
}

void append_headers(std::string& out, uint32 stream_id, std::string_view block, bool end_stream, uint32 max_frame_size) {
    std::size_t max = max_frame_size ? max_frame_size : initial_max_frame_size;
    std::string_view first = block.substr(0, max);
    block.remove_prefix(first.size());
    uint8 fl = (end_stream ? flags::end_stream : 0) | (block.empty() ? flags::end_headers : 0);
    append_frame_header(out, FrameType::headers, fl, stream_id, first.size());
    out.append(first);
    while (!block.empty()) {
        std::string_view frag = block.substr(0, max);
        block.remove_prefix(frag.size());
        append_frame_header(out, FrameType::continuation, block.empty() ? flags::end_headers : 0, stream_id, frag.size());
        out.append(frag);
    }
}

void append_settings(std::string& out, std::span<const Setting> settings) {
    append_frame_header(out, FrameType::settings, 0, 0, settings.size() * 6);
    for (auto& s : settings) {
        out.push_back(static_cast<char>(static_cast<uint16>(s.id) >> 8));
        out.push_back(static_cast<char>(static_cast<uint16>(s.id)));
        put_u32(out, s.val);
    }
}

void append_settings_ack(std::string& out) { append_frame_header(out, FrameType::settings, flags::ack, 0, 0); }

void append_ping(std::string& out, bool ack, std::string_view data8) {
    append_frame_header(out, FrameType::ping, ack ? flags::ack : 0, 0, 8);
    char b[8] = {};
    std::memcpy(b, data8.data(), std::min<std::size_t>(8, data8.size()));
    out.append(b, 8);
}

void append_goaway(std::string& out, uint32 max_stream_id, ErrCode code, std::string_view debug) {
    append_frame_header(out, FrameType::goaway, 0, 0, 8 + debug.size());
    put_u32(out, max_stream_id & 0x7fffffff);
    put_u32(out, static_cast<uint32>(code));
    out.append(debug);
}

void append_window_update(std::string& out, uint32 stream_id, uint32 incr) {
    append_frame_header(out, FrameType::window_update, 0, stream_id, 4);
    put_u32(out, incr & 0x7fffffff);
}

void append_rst_stream(std::string& out, uint32 stream_id, ErrCode code) {
    append_frame_header(out, FrameType::rst_stream, 0, stream_id, 4);
    put_u32(out, static_cast<uint32>(code));
}

void append_priority(std::string& out, uint32 stream_id, const PriorityParam& p) {
    append_frame_header(out, FrameType::priority, 0, stream_id, 5);
    put_u32(out, p.stream_dep | (p.exclusive ? 0x80000000u : 0));
    out.push_back(static_cast<char>(p.weight));
}

bool valid_wire_header_field_name(std::string_view v) noexcept {
    if (v.empty()) return false;
    for (unsigned char c : v) {
        if (!internal::is_token_char(c)) return false;
        if (c >= 'A' && c <= 'Z') return false;
    }
    return true;
}

bool is_connection_header(std::string_view k) noexcept {
    return k == "Connection" || k == "Proxy-Connection" || k == "Keep-Alive" || k == "Transfer-Encoding" || k == "Upgrade";
}

string lower_header(std::string_view key) {
    // Common keys are canonical already; lower them without a table.
    return strings::ToLower(key);
}

bool valid_pseudo_path(std::string_view v) noexcept { return !v.empty() && (v[0] == '/' || v == "*"); }

// ---------------------------------------------------------------------------
// Flow control

int32 inflow::add(int32 n) noexcept {
    constexpr int32 inflow_min_refresh = 4 << 10;
    int64 u = int64(unsent) + n;
    if (u + avail > max_window_size) panic("flow control update exceeds maximum window size");
    unsent = static_cast<int32>(u);
    if (unsent < inflow_min_refresh && unsent < avail) return 0;
    avail += unsent;
    unsent = 0;
    return static_cast<int32>(u);
}

bool inflow::take(uint32 n) noexcept {
    if (n > uint32(avail)) return false;
    avail -= static_cast<int32>(n);
    return true;
}

bool take_inflows(inflow& f1, inflow& f2, uint32 n) noexcept {
    if (n > uint32(f1.avail) || n > uint32(f2.avail)) return false;
    f1.avail -= static_cast<int32>(n);
    f2.avail -= static_cast<int32>(n);
    return true;
}

// ---------------------------------------------------------------------------
// pipe

std::size_t pipe::len() {
    std::lock_guard l(mu_);
    return break_err_ ? unread_ : len_;
}

std::pair<isize, gopp::error> pipe::Read(std::span<byte> d) {
    std::lock_guard l(mu_);
    for (;;) {
        if (break_err_) return {0, break_err_};
        if (len_ > 0) {
            std::size_t n = 0;
            while (n < d.size() && !b_.empty()) {
                const string& front = b_.front();
                std::size_t k = std::min(d.size() - n, front.size() - front_off_);
                std::memcpy(d.data() + n, front.data() + front_off_, k);
                n += k;
                front_off_ += k;
                if (front_off_ == front.size()) {
                    b_.pop_front();
                    front_off_ = 0;
                }
            }
            len_ -= n;
            return {static_cast<isize>(n), nullptr};
        }
        if (err_) {
            if (read_fn_) {
                auto fn = std::move(read_fn_);
                read_fn_ = nullptr;
                fn();
            }
            return {0, err_};
        }
        c_.Wait();
    }
}

gopp::error pipe::write(string data) {
    std::lock_guard l(mu_);
    if (err_ || break_err_) return err_closed_pipe_write;
    if (!data.empty()) {
        len_ += data.size();
        b_.push_back(std::move(data));
    }
    c_.Signal();
    return nullptr;
}

void pipe::close_with_error(gopp::error err) { close_with_error_and_code(std::move(err), nullptr); }

void pipe::close_with_error_and_code(gopp::error err, std::function<void()> fn) {
    if (!err) panic("err must be non-nil");
    std::lock_guard l(mu_);
    if (!err_) {
        read_fn_ = std::move(fn);
        err_ = std::move(err);
        close_done_locked();
    }
    c_.Signal();
}

void pipe::break_with_error(gopp::error err) {
    if (!err) panic("err must be non-nil");
    std::lock_guard l(mu_);
    if (!break_err_) {
        unread_ += len_;  // still counts for flow control
        b_.clear();
        front_off_ = 0;
        len_ = 0;
        break_err_ = std::move(err);
        close_done_locked();
    }
    c_.Signal();
}

gopp::error pipe::err() {
    std::lock_guard l(mu_);
    return break_err_ ? break_err_ : err_;
}

void pipe::close_done_locked() {
    if (!donec_) return;  // done() creates it closed
    if (!done_closed_) {
        done_closed_ = true;
        donec_.close();
    }
}

chan<empty> pipe::done() {
    std::lock_guard l(mu_);
    if (!donec_) {
        donec_ = make_chan<empty>();
        if (err_ || break_err_) {
            done_closed_ = true;
            donec_.close();
        }
    }
    return donec_;
}

}  // namespace gopp::http::http2
