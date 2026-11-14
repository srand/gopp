// Ported from Go 1.27.3's encoding/binary/binary_test.go and varint_test.go,
// with Go's test names. Go's encoders and decoders take any value; here
// they're templates. What Go reports as an error at run time (data that
// isn't fixed-size, unexported fields, non-pointer destinations) doesn't
// compile here, so TestWriteT, TestUnexportedRead, TestReadErrorMsg,
// TestReadInvalidDestination and TestNoFixedSize check that instead. Go's
// allocation and size cache tests are left out: there's no cache.
#include <gopp/bytes.hpp>
#include <gopp/encoding/binary.hpp>
#include <gopp/strings.hpp>

#include <gtest/gtest.h>

#include <bit>
#include <complex>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

using namespace gopp;

namespace {

struct Struct {
    int8 Int8;
    int16 Int16;
    int32 Int32;
    int64 Int64;
    uint8 Uint8;
    uint16 Uint16;
    uint32 Uint32;
    uint64 Uint64;
    float Float32;
    double Float64;
    std::complex<float> Complex64;
    std::complex<double> Complex128;
    uint8 Array[4];
    bool Bool;
    std::array<bool, 4> BoolArray;

    bool operator==(const Struct& o) const {
        return Int8 == o.Int8 && Int16 == o.Int16 && Int32 == o.Int32 && Int64 == o.Int64 && Uint8 == o.Uint8 &&
               Uint16 == o.Uint16 && Uint32 == o.Uint32 && Uint64 == o.Uint64 && Float32 == o.Float32 &&
               Float64 == o.Float64 && Complex64 == o.Complex64 && Complex128 == o.Complex128 &&
               std::memcmp(Array, o.Array, 4) == 0 && Bool == o.Bool && BoolArray == o.BoolArray;
    }
};

const Struct s = {
    0x01,
    0x0203,
    0x04050607,
    0x08090a0b0c0d0e0f,
    0x10,
    0x1112,
    0x13141516,
    0x1718191a1b1c1d1e,
    std::bit_cast<float>(uint32(0x1f202122)),
    std::bit_cast<double>(uint64(0x232425262728292a)),
    {std::bit_cast<float>(uint32(0x2b2c2d2e)), std::bit_cast<float>(uint32(0x2f303132))},
    {std::bit_cast<double>(uint64(0x333435363738393a)), std::bit_cast<double>(uint64(0x3b3c3d3e3f404142))},
    {0x43, 0x44, 0x45, 0x46},
    true,
    {true, false, true, false},
};

const std::vector<byte> big = {
    1,
    2, 3,
    4, 5, 6, 7,
    8, 9, 10, 11, 12, 13, 14, 15,
    16,
    17, 18,
    19, 20, 21, 22,
    23, 24, 25, 26, 27, 28, 29, 30,

    31, 32, 33, 34,
    35, 36, 37, 38, 39, 40, 41, 42,
    43, 44, 45, 46, 47, 48, 49, 50,
    51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 65, 66,

    67, 68, 69, 70,

    1,
    1, 0, 1, 0,
};

const std::vector<byte> little = {
    1,
    3, 2,
    7, 6, 5, 4,
    15, 14, 13, 12, 11, 10, 9, 8,
    16,
    18, 17,
    22, 21, 20, 19,
    30, 29, 28, 27, 26, 25, 24, 23,

    34, 33, 32, 31,
    42, 41, 40, 39, 38, 37, 36, 35,
    46, 45, 44, 43, 50, 49, 48, 47,
    58, 57, 56, 55, 54, 53, 52, 51, 66, 65, 64, 63, 62, 61, 60, 59,

    67, 68, 69, 70,

    1,
    1, 0, 1, 0,
};

const std::vector<byte> src = {1, 2, 3, 4, 5, 6, 7, 8};
const std::vector<int32> res = {0x01020304, 0x05060708};

std::vector<byte> vec(const slice<byte>& b) { return std::vector<byte>(b.begin(), b.end()); }

// Go's encoders: Write, Encode and Append.
enum class enc { Write, Encode, Append };
constexpr enc encoders[] = {enc::Write, enc::Encode, enc::Append};
const char* name(enc e) { return e == enc::Write ? "Write" : e == enc::Encode ? "Encode" : "Append"; }

template <typename T>
std::pair<std::vector<byte>, error> encode(enc e, const binary::ByteOrder& order, const T& data) {
    switch (e) {
        case enc::Write: {
            bytes::Buffer buf;
            auto err = binary::Write(buf, order, data);
            return {vec(buf.Bytes()), err};
        }
        case enc::Encode: {
            isize size = binary::Size(data);
            std::vector<byte> buf(std::size_t(size > 0 ? size : 0));
            auto [n, err] = binary::Encode(buf, order, data);
            if (!err && n != size) return {{}, errors::New("returned wrong size")};
            return {buf, err};
        }
        case enc::Append: {
            auto [b, err] = binary::Append(slice<byte>{}, order, data);
            return {vec(b), err};
        }
    }
    return {};
}

// Go's decoders: Read and Decode.
enum class dec { Read, Decode };
constexpr dec decoders[] = {dec::Read, dec::Decode};
const char* name(dec d) { return d == dec::Read ? "Read" : "Decode"; }

template <typename T>
error decode(dec d, const binary::ByteOrder& order, T& data, const std::vector<byte>& buf) {
    if (d == dec::Read) {
        auto r = bytes::NewReader(slice<byte>::of(std::span<const byte>(buf)));
        return binary::Read(*r, order, data);
    }
    auto [n, err] = binary::Decode(buf, order, data);
    if (!err && n != binary::Size(data)) return errors::New("returned wrong size");
    return err;
}

void testRead(const binary::ByteOrder& order, const std::vector<byte>& b, const Struct& s1) {
    for (dec d : decoders) {
        SCOPED_TRACE(name(d));
        Struct s2{};
        auto err = decode(d, order, s2, b);
        EXPECT_EQ(err, nullptr) << err;
        EXPECT_TRUE(s2 == s1);
    }
}

template <typename T>
void testWrite(const binary::ByteOrder& order, const std::vector<byte>& b, const T& s1) {
    for (enc e : encoders) {
        SCOPED_TRACE(name(e));
        auto [buf, err] = encode(e, order, s1);
        EXPECT_EQ(err, nullptr) << err;
        EXPECT_EQ(buf, b);
    }
}

}  // namespace

TEST(GoEncodingBinary, LittleEndianRead) { testRead(binary::LittleEndian, little, s); }
TEST(GoEncodingBinary, LittleEndianWrite) { testWrite(binary::LittleEndian, little, s); }
TEST(GoEncodingBinary, BigEndianRead) { testRead(binary::BigEndian, big, s); }
TEST(GoEncodingBinary, BigEndianWrite) { testWrite(binary::BigEndian, big, s); }

TEST(GoEncodingBinary, ReadSlice) {
    {
        std::vector<int32> sl(2);
        auto r = bytes::NewReader(slice<byte>::of(std::span<const byte>(src)));
        EXPECT_EQ(binary::Read(*r, binary::BigEndian, sl), nullptr);
        EXPECT_EQ(sl, res);
    }
    {
        auto sl = slice<int32>::make(2);
        auto [_, err] = binary::Decode(src, binary::BigEndian, sl);
        EXPECT_EQ(err, nullptr);
        EXPECT_EQ(std::vector<int32>(sl.begin(), sl.end()), res);
    }
    {
        int32 arr[2];
        std::span<int32> sp(arr);
        auto [_, err] = binary::Decode(src, binary::BigEndian, sp);
        EXPECT_EQ(err, nullptr);
        EXPECT_EQ(std::vector<int32>(arr, arr + 2), res);
    }
}

TEST(GoEncodingBinary, WriteSlice) {
    testWrite(binary::BigEndian, src, res);
    testWrite(binary::BigEndian, src, std::span<const int32>(res));
    testWrite(binary::BigEndian, src, slice<int32>{0x01020304, 0x05060708});
}

TEST(GoEncodingBinary, ReadBool) {
    for (dec d : decoders) {
        SCOPED_TRACE(name(d));
        bool r = true;
        EXPECT_EQ(decode(d, binary::BigEndian, r, {0}), nullptr);
        EXPECT_FALSE(r);
        r = false;
        EXPECT_EQ(decode(d, binary::BigEndian, r, {1}), nullptr);
        EXPECT_TRUE(r);
        r = false;
        EXPECT_EQ(decode(d, binary::BigEndian, r, {2}), nullptr);
        EXPECT_TRUE(r);
    }
}

TEST(GoEncodingBinary, ReadBoolSlice) {
    for (dec d : decoders) {
        SCOPED_TRACE(name(d));
        std::vector<bool> want = {false, true, true, true};
        std::array<bool, 4> got{};
        EXPECT_EQ(decode(d, binary::BigEndian, got, {0, 1, 2, 255}), nullptr);
        EXPECT_TRUE(std::equal(got.begin(), got.end(), want.begin()));
    }
}

namespace {

template <typename T>
void sliceRoundTrip(enc e, dec d) {
    SCOPED_TRACE(typeid(T).name());
    std::vector<T> source(100);
    for (std::size_t i = 0; i < source.size(); i++) source[i] = T(int64(i) * 0x07654321);
    auto [buf, err] = encode(e, binary::BigEndian, source);
    ASSERT_EQ(err, nullptr);
    std::vector<T> dst(100);
    ASSERT_EQ(decode(d, binary::BigEndian, dst, buf), nullptr);
    EXPECT_EQ(source, dst);
}

}  // namespace

TEST(GoEncodingBinary, SliceRoundTrip) {
    for (enc e : encoders) {
        for (dec d : decoders) {
            SCOPED_TRACE(std::string(name(e)) + "," + name(d));
            sliceRoundTrip<int8>(e, d);
            sliceRoundTrip<int16>(e, d);
            sliceRoundTrip<int32>(e, d);
            sliceRoundTrip<int64>(e, d);
            sliceRoundTrip<uint8>(e, d);
            sliceRoundTrip<uint16>(e, d);
            sliceRoundTrip<uint32>(e, d);
            sliceRoundTrip<uint64>(e, d);
        }
    }
}

namespace {

struct T {
    int* Ptr;
    std::string Str;
    std::vector<int> Slice;
};

struct Unsized {
    int32 A;
    T B;
};

// Go's TestNoFixedSize's Person: in C++ it's fixed-size, int included.
struct Person {
    int Age;
    double Weight;
    double Height;
};

class Unexported {
    int32 a = 0;

public:
    int32 get() const { return a; }
};

template <typename T>
concept writable = requires(io::Writer& w, const T& v) { binary::Write(w, binary::LittleEndian, v); };
template <typename T>
concept readable = requires(io::Reader& r, T& v) { binary::Read(r, binary::LittleEndian, v); };

}  // namespace

// Go's TestWriteT, TestUnexportedRead, TestReadErrorMsg,
// TestReadInvalidDestination and TestNoFixedSize: what isn't fixed-size
// data is rejected at compile time.
TEST(GoEncodingBinary, WriteT) {
    static_assert(!writable<T>);
    static_assert(!writable<int*>);
    static_assert(!writable<std::string>);
    static_assert(!writable<std::vector<int*>>);
    static_assert(!writable<Unsized>);
    static_assert(!writable<Unexported>);
    static_assert(!readable<Unexported>);
    static_assert(!readable<const int32>);
    static_assert(writable<Person>);
    static_assert(readable<std::vector<Person>>);
    EXPECT_EQ(binary::Size(T{}), -1);
    EXPECT_EQ(binary::Size(Person{}), isize(sizeof(int) + 16));
}

namespace {

// Go's BlankFieldsProbe; C++ has no blank fields.
struct BlankFieldsProbe {
    uint32 A;
    int32 P0;
    double B;
    int16 P1[4];
    byte C;
    byte P2[7];
    struct {
        float F[8];
    } P3;
};

}  // namespace

TEST(GoEncodingBinary, BlankFields) {
    for (enc e : encoders) {
        SCOPED_TRACE(name(e));
        BlankFieldsProbe b1{.A = 1234567890, .P0 = 0, .B = 2.718281828, .P1 = {}, .C = 42, .P2 = {}, .P3 = {}};
        auto [buf, err] = encode(e, binary::LittleEndian, b1);
        EXPECT_EQ(err, nullptr);
        EXPECT_EQ(buf.size(), 4u + 4 + 8 + 8 + 1 + 7 + 32);
        BlankFieldsProbe b2{};
        b2.P0 = 9;
        auto r = bytes::NewReader(slice<byte>::of(std::span<const byte>(buf)));
        EXPECT_EQ(binary::Read(*r, binary::LittleEndian, b2), nullptr);
        EXPECT_EQ(b1.A, b2.A);
        EXPECT_EQ(b1.B, b2.B);
        EXPECT_EQ(b1.C, b2.C);
        EXPECT_EQ(b2.P0, 0);
    }
}

TEST(GoEncodingBinary, SizeInvalid) {
    int i = 0;
    EXPECT_EQ(binary::Size(&i), -1);
    EXPECT_EQ(binary::Size(std::vector<int*>{}), -1);
    EXPECT_EQ(binary::Size(std::string()), -1);
    EXPECT_EQ(binary::Size(std::vector<int>{}), 0);
    EXPECT_EQ(binary::Size(std::vector<Struct>(2)), 2 * binary::Size(s));
    EXPECT_EQ(binary::Size(std::array<Struct, 1>{}), binary::Size(s));
    EXPECT_EQ(binary::Size(s), isize(big.size()));
}

TEST(GoEncodingBinary, ReadTruncated) {
    const std::string data = "0123456789abcdef";

    std::vector<int32> b1(4);
    struct {
        byte A, B, C, D;
        int32 E;
        double F;
    } b2;

    for (std::size_t i = 0; i <= data.size(); i++) {
        error errWant;
        if (i == 0) {
            errWant = io::EOF_;
        } else if (i == data.size()) {
            errWant = nullptr;
        } else {
            errWant = io::ErrUnexpectedEOF;
        }
        auto r1 = strings::NewReader(string(data.substr(0, i)));
        EXPECT_EQ(binary::Read(*r1, binary::LittleEndian, b1), errWant) << "Read(" << i << ") with slice";
        auto r2 = strings::NewReader(string(data.substr(0, i)));
        EXPECT_EQ(binary::Read(*r2, binary::LittleEndian, b2), errWant) << "Read(" << i << ") with struct";
    }
}

TEST(GoEncodingBinary, ByteOrder) {
    auto check = [](const auto& order) {
        auto buf = slice<byte>::make(8);
        constexpr std::size_t offset = 3;
        for (uint64 value : {uint64(0x0000000000000000), uint64(0x0123456789abcdef), uint64(0xfedcba9876543210),
                             uint64(0xffffffffffffffff), uint64(0xaaaaaaaaaaaaaaaa),
                             std::bit_cast<uint64>(3.141592653589793), std::bit_cast<uint64>(2.718281828459045)}) {
            auto want16 = uint16(value);
            order.PutUint16(buf.Sub(0, 2), want16);
            EXPECT_EQ(order.Uint16(buf.Sub(0, 2)), want16);
            buf = order.AppendUint16(buf.Sub(0, offset), want16);
            EXPECT_EQ(order.Uint16(buf.Sub(offset)), want16);
            EXPECT_EQ(buf.len(), offset + 2);

            auto want32 = uint32(value);
            order.PutUint32(buf.Sub(0, 4), want32);
            EXPECT_EQ(order.Uint32(buf.Sub(0, 4)), want32);
            buf = order.AppendUint32(buf.Sub(0, offset), want32);
            EXPECT_EQ(order.Uint32(buf.Sub(offset)), want32);
            EXPECT_EQ(buf.len(), offset + 4);

            auto want64 = value;
            order.PutUint64(buf.Sub(0, 8), want64);
            EXPECT_EQ(order.Uint64(buf.Sub(0, 8)), want64);
            buf = order.AppendUint64(buf.Sub(0, offset), want64);
            EXPECT_EQ(order.Uint64(buf.Sub(offset)), want64);
            EXPECT_EQ(buf.len(), offset + 8);
        }
    };
    check(binary::LittleEndian);
    check(binary::BigEndian);
    EXPECT_EQ(binary::LittleEndian.String(), "LittleEndian");
    EXPECT_EQ(binary::BigEndian.String(), "BigEndian");
}

// Go recovers the panics; go++ can't: death tests.
TEST(GoEncodingBinaryDeathTest, EarlyBoundsChecks) {
    byte b[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    EXPECT_DEATH((void)binary::LittleEndian.Uint64(std::span<const byte>(b, 4)), "index out of range");
    EXPECT_DEATH(binary::LittleEndian.PutUint64(std::span<byte>(b, 4), 0x0102030405060708), "index out of range");
}

TEST(GoEncodingBinary, NativeEndian) {
    const uint32 val = 0x12345678;
    uint32 i = val;
    byte b[4];
    std::memcpy(b, &i, 4);
    EXPECT_EQ(binary::NativeEndian.Uint32(b), val);
}

// ---------------------------------------------------------------------------
// varint_test.go

namespace {

void testConstant(unsigned w, int max) {
    byte buf[binary::MaxVarintLen64];
    int n = binary::PutUvarint(buf, w == 64 ? ~uint64(0) : (uint64(1) << w) - 1);
    EXPECT_EQ(n, max) << "MaxVarintLen" << w;
}

void testVarint(int64 x) {
    SCOPED_TRACE(x);
    auto buf = slice<byte>::make(binary::MaxVarintLen64);
    int n = binary::PutVarint(buf, x);
    auto [y, m] = binary::Varint(buf.Sub(0, std::size_t(n)));
    EXPECT_EQ(x, y);
    EXPECT_EQ(n, m);

    auto buf2 = binary::AppendVarint(toBytes("prefix"), x);
    EXPECT_EQ(asStringView(buf2), "prefix" + std::string(asStringView(buf.Sub(0, std::size_t(n)))));

    auto [y2, err] = binary::ReadVarint(*bytes::NewReader(buf));
    EXPECT_EQ(err, nullptr);
    EXPECT_EQ(x, y2);
}

void testUvarint(uint64 x) {
    SCOPED_TRACE(x);
    auto buf = slice<byte>::make(binary::MaxVarintLen64);
    int n = binary::PutUvarint(buf, x);
    auto [y, m] = binary::Uvarint(buf.Sub(0, std::size_t(n)));
    EXPECT_EQ(x, y);
    EXPECT_EQ(n, m);

    auto buf2 = binary::AppendUvarint(toBytes("prefix"), x);
    EXPECT_EQ(asStringView(buf2), "prefix" + std::string(asStringView(buf.Sub(0, std::size_t(n)))));

    auto [y2, err] = binary::ReadUvarint(*bytes::NewReader(buf));
    EXPECT_EQ(err, nullptr);
    EXPECT_EQ(x, y2);
}

constexpr int64 tests[] = {
    std::numeric_limits<int64>::min(),
    std::numeric_limits<int64>::min() + 1,
    -1,
    0,
    1,
    2,
    10,
    20,
    63,
    64,
    65,
    127,
    128,
    129,
    255,
    256,
    257,
    std::numeric_limits<int64>::max(),
};

}  // namespace

TEST(GoEncodingBinary, Constants) {
    testConstant(16, binary::MaxVarintLen16);
    testConstant(32, binary::MaxVarintLen32);
    testConstant(64, binary::MaxVarintLen64);
}

TEST(GoEncodingBinary, Varint) {
    for (int64 x : tests) {
        testVarint(x);
        testVarint(int64(0 - uint64(x)));  // Go's -x wraps
    }
    for (int64 x = 0x7; x != 0; x = int64(uint64(x) << 1)) {
        testVarint(x);
        testVarint(int64(0 - uint64(x)));
    }
}

TEST(GoEncodingBinary, Uvarint) {
    for (int64 x : tests) testUvarint(uint64(x));
    for (uint64 x = 0x7; x != 0; x <<= 1) testUvarint(x);
}

TEST(GoEncodingBinary, BufferTooSmall) {
    slice<byte> full = {0x80, 0x80, 0x80, 0x80};
    for (std::size_t i = 0; i <= full.len(); i++) {
        auto buf = full.Sub(0, i);
        auto [x, n] = binary::Uvarint(buf);
        EXPECT_EQ(x, 0u);
        EXPECT_EQ(n, 0);

        auto [x2, err] = binary::ReadUvarint(*bytes::NewReader(buf));
        EXPECT_EQ(x2, 0u);
        EXPECT_EQ(err, i > 0 ? io::ErrUnexpectedEOF : io::EOF_);
    }
}

TEST(GoEncodingBinary, BufferTooBigWithOverflow) {
    struct {
        std::vector<byte> in;
        const char* name;
        int wantN;
        uint64 wantValue;
    } tests[] = {
        {[] {
             std::vector<byte> b(1000, 0xff);
             b[999] = 0;
             return b;
         }(),
         "invalid: 1000 bytes", -11, 0},
        {{0xd7, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01}, "valid: math.MaxUint64-40", 10,
         std::numeric_limits<uint64>::max() - 40},
        {{0xd7, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01},
         "invalid: with more than MaxVarintLen64 bytes", -11, 0},
        {{0xd7, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f}, "invalid: 10th byte", -10, 0},
    };
    for (const auto& tt : tests) {
        SCOPED_TRACE(tt.name);
        auto [value, n] = binary::Uvarint(tt.in);
        EXPECT_EQ(n, tt.wantN);
        EXPECT_EQ(value, tt.wantValue);
    }
}

namespace {

void testOverflow(slice<byte> buf, uint64 x0, int n0) {
    auto [x, n] = binary::Uvarint(buf);
    EXPECT_EQ(x, 0u);
    EXPECT_EQ(n, n0);

    auto r = bytes::NewReader(buf);
    isize len = r->Len();
    auto [x2, err] = binary::ReadUvarint(*r);
    EXPECT_EQ(x2, x0);
    EXPECT_NE(err, nullptr);
    if (err) {
        EXPECT_EQ(err->Error(), "binary: varint overflows a 64-bit integer");
    }
    EXPECT_LE(len - r->Len(), binary::MaxVarintLen64);
}

}  // namespace

TEST(GoEncodingBinary, Overflow) {
    testOverflow({0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x2}, 0, -10);
    testOverflow({0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x1, 0, 0}, 0, -11);
    testOverflow({0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}, ~uint64(0), -11);
}

TEST(GoEncodingBinary, NonCanonicalZero) {
    slice<byte> buf = {0x80, 0x80, 0x80, 0};
    auto [x, n] = binary::Uvarint(buf);
    EXPECT_EQ(x, 0u);
    EXPECT_EQ(n, 4);
}
