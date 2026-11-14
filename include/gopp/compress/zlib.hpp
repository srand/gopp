// The compress/zlib package: the zlib format of RFC 1950, ported from Go
// 1.27.3, over compress/flate.
//
//     auto zw = zlib::NewWriter(out);
//     io::WriteString(*zw, "hello, world\n");
//     zw->Close();
//
//     auto [zr, err] = zlib::NewReader(in);
//     io::Copy(*out, *zr);
#pragma once

// The module interfaces (modules/) import these instead.
#ifndef GOPP_MODULE
#include <gopp/compress/flate.hpp>
#include <gopp/errors.hpp>
#include <gopp/hash.hpp>
#include <gopp/io.hpp>
#include <gopp/ptr.hpp>
#include <gopp/slice.hpp>

#include <span>
#include <utility>
#endif

namespace gopp::zlib {

inline constexpr int NoCompression = flate::NoCompression;
inline constexpr int BestSpeed = flate::BestSpeed;
inline constexpr int BestCompression = flate::BestCompression;
inline constexpr int DefaultCompression = flate::DefaultCompression;
inline constexpr int HuffmanOnly = flate::HuffmanOnly;

// zlib.ErrChecksum: the checksum of the data is wrong.
extern const error ErrChecksum;
// zlib.ErrDictionary: the stream needs a different dictionary.
extern const error ErrDictionary;
// zlib.ErrHeader: the input isn't zlib, or its header is invalid.
extern const error ErrHeader;

// zlib.Resetter: the Readers of NewReader reset to read a new stream.
using Resetter = flate::Resetter;

// zlib.NewReader: a ReadCloser that decompresses the zlib stream read from
// r, after reading its header; it's also a Resetter. Close doesn't close r.
std::pair<ptr<io::ReadCloser>, error> NewReader(ptr<io::Reader> r);
// zlib.NewReaderDict: NewReader with a preset dictionary.
std::pair<ptr<io::ReadCloser>, error> NewReaderDict(ptr<io::Reader> r, std::span<const byte> dict);

// zlib.Writer: compresses what's written to it into a zlib stream.
class Writer final : public io::WriteCloser {
public:
    Writer(ptr<io::Writer> w, int level, slice<byte> dict);
    ~Writer() override;

    std::pair<isize, error> Write(std::span<const byte> p) override;
    // Writes what's pending (a flate sync flush).
    error Flush();
    // Ends the stream, with its checksum. It doesn't close the underlying
    // writer.
    error Close() override;
    // Starts a new stream to w, with the same level and dictionary.
    void Reset(ptr<io::Writer> w);

private:
    error writeHeader();

    ptr<io::Writer> w_;
    int level_;
    slice<byte> dict_;
    ptr<flate::Writer> compressor_;
    ptr<hash::Hash32> digest_;
    error err_;
    byte scratch_[4] = {};
    bool wroteHeader_ = false;
};

// zlib.NewWriter: a Writer to w with DefaultCompression.
ptr<Writer> NewWriter(ptr<io::Writer> w);
// zlib.NewWriterLevel: an error if level isn't valid.
std::pair<ptr<Writer>, error> NewWriterLevel(ptr<io::Writer> w, int level);
// zlib.NewWriterLevelDict: with a preset dictionary; nil for none.
std::pair<ptr<Writer>, error> NewWriterLevelDict(ptr<io::Writer> w, int level, slice<byte> dict);

}  // namespace gopp::zlib
