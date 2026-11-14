// Reading requests: after Go's net/http/request.Go (readRequest),
// transfer.Go and internal/chunked.Go.
#include "ascii.hpp"
#include "internal.hpp"

#include <gopp/strings.hpp>

#include <string>
#include <vector>

namespace gopp::http {

// ---------------------------------------------------------------------------
// Request

Request::Request(const Request& o)
    : object(o),
      Method(o.Method),
      URL(o.URL),
      Proto(o.Proto),
      ProtoMajor(o.ProtoMajor),
      ProtoMinor(o.ProtoMinor),
      Header(o.Header),
      Body(o.Body),
      ContentLength(o.ContentLength),
      TransferEncoding(o.TransferEncoding),
      Close(o.Close),
      Host(o.Host),
      RemoteAddr(o.RemoteAddr),
      RequestURI(o.RequestURI),
      Pattern(o.Pattern),
      TLS(o.TLS),
      ctx_(o.ctx_),
      path_values_(o.path_values_) {}

Request::~Request() = default;

ptr<context::Context> Request::Context() const {
    if (!ctx_) {
        auto [c, cancel] = context::WithCancel(context::Background());
        ctx_ = std::move(c);
        cancel_ = std::move(cancel);
    }
    return ctx_;
}

void Request::CancelContext() const {
    if (cancel_) cancel_();
}

ptr<Request> Request::WithContext(ptr<context::Context> ctx) const {
    if (!ctx) panic("nil context");
    auto r2 = make_ptr<Request>(*this);
    r2->ctx_ = std::move(ctx);
    return r2;
}

string Request::PathValue(std::string_view name) const {
    for (auto& [k, v] : path_values_) {
        if (k == name) return v;
    }
    return {};
}

void Request::SetPathValue(std::string_view name, string value) {
    for (auto& [k, v] : path_values_) {
        if (k == name) {
            v = std::move(value);
            return;
        }
    }
    path_values_.emplace_back(string(name), std::move(value));
}

string Request::FormValue(std::string_view key) const { return URL.Query().Get(key); }

namespace internal {

namespace {

constexpr int64 max_post_handler_read_bytes = 256 << 10;

gopp::error bad_request(std::string_view what) { return make_ptr<status_error>(400, string(what)); }

}  // namespace

bool parse_http_version(std::string_view v, int& major, int& minor) {
    if (v == "HTTP/1.1") {
        major = 1;
        minor = 1;
        return true;
    }
    if (v == "HTTP/1.0") {
        major = 1;
        minor = 0;
        return true;
    }
    if (!v.starts_with("HTTP/") || v.size() != 8 || v[6] != '.') return false;
    char a = v[5], b = v[7];
    if (a < '0' || a > '9' || b < '0' || b > '9') return false;
    major = a - '0';
    minor = b - '0';
    return true;
}

namespace {

struct empty_body final : io::ReadCloser {
    std::pair<isize, gopp::error> Read(std::span<byte>) override { return {0, io::EOF_}; }
    gopp::error Close() override { return nullptr; }
};

}  // namespace

ptr<io::ReadCloser> no_body() {
    static empty_body* b = [] {
        auto* e = new empty_body();
        e->add_ref();
        return e;
    }();
    return ptr<io::ReadCloser>(b);
}

// ---------------------------------------------------------------------------
// Bodies

std::pair<isize, gopp::error> body_reader::Read(std::span<byte> p) {
    if (closed_) return {0, ErrBodyReadAfterClose};
    if (saw_eof_) return {0, io::EOF_};
    auto [n, err] = src_->Read(p);
    if (err == io::EOF_) {
        saw_eof_ = true;
        if (limited_ && static_cast<io::LimitedReader*>(src_.get())->Remaining() > 0) err = io::ErrUnexpectedEOF;
    } else if (!err && limited_ && static_cast<io::LimitedReader*>(src_.get())->Remaining() == 0) {
        // Report EOF along with the last bytes, so callers needn't read again.
        saw_eof_ = true;
        err = io::EOF_;
        if (n == 0) return {0, io::EOF_};
        return {n, nullptr};
    }
    return {n, err};
}

int64 body_reader::unread_size() const noexcept {
    if (saw_eof_) return 0;
    if (limited_) return static_cast<io::LimitedReader*>(src_.get())->Remaining();
    return -1;
}

gopp::error body_reader::Close() {
    if (closed_) return nullptr;
    closed_ = true;
    if (saw_eof_) return nullptr;
    // Read up to max_post_handler_read_bytes looking for the end, so that
    // the connection can serve another request.
    if (limited_ && static_cast<io::LimitedReader*>(src_.get())->Remaining() > max_post_handler_read_bytes) {
        close_failed_ = true;
        return nullptr;
    }
    byte buf[4096];
    int64 total = 0;
    for (;;) {
        auto [n, err] = src_->Read(buf);
        total += n;
        if (err == io::EOF_) {
            saw_eof_ = true;
            if (limited_ && static_cast<io::LimitedReader*>(src_.get())->Remaining() > 0) close_failed_ = true;
            return nullptr;
        }
        if (err) {
            close_failed_ = true;
            return err;
        }
        if (limited_ && static_cast<io::LimitedReader*>(src_.get())->Remaining() == 0) {
            saw_eof_ = true;
            return nullptr;
        }
        if (total > max_post_handler_read_bytes) {
            close_failed_ = true;
            return nullptr;
        }
    }
}

gopp::error chunked_reader::begin_chunk() {
    auto [line, IsPrefix, err] = r_->ReadLine();
    if (err) return err == io::EOF_ ? io::ErrUnexpectedEOF : err;
    if (IsPrefix) return errors::New("header line too long");
    std::string_view s = as_string_view(line);
    if (auto semi = s.find(';'); semi != s.npos) s = s.substr(0, semi);  // chunk extensions
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    if (s.empty() || s.size() > 16) return errors::New("invalid byte in chunk length");
    uint64 n = 0;
    for (char c : s) {
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return errors::New("invalid byte in chunk length");
        n = n << 4 | static_cast<uint64>(d);
    }
    n_ = n;
    if (n == 0) {
        // Last chunk: skip the trailer up to the empty line.
        for (;;) {
            auto [tl, tp, terr] = r_->ReadLine();
            if (terr) return terr == io::EOF_ ? io::ErrUnexpectedEOF : terr;
            if (tl.empty() && !tp) break;
        }
        done_ = true;
    }
    return nullptr;
}

std::pair<isize, gopp::error> chunked_reader::Read(std::span<byte> p) {
    while (!err_) {
        if (check_end_) {
            auto [a, e1] = r_->ReadByte();
            if (e1) {
                err_ = e1 == io::EOF_ ? io::ErrUnexpectedEOF : e1;
                break;
            }
            auto [b, e2] = r_->ReadByte();
            if (e2) {
                err_ = e2 == io::EOF_ ? io::ErrUnexpectedEOF : e2;
                break;
            }
            if (a != '\r' || b != '\n') {
                err_ = errors::New("malformed chunked encoding");
                break;
            }
            check_end_ = false;
        }
        if (done_) return {0, io::EOF_};
        if (n_ == 0) {
            err_ = begin_chunk();
            continue;
        }
        if (p.empty()) return {0, nullptr};
        std::size_t m = static_cast<std::size_t>(std::min<uint64>(p.size(), n_));
        auto [n, err] = r_->Read(p.first(m));
        n_ -= static_cast<uint64>(n);
        if (n_ == 0) check_end_ = true;
        if (err == io::EOF_) err = io::ErrUnexpectedEOF;
        if (err) err_ = err;
        return {n, err};
    }
    return {0, err_};
}

// ---------------------------------------------------------------------------
// Reading a request

gopp::error read_header_block(const ptr<bufio::Reader>& br, int64 max_header_bytes, string& first_line, Header& header) {
    // Copy the first line and the header lines into one buffer; all the
    // strings are then slices of one shared allocation.
    std::string block;
    block.reserve(512);
    struct span_t {
        std::size_t off, len;
    };
    std::vector<span_t> lines;
    lines.reserve(16);
    int64 budget = max_header_bytes + 4096;

    for (;;) {
        std::size_t start = block.size();
        for (;;) {
            auto [line, IsPrefix, err] = br->ReadLine();
            if (err) {
                if (lines.empty() && block.empty() && err == io::EOF_) return io::EOF_;
                return err == io::EOF_ ? io::ErrUnexpectedEOF : err;
            }
            block.append(reinterpret_cast<const char*>(line.data()), line.size());
            budget -= static_cast<int64>(line.size()) + 2;
            if (budget < 0) return err_header_too_large;
            if (!IsPrefix) break;
        }
        std::size_t len = block.size() - start;
        if (len == 0) {
            if (lines.empty()) {
                first_line = {};
                return nullptr;  // the caller rejects the empty first line
            }
            break;  // end of header
        }
        lines.push_back({start, len});
    }

    string shared(block);
    auto line_at = [&](std::size_t i) { return shared.slice(lines[i].off, lines[i].off + lines[i].len); };
    first_line = line_at(0);

    for (std::size_t i = 1; i < lines.size(); i++) {
        string line = line_at(i);
        if (line[0] == ' ' || line[0] == '\t') {
            return bad_request("malformed MIME header: unexpected continuation line");
        }
        auto colon = line.find(':');
        if (colon == line.npos) return bad_request("malformed MIME header line: " + line);
        std::string_view key = line.view().substr(0, colon);
        if (!valid_header_field_name(key)) return bad_request("malformed MIME header line: " + line);
        std::size_t vs = colon + 1, ve = line.size();
        while (vs < ve && (line[vs] == ' ' || line[vs] == '\t')) vs++;
        while (ve > vs && (line[ve - 1] == ' ' || line[ve - 1] == '\t')) ve--;
        string value = line.slice(vs, ve);
        if (!valid_header_field_value(value)) return bad_request("invalid header field value for " + strconv::Quote(key));
        string ckey = CanonicalHeaderKey(key);
        if (ckey.view() == key) ckey = line.slice(0, colon);  // share the buffer
        header.AddCanonical(std::move(ckey), std::move(value));
    }
    return nullptr;
}

std::pair<ptr<Request>, gopp::error> read_request(const ptr<bufio::Reader>& br, int64 max_header_bytes) {
    auto req = make_ptr<Request>();
    string rl;
    if (gopp::error err = read_header_block(br, max_header_bytes, rl, req->Header)) return {nullptr, err};
    if (rl.empty()) return {nullptr, bad_request("malformed HTTP request \"\"")};

    // Request line: METHOD SP request-target SP HTTP-version
    auto sp1 = rl.find(' ');
    auto sp2 = sp1 == rl.npos ? rl.npos : rl.find(' ', sp1 + 1);
    if (sp1 == rl.npos || sp2 == rl.npos) return {nullptr, bad_request("malformed HTTP request " + strconv::Quote(rl))};
    req->Method = rl.slice(0, sp1);
    req->RequestURI = rl.slice(sp1 + 1, sp2);
    req->Proto = rl.slice(sp2 + 1);
    if (!valid_method(req->Method)) return {nullptr, bad_request("invalid method " + strconv::Quote(req->Method))};
    if (!parse_http_version(req->Proto, req->ProtoMajor, req->ProtoMinor))
        return {nullptr, bad_request("malformed HTTP version " + strconv::Quote(req->Proto))};
    if (req->ProtoMajor != 1) {
        return {nullptr, make_ptr<status_error>(505, string("unsupported protocol version"))};
    }

    bool just_authority = req->Method == "CONNECT" && !req->RequestURI.starts_with("/");
    std::string uri_for_parse(req->RequestURI.view());
    if (just_authority) uri_for_parse = "http://" + uri_for_parse;
    auto [u, uerr] = url::ParseRequestURI(uri_for_parse);
    if (uerr) return {nullptr, bad_request(uerr.Error())};
    if (just_authority) u.Scheme = {};
    req->URL = std::move(u);

    // Host: from an absolute request-target, else the Host header.
    std::vector<string> hosts = req->Header.Values("Host");
    if (req->ProtoAtLeast(1, 1) && hosts.size() != 1) {
        return {nullptr, bad_request(hosts.empty() ? "missing required Host header" : "too many Host headers")};
    }
    req->Host = !req->URL.Host.empty() ? req->URL.Host : (hosts.empty() ? string() : hosts[0]);

    // Connection persistence.
    string conn = req->Header.Get("Connection");
    if (req->ProtoMajor == 1 && req->ProtoMinor == 0) {
        req->Close = !has_token(conn, "keep-alive");
    } else {
        req->Close = has_token(conn, "close");
    }

    // Body framing.
    std::vector<string> te = req->Header.Values("Transfer-Encoding");
    if (!te.empty()) {
        if (te.size() != 1 || !ascii::EqualFold(te[0], "chunked")) {
            return {nullptr, make_ptr<status_error>(501, "unsupported transfer encoding: " + strconv::Quote(te[0]))};
        }
        req->TransferEncoding = {string::from_static("chunked")};
        req->Header.Del("Content-Length");  // RFC 9112: Transfer-Encoding overrides
        req->ContentLength = -1;
        req->Body = make_ptr<body_reader>(make_ptr<chunked_reader>(br), false);
        return {req, nullptr};
    }
    std::vector<string> cl = req->Header.Values("Content-Length");
    if (!cl.empty()) {
        for (auto& v : cl) {
            if (strings::TrimSpace(v) != strings::TrimSpace(cl[0])) return {nullptr, bad_request("invalid Content-Length")};
        }
        auto [n, perr] = strconv::ParseUint(strings::TrimSpace(cl[0]).view(), 10, 63);
        if (perr) return {nullptr, bad_request("bad Content-Length " + strconv::Quote(cl[0]))};
        req->ContentLength = static_cast<int64>(n);
    }
    if (req->ContentLength > 0) {
        req->Body = make_ptr<body_reader>(make_ptr<io::LimitedReader>(br, req->ContentLength), true);
    } else {
        req->Body = no_body();
    }
    return {req, nullptr};
}

// ---------------------------------------------------------------------------
// Paths

string clean_path(const string& p) {
    // path.Clean: lexical processing of "." and ".." elements and repeated
    // slashes; then put back a trailing slash.
    if (p.empty()) return string::from_static("/");
    std::string_view in = p;
    std::string rooted;
    if (in[0] != '/') {
        rooted = "/" + std::string(in);
        in = rooted;
    }
    std::string out;
    out.reserve(in.size());
    std::size_t r = 0, n = in.size();
    out.push_back('/');
    r = 1;
    std::size_t dotdot = 1;
    while (r < n) {
        if (in[r] == '/') {
            r++;
        } else if (in[r] == '.' && (r + 1 == n || in[r + 1] == '/')) {
            r++;
        } else if (in[r] == '.' && in[r + 1] == '.' && (r + 2 == n || in[r + 2] == '/')) {
            r += 2;
            if (out.size() > dotdot) {
                while (out.size() > dotdot && out.back() != '/') out.pop_back();
                if (out.size() > 1) out.pop_back();
            }
        } else {
            if (out.size() != 1) out.push_back('/');
            for (; r < n && in[r] != '/'; r++) out.push_back(in[r]);
        }
    }
    // Trailing slash, except for the root.
    if (in.back() == '/' && out != "/") out.push_back('/');
    if (out == in) return rooted.empty() ? p : string(out);
    return string(out);
}

}  // namespace internal

}  // namespace gopp::http
