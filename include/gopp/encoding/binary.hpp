// The encoding/binary package: numbers as byte sequences, and varints,
// after Go 1.27.3.
//
//     binary::BigEndian.PutUint32(buf, 0xcafebabe);
//     uint16 port = binary::LittleEndian.Uint16(packet);
//
//     struct Header { uint32 Magic; uint16 Version; std::array<byte, 2> Flags; };
//     Header h;
//     auto err = binary::Read(*file, binary::LittleEndian, h);   // fields by reflection
//
//     auto n = binary::PutUvarint(buf, 300);                     // LEB128, 2 bytes
//
// Read, Write, Encode, Decode, Append and Size take fixed-size data: bool,
// arithmetic types, std::complex, arrays and std::arrays of fixed-size
// data, structs whose fields all are (in declaration order, by C++26
// reflection), and, at the top level, spans, vectors and slices of them.
// Other types don't compile. Unlike Go, int and uint have a fixed size in
// C++, so they're accepted; and C++ has no blank fields.
#pragma once

// The module interfaces (modules/) import these instead.
#ifndef GOPP_MODULE
#include <gopp/errors.hpp>
#include <gopp/io.hpp>
#include <gopp/slice.hpp>
#include <gopp/string.hpp>

#include <array>
#include <bit>
#include <complex>
#include <cstring>
#include <meta>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>
#endif

namespace gopp::binary {

// binary.ByteOrder: how to read and write unsigned integers.
struct ByteOrder {
    virtual uint16 Uint16(std::span<const byte> b) const = 0;
    virtual uint32 Uint32(std::span<const byte> b) const = 0;
    virtual uint64 Uint64(std::span<const byte> b) const = 0;
    virtual void PutUint16(std::span<byte> b, uint16 v) const = 0;
    virtual void PutUint32(std::span<byte> b, uint32 v) const = 0;
    virtual void PutUint64(std::span<byte> b, uint64 v) const = 0;
    virtual string String() const = 0;

protected:
    ~ByteOrder() = default;
};

// binary.AppendByteOrder: how to append unsigned integers to a slice.
struct AppendByteOrder {
    virtual slice<byte> AppendUint16(slice<byte> b, uint16 v) const = 0;
    virtual slice<byte> AppendUint32(slice<byte> b, uint32 v) const = 0;
    virtual slice<byte> AppendUint64(slice<byte> b, uint64 v) const = 0;
    virtual string String() const = 0;

protected:
    ~AppendByteOrder() = default;
};

namespace detail {

template <bool big>
struct order final : ByteOrder, AppendByteOrder {
    // Go panics on short buffers, with an index error; so does go++.
    static void need(std::size_t have, std::size_t want) {
        if (have < want) panic("index out of range");
    }
    template <typename U>
    static U get(std::span<const byte> b) {
        need(b.size(), sizeof(U));
        U v = 0;
        for (std::size_t i = 0; i < sizeof(U); i++) {
            std::size_t shift = big ? 8 * (sizeof(U) - 1 - i) : 8 * i;
            v |= U(U(b[i]) << shift);
        }
        return v;
    }
    template <typename U>
    static void put(std::span<byte> b, U v) {
        need(b.size(), sizeof(U));
        for (std::size_t i = 0; i < sizeof(U); i++) {
            std::size_t shift = big ? 8 * (sizeof(U) - 1 - i) : 8 * i;
            b[i] = byte(v >> shift);
        }
    }
    template <typename U>
    static slice<byte> app(slice<byte> b, U v) {
        byte tmp[sizeof(U)];
        put<U>(tmp, v);
        return appendSpan(std::move(b), std::span<const byte>(tmp, sizeof(U)));
    }

    uint16 Uint16(std::span<const byte> b) const override { return get<uint16>(b); }
    uint32 Uint32(std::span<const byte> b) const override { return get<uint32>(b); }
    uint64 Uint64(std::span<const byte> b) const override { return get<uint64>(b); }
    void PutUint16(std::span<byte> b, uint16 v) const override { put(b, v); }
    void PutUint32(std::span<byte> b, uint32 v) const override { put(b, v); }
    void PutUint64(std::span<byte> b, uint64 v) const override { put(b, v); }
    slice<byte> AppendUint16(slice<byte> b, uint16 v) const override { return app(std::move(b), v); }
    slice<byte> AppendUint32(slice<byte> b, uint32 v) const override { return app(std::move(b), v); }
    slice<byte> AppendUint64(slice<byte> b, uint64 v) const override { return app(std::move(b), v); }
    string String() const override { return string::fromStatic(big ? "BigEndian" : "LittleEndian"); }
};

}  // namespace detail

// binary.LittleEndian and binary.BigEndian.
inline const detail::order<false> LittleEndian{};
inline const detail::order<true> BigEndian{};
// binary.NativeEndian: the byte order of the machine.
inline const auto& NativeEndian = [] -> const auto& {
    if constexpr (std::endian::native == std::endian::big) {
        return BigEndian;
    } else {
        return LittleEndian;
    }
}();

// ---------------------------------------------------------------------------
// Fixed-size data

namespace detail {

template <typename T>
struct isStdArray : std::false_type {};
template <typename T, std::size_t N>
struct isStdArray<std::array<T, N>> : std::true_type {};
template <typename T>
struct isComplex : std::false_type {};
template <typename T>
struct isComplex<std::complex<T>> : std::true_type {};

template <typename T>
consteval auto fieldsOf() {
    return std::define_static_array(std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::unprivileged()));
}

// The encoded size of a T, or -1 if T isn't fixed-size data.
template <typename T>
consteval isize sizeOf() {
    if constexpr (std::is_same_v<T, bool>) {
        return 1;
    } else if constexpr (std::is_arithmetic_v<T>) {
        return isize(sizeof(T));
    } else if constexpr (isComplex<T>::value) {
        return 2 * sizeOf<typename T::value_type>();
    } else if constexpr (std::is_array_v<T>) {
        constexpr isize e = sizeOf<std::remove_extent_t<T>>();
        return e < 0 ? -1 : e * isize(std::extent_v<T>);
    } else if constexpr (isStdArray<T>::value) {
        constexpr isize e = sizeOf<typename T::value_type>();
        return e < 0 ? -1 : e * isize(std::tuple_size_v<T>);
    } else if constexpr (std::is_class_v<T> && std::is_aggregate_v<T>) {
        isize sum = 0;
        template for (constexpr std::meta::info m : fieldsOf<T>()) {
            constexpr isize s = sizeOf<typename[:std::meta::type_of(m):]>();
            if (s < 0) return -1;
            sum += s;
        }
        return sum;
    } else {
        return -1;
    }
}

template <typename T>
inline constexpr bool fixedSize = sizeOf<T>() >= 0;

struct encoder {
    const ByteOrder& order;
    byte* p;

    template <typename T>
    void value(const T& v) {
        if constexpr (std::is_same_v<T, bool>) {
            *p++ = v ? 1 : 0;
        } else if constexpr (std::is_integral_v<T>) {
            using U = std::make_unsigned_t<T>;
            U u = U(v);
            if constexpr (sizeof(T) == 1) {
                *p = byte(u);
            } else if constexpr (sizeof(T) == 2) {
                order.PutUint16({p, 2}, uint16(u));
            } else if constexpr (sizeof(T) == 4) {
                order.PutUint32({p, 4}, uint32(u));
            } else {
                order.PutUint64({p, 8}, uint64(u));
            }
            p += sizeof(T);
        } else if constexpr (std::is_same_v<T, float>) {
            value(std::bit_cast<uint32>(v));
        } else if constexpr (std::is_same_v<T, double>) {
            value(std::bit_cast<uint64>(v));
        } else if constexpr (isComplex<T>::value) {
            value(v.real());
            value(v.imag());
        } else if constexpr (std::is_array_v<T> || isStdArray<T>::value) {
            for (const auto& e : v) value(e);
        } else {
            template for (constexpr std::meta::info m : fieldsOf<T>()) value(v.[:m:]);
        }
    }
};

struct decoder {
    const ByteOrder& order;
    const byte* p;

    template <typename T>
    void value(T& v) {
        if constexpr (std::is_same_v<T, bool>) {
            v = *p++ != 0;
        } else if constexpr (std::is_integral_v<T>) {
            using U = std::make_unsigned_t<T>;
            U u;
            if constexpr (sizeof(T) == 1) {
                u = U(*p);
            } else if constexpr (sizeof(T) == 2) {
                u = U(order.Uint16({p, 2}));
            } else if constexpr (sizeof(T) == 4) {
                u = U(order.Uint32({p, 4}));
            } else {
                u = U(order.Uint64({p, 8}));
            }
            v = T(u);
            p += sizeof(T);
        } else if constexpr (std::is_same_v<T, float>) {
            uint32 u;
            value(u);
            v = std::bit_cast<float>(u);
        } else if constexpr (std::is_same_v<T, double>) {
            uint64 u;
            value(u);
            v = std::bit_cast<double>(u);
        } else if constexpr (isComplex<T>::value) {
            typename T::value_type re, im;
            value(re);
            value(im);
            v = T(re, im);
        } else if constexpr (std::is_array_v<T> || isStdArray<T>::value) {
            for (auto& e : v) value(e);
        } else {
            template for (constexpr std::meta::info m : fieldsOf<T>()) value(v.[:m:]);
        }
    }
};

// The elements of the top-level sequences Go accepts: spans, vectors and
// slices.
template <typename T>
struct sequence : std::false_type {};
template <typename T>
struct sequence<std::vector<T>> : std::true_type {
    using elem = T;
};
template <typename T>
struct sequence<slice<T>> : std::true_type {
    using elem = T;
};
template <typename T, std::size_t E>
struct sequence<std::span<T, E>> : std::true_type {
    using elem = std::remove_const_t<T>;
};

template <typename T>
std::span<const typename sequence<T>::elem> elements(const T& v) {
    return std::span<const typename sequence<T>::elem>(v.data(), v.size());
}

template <typename T>
std::span<typename sequence<T>::elem> elements(T& v) {
    return std::span<typename sequence<T>::elem>(v.data(), v.size());
}

template <typename T>
concept data = fixedSize<T> || (sequence<T>::value && fixedSize<typename sequence<T>::elem>);

template <data T>
std::size_t dataSize(const T& v) {
    if constexpr (sequence<T>::value) {
        return std::size_t(sizeOf<typename sequence<T>::elem>()) * v.size();
    } else {
        return std::size_t(sizeOf<T>());
    }
}

template <data T>
void encode(const ByteOrder& order, byte* p, const T& v) {
    encoder e{order, p};
    if constexpr (sequence<T>::value) {
        for (const auto& x : elements(v)) e.value(x);
    } else {
        e.value(v);
    }
}

template <typename T>
void decode(const ByteOrder& order, const byte* p, T& v) {
    decoder d{order, p};
    if constexpr (sequence<T>::value) {
        for (auto& x : elements(v)) d.value(x);
    } else {
        d.value(v);
    }
}

error errBufferTooSmall();

}  // namespace detail

// binary.Size: the number of bytes Write would write for v: its size, or
// the size of the elements times their number for a span, vector or slice.
template <typename T>
isize Size(const T& v) {
    if constexpr (detail::data<T>) {
        return isize(detail::dataSize(v));
    } else {
        return -1;
    }
}

// binary.Write: writes the binary representation of data to w, in order.
// Spans, vectors and slices are written element by element.
template <detail::data T>
error Write(io::Writer& w, const ByteOrder& order, const T& data) {
    std::vector<byte> bs(detail::dataSize(data));
    detail::encode(order, bs.data(), data);
    return w.Write(bs).second;
}

// binary.Read: reads binary data from r into data, in order: a value of
// fixed-size data, or the elements of a span, vector or slice (whose size
// is that of the data read). At most one Read call fills data; io::EOF_ if
// no bytes were read, io::ErrUnexpectedEOF if some, but not all, were.
template <typename T>
    requires detail::data<T> && (!std::is_const_v<T>)
error Read(io::Reader& r, const ByteOrder& order, T& data) {
    std::vector<byte> bs(detail::dataSize(data));
    if (auto [_, err] = io::ReadFull(r, bs); err) return err;
    detail::decode(order, bs.data(), data);
    return nullptr;
}

// binary.Encode: encodes data into buf, and returns the number of bytes
// written; an error if buf is too small.
template <detail::data T>
std::pair<isize, error> Encode(std::span<byte> buf, const ByteOrder& order, const T& data) {
    std::size_t n = detail::dataSize(data);
    if (buf.size() < n) return {0, detail::errBufferTooSmall()};
    detail::encode(order, buf.data(), data);
    return {isize(n), nullptr};
}

// binary.Decode: decodes data from buf, and returns the number of bytes
// read; an error if buf is too small.
template <typename T>
    requires detail::data<T> && (!std::is_const_v<T>)
std::pair<isize, error> Decode(std::span<const byte> buf, const ByteOrder& order, T& data) {
    std::size_t n = detail::dataSize(data);
    if (buf.size() < n) return {0, detail::errBufferTooSmall()};
    detail::decode(order, buf.data(), data);
    return {isize(n), nullptr};
}

// binary.Append: appends the binary representation of data to buf.
template <detail::data T>
std::pair<slice<byte>, error> Append(slice<byte> buf, const ByteOrder& order, const T& data) {
    std::size_t n = detail::dataSize(data);
    std::size_t start = buf.len();
    buf = appendSpan(std::move(buf), std::span<const byte>(std::vector<byte>(n)));
    detail::encode(order, buf.data() + start, data);
    return {std::move(buf), nullptr};
}

// ---------------------------------------------------------------------------
// Varints (varint.go): unsigned integers 7 bits a byte, least significant
// first, with the high bit set in all but the last byte (LEB128); signed
// ones zig-zag encoded, so that small negative numbers are short too.

// binary.MaxVarintLen16, MaxVarintLen32 and MaxVarintLen64: the longest
// varints of 16, 32 and 64-bit integers.
inline constexpr int MaxVarintLen16 = 3;
inline constexpr int MaxVarintLen32 = 5;
inline constexpr int MaxVarintLen64 = 10;

// binary.AppendUvarint
slice<byte> AppendUvarint(slice<byte> buf, uint64 x);
// binary.PutUvarint: encodes x into buf and returns the number of bytes
// written. Panics if buf is too small.
int PutUvarint(std::span<byte> buf, uint64 x);
// binary.Uvarint: decodes a uint64 from buf and returns it and the number
// of bytes read; 0 bytes if buf is too small, and -n if the value
// overflows 64 bits (n being the bytes read).
std::pair<uint64, int> Uvarint(std::span<const byte> buf);
// binary.AppendVarint
slice<byte> AppendVarint(slice<byte> buf, int64 x);
// binary.PutVarint
int PutVarint(std::span<byte> buf, int64 x);
// binary.Varint: as Uvarint, for a zig-zag encoded int64.
std::pair<int64, int> Varint(std::span<const byte> buf);

namespace detail {
error errOverflow();
}  // namespace detail

// binary.ReadUvarint: reads a uvarint from r, a reader with
// ReadByte() -> pair<byte, error> (Go's io.ByteReader: bufio::Reader,
// bytes::Reader, bytes::Buffer). io::EOF_ if no bytes were read,
// io::ErrUnexpectedEOF if the varint was cut short.
template <typename R>
std::pair<uint64, error> ReadUvarint(R& r) {
    uint64 x = 0;
    unsigned s = 0;
    for (int i = 0; i < MaxVarintLen64; i++) {
        auto [b, err] = r.ReadByte();
        if (err) {
            if (i > 0 && err == io::EOF_) err = io::ErrUnexpectedEOF;
            return {x, err};
        }
        if (b < 0x80) {
            if (i == MaxVarintLen64 - 1 && b > 1) return {x, detail::errOverflow()};
            return {x | uint64(b) << s, nullptr};
        }
        x |= uint64(b & 0x7f) << s;
        s += 7;
    }
    return {x, detail::errOverflow()};
}

// binary.ReadVarint: reads a zig-zag encoded varint from r.
template <typename R>
std::pair<int64, error> ReadVarint(R& r) {
    auto [ux, err] = ReadUvarint(r);  // ok to continue in presence of error
    int64 x = int64(ux >> 1);
    if (ux & 1) x = ~x;
    return {x, err};
}

}  // namespace gopp::binary
