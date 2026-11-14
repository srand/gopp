// The compress/flate package: the DEFLATE format of RFC 1951, after Go
// 1.27.3's API, on zlib. The output of a Writer is valid DEFLATE, but not
// byte for byte what Go's compressor writes.
//
//     auto [w, err] = flate::NewWriter(out, flate::BestSpeed);
//     io::Copy(*w, *in);
//     w->Close();
//
//     auto r = flate::NewReader(in);
//     io::Copy(*out, *r);
//
// A Reader reads its input through a bufio::Reader, and reads no more of
// it than the stream: given a bufio::Reader, it uses that, and what follows
// the stream can be read from it afterwards (as gzip and zlib do). Any
// other reader is wrapped in one.
#pragma once

// The module interfaces (modules/) import these instead.
#ifndef GOPP_MODULE
#include <gopp/errors.hpp>
#include <gopp/io.hpp>
#include <gopp/ptr.hpp>
#include <gopp/string.hpp>

#include <memory>
#include <span>
#include <utility>
#endif

namespace gopp::flate {

// Compression levels: 1 (BestSpeed) to 9 (BestCompression), or these.
inline constexpr int NoCompression = 0;
inline constexpr int BestSpeed = 1;
inline constexpr int BestCompression = 9;
inline constexpr int DefaultCompression = -1;
// Huffman coding only, without matching: for data that is compressed by
// an LZ-style algorithm already.
inline constexpr int HuffmanOnly = -2;

// flate.CorruptInputError: the input is corrupt, before the given offset.
struct CorruptInputError : errorInterface {
    explicit CorruptInputError(int64 offset) noexcept : Offset(offset) {}
    string Error() const override;
    int64 Offset;
};

// flate.InternalError: an error in the compressor itself (zlib's).
struct InternalError : errorInterface {
    explicit InternalError(string msg) : msg_(std::move(msg)) {}
    string Error() const override { return "flate: internal error: " + msg_; }

private:
    string msg_;
};

// flate.Resetter: the Readers of NewReader reset to read a new stream, to
// reuse their buffers.
struct Resetter : public virtual object {
    // Reads a new stream from r, with the preset dictionary dict.
    virtual error Reset(ptr<io::Reader> r, std::span<const byte> dict) = 0;
};

// flate.NewReader: a ReadCloser that decompresses the DEFLATE stream read
// from r. It's also a Resetter. Reading ends with io::EOF_ at the end of
// the stream; a stream cut short is io::ErrUnexpectedEOF, a corrupt one a
// CorruptInputError. Close doesn't close r.
ptr<io::ReadCloser> NewReader(ptr<io::Reader> r);
// flate.NewReaderDict: NewReader with a preset dictionary, which must be
// the one the stream was compressed with.
ptr<io::ReadCloser> NewReaderDict(ptr<io::Reader> r, std::span<const byte> dict);

// flate.Writer: compresses what's written to it, and writes the result to
// the underlying writer.
class Writer final : public io::WriteCloser {
public:
    Writer(ptr<io::Writer> w, int level, std::span<const byte> dict);
    ~Writer() override;

    std::pair<isize, error> Write(std::span<const byte> p) override;
    // Writes what's pending, ending with an empty stored block (a sync
    // flush), so that a reader can decompress all that was written so far.
    error Flush();
    // Writes what's pending and ends the stream. It doesn't close the
    // underlying writer.
    error Close() override;
    // Starts a new stream, to w, with the level and dictionary as before.
    void Reset(ptr<io::Writer> w);

    struct impl;

private:
    std::unique_ptr<impl> impl_;
};

// flate.NewWriter: a Writer of the given level to w; an error if the level
// isn't one of the above.
std::pair<ptr<Writer>, error> NewWriter(ptr<io::Writer> w, int level);
// flate.NewWriterDict: NewWriter with a preset dictionary.
std::pair<ptr<Writer>, error> NewWriterDict(ptr<io::Writer> w, int level, std::span<const byte> dict);

}  // namespace gopp::flate
