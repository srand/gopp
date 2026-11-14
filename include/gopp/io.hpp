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

#include <memory>
#include <span>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>
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
std::pair<slice<byte>, error> ReadAll(Reader& r);

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
class StringReader : public Reader, public Seeker, public ReaderAt {
public:
    explicit StringReader(string s) : s_(std::move(s)) {}
    std::pair<isize, error> Read(std::span<byte> p) override;
    // Seek: io::Seeker. The offset may go past the end, where Read
    // returns io::EOF_.
    std::pair<int64, error> Seek(int64 offset, int whence) override;
    std::pair<isize, error> ReadAt(std::span<byte> p, int64 off) override;
    // The number of unread bytes (Len).
    std::size_t len() const noexcept { return i_ < s_.size() ? s_.size() - i_ : 0; }
    // The length of the whole string (Size).
    int64 Size() const noexcept { return static_cast<int64>(s_.size()); }

private:
    string s_;
    std::size_t i_ = 0;
};

// io.SectionReader: reads n bytes of r from offset off. A ReaderAt, so
// that it can be read from concurrently.
class SectionReader : public Reader, public Seeker, public ReaderAt {
public:
    SectionReader(ptr<ReaderAt> r, int64 off, int64 n);

    std::pair<isize, error> Read(std::span<byte> p) override;
    std::pair<int64, error> Seek(int64 offset, int whence) override;
    std::pair<isize, error> ReadAt(std::span<byte> p, int64 off) override;
    // The size of the section in bytes.
    int64 Size() const noexcept { return limit_ - base_; }
    // The underlying ReaderAt and the section's offset and size.
    std::tuple<ptr<ReaderAt>, int64, int64> Outer() const { return {r_, base_, n_}; }

private:
    ptr<ReaderAt> r_;
    int64 base_;
    int64 off_;
    int64 limit_;
    int64 n_;
};

// io.NewSectionReader
ptr<SectionReader> NewSectionReader(ptr<ReaderAt> r, int64 off, int64 n);

// io.OffsetWriter: writes to w at offset base and on.
class OffsetWriter : public Writer, public WriterAt, public Seeker {
public:
    OffsetWriter(ptr<WriterAt> w, int64 off) : w_(std::move(w)), base_(off), off_(off) {}
    std::pair<isize, error> Write(std::span<const byte> p) override;
    std::pair<isize, error> WriteAt(std::span<const byte> p, int64 off) override;
    std::pair<int64, error> Seek(int64 offset, int whence) override;

private:
    ptr<WriterAt> w_;
    int64 base_;
    int64 off_;
};

// io.NewOffsetWriter
ptr<OffsetWriter> NewOffsetWriter(ptr<WriterAt> w, int64 off);

// io.MultiReader: the concatenation of readers, read in turn.
ptr<Reader> MultiReader(std::vector<ptr<Reader>> readers);
// io.MultiWriter: a Writer that writes to all of writers, as tee(1) does;
// a write stops at the first error.
ptr<Writer> MultiWriter(std::vector<ptr<Writer>> writers);
// io.TeeReader: a Reader that writes to w what it reads from r.
ptr<Reader> TeeReader(ptr<Reader> r, ptr<Writer> w);

// io.CopyBuffer: io::Copy through buf, if it needs a buffer.
std::pair<int64, error> CopyBuffer(Writer& dst, Reader& src, std::span<byte> buf);

namespace detail {
struct pipe;
}

// io.PipeReader: the read half of a pipe.
class PipeReader final : public ReadCloser {
public:
    explicit PipeReader(std::shared_ptr<detail::pipe> p) noexcept : p_(std::move(p)) {}
    ~PipeReader() override;
    // Reads what a Write writes, waiting for one; io::EOF_ or the
    // writer's CloseWithError error once the write half is closed.
    std::pair<isize, error> Read(std::span<byte> p) override;
    // Closes the reader: later writes fail with ErrClosedPipe.
    error Close() override;
    // Closes the reader: later writes fail with err (ErrClosedPipe if
    // nil). The first error passed wins.
    error CloseWithError(error err);

private:
    std::shared_ptr<detail::pipe> p_;
};

// io.PipeWriter: the write half of a pipe.
class PipeWriter final : public WriteCloser {
public:
    explicit PipeWriter(std::shared_ptr<detail::pipe> p) noexcept : p_(std::move(p)) {}
    ~PipeWriter() override;
    // Writes p, waiting until readers have read all of it or the read
    // half is closed.
    std::pair<isize, error> Write(std::span<const byte> p) override;
    // Closes the writer: reads then return io::EOF_.
    error Close() override;
    // Closes the writer: reads then return err (io::EOF_ if nil).
    error CloseWithError(error err);

private:
    std::shared_ptr<detail::pipe> p_;
};

// io.Pipe: a synchronous in-memory pipe. Each Write waits for Reads that
// consume all its data; there's no buffering. Reads and writes park their
// fibers, and may come from several fibers at once.
std::pair<ptr<PipeReader>, ptr<PipeWriter>> Pipe();

}  // namespace gopp::io
