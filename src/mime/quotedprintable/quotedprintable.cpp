// The mime/quotedprintable package, ported from Go 1.27.3's
// mime/quotedprintable/reader.go and writer.go.
#include <gopp/fmt.hpp>
#include <gopp/mime/quotedprintable.hpp>
#include <gopp/strings.hpp>

namespace gopp::quotedprintable {

// ---------------------------------------------------------------------------
// Reader

Reader::Reader(ptr<io::Reader> r) : br_(make_ptr<bufio::Reader>(std::move(r))) {}

ptr<Reader> NewReader(ptr<io::Reader> r) { return make_ptr<Reader>(std::move(r)); }

namespace {

std::pair<byte, error> fromHex(byte b) {
    if (b >= '0' && b <= '9') return {byte(b - '0'), nullptr};
    if (b >= 'A' && b <= 'F') return {byte(b - 'A' + 10), nullptr};
    // Accept badly encoded bytes.
    if (b >= 'a' && b <= 'f') return {byte(b - 'a' + 10), nullptr};
    return {0, fmt::Errorf("quotedprintable: invalid hex byte 0x{:02x}", b)};
}

std::pair<byte, error> readHexByte(std::string_view v) {
    if (v.size() < 2) return {0, io::ErrUnexpectedEOF};
    auto [hb, err] = fromHex(byte(v[0]));
    if (err) return {0, err};
    auto [lb, err2] = fromHex(byte(v[1]));
    if (err2) return {0, err2};
    return {byte(hb << 4 | lb), nullptr};
}

bool isQPDiscardWhitespace(char r) { return r == '\n' || r == '\r' || r == ' ' || r == '\t'; }

}  // namespace

std::pair<isize, error> Reader::Read(std::span<byte> p) {
    // Deviations from RFC 2045:
    // 1. in addition to "=\r\n", "=\n" is also treated as soft line break.
    // 2. it will pass through a '\r' or '\n' not preceded by '=', consistent
    //    with other broken QP encoders & decoders.
    // 3. it accepts soft line-break (=) at end of message (issue 15486);
    //    i.e. if the final byte read from the underlying reader is =, it
    //    will be ignored.
    isize n = 0;
    while (!p.empty()) {
        std::string_view line = std::string_view(line_).substr(pos_);
        if (line.empty()) {
            if (rerr_) return {n, rerr_};
            auto [l, err] = br_->ReadSlice('\n');
            rerr_ = err;
            std::string_view wholeLine = asStringView(l);
            // Note: we could try to strip the line's trailing spaces as the
            // RFC requires, but Go doesn't: see its comment.
            bool hasLF = wholeLine.ends_with('\n');
            bool hasCR = wholeLine.ends_with("\r\n");
            std::string_view t = wholeLine;
            while (!t.empty() && isQPDiscardWhitespace(t.back())) t.remove_suffix(1);
            line_.assign(t);
            pos_ = 0;
            if (t.ends_with('=')) {
                auto rightStripped = wholeLine.substr(t.size());
                while (!rightStripped.empty() && (rightStripped[0] == ' ' || rightStripped[0] == '\t')) {
                    rightStripped.remove_prefix(1);
                }
                line_.pop_back();
                if (!rightStripped.starts_with('\n') && !rightStripped.starts_with("\r\n") &&
                    !(rightStripped.empty() && !line_.empty() && rerr_ == io::EOF_)) {
                    rerr_ = fmt::Errorf("quotedprintable: invalid bytes after =: {}", strconv::Quote(rightStripped));
                }
            } else if (hasLF) {
                line_ += hasCR ? "\r\n" : "\n";
            }
            continue;
        }
        byte b = byte(line[0]);
        if (b == '=') {
            auto [hb, err] = readHexByte(line.substr(1));
            if (err) {
                if (line.size() >= 2 && line[1] != '\r' && line[1] != '\n') {
                    // Not a valid escape: pass the '=' through.
                    b = '=';
                } else {
                    return {n, err};
                }
            } else {
                b = hb;
                pos_ += 2;  // 2 of the 3; other 1 is done below
            }
        } else if (b == '\t' || b == '\r' || b == '\n' || b >= 0x80) {
            // Pass through; b >= 0x80 is invalid, but other decoders accept
            // it.
        } else if (b < ' ' || b > '~') {
            return {n, fmt::Errorf("quotedprintable: invalid unescaped byte 0x{:02x} in body", b)};
        }
        p[0] = b;
        p = p.subspan(1);
        pos_++;
        n++;
    }
    return {n, nullptr};
}

// ---------------------------------------------------------------------------
// Writer

namespace {

constexpr int lineMaxLen = 76;
constexpr std::string_view upperhex = "0123456789ABCDEF";

bool isWhitespace(byte b) { return b == ' ' || b == '\t'; }

}  // namespace

ptr<Writer> NewWriter(ptr<io::Writer> w) { return make_ptr<Writer>(std::move(w)); }

std::pair<isize, error> Writer::Write(std::span<const byte> p) {
    std::size_t n = 0;
    for (std::size_t i = 0; i < p.size(); i++) {
        byte b = p[i];
        // Simple writes are done in batch.
        if (b >= '!' && b <= '~' && b != '=') continue;
        if (isWhitespace(b) || (!Binary && (b == '\n' || b == '\r'))) continue;
        if (i > n) {
            if (auto err = write(p.subspan(n, i - n))) return {isize(n), err};
            n = i;
        }
        if (auto err = encode(b)) return {isize(n), err};
        n++;
    }
    if (n == p.size()) return {isize(n), nullptr};
    if (auto err = write(p.subspan(n))) return {isize(n), err};
    return {isize(p.size()), nullptr};
}

error Writer::Close() {
    if (auto err = checkLastByte()) return err;
    return flush();
}

// write limits text encoded in quoted-printable to 76 characters per line.
error Writer::write(std::span<const byte> p) {
    for (byte b : p) {
        if (b == '\n' || b == '\r') {
            // If the previous byte was \r, the CRLF has already been
            // inserted.
            if (cr_ && b == '\n') {
                cr_ = false;
                continue;
            }
            if (b == '\r') cr_ = true;
            if (auto err = checkLastByte()) return err;
            if (auto err = insertCRLF()) return err;
            continue;
        }
        if (i_ == lineMaxLen - 1) {
            if (auto err = insertSoftLineBreak()) return err;
        }
        line_[i_++] = b;
        cr_ = false;
    }
    return nullptr;
}

error Writer::encode(byte b) {
    if (lineMaxLen - 1 - i_ < 3) {
        if (auto err = insertSoftLineBreak()) return err;
    }
    line_[i_] = '=';
    line_[i_ + 1] = byte(upperhex[b >> 4]);
    line_[i_ + 2] = byte(upperhex[b & 0x0f]);
    i_ += 3;
    return nullptr;
}

// checkLastByte encodes the last buffered byte if it is a space or a tab.
error Writer::checkLastByte() {
    if (i_ == 0) return nullptr;
    byte b = line_[i_ - 1];
    if (isWhitespace(b)) {
        i_--;
        if (auto err = encode(b)) return err;
    }
    return nullptr;
}

error Writer::insertSoftLineBreak() {
    line_[i_++] = '=';
    return insertCRLF();
}

error Writer::insertCRLF() {
    line_[i_] = '\r';
    line_[i_ + 1] = '\n';
    i_ += 2;
    return flush();
}

error Writer::flush() {
    if (auto [_, err] = w_->Write(std::span<const byte>(line_, std::size_t(i_))); err) return err;
    i_ = 0;
    return nullptr;
}

}  // namespace gopp::quotedprintable
