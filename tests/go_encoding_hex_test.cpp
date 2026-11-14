// Ported from Go 1.27.3's encoding/hex/hex_test.go, with Go's test names.
// Left out: the benchmarks.
#include <gopp/encoding/hex.hpp>
#include <gopp/io.hpp>
#include <gopp/strings.hpp>

#include <gtest/gtest.h>

#include <array>
#include <string>
#include <vector>

using namespace gopp;

namespace {

struct encDecTest {
    std::string enc;
    std::vector<byte> dec;
};

const std::vector<encDecTest> encDecTests = {
    {"", {}},
    {"0001020304050607", {0, 1, 2, 3, 4, 5, 6, 7}},
    {"08090a0b0c0d0e0f", {8, 9, 10, 11, 12, 13, 14, 15}},
    {"f0f1f2f3f4f5f6f7", {0xf0, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7}},
    {"f8f9fafbfcfdfeff", {0xf8, 0xf9, 0xfa, 0xfb, 0xfc, 0xfd, 0xfe, 0xff}},
    {"67", {'g'}},
    {"e3a1", {0xe3, 0xa1}},
};

std::string str(std::span<const byte> b) { return std::string(asStringView(b)); }
std::string str(const std::vector<byte>& b) { return std::string(b.begin(), b.end()); }

TEST(GoHex, Encode) {
    int i = 0;
    for (const auto& test : encDecTests) {
        auto dst = slice<byte>::make(static_cast<std::size_t>(hex::EncodedLen(static_cast<isize>(test.dec.size()))));
        auto n = hex::Encode(dst, test.dec);
        EXPECT_EQ(n, static_cast<isize>(dst.len())) << "#" << i << ": bad return value";
        EXPECT_EQ(str(dst), test.enc) << "#" << i;
        dst = hex::AppendEncode(toBytes("lead"), test.dec);
        EXPECT_EQ(str(dst), "lead" + test.enc) << "#" << i;
        i++;
    }
}

TEST(GoHex, Decode) {
    // Case for decoding uppercase hex characters, since Encode always uses
    // lowercase.
    auto decTests = encDecTests;
    decTests.push_back({"F8F9FAFBFCFDFEFF", {0xf8, 0xf9, 0xfa, 0xfb, 0xfc, 0xfd, 0xfe, 0xff}});
    int i = 0;
    for (const auto& test : decTests) {
        auto dst = slice<byte>::make(static_cast<std::size_t>(hex::DecodedLen(static_cast<isize>(test.enc.size()))));
        auto [n, err] = hex::Decode(dst, as_bytes(test.enc));
        EXPECT_FALSE(err) << "#" << i;
        EXPECT_EQ(str(dst), str(test.dec)) << "#" << i;
        auto [dst2, err2] = hex::AppendDecode(toBytes("lead"), as_bytes(test.enc));
        EXPECT_FALSE(err2) << "#" << i << ": AppendDecode error: " << err2;
        EXPECT_EQ(str(dst2), "lead" + str(test.dec)) << "#" << i;
        i++;
    }
}

TEST(GoHex, EncodeToString) {
    for (const auto& test : encDecTests) EXPECT_EQ(hex::EncodeToString(test.dec), test.enc);
}

TEST(GoHex, DecodeString) {
    int i = 0;
    for (const auto& test : encDecTests) {
        auto [dst, err] = hex::DecodeString(test.enc);
        ASSERT_FALSE(err) << "#" << i << ": unexpected err value: " << err;
        EXPECT_EQ(str(dst), str(test.dec)) << "#" << i;
        i++;
    }
}

// The error of errTests: nil, ErrLength or an InvalidByteError.
struct errTest {
    std::string in;
    std::string out;
    error err;
};

std::vector<errTest> errTests() {
    auto invalid = [](byte b) { return error(make_ptr<hex::InvalidByteError>(b)); };
    return {
        {"", "", nullptr},
        {"0", "", hex::ErrLength},
        {"zd4aa", "", invalid('z')},
        {"d4aaz", "\xd4\xaa", invalid('z')},
        {"30313", "01", hex::ErrLength},
        {"0g", "", invalid('g')},
        {"00gg", std::string("\x00", 1), invalid('g')},
        {"0\x01", "", invalid('\x01')},
        {"ffeed", "\xff\xee", hex::ErrLength},
    };
}

// Go compares the errors with ==, which compares InvalidByteError values.
bool sameError(const error& got, const error& want) {
    auto* w = errors::As<hex::InvalidByteError>(want);
    if (!w) return got == want;
    auto* g = errors::As<hex::InvalidByteError>(got);
    return g && g->Byte == w->Byte;
}

TEST(GoHex, DecodeErr) {
    for (const auto& tt : errTests()) {
        auto out = slice<byte>::make(tt.in.size() + 10);
        auto [n, err] = hex::Decode(out, as_bytes(tt.in));
        EXPECT_EQ(str(out.Sub(0, static_cast<std::size_t>(n))), tt.out) << "Decode(" << tt.in << ")";
        EXPECT_TRUE(sameError(err, tt.err)) << "Decode(" << tt.in << ") = " << err << ", want " << tt.err;
    }
}

TEST(GoHex, DecodeStringErr) {
    for (const auto& tt : errTests()) {
        auto [out, err] = hex::DecodeString(tt.in);
        EXPECT_EQ(str(out), tt.out) << "DecodeString(" << tt.in << ")";
        EXPECT_TRUE(sameError(err, tt.err)) << "DecodeString(" << tt.in << ") = " << err << ", want " << tt.err;
    }
}

// Go's io.CopyBuffer with a 7-byte buffer, between a Reader and a Writer
// that are nothing else.
std::pair<int64, error> copyBuffer(io::Writer& dst, io::Reader& src) {
    byte buf[7];
    int64 written = 0;
    for (;;) {
        auto [nr, er] = src.Read(buf);
        if (nr > 0) {
            auto [nw, ew] = dst.Write(std::span<const byte>(buf, static_cast<std::size_t>(nr)));
            written += nw;
            if (ew) return {written, ew};
        }
        if (er == io::EOF_) return {written, nullptr};
        if (er) return {written, er};
    }
}

TEST(GoHex, EncoderDecoder) {
    for (int multiplier : {1, 128, 192}) {
        for (const auto& test : encDecTests) {
            std::string input, output;
            for (int i = 0; i < multiplier; i++) {
                input += str(test.dec);
                output += test.enc;
            }

            auto buf = make_ptr<strings::Builder>();
            auto enc = hex::NewEncoder(buf);
            io::StringReader r{string(input)};
            auto [n, err] = copyBuffer(*enc, r);
            ASSERT_TRUE(n == static_cast<int64>(input.size()) && !err) << "encoder.Write = " << n << ", " << err;
            ASSERT_EQ(buf->view(), output);

            auto dec = hex::NewDecoder(strings::NewReader(string(buf->view())));
            strings::Builder decBuf;
            auto [m, err2] = copyBuffer(decBuf, *dec);
            ASSERT_TRUE(!err2 && decBuf.len() == input.size()) << "decoder.Read = " << decBuf.len() << ", " << err2;
            EXPECT_EQ(decBuf.view(), input);
        }
    }
}

TEST(GoHex, DecoderErr) {
    for (const auto& tt : errTests()) {
        auto dec = hex::NewDecoder(strings::NewReader(string(tt.in)));
        auto [out, err] = io::ReadAll(*dec);
        auto wantErr = tt.err;
        // Decoder is reading from stream, so it reports io.ErrUnexpectedEOF instead of ErrLength.
        if (wantErr == hex::ErrLength) wantErr = io::ErrUnexpectedEOF;
        EXPECT_EQ(str(out), tt.out) << "NewDecoder(" << tt.in << ")";
        EXPECT_TRUE(sameError(err, wantErr)) << "NewDecoder(" << tt.in << ") = " << err << ", want " << wantErr;
    }
}

const std::string expectedHexDump =
    R"(00000000  1e 1f 20 21 22 23 24 25  26 27 28 29 2a 2b 2c 2d  |.. !"#$%&'()*+,-|
00000010  2e 2f 30 31 32 33 34 35  36 37 38 39 3a 3b 3c 3d  |./0123456789:;<=|
00000020  3e 3f 40 41 42 43 44 45                           |>?@ABCDE|
)";

TEST(GoHex, Dumper) {
    std::array<byte, 40> in;
    for (std::size_t i = 0; i < in.size(); i++) in[i] = static_cast<byte>(i + 30);

    for (std::size_t stride = 1; stride < in.size(); stride++) {
        auto out = make_ptr<strings::Builder>();
        auto dumper = hex::Dumper(out);
        std::size_t done = 0;
        while (done < in.size()) {
            std::size_t todo = std::min(done + stride, in.size());
            dumper->Write(std::span<const byte>(in.data() + done, todo - done));
            done = todo;
        }
        dumper->Close();
        EXPECT_EQ(out->view(), expectedHexDump) << "stride: " << stride;
    }
}

TEST(GoHex, Dumper_doubleclose) {
    auto out = make_ptr<strings::Builder>();
    auto dumper = hex::Dumper(out);
    dumper->Write(as_bytes("gopher"));
    dumper->Close();
    dumper->Close();
    dumper->Write(as_bytes("gopher"));
    dumper->Close();
    EXPECT_EQ(out->view(), "00000000  67 6f 70 68 65 72                                 |gopher|\n");
}

TEST(GoHex, Dumper_earlyclose) {
    auto out = make_ptr<strings::Builder>();
    auto dumper = hex::Dumper(out);
    dumper->Close();
    dumper->Write(as_bytes("gopher"));
    EXPECT_EQ(out->view(), "");
}

TEST(GoHex, Dump) {
    std::array<byte, 40> in;
    for (std::size_t i = 0; i < in.size(); i++) in[i] = static_cast<byte>(i + 30);
    EXPECT_EQ(hex::Dump(in), expectedHexDump);
}

TEST(GoHex, InvalidByteError) {
    // fmt's %#U, as Go's message has it.
    EXPECT_EQ(hex::InvalidByteError('g').Error(), "encoding/hex: invalid byte: U+0067 'g'");
    EXPECT_EQ(hex::InvalidByteError('\x01').Error(), "encoding/hex: invalid byte: U+0001");
}

}  // namespace
