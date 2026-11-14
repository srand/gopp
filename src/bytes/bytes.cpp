// Ported from Go 1.27.3's bytes/bytes.go, iter.go, buffer.go and reader.go,
// over the algorithms in strings/text.hpp that it shares with strings.
#include <gopp/bytes.hpp>
#include <gopp/unicode/utf8.hpp>

#include "../strings/text.hpp"

#include <cstring>
#include <limits>

namespace gopp::bytes {

namespace text = strings::text;

namespace {

std::string_view sv(std::span<const byte> b) noexcept { return asStringView(b); }

// The part v of s as a subslice sharing s's array, with its capacity up to
// s's, or, if limit_cap, ending with it (Go's s[i:j:j]); nil for nil.
slice<byte> sub(const slice<byte>& s, std::string_view v, bool limit_cap = false) {
    if (v.data() == nullptr) return {};
    auto i = std::size_t(reinterpret_cast<const byte*>(v.data()) - s.data());
    if (v.empty() && s.isNil()) return {};
    return limit_cap ? s.Sub(i, i + v.size(), i + v.size()) : s.Sub(i, i + v.size());
}

slice<slice<byte>> subs(const slice<byte>& s, const std::vector<std::string_view>& parts) {
    auto out = slice<slice<byte>>::make(parts.size());
    for (std::size_t i = 0; i < parts.size(); i++) out[i] = sub(s, parts[i], true);
    return out;
}

slice<byte> copyOf(std::string_view s) { return slice<byte>::of(as_bytes(s)); }

// The result r, or a copy of s if r is empty (the result equals s).
slice<byte> orCopy(std::span<const byte> s, const std::optional<std::string>& r) {
    return r ? copyOf(*r) : slice<byte>::of(s);
}

const error errNegativeRead = errors::New("bytes.Buffer: reader returned negative count from Read");
const error errUnreadByte = errors::New("bytes.Buffer: UnreadByte: previous operation was not a successful read");

constexpr std::size_t smallBufferSize = 64;

}  // namespace

bool Equal(std::span<const byte> a, std::span<const byte> b) noexcept { return sv(a) == sv(b); }
int Compare(std::span<const byte> a, std::span<const byte> b) noexcept { return text::compare(sv(a), sv(b)); }
bool HasPrefix(std::span<const byte> s, std::span<const byte> prefix) noexcept { return sv(s).starts_with(sv(prefix)); }
bool HasSuffix(std::span<const byte> s, std::span<const byte> suffix) noexcept { return sv(s).ends_with(sv(suffix)); }
bool Contains(std::span<const byte> b, std::span<const byte> subslice) noexcept { return Index(b, subslice) != -1; }
bool ContainsAny(std::span<const byte> b, std::string_view chars) noexcept { return IndexAny(b, chars) >= 0; }
bool ContainsRune(std::span<const byte> b, rune r) noexcept { return IndexRune(b, r) >= 0; }
bool ContainsFunc(std::span<const byte> b, std::function_ref<bool(rune)> f) { return IndexFunc(b, f) >= 0; }

isize Index(std::span<const byte> s, std::span<const byte> sep) noexcept { return text::index(sv(s), sv(sep)); }
isize IndexByte(std::span<const byte> b, byte c) noexcept { return text::indexByte(sv(b), char(c)); }
isize IndexRune(std::span<const byte> s, rune r) noexcept { return text::indexRune(sv(s), r); }
isize IndexAny(std::span<const byte> s, std::string_view chars) noexcept { return text::indexAny(sv(s), chars); }
isize IndexFunc(std::span<const byte> s, std::function_ref<bool(rune)> f) { return text::indexFunc(sv(s), f); }
isize LastIndex(std::span<const byte> s, std::span<const byte> sep) noexcept { return text::lastIndex(sv(s), sv(sep)); }
isize LastIndexByte(std::span<const byte> s, byte c) noexcept { return text::lastIndexByte(sv(s), char(c)); }
isize LastIndexAny(std::span<const byte> s, std::string_view chars) noexcept { return text::lastIndexAny(sv(s), chars); }
isize LastIndexFunc(std::span<const byte> s, std::function_ref<bool(rune)> f) { return text::lastIndexFunc(sv(s), f); }
isize Count(std::span<const byte> s, std::span<const byte> sep) noexcept { return text::count(sv(s), sv(sep)); }
bool EqualFold(std::span<const byte> s, std::span<const byte> t) noexcept { return text::equal_fold(sv(s), sv(t)); }

std::tuple<slice<byte>, slice<byte>, bool> Cut(const slice<byte>& s, std::span<const byte> sep) {
    auto r = text::cut(sv(s), sv(sep));
    return {sub(s, r.before), sub(s, r.after), r.found};
}

std::tuple<slice<byte>, slice<byte>, bool> CutLast(const slice<byte>& s, std::span<const byte> sep) {
    auto r = text::cutLast(sv(s), sv(sep));
    return {sub(s, r.before), sub(s, r.after), r.found};
}

std::pair<slice<byte>, bool> CutPrefix(const slice<byte>& s, std::span<const byte> prefix) {
    if (!HasPrefix(s, prefix)) return {s, false};
    return {s.Sub(prefix.size()), true};
}

std::pair<slice<byte>, bool> CutSuffix(const slice<byte>& s, std::span<const byte> suffix) {
    if (!HasSuffix(s, suffix)) return {s, false};
    return {s.Sub(0, s.len() - suffix.size()), true};
}

slice<byte> TrimSpace(const slice<byte>& s) { return sub(s, text::trim_space(sv(s))); }
slice<byte> Trim(const slice<byte>& s, std::string_view cutset) { return sub(s, text::trim(sv(s), cutset)); }
slice<byte> TrimLeft(const slice<byte>& s, std::string_view cutset) { return sub(s, text::trimLeft(sv(s), cutset)); }
slice<byte> TrimRight(const slice<byte>& s, std::string_view cutset) { return sub(s, text::trimRight(sv(s), cutset)); }

slice<byte> TrimPrefix(const slice<byte>& s, std::span<const byte> prefix) {
    return HasPrefix(s, prefix) ? s.Sub(prefix.size()) : s;
}

slice<byte> TrimSuffix(const slice<byte>& s, std::span<const byte> suffix) {
    return HasSuffix(s, suffix) ? s.Sub(0, s.len() - suffix.size()) : s;
}

slice<byte> TrimFunc(const slice<byte>& s, std::function_ref<bool(rune)> f) { return sub(s, text::trimFunc(sv(s), f)); }

slice<byte> TrimLeftFunc(const slice<byte>& s, std::function_ref<bool(rune)> f) {
    return sub(s, text::trimLeftFunc(sv(s), f));
}

slice<byte> TrimRightFunc(const slice<byte>& s, std::function_ref<bool(rune)> f) {
    return sub(s, text::trimRightFunc(sv(s), f));
}

slice<slice<byte>> Split(const slice<byte>& s, std::span<const byte> sep) {
    return subs(s, text::genSplit(sv(s), sv(sep), 0, -1));
}

slice<slice<byte>> SplitN(const slice<byte>& s, std::span<const byte> sep, isize n) {
    if (n == 0) return {};
    return subs(s, text::genSplit(sv(s), sv(sep), 0, n));
}

slice<slice<byte>> SplitAfter(const slice<byte>& s, std::span<const byte> sep) {
    return subs(s, text::genSplit(sv(s), sv(sep), sep.size(), -1));
}

slice<slice<byte>> SplitAfterN(const slice<byte>& s, std::span<const byte> sep, isize n) {
    if (n == 0) return {};
    return subs(s, text::genSplit(sv(s), sv(sep), sep.size(), n));
}

slice<slice<byte>> Fields(const slice<byte>& s) { return subs(s, text::fields(sv(s))); }

slice<slice<byte>> FieldsFunc(const slice<byte>& s, std::function_ref<bool(rune)> f) {
    return subs(s, text::fieldsFunc(sv(s), f));
}

std::generator<slice<byte>> Lines(slice<byte> s) {
    while (!s.empty()) {
        isize i = IndexByte(s, '\n');
        std::size_t end = i >= 0 ? std::size_t(i) + 1 : s.len();
        co_yield s.Sub(0, end, end);
        s = s.Sub(end);
    }
}

namespace {

std::generator<slice<byte>> splitSeq(slice<byte> s, std::string sep, std::size_t sep_save) {
    if (sep.empty()) {
        while (!s.empty()) {
            auto [_, size] = utf8::DecodeRune(s);
            co_yield s.Sub(0, std::size_t(size), std::size_t(size));
            s = s.Sub(std::size_t(size));
        }
        co_return;
    }
    for (;;) {
        isize i = Index(s, as_bytes(sep));
        if (i < 0) break;
        std::size_t end = std::size_t(i) + sep_save;
        co_yield s.Sub(0, end, end);
        s = s.Sub(std::size_t(i) + sep.size());
    }
    co_yield s.Sub(0, s.len(), s.len());
}

}  // namespace

std::generator<slice<byte>> SplitSeq(slice<byte> s, std::span<const byte> sep) {
    return splitSeq(std::move(s), std::string(sv(sep)), 0);
}

std::generator<slice<byte>> SplitAfterSeq(slice<byte> s, std::span<const byte> sep) {
    return splitSeq(std::move(s), std::string(sv(sep)), sep.size());
}

std::generator<slice<byte>> FieldsSeq(slice<byte> s) {
    isize start = -1;
    for (std::size_t i = 0; i < s.len();) {
        int size = 1;
        rune r = s[i];
        bool isSpace = text::asciiSpace(s[i]);
        if (r >= utf8::RuneSelf) {
            std::tie(r, size) = utf8::DecodeRune(s.span().subspan(i));
            isSpace = unicode::IsSpace(r);
        }
        if (isSpace) {
            if (start >= 0) {
                co_yield s.Sub(std::size_t(start), i, i);
                start = -1;
            }
        } else if (start < 0) {
            start = isize(i);
        }
        i += std::size_t(size);
    }
    if (start >= 0) co_yield s.Sub(std::size_t(start), s.len(), s.len());
}

std::generator<slice<byte>> FieldsFuncSeq(slice<byte> s, std::function<bool(rune)> f) {
    isize start = -1;
    for (std::size_t i = 0; i < s.len();) {
        auto [r, size] = utf8::DecodeRune(s.span().subspan(i));
        if (f(r)) {
            if (start >= 0) {
                co_yield s.Sub(std::size_t(start), i, i);
                start = -1;
            }
        } else if (start < 0) {
            start = isize(i);
        }
        i += std::size_t(size);
    }
    if (start >= 0) co_yield s.Sub(std::size_t(start), s.len(), s.len());
}

slice<byte> ToLower(std::span<const byte> s) { return orCopy(s, text::toLower(sv(s))); }
slice<byte> ToUpper(std::span<const byte> s) { return orCopy(s, text::toUpper(sv(s))); }
slice<byte> ToTitle(std::span<const byte> s) { return copyOf(text::map(unicode::ToTitle, sv(s))); }

slice<byte> ToLowerSpecial(const unicode::SpecialCase& c, std::span<const byte> s) {
    return copyOf(text::map([&](rune r) { return c.ToLower(r); }, sv(s)));
}

slice<byte> ToUpperSpecial(const unicode::SpecialCase& c, std::span<const byte> s) {
    return copyOf(text::map([&](rune r) { return c.ToUpper(r); }, sv(s)));
}

slice<byte> ToTitleSpecial(const unicode::SpecialCase& c, std::span<const byte> s) {
    return copyOf(text::map([&](rune r) { return c.ToTitle(r); }, sv(s)));
}

slice<byte> ToValidUTF8(std::span<const byte> s, std::span<const byte> replacement) {
    return orCopy(s, text::toValidUtf8(sv(s), sv(replacement)));
}

slice<byte> Map(std::function_ref<rune(rune)> mapping, std::span<const byte> s) {
    return copyOf(text::map(mapping, sv(s)));
}

slice<byte> Repeat(std::span<const byte> b, isize count) { return copyOf(text::repeat(sv(b), count, "bytes")); }

slice<byte> Replace(std::span<const byte> s, std::span<const byte> old, std::span<const byte> new_, isize n) {
    auto r = text::replace(sv(s), sv(old), sv(new_), n);
    // Go returns append([]byte(nil), s...) when nothing is replaced: nil
    // for an empty s.
    if (!r) return s.empty() ? slice<byte>() : slice<byte>::of(s);
    return copyOf(*r);
}

slice<byte> ReplaceAll(std::span<const byte> s, std::span<const byte> old, std::span<const byte> new_) {
    return Replace(s, old, new_, -1);
}

slice<byte> Join(std::span<const slice<byte>> s, std::span<const byte> sep) {
    if (s.empty()) return slice<byte>::make(0);
    // Just return a copy.
    if (s.size() == 1) return s[0].empty() ? slice<byte>() : slice<byte>::of(s[0]);
    std::size_t n = sep.size() * (s.size() - 1);
    for (const auto& v : s) n += v.len();
    auto b = slice<byte>::make(n);
    std::size_t bp = Copy<byte>(b, s[0]);
    for (const auto& v : s.subspan(1)) {
        bp += Copy<byte>(b.span().subspan(bp), sep);
        bp += Copy<byte>(b.span().subspan(bp), v);
    }
    return b;
}

slice<rune> Runes(std::span<const byte> s) {
    auto t = slice<rune>::make(std::size_t(utf8::RuneCount(s)));
    std::size_t i = 0;
    while (!s.empty()) {
        auto [r, l] = utf8::DecodeRune(s);
        t[i++] = r;
        s = s.subspan(std::size_t(l));
    }
    return t;
}

slice<byte> Clone(const slice<byte>& b) {
    if (b.isNil()) return {};
    return slice<byte>::of(b);
}

// Buffer.

const error ErrTooLarge = errors::New("bytes.Buffer: too large");

std::pair<slice<byte>, error> Buffer::Peek(isize n) const {
    if (Len() < n) return {buf_.Sub(off_), io::EOF_};
    return {buf_.Sub(off_, off_ + std::size_t(n)), nullptr};
}

void Buffer::Truncate(isize n) {
    if (n == 0) {
        Reset();
        return;
    }
    lastRead_ = opInvalid;
    if (n < 0 || n > Len()) panic("bytes.Buffer: truncation out of range");
    buf_ = buf_.Sub(0, off_ + std::size_t(n));
}

void Buffer::Reset() noexcept {
    if (!buf_.isNil()) buf_ = buf_.Sub(0, 0);
    off_ = 0;
    lastRead_ = opInvalid;
}

// The inlineable version of grow for the fast-case where the internal
// buffer only needs to be resliced: the index where bytes should be
// written and whether it succeeded.
std::pair<std::size_t, bool> Buffer::tryGrowByReslice(std::size_t n) {
    if (std::size_t l = buf_.len(); n <= buf_.cap() - l) {
        buf_ = buf_.Sub(0, l + n);
        return {l, true};
    }
    return {0, false};
}

// Grows the buffer to guarantee space for n more bytes; returns the index
// where bytes should be written.
std::size_t Buffer::grow(std::size_t n) {
    std::size_t m = std::size_t(Len());
    // If buffer is empty, reset to recover space.
    if (m == 0 && off_ != 0) Reset();
    // Try to grow by means of a reslice.
    if (auto [i, ok] = tryGrowByReslice(n); ok) return i;
    if (buf_.isNil() && n <= smallBufferSize) {
        buf_ = slice<byte>::make(n, smallBufferSize);
        return 0;
    }
    std::size_t c = buf_.cap();
    constexpr auto maxInt = std::size_t(std::numeric_limits<isize>::max());
    if (n <= c / 2 - std::min(m, c / 2)) {
        // We can slide things down instead of allocating a new slice. We
        // only need m+n <= c to slide, but we instead let capacity get
        // twice as large so we don't spend all our time copying.
        std::memmove(buf_.data(), buf_.data() + off_, m);
    } else if (c > maxInt / 2 || n > maxInt - 2 * c) {
        // Go's c > maxInt-c-n, without wrapping around.
        panic(ErrTooLarge.Error().view());
    } else {
        // Add b.off to account for b.buf[:b.off] being sliced off the
        // front (Go's growSlice).
        std::size_t want = std::max(m + off_ + n, 2 * (c - off_));
        auto b2 = slice<byte>::make(m, want);
        std::memcpy(b2.data(), buf_.data() + off_, m);
        buf_ = std::move(b2);
    }
    // Restore b.off and len(b.buf).
    off_ = 0;
    buf_ = buf_.Sub(0, m + n);
    return m;
}

void Buffer::Grow(isize n) {
    if (n < 0) panic("bytes.Buffer.Grow: negative count");
    std::size_t m = grow(std::size_t(n));
    buf_ = buf_.Sub(0, m);
}

std::pair<isize, error> Buffer::Write(std::span<const byte> p) {
    lastRead_ = opInvalid;
    auto [m, ok] = tryGrowByReslice(p.size());
    if (!ok) m = grow(p.size());
    return {isize(Copy<byte>(buf_.span().subspan(m), p)), nullptr};
}

std::pair<isize, error> Buffer::WriteString(std::string_view s) { return Write(as_bytes(s)); }

error Buffer::WriteByte(byte c) {
    lastRead_ = opInvalid;
    auto [m, ok] = tryGrowByReslice(1);
    if (!ok) m = grow(1);
    buf_[m] = c;
    return nullptr;
}

std::pair<isize, error> Buffer::WriteRune(rune r) {
    // Compare as uint32 to correctly handle negative runes.
    if (uint32(r) < uint32(utf8::RuneSelf)) {
        WriteByte(byte(r));
        return {1, nullptr};
    }
    lastRead_ = opInvalid;
    auto [m, ok] = tryGrowByReslice(utf8::UTFMax);
    if (!ok) m = grow(utf8::UTFMax);
    buf_ = utf8::AppendRune(buf_.Sub(0, m), r);
    return {isize(buf_.len() - m), nullptr};
}

std::pair<int64, error> Buffer::ReadFrom(io::Reader& r) {
    lastRead_ = opInvalid;
    int64 n = 0;
    for (;;) {
        std::size_t i = grow(std::size_t(MinRead));
        buf_ = buf_.Sub(0, i);
        auto [m, e] = r.Read(buf_.Sub(i, buf_.cap()).span());
        if (m < 0) panic(errNegativeRead.Error().view());
        buf_ = buf_.Sub(0, i + std::size_t(m));
        n += m;
        if (e == io::EOF_) return {n, nullptr};  // e is EOF, so return nil explicitly
        if (e) return {n, e};
    }
}

std::pair<int64, error> Buffer::WriteTo(io::Writer& w) {
    lastRead_ = opInvalid;
    int64 n = 0;
    if (isize nBytes = Len(); nBytes > 0) {
        auto [m, e] = w.Write(buf_.span().subspan(off_));
        if (m > nBytes) panic("bytes.Buffer.WriteTo: invalid Write count");
        off_ += std::size_t(m);
        n = m;
        if (e) return {n, e};
        // all bytes should have been written, by definition of Write
        // method in io.Writer
        if (m != nBytes) return {n, io::ErrShortWrite};
    }
    // Buffer is now empty; reset.
    Reset();
    return {n, nullptr};
}

std::pair<isize, error> Buffer::Read(std::span<byte> p) {
    lastRead_ = opInvalid;
    if (empty()) {
        // Buffer is empty, reset to recover space.
        Reset();
        if (p.empty()) return {0, nullptr};
        return {0, io::EOF_};
    }
    std::size_t n = Copy<byte>(p, buf_.span().subspan(off_));
    off_ += n;
    if (n > 0) lastRead_ = opRead;
    return {isize(n), nullptr};
}

slice<byte> Buffer::Next(isize n) {
    lastRead_ = opInvalid;
    isize m = Len();
    if (n > m) n = m;
    auto data = buf_.Sub(off_, off_ + std::size_t(n));
    off_ += std::size_t(n);
    if (n > 0) lastRead_ = opRead;
    return data;
}

std::pair<byte, error> Buffer::ReadByte() {
    if (empty()) {
        // Buffer is empty, reset to recover space.
        Reset();
        return {0, io::EOF_};
    }
    byte c = buf_[off_];
    off_++;
    lastRead_ = opRead;
    return {c, nullptr};
}

std::tuple<rune, int, error> Buffer::ReadRune() {
    if (empty()) {
        // Buffer is empty, reset to recover space.
        Reset();
        return {0, 0, io::EOF_};
    }
    byte c = buf_[off_];
    if (c < utf8::RuneSelf) {
        off_++;
        lastRead_ = opReadRune1;
        return {rune(c), 1, nullptr};
    }
    auto [r, n] = utf8::DecodeRune(buf_.span().subspan(off_));
    off_ += std::size_t(n);
    lastRead_ = readOp(n);
    return {r, n, nullptr};
}

error Buffer::UnreadRune() {
    if (lastRead_ <= opInvalid) {
        return errors::New("bytes.Buffer: UnreadRune: previous operation was not a successful ReadRune");
    }
    if (off_ >= std::size_t(lastRead_)) off_ -= std::size_t(lastRead_);
    lastRead_ = opInvalid;
    return nullptr;
}

error Buffer::UnreadByte() {
    if (lastRead_ == opInvalid) return errUnreadByte;
    lastRead_ = opInvalid;
    if (off_ > 0) off_--;
    return nullptr;
}

std::pair<slice<byte>, error> Buffer::readSlice(byte delim) {
    isize i = IndexByte(buf_.span().subspan(off_), delim);
    std::size_t end = off_ + std::size_t(i) + 1;
    error err;
    if (i < 0) {
        end = buf_.len();
        err = io::EOF_;
    }
    auto line = buf_.Sub(off_, end);
    off_ = end;
    lastRead_ = opRead;
    return {line, err};
}

std::pair<slice<byte>, error> Buffer::ReadBytes(byte delim) {
    auto [s, err] = readSlice(delim);
    // return a copy of slice. The buffer's backing array may be
    // overwritten by later calls.
    return {s.empty() ? slice<byte>() : slice<byte>::of(s), err};
}

std::pair<string, error> Buffer::ReadString(byte delim) {
    auto [s, err] = readSlice(delim);
    return {gopp::String(s), err};
}

// Reader.

std::pair<isize, error> Reader::Read(std::span<byte> b) {
    if (i_ >= int64(s_.len())) return {0, io::EOF_};
    prevRune_ = -1;
    std::size_t n = Copy<byte>(b, s_.span().subspan(std::size_t(i_)));
    i_ += int64(n);
    return {isize(n), nullptr};
}

std::pair<isize, error> Reader::ReadAt(std::span<byte> b, int64 off) {
    // cannot modify state - see io.ReaderAt
    if (off < 0) return {0, errors::New("bytes.Reader.ReadAt: negative offset")};
    if (off >= int64(s_.len())) return {0, io::EOF_};
    std::size_t n = Copy<byte>(b, s_.span().subspan(std::size_t(off)));
    return {isize(n), n < b.size() ? io::EOF_ : nullptr};
}

std::pair<byte, error> Reader::ReadByte() {
    prevRune_ = -1;
    if (i_ >= int64(s_.len())) return {0, io::EOF_};
    byte b = s_[std::size_t(i_)];
    i_++;
    return {b, nullptr};
}

error Reader::UnreadByte() {
    if (i_ <= 0) return errors::New("bytes.Reader.UnreadByte: at beginning of slice");
    prevRune_ = -1;
    i_--;
    return nullptr;
}

std::tuple<rune, int, error> Reader::ReadRune() {
    if (i_ >= int64(s_.len())) {
        prevRune_ = -1;
        return {0, 0, io::EOF_};
    }
    prevRune_ = isize(i_);
    if (byte c = s_[std::size_t(i_)]; c < utf8::RuneSelf) {
        i_++;
        return {rune(c), 1, nullptr};
    }
    auto [ch, size] = utf8::DecodeRune(s_.span().subspan(std::size_t(i_)));
    i_ += size;
    return {ch, size, nullptr};
}

error Reader::UnreadRune() {
    if (i_ <= 0) return errors::New("bytes.Reader.UnreadRune: at beginning of slice");
    if (prevRune_ < 0) return errors::New("bytes.Reader.UnreadRune: previous operation was not ReadRune");
    i_ = int64(prevRune_);
    prevRune_ = -1;
    return nullptr;
}

std::pair<int64, error> Reader::Seek(int64 offset, int whence) {
    prevRune_ = -1;
    int64 abs = 0;
    switch (whence) {
    case io::SeekStart:
        abs = offset;
        break;
    case io::SeekCurrent:
        abs = i_ + offset;
        break;
    case io::SeekEnd:
        abs = int64(s_.len()) + offset;
        break;
    default:
        return {0, errors::New("bytes.Reader.Seek: invalid whence")};
    }
    if (abs < 0) return {0, errors::New("bytes.Reader.Seek: negative position")};
    i_ = abs;
    return {abs, nullptr};
}

std::pair<int64, error> Reader::WriteTo(io::Writer& w) {
    prevRune_ = -1;
    if (i_ >= int64(s_.len())) return {0, nullptr};
    auto b = s_.span().subspan(std::size_t(i_));
    auto [m, err] = w.Write(b);
    if (m > isize(b.size())) panic("bytes.Reader.WriteTo: invalid Write count");
    i_ += m;
    if (std::size_t(m) != b.size() && !err) err = io::ErrShortWrite;
    return {int64(m), err};
}

}  // namespace gopp::bytes
