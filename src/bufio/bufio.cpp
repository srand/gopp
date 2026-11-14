// After Go's bufio/bufio.Go.
#include <gopp/bufio.hpp>

#include <algorithm>
#include <cstring>

namespace gopp::bufio {

const error ErrBufferFull = errors::New("bufio: buffer full");
const error ErrNegativeCount = errors::New("bufio: negative count");
const error ErrInvalidUnreadByte = errors::New("bufio: invalid use of UnreadByte");

namespace {
constexpr int max_consecutive_empty_reads = 100;
constexpr std::size_t min_read_buffer_size = 16;
}  // namespace

// ---------------------------------------------------------------------------
// Reader

Reader::Reader(ptr<io::Reader> rd, std::size_t size)
    : buf_(new byte[std::max(size, min_read_buffer_size)]),
      size_(std::max(size, min_read_buffer_size)),
      rd_(std::move(rd)) {}

void Reader::Reset(ptr<io::Reader> rd) {
    rd_ = std::move(rd);
    r_ = w_ = 0;
    err_ = nullptr;
    last_byte_ = -1;
}

error Reader::readErr() {
    error e = std::move(err_);
    err_ = nullptr;
    return e;
}

// Reads a new chunk into the buffer.
void Reader::fill() {
    if (r_ > 0) {
        std::memmove(buf_.get(), buf_.get() + r_, w_ - r_);
        w_ -= r_;
        r_ = 0;
    }
    if (w_ >= size_) panic("bufio: tried to fill full buffer");
    for (int i = max_consecutive_empty_reads; i > 0; i--) {
        auto [n, err] = rd_->Read(std::span<byte>(buf_.get() + w_, size_ - w_));
        if (n < 0) panic("bufio: reader returned negative count from Read");
        w_ += static_cast<std::size_t>(n);
        if (err) {
            err_ = err;
            return;
        }
        if (n > 0) return;
    }
    err_ = io::ErrNoProgress;
}

std::pair<isize, error> Reader::Read(std::span<byte> p) {
    if (p.empty()) {
        if (Buffered() > 0) return {0, nullptr};
        return {0, readErr()};
    }
    if (r_ == w_) {
        if (err_) return {0, readErr()};
        if (p.size() >= size_) {
            // Large read with an empty buffer: read directly into p.
            auto [n, err] = rd_->Read(p);
            if (n < 0) panic("bufio: reader returned negative count from Read");
            if (n > 0) last_byte_ = p[static_cast<std::size_t>(n) - 1];
            return {n, err};
        }
        r_ = w_ = 0;
        auto [n, err] = rd_->Read(std::span<byte>(buf_.get(), size_));
        if (n < 0) panic("bufio: reader returned negative count from Read");
        if (n == 0) return {0, err};
        w_ += static_cast<std::size_t>(n);
        err_ = err;
    }
    std::size_t n = std::min(p.size(), w_ - r_);
    std::memcpy(p.data(), buf_.get() + r_, n);
    r_ += n;
    last_byte_ = buf_[r_ - 1];
    return {static_cast<isize>(n), nullptr};
}

std::pair<byte, error> Reader::ReadByte() {
    while (r_ == w_) {
        if (err_) return {0, readErr()};
        fill();
    }
    byte c = buf_[r_++];
    last_byte_ = c;
    return {c, nullptr};
}

error Reader::UnreadByte() {
    if (last_byte_ < 0 || (r_ == 0 && w_ > 0)) return ErrInvalidUnreadByte;
    if (r_ > 0) {
        r_--;
    } else {
        w_ = 1;
    }
    buf_[r_] = static_cast<byte>(last_byte_);
    last_byte_ = -1;
    return nullptr;
}

std::pair<std::span<const byte>, error> Reader::Peek(std::size_t n) {
    last_byte_ = -1;
    while (w_ - r_ < n && w_ - r_ < size_ && !err_) fill();
    if (n > size_) return {std::span<const byte>(buf_.get() + r_, w_ - r_), ErrBufferFull};
    error err;
    std::size_t avail = w_ - r_;
    if (avail < n) {
        n = avail;
        err = readErr();
        if (!err) err = ErrBufferFull;
    }
    return {std::span<const byte>(buf_.get() + r_, n), err};
}

std::pair<std::span<const byte>, error> Reader::ReadSlice(byte delim) {
    std::size_t s = 0;  // search start
    std::span<const byte> line;
    error err;
    for (;;) {
        auto* start = buf_.get() + r_ + s;
        auto* end = buf_.get() + w_;
        if (auto* i = static_cast<byte*>(std::memchr(start, delim, static_cast<std::size_t>(end - start)))) {
            std::size_t len = static_cast<std::size_t>(i - (buf_.get() + r_)) + 1;
            line = std::span<const byte>(buf_.get() + r_, len);
            r_ += len;
            break;
        }
        if (err_) {
            line = std::span<const byte>(buf_.get() + r_, w_ - r_);
            r_ = w_;
            err = readErr();
            break;
        }
        if (Buffered() >= size_) {
            r_ = w_;
            line = std::span<const byte>(buf_.get(), size_);
            err = ErrBufferFull;
            break;
        }
        s = w_ - r_;
        fill();
    }
    if (!line.empty()) last_byte_ = line.back();
    return {line, err};
}

Reader::line_result Reader::ReadLine() {
    auto [line, err] = ReadSlice('\n');
    if (err == ErrBufferFull) {
        // Keep a trailing \r for the next call, if the \n follows it.
        if (!line.empty() && line.back() == '\r') {
            if (r_ == 0) panic("bufio: tried to rewind past start of buffer");
            r_--;
            line = line.first(line.size() - 1);
        }
        return {line, true, nullptr};
    }
    if (line.empty()) {
        if (err) line = {};
        return {line, false, err};
    }
    err = nullptr;
    if (line.back() == '\n') {
        std::size_t drop = 1;
        if (line.size() > 1 && line[line.size() - 2] == '\r') drop = 2;
        line = line.first(line.size() - drop);
    }
    return {line, false, err};
}

std::pair<string, error> Reader::ReadString(byte delim) {
    std::string acc;
    for (;;) {
        auto [frag, err] = ReadSlice(delim);
        acc.append(reinterpret_cast<const char*>(frag.data()), frag.size());
        if (err != ErrBufferFull) return {string(acc), err};
    }
}

std::pair<isize, error> Reader::Discard(isize n) {
    if (n < 0) return {0, ErrNegativeCount};
    if (n == 0) return {0, nullptr};
    last_byte_ = -1;
    isize remain = n;
    for (;;) {
        std::size_t skip = Buffered();
        if (skip == 0) {
            fill();
            skip = Buffered();
        }
        if (static_cast<isize>(skip) > remain) skip = static_cast<std::size_t>(remain);
        r_ += skip;
        remain -= static_cast<isize>(skip);
        if (remain == 0) return {n, nullptr};
        if (err_) return {n - remain, readErr()};
    }
}

// ---------------------------------------------------------------------------
// Writer

Writer::Writer(ptr<io::Writer> wr, std::size_t size)
    : buf_(new byte[size > 0 ? size : DefaultBufSize]), size_(size > 0 ? size : DefaultBufSize), wr_(std::move(wr)) {}

void Writer::Reset(ptr<io::Writer> wr) {
    wr_ = std::move(wr);
    n_ = 0;
    err_ = nullptr;
}

error Writer::Flush() {
    if (err_) return err_;
    if (n_ == 0) return nullptr;
    auto [n, err] = wr_->Write(std::span<const byte>(buf_.get(), n_));
    if (n < static_cast<isize>(n_) && !err) err = io::ErrShortWrite;
    if (err) {
        if (n > 0 && static_cast<std::size_t>(n) < n_) std::memmove(buf_.get(), buf_.get() + n, n_ - static_cast<std::size_t>(n));
        n_ -= static_cast<std::size_t>(std::max<isize>(n, 0));
        err_ = err;
        return err;
    }
    n_ = 0;
    return nullptr;
}

std::pair<isize, error> Writer::Write(std::span<const byte> p) {
    isize nn = 0;
    while (p.size() > Available() && !err_) {
        isize n;
        if (Buffered() == 0) {
            // Large write with an empty buffer: write directly.
            auto [m, err] = wr_->Write(p);
            n = m;
            err_ = err;
        } else {
            n = static_cast<isize>(Available());
            std::memcpy(buf_.get() + n_, p.data(), static_cast<std::size_t>(n));
            n_ += static_cast<std::size_t>(n);
            Flush();
        }
        nn += n;
        p = p.subspan(static_cast<std::size_t>(n));
    }
    if (err_) return {nn, err_};
    std::memcpy(buf_.get() + n_, p.data(), p.size());
    n_ += p.size();
    nn += static_cast<isize>(p.size());
    return {nn, nullptr};
}

std::pair<isize, error> Writer::WriteString(std::string_view s) { return Write(as_bytes(s)); }

error Writer::WriteByte(byte c) {
    if (err_) return err_;
    if (Available() <= 0 && Flush()) return err_;
    buf_[n_++] = c;
    return nullptr;
}

}  // namespace gopp::bufio
