// Ported from Go 1.27.3's encoding/base64/base64_test.go, with Go's test
// names. Left out: the benchmarks.
#include <gopp/chan.hpp>
#include <gopp/encoding/base64.hpp>
#include <gopp/io.hpp>
#include <gopp/runtime.hpp>
#include <gopp/strings.hpp>
#include <gopp/time.hpp>

#include <gtest/gtest.h>

#include <functional>
#include <limits>
#include <string>
#include <vector>

using namespace gopp;

namespace {

struct testpair {
    std::string decoded, encoded;
};

const std::vector<testpair> pairs = {
    // RFC 3548 examples
    {"\x14\xfb\x9c\x03\xd9\x7e", "FPucA9l+"},
    {"\x14\xfb\x9c\x03\xd9", "FPucA9k="},
    {"\x14\xfb\x9c\x03", "FPucAw=="},

    // RFC 4648 examples
    {"", ""},
    {"f", "Zg=="},
    {"fo", "Zm8="},
    {"foo", "Zm9v"},
    {"foob", "Zm9vYg=="},
    {"fooba", "Zm9vYmE="},
    {"foobar", "Zm9vYmFy"},

    // Wikipedia examples
    {"sure.", "c3VyZS4="},
    {"sure", "c3VyZQ=="},
    {"sur", "c3Vy"},
    {"su", "c3U="},
    {"leasure.", "bGVhc3VyZS4="},
    {"easure.", "ZWFzdXJlLg=="},
    {"asure.", "YXN1cmUu"},
    {"sure.", "c3VyZS4="},
};

std::string replaceAll(std::string s, char from, char to) {
    std::replace(s.begin(), s.end(), from, to);
    return s;
}

// Do nothing to a reference base64 string (leave in standard format)
std::string stdRef(std::string ref) { return ref; }

// Convert a reference string to URL-encoding
std::string urlRef(std::string ref) { return replaceAll(replaceAll(ref, '+', '-'), '/', '_'); }

// Convert a reference string to raw, unpadded format
std::string rawRef(std::string ref) {
    while (!ref.empty() && ref.back() == '=') ref.pop_back();
    return ref;
}

// Both URL and unpadding conversions
std::string rawURLRef(std::string ref) { return rawRef(urlRef(ref)); }

constexpr std::string_view encodeStd = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// A nonstandard encoding with a funny padding character, for testing
constexpr base64::Encoding funnyEncoding = base64::NewEncoding(encodeStd).WithPadding('@');

std::string funnyRef(std::string ref) { return replaceAll(ref, '=', '@'); }

struct encodingTest {
    base64::Encoding enc;                       // Encoding to test
    std::function<std::string(std::string)> conv;  // Reference string converter
};

const std::vector<encodingTest> encodingTests = {
    {base64::StdEncoding, stdRef},
    {base64::URLEncoding, urlRef},
    {base64::RawStdEncoding, rawRef},
    {base64::RawURLEncoding, rawURLRef},
    {funnyEncoding, funnyRef},
    {base64::StdEncoding.Strict(), stdRef},
    {base64::URLEncoding.Strict(), urlRef},
    {base64::RawStdEncoding.Strict(), rawRef},
    {base64::RawURLEncoding.Strict(), rawURLRef},
    {funnyEncoding.Strict(), funnyRef},
};

const testpair bigtest = {
    "Twas brillig, and the slithy toves",
    "VHdhcyBicmlsbGlnLCBhbmQgdGhlIHNsaXRoeSB0b3Zlcw==",
};

std::string str(std::span<const byte> b) { return std::string(asStringView(b)); }

TEST(GoBase64, Encode) {
    for (const auto& p : pairs) {
        for (const auto& tt : encodingTests) {
            EXPECT_EQ(tt.enc.EncodeToString(as_bytes(p.decoded)), tt.conv(p.encoded)) << "Encode(" << p.decoded << ")";
            auto dst = tt.enc.AppendEncode(toBytes("lead"), as_bytes(p.decoded));
            EXPECT_EQ(str(dst), "lead" + tt.conv(p.encoded)) << "AppendEncode(\"lead\", " << p.decoded << ")";
        }
    }
}

TEST(GoBase64, Encoder) {
    for (const auto& p : pairs) {
        auto bb = make_ptr<strings::Builder>();
        auto encoder = base64::NewEncoder(base64::StdEncoding, bb);
        encoder->Write(as_bytes(p.decoded));
        encoder->Close();
        EXPECT_EQ(bb->view(), p.encoded);
    }
}

TEST(GoBase64, EncoderBuffering) {
    auto input = as_bytes(bigtest.decoded);
    for (std::size_t bs = 1; bs <= 12; bs++) {
        auto bb = make_ptr<strings::Builder>();
        auto encoder = base64::NewEncoder(base64::StdEncoding, bb);
        for (std::size_t pos = 0; pos < input.size(); pos += bs) {
            std::size_t end = std::min(pos + bs, input.size());
            auto [n, err] = encoder->Write(input.subspan(pos, end - pos));
            EXPECT_FALSE(err) << "Write gave error " << err;
            EXPECT_EQ(n, static_cast<isize>(end - pos)) << "Write gave length " << n;
        }
        EXPECT_FALSE(encoder->Close());
        EXPECT_EQ(bb->view(), bigtest.encoded) << "Encoding/" << bs;
    }
}

TEST(GoBase64, Decode) {
    for (const auto& p : pairs) {
        for (const auto& tt : encodingTests) {
            std::string encoded = tt.conv(p.encoded);
            auto dbuf = slice<byte>::make(static_cast<std::size_t>(tt.enc.DecodedLen(static_cast<isize>(encoded.size()))));
            auto [count, err] = tt.enc.Decode(dbuf, as_bytes(encoded));
            EXPECT_FALSE(err) << "Decode(" << encoded << ") = error " << err;
            EXPECT_EQ(count, static_cast<isize>(p.decoded.size())) << "Decode(" << encoded << ") = length";
            EXPECT_EQ(str(dbuf.Sub(0, static_cast<std::size_t>(count))), p.decoded) << "Decode(" << encoded << ")";

            auto [dbuf2, err2] = tt.enc.DecodeString(encoded);
            EXPECT_FALSE(err2) << "DecodeString(" << encoded << ") = error " << err2;
            EXPECT_EQ(str(dbuf2), p.decoded) << "DecodeString(" << encoded << ")";

            auto [dst, err3] = tt.enc.AppendDecode(toBytes("lead"), as_bytes(encoded));
            EXPECT_FALSE(err3) << "AppendDecode(" << p.encoded << ") = error " << err3;
            EXPECT_EQ(str(dst), "lead" + p.decoded) << "AppendDecode(\"lead\", " << p.encoded << ")";

            auto [dst2, err4] = tt.enc.AppendDecode(dst.Sub(0, 0, p.decoded.size()), as_bytes(encoded));
            EXPECT_FALSE(err4) << "AppendDecode(" << p.encoded << ") = error " << err4;
            EXPECT_EQ(str(dst2), p.decoded) << "AppendDecode(\"\", " << p.encoded << ")";
            if (dst.len() > 0 && dst2.len() > 0) {
                EXPECT_EQ(dst.data(), dst2.data()) << "unexpected capacity growth: got " << dst2.cap() << ", want " << dst.cap();
            }
        }
    }
}

TEST(GoBase64, Decoder) {
    for (const auto& p : pairs) {
        auto decoder = base64::NewDecoder(base64::StdEncoding, strings::NewReader(string(p.encoded)));
        auto dbuf = slice<byte>::make(static_cast<std::size_t>(base64::StdEncoding.DecodedLen(static_cast<isize>(p.encoded.size()))));
        auto [count, err] = decoder->Read(dbuf);
        ASSERT_TRUE(!err || err == io::EOF_) << "Read failed " << err;
        EXPECT_EQ(count, static_cast<isize>(p.decoded.size())) << "Read from " << p.encoded;
        EXPECT_EQ(str(dbuf.Sub(0, static_cast<std::size_t>(count))), p.decoded) << "Decoding of " << p.encoded;
        if (err != io::EOF_) err = decoder->Read(dbuf).second;
        EXPECT_EQ(err, io::EOF_) << "Read from " << p.encoded << " = " << err;
    }
}

TEST(GoBase64, DecoderBuffering) {
    for (std::size_t bs = 1; bs <= 12; bs++) {
        auto decoder = base64::NewDecoder(base64::StdEncoding, strings::NewReader(string(bigtest.encoded)));
        std::vector<byte> buf(bigtest.decoded.size() + 12);
        std::size_t total = 0;
        isize n = 0;
        error err;
        while (total < bigtest.decoded.size() && !err) {
            std::tie(n, err) = decoder->Read(std::span<byte>(buf.data() + total, bs));
            total += static_cast<std::size_t>(n);
        }
        EXPECT_TRUE(!err || err == io::EOF_) << "Read from " << bigtest.encoded << " at pos " << total << " = " << err;
        EXPECT_EQ(str(std::span<const byte>(buf.data(), total)), bigtest.decoded) << "Decoding/" << bs;
    }
}

TEST(GoBase64, DecodeCorrupt) {
    struct {
        std::string input;
        int offset;  // -1 means no corruption.
    } testCases[] = {
        {"", -1},          {"\n", -1},        {"AAA=\n", -1},    {"AAAA\n", -1},  {"!!!!", 0},
        {"====", 0},       {"x===", 1},       {"=AAA", 0},       {"A=AA", 1},     {"AA=A", 2},
        {"AA==A", 4},      {"AAA=AAAA", 4},   {"AAAAA", 4},      {"AAAAAA", 4},   {"A=", 1},
        {"A==", 1},        {"AA=", 3},        {"AA==", -1},      {"AAA=", -1},    {"AAAA", -1},
        {"AAAAAA=", 7},    {"YWJjZA=====", 8}, {"A!\n", 1},      {"A=\n", 1},
    };
    for (const auto& tc : testCases) {
        auto dbuf = slice<byte>::make(static_cast<std::size_t>(base64::StdEncoding.DecodedLen(static_cast<isize>(tc.input.size()))));
        auto [_, err] = base64::StdEncoding.Decode(dbuf, as_bytes(tc.input));
        if (tc.offset == -1) {
            EXPECT_FALSE(err) << "Decoder wrongly detected corruption in " << tc.input;
            continue;
        }
        auto* e = errors::As<base64::CorruptInputError>(err);
        ASSERT_TRUE(e) << "Decoder failed to detect corruption in " << tc.input;
        EXPECT_EQ(e->Offset, tc.offset) << "Corruption in " << tc.input;
    }
}

TEST(GoBase64, DecodeBounds) {
    std::array<byte, 32> buf{};
    std::string s(base64::StdEncoding.EncodeToString(buf).view());
    auto [n, err] = base64::StdEncoding.Decode(buf, as_bytes(s));
    EXPECT_TRUE(n == static_cast<isize>(buf.size()) && !err) << "StdEncoding.Decode = " << n << ", " << err;
}

TEST(GoBase64, EncodedLen) {
    constexpr isize MaxInt = std::numeric_limits<isize>::max();
    struct {
        base64::Encoding enc;
        isize n;
        int64 want;
    } tests[] = {
        {base64::RawStdEncoding, 0, 0},
        {base64::RawStdEncoding, 1, 2},
        {base64::RawStdEncoding, 2, 3},
        {base64::RawStdEncoding, 3, 4},
        {base64::RawStdEncoding, 7, 10},
        {base64::StdEncoding, 0, 0},
        {base64::StdEncoding, 1, 4},
        {base64::StdEncoding, 2, 4},
        {base64::StdEncoding, 3, 4},
        {base64::StdEncoding, 4, 8},
        {base64::StdEncoding, 7, 12},
        // check overflow
        {base64::RawStdEncoding, (MaxInt - 5) / 8 + 1, 1537228672809129302},
        {base64::RawStdEncoding, MaxInt / 4 * 3 + 2, MaxInt},
    };
    for (const auto& tt : tests) EXPECT_EQ(tt.enc.EncodedLen(tt.n), tt.want) << "EncodedLen(" << tt.n << ")";
}

TEST(GoBase64, DecodedLen) {
    constexpr isize MaxInt = std::numeric_limits<isize>::max();
    struct {
        base64::Encoding enc;
        isize n;
        int64 want;
    } tests[] = {
        {base64::RawStdEncoding, 0, 0},
        {base64::RawStdEncoding, 2, 1},
        {base64::RawStdEncoding, 3, 2},
        {base64::RawStdEncoding, 4, 3},
        {base64::RawStdEncoding, 10, 7},
        {base64::StdEncoding, 0, 0},
        {base64::StdEncoding, 4, 3},
        {base64::StdEncoding, 8, 6},
        // check overflow
        {base64::RawStdEncoding, MaxInt / 6 + 1, 1152921504606846976},
        {base64::RawStdEncoding, MaxInt, 6917529027641081855},
    };
    for (const auto& tt : tests) EXPECT_EQ(tt.enc.DecodedLen(tt.n), tt.want) << "DecodedLen(" << tt.n << ")";
}

TEST(GoBase64, Big) {
    constexpr std::size_t n = 3 * 1000 + 1;
    std::string raw(n, '\0');
    constexpr std::string_view alpha = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
    for (std::size_t i = 0; i < n; i++) raw[i] = alpha[i % alpha.size()];
    auto encoded = make_ptr<strings::Builder>();
    auto w = base64::NewEncoder(base64::StdEncoding, encoded);
    auto [nn, err] = w->Write(as_bytes(raw));
    ASSERT_TRUE(nn == static_cast<isize>(n) && !err) << "Encoder.Write(raw) = " << nn << ", " << err;
    ASSERT_FALSE(w->Close());
    auto [decoded, err2] = io::ReadAll(*base64::NewDecoder(base64::StdEncoding, strings::NewReader(string(encoded->view()))));
    ASSERT_FALSE(err2) << "io.ReadAll(NewDecoder(...)): " << err2;
    EXPECT_EQ(str(decoded), raw) << "Decode(Encode(" << n << "-byte string)) failed";
}

TEST(GoBase64, NewLineCharacters) {
    // Each of these should decode to the string "sure", without errors.
    const std::string expected = "sure";
    for (std::string e : {"c3VyZQ==", "c3VyZQ==\r", "c3VyZQ==\n", "c3VyZQ==\r\n", "c3VyZ\r\nQ==", "c3V\ryZ\nQ==",
                          "c3V\nyZ\rQ==", "c3VyZ\nQ==", "c3VyZQ\n==", "c3VyZQ=\n=", "c3VyZQ=\r\n\r\n="}) {
        auto [buf, err] = base64::StdEncoding.DecodeString(e);
        ASSERT_FALSE(err) << "Decode(" << e << ") failed: " << err;
        EXPECT_EQ(str(buf), expected) << "Decode(" << e << ")";
    }
}

struct nextRead {
    isize n;    // bytes to return
    error err;  // error to return
};

// faultInjectReader returns data from source, rate-limited and with the
// errors as written to nextc.
struct faultInjectReader : io::Reader {
    faultInjectReader(std::string source, chan<nextRead> nextc) : source(std::move(source)), nextc(std::move(nextc)) {}
    std::pair<isize, error> Read(std::span<byte> p) override {
        nextRead nr = nextc.recv();
        if (static_cast<isize>(p.size()) > nr.n) p = p.first(static_cast<std::size_t>(nr.n));
        std::size_t n = std::min(p.size(), source.size());
        std::copy_n(source.begin(), n, p.begin());
        source.erase(0, n);
        return {static_cast<isize>(n), nr.err};
    }
    std::string source;
    chan<nextRead> nextc;
};

// tests that we don't ignore errors from our underlying reader
TEST(GoBase64, DecoderIssue3577) {
    auto next = makeChan<nextRead>(10);
    auto wantErr = errors::New("my error");
    next.send({5, nullptr});
    next.send({10, wantErr});
    next.send({0, wantErr});
    auto d = base64::NewDecoder(base64::StdEncoding,
                                make_ptr<faultInjectReader>("VHdhcyBicmlsbGlnLCBhbmQgdGhlIHNsaXRoeSB0b3Zlcw==",  // twas brillig...
                                                            next));
    auto errc = makeChan<error>(1);
    go([d, errc] { errc.send(io::ReadAll(*d).second); });
    select(recv(errc, [&](error err) { EXPECT_EQ(err, wantErr) << "got error " << err << "; want " << wantErr; }),
           recv(time::After(5 * time::Second), [] { ADD_FAILURE() << "timeout; Decoder blocked without returning an error"; }));
}

TEST(GoBase64, DecoderIssue4779) {
    const std::string encoded = R"(CP/EAT8AAAEF
AQEBAQEBAAAAAAAAAAMAAQIEBQYHCAkKCwEAAQUBAQEBAQEAAAAAAAAAAQACAwQFBgcICQoLEAAB
BAEDAgQCBQcGCAUDDDMBAAIRAwQhEjEFQVFhEyJxgTIGFJGhsUIjJBVSwWIzNHKC0UMHJZJT8OHx
Y3M1FqKygyZEk1RkRcKjdDYX0lXiZfKzhMPTdePzRieUpIW0lcTU5PSltcXV5fVWZnaGlqa2xtbm
9jdHV2d3h5ent8fX5/cRAAICAQIEBAMEBQYHBwYFNQEAAhEDITESBEFRYXEiEwUygZEUobFCI8FS
0fAzJGLhcoKSQ1MVY3M08SUGFqKygwcmNcLSRJNUoxdkRVU2dGXi8rOEw9N14/NGlKSFtJXE1OT0
pbXF1eX1VmZ2hpamtsbW5vYnN0dXZ3eHl6e3x//aAAwDAQACEQMRAD8A9VSSSSUpJJJJSkkkJ+Tj
1kiy1jCJJDnAcCTykpKkuQ6p/jN6FgmxlNduXawwAzaGH+V6jn/R/wCt71zdn+N/qL3kVYFNYB4N
ji6PDVjWpKp9TSXnvTf8bFNjg3qOEa2n6VlLpj/rT/pf567DpX1i6L1hs9Py67X8mqdtg/rUWbbf
+gkp0kkkklKSSSSUpJJJJT//0PVUkkklKVLq3WMDpGI7KzrNjADtYNXvI/Mqr/Pd/q9W3vaxjnvM
NaCXE9gNSvGPrf8AWS3qmba5jjsJhoB0DAf0NDf6sevf+/lf8Hj0JJATfWT6/dV6oXU1uOLQeKKn
EQP+Hubtfe/+R7Mf/g7f5xcocp++Z11JMCJPgFBxOg7/AOuqDx8I/ikpkXkmSdU8mJIJA/O8EMAy
j+mSARB/17pKVXYWHXjsj7yIex0PadzXMO1zT5KHoNA3HT8ietoGhgjsfA+CSnvvqh/jJtqsrwOv
2b6NGNzXfTYexzJ+nU7/ALkf4P8Awv6P9KvTQQ4AgyDqCF85Pho3CTB7eHwXoH+LT65uZbX9X+o2
bqbPb06551Y4
)";
    std::string encodedShort = encoded;
    std::erase(encodedShort, '\n');

    auto [res1, err] = io::ReadAll(*base64::NewDecoder(base64::StdEncoding, strings::NewReader(string(encoded))));
    EXPECT_FALSE(err) << "ReadAll failed: " << err;
    auto [res2, err2] = io::ReadAll(*base64::NewDecoder(base64::StdEncoding, strings::NewReader(string(encodedShort))));
    EXPECT_FALSE(err2) << "ReadAll failed: " << err2;
    EXPECT_EQ(str(res1), str(res2)) << "Decoded results not equal";
}

TEST(GoBase64, DecoderIssue7733) {
    auto [s, err] = base64::StdEncoding.DecodeString("YWJjZA=====");
    auto* e = errors::As<base64::CorruptInputError>(err);
    ASSERT_TRUE(e) << "Error = " << err << "; want CorruptInputError(8)";
    EXPECT_EQ(e->Offset, 8);
    EXPECT_EQ(str(s), "abcd");
}

TEST(GoBase64, DecoderIssue15656) {
    auto err = base64::StdEncoding.Strict().DecodeString("WvLTlMrX9NpYDQlEIFlnDB==").second;
    auto* e = errors::As<base64::CorruptInputError>(err);
    ASSERT_TRUE(e) << "Error = " << err << "; want CorruptInputError(22)";
    EXPECT_EQ(e->Offset, 22);
    err = base64::StdEncoding.Strict().DecodeString("WvLTlMrX9NpYDQlEIFlnDA==").second;
    EXPECT_FALSE(err) << "Error = " << err << "; want nil";
    err = base64::StdEncoding.DecodeString("WvLTlMrX9NpYDQlEIFlnDB==").second;
    EXPECT_FALSE(err) << "Error = " << err << "; want nil";
}

TEST(GoBase64, DecoderRaw) {
    const std::string source = "AAAAAA";
    const std::string want(4, '\0');

    // Direct.
    auto [dec1, err] = base64::RawURLEncoding.DecodeString(source);
    EXPECT_TRUE(!err && str(dec1) == want) << "RawURLEncoding.DecodeString = " << err;

    // Through reader. Used to fail.
    auto r = base64::NewDecoder(base64::RawURLEncoding, strings::NewReader(string(source)));
    auto [dec2, err2] = io::ReadAll(*io::LimitReader(r, 100));
    EXPECT_TRUE(!err2 && str(dec2) == want) << "reading NewDecoder(RawURLEncoding) = " << err2;

    // Should work with padding.
    r = base64::NewDecoder(base64::URLEncoding, strings::NewReader(string(source + "==")));
    auto [dec3, err3] = io::ReadAll(*r);
    EXPECT_TRUE(!err3 && str(dec3) == want) << "reading NewDecoder(URLEncoding) = " << err3;
}

TEST(GoBase64DeathTest, NewEncodingPanics) {
    // The checks of NewEncoding and WithPadding, which Go's examples rely on.
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH(base64::NewEncoding("abc"), "encoding alphabet is not 64-bytes long");
    EXPECT_DEATH(base64::StdEncoding.WithPadding('A'), "padding contained in alphabet");
}

}  // namespace
