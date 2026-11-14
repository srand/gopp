#include <gopp/io.hpp>
#include <gopp/unicode/utf8.hpp>

#include <algorithm>
#include <cstring>
#include <limits>

namespace gopp::io {

const error EOF_ = errors::New("EOF");
const error ErrUnexpectedEOF = errors::New("unexpected EOF");
const error ErrShortWrite = errors::New("short write");
const error ErrShortBuffer = errors::New("short buffer");
const error ErrNoProgress = errors::New("multiple Read calls return no data or error");
const error ErrClosedPipe = errors::New("io: read/write on closed pipe");

std::pair<isize, error> WriteString(Writer& w, std::string_view s) { return w.Write(as_bytes(s)); }

std::pair<isize, error> ReadAtLeast(Reader& r, std::span<byte> buf, isize min) {
    if (static_cast<isize>(buf.size()) < min) return {0, ErrShortBuffer};
    isize n = 0;
    error err;
    while (n < min && !err) {
        auto [nn, e] = r.Read(buf.subspan(static_cast<std::size_t>(n)));
        n += nn;
        err = e;
    }
    if (n >= min) err = nullptr;
    else if (n > 0 && err == EOF_) err = ErrUnexpectedEOF;
    return {n, err};
}

std::pair<isize, error> ReadFull(Reader& r, std::span<byte> buf) {
    return ReadAtLeast(r, buf, static_cast<isize>(buf.size()));
}

std::pair<int64, error> CopyBuffer(Writer& dst, Reader& src, std::span<byte> buf) {
    // If the reader has a WriteTo method, use it to do the copy; it avoids
    // an allocation and a copy. Likewise ReadFrom on the writer.
    if (auto* wt = dynamic_cast<WriterTo*>(&src)) return wt->WriteTo(dst);
    if (auto* rf = dynamic_cast<ReaderFrom*>(&dst)) return rf->ReadFrom(src);
    if (buf.empty()) panic("empty buffer in CopyBuffer");
    int64 written = 0;
    for (;;) {
        auto [nr, er] = src.Read(buf);
        if (nr > 0) {
            auto [nw, ew] = dst.Write(std::span<const byte>(buf.data(), static_cast<std::size_t>(nr)));
            if (nw < 0 || nr < nw) {
                nw = 0;
                if (!ew) ew = errors::New("invalid write result");
            }
            written += nw;
            if (ew) return {written, ew};
            if (nr != nw) return {written, ErrShortWrite};
        }
        if (er) {
            if (er == EOF_) return {written, nullptr};
            return {written, er};
        }
    }
}

std::pair<int64, error> Copy(Writer& dst, Reader& src) {
    byte buf[32 * 1024];
    return CopyBuffer(dst, src, buf);
}

namespace {

// Forwards to a reader that isn't reference counted (one on the stack),
// so that a LimitedReader can hold it.
struct readerRef final : Reader {
    Reader& r;
    explicit readerRef(Reader& r_) : r(r_) {}
    std::pair<isize, error> Read(std::span<byte> p) override { return r.Read(p); }
};

}  // namespace

std::pair<int64, error> CopyN(Writer& dst, Reader& src, int64 n) {
    // Copy over a LimitedReader, as Go does: a ReaderFrom sees the limit
    // and the reader under it, so that a part of an os::File goes to a
    // socket with sendfile(2). A reader that someone holds a reference to
    // is held here too; one that nobody does would be freed by it.
    LimitedReader lr(src.use_count() > 0 ? ptr<Reader>(&src) : make_ptr<readerRef>(src), n);
    auto [written, err] = Copy(dst, lr);
    if (written == n) return {n, nullptr};
    if (written < n && !err) err = EOF_;  // src ended early
    return {written, err};
}

std::pair<slice<byte>, error> ReadAll(Reader& r) {
    slice<byte> b = slice<byte>::make(0, 512);
    for (;;) {
        if (b.len() == b.cap()) b = append(b, byte(0)).Sub(0, b.len());
        auto [n, err] = r.Read(std::span<byte>(b.data() + b.len(), b.cap() - b.len()));
        b = b.Sub(0, b.len() + static_cast<std::size_t>(n));
        if (err) {
            if (err == EOF_) err = nullptr;
            return {b, err};
        }
    }
}

namespace {

struct discardWriter final : Writer {
    std::pair<isize, error> Write(std::span<const byte> p) override { return {static_cast<isize>(p.size()), nullptr}; }
};

struct nopCloser final : ReadCloser {
    explicit nopCloser(ptr<Reader> r) : r(std::move(r)) {}
    std::pair<isize, error> Read(std::span<byte> p) override { return r->Read(p); }
    error Close() override { return nullptr; }
    ptr<Reader> r;
};

}  // namespace

Writer& Discard() {
    static discardWriter* w = [] {
        auto* d = new discardWriter();
        d->addRef();  // never freed
        return d;
    }();
    return *w;
}

std::pair<isize, error> LimitedReader::Read(std::span<byte> p) {
    if (N <= 0) return {0, EOF_};
    if (static_cast<int64>(p.size()) > N) p = p.first(static_cast<std::size_t>(N));
    auto [n, err] = R->Read(p);
    N -= n;
    return {n, err};
}

ptr<Reader> LimitReader(ptr<Reader> r, int64 n) { return make_ptr<LimitedReader>(std::move(r), n); }

ptr<ReadCloser> NopCloser(ptr<Reader> r) { return make_ptr<nopCloser>(std::move(r)); }

std::pair<isize, error> StringReader::Read(std::span<byte> p) {
    if (i_ >= s_.size()) return {0, EOF_};
    std::size_t n = std::min(p.size(), s_.size() - i_);
    std::memcpy(p.data(), s_.data() + i_, n);
    i_ += n;
    return {static_cast<isize>(n), nullptr};
}

std::pair<int64, error> StringReader::Seek(int64 offset, int whence) {
    int64 abs;
    switch (whence) {
    case SeekStart: abs = offset; break;
    case SeekCurrent: abs = static_cast<int64>(i_) + offset; break;
    case SeekEnd: abs = static_cast<int64>(s_.size()) + offset; break;
    default: return {0, errors::New("strings.Reader.Seek: invalid whence")};
    }
    if (abs < 0) return {0, errors::New("strings.Reader.Seek: negative position")};
    i_ = static_cast<std::size_t>(abs);
    return {abs, nullptr};
}

std::tuple<rune, int, error> StringReader::ReadRune() {
    if (i_ >= s_.size()) {
        prevRune_ = -1;
        return {0, 0, EOF_};
    }
    prevRune_ = isize(i_);
    if (byte c = byte(s_[i_]); c < 0x80) {
        i_++;
        return {rune(c), 1, nullptr};
    }
    auto [r, size] = utf8::DecodeRuneInString(s_.view().substr(i_));
    i_ += std::size_t(size);
    return {r, size, nullptr};
}

error StringReader::UnreadRune() {
    if (i_ == 0) return errors::New("strings.Reader.UnreadRune: at beginning of string");
    if (prevRune_ < 0) return errors::New("strings.Reader.UnreadRune: previous operation was not ReadRune");
    i_ = std::size_t(prevRune_);
    prevRune_ = -1;
    return nullptr;
}

std::pair<isize, error> StringReader::ReadAt(std::span<byte> p, int64 off) {
    if (off < 0) return {0, errors::New("strings.Reader.ReadAt: negative offset")};
    if (off >= int64(s_.size())) return {0, EOF_};
    auto n = gopp::Copy(p, as_bytes(s_.view().substr(std::size_t(off))));
    if (n < p.size()) return {isize(n), EOF_};
    return {isize(n), nullptr};
}

// ---------------------------------------------------------------------------
// SectionReader, OffsetWriter

namespace {
const error errWhence = errors::New("Seek: invalid whence");
const error errOffset = errors::New("Seek: invalid offset");
}  // namespace

SectionReader::SectionReader(ptr<ReaderAt> r, int64 off, int64 n) : r_(std::move(r)), base_(off), off_(off), n_(n) {
    constexpr int64 max = std::numeric_limits<int64>::max();
    int64 remaining = 0;
    if (off <= max - n) {
        remaining = n + off;
    } else {
        // Overflow, with no way to return an error: assume we can read up
        // to an offset of max.
        remaining = max;
    }
    limit_ = remaining;
}

ptr<SectionReader> NewSectionReader(ptr<ReaderAt> r, int64 off, int64 n) {
    return make_ptr<SectionReader>(std::move(r), off, n);
}

std::pair<isize, error> SectionReader::Read(std::span<byte> p) {
    if (off_ >= limit_) return {0, EOF_};
    if (int64 max = limit_ - off_; int64(p.size()) > max) p = p.first(std::size_t(max));
    auto [n, err] = r_->ReadAt(p, off_);
    off_ += n;
    return {n, err};
}

std::pair<int64, error> SectionReader::Seek(int64 offset, int whence) {
    switch (whence) {
    case SeekStart: offset += base_; break;
    case SeekCurrent: offset += off_; break;
    case SeekEnd: offset += limit_; break;
    default: return {0, errWhence};
    }
    if (offset < base_) return {0, errOffset};
    off_ = offset;
    return {offset - base_, nullptr};
}

std::pair<isize, error> SectionReader::ReadAt(std::span<byte> p, int64 off) {
    if (off < 0 || off >= Size()) return {0, EOF_};
    off += base_;
    if (int64 max = limit_ - off; int64(p.size()) > max) {
        p = p.first(std::size_t(max));
        auto [n, err] = r_->ReadAt(p, off);
        if (!err) err = EOF_;
        return {n, err};
    }
    return r_->ReadAt(p, off);
}

ptr<OffsetWriter> NewOffsetWriter(ptr<WriterAt> w, int64 off) { return make_ptr<OffsetWriter>(std::move(w), off); }

std::pair<isize, error> OffsetWriter::Write(std::span<const byte> p) {
    auto [n, err] = w_->WriteAt(p, off_);
    off_ += n;
    return {n, err};
}

std::pair<isize, error> OffsetWriter::WriteAt(std::span<const byte> p, int64 off) {
    if (off < 0) return {0, errOffset};
    return w_->WriteAt(p, off + base_);
}

std::pair<int64, error> OffsetWriter::Seek(int64 offset, int whence) {
    switch (whence) {
    case SeekStart: offset += base_; break;
    case SeekCurrent: offset += off_; break;
    default: return {0, errWhence};
    }
    if (offset < base_) return {0, errOffset};
    off_ = offset;
    return {offset - base_, nullptr};
}

}  // namespace gopp::io
