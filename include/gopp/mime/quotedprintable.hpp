// The mime/quotedprintable package: the quoted-printable encoding of RFC
// 2045, ported from Go 1.27.3.
//
//     auto w = quotedprintable::NewWriter(out);
//     io::WriteString(*w, "Café = coffee");    // "Caf=C3=A9 =3D coffee"
//     w->Close();
//
//     auto r = quotedprintable::NewReader(in);
#pragma once

// The module interfaces (modules/) import these instead.
#ifndef GOPP_MODULE
#include <gopp/bufio.hpp>
#include <gopp/errors.hpp>
#include <gopp/io.hpp>
#include <gopp/ptr.hpp>

#include <span>
#include <string>
#include <utility>
#endif

namespace gopp::quotedprintable {

// quotedprintable.Reader: decodes quoted-printable data. It accepts the
// soft line breaks and escapes of RFC 2045, and is lenient with invalid
// escapes as RFC 2045 recommends: "=" not followed by two hex digits reads
// as itself.
class Reader final : public io::Reader {
public:
    explicit Reader(ptr<io::Reader> r);

    std::pair<isize, error> Read(std::span<byte> p) override;

private:
    ptr<bufio::Reader> br_;
    error rerr_;        // last read error
    std::string line_;  // to be consumed before more of br_
    std::size_t pos_ = 0;
};

// quotedprintable.NewReader
ptr<Reader> NewReader(ptr<io::Reader> r);

// quotedprintable.Writer: encodes what's written to it, with lines of at
// most 76 characters. Close flushes the last line.
class Writer final : public io::WriteCloser {
public:
    explicit Writer(ptr<io::Writer> w) : w_(std::move(w)) {}

    // Whether to treat the input as binary data: line breaks are then
    // encoded, as =0D=0A, rather than written as CRLF.
    bool Binary = false;

    std::pair<isize, error> Write(std::span<const byte> p) override;
    // Writes the last line. It doesn't close the underlying writer.
    error Close() override;

private:
    error write(std::span<const byte> p);
    error encode(byte b);
    error checkLastByte();
    error insertSoftLineBreak();
    error insertCRLF();
    error flush();

    ptr<io::Writer> w_;
    int i_ = 0;
    byte line_[78];
    bool cr_ = false;
};

// quotedprintable.NewWriter
ptr<Writer> NewWriter(ptr<io::Writer> w);

}  // namespace gopp::quotedprintable
