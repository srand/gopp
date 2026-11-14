// The compress/gzip package: the gzip format of RFC 1952, ported from Go
// 1.27.3, over compress/flate.
//
//     auto zw = gzip::NewWriter(out);
//     zw->Name = "notes.txt";
//     io::Copy(*zw, *in);
//     zw->Close();
//
//     auto [zr, err] = gzip::NewReader(in);
//     io::Copy(*out, *zr);
#pragma once

// The module interfaces (modules/) import these instead.
#ifndef GOPP_MODULE
#include <gopp/bufio.hpp>
#include <gopp/compress/flate.hpp>
#include <gopp/errors.hpp>
#include <gopp/io.hpp>
#include <gopp/ptr.hpp>
#include <gopp/slice.hpp>
#include <gopp/string.hpp>
#include <gopp/time.hpp>

#include <utility>
#endif

namespace gopp::gzip {

inline constexpr int NoCompression = flate::NoCompression;
inline constexpr int BestSpeed = flate::BestSpeed;
inline constexpr int BestCompression = flate::BestCompression;
inline constexpr int DefaultCompression = flate::DefaultCompression;
inline constexpr int HuffmanOnly = flate::HuffmanOnly;

// gzip.ErrChecksum: the checksum or size of the data is wrong.
extern const error ErrChecksum;
// gzip.ErrHeader: the input isn't gzip, or its header is invalid.
extern const error ErrHeader;

// gzip.Header: the header of a gzip member. Strings are Latin-1 in the
// file, and UTF-8 here.
struct Header {
    string Comment;
    slice<byte> Extra;     // "extra data"; nil for none
    time::Time ModTime;    // zero if unknown
    string Name;           // the name of the file
    byte OS = 255;         // the operating system type (255: unknown)
};

// gzip.Reader: decompresses gzip data. A gzip file may be several members
// concatenated: the Reader reads them all as one stream, unless
// Multistream(false), and its Header is that of the member being read.
//
// It reads through a bufio::Reader, ahead of the data. At the end, it
// seeks a source that is an io::Seeker (a bytes::Reader, a file) back to
// the end of the gzip data, so that what follows can be read from it, as
// Go's does with sources that are io.ByteReaders.
class Reader final : public io::ReadCloser, public Header {
public:
    Reader() = default;
    ~Reader() override;

    // Reads a new gzip stream from r, and its header.
    error Reset(ptr<io::Reader> r);
    // Whether to read concatenated members as one stream (the default),
    // or stop with io::EOF_ at the end of each, to read the next one after
    // Reset or Multistream(true).
    void Multistream(bool ok) noexcept { multistream_ = ok; }

    std::pair<isize, error> Read(std::span<byte> p) override;
    // Closes the decompressor; it doesn't close the underlying reader.
    error Close() override;

private:
    std::pair<string, error> readString();
    std::pair<Header, error> readHeader();
    void giveBack();

    ptr<bufio::Reader> r_;
    ptr<io::Seeker> seeker_;  // the source, if r_ reads ahead of a Seeker
    ptr<io::ReadCloser> decompressor_;
    uint32 digest_ = 0;  // CRC-32, IEEE polynomial (section 8)
    uint32 size_ = 0;    // uncompressed size (section 2.3.1)
    byte buf_[512];
    error err_;
    bool multistream_ = true;
};

// gzip.NewReader: a Reader of r, after reading its header (errors are
// ErrHeader, or the reader's).
std::pair<ptr<Reader>, error> NewReader(ptr<io::Reader> r);

// gzip.Writer: compresses what's written to it into a gzip member. Set the
// Header's fields before the first Write, Flush or Close.
class Writer final : public io::WriteCloser, public Header {
public:
    Writer(ptr<io::Writer> w, int level);
    ~Writer() override;

    std::pair<isize, error> Write(std::span<const byte> p) override;
    // Writes what's pending, so that a reader can decompress what was
    // written so far (a flate sync flush).
    error Flush();
    // Ends the member, with its checksum. It doesn't close the underlying
    // writer.
    error Close() override;
    // Starts over, writing to w, with the same level, and a fresh Header.
    void Reset(ptr<io::Writer> w);

private:
    error writeBytes(std::span<const byte> b);
    error writeString(const string& s);

    ptr<io::Writer> w_;
    int level_;
    bool wroteHeader_ = false;
    bool closed_ = false;
    byte buf_[10];
    ptr<flate::Writer> compressor_;
    uint32 digest_ = 0;  // CRC-32, IEEE polynomial (section 8)
    uint32 size_ = 0;    // uncompressed size (section 2.3.1)
    error err_;
};

// gzip.NewWriter: a Writer to w with DefaultCompression.
ptr<Writer> NewWriter(ptr<io::Writer> w);
// gzip.NewWriterLevel: an error if level isn't valid.
std::pair<ptr<Writer>, error> NewWriterLevel(ptr<io::Writer> w, int level);

}  // namespace gopp::gzip
