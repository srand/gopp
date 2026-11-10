// Internals shared by the net/http sources.
#pragma once

#include <gopp/bufio.hpp>
#include <gopp/net/http.hpp>

#include <string_view>

namespace gopp::http::internal {

// RFC 7230 token characters.
bool is_token_char(unsigned char c) noexcept;
bool valid_header_field_name(std::string_view s) noexcept;
bool valid_header_field_value(std::string_view s) noexcept;
bool valid_method(std::string_view m) noexcept;
// Whether the comma-separated header value v contains token (ignoring case).
bool has_token(std::string_view v, std::string_view token) noexcept;

// A request-reading failure that the server answers with a status.
struct status_error : error_interface {
    int code;
    string text;
    status_error(int c, string t) : code(c), text(std::move(t)) {}
    string Error() const override { return text; }
};

extern const gopp::error err_header_too_large;

// Reads a first line and header fields up to the empty line.
gopp::error read_header_block(const ptr<bufio::Reader>& br, int64 max_header_bytes, string& first_line, Header& header);
bool parse_http_version(std::string_view v, int& major, int& minor);

// Reads a request from br. Its body reads from br too.
std::pair<ptr<Request>, gopp::error> read_request(const ptr<bufio::Reader>& br, int64 max_header_bytes);

// Logs msg with a timestamp to stderr, like Go's log package.
void logf(std::string_view msg);

// Path cleaning, after path.Clean, keeping a trailing slash.
string clean_path(const string& p);

// Appends an RFC 1123 date for now, cached per second.
void append_date(std::string& out);

// The request body as the server provides it.
class body_reader final : public io::ReadCloser {
public:
    // src reads the body's bytes (limited or dechunked).
    body_reader(ptr<io::Reader> src, bool limited) : src_(std::move(src)), limited_(limited) {}

    std::pair<isize, gopp::error> Read(std::span<byte> p) override;
    // Discards up to max unread bytes so that the connection can be reused.
    gopp::error Close() override;

    bool saw_eof() const noexcept { return saw_eof_; }
    bool closed() const noexcept { return closed_; }
    // Bytes left for a Content-Length body; -1 if unknown (chunked).
    int64 unread_size() const noexcept;
    bool close_failed() const noexcept { return close_failed_; }

private:
    ptr<io::Reader> src_;
    bool limited_;
    bool saw_eof_ = false;
    bool closed_ = false;
    bool close_failed_ = false;
};

// Reads the chunked transfer encoding (after net/http/internal/chunked.Go).
class chunked_reader final : public io::Reader {
public:
    explicit chunked_reader(ptr<bufio::Reader> r) : r_(std::move(r)) {}
    std::pair<isize, gopp::error> Read(std::span<byte> p) override;

private:
    gopp::error begin_chunk();
    ptr<bufio::Reader> r_;
    uint64 n_ = 0;          // bytes left in the current chunk
    bool check_end_ = false;  // expect CRLF after the chunk data
    bool done_ = false;
    gopp::error err_;
};

// An empty body (http.NoBody).
ptr<io::ReadCloser> no_body();

// ---------------------------------------------------------------------------
// HTTP/2 (http2_server.cpp, http2_transport.cpp)

// Whether HTTP/2 is offered with ALPN and accepted with prior knowledge.
inline constexpr bool http2_enabled = false;  // the server is written, not yet tested
// Whether the client offers h2; off until the HTTP/2 transport exists.
inline constexpr bool http2_client_enabled = false;

// The client connection preface.
inline constexpr std::string_view http2_preface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";

struct server_options {
    time::Duration ReadTimeout{};
    time::Duration WriteTimeout{};
    time::Duration IdleTimeout{};
    int64 MaxHeaderBytes = 0;
    bool DisableKeepAlives = false;
    std::shared_ptr<detail::server_state> state;
    // Reports whether the connection has open streams, for shutdown().
    std::function<void(bool active)> set_active;
    // Registers a function that starts a graceful shutdown (GOAWAY) when
    // the server shuts down; nullptr unregisters it.
    std::function<void(std::function<void()>)> on_shutdown;
};

// Serves HTTP/2 on c, whose reads go through br.
void serve_http2(const ptr<net::Conn>& c, const ptr<bufio::Reader>& br, const ptr<Handler>& handler,
                 const server_options& opts, std::optional<tls::ConnectionState> tls_state);

}  // namespace gopp::http::internal
