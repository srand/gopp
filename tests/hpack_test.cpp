// Ported from golang.org/x/net/http2/hpack: hpack_test.Go, encode_test.Go.
#include "net/http/hpack.hpp"

#include <gtest/gtest.h>

#include <random>

using namespace gopp;
using namespace gopp::http::hpack;
namespace hpack = gopp::http::hpack;

namespace gopp::http::hpack {
struct test_access {
    static std::pair<uint64, bool> searchTable(const Encoder& e, const HeaderField& f) { return e.searchTable(f); }
    static detail::dynamicTable& dyn(Encoder& e) { return e.dyn_; }
    static bool tableSizeUpdate(const Encoder& e) { return e.table_size_update_; }
    static uint32 minSize(const Encoder& e) { return e.min_size_; }
    static uint32 maxSizeLimit(const Encoder& e) { return e.max_size_limit_; }
};
}  // namespace gopp::http::hpack

namespace {

HeaderField P(std::string_view n, std::string_view v) { return HeaderField{string(n), string(v)}; }

std::string dehex(std::string_view s) {
    std::string out;
    int hi = -1;
    for (char c : s) {
        int v;
        if (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
        else continue;
        if (hi < 0) hi = v;
        else {
            out.push_back(static_cast<char>(hi << 4 | v));
            hi = -1;
        }
    }
    return out;
}

std::string hex(std::string_view s) {
    static const char* d = "0123456789abcdef";
    std::string out;
    for (unsigned char c : s) {
        out.push_back(d[c >> 4]);
        out.push_back(d[c & 15]);
    }
    return out;
}

std::span<const byte> byteSpan(std::string_view s) { return {reinterpret_cast<const byte*>(s.data()), s.size()}; }

std::vector<HeaderField> reverseCopy(const hpack::detail::dynamicTable& dt) {
    std::vector<HeaderField> hf;
    for (std::size_t i = dt.table.len(); i > 0; i--) hf.push_back(dt.table.at(i - 1));
    return hf;
}

struct enc_and_want {
    std::string enc;
    std::vector<HeaderField> want;
    std::vector<HeaderField> want_dyn_tab;
    uint32 want_dyn_size;
};

void testDecodeSeries(uint32 size, std::vector<enc_and_want> steps) {
    Decoder d(size, nullptr);
    for (std::size_t i = 0; i < steps.size(); i++) {
        auto& step = steps[i];
        auto [hf, err] = d.decodeFull(byteSpan(step.enc));
        ASSERT_FALSE(err) << "step " << i << ": " << err;
        ASSERT_EQ(hf, step.want) << "step " << i;
        EXPECT_EQ(reverseCopy(d.dynamicTable()), step.want_dyn_tab) << "step " << i;
        EXPECT_EQ(d.dynamicTable().size, step.want_dyn_size) << "step " << i;
    }
}

}  // namespace

namespace gopp::http::hpack {
void PrintTo(const HeaderField& f, std::ostream* os) { *os << f.String(); }
}  // namespace gopp::http::hpack

TEST(Hpack, DecoderDecode) {
    struct test {
        const char* name;
        std::string in;
        std::vector<HeaderField> want, want_dyn_tab;
    };
    test tests[] = {
        // C.2.1 Literal Header Field with Indexing
        {"C.2.1", dehex("400a 6375 7374 6f6d 2d6b 6579 0d63 7573 746f 6d2d 6865 6164 6572"),
         {P("custom-key", "custom-header")}, {P("custom-key", "custom-header")}},
        // C.2.2 Literal Header Field without Indexing
        {"C.2.2", dehex("040c 2f73 616d 706c 652f 7061 7468"), {P(":path", "/sample/path")}, {}},
        // C.2.3 Literal Header Field never Indexed
        {"C.2.3", dehex("1008 7061 7373 776f 7264 0673 6563 7265 74"), {HeaderField{"password", "secret", true}}, {}},
        // C.2.4 Indexed Header Field
        {"C.2.4", "\x82", {P(":method", "GET")}, {}},
    };
    for (auto& tt : tests) {
        Decoder d(4096, nullptr);
        auto [hf, err] = d.decodeFull(byteSpan(tt.in));
        ASSERT_FALSE(err) << tt.name << ": " << err;
        EXPECT_EQ(hf, tt.want) << tt.name;
        EXPECT_EQ(reverseCopy(d.dynamicTable()), tt.want_dyn_tab) << tt.name;
    }
}

TEST(Hpack, DynamicTableAt) {
    Decoder d(4096, nullptr);
    auto at = [&](int i) {
        // Index i of the combined table, by decoding an indexed field.
        std::string in;
        appendVarInt(in, 7, static_cast<uint64>(i));
        in[0] = static_cast<char>(in[0] | 0x80);
        auto [hf, err] = d.decodeFull(byteSpan(in));
        EXPECT_FALSE(err) << err;
        return hf.empty() ? HeaderField{} : hf[0];
    };
    EXPECT_EQ(at(2), P(":method", "GET"));
    std::string lit = dehex("4003 666f 6f03 6261 72");  // foo: bar, indexed
    ASSERT_FALSE(d.decodeFull(byteSpan(lit)).second);
    EXPECT_EQ(at(62), P("foo", "bar"));
    std::string lit2 = dehex("4004 626c 6168 0362 617a");  // blah: baz, indexed
    ASSERT_FALSE(d.decodeFull(byteSpan(lit2)).second);
    EXPECT_EQ(at(62), P("blah", "baz"));
    EXPECT_EQ(at(63), P("foo", "bar"));
    std::string bad = "\xc0";  // index 64: past the end
    auto [hf, err] = d.decodeFull(byteSpan(bad));
    ASSERT_TRUE(err);
    EXPECT_EQ(err.Error(), "decoding error: invalid indexed representation index 64");
}

// https://httpwg.org/specs/rfc7541.html#rfc.section.C.3
TEST(Hpack, DecodeC3_NoHuffman) {
	testDecodeSeries(4096, {
		{dehex("8286 8441 0f77 7777 2e65 7861 6d70 6c65 2e63 6f6d"),
			{
				P(":method", "GET"),
				P(":scheme", "http"),
				P(":path", "/"),
				P(":authority", "www.example.com"),
			},
			{
				P(":authority", "www.example.com"),
			},
			57,
		},
		{dehex("8286 84be 5808 6e6f 2d63 6163 6865"),
			{
				P(":method", "GET"),
				P(":scheme", "http"),
				P(":path", "/"),
				P(":authority", "www.example.com"),
				P("cache-control", "no-cache"),
			},
			{
				P("cache-control", "no-cache"),
				P(":authority", "www.example.com"),
			},
			110,
		},
		{dehex("8287 85bf 400a 6375 7374 6f6d 2d6b 6579 0c63 7573 746f 6d2d 7661 6c75 65"),
			{
				P(":method", "GET"),
				P(":scheme", "https"),
				P(":path", "/index.html"),
				P(":authority", "www.example.com"),
				P("custom-key", "custom-value"),
			},
			{
				P("custom-key", "custom-value"),
				P("cache-control", "no-cache"),
				P(":authority", "www.example.com"),
			},
			164,
		},
	});
}

// C.4 Request Examples with Huffman Coding
// https://httpwg.org/specs/rfc7541.html#rfc.section.C.4
TEST(Hpack, DecodeC4_Huffman) {
	testDecodeSeries(4096, {
		{dehex("8286 8441 8cf1 e3c2 e5f2 3a6b a0ab 90f4 ff"),
			{
				P(":method", "GET"),
				P(":scheme", "http"),
				P(":path", "/"),
				P(":authority", "www.example.com"),
			},
			{
				P(":authority", "www.example.com"),
			},
			57,
		},
		{dehex("8286 84be 5886 a8eb 1064 9cbf"),
			{
				P(":method", "GET"),
				P(":scheme", "http"),
				P(":path", "/"),
				P(":authority", "www.example.com"),
				P("cache-control", "no-cache"),
			},
			{
				P("cache-control", "no-cache"),
				P(":authority", "www.example.com"),
			},
			110,
		},
		{dehex("8287 85bf 4088 25a8 49e9 5ba9 7d7f 8925 a849 e95b b8e8 b4bf"),
			{
				P(":method", "GET"),
				P(":scheme", "https"),
				P(":path", "/index.html"),
				P(":authority", "www.example.com"),
				P("custom-key", "custom-value"),
			},
			{
				P("custom-key", "custom-value"),
				P("cache-control", "no-cache"),
				P(":authority", "www.example.com"),
			},
			164,
		},
	});
}

// https://httpwg.org/specs/rfc7541.html#rfc.section.C.5
// "This section shows several consecutive header lists, corresponding
// to HTTP responses, on the same connection. The HTTP/2 setting
// parameter SETTINGS_HEADER_TABLE_SIZE is set to the value of 256
// octets, causing some evictions to occur."
TEST(Hpack, DecodeC5_ResponsesNoHuff) {
	testDecodeSeries(256, {
		{dehex(R"(
4803 3330 3258 0770 7269 7661 7465 611d
4d6f 6e2c 2032 3120 4f63 7420 3230 3133
2032 303a 3133 3a32 3120 474d 546e 1768
7474 7073 3a2f 2f77 7777 2e65 7861 6d70
6c65 2e63 6f6d
)"),
			{
				P(":status", "302"),
				P("cache-control", "private"),
				P("date", "Mon, 21 Oct 2013 20:13:21 GMT"),
				P("location", "https://www.example.com"),
			},
			{
				P("location", "https://www.example.com"),
				P("date", "Mon, 21 Oct 2013 20:13:21 GMT"),
				P("cache-control", "private"),
				P(":status", "302"),
			},
			222,
		},
		{dehex("4803 3330 37c1 c0bf"),
			{
				P(":status", "307"),
				P("cache-control", "private"),
				P("date", "Mon, 21 Oct 2013 20:13:21 GMT"),
				P("location", "https://www.example.com"),
			},
			{
				P(":status", "307"),
				P("location", "https://www.example.com"),
				P("date", "Mon, 21 Oct 2013 20:13:21 GMT"),
				P("cache-control", "private"),
			},
			222,
		},
		{dehex(R"(
88c1 611d 4d6f 6e2c 2032 3120 4f63 7420
3230 3133 2032 303a 3133 3a32 3220 474d
54c0 5a04 677a 6970 7738 666f 6f3d 4153
444a 4b48 514b 425a 584f 5157 454f 5049
5541 5851 5745 4f49 553b 206d 6178 2d61
6765 3d33 3630 303b 2076 6572 7369 6f6e
3d31
)"),
			{
				P(":status", "200"),
				P("cache-control", "private"),
				P("date", "Mon, 21 Oct 2013 20:13:22 GMT"),
				P("location", "https://www.example.com"),
				P("content-encoding", "gzip"),
				P("set-cookie", "foo=ASDJKHQKBZXOQWEOPIUAXQWEOIU; max-age=3600; version=1"),
			},
			{
				P("set-cookie", "foo=ASDJKHQKBZXOQWEOPIUAXQWEOIU; max-age=3600; version=1"),
				P("content-encoding", "gzip"),
				P("date", "Mon, 21 Oct 2013 20:13:22 GMT"),
			},
			215,
		},
	});
}

// https://httpwg.org/specs/rfc7541.html#rfc.section.C.6
// "This section shows the same examples as the previous section, but
// using Huffman encoding for the literal values. The HTTP/2 setting
// parameter SETTINGS_HEADER_TABLE_SIZE is set to the value of 256
// octets, causing some evictions to occur. The eviction mechanism
// uses the length of the decoded literal values, so the same
// evictions occurs as in the previous section."
TEST(Hpack, DecodeC6_ResponsesHuffman) {
	testDecodeSeries(256, {
		{dehex(R"(
4882 6402 5885 aec3 771a 4b61 96d0 7abe
9410 54d4 44a8 2005 9504 0b81 66e0 82a6
2d1b ff6e 919d 29ad 1718 63c7 8f0b 97c8
e9ae 82ae 43d3
)"),
			{
				P(":status", "302"),
				P("cache-control", "private"),
				P("date", "Mon, 21 Oct 2013 20:13:21 GMT"),
				P("location", "https://www.example.com"),
			},
			{
				P("location", "https://www.example.com"),
				P("date", "Mon, 21 Oct 2013 20:13:21 GMT"),
				P("cache-control", "private"),
				P(":status", "302"),
			},
			222,
		},
		{dehex("4883 640e ffc1 c0bf"),
			{
				P(":status", "307"),
				P("cache-control", "private"),
				P("date", "Mon, 21 Oct 2013 20:13:21 GMT"),
				P("location", "https://www.example.com"),
			},
			{
				P(":status", "307"),
				P("location", "https://www.example.com"),
				P("date", "Mon, 21 Oct 2013 20:13:21 GMT"),
				P("cache-control", "private"),
			},
			222,
		},
		{dehex(R"(
88c1 6196 d07a be94 1054 d444 a820 0595
040b 8166 e084 a62d 1bff c05a 839b d9ab
77ad 94e7 821d d7f2 e6c7 b335 dfdf cd5b
3960 d5af 2708 7f36 72c1 ab27 0fb5 291f
9587 3160 65c0 03ed 4ee5 b106 3d50 07
)"),
			{
				P(":status", "200"),
				P("cache-control", "private"),
				P("date", "Mon, 21 Oct 2013 20:13:22 GMT"),
				P("location", "https://www.example.com"),
				P("content-encoding", "gzip"),
				P("set-cookie", "foo=ASDJKHQKBZXOQWEOPIUAXQWEOIU; max-age=3600; version=1"),
			},
			{
				P("set-cookie", "foo=ASDJKHQKBZXOQWEOPIUAXQWEOIU; max-age=3600; version=1"),
				P("content-encoding", "gzip"),
				P("date", "Mon, 21 Oct 2013 20:13:22 GMT"),
			},
			215,
		},
	});
}

TEST(Hpack, HuffmanDecodeExcessPadding) {
    std::string tests[] = {
        "\xff",                                 // Padding Exceeds 7 bits
        "\x1f\xff",                             // {"a", 1 byte excess padding}
        "\x1f\xff\xff",                         // {"a", 2 byte excess padding}
        "\x1f\xff\xff\xff",                     // {"a", 3 byte excess padding}
        "\xff\x9f\xff\xff\xff",                 // {"a", 29 bit excess padding}
        std::string("R\xbc" "0\xff\xff\xff\xff"),  // Padding ends on partial symbol.
    };
    for (std::size_t i = 0; i < std::size(tests); i++) {
        EXPECT_EQ(huffmanDecodeToString(byteSpan(tests[i])).second, err_invalid_huffman) << "test-" << i;
    }
}

TEST(Hpack, HuffmanDecodeEOS) {
    std::string in = "\xff\xff\xff\xff\xfc";  // {EOS, "?"}
    EXPECT_EQ(huffmanDecodeToString(byteSpan(in)).second, err_invalid_huffman);
}

TEST(Hpack, HuffmanDecodeMaxLengthOnTrailingByte) {
    std::string in("\x00\x01", 2);  // {"0", "0", "0"}
    std::string buf;
    EXPECT_EQ(huffmanDecode(buf, 2, byteSpan(in)), err_string_length);
}

TEST(Hpack, HuffmanDecodeCorruptPadding) {
    std::string in("\x00", 1);
    EXPECT_EQ(huffmanDecodeToString(byteSpan(in)).second, err_invalid_huffman);
}

namespace {
const std::pair<const char*, const char*> huffman_vectors[] = {
    {"f1e3 c2e5 f23a 6ba0 ab90 f4ff", "www.example.com"},
    {"a8eb 1064 9cbf", "no-cache"},
    {"25a8 49e9 5ba9 7d7f", "custom-key"},
    {"25a8 49e9 5bb8 e8b4 bf", "custom-value"},
    {"6402", "302"},
    {"aec3 771a 4b", "private"},
    {"d07a be94 1054 d444 a820 0595 040b 8166 e082 a62d 1bff", "Mon, 21 Oct 2013 20:13:21 GMT"},
    {"9d29 ad17 1863 c78f 0b97 c8e9 ae82 ae43 d3", "https://www.example.com"},
    {"9bd9 ab", "gzip"},
    {"94e7 821d d7f2 e6c7 b335 dfdf cd5b 3960 d5af 2708 7f36 72c1 ab27 0fb5 291f 9587 3160 65c0 03ed 4ee5 b106 3d50 07",
     "foo=ASDJKHQKBZXOQWEOPIUAXQWEOIU; max-age=3600; version=1"},
};
}  // namespace

TEST(Hpack, HuffmanDecode) {
    for (auto [in_hex, want] : huffman_vectors) {
        auto [got, err] = huffmanDecodeToString(byteSpan(dehex(in_hex)));
        ASSERT_FALSE(err) << in_hex << ": " << err;
        EXPECT_EQ(got, want);
    }
}

TEST(Hpack, AppendHuffmanString) {
    for (auto [want, in] : huffman_vectors) {
        std::string buf;
        appendHuffmanString(buf, in);
        EXPECT_EQ(hex(buf), hex(dehex(want))) << in;
    }
}

TEST(Hpack, HuffmanMaxStrLen) {
    const std::string msg = "Some string";
    std::string huff;
    appendHuffmanString(huff, msg);
    for (int max : {0, static_cast<int>(msg.size()), static_cast<int>(msg.size()) + 1}) {
        std::string out;
        EXPECT_FALSE(huffmanDecode(out, max, byteSpan(huff))) << max;
        EXPECT_EQ(out, msg);
    }
    std::string out;
    EXPECT_EQ(huffmanDecode(out, static_cast<int>(msg.size()) - 1, byteSpan(huff)), err_string_length);
}

TEST(Hpack, HuffmanRoundtripStress) {
    std::mt19937 rng(1);
    std::string input(50, '\0');
    for (int i = 0; i < 5000; i++) {
        for (auto& c : input) c = static_cast<char>(rng());
        std::string huff;
        appendHuffmanString(huff, input);
        auto [out, err] = huffmanDecodeToString(byteSpan(huff));
        ASSERT_FALSE(err) << err;
        ASSERT_EQ(out, input);
    }
}

TEST(Hpack, HuffmanDecodeFuzz) {
    // Random input either decodes or fails cleanly.
    std::mt19937 rng(1);
    int good = 0;
    for (int i = 0; i < 50000; i++) {
        std::string in(1 + rng() % 49, '\0');
        for (auto& c : in) c = static_cast<char>(rng());
        auto [out, err] = huffmanDecodeToString(byteSpan(in));
        if (!err) {
            good++;
            std::string re;
            appendHuffmanString(re, out);
            ASSERT_EQ(huffmanDecodeToString(byteSpan(re)).first, out);
        } else {
            ASSERT_EQ(err, err_invalid_huffman);
        }
    }
    EXPECT_GT(good, 0);
}

TEST(Hpack, ReadVarInt) {
    struct test {
        uint8 n;
        std::string p;
        uint64 i;
        std::size_t consumed;
        gopp::error err;
    };
    std::string overflow = "\xff";
    for (int k = 0; k < 15; k++) overflow.push_back('\x80');
    test tests[] = {
        // Fits in a byte:
        {1, std::string(1, '\0'), 0, 1, nullptr}, {2, "\x02", 2, 1, nullptr}, {3, "\x06", 6, 1, nullptr},
        {4, "\x0e", 14, 1, nullptr}, {5, "\x1e", 30, 1, nullptr}, {6, "\x3e", 62, 1, nullptr},
        {7, "\x7e", 126, 1, nullptr}, {8, "\xfe", 254, 1, nullptr},
        // Doesn't fit in a byte:
        {1, "\x01", 0, 0, err_need_more}, {2, "\x03", 0, 0, err_need_more}, {3, "\x07", 0, 0, err_need_more},
        {4, "\x0f", 0, 0, err_need_more}, {5, "\x1f", 0, 0, err_need_more}, {6, "\x3f", 0, 0, err_need_more},
        {7, "\x7f", 0, 0, err_need_more}, {8, "\xff", 0, 0, err_need_more},
        // Ignoring top bits:
        {5, "\xff\x9a\x0a", 1337, 3, nullptr}, {5, "\x9f\x9a\x0a", 1337, 3, nullptr}, {5, "\xbf\x9a\x0a", 1337, 3, nullptr},
        // Extra byte:
        {5, "\xbf\x9a\x0a\x02", 1337, 3, nullptr},
        // Short a byte:
        {5, "\xbf\x9a", 0, 0, err_need_more},
        // Integer overflow:
        {1, overflow, 0, 0, err_varint_overflow},
    };
    for (auto& tt : tests) {
        std::span<const byte> p = byteSpan(tt.p);
        uint64 i = 0;
        gopp::error err = readVarInt(tt.n, p, i);
        EXPECT_EQ(err, tt.err) << hex(tt.p);
        if (!err) {
            EXPECT_EQ(i, tt.i) << hex(tt.p);
        }
        EXPECT_EQ(tt.p.size() - p.size(), tt.consumed) << hex(tt.p);
    }
}

TEST(Hpack, HuffmanFuzzCrash) {
    // Originally reported at https://github.com/bradfitz/http2/issues/56
    auto [got, err] = huffmanDecodeToString(byteSpan("00\x91\xff\xff\xff\xff\xc8"));
    EXPECT_EQ(got, "");
    EXPECT_EQ(err, err_invalid_huffman);
}

TEST(Hpack, EmitEnabled) {
    std::string buf;
    Encoder enc(&buf);
    enc.writeField(P("foo", "bar"));
    enc.writeField(P("foo", "bar"));
    int num_callback = 0;
    Decoder* dec = nullptr;
    Decoder d(8 << 20, [&](const HeaderField&) {
        num_callback++;
        dec->setEmitEnabled(false);
    });
    dec = &d;
    EXPECT_TRUE(d.emitEnabled());
    EXPECT_FALSE(d.Write(byteSpan(buf)).second);
    EXPECT_EQ(num_callback, 1);
    EXPECT_FALSE(d.emitEnabled());
}

TEST(Hpack, SlowIncrementalDecode) {
    // Go skips this one as too slow under -race; with 16 KiB fields it is
    // fast enough, and still feeds a field one byte at a time.
    std::string buf;
    Encoder enc(&buf);
    HeaderField hf{string(std::string(1 << 14, 'k')), string(std::string(1 << 14, 'v'))};
    enc.writeField(hf);
    int count = 0;
    Decoder dec(4096, [&](const HeaderField& got) {
        count++;
        EXPECT_EQ(got.name, hf.name);
        EXPECT_EQ(got.value, hf.value);
    });
    for (std::size_t i = 0; i < buf.size(); i++) dec.Write(byteSpan(std::string_view(buf).substr(i, 1)));
    EXPECT_EQ(count, 1);
}

TEST(Hpack, SaveBufLimit) {
    constexpr int max_str = 1 << 10;
    std::vector<HeaderField> got;
    Decoder dec(4096, [&](const HeaderField& hf) { got.push_back(hf); });
    dec.setMaxStringLength(max_str);
    std::string frag(1, '\0');  // literal without indexing, new name
    appendVarInt(frag, 7, 3);
    frag += "foo";
    appendVarInt(frag, 7, 3);
    frag += "bar";
    ASSERT_FALSE(dec.Write(byteSpan(frag)).second);
    EXPECT_EQ(got, std::vector<HeaderField>{P("foo", "bar")});

    frag.assign(1, '\0');
    appendVarInt(frag, 7, max_str * 3);
    frag += std::string(max_str * 3, '\0');
    EXPECT_EQ(dec.Write(byteSpan(frag)).second, err_string_length);
}

TEST(Hpack, DynamicSizeUpdate) {
    std::string buf;
    Encoder enc(&buf);
    enc.setMaxDynamicTableSize(255);
    enc.writeField(P("foo", "bar"));
    Decoder d(4096, [](const HeaderField&) {});
    ASSERT_FALSE(d.Write(byteSpan(buf)).second);
    d.Close();
    // A new header block.
    ASSERT_FALSE(d.Write(byteSpan(buf)).second);
    // Must fail: the size update is not at the beginning of the block.
    EXPECT_TRUE(d.Write(byteSpan(buf)).second);
}

// encode_test.Go

TEST(Hpack, EncoderTableSizeUpdate) {
    struct test {
        uint32 size1, size2;
        const char* want_hex;
    };
    test tests[] = {
        {2048, 4096, "3fe10f 3fe11f 82"},  // two table size updates (2048 and 4096)
        {16384, 2048, "3fe10f 82"},        // one (2048)
    };
    for (auto& tt : tests) {
        std::string buf;
        Encoder e(&buf);
        e.setMaxDynamicTableSize(tt.size1);
        e.setMaxDynamicTableSize(tt.size2);
        ASSERT_FALSE(e.writeField(P(":method", "GET")));
        EXPECT_EQ(hex(buf), hex(dehex(tt.want_hex)));
    }
}

TEST(Hpack, EncoderWriteField) {
    std::string buf;
    Encoder e(&buf);
    std::vector<HeaderField> got;
    Decoder d(4 << 10, [&](const HeaderField& f) { got.push_back(f); });
    std::vector<std::vector<HeaderField>> tests = {
        {P(":method", "GET"), P(":scheme", "http"), P(":path", "/"), P(":authority", "www.example.com")},
        {P(":method", "GET"), P(":scheme", "http"), P(":path", "/"), P(":authority", "www.example.com"),
         P("cache-control", "no-cache")},
        {P(":method", "GET"), P(":scheme", "https"), P(":path", "/index.html"), P(":authority", "www.example.com"),
         P("custom-key", "custom-value")},
    };
    for (std::size_t i = 0; i < tests.size(); i++) {
        buf.clear();
        got.clear();
        for (auto& hf : tests[i]) ASSERT_FALSE(e.writeField(hf));
        EXPECT_FALSE(d.Write(byteSpan(buf)).second) << i;
        EXPECT_EQ(got, tests[i]) << i;
    }
}

TEST(Hpack, EncoderSearchTable) {
    Encoder e(nullptr);
    test_access::dyn(e).add(P("foo", "bar"));
    test_access::dyn(e).add(P("blake", "miz"));
    test_access::dyn(e).add(P(":method", "GET"));
    struct test {
        HeaderField hf;
        uint64 want_i;
        bool want_match;
    };
    test tests[] = {
        // Name and value match.
        {P("foo", "bar"), 61 + 3, true},
        {P("blake", "miz"), 61 + 2, true},
        {P(":method", "GET"), 2, true},
        // Only the name matches because the field is sensitive; any
        // ":method" entry would do, and the last in the static table wins.
        {HeaderField{":method", "GET", true}, 3, false},
        // Only the name matches.
        {P("foo", "..."), 61 + 3, false},
        {P("blake", "..."), 61 + 2, false},
        {P(":method", "..."), 3, false},
        // None match.
        {P("foo-", "bar"), 0, false},
    };
    for (auto& tt : tests) {
        auto [i, match] = test_access::searchTable(e, tt.hf);
        EXPECT_EQ(i, tt.want_i) << tt.hf.String();
        EXPECT_EQ(match, tt.want_match) << tt.hf.String();
    }
}

TEST(Hpack, AppendVarInt) {
    struct test {
        uint8 n;
        uint64 i;
        std::string want;
    };
    test tests[] = {
        // Fits in a byte:
        {1, 0, std::string(1, '\0')}, {2, 2, "\x02"}, {3, 6, "\x06"}, {4, 14, "\x0e"},
        {5, 30, "\x1e"}, {6, 62, "\x3e"}, {7, 126, "\x7e"}, {8, 254, "\xfe"},
        // Multiple bytes:
        {5, 1337, "\x1f\x9a\x0a"},
    };
    for (auto& tt : tests) {
        std::string got;
        appendVarInt(got, tt.n, tt.i);
        EXPECT_EQ(hex(got), hex(tt.want)) << tt.i;
    }
}

TEST(Hpack, EncoderSetMaxDynamicTableSize) {
    std::string buf;
    Encoder e(&buf);
    e.setMaxDynamicTableSize(2048);
    EXPECT_TRUE(test_access::tableSizeUpdate(e));
    EXPECT_EQ(test_access::minSize(e), 2048u);
    EXPECT_EQ(e.maxDynamicTableSize(), 2048u);
    // 16384 is still limited to 4096.
    e.setMaxDynamicTableSize(16384);
    EXPECT_TRUE(test_access::tableSizeUpdate(e));
    EXPECT_EQ(test_access::minSize(e), 2048u);
    EXPECT_EQ(e.maxDynamicTableSize(), 4096u);
}

TEST(Hpack, EncoderSetMaxDynamicTableSizeLimit) {
    Encoder e(nullptr);
    // 4095 < the initial table size: the size is truncated to 4095.
    e.setMaxDynamicTableSizeLimit(4095);
    EXPECT_EQ(e.maxDynamicTableSize(), 4095u);
    EXPECT_EQ(test_access::maxSizeLimit(e), 4095u);
    EXPECT_TRUE(test_access::tableSizeUpdate(e));
    // Limited to the limit.
    e.setMaxDynamicTableSize(16384);
    EXPECT_EQ(e.maxDynamicTableSize(), 4095u);
    // A larger limit doesn't change the size.
    e.setMaxDynamicTableSizeLimit(8192);
    EXPECT_EQ(e.maxDynamicTableSize(), 4095u);
    EXPECT_EQ(test_access::maxSizeLimit(e), 8192u);
}

