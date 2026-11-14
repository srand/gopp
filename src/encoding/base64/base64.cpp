// encoding/base64, ported from Go 1.27.3's encoding/base64/base64.go.
#include <gopp/encoding/base64.hpp>
#include <gopp/slices.hpp>
#include <gopp/strings.hpp>

#include <algorithm>
#include <cstring>

namespace gopp::base64 {

string CorruptInputError::Error() const { return "illegal base64 data at input byte " + strconv::FormatInt(Offset, 10); }

// ---------------------------------------------------------------------------
// Encoder

void Encoding::Encode(std::span<byte> dst, std::span<const byte> src) const noexcept {
    if (src.empty()) return;
    std::size_t di = 0, si = 0;
    std::size_t n = (src.size() / 3) * 3;
    while (si < n) {
        // Convert 3x 8bit source bytes into 4 bytes
        auto val = static_cast<unsigned>(src[si]) << 16 | static_cast<unsigned>(src[si + 1]) << 8 | src[si + 2];
        dst[di + 0] = encode_[val >> 18 & 0x3F];
        dst[di + 1] = encode_[val >> 12 & 0x3F];
        dst[di + 2] = encode_[val >> 6 & 0x3F];
        dst[di + 3] = encode_[val & 0x3F];
        si += 3;
        di += 4;
    }
    std::size_t remain = src.size() - si;
    if (remain == 0) return;
    // Add the remaining small block
    unsigned val = static_cast<unsigned>(src[si]) << 16;
    if (remain == 2) val |= static_cast<unsigned>(src[si + 1]) << 8;
    dst[di + 0] = encode_[val >> 18 & 0x3F];
    dst[di + 1] = encode_[val >> 12 & 0x3F];
    switch (remain) {
    case 2:
        dst[di + 2] = encode_[val >> 6 & 0x3F];
        if (pad_char_ != NoPadding) dst[di + 3] = static_cast<byte>(pad_char_);
        break;
    case 1:
        if (pad_char_ != NoPadding) {
            dst[di + 2] = static_cast<byte>(pad_char_);
            dst[di + 3] = static_cast<byte>(pad_char_);
        }
        break;
    }
}

bytes Encoding::AppendEncode(bytes dst, std::span<const byte> src) const {
    auto n = static_cast<std::size_t>(EncodedLen(static_cast<isize>(src.size())));
    std::size_t len = dst.len();
    dst = slices::Grow(std::move(dst), static_cast<isize>(n)).Sub(0, len + n);
    Encode(std::span<byte>(dst.data() + len, n), src);
    return dst;
}

string Encoding::EncodeToString(std::span<const byte> src) const {
    auto n = static_cast<std::size_t>(EncodedLen(static_cast<isize>(src.size())));
    return string::build(n, [&](char* p) { Encode(std::span<byte>(reinterpret_cast<byte*>(p), n), src); });
}

namespace {

class encoder final : public io::WriteCloser {
public:
    encoder(const Encoding& enc, ptr<io::Writer> w) : enc_(enc), w_(std::move(w)) {}

    std::pair<isize, error> Write(std::span<const byte> p) override {
        if (err_) return {0, err_};
        isize n = 0;
        // Leading fringe.
        if (nbuf_ > 0) {
            std::size_t i = 0;
            for (; i < p.size() && nbuf_ < 3; i++) buf_[nbuf_++] = p[i];
            n += static_cast<isize>(i);
            p = p.subspan(i);
            if (nbuf_ < 3) return {n, nullptr};
            enc_.Encode(out_, buf_);
            if ((err_ = w_->Write(std::span<const byte>(out_.data(), 4)).second)) return {n, err_};
            nbuf_ = 0;
        }
        // Large interior chunks.
        while (p.size() >= 3) {
            std::size_t nn = out_.size() / 4 * 3;
            if (nn > p.size()) {
                nn = p.size();
                nn -= nn % 3;
            }
            enc_.Encode(out_, p.first(nn));
            if ((err_ = w_->Write(std::span<const byte>(out_.data(), nn / 3 * 4)).second)) return {n, err_};
            n += static_cast<isize>(nn);
            p = p.subspan(nn);
        }
        // Trailing fringe.
        std::copy(p.begin(), p.end(), buf_.begin());
        nbuf_ = p.size();
        n += static_cast<isize>(p.size());
        return {n, nullptr};
    }

    // Flushes any pending output from the encoder. It is an error to call
    // Write after calling Close.
    error Close() override {
        // If there's anything left in the buffer, flush it out
        if (!err_ && nbuf_ > 0) {
            enc_.Encode(out_, std::span<const byte>(buf_.data(), nbuf_));
            auto n = static_cast<std::size_t>(enc_.EncodedLen(static_cast<isize>(nbuf_)));
            err_ = w_->Write(std::span<const byte>(out_.data(), n)).second;
            nbuf_ = 0;
        }
        return err_;
    }

private:
    error err_;
    Encoding enc_;
    ptr<io::Writer> w_;
    std::array<byte, 3> buf_{};  // buffered data waiting to be encoded
    std::size_t nbuf_ = 0;       // number of bytes in buf
    std::array<byte, 1024> out_;  // output buffer
};

}  // namespace

ptr<io::WriteCloser> NewEncoder(const Encoding& enc, ptr<io::Writer> w) { return make_ptr<encoder>(enc, std::move(w)); }

// ---------------------------------------------------------------------------
// Decoder

// Go's decodeQuantum: decodes up to 4 base64 bytes. Returns the new index
// into src, the bytes written to dst, and an error.
std::tuple<isize, isize, error> Encoding::decode_quantum(std::span<byte> dst, std::span<const byte> src, isize si_) const {
    auto si = static_cast<std::size_t>(si_);
    auto corrupt = [](std::size_t off) { return make_ptr<CorruptInputError>(static_cast<int64>(off)); };
    // Decode quantum using the base64 alphabet
    std::array<byte, 4> dbuf{};
    std::size_t dlen = 4;
    error err;

    for (std::size_t j = 0; j < dbuf.size(); j++) {
        if (src.size() == si) {
            if (j == 0) return {static_cast<isize>(si), 0, nullptr};
            if (j == 1 || pad_char_ != NoPadding) return {static_cast<isize>(si), 0, corrupt(si - j)};
            dlen = j;
            break;
        }
        byte in = src[si];
        si++;

        byte out = decode_map_[in];
        if (out != 0xff) {
            dbuf[j] = out;
            continue;
        }

        if (in == '\n' || in == '\r') {
            j--;
            continue;
        }

        if (static_cast<rune>(in) != pad_char_) return {static_cast<isize>(si), 0, corrupt(si - 1)};

        // We've reached the end and there's padding
        switch (j) {
        case 0:
        case 1:
            // incorrect padding
            return {static_cast<isize>(si), 0, corrupt(si - 1)};
        case 2:
            // "==" is expected, the first "=" is already consumed.
            // skip over newlines
            while (si < src.size() && (src[si] == '\n' || src[si] == '\r')) si++;
            if (si == src.size()) {
                // not enough padding
                return {static_cast<isize>(si), 0, corrupt(src.size())};
            }
            if (static_cast<rune>(src[si]) != pad_char_) {
                // incorrect padding
                return {static_cast<isize>(si), 0, corrupt(si - 1)};
            }
            si++;
        }

        // skip over newlines
        while (si < src.size() && (src[si] == '\n' || src[si] == '\r')) si++;
        if (si < src.size()) {
            // trailing garbage
            err = corrupt(si);
        }
        dlen = j;
        break;
    }

    // Convert 4x 6bit source bytes into 3 bytes
    unsigned val = static_cast<unsigned>(dbuf[0]) << 18 | static_cast<unsigned>(dbuf[1]) << 12 |
                   static_cast<unsigned>(dbuf[2]) << 6 | dbuf[3];
    dbuf[2] = static_cast<byte>(val >> 0);
    dbuf[1] = static_cast<byte>(val >> 8);
    dbuf[0] = static_cast<byte>(val >> 16);
    switch (dlen) {
    case 4:
        dst[2] = dbuf[2];
        dbuf[2] = 0;
        [[fallthrough]];
    case 3:
        dst[1] = dbuf[1];
        if (strict_ && dbuf[2] != 0) return {static_cast<isize>(si), 0, corrupt(si - 1)};
        dbuf[1] = 0;
        [[fallthrough]];
    case 2:
        dst[0] = dbuf[0];
        if (strict_ && (dbuf[1] != 0 || dbuf[2] != 0)) return {static_cast<isize>(si), 0, corrupt(si - 2)};
    }

    return {static_cast<isize>(si), static_cast<isize>(dlen) - 1, err};
}

std::pair<bytes, error> Encoding::AppendDecode(bytes dst, std::span<const byte> src) const {
    // Compute the output size without padding to avoid over allocating.
    std::size_t n = src.size();
    while (n > 0 && static_cast<rune>(src[n - 1]) == pad_char_) n--;
    n = static_cast<std::size_t>(decoded_len(static_cast<isize>(n), NoPadding));

    std::size_t len = dst.len();
    dst = slices::Grow(std::move(dst), static_cast<isize>(n)).Sub(0, len + n);
    auto [m, err] = Decode(std::span<byte>(dst.data() + len, n), src);
    return {dst.Sub(0, len + static_cast<std::size_t>(m)), err};
}

std::pair<bytes, error> Encoding::DecodeString(std::string_view s) const {
    auto dbuf = bytes::make(static_cast<std::size_t>(DecodedLen(static_cast<isize>(s.size()))));
    auto [n, err] = Decode(dbuf, as_bytes(s));
    return {dbuf.Sub(0, static_cast<std::size_t>(n)), err};
}

namespace {

// Go's assemble32 and assemble64: the 6-bit values as big-endian bits, if
// none of them is invalid (0xff).
bool assemble32(const std::array<byte, 256>& m, const byte* s, uint32& dn) {
    byte n1 = m[s[0]], n2 = m[s[1]], n3 = m[s[2]], n4 = m[s[3]];
    if ((n1 | n2 | n3 | n4) == 0xff) return false;
    dn = uint32(n1) << 26 | uint32(n2) << 20 | uint32(n3) << 14 | uint32(n4) << 8;
    return true;
}

bool assemble64(const std::array<byte, 256>& m, const byte* s, uint64& dn) {
    byte n[8];
    byte all = 0;
    for (int i = 0; i < 8; i++) all |= n[i] = m[s[i]];
    if (all == 0xff) return false;
    dn = 0;
    for (int i = 0; i < 8; i++) dn |= uint64(n[i]) << (58 - 6 * i);
    return true;
}

void put_be(byte* dst, uint64 v, int nbytes) {
    for (int i = 0; i < nbytes; i++) dst[i] = static_cast<byte>(v >> (8 * (nbytes - 1 - i)));
}

}  // namespace

std::pair<isize, error> Encoding::Decode(std::span<byte> dst, std::span<const byte> src) const {
    if (src.empty()) return {0, nullptr};

    std::size_t n = 0, si = 0;
    error err;
    auto quantum = [&]() -> bool {
        auto [nsi, ninc, e] = decode_quantum(dst.subspan(n), src, static_cast<isize>(si));
        si = static_cast<std::size_t>(nsi);
        n += static_cast<std::size_t>(ninc);
        err = e;
        return !err;
    };
    while (src.size() - si >= 8 && dst.size() - n >= 8) {
        uint64 dn;
        if (assemble64(decode_map_, src.data() + si, dn)) {
            put_be(dst.data() + n, dn, 8);
            n += 6;
            si += 8;
        } else if (!quantum()) {
            return {static_cast<isize>(n), err};
        }
    }

    while (src.size() - si >= 4 && dst.size() - n >= 4) {
        uint32 dn;
        if (assemble32(decode_map_, src.data() + si, dn)) {
            put_be(dst.data() + n, dn, 4);
            n += 3;
            si += 4;
        } else if (!quantum()) {
            return {static_cast<isize>(n), err};
        }
    }

    while (si < src.size()) {
        if (!quantum()) return {static_cast<isize>(n), err};
    }
    return {static_cast<isize>(n), err};
}

namespace {

class decoder final : public io::Reader {
public:
    decoder(const Encoding& enc, ptr<io::Reader> r) : enc_(enc), r_(std::move(r)) {}

    std::pair<isize, error> Read(std::span<byte> p) override {
        // Use leftover decoded output from last read.
        if (!out_.empty()) {
            std::size_t n = std::min(p.size(), out_.size());
            std::memcpy(p.data(), out_.data(), n);
            out_ = out_.subspan(n);
            return {static_cast<isize>(n), nullptr};
        }

        if (err_) return {0, err_};

        // This code assumes that d.r strips supported whitespace ('\r' and '\n').

        // Refill buffer.
        while (nbuf_ < 4 && !read_err_) {
            std::size_t nn = std::max<std::size_t>(p.size() / 3 * 4, 4);
            nn = std::min(nn, buf_.size());
            auto [m, e] = r_->Read(std::span<byte>(buf_.data() + nbuf_, nn - nbuf_));
            read_err_ = e;
            nbuf_ += static_cast<std::size_t>(m);
        }

        if (nbuf_ < 4) {
            if (enc_.pad_char() == NoPadding && nbuf_ > 0) {
                // Decode final fragment, without padding.
                auto [nw, e] = enc_.Decode(outbuf_, std::span<const byte>(buf_.data(), nbuf_));
                err_ = e;
                nbuf_ = 0;
                out_ = std::span<const byte>(outbuf_.data(), static_cast<std::size_t>(nw));
                std::size_t n = std::min(p.size(), out_.size());
                std::memcpy(p.data(), out_.data(), n);
                out_ = out_.subspan(n);
                if (n > 0 || (p.empty() && !out_.empty())) return {static_cast<isize>(n), nullptr};
                if (err_) return {0, err_};
            }
            err_ = read_err_;
            if (err_ == io::EOF_ && nbuf_ > 0) err_ = io::ErrUnexpectedEOF;
            return {0, err_};
        }

        // Decode chunk into p, or d.out and then p if p is too small.
        std::size_t nr = nbuf_ / 4 * 4;
        std::size_t nw = nbuf_ / 4 * 3;
        isize n;
        if (nw > p.size()) {
            auto [m, e] = enc_.Decode(outbuf_, std::span<const byte>(buf_.data(), nr));
            err_ = e;
            out_ = std::span<const byte>(outbuf_.data(), static_cast<std::size_t>(m));
            std::size_t c = std::min(p.size(), out_.size());
            std::memcpy(p.data(), out_.data(), c);
            out_ = out_.subspan(c);
            n = static_cast<isize>(c);
        } else {
            auto [m, e] = enc_.Decode(p, std::span<const byte>(buf_.data(), nr));
            err_ = e;
            n = m;
        }
        nbuf_ -= nr;
        std::memmove(buf_.data(), buf_.data() + nr, nbuf_);
        return {n, err_};
    }

private:
    error err_;
    error read_err_;  // error from r.Read
    Encoding enc_;
    ptr<io::Reader> r_;
    std::array<byte, 1024> buf_;  // leftover input
    std::size_t nbuf_ = 0;
    std::span<const byte> out_;  // leftover decoded output
    std::array<byte, 1024 / 4 * 3> outbuf_;
};

// Drops '\r' and '\n' from what it reads.
class newline_filtering_reader final : public io::Reader {
public:
    explicit newline_filtering_reader(ptr<io::Reader> r) : wrapped_(std::move(r)) {}

    std::pair<isize, error> Read(std::span<byte> p) override {
        auto [n, err] = wrapped_->Read(p);
        while (n > 0) {
            std::size_t offset = 0;
            for (std::size_t i = 0; i < static_cast<std::size_t>(n); i++) {
                byte b = p[i];
                if (b != '\r' && b != '\n') {
                    if (i != offset) p[offset] = b;
                    offset++;
                }
            }
            if (offset > 0) return {static_cast<isize>(offset), err};
            // Previous buffer entirely whitespace, read again
            std::tie(n, err) = wrapped_->Read(p);
        }
        return {n, err};
    }

private:
    ptr<io::Reader> wrapped_;
};

}  // namespace

ptr<io::Reader> NewDecoder(const Encoding& enc, ptr<io::Reader> r) {
    return make_ptr<decoder>(enc, make_ptr<newline_filtering_reader>(std::move(r)));
}

}  // namespace gopp::base64
