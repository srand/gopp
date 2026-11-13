// The io package.
//
// Read and Write take spans: the caller owns the buffer, and
// implementations must not keep it after returning, as in Go.
#pragma once

// The module interfaces (modules/) import these instead.
#ifndef GOPP_MODULE
#include <gopp/errors.hpp>
#include <gopp/ptr.hpp>
#include <gopp/slice.hpp>

#include <span>
#include <string_view>
#include <utility>
#endif

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

// io.SeekStart, io.SeekCurrent, io.SeekEnd: the whence of Seek.
inline constexpr int SeekStart = 0;
inline constexpr int SeekCurrent = 1;
inline constexpr int SeekEnd = 2;

struct Seeker : public virtual object {
    // Sets the offset of the next Read or Write to offset, relative to
    // whence, and returns the new offset.
    virtual std::pair<int64, error> Seek(int64 offset, int whence) = 0;
};

struct ReaderAt : public virtual object {
    // Reads len(p) bytes at offset off, or fewer and an error: io::EOF_ at
    // the end of the input.
    virtual std::pair<isize, error> ReadAt(std::span<byte> p, int64 off) = 0;
};

struct WriterAt : public virtual object {
    // Writes p at offset off, or returns the count written and an error.
    virtual std::pair<isize, error> WriteAt(std::span<const byte> p, int64 off) = 0;
};

// io.ReaderFrom: reads from r until EOF or an error; io::Copy uses it
// (net::TCPConn does, with splice(2) and sendfile(2)).
struct ReaderFrom : public virtual object {
    virtual std::pair<int64, error> ReadFrom(Reader& r) = 0;
};

// io.WriterTo: writes to w until there's nothing left or an error; io::Copy
// uses it.
struct WriterTo : public virtual object {
    virtual std::pair<int64, error> WriteTo(Writer& w) = 0;
};

struct ReadWriter : public Reader, public Writer {};
struct ReadSeeker : public Reader, public Seeker {};
struct ReadWriteSeeker : public Reader, public Writer, public Seeker {};
struct ReadCloser : public Reader, public Closer {};
struct WriteCloser : public Writer, public Closer {};
struct ReadWriteCloser : public Reader, public Writer, public Closer {};

// io.WriteString
std::pair<isize, error> WriteString(Writer& w, std::string_view s);

// io.ReadFull
std::pair<isize, error> ReadFull(Reader& r, std::span<byte> buf);

// io.ReadAtLeast
std::pair<isize, error> ReadAtLeast(Reader& r, std::span<byte> buf, isize min);

// io.Copy: with src.WriteTo if src is a WriterTo, else dst.ReadFrom if
// dst is a ReaderFrom, else through a buffer.
std::pair<int64, error> Copy(Writer& dst, Reader& src);

// io.CopyN: copies exactly n bytes, or fewer and an error (io::EOF_ if
// src ended early).
std::pair<int64, error> CopyN(Writer& dst, Reader& src, int64 n);

// io.ReadAll
std::pair<bytes, error> ReadAll(Reader& r);

// io.Discard
Writer& Discard();

// io.LimitedReader: reads at most N bytes from R, counting N down.
class LimitedReader : public Reader {
public:
    LimitedReader(ptr<Reader> r, int64 n) : R(std::move(r)), N(n) {}
    std::pair<isize, error> Read(std::span<byte> p) override;
    int64 Remaining() const noexcept { return N; }

    ptr<Reader> R;  // underlying reader
    int64 N;        // max bytes remaining
};

ptr<Reader> LimitReader(ptr<Reader> r, int64 n);

// io.NopCloser
ptr<ReadCloser> NopCloser(ptr<Reader> r);

// A Reader over a string (strings.NewReader, Go's strings.Reader).
class StringReader : public Reader, public Seeker {
public:
    explicit StringReader(string s) : s_(std::move(s)) {}
    std::pair<isize, error> Read(std::span<byte> p) override;
    // Seek: io::Seeker. The offset may go past the end, where Read
    // returns io::EOF_.
    std::pair<int64, error> Seek(int64 offset, int whence) override;
    // The number of unread bytes (Len).
    std::size_t len() const noexcept { return i_ < s_.size() ? s_.size() - i_ : 0; }
    // The length of the whole string (Size).
    int64 Size() const noexcept { return static_cast<int64>(s_.size()); }

private:
    string s_;
    std::size_t i_ = 0;
};

}  // namespace gopp::io
