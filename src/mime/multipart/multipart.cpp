// The mime/multipart package, ported from Go 1.27.3's mime/multipart:
// multipart.go, formdata.go and writer.go.
#include <gopp/bytes.hpp>
#include <gopp/crypto/rand.hpp>
#include <gopp/encoding/hex.hpp>
#include <gopp/fmt.hpp>
#include <gopp/mime.hpp>
#include <gopp/mime/multipart.hpp>
#include <gopp/mime/quotedprintable.hpp>
#include <gopp/os.hpp>
#include <gopp/path/filepath.hpp>
#include <gopp/strings.hpp>

#include <algorithm>
#include <cstring>
#include <limits>

namespace gopp::multipart {

const error ErrMessageTooLarge = errors::New("multipart: message too large");

namespace {

constexpr std::size_t peekBufferSize = 4096;

// The limits of NextPart and ReadForm: Go's defaults of
// GODEBUG=multipartmaxheaders and multipartmaxparts.
constexpr int64 maxMIMEHeaderSize = 10 << 20;
constexpr int64 maxMIMEHeaders = 10000;
constexpr int maxParts = 1000;

// A Reader that keeps returning the first error it got, so that bufio
// doesn't read again after an error.
class stickyErrorReader final : public io::Reader {
public:
    explicit stickyErrorReader(ptr<io::Reader> r) : r_(std::move(r)) {}
    std::pair<isize, error> Read(std::span<byte> p) override {
        if (err_) return {0, err_};
        auto [n, err] = r_->Read(p);
        err_ = err;
        return {n, err};
    }

private:
    ptr<io::Reader> r_;
    error err_;
};

bool hasPrefix(std::span<const byte> b, std::string_view prefix) { return asStringView(b).starts_with(prefix); }

bool hasPrefix(std::string_view s, std::span<const byte> prefix) { return s.starts_with(asStringView(prefix)); }

std::span<const byte> skipLWSPChar(std::span<const byte> b) {
    while (!b.empty() && (b[0] == ' ' || b[0] == '\t')) b = b.subspan(1);
    return b;
}

// matchAfterPrefix checks whether buf should be considered to match the
// boundary. The prefix is "--boundary" or "\r\n--boundary" or
// "\n--boundary", and the caller has verified already that
// hasPrefix(buf, prefix) is true.
//
// matchAfterPrefix returns +1 if the buffer does match the boundary,
// meaning the prefix is followed by a double dash, space, tab, cr, nl, or
// end of input. It returns -1 if the buffer definitely does NOT match the
// boundary, meaning the prefix is followed by some other character. For
// example, "--foobar" does not match "--foo". It returns 0 more input needs
// to be read to make the decision, meaning that len(buf) == len(prefix) and
// readErr == nil.
int matchAfterPrefix(std::span<const byte> buf, std::string_view prefix, const error& readErr) {
    if (buf.size() == prefix.size()) return readErr ? +1 : 0;
    byte c = buf[prefix.size()];
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') return +1;
    // Try to detect boundaryDash.
    if (c == '-') {
        if (buf.size() == prefix.size() + 1) return readErr ? -1 : 0;  // prefix + "-" does not match
        if (buf[prefix.size() + 1] == '-') return +1;
    }
    return -1;
}

// scanUntilBoundary scans buf to identify how much of it can be safely
// returned as part of the Part body. dashBoundary is "--boundary".
// nlDashBoundary is "\r\n--boundary" or "\n--boundary", depending on what
// mode we are in. The comments below (and the name) assume
// "\n--boundary", but either is accepted. total is the number of bytes
// read out so far. If total == 0, then a leading "--boundary" is
// recognized. readErr is the read error, if any, that followed reading the
// bytes in buf. scanUntilBoundary returns the number of data bytes from
// buf that can be returned as part of the Part body and also the error to
// return (if any) once those data bytes are done.
std::pair<isize, error> scanUntilBoundary(std::span<const byte> buf, std::string_view dashBoundary,
                                          std::string_view nlDashBoundary, int64 total, const error& readErr) {
    if (total == 0) {
        // At beginning of body, allow dashBoundary.
        if (hasPrefix(buf, dashBoundary)) {
            switch (matchAfterPrefix(buf, dashBoundary, readErr)) {
            case -1: return {isize(dashBoundary.size()), nullptr};
            case 0: return {0, nullptr};
            case +1: return {0, io::EOF_};
            }
        }
        if (hasPrefix(dashBoundary, buf)) return {0, readErr};
    }

    // Search for "\n--boundary".
    if (auto i = asStringView(buf).find(nlDashBoundary); i != std::string_view::npos) {
        switch (matchAfterPrefix(buf.subspan(i), nlDashBoundary, readErr)) {
        case -1: return {isize(i + nlDashBoundary.size()), nullptr};
        case 0: return {isize(i), nullptr};
        case +1: return {isize(i), io::EOF_};
        }
    }
    if (hasPrefix(nlDashBoundary, buf)) return {0, readErr};

    // Otherwise, anything up to the final \n is not part of the boundary
    // and so must be part of the body. Also if the section from the final
    // \n onward is not a prefix of the boundary, it too must be part of the
    // body.
    if (auto i = asStringView(buf).rfind(nlDashBoundary[0]); i != std::string_view::npos) {
        if (hasPrefix(nlDashBoundary, buf.subspan(i))) return {isize(i), nullptr};
    }
    return {isize(buf.size()), readErr};
}

}  // namespace

// ---------------------------------------------------------------------------
// Part

// partReader implements io.Reader by reading raw bytes directly from the
// wrapped *Part, without doing any Transfer-Encoding decoding.
class partReader final : public io::Reader {
public:
    partReader(ptr<multipart::Reader> mr, uint64 gen) : mr_(std::move(mr)), gen_(gen) {}
    std::pair<isize, error> Read(std::span<byte> d) override { return mr_->readPart(gen_, d); }

private:
    ptr<multipart::Reader> mr_;
    uint64 gen_;
};

Part::Part(ptr<multipart::Reader> mr, uint64 gen, textproto::MIMEHeader header, bool rawPart) : Header(std::move(header)) {
    r_ = make_ptr<partReader>(std::move(mr), gen);
    if (!rawPart) {
        constexpr std::string_view cte = "Content-Transfer-Encoding";
        if (strings::EqualFold(Header.Get(cte), "quoted-printable")) {
            Header.Del(cte);
            r_ = quotedprintable::NewReader(r_);
        }
    }
}

Part::~Part() = default;

std::pair<isize, error> Part::Read(std::span<byte> p) { return r_->Read(p); }

error Part::Close() {
    io::Copy(io::Discard(), *this);
    return nullptr;
}

void Part::parseContentDisposition() {
    parsed_ = true;
    auto v = Header.Get("Content-Disposition");
    auto [d, params, err] = mime::ParseMediaType(v);
    disposition_ = d;
    if (err) {
        dispositionParams_.clear();
    } else {
        dispositionParams_ = std::move(params);
    }
}

string Part::FormName() {
    // See https://tools.ietf.org/html/rfc2183 section 2 for EBNF of
    // Content-Disposition.
    if (!parsed_) parseContentDisposition();
    if (disposition_ != "form-data") return {};
    auto it = dispositionParams_.find(string::fromStatic("name"));
    return it == dispositionParams_.end() ? string() : it->second;
}

string Part::FileName() {
    if (!parsed_) parseContentDisposition();
    auto it = dispositionParams_.find(string::fromStatic("filename"));
    if (it == dispositionParams_.end() || it->second.empty()) return {};
    // RFC 7578, Section 4.2 requires that if a filename is provided, the
    // directory path information must not be used.
    return filepath::Base(it->second);
}

// ---------------------------------------------------------------------------
// Reader

ptr<Reader> NewReader(ptr<io::Reader> r, std::string_view boundary) { return make_ptr<Reader>(std::move(r), boundary); }

Reader::Reader(ptr<io::Reader> r, std::string_view boundary)
    : bufReader_(make_ptr<bufio::Reader>(make_ptr<stickyErrorReader>(std::move(r)), peekBufferSize)) {
    boundary_ = "\r\n--" + std::string(boundary) + "--";
    std::string_view b = boundary_;
    nl_ = b.substr(0, 2);
    nlDashBoundary_ = b.substr(0, b.size() - 2);
    dashBoundaryDash_ = b.substr(2);
    dashBoundary_ = b.substr(2, b.size() - 4);
}

Reader::~Reader() = default;

std::pair<isize, error> Reader::readPart(uint64 gen, std::span<byte> d) {
    if (gen != gen_ || !inPart_) return {0, io::EOF_};  // a part before the current one
    auto& br = *bufReader_;

    // Read into buffer until we identify some data to return, or we find a
    // reason to stop (boundary or read error).
    while (n_ == 0 && !err_) {
        auto [peek, _] = br.Peek(br.Buffered());
        std::tie(n_, err_) = scanUntilBoundary(peek, dashBoundary_, nlDashBoundary_, total_, readErr_);
        if (n_ == 0 && !err_) {
            // Force buffered I/O to read more into buffer.
            readErr_ = br.Peek(peek.size() + 1).second;
            if (readErr_ == io::EOF_) readErr_ = io::ErrUnexpectedEOF;
        }
    }

    // Read out from "data to return" part of buffer.
    if (n_ == 0) return {0, err_};
    std::size_t n = std::min(d.size(), std::size_t(n_));
    auto [nr, _] = br.Read(d.first(n));
    total_ += nr;
    n_ -= nr;
    if (n_ == 0) return {nr, err_};
    return {nr, nullptr};
}

// Discards the rest of the current part, as Go's NextPart does by closing
// it.
void Reader::closePart() {
    if (!inPart_) return;
    byte buf[4096];
    for (;;) {
        auto [n, err] = readPart(gen_, buf);
        if (err) break;
    }
    inPart_ = false;
}

std::pair<ptr<Part>, error> Reader::NextPart() { return nextPart(false, maxMIMEHeaderSize, maxMIMEHeaders); }

std::pair<ptr<Part>, error> Reader::NextRawPart() { return nextPart(true, maxMIMEHeaderSize, maxMIMEHeaders); }

std::pair<ptr<Part>, error> Reader::nextPart(bool rawPart, int64 maxHeaderSize, int64 maxHeaders) {
    closePart();
    if (dashBoundary_ == "--") return {nullptr, errors::New("multipart: boundary is empty")};
    bool expectNewPart = false;
    for (;;) {
        auto [line, err] = bufReader_->ReadSlice('\n');

        if (err == io::EOF_ && isFinalBoundary(line)) {
            // If the buffer ends in "--boundary--" without the trailing
            // "\r\n", ReadSlice will return an error (since it's missing the
            // '\n'), but this is a valid multipart EOF so we need to return
            // io.EOF instead of a fmt-wrapped one.
            return {nullptr, io::EOF_};
        }
        if (err) return {nullptr, fmt::Errorf("multipart: NextPart: {}", fmt::W(err))};

        if (isBoundaryDelimiterLine(line)) {
            partsRead_++;
            auto tr = textproto::NewReader(bufReader_);
            auto [header, herr] = tr->readMIMEHeader(maxHeaderSize, maxHeaders);
            if (herr) {
                if (herr.Error() == "message too large") herr = ErrMessageTooLarge;
                return {nullptr, herr};
            }
            gen_++;
            inPart_ = true;
            n_ = 0;
            total_ = 0;
            err_ = nullptr;
            readErr_ = nullptr;
            return {make_ptr<Part>(ptr<Reader>(this), gen_, std::move(header), rawPart), nullptr};
        }

        if (isFinalBoundary(line)) {
            // Expected EOF.
            return {nullptr, io::EOF_};
        }

        if (expectNewPart) {
            return {nullptr, fmt::Errorf("multipart: expecting a new Part; got line {}", strconv::Quote(asStringView(line)))};
        }

        if (partsRead_ == 0) {
            // Skip line.
            continue;
        }

        // Consume the "\n" or "\r\n" separator between the body of the
        // previous part and the boundary line we now expect will follow.
        // (either a new part or the end boundary)
        if (asStringView(line) == nl_) {
            expectNewPart = true;
            continue;
        }

        return {nullptr, fmt::Errorf("multipart: unexpected line in Next(): {}", strconv::Quote(asStringView(line)))};
    }
}

// isFinalBoundary reports whether line is the final boundary line
// indicating that all parts are over. It matches `^--boundary--[ \t]*(\r\n)?$`.
bool Reader::isFinalBoundary(std::span<const byte> line) {
    if (!hasPrefix(line, dashBoundaryDash_)) return false;
    auto rest = skipLWSPChar(line.subspan(dashBoundaryDash_.size()));
    return rest.empty() || asStringView(rest) == nl_;
}

bool Reader::isBoundaryDelimiterLine(std::span<const byte> line) {
    // https://tools.ietf.org/html/rfc2046#section-5.1
    //   The boundary delimiter line is then defined as a line consisting
    //   entirely of two hyphen characters ("-", decimal value 45) followed
    //   by the boundary parameter value from the Content-Type header field,
    //   optional linear whitespace, and a terminating CRLF.
    if (!hasPrefix(line, dashBoundary_)) return false;
    auto rest = skipLWSPChar(line.subspan(dashBoundary_.size()));

    // On the first part, see our lines are ending in \n instead of \r\n and
    // switch into that mode if so. This is a violation of the spec, but
    // occurs in practice.
    if (partsRead_ == 0 && rest.size() == 1 && rest[0] == '\n') {
        nl_ = nl_.substr(1);
        nlDashBoundary_ = nlDashBoundary_.substr(1);
    }
    return asStringView(rest) == nl_;
}

// ---------------------------------------------------------------------------
// ReadForm

namespace {

int64 mimeHeaderSize(const textproto::MIMEHeader& h) {
    int64 size = 400;
    for (const auto& [k, vs] : h) {
        size += int64(k.size());
        size += 200;  // map entry overhead
        for (const auto& v : vs) size += int64(v.size());
    }
    return size;
}

}  // namespace

std::pair<ptr<Form>, error> Reader::ReadForm(int64 maxMemory) {
    auto form = make_ptr<Form>();
    ptr<os::File> file;
    int64 fileOff = 0;
    int partsLeft = maxParts;
    int64 maxHeaders = maxMIMEHeaders;

    auto fail = [&](error err) -> std::pair<ptr<Form>, error> {
        if (file) file->Close();
        form->RemoveAll();
        if (file) os::Remove(file->Name());
        return {nullptr, err};
    };

    if (maxMemory < 0) maxMemory = 0;
    // Reserve an additional 10 MB for non-file parts.
    int64 maxFileMemoryBytes = maxMemory;
    if (maxFileMemoryBytes == std::numeric_limits<int64>::max()) maxFileMemoryBytes--;
    int64 maxMemoryBytes = maxMemory + int64(10 << 20);
    if (maxMemoryBytes <= 0) maxMemoryBytes = std::numeric_limits<int64>::max();  // overflow
    for (;;) {
        auto [p, err] = nextPart(false, maxMemoryBytes, maxHeaders);
        if (err == io::EOF_) break;
        if (err) return fail(err);
        if (partsLeft <= 0) return fail(ErrMessageTooLarge);
        partsLeft--;

        auto name = p->FormName();
        if (name.empty()) continue;
        auto filename = p->FileName();

        // Multiple values for the same key (one map entry, longer slice) are
        // cheaper than the same number of values for different keys (many
        // map entries), but using a consistent per-value cost for overhead
        // is simpler.
        constexpr int64 mapEntryOverhead = 200;
        maxMemoryBytes -= int64(name.size());
        maxMemoryBytes -= mapEntryOverhead;
        if (maxMemoryBytes < 0) {
            // We can't actually take this path, since nextPart would already
            // have rejected the MIME headers for being too large. Check
            // anyway.
            return fail(ErrMessageTooLarge);
        }

        bytes::Buffer b;

        if (filename.empty()) {
            // value, store as string in memory
            auto [n, cerr] = io::CopyN(b, *p, maxMemoryBytes + 1);
            if (cerr && cerr != io::EOF_) return fail(cerr);
            maxMemoryBytes -= n;
            if (maxMemoryBytes < 0) return fail(ErrMessageTooLarge);
            form->Value[name].push_back(b.String());
            continue;
        }

        // file, store in memory or on disk
        constexpr int64 fileHeaderSize = 100;
        maxMemoryBytes -= mimeHeaderSize(p->Header);
        maxMemoryBytes -= mapEntryOverhead;
        maxMemoryBytes -= fileHeaderSize;
        if (maxMemoryBytes < 0) return fail(ErrMessageTooLarge);
        for (const auto& [_, v] : p->Header) maxHeaders -= int64(v.size());
        auto fh = make_ptr<FileHeader>();
        fh->Filename = filename;
        fh->Header = p->Header;
        auto [n, cerr] = io::CopyN(b, *p, maxFileMemoryBytes + 1);
        if (cerr && cerr != io::EOF_) return fail(cerr);
        if (n > maxFileMemoryBytes) {
            // The files of a form go into one temporary file, one after the
            // other, as Go does by default.
            if (!file) {
                auto [f, ferr] = os::CreateTemp(tempDir, "multipart-");
                if (ferr) return fail(ferr);
                file = f;
            }
            if (auto [_, werr] = file->Write(b.Bytes()); werr) return fail(werr);
            // Hide the file's ReaderFrom, so that io::Copy goes through a
            // buffer: the part isn't a regular file.
            struct writerOnly final : io::Writer {
                io::Writer& w;
                explicit writerOnly(io::Writer& w_) : w(w_) {}
                std::pair<isize, error> Write(std::span<const byte> q) override { return w.Write(q); }
            } wo(*file);
            auto [remainingSize, rerr] = io::Copy(wo, *p);
            if (rerr) return fail(rerr);
            fh->tmpfile_ = file->Name();
            fh->Size = int64(b.Len()) + remainingSize;
            fh->tmpoff_ = fileOff;
            fileOff += fh->Size;
        } else {
            fh->content_ = bytes::Clone(b.Bytes());
            if (fh->content_.isNil()) fh->content_ = slice<byte>::make(0);
            fh->Size = int64(fh->content_.len());
            maxFileMemoryBytes -= n;
            maxMemoryBytes -= n;
        }
        form->File[name].push_back(std::move(fh));
    }
    if (file) {
        if (auto cerr = file->Close()) return fail(cerr);
    }
    return {std::move(form), nullptr};
}

// ---------------------------------------------------------------------------
// Form, FileHeader

error Form::RemoveAll() {
    error err;
    for (const auto& [_, fhs] : File) {
        for (const auto& fh : fhs) {
            if (!fh->tmpfile_.empty()) {
                auto e = os::Remove(fh->tmpfile_);
                if (e && !errors::Is(e, os::ErrNotExist) && !err) err = e;
            }
        }
    }
    return err;
}

namespace {

// A File over a section of a reader, and its closer, if any.
class sectionReadCloser final : public File {
public:
    sectionReadCloser(ptr<io::SectionReader> r, ptr<io::Closer> c) : r_(std::move(r)), c_(std::move(c)) {}
    std::pair<isize, error> Read(std::span<byte> p) override { return r_->Read(p); }
    std::pair<isize, error> ReadAt(std::span<byte> p, int64 off) override { return r_->ReadAt(p, off); }
    std::pair<int64, error> Seek(int64 offset, int whence) override { return r_->Seek(offset, whence); }
    error Close() override { return c_ ? c_->Close() : nullptr; }

private:
    ptr<io::SectionReader> r_;
    ptr<io::Closer> c_;
};

}  // namespace

std::pair<ptr<File>, error> FileHeader::Open() {
    if (tmpfile_.empty()) {
        auto r = io::NewSectionReader(bytes::NewReader(content_), 0, int64(content_.len()));
        return {make_ptr<sectionReadCloser>(std::move(r), nullptr), nullptr};
    }
    auto [f, err] = os::Open(tmpfile_);
    if (err) return {nullptr, err};
    auto r = io::NewSectionReader(f, tmpoff_, Size);
    return {make_ptr<sectionReadCloser>(std::move(r), f), nullptr};
}

// ---------------------------------------------------------------------------
// Writer

struct Writer::part final : io::Writer {
    multipart::Writer* mw;
    bool closed = false;
    error we;  // last error that occurred writing

    explicit part(multipart::Writer* w) : mw(w) {}

    error close() {
        closed = true;
        return we;
    }

    std::pair<isize, error> Write(std::span<const byte> d) override {
        if (closed || !mw) return {0, errors::New("multipart: can't write to finished part")};
        auto [n, err] = mw->w_->Write(d);
        if (err) we = err;
        return {n, err};
    }
};

namespace {

string randomBoundary() {
    std::array<byte, 30> buf;
    rand::Read(buf);
    return hex::EncodeToString(buf);
}

std::string escapeQuotes(std::string_view s) {
    std::string r;
    for (char c : s) {
        switch (c) {
        case '\\': r += "\\\\"; break;
        case '"': r += "\\\""; break;
        case '\r': r += "%0D"; break;
        case '\n': r += "%0A"; break;
        default: r += c;
        }
    }
    return r;
}

}  // namespace

ptr<Writer> NewWriter(ptr<io::Writer> w) { return make_ptr<Writer>(std::move(w)); }

Writer::Writer(ptr<io::Writer> w) : w_(std::move(w)), boundary_(randomBoundary()) {}

Writer::~Writer() {
    if (lastpart_) lastpart_->mw = nullptr;
}

error Writer::SetBoundary(std::string_view boundary) {
    if (lastpart_) return errors::New("mime: SetBoundary called after write");
    // rfc2046#section-5.1.1
    if (boundary.empty() || boundary.size() > 70) return errors::New("mime: invalid boundary length");
    std::size_t end = boundary.size() - 1;
    for (std::size_t i = 0; i < boundary.size(); i++) {
        char b = boundary[i];
        if (('A' <= b && b <= 'Z') || ('a' <= b && b <= 'z') || ('0' <= b && b <= '9')) continue;
        switch (b) {
        case '\'': case '(': case ')': case '+': case '_': case ',': case '-': case '.': case '/': case ':': case '=':
        case '?':
            continue;
        case ' ':
            if (i != end) continue;
            break;
        }
        return errors::New("mime: invalid boundary character");
    }
    boundary_ = string(boundary);
    return nullptr;
}

string Writer::FormDataContentType() const {
    std::string b(boundary_.view());
    // We must quote the boundary if it contains any of the tspecials
    // characters defined by RFC 2045, or space.
    if (b.find_first_of("()<>@,;:\\\"/[]?= ") != std::string::npos) b = '"' + b + '"';
    return string("multipart/form-data; boundary=" + b);
}

std::pair<ptr<io::Writer>, error> Writer::CreatePart(const textproto::MIMEHeader& header) {
    if (lastpart_) {
        if (auto err = lastpart_->close()) return {nullptr, err};
    }
    std::string b;
    if (lastpart_) {
        b += std::format("\r\n--{}\r\n", boundary_);
    } else {
        b += std::format("--{}\r\n", boundary_);
    }
    for (const auto& [k, vs] : header) {  // the keys are sorted
        for (const auto& v : vs) b += std::format("{}: {}\r\n", k, v);
    }
    b += "\r\n";
    if (auto [_, err] = io::WriteString(*w_, b); err) return {nullptr, err};
    if (lastpart_) lastpart_->mw = nullptr;
    lastpart_ = make_ptr<part>(this);
    return {lastpart_, nullptr};
}

std::pair<ptr<io::Writer>, error> Writer::CreateFormFile(std::string_view fieldname, std::string_view filename) {
    textproto::MIMEHeader h;
    h.Set("Content-Disposition", FileContentDisposition(fieldname, filename));
    h.Set("Content-Type", "application/octet-stream");
    return CreatePart(h);
}

std::pair<ptr<io::Writer>, error> Writer::CreateFormField(std::string_view fieldname) {
    textproto::MIMEHeader h;
    h.Set("Content-Disposition", string(std::format("form-data; name=\"{}\"", escapeQuotes(fieldname))));
    return CreatePart(h);
}

string FileContentDisposition(std::string_view fieldname, std::string_view filename) {
    return string(std::format("form-data; name=\"{}\"; filename=\"{}\"", escapeQuotes(fieldname), escapeQuotes(filename)));
}

error Writer::WriteField(std::string_view fieldname, std::string_view value) {
    auto [p, err] = CreateFormField(fieldname);
    if (err) return err;
    return io::WriteString(*p, value).second;
}

error Writer::Close() {
    if (lastpart_) {
        if (auto err = lastpart_->close()) return err;
        lastpart_->mw = nullptr;
        lastpart_ = nullptr;
    }
    return io::WriteString(*w_, std::format("\r\n--{}--\r\n", boundary_)).second;
}

}  // namespace gopp::multipart
