// The net/textproto package, ported from Go 1.27.3's net/textproto: header.go,
// reader.go, writer.go, pipeline.go and textproto.go.
#include <gopp/chan.hpp>
#include <gopp/net.hpp>
#include <gopp/net/textproto.hpp>
#include <gopp/strings.hpp>
#include <gopp/sync.hpp>

#include <cstring>
#include <limits>
#include <mutex>
#include <unordered_map>

namespace gopp::textproto {

const error errMessageTooLarge = errors::New("message too large");

namespace {

bool isASCIISpace(byte b) { return b == ' ' || b == '\t' || b == '\n' || b == '\r'; }

bool isASCIILetter(byte b) {
    b |= 0x20;  // make lower case
    return 'a' <= b && b <= 'z';
}

// validHeaderFieldByte reports whether c is a valid byte in a header field
// name: RFC 7230's tchar.
bool validHeaderFieldByte(byte c) {
    if ('0' <= c && c <= '9') return true;
    if ('a' <= c && c <= 'z') return true;
    if ('A' <= c && c <= 'Z') return true;
    switch (c) {
    case '!': case '#': case '$': case '%': case '&': case '\'': case '*': case '+': case '-': case '.':
    case '^': case '_': case '`': case '|': case '~':
        return true;
    default:
        return false;
    }
}

// validHeaderValueByte reports whether c is a valid byte in a header field
// value: RFC 7230's field-content, including obs-text (0x80 and up), but not
// control characters other than tab.
bool validHeaderValueByte(byte c) { return !((c < 0x20 && c != '\t') || c == 0x7f); }

constexpr byte toLower = 'a' - 'A';

// The canonical form of the header key a, which it may modify, and whether
// it is a valid key.
std::pair<string, bool> canonicalMIMEHeaderKey(std::span<byte> a) {
    if (a.empty()) return {string(), false};
    // See if a looks like a header key. If not, return it unchanged.
    bool noCanon = false;
    for (byte c : a) {
        if (validHeaderFieldByte(c)) continue;
        // Don't canonicalize.
        if (c == ' ') {
            // We accept invalid headers with a space before the colon, but
            // must not canonicalize them. See Go's issue 34540.
            noCanon = true;
            continue;
        }
        return {String(a), false};
    }
    if (noCanon) return {String(a), true};

    bool upper = true;
    for (byte& c : a) {
        // Canonicalize: first letter upper case and upper case after each
        // dash. (Host, User-Agent, If-Modified-Since). MIME headers are
        // ASCII only, so no Unicode issues.
        if (upper && 'a' <= c && c <= 'z') {
            c -= toLower;
        } else if (!upper && 'A' <= c && c <= 'Z') {
            c += toLower;
        }
        upper = c == '-';  // for next time
    }
    return {String(a), true};
}

std::span<const byte> trim(std::span<const byte> s) {
    std::size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
    s = s.subspan(i);
    std::size_t n = s.size();
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t')) n--;
    return s.first(n);
}

error noValidation(std::span<const byte>) { return nullptr; }

// mustHaveFieldNameColon ensures that, per RFC 7230, the field-name is on a
// single line, so the first line must contain a colon.
error mustHaveFieldNameColon(std::span<const byte> line) {
    if (asStringView(line).find(':') == std::string_view::npos) {
        return make_ptr<ProtocolError>("malformed MIME header: missing colon: " + strconv::Quote(asStringView(line)));
    }
    return nullptr;
}

error protocolError(std::string_view what, std::span<const byte> line) {
    return make_ptr<ProtocolError>(string(what) + strconv::Quote(asStringView(line)));
}

}  // namespace

// ---------------------------------------------------------------------------
// MIMEHeader

string CanonicalMIMEHeaderKey(std::string_view s) {
    // Quick check for canonical encoding.
    bool upper = true;
    for (char ch : s) {
        byte c = byte(ch);
        if (!validHeaderFieldByte(c)) return string(s);
        if ((upper && 'a' <= c && c <= 'z') || (!upper && 'A' <= c && c <= 'Z')) {
            std::vector<byte> a(s.begin(), s.end());
            return canonicalMIMEHeaderKey(a).first;
        }
        upper = c == '-';
    }
    return string(s);
}

void MIMEHeader::Add(std::string_view key, string value) {
    auto k = CanonicalMIMEHeaderKey(key);
    auto it = find(k);
    if (it == end()) it = emplace(std::move(k), std::vector<string>{}).first;
    it->second.push_back(std::move(value));
}

void MIMEHeader::Set(std::string_view key, string value) {
    (*this)[CanonicalMIMEHeaderKey(key)] = {std::move(value)};
}

string MIMEHeader::Get(std::string_view key) const {
    auto it = find(CanonicalMIMEHeaderKey(key).view());
    if (it == end() || it->second.empty()) return {};
    return it->second.front();
}

std::vector<string> MIMEHeader::Values(std::string_view key) const {
    auto it = find(CanonicalMIMEHeaderKey(key).view());
    if (it == end()) return {};
    return it->second;
}

void MIMEHeader::Del(std::string_view key) {
    if (auto it = find(CanonicalMIMEHeaderKey(key).view()); it != end()) erase(it);
}

// ---------------------------------------------------------------------------
// Errors, trimming

string Error::message() const { return string(std::format("{:03d} ", Code)) + strconv::Quote(Msg); }

string TrimString(const string& s) {
    std::size_t i = 0, j = s.size();
    while (i < j && isASCIISpace(byte(s[i]))) i++;
    while (j > i && isASCIISpace(byte(s[j - 1]))) j--;
    return s.slice(i, j);
}

slice<byte> TrimBytes(const slice<byte>& b) {
    std::size_t i = 0, j = b.len();
    while (i < j && isASCIISpace(b[i])) i++;
    while (j > i && isASCIISpace(b[j - 1])) j--;
    return b.Sub(i, j);
}

// ---------------------------------------------------------------------------
// Reader

// A Reader of a dot-encoded block; see Reader::DotReader.
class dotReader final : public io::Reader {
public:
    explicit dotReader(textproto::Reader* r) : r_(r) {}

    std::pair<isize, error> Read(std::span<byte> b) override;

private:
    friend class textproto::Reader;
    enum : int {
        stateBeginLine,  // beginning of line; initial state; must be zero
        stateDot,        // read . at beginning of line
        stateDotCR,      // read .\r at beginning of line
        stateCR,         // read \r (possibly at end of line)
        stateData,       // reading data in middle of line
        stateEOF,        // reached .\r\n end marker line
    };
    textproto::Reader* r_;  // nil once the Reader is gone
    int state_ = stateBeginLine;
};

std::pair<isize, error> dotReader::Read(std::span<byte> b) {
    // Run data through a simple state machine to elide leading dots, rewrite
    // trailing \r\n into \n, and detect the ending .\r\n line.
    if (!r_) return {0, io::ErrClosedPipe};
    auto& br = *r_->R;
    isize n = 0;
    error err;
    while (std::size_t(n) < b.size() && state_ != stateEOF) {
        auto [c, rerr] = br.ReadByte();
        if (rerr) {
            err = rerr == io::EOF_ ? io::ErrUnexpectedEOF : rerr;
            break;
        }
        switch (state_) {
        case stateBeginLine:
            if (c == '.') {
                state_ = stateDot;
                continue;
            }
            if (c == '\r') {
                state_ = stateCR;
                continue;
            }
            state_ = stateData;
            break;
        case stateDot:
            if (c == '\r') {
                state_ = stateDotCR;
                continue;
            }
            if (c == '\n') {
                state_ = stateEOF;
                continue;
            }
            state_ = stateData;
            break;
        case stateDotCR:
            if (c == '\n') {
                state_ = stateEOF;
                continue;
            }
            // Not part of .\r\n. Consume leading dot and emit saved \r.
            br.UnreadByte();
            c = '\r';
            state_ = stateData;
            break;
        case stateCR:
            if (c == '\n') {
                state_ = stateBeginLine;
                break;
            }
            // Not part of \r\n. Emit saved \r.
            br.UnreadByte();
            c = '\r';
            state_ = stateData;
            break;
        case stateData:
            if (c == '\r') {
                state_ = stateCR;
                continue;
            }
            if (c == '\n') state_ = stateBeginLine;
            break;
        }
        b[std::size_t(n)] = c;
        n++;
    }
    if (!err && state_ == stateEOF) err = io::EOF_;
    if (err && r_->dot_ == this) {
        r_->dot_ = nullptr;
        r_->dotRef_ = nullptr;  // may destroy this
    }
    return {n, err};
}

Reader::~Reader() {
    if (dot_) dot_->r_ = nullptr;
}

std::pair<std::span<const byte>, error> Reader::readLineSlice(int64 lim) {
    closeDot();
    bool first = true;
    line_.clear();
    for (;;) {
        auto [l, more, err] = R->ReadLine();
        if (err) return {{}, err};
        if (lim >= 0 && int64(line_.size()) + int64(l.size()) > lim) return {{}, errMessageTooLarge};
        // Avoid the copy if the first call produced a full line.
        if (first && !more) return {l, nullptr};
        first = false;
        line_.insert(line_.end(), l.begin(), l.end());
        if (!more) break;
    }
    return {line_, nullptr};
}

std::pair<string, error> Reader::ReadLine() {
    auto [line, err] = readLineSlice(-1);
    return {String(line), err};
}

std::pair<slice<byte>, error> Reader::ReadLineBytes() {
    auto [line, err] = readLineSlice(-1);
    if (err) return {{}, err};
    return {slice<byte>::of(line), nullptr};
}

std::pair<string, error> Reader::ReadContinuedLine() {
    auto [line, err] = readContinuedLineSlice(-1, noValidation);
    return {String(line), err};
}

std::pair<slice<byte>, error> Reader::ReadContinuedLineBytes() {
    auto [line, err] = readContinuedLineSlice(-1, noValidation);
    if (err) return {{}, err};
    return {slice<byte>::of(line), nullptr};
}

// readContinuedLineSlice reads continued lines from the reader buffer,
// returning a byte slice with all lines. The validateFirstLine function is
// run on the first read line, and if it returns an error then this error is
// returned from readContinuedLineSlice. It reads up to lim bytes of data
// (or unlimited if lim is less than 0).
std::pair<std::span<const byte>, error> Reader::readContinuedLineSlice(int64 lim,
                                                                        error (*validateFirstLine)(std::span<const byte>)) {
    // Read the first line.
    auto [line, err] = readLineSlice(lim);
    if (err) return {{}, err};
    if (line.empty()) return {line, nullptr};  // blank line - no continuation
    if (auto verr = validateFirstLine(line)) return {{}, verr};

    // Optimistically assume that we have started to buffer the next line and
    // it starts with an ASCII letter (the next header key), or a blank line,
    // so we can avoid copying that buffered data around in memory and
    // skipping over non-existent whitespace.
    if (R->Buffered() > 1) {
        auto [peek, _] = R->Peek(2);
        if ((!peek.empty() && (isASCIILetter(peek[0]) || peek[0] == '\n')) ||
            (peek.size() == 2 && peek[0] == '\r' && peek[1] == '\n')) {
            return {trim(line), nullptr};
        }
    }

    // ReadByte or the next readLineSlice will flush the read buffer; copy
    // the slice into buf.
    auto t = trim(line);
    buf_.assign(t.begin(), t.end());

    if (lim < 0) lim = std::numeric_limits<int64>::max();
    lim -= int64(buf_.size());

    // Read continuation lines.
    while (skipSpace() > 0) {
        buf_.push_back(' ');
        if (int64(buf_.size()) >= lim) return {{}, errMessageTooLarge};
        auto [l, lerr] = readLineSlice(lim - int64(buf_.size()));
        if (lerr) return {{}, lerr};
        auto tl = trim(l);
        buf_.insert(buf_.end(), tl.begin(), tl.end());
    }
    return {buf_, nullptr};
}

// skipSpace skips R over all spaces and returns the number of bytes skipped.
int Reader::skipSpace() {
    int n = 0;
    for (;;) {
        auto [c, err] = R->ReadByte();
        if (err) break;  // bufio will keep err until next read
        if (c != ' ' && c != '\t') {
            R->UnreadByte();
            break;
        }
        n++;
    }
    return n;
}

namespace {

std::tuple<int, bool, string, error> parseCodeLine(const string& line, int expectCode) {
    if (line.size() < 4 || (line[3] != ' ' && line[3] != '-')) {
        return {0, false, {}, make_ptr<ProtocolError>("short response: " + strconv::Quote(line))};
    }
    bool continued = line[3] == '-';
    auto [code, err] = strconv::Atoi(line.view().substr(0, 3));
    if (err || code < 100) {
        return {int(code), continued, {}, make_ptr<ProtocolError>("invalid response code: " + strconv::Quote(line))};
    }
    string message = line.slice(4);
    if ((1 <= expectCode && expectCode < 10 && code / 100 != expectCode) ||
        (10 <= expectCode && expectCode < 100 && code / 10 != expectCode) ||
        (100 <= expectCode && expectCode < 1000 && code != expectCode)) {
        return {int(code), continued, message, make_ptr<Error>(int(code), message)};
    }
    return {int(code), continued, message, nullptr};
}

}  // namespace

std::tuple<int, bool, string, error> Reader::readCodeLine(int expectCode) {
    auto [line, err] = ReadLine();
    if (err) return {0, false, {}, err};
    return parseCodeLine(line, expectCode);
}

std::tuple<int, string, error> Reader::ReadCodeLine(int expectCode) {
    auto [code, continued, message, err] = readCodeLine(expectCode);
    if (!err && continued) err = make_ptr<ProtocolError>("unexpected multi-line response: " + strconv::Quote(message));
    return {code, message, err};
}

std::tuple<int, string, error> Reader::ReadResponse(int expectCode) {
    auto [code, continued, first, err] = readCodeLine(expectCode);
    bool multi = continued;
    std::string message(first.view());
    while (continued) {
        auto [line, lerr] = ReadLine();
        if (lerr) return {0, {}, lerr};
        auto [code2, cont2, moreMessage, perr] = parseCodeLine(line, 0);
        continued = cont2;
        if (perr || code2 != code) {
            message += '\n';
            auto l = line.view();
            while (!l.empty() && (l.back() == '\r' || l.back() == '\n')) l.remove_suffix(1);
            message += l;
            continued = true;
            continue;
        }
        message += '\n';
        message += moreMessage.view();
    }
    if (err && multi && !message.empty()) {
        // Replace one line error message with all lines (full message).
        err = make_ptr<Error>(code, string(message));
    }
    return {code, string(message), err};
}

ptr<io::Reader> Reader::DotReader() {
    closeDot();
    auto d = make_ptr<dotReader>(this);
    dot_ = d.get();
    dotRef_ = d;
    return d;
}

// closeDot drains the current DotReader if any, making sure that it reads
// until the ending dot line.
void Reader::closeDot() {
    byte buf[128];
    while (dot_) {
        // When Read reaches EOF or an error, it will clear dot_.
        dot_->Read(buf);
    }
}

std::pair<slice<byte>, error> Reader::ReadDotBytes() { return io::ReadAll(*DotReader()); }

std::pair<std::vector<string>, error> Reader::ReadDotLines() {
    // We could use ReadDotBytes and then Split it, but reading a line at a
    // time avoids needing a large contiguous block of memory and is simpler.
    std::vector<string> v;
    error err;
    for (;;) {
        auto [line, lerr] = ReadLine();
        if (lerr) {
            err = lerr == io::EOF_ ? io::ErrUnexpectedEOF : lerr;
            break;
        }
        // Dot by itself marks end; otherwise cut one dot.
        if (!line.empty() && line[0] == '.') {
            if (line.size() == 1) break;
            line = line.slice(1);
        }
        v.push_back(std::move(line));
    }
    return {std::move(v), err};
}

std::pair<MIMEHeader, error> Reader::ReadMIMEHeader() {
    return readMIMEHeader(std::numeric_limits<int64>::max(), std::numeric_limits<int64>::max());
}

std::pair<MIMEHeader, error> Reader::readMIMEHeader(int64 maxMemory, int64 maxHeaders) {
    MIMEHeader m;
    if (maxMemory < 0) return {std::move(m), errMessageTooLarge};

    // The first line cannot start with a leading space.
    if (auto [buf, err] = R->Peek(1); !err && (buf[0] == ' ' || buf[0] == '\t')) {
        constexpr int64 errorLimit = 80;  // arbitrary limit on how much of the line we'll quote
        auto [line, lerr] = readLineSlice(errorLimit);
        if (lerr) return {std::move(m), lerr};
        return {std::move(m), protocolError("malformed MIME header initial line: ", line)};
    }

    std::vector<byte> kvbuf;
    for (;;) {
        auto [kv, err] = readContinuedLineSlice(maxMemory, mustHaveFieldNameColon);
        if (kv.empty()) return {std::move(m), err};

        // Key ends at first colon.
        auto colon = asStringView(kv).find(':');
        if (colon == std::string_view::npos) return {std::move(m), protocolError("malformed MIME header line: ", kv)};
        kvbuf.assign(kv.begin(), kv.end());
        auto [key, ok] = canonicalMIMEHeaderKey(std::span(kvbuf).first(colon));
        if (!ok) return {std::move(m), protocolError("malformed MIME header line: ", kv)};
        auto v = kv.subspan(colon + 1);
        for (byte c : v) {
            if (!validHeaderValueByte(c)) return {std::move(m), protocolError("malformed MIME header line: ", kv)};
        }

        if (--maxHeaders < 0) return {MIMEHeader{}, errMessageTooLarge};

        // Skip initial spaces in value.
        while (!v.empty() && (v[0] == ' ' || v[0] == '\t')) v = v.subspan(1);
        string value = String(v);

        auto it = m.find(key.view());
        if (it == m.end()) {
            // Each key in the map costs some memory: estimate 200 bytes,
            // as Go does.
            constexpr int64 mapEntryOverhead = 200;
            maxMemory -= int64(key.size());
            maxMemory -= mapEntryOverhead;
        }
        maxMemory -= int64(value.size());
        if (maxMemory < 0) return {std::move(m), errMessageTooLarge};
        if (it == m.end()) it = m.emplace(std::move(key), std::vector<string>{}).first;
        it->second.push_back(std::move(value));

        if (err) return {std::move(m), err};
    }
}

// ---------------------------------------------------------------------------
// Writer

// A WriteCloser that dot-encodes; see Writer::DotWriter.
class dotWriter final : public io::WriteCloser {
public:
    explicit dotWriter(textproto::Writer* w) : w_(w) {}

    std::pair<isize, error> Write(std::span<const byte> b) override;
    error Close() override;

private:
    friend class textproto::Writer;
    enum : int {
        wstateBegin,      // initial state; must be zero
        wstateBeginLine,  // beginning of line
        wstateCR,         // wrote \r (possibly at end of line)
        wstateData,       // writing data in middle of line
    };
    textproto::Writer* w_;  // nil once the Writer is gone
    int state_ = wstateBegin;
};

std::pair<isize, error> dotWriter::Write(std::span<const byte> b) {
    if (!w_) return {0, io::ErrClosedPipe};
    auto& bw = *w_->W;
    isize n = 0;
    while (std::size_t(n) < b.size()) {
        byte c = b[std::size_t(n)];
        switch (state_) {
        case wstateBegin:
        case wstateBeginLine:
            state_ = wstateData;
            if (c == '.') {
                // Escape leading dot.
                bw.WriteByte('.');
            }
            [[fallthrough]];
        case wstateData:
            if (c == '\r') state_ = wstateCR;
            if (c == '\n') {
                bw.WriteByte('\r');
                state_ = wstateBeginLine;
            }
            break;
        case wstateCR:
            state_ = wstateData;
            if (c == '\n') state_ = wstateBeginLine;
            break;
        }
        if (auto err = bw.WriteByte(c)) return {n, err};
        n++;
    }
    return {n, nullptr};
}

error dotWriter::Close() {
    if (!w_) return nullptr;
    auto* w = w_;
    auto& bw = *w->W;
    switch (state_) {
    default:
        bw.WriteByte('\r');
        [[fallthrough]];
    case wstateCR:
        bw.WriteByte('\n');
        [[fallthrough]];
    case wstateBeginLine:
        bw.WriteString(".\r\n");
    }
    auto err = bw.Flush();
    if (w->dot_ == this) {
        w_ = nullptr;
        w->dot_ = nullptr;
        w->dotRef_ = nullptr;  // may destroy this
    } else {
        w_ = nullptr;
    }
    return err;
}

Writer::~Writer() {
    if (dot_) dot_->w_ = nullptr;
}

error Writer::printLine(std::string_view line) {
    closeDot();
    W->WriteString(line);
    W->WriteString("\r\n");
    return W->Flush();
}

ptr<io::WriteCloser> Writer::DotWriter() {
    closeDot();
    auto d = make_ptr<dotWriter>(this);
    dot_ = d.get();
    dotRef_ = d;
    return d;
}

void Writer::closeDot() {
    if (dot_) dot_->Close();  // sets dot_ = nullptr
}

// ---------------------------------------------------------------------------
// Pipeline

// A sequencer schedules a sequence of numbered events that must happen in
// order, one after the other.
struct Pipeline::sequencer {
    sync::Mutex mu;
    uint64 id = 0;
    std::unordered_map<uint64, chan<empty>> wait;

    // Start waits until it is time for the event numbered id to begin. That
    // is, except for the first event, it waits until End(id-1) has been
    // called.
    void Start(uint64 id_) {
        mu.lock();
        if (id == id_) {
            mu.unlock();
            return;
        }
        auto c = makeChan<empty>();
        wait[id_] = c;
        mu.unlock();
        c.recv();
    }

    // End notifies the sequencer that the event numbered id has completed,
    // allowing it to schedule the event numbered id+1. It is a run-time
    // error to call End with an id that is not the current one.
    void End(uint64 id_) {
        mu.lock();
        if (id != id_) {
            mu.unlock();
            panic("out of sync");
        }
        id = ++id_;
        chan<empty> c;
        if (auto it = wait.find(id_); it != wait.end()) {
            c = it->second;
            wait.erase(it);
        }
        mu.unlock();
        if (c) c.close();
    }
};

struct Pipeline::impl {
    sync::Mutex mu;
    uint64 id = 0;
    sequencer request;
    sequencer response;
};

Pipeline::Pipeline() : impl_(std::make_unique<impl>()) {}
Pipeline::~Pipeline() = default;

uint64 Pipeline::Next() {
    std::lock_guard l(impl_->mu);
    return impl_->id++;
}

void Pipeline::StartRequest(uint64 id) { impl_->request.Start(id); }
void Pipeline::EndRequest(uint64 id) { impl_->request.End(id); }
void Pipeline::StartResponse(uint64 id) { impl_->response.Start(id); }
void Pipeline::EndResponse(uint64 id) { impl_->response.End(id); }

// ---------------------------------------------------------------------------
// Conn

Conn::Conn(ptr<io::ReadWriteCloser> conn)
    : textproto::Reader(make_ptr<bufio::Reader>(conn)), textproto::Writer(make_ptr<bufio::Writer>(conn)), conn_(conn) {}

error Conn::Close() { return conn_->Close(); }

std::pair<uint64, error> Conn::cmd(std::string_view line) {
    uint64 id = Pipeline.Next();
    Pipeline.StartRequest(id);
    auto err = printLine(line);
    Pipeline.EndRequest(id);
    if (err) return {0, err};
    return {id, nullptr};
}

namespace {

// A net::Conn as an io::ReadWriteCloser.
struct netConn final : io::ReadWriteCloser {
    explicit netConn(ptr<net::Conn> c) : c_(std::move(c)) {}
    std::pair<isize, error> Read(std::span<byte> p) override { return c_->Read(p); }
    std::pair<isize, error> Write(std::span<const byte> p) override { return c_->Write(p); }
    error Close() override { return c_->Close(); }
    ptr<net::Conn> c_;
};

}  // namespace

std::pair<ptr<Conn>, error> Dial(std::string_view network, std::string_view addr) {
    auto [c, err] = net::Dial(network, addr);
    if (err) return {nullptr, err};
    return {NewConn(make_ptr<netConn>(std::move(c))), nullptr};
}

}  // namespace gopp::textproto
