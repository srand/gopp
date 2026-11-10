// The io package.
//
// Read and Write take spans: the caller owns the buffer, and
// implementations must not keep it after returning, as in Go.
#pragma once

#include <gopp/errors.hpp>
#include <gopp/ptr.hpp>
#include <gopp/slice.hpp>

#include <span>
#include <string_view>
#include <utility>

namespace gopp::io {

// io.EOF and friends.
extern const error EOF_;  // io.EOF (EOF is a C macro)
extern const error ErrUnexpectedEOF;
extern const error ErrShortWrite;
extern const error ErrShortBuffer;
extern const error ErrNoProgress;
extern const error ErrClosedPipe;

struct Reader : public virtual object {
    // Reads up to p.size() bytes into p. Returns the count and an error;
    // at the end of the input, the error is io::EOF_.
    virtual std::pair<isize, error> Read(std::span<byte> p) = 0;
};

struct Writer : public virtual object {
    // Writes all of p, or returns the count written and an error.
    virtual std::pair<isize, error> Write(std::span<const byte> p) = 0;
};

struct Closer : public virtual object {
    virtual error Close() = 0;
};

struct ReadWriter : public Reader, public Writer {};
struct ReadCloser : public Reader, public Closer {};
struct WriteCloser : public Writer, public Closer {};
struct ReadWriteCloser : public Reader, public Writer, public Closer {};

// io.WriteString
std::pair<isize, error> WriteString(Writer& w, std::string_view s);

// io.ReadFull
std::pair<isize, error> ReadFull(Reader& r, std::span<byte> buf);

// io.ReadAtLeast
std::pair<isize, error> ReadAtLeast(Reader& r, std::span<byte> buf, isize min);

// io.Copy
std::pair<int64, error> Copy(Writer& dst, Reader& src);

// io.CopyN: copies exactly n bytes, or fewer and an error (io::EOF_ if
// src ended early).
std::pair<int64, error> CopyN(Writer& dst, Reader& src, int64 n);

// io.ReadAll
std::pair<bytes, error> ReadAll(Reader& r);

// io.Discard
Writer& Discard();

// io.LimitReader: reads at most n bytes from r.
class LimitedReader : public Reader {
public:
    LimitedReader(ptr<Reader> r, int64 n) : r_(std::move(r)), n_(n) {}
    std::pair<isize, error> Read(std::span<byte> p) override;
    int64 Remaining() const noexcept { return n_; }

private:
    ptr<Reader> r_;
    int64 n_;
};

ptr<Reader> LimitReader(ptr<Reader> r, int64 n);

// io.NopCloser
ptr<ReadCloser> NopCloser(ptr<Reader> r);

// A Reader over a string (strings.NewReader).
class StringReader : public Reader {
public:
    explicit StringReader(string s) : s_(std::move(s)) {}
    std::pair<isize, error> Read(std::span<byte> p) override;
    std::size_t len() const noexcept { return s_.size() - i_; }

private:
    string s_;
    std::size_t i_ = 0;
};

}  // namespace gopp::io
