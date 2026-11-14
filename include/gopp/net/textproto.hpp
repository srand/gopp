// The net/textproto package: the text protocols in the style of HTTP,
// NNTP and SMTP, ported from Go 1.27.3. MIME headers, reading lines,
// continued header lines, status code lines and dot-encoded blocks.
//
//     auto r = textproto::NewReader(make_ptr<bufio::Reader>(conn));
//     auto [header, err] = r->ReadMIMEHeader();
//     header.Get("Content-Type");
//
//     auto [c, err] = textproto::Dial("tcp", "mail.example.com:25");
//     auto [code, msg, err] = c->ReadResponse(220);
//     c->PrintfLine("HELO {}", host);
#pragma once

// The module interfaces (modules/) import these instead.
#ifndef GOPP_MODULE
#include <gopp/bufio.hpp>
#include <gopp/errors.hpp>
#include <gopp/io.hpp>
#include <gopp/ptr.hpp>
#include <gopp/slice.hpp>
#include <gopp/string.hpp>

#include <format>
#include <functional>
#include <map>
#include <memory>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>
#endif

namespace gopp::textproto {

// textproto.CanonicalMIMEHeaderKey: "accept-encoding" -> "Accept-Encoding".
// A key with a space or an invalid byte is returned unchanged.
string CanonicalMIMEHeaderKey(std::string_view s);

// textproto.MIMEHeader: a MIME-style header, the values of each canonical
// key, as Go's map[string][]string. Keys are kept sorted.
class MIMEHeader : public std::map<string, std::vector<string>, std::less<>> {
public:
    using std::map<string, std::vector<string>, std::less<>>::map;

    // Appends value to the values of key.
    void Add(std::string_view key, string value);
    // Replaces the values of key with value.
    void Set(std::string_view key, string value);
    // The first value of key, or "".
    string Get(std::string_view key) const;
    // The values of key, empty if there are none.
    std::vector<string> Values(std::string_view key) const;
    // Deletes the values of key.
    void Del(std::string_view key);
};

namespace detail {
// Implements Error() for an error type named Error, which can't declare it.
template <typename E>
struct namedError : errorInterface {
    string Error() const override { return static_cast<const E&>(*this).message(); }
};
}  // namespace detail

// textproto.Error: a numeric error response from a server.
struct Error : detail::namedError<Error> {
    Error(int code, string msg) : Code(code), Msg(std::move(msg)) {}
    // The message, as Error() returns it: "550 \"no such user\"".
    string message() const;
    int Code;
    string Msg;
};

// textproto.ProtocolError: a protocol violation, such as a malformed
// response or header.
struct ProtocolError : errorInterface {
    explicit ProtocolError(string msg) : msg_(std::move(msg)) {}
    string Error() const override { return msg_; }

private:
    string msg_;
};

// textproto.Reader: reads the lines of a text protocol from R.
class Reader : public virtual object {
public:
    explicit Reader(ptr<bufio::Reader> r) : R(std::move(r)) {}
    ~Reader() override;

    ptr<bufio::Reader> R;

    // Reads a line, without its \n or \r\n.
    std::pair<string, error> ReadLine();
    // ReadLine as bytes, a copy.
    std::pair<slice<byte>, error> ReadLineBytes();
    // Reads a line and the lines that continue it, those starting with a
    // space or tab, joined by single spaces and trimmed: "Line 1\n  continued"
    // reads as "Line 1 continued". An empty line ends the input.
    std::pair<string, error> ReadContinuedLine();
    std::pair<slice<byte>, error> ReadContinuedLineBytes();
    // Reads a status line "code message", as "220 plaintext": the code, and
    // the message. With expectCode 1 to 9, the code's first digit must be
    // it; 10 to 99, its first two; 100 to 999, all of it; otherwise any code
    // is accepted. A wrong code is an *Error. A continued line ("220-")
    // is a ProtocolError.
    std::tuple<int, string, error> ReadCodeLine(int expectCode);
    // Reads a multi-line response, "code-message" lines ended by a
    // "code message" line, as RFC 959 has it (FTP), and returns the lines'
    // messages joined by newlines. The code is checked as by ReadCodeLine;
    // a wrong one is an *Error with the whole message.
    std::tuple<int, string, error> ReadResponse(int expectCode);
    // A Reader of a dot-encoded block, as SMTP sends message bodies: lines
    // ended by \r\n read as ended by \n, a leading ".." as ".", and a line
    // "." ends the block (io::EOF_). Reading from r again first reads the
    // rest of the block.
    ptr<io::Reader> DotReader();
    // Reads all of a dot-encoded block.
    std::pair<slice<byte>, error> ReadDotBytes();
    // Reads the lines of a dot-encoded block, without their ends and the
    // final ".".
    std::pair<std::vector<string>, error> ReadDotLines();
    // Reads a MIME header: lines "Key: value" up to an empty line, with
    // continued lines joined. Keys are made canonical. On an error, the
    // header read so far is returned with it.
    std::pair<MIMEHeader, error> ReadMIMEHeader();

    // readMIMEHeader with limits: the bytes of the header (counting 200 per
    // key) and the number of lines. errMessageTooLarge past them. For
    // mime/multipart and net/http.
    std::pair<MIMEHeader, error> readMIMEHeader(int64 maxMemory, int64 maxHeaders);

private:
    friend class dotReader;
    std::pair<std::span<const byte>, error> readLineSlice(int64 lim);
    std::pair<std::span<const byte>, error> readContinuedLineSlice(int64 lim,
                                                                    error (*validateFirstLine)(std::span<const byte>));
    int skipSpace();
    std::tuple<int, bool, string, error> readCodeLine(int expectCode);
    int upcomingHeaderKeys();
    void closeDot();

    class dotReader* dot_ = nullptr;
    ptr<io::Reader> dotRef_;  // keeps dot_ alive while it is current
    std::vector<byte> line_;  // the line of readLineSlice, when it spans buffers
    std::vector<byte> buf_;   // a reusable buffer for readContinuedLineSlice
};

// textproto.NewReader. To avoid denial of service attacks, r should be
// wrapped in an io::LimitReader or similar if the input could be large.
inline ptr<Reader> NewReader(ptr<bufio::Reader> r) { return make_ptr<Reader>(std::move(r)); }

// The error ReadMIMEHeader's limits return: "message too large".
extern const error errMessageTooLarge;

// textproto.Writer: writes the lines of a text protocol to W.
class Writer : public virtual object {
public:
    explicit Writer(ptr<bufio::Writer> w) : W(std::move(w)) {}
    ~Writer() override;

    ptr<bufio::Writer> W;

    // Writes the formatted line, ended by \r\n, and flushes.
    template <typename... A>
    error PrintfLine(std::format_string<A...> f, A&&... a) {
        return printLine(std::format(f, std::forward<A>(a)...));
    }
    // A WriteCloser that dot-encodes what's written to W: \n line ends as
    // \r\n, a leading "." as "..". Close ends the block with ".\r\n" and
    // flushes.
    ptr<io::WriteCloser> DotWriter();

protected:
    error printLine(std::string_view line);

private:
    friend class dotWriter;
    void closeDot();

    class dotWriter* dot_ = nullptr;
    ptr<io::WriteCloser> dotRef_;
};

// textproto.NewWriter
inline ptr<Writer> NewWriter(ptr<bufio::Writer> w) { return make_ptr<Writer>(std::move(w)); }

// textproto.Pipeline: puts the requests and responses of a pipelined
// protocol in order. A client calls Next for an id, then StartRequest(id),
// sends the request, EndRequest(id); then StartResponse(id), reads the
// response, EndResponse(id). The Start calls wait for their turn.
class Pipeline {
public:
    Pipeline();
    ~Pipeline();
    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;

    // The next id for a request/response pair.
    uint64 Next();
    void StartRequest(uint64 id);
    void EndRequest(uint64 id);
    void StartResponse(uint64 id);
    void EndResponse(uint64 id);

    struct sequencer;

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

// textproto.Conn: a Reader and Writer over a connection, with a Pipeline
// for concurrent clients.
class Conn final : public Reader, public Writer {
public:
    explicit Conn(ptr<io::ReadWriteCloser> conn);

    textproto::Pipeline Pipeline;

    // Sends the formatted command, after waiting for its turn, and returns
    // its id, to wait for the response with StartResponse(id):
    //
    //     auto [id, err] = c->Cmd("USER {}", user);
    //     c->StartResponse(id);
    //     auto [code, msg, err2] = c->ReadCodeLine(331);
    //     c->EndResponse(id);
    template <typename... A>
    std::pair<uint64, error> Cmd(std::format_string<A...> f, A&&... a) {
        return cmd(std::format(f, std::forward<A>(a)...));
    }
    // Waits for the turn of the response id.
    void StartResponse(uint64 id) { Pipeline.StartResponse(id); }
    void EndResponse(uint64 id) { Pipeline.EndResponse(id); }
    // Closes the connection.
    error Close();

private:
    std::pair<uint64, error> cmd(std::string_view line);
    ptr<io::ReadWriteCloser> conn_;
};

// textproto.NewConn
inline ptr<Conn> NewConn(ptr<io::ReadWriteCloser> conn) { return make_ptr<Conn>(std::move(conn)); }

// textproto.Dial: connects with net::Dial and returns a Conn over it.
std::pair<ptr<Conn>, error> Dial(std::string_view network, std::string_view addr);

// textproto.TrimString: s without leading and trailing ASCII space.
string TrimString(const string& s);
// textproto.TrimBytes
slice<byte> TrimBytes(const slice<byte>& b);

}  // namespace gopp::textproto
