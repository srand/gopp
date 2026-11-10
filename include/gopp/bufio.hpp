// The bufio package.
#pragma once

#include <gopp/io.hpp>

#include <memory>
#include <span>
#include <tuple>

namespace gopp::bufio {

extern const error ErrBufferFull;
extern const error ErrNegativeCount;
extern const error ErrInvalidUnreadByte;

constexpr std::size_t DefaultBufSize = 4096;

// bufio.Reader
class Reader : public io::Reader {
public:
    explicit Reader(ptr<io::Reader> rd, std::size_t size = DefaultBufSize);

    std::pair<isize, error> Read(std::span<byte> p) override;
    std::pair<byte, error> ReadByte();
    error UnreadByte();

    // The next n bytes without advancing; valid until the next read.
    std::pair<std::span<const byte>, error> Peek(std::size_t n);

    // Reads until delim, returning a view into the buffer valid until the
    // next read. ErrBufferFull if the buffer fills first.
    std::pair<std::span<const byte>, error> ReadSlice(byte delim);

    // Reads a line without its end of line; IsPrefix if it was too long
    // for the buffer.
    struct line_result {
        std::span<const byte> line;
        bool IsPrefix;
        error err;
    };
    line_result ReadLine();

    // Reads until delim and returns a copy, including delim.
    std::pair<string, error> ReadString(byte delim);

    std::pair<isize, error> Discard(isize n);
    std::size_t Buffered() const noexcept { return w_ - r_; }
    std::size_t size() const noexcept { return size_; }
    void Reset(ptr<io::Reader> rd);

private:
    void fill();
    error read_err();

    std::unique_ptr<byte[]> buf_;
    std::size_t size_;
    ptr<io::Reader> rd_;
    std::size_t r_ = 0;
    std::size_t w_ = 0;
    error err_;
    int last_byte_ = -1;
};

// bufio.Writer
class Writer : public io::Writer {
public:
    explicit Writer(ptr<io::Writer> wr, std::size_t size = DefaultBufSize);

    std::pair<isize, error> Write(std::span<const byte> p) override;
    std::pair<isize, error> WriteString(std::string_view s);
    error WriteByte(byte c);
    error Flush();

    std::size_t Available() const noexcept { return size_ - n_; }
    std::size_t Buffered() const noexcept { return n_; }
    std::size_t size() const noexcept { return size_; }
    void Reset(ptr<io::Writer> wr);

private:
    std::unique_ptr<byte[]> buf_;
    std::size_t size_;
    std::size_t n_ = 0;
    ptr<io::Writer> wr_;
    error err_;
};

}  // namespace gopp::bufio
