// encoding/hex, ported from Go 1.27.3's encoding/hex/hex.go.
#include <gopp/encoding/hex.hpp>
#include <gopp/fmt.hpp>
#include <gopp/slices.hpp>
#include <gopp/strings.hpp>

#include "text/template/goconv.hpp"

#include <array>
#include <cstring>

namespace gopp::hex {

namespace {

constexpr std::string_view hextable = "0123456789abcdef";

constexpr std::array<byte, 256> reverse_hex_table = [] {
    std::array<byte, 256> t{};
    t.fill(0xff);
    for (int c = '0'; c <= '9'; c++) t[static_cast<std::size_t>(c)] = static_cast<byte>(c - '0');
    for (int c = 'a'; c <= 'f'; c++) t[static_cast<std::size_t>(c)] = static_cast<byte>(c - 'a' + 10);
    for (int c = 'A'; c <= 'F'; c++) t[static_cast<std::size_t>(c)] = static_cast<byte>(c - 'A' + 10);
    return t;
}();

constexpr std::size_t buffer_size = 1024;

std::span<const byte> viewBytes(const byte* p, std::size_t n) { return {p, n}; }

}  // namespace

const error ErrLength = errors::New("encoding/hex: odd length hex string");

string InvalidByteError::Error() const {
    // fmt's %#U: U+0067 'g', without the quoted character if it isn't printable.
    auto r = static_cast<rune>(Byte);
    std::string s = std::format("encoding/hex: invalid byte: U+{:04X}", r);
    if (text::tmpl::detail::isPrint(r)) {
        s += " '";
        text::tmpl::detail::appendRune(s, r);
        s += "'";
    }
    return string(s);
}

isize Encode(std::span<byte> dst, std::span<const byte> src) noexcept {
    std::size_t j = 0;
    for (byte v : src) {
        dst[j] = static_cast<byte>(hextable[v >> 4]);
        dst[j + 1] = static_cast<byte>(hextable[v & 0x0f]);
        j += 2;
    }
    return static_cast<isize>(src.size() * 2);
}

slice<byte> AppendEncode(slice<byte> dst, std::span<const byte> src) {
    auto n = static_cast<std::size_t>(EncodedLen(static_cast<isize>(src.size())));
    std::size_t len = dst.len();
    dst = slices::Grow(std::move(dst), static_cast<isize>(n)).Sub(0, len + n);
    Encode(std::span<byte>(dst.data() + len, n), src);
    return dst;
}

std::pair<isize, error> Decode(std::span<byte> dst, std::span<const byte> src) {
    std::size_t i = 0, j = 0;
    for (; j + 1 < src.size(); j += 2) {
        byte p = src[j];
        byte q = src[j + 1];
        byte a = reverse_hex_table[p];
        byte b = reverse_hex_table[q];
        if (a > 0x0f) return {static_cast<isize>(i), make_ptr<InvalidByteError>(p)};
        if (b > 0x0f) return {static_cast<isize>(i), make_ptr<InvalidByteError>(q)};
        dst[i] = static_cast<byte>((a << 4) | b);
        i++;
    }
    if (src.size() % 2 == 1) {
        // Check for invalid char before reporting bad length, since the
        // invalid char (if present) is an earlier problem.
        if (reverse_hex_table[src[j]] > 0x0f) return {static_cast<isize>(i), make_ptr<InvalidByteError>(src[j])};
        return {static_cast<isize>(i), ErrLength};
    }
    return {static_cast<isize>(i), nullptr};
}

std::pair<slice<byte>, error> AppendDecode(slice<byte> dst, std::span<const byte> src) {
    auto n = static_cast<std::size_t>(DecodedLen(static_cast<isize>(src.size())));
    std::size_t len = dst.len();
    dst = slices::Grow(std::move(dst), static_cast<isize>(n)).Sub(0, len + n);
    auto [m, err] = Decode(std::span<byte>(dst.data() + len, n), src);
    return {dst.Sub(0, len + static_cast<std::size_t>(m)), err};
}

string EncodeToString(std::span<const byte> src) {
    return string::build(static_cast<std::size_t>(EncodedLen(static_cast<isize>(src.size()))),
                         [&](char* p) { Encode(std::span<byte>(reinterpret_cast<byte*>(p), src.size() * 2), src); });
}

std::pair<slice<byte>, error> DecodeString(std::string_view s) {
    auto dst = slice<byte>::make(static_cast<std::size_t>(DecodedLen(static_cast<isize>(s.size()))));
    auto [n, err] = Decode(dst, as_bytes(s));
    return {dst.Sub(0, static_cast<std::size_t>(n)), err};
}

namespace {

class encoder final : public io::Writer {
public:
    explicit encoder(ptr<io::Writer> w) : w_(std::move(w)) {}

    std::pair<isize, error> Write(std::span<const byte> p) override {
        isize n = 0;
        while (!p.empty() && !err_) {
            std::size_t chunk = std::min(buffer_size / 2, p.size());
            isize encoded = hex::Encode(out_, p.first(chunk));
            auto [written, err] = w_->Write(viewBytes(out_.data(), static_cast<std::size_t>(encoded)));
            err_ = err;
            n += written / 2;
            p = p.subspan(chunk);
        }
        return {n, err_};
    }

private:
    ptr<io::Writer> w_;
    error err_;
    std::array<byte, buffer_size> out_;
};

class decoder final : public io::Reader {
public:
    explicit decoder(ptr<io::Reader> r) : r_(std::move(r)) {}

    std::pair<isize, error> Read(std::span<byte> p) override {
        // Fill internal buffer with sufficient bytes to decode.
        if (in_.size() < 2 && !err_) {
            std::size_t num_copy = in_.size();  // either 0 or 1 bytes
            std::memmove(arr_.data(), in_.data(), num_copy);
            auto [num_read, err] = r_->Read(std::span<byte>(arr_).subspan(num_copy));
            err_ = err;
            in_ = std::span<byte>(arr_.data(), num_copy + static_cast<std::size_t>(num_read));
            if (err_ == io::EOF_ && in_.size() % 2 != 0) {
                if (reverse_hex_table[in_.back()] > 0x0f) err_ = make_ptr<InvalidByteError>(in_.back());
                else err_ = io::ErrUnexpectedEOF;
            }
        }
        // Decode internal buffer into output buffer.
        if (std::size_t num_avail = in_.size() / 2; p.size() > num_avail) p = p.first(num_avail);
        auto [num_dec, err] = hex::Decode(p, in_.first(p.size() * 2));
        in_ = in_.subspan(2 * static_cast<std::size_t>(num_dec));
        if (err) {
            in_ = {};
            err_ = err;  // decode error; discard input remainder
        }
        if (in_.size() < 2) return {num_dec, err_};  // only expose errors when buffer fully consumed
        return {num_dec, nullptr};
    }

private:
    ptr<io::Reader> r_;
    error err_;
    std::span<byte> in_;  // input buffer (encoded form)
    std::array<byte, buffer_size> arr_;  // backing array for in_
};

constexpr byte toChar(byte b) { return b < 32 || b > 126 ? '.' : b; }

class dumper final : public io::WriteCloser {
public:
    explicit dumper(ptr<io::Writer> w) : w_(std::move(w)) {}

    std::pair<isize, error> Write(std::span<const byte> data) override {
        if (closed_) return {0, errors::New("encoding/hex: dumper closed")};
        // Output lines look like:
        // 00000010  2e 2f 30 31 32 33 34 35  36 37 38 39 3a 3b 3c 3d  |./0123456789:;<=|
        // ^ offset                          ^ extra space              ^ ASCII of line.
        isize n = 0;
        for (byte b : data) {
            if (used_ == 0) {
                // At the beginning of a line we print the current offset
                // in hex.
                buf_[0] = static_cast<byte>(n_ >> 24);
                buf_[1] = static_cast<byte>(n_ >> 16);
                buf_[2] = static_cast<byte>(n_ >> 8);
                buf_[3] = static_cast<byte>(n_);
                hex::Encode(std::span<byte>(buf_).subspan(4), std::span<const byte>(buf_.data(), 4));
                buf_[12] = ' ';
                buf_[13] = ' ';
                if (auto err = w_->Write(viewBytes(buf_.data() + 4, 10)).second) return {n, err};
            }
            hex::Encode(buf_, std::span<const byte>(&b, 1));
            buf_[2] = ' ';
            std::size_t l = 3;
            if (used_ == 7) {
                // There's an additional space after the 8th byte.
                buf_[3] = ' ';
                l = 4;
            } else if (used_ == 15) {
                // At the end of the line there's an extra space and the
                // bar for the right column.
                buf_[3] = ' ';
                buf_[4] = '|';
                l = 5;
            }
            if (auto err = w_->Write(viewBytes(buf_.data(), l)).second) return {n, err};
            n++;
            right_chars_[used_] = toChar(b);
            used_++;
            n_++;
            if (used_ == 16) {
                right_chars_[16] = '|';
                right_chars_[17] = '\n';
                if (auto err = w_->Write(right_chars_).second) return {n, err};
                used_ = 0;
            }
        }
        return {n, nullptr};
    }

    error Close() override {
        // See the comments in Write() for the details of this format.
        if (closed_) return nullptr;
        closed_ = true;
        if (used_ == 0) return nullptr;
        buf_[0] = ' ';
        buf_[1] = ' ';
        buf_[2] = ' ';
        buf_[3] = ' ';
        buf_[4] = '|';
        std::size_t n_bytes = used_;
        while (used_ < 16) {
            std::size_t l = 3;
            if (used_ == 7) l = 4;
            else if (used_ == 15) l = 5;
            if (auto err = w_->Write(viewBytes(buf_.data(), l)).second) return err;
            used_++;
        }
        right_chars_[n_bytes] = '|';
        right_chars_[n_bytes + 1] = '\n';
        return w_->Write(viewBytes(right_chars_.data(), n_bytes + 2)).second;
    }

private:
    ptr<io::Writer> w_;
    std::array<byte, 18> right_chars_{};
    std::array<byte, 14> buf_{};
    std::size_t used_ = 0;  // number of bytes in the current line
    uint64 n_ = 0;          // number of bytes, total
    bool closed_ = false;
};

}  // namespace

string Dump(std::span<const byte> data) {
    if (data.empty()) return {};
    auto buf = make_ptr<strings::Builder>();
    // Dumper will write 79 bytes per complete 16 byte chunk, and at least
    // 64 bytes for whatever remains. Round the allocation up, since only a
    // maximum of 15 bytes will be wasted.
    buf->Grow((1 + ((data.size() - 1) / 16)) * 79);
    auto d = Dumper(buf);
    d->Write(data);
    d->Close();
    return buf->str();
}

ptr<io::WriteCloser> Dumper(ptr<io::Writer> w) { return make_ptr<dumper>(std::move(w)); }
ptr<io::Writer> NewEncoder(ptr<io::Writer> w) { return make_ptr<encoder>(std::move(w)); }
ptr<io::Reader> NewDecoder(ptr<io::Reader> r) { return make_ptr<decoder>(std::move(r)); }

}  // namespace gopp::hex
