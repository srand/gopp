// The bytes package: functions over byte slices, Buffer and Reader, ported
// from Go 1.27.3.
//
//     auto fields = bytes::Fields(line);               // slice<slice<byte>>
//     if (bytes::HasPrefix(b, as_bytes("#!"))) { ... }
//     bytes::Buffer buf;
//     buf.WriteString("hello, ");
//     buf.Write(name);
//     io::Copy(*w, buf);
//
// The functions that only read take std::span<const byte>, so they accept
// slices, arrays and vectors (as_bytes(s) gives one of a string). The ones
// that return parts of their input (Trim, Cut, Split, Fields, ...) take and
// return slice<byte>; the parts share the input's array, as Go's subslices
// do, and are nil where Go's are. Go's iterators (iter.Seq) are
// std::generators.
#pragma once

// The module interfaces (modules/) import these instead.
#ifndef GOPP_MODULE
#include <gopp/errors.hpp>
#include <gopp/io.hpp>
#include <gopp/ptr.hpp>
#include <gopp/slice.hpp>
#include <gopp/string.hpp>
#include <gopp/unicode.hpp>

#include <functional>
#include <generator>
#include <span>
#include <string_view>
#include <tuple>
#include <utility>
#endif

namespace gopp::bytes {

// bytes.Equal: whether a and b have the same bytes; nil equals empty.
bool Equal(std::span<const byte> a, std::span<const byte> b) noexcept;
// bytes.Compare: -1, 0 or +1 as a < b, a == b or a > b.
int Compare(std::span<const byte> a, std::span<const byte> b) noexcept;
bool HasPrefix(std::span<const byte> s, std::span<const byte> prefix) noexcept;
bool HasSuffix(std::span<const byte> s, std::span<const byte> suffix) noexcept;
bool Contains(std::span<const byte> b, std::span<const byte> subslice) noexcept;
// bytes.ContainsAny: whether any of the runes of chars is in b.
bool ContainsAny(std::span<const byte> b, std::string_view chars) noexcept;
bool ContainsRune(std::span<const byte> b, rune r) noexcept;
bool ContainsFunc(std::span<const byte> b, std::function_ref<bool(rune)> f);

// bytes.Index: the index of the first sep in s, or -1.
isize Index(std::span<const byte> s, std::span<const byte> sep) noexcept;
isize IndexByte(std::span<const byte> b, byte c) noexcept;
// bytes.IndexRune: the index of the first r in s, or -1. For RuneError,
// the first invalid UTF-8 sequence or U+FFFD.
isize IndexRune(std::span<const byte> s, rune r) noexcept;
// bytes.IndexAny: the index of the first of the runes of chars in s.
isize IndexAny(std::span<const byte> s, std::string_view chars) noexcept;
isize IndexFunc(std::span<const byte> s, std::function_ref<bool(rune)> f);
isize LastIndex(std::span<const byte> s, std::span<const byte> sep) noexcept;
isize LastIndexByte(std::span<const byte> s, byte c) noexcept;
isize LastIndexAny(std::span<const byte> s, std::string_view chars) noexcept;
isize LastIndexFunc(std::span<const byte> s, std::function_ref<bool(rune)> f);
// bytes.Count: the number of non-overlapping sep in s; for an empty sep,
// the number of runes plus one.
isize Count(std::span<const byte> s, std::span<const byte> sep) noexcept;
// bytes.EqualFold: whether s and t, as UTF-8, are equal under Unicode's
// simple case folding.
bool EqualFold(std::span<const byte> s, std::span<const byte> t) noexcept;

// bytes.Cut: the bytes before and after the first sep, and whether found;
// s, nil and false if not.
std::tuple<slice<byte>, slice<byte>, bool> Cut(const slice<byte>& s, std::span<const byte> sep);
// bytes.CutLast: as Cut, around the last sep.
std::tuple<slice<byte>, slice<byte>, bool> CutLast(const slice<byte>& s, std::span<const byte> sep);
// bytes.CutPrefix: s without prefix, and whether it had it.
std::pair<slice<byte>, bool> CutPrefix(const slice<byte>& s, std::span<const byte> prefix);
// bytes.CutSuffix: s without suffix, and whether it had it.
std::pair<slice<byte>, bool> CutSuffix(const slice<byte>& s, std::span<const byte> suffix);

// bytes.TrimSpace: s without leading and trailing white space
// (unicode::IsSpace); nil if that's all there is.
slice<byte> TrimSpace(const slice<byte>& s);
// bytes.Trim: s without the leading and trailing runes in cutset.
slice<byte> Trim(const slice<byte>& s, std::string_view cutset);
slice<byte> TrimLeft(const slice<byte>& s, std::string_view cutset);
slice<byte> TrimRight(const slice<byte>& s, std::string_view cutset);
slice<byte> TrimPrefix(const slice<byte>& s, std::span<const byte> prefix);
slice<byte> TrimSuffix(const slice<byte>& s, std::span<const byte> suffix);
// bytes.TrimFunc: s without the leading and trailing runes that satisfy f.
slice<byte> TrimFunc(const slice<byte>& s, std::function_ref<bool(rune)> f);
slice<byte> TrimLeftFunc(const slice<byte>& s, std::function_ref<bool(rune)> f);
slice<byte> TrimRightFunc(const slice<byte>& s, std::function_ref<bool(rune)> f);

// bytes.Split: the parts of s between the seps; for an empty sep, each
// UTF-8 sequence. Each part's capacity ends with it.
slice<slice<byte>> Split(const slice<byte>& s, std::span<const byte> sep);
// bytes.SplitN: as Split, but at most n parts, the last holding the rest
// (n < 0: all of them; n == 0: nil).
slice<slice<byte>> SplitN(const slice<byte>& s, std::span<const byte> sep, isize n);
// bytes.SplitAfter: as Split, keeping each sep at the end of its part.
slice<slice<byte>> SplitAfter(const slice<byte>& s, std::span<const byte> sep);
slice<slice<byte>> SplitAfterN(const slice<byte>& s, std::span<const byte> sep, isize n);
// bytes.Fields: the runs of non-space runes (unicode::IsSpace) of s.
slice<slice<byte>> Fields(const slice<byte>& s);
// bytes.FieldsFunc: the runs of runes of s that don't satisfy f.
slice<slice<byte>> FieldsFunc(const slice<byte>& s, std::function_ref<bool(rune)> f);

// bytes.Lines: the newline-terminated lines of s, each with its "\n"; the
// last may lack one.
std::generator<slice<byte>> Lines(slice<byte> s);
// bytes.SplitSeq: Split's parts, one at a time.
std::generator<slice<byte>> SplitSeq(slice<byte> s, std::span<const byte> sep);
// bytes.SplitAfterSeq: SplitAfter's parts, one at a time.
std::generator<slice<byte>> SplitAfterSeq(slice<byte> s, std::span<const byte> sep);
// bytes.FieldsSeq: Fields' fields, one at a time.
std::generator<slice<byte>> FieldsSeq(slice<byte> s);
// bytes.FieldsFuncSeq: FieldsFunc's fields, one at a time.
std::generator<slice<byte>> FieldsFuncSeq(slice<byte> s, std::function<bool(rune)> f);

// The functions below return new slices.

// bytes.ToLower: s with its runes mapped to lower case.
slice<byte> ToLower(std::span<const byte> s);
// bytes.ToUpper: s with its runes mapped to upper case.
slice<byte> ToUpper(std::span<const byte> s);
// bytes.ToTitle: s with its runes mapped to title case.
slice<byte> ToTitle(std::span<const byte> s);
// bytes.ToLowerSpecial: as ToLower, giving priority to c's mappings.
slice<byte> ToLowerSpecial(const unicode::SpecialCase& c, std::span<const byte> s);
slice<byte> ToUpperSpecial(const unicode::SpecialCase& c, std::span<const byte> s);
slice<byte> ToTitleSpecial(const unicode::SpecialCase& c, std::span<const byte> s);
// bytes.ToValidUTF8: s with each run of invalid UTF-8 replaced by
// replacement.
slice<byte> ToValidUTF8(std::span<const byte> s, std::span<const byte> replacement);
// bytes.Map: s with each rune mapped by mapping; a negative result drops
// the rune.
slice<byte> Map(std::function_ref<rune(rune)> mapping, std::span<const byte> s);
// bytes.Repeat: count copies of b. Panics if count is negative or the
// result would overflow.
slice<byte> Repeat(std::span<const byte> b, isize count);
// bytes.Replace: s with the first n old replaced by new_ (n < 0: all of
// them). An empty old matches at the start and after each UTF-8 sequence.
slice<byte> Replace(std::span<const byte> s, std::span<const byte> old, std::span<const byte> new_, isize n);
// bytes.ReplaceAll
slice<byte> ReplaceAll(std::span<const byte> s, std::span<const byte> old, std::span<const byte> new_);
// bytes.Join: the elements with sep between them.
slice<byte> Join(std::span<const slice<byte>> s, std::span<const byte> sep);
// bytes.Runes: the runes of s.
slice<rune> Runes(std::span<const byte> s);
// bytes.Clone: a copy of b; nil for nil.
slice<byte> Clone(const slice<byte>& b);

// bytes.MinRead: the smallest buffer Buffer.ReadFrom reads into.
inline constexpr isize MinRead = 512;

// bytes.ErrTooLarge: the panic when a Buffer can't grow.
extern const error ErrTooLarge;

// bytes.Buffer: a buffer of bytes to write to and read from. The zero
// Buffer is empty and ready to use.
class Buffer : public io::Reader, public io::Writer, public io::ReaderFrom, public io::WriterTo {
public:
    Buffer() = default;
    // A Buffer that starts with buf's contents (bytes.NewBuffer).
    explicit Buffer(slice<byte> buf) noexcept : buf_(std::move(buf)) {}

    // The unread bytes, valid until the next change to the buffer; they
    // share its array.
    slice<byte> Bytes() const { return buf_.Sub(off_); }
    // The empty slice at the end of the buffer's capacity, to append to and
    // then Write.
    slice<byte> AvailableBuffer() const { return buf_.Sub(buf_.len()); }
    // The unread bytes as a string.
    string String() const { return gopp::String(buf_.span().subspan(off_)); }
    // The next n unread bytes without reading them; fewer and io::EOF_ if
    // there aren't n.
    std::pair<slice<byte>, error> Peek(isize n) const;
    // The number of unread bytes.
    isize Len() const noexcept { return isize(buf_.len() - off_); }
    // The capacity of the buffer's array.
    isize Cap() const noexcept { return isize(buf_.cap()); }
    // The unused bytes of the array.
    isize Available() const noexcept { return isize(buf_.cap() - buf_.len()); }
    // Keeps the first n unread bytes. Panics if n is out of range.
    void Truncate(isize n);
    // Empties the buffer, keeping its array.
    void Reset() noexcept;
    // Makes room for n more bytes. Panics if n is negative, or with
    // ErrTooLarge.
    void Grow(isize n);

    std::pair<isize, error> Write(std::span<const byte> p) override;
    std::pair<isize, error> WriteString(std::string_view s);
    error WriteByte(byte c);
    // Appends the UTF-8 encoding of r; returns its length.
    std::pair<isize, error> WriteRune(rune r);
    // Reads from r until io::EOF_ (which isn't returned) or an error.
    std::pair<int64, error> ReadFrom(io::Reader& r) override;
    // Writes the unread bytes to w until they're gone or an error.
    std::pair<int64, error> WriteTo(io::Writer& w) override;

    std::pair<isize, error> Read(std::span<byte> p) override;
    // The next n unread bytes (or all of them), read; they share the
    // buffer's array until the next change.
    slice<byte> Next(isize n);
    std::pair<byte, error> ReadByte();
    // The next rune, its size and an error; io::EOF_ if there's nothing to
    // read.
    std::tuple<rune, int, error> ReadRune();
    // Unreads the last rune of ReadRune.
    error UnreadRune();
    // Unreads the last byte of a successful read.
    error UnreadByte();
    // The bytes up to and including delim; with io::EOF_ the rest, if there
    // is no delim.
    std::pair<slice<byte>, error> ReadBytes(byte delim);
    std::pair<string, error> ReadString(byte delim);

private:
    // The last read operation, so that Unread* can work correctly.
    enum readOp : int8 {
        opRead = -1,      // Any other read operation.
        opInvalid = 0,    // Non-read operation.
        opReadRune1 = 1,  // Read rune of size 1.
        opReadRune2 = 2,  // Read rune of size 2.
        opReadRune3 = 3,  // Read rune of size 3.
        opReadRune4 = 4,  // Read rune of size 4.
    };

    bool empty() const noexcept { return buf_.len() <= off_; }
    std::pair<std::size_t, bool> tryGrowByReslice(std::size_t n);
    std::size_t grow(std::size_t n);
    std::pair<slice<byte>, error> readSlice(byte delim);

    slice<byte> buf_;  // contents are the bytes buf_[off_ : len(buf_)]
    std::size_t off_ = 0;  // read at &buf_[off_], write at &buf_[len(buf_)]
    readOp lastRead_ = opInvalid;
};

// bytes.NewBuffer: a Buffer that starts with buf's contents and array.
inline ptr<Buffer> NewBuffer(slice<byte> buf) { return make_ptr<Buffer>(std::move(buf)); }
// bytes.NewBufferString: a Buffer that starts with a copy of s.
inline ptr<Buffer> NewBufferString(std::string_view s) { return make_ptr<Buffer>(to_bytes(s)); }

// bytes.Reader: reads from a byte slice.
class Reader : public io::Reader, public io::Seeker, public io::ReaderAt, public io::WriterTo {
public:
    // The zero Reader reads an empty slice.
    Reader() = default;
    explicit Reader(slice<byte> b) noexcept : s_(std::move(b)) {}

    // The number of unread bytes.
    isize Len() const noexcept { return i_ >= int64(s_.len()) ? 0 : isize(int64(s_.len()) - i_); }
    // The length of the whole slice.
    int64 Size() const noexcept { return int64(s_.len()); }

    std::pair<isize, error> Read(std::span<byte> b) override;
    std::pair<isize, error> ReadAt(std::span<byte> b, int64 off) override;
    std::pair<byte, error> ReadByte();
    error UnreadByte();
    std::tuple<rune, int, error> ReadRune();
    error UnreadRune();
    // io::Seeker. The offset may go past the end, where Read returns
    // io::EOF_.
    std::pair<int64, error> Seek(int64 offset, int whence) override;
    std::pair<int64, error> WriteTo(io::Writer& w) override;
    // Reads from b from the start.
    void Reset(slice<byte> b) noexcept {
        s_ = std::move(b);
        i_ = 0;
        prevRune_ = -1;
    }

private:
    slice<byte> s_;
    int64 i_ = 0;        // current reading index
    isize prevRune_ = -1;  // index of previous rune; or < 0
};

// bytes.NewReader
inline ptr<Reader> NewReader(slice<byte> b) { return make_ptr<Reader>(std::move(b)); }

}  // namespace gopp::bytes
