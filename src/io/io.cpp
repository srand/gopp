#include <gopp/io.hpp>

#include <algorithm>
#include <cstring>

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

std::pair<int64, error> Copy(Writer& dst, Reader& src) {
    // If the reader has a WriteTo method, use it to do the copy; it avoids
    // an allocation and a copy. Likewise ReadFrom on the writer.
    if (auto* wt = dynamic_cast<WriterTo*>(&src)) return wt->WriteTo(dst);
    if (auto* rf = dynamic_cast<ReaderFrom*>(&dst)) return rf->ReadFrom(src);
    byte buf[32 * 1024];
    int64 written = 0;
    for (;;) {
        auto [nr, er] = src.Read(buf);
        if (nr > 0) {
            auto [nw, ew] = dst.Write(std::span<const byte>(buf, static_cast<std::size_t>(nr)));
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

namespace {

// Forwards to a reader that isn't reference counted (one on the stack),
// so that a LimitedReader can hold it.
struct reader_ref final : Reader {
    Reader& r;
    explicit reader_ref(Reader& r_) : r(r_) {}
    std::pair<isize, error> Read(std::span<byte> p) override { return r.Read(p); }
};

}  // namespace

std::pair<int64, error> CopyN(Writer& dst, Reader& src, int64 n) {
    // Copy over a LimitedReader, as Go does: a ReaderFrom sees the limit
    // and the reader under it, so that a part of an os::File goes to a
    // socket with sendfile(2). A reader that someone holds a reference to
    // is held here too; one that nobody does would be freed by it.
    LimitedReader lr(src.use_count() > 0 ? ptr<Reader>(&src) : make_ptr<reader_ref>(src), n);
    auto [written, err] = Copy(dst, lr);
    if (written == n) return {n, nullptr};
    if (written < n && !err) err = EOF_;  // src ended early
    return {written, err};
}

std::pair<bytes, error> ReadAll(Reader& r) {
    bytes b = bytes::make(0, 512);
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

struct discard_writer final : Writer {
    std::pair<isize, error> Write(std::span<const byte> p) override { return {static_cast<isize>(p.size()), nullptr}; }
};

struct nop_closer_t final : ReadCloser {
    explicit nop_closer_t(ptr<Reader> r) : r(std::move(r)) {}
    std::pair<isize, error> Read(std::span<byte> p) override { return r->Read(p); }
    error Close() override { return nullptr; }
    ptr<Reader> r;
};

}  // namespace

Writer& Discard() {
    static discard_writer* w = [] {
        auto* d = new discard_writer();
        d->add_ref();  // never freed
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

ptr<ReadCloser> NopCloser(ptr<Reader> r) { return make_ptr<nop_closer_t>(std::move(r)); }

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

}  // namespace gopp::io
