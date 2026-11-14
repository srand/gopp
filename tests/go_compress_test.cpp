// Ported from Go 1.27.3's compress/gzip, compress/zlib, compress/flate,
// hash/crc32 and hash/adler32 tests, with Go's test names. Left out: the
// tests of Go's own compressor and Huffman coder (go++'s is zlib's), of
// marshaled hash states, allocations, and the benchmarks. Go's testdata
// files (gettysburg.txt, e.txt, pi.txt) are replaced by generated text.
#include <gopp/bytes.hpp>
#include <gopp/compress/flate.hpp>
#include <gopp/compress/gzip.hpp>
#include <gopp/compress/zlib.hpp>
#include <gopp/hash/adler32.hpp>
#include <gopp/hash/crc32.hpp>
#include <gopp/io.hpp>
#include <gopp/runtime.hpp>
#include <gopp/strings.hpp>
#include <gopp/time.hpp>

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace gopp;

namespace {

std::string repeat(std::string_view s, std::size_t n) {
    std::string r;
    r.reserve(s.size() * n);
    for (std::size_t i = 0; i < n; i++) r += s;
    return r;
}

std::string str(std::span<const byte> b) { return std::string(asStringView(b)); }

// ---------------------------------------------------------------------------
// gzip

struct gunzipTest {
    std::string name;
    std::string desc;
    std::string raw;
    std::vector<byte> gzip;
    error err;
};

std::vector<gunzipTest> gunzipTests() {
    return {
    { // has 1 empty fixed-huffman block
        "empty.txt",
        "empty.txt",
        "",
        std::vector<byte>{
            0x1f, 0x8b, 0x08, 0x08, 0xf7, 0x5e, 0x14, 0x4a,
            0x00, 0x03, 0x65, 0x6d, 0x70, 0x74, 0x79, 0x2e,
            0x74, 0x78, 0x74, 0x00, 0x03, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        },
        nullptr,
    },
    {
        "",
        "empty - with no file name",
        "",
        std::vector<byte>{
            0x1f, 0x8b, 0x08, 0x00, 0x00, 0x09, 0x6e, 0x88,
            0x00, 0xff, 0x01, 0x00, 0x00, 0xff, 0xff, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        },
        nullptr,
    },
    { // has 1 non-empty fixed huffman block
        "hello.txt",
        "hello.txt",
        "hello world\n",
        std::vector<byte>{
            0x1f, 0x8b, 0x08, 0x08, 0xc8, 0x58, 0x13, 0x4a,
            0x00, 0x03, 0x68, 0x65, 0x6c, 0x6c, 0x6f, 0x2e,
            0x74, 0x78, 0x74, 0x00, 0xcb, 0x48, 0xcd, 0xc9,
            0xc9, 0x57, 0x28, 0xcf, 0x2f, 0xca, 0x49, 0xe1,
            0x02, 0x00, 0x2d, 0x3b, 0x08, 0xaf, 0x0c, 0x00,
            0x00, 0x00,
        },
        nullptr,
    },
    { // concatenation
        "hello.txt",
        "hello.txt x2",
        "hello world\n"
            "hello world\n",
        std::vector<byte>{
            0x1f, 0x8b, 0x08, 0x08, 0xc8, 0x58, 0x13, 0x4a,
            0x00, 0x03, 0x68, 0x65, 0x6c, 0x6c, 0x6f, 0x2e,
            0x74, 0x78, 0x74, 0x00, 0xcb, 0x48, 0xcd, 0xc9,
            0xc9, 0x57, 0x28, 0xcf, 0x2f, 0xca, 0x49, 0xe1,
            0x02, 0x00, 0x2d, 0x3b, 0x08, 0xaf, 0x0c, 0x00,
            0x00, 0x00,
            0x1f, 0x8b, 0x08, 0x08, 0xc8, 0x58, 0x13, 0x4a,
            0x00, 0x03, 0x68, 0x65, 0x6c, 0x6c, 0x6f, 0x2e,
            0x74, 0x78, 0x74, 0x00, 0xcb, 0x48, 0xcd, 0xc9,
            0xc9, 0x57, 0x28, 0xcf, 0x2f, 0xca, 0x49, 0xe1,
            0x02, 0x00, 0x2d, 0x3b, 0x08, 0xaf, 0x0c, 0x00,
            0x00, 0x00,
        },
        nullptr,
    },
    { // has a fixed huffman block with some length-distance pairs
        "shesells.txt",
        "shesells.txt",
        "she sells seashells by the seashore\n",
        std::vector<byte>{
            0x1f, 0x8b, 0x08, 0x08, 0x72, 0x66, 0x8b, 0x4a,
            0x00, 0x03, 0x73, 0x68, 0x65, 0x73, 0x65, 0x6c,
            0x6c, 0x73, 0x2e, 0x74, 0x78, 0x74, 0x00, 0x2b,
            0xce, 0x48, 0x55, 0x28, 0x4e, 0xcd, 0xc9, 0x29,
            0x06, 0x92, 0x89, 0xc5, 0x19, 0x60, 0x56, 0x52,
            0xa5, 0x42, 0x09, 0x58, 0x18, 0x28, 0x90, 0x5f,
            0x94, 0xca, 0x05, 0x00, 0x76, 0xb0, 0x3b, 0xeb,
            0x24, 0x00, 0x00, 0x00,
        },
        nullptr,
    },
    { // has dynamic huffman blocks
        "gettysburg",
        "gettysburg",
        "  Four score and seven years ago our fathers brought forth on\n"
            "this continent, a new nation, conceived in Liberty, and dedicated\n"
            "to the proposition that all men are created equal.\n"
            "  Now we are engaged in a great Civil War, testing whether that\n"
            "nation, or any nation so conceived and so dedicated, can long\n"
            "endure.\n"
            "  We are met on a great battle-field of that war.\n"
            "  We have come to dedicate a portion of that field, as a final\n"
            "resting place for those who here gave their lives that that\n"
            "nation might live.  It is altogether fitting and proper that\n"
            "we should do this.\n"
            "  But, in a larger sense, we can not dedicate — we can not\n"
            "consecrate — we can not hallow — this ground.\n"
            "  The brave men, living and dead, who struggled here, have\n"
            "consecrated it, far above our poor power to add or detract.\n"
            "The world will little note, nor long remember what we say here,\n"
            "but it can never forget what they did here.\n"
            "  It is for us the living, rather, to be dedicated here to the\n"
            "unfinished work which they who fought here have thus far so\n"
            "nobly advanced.  It is rather for us to be here dedicated to\n"
            "the great task remaining before us — that from these honored\n"
            "dead we take increased devotion to that cause for which they\n"
            "gave the last full measure of devotion —\n"
            "  that we here highly resolve that these dead shall not have\n"
            "died in vain — that this nation, under God, shall have a new\n"
            "birth of freedom — and that government of the people, by the\n"
            "people, for the people, shall not perish from this earth.\n"
            "\n"
            "Abraham Lincoln, November 19, 1863, Gettysburg, Pennsylvania\n",
        std::vector<byte>{
            0x1f, 0x8b, 0x08, 0x08, 0xd1, 0x12, 0x2b, 0x4a,
            0x00, 0x03, 0x67, 0x65, 0x74, 0x74, 0x79, 0x73,
            0x62, 0x75, 0x72, 0x67, 0x00, 0x65, 0x54, 0xcd,
            0x6e, 0xd4, 0x30, 0x10, 0xbe, 0xfb, 0x29, 0xe6,
            0x01, 0x42, 0xa5, 0x0a, 0x09, 0xc1, 0x11, 0x90,
            0x40, 0x48, 0xa8, 0xe2, 0x80, 0xd4, 0xf3, 0x24,
            0x9e, 0x24, 0x56, 0xbd, 0x9e, 0xc5, 0x76, 0x76,
            0x95, 0x1b, 0x0f, 0xc1, 0x13, 0xf2, 0x24, 0x7c,
            0x63, 0x77, 0x9b, 0x4a, 0x5c, 0xaa, 0x6e, 0x6c,
            0xcf, 0x7c, 0x7f, 0x33, 0x44, 0x5f, 0x74, 0xcb,
            0x54, 0x26, 0xcd, 0x42, 0x9c, 0x3c, 0x15, 0xb9,
            0x48, 0xa2, 0x5d, 0x38, 0x17, 0xe2, 0x45, 0xc9,
            0x4e, 0x67, 0xae, 0xab, 0xe0, 0xf7, 0x98, 0x75,
            0x5b, 0xd6, 0x4a, 0xb3, 0xe6, 0xba, 0x92, 0x26,
            0x57, 0xd7, 0x50, 0x68, 0xd2, 0x54, 0x43, 0x92,
            0x54, 0x07, 0x62, 0x4a, 0x72, 0xa5, 0xc4, 0x35,
            0x68, 0x1a, 0xec, 0x60, 0x92, 0x70, 0x11, 0x4f,
            0x21, 0xd1, 0xf7, 0x30, 0x4a, 0xae, 0xfb, 0xd0,
            0x9a, 0x78, 0xf1, 0x61, 0xe2, 0x2a, 0xde, 0x55,
            0x25, 0xd4, 0xa6, 0x73, 0xd6, 0xb3, 0x96, 0x60,
            0xef, 0xf0, 0x9b, 0x2b, 0x71, 0x8c, 0x74, 0x02,
            0x10, 0x06, 0xac, 0x29, 0x8b, 0xdd, 0x25, 0xf9,
            0xb5, 0x71, 0xbc, 0x73, 0x44, 0x0f, 0x7a, 0xa5,
            0xab, 0xb4, 0x33, 0x49, 0x0b, 0x2f, 0xbd, 0x03,
            0xd3, 0x62, 0x17, 0xe9, 0x73, 0xb8, 0x84, 0x48,
            0x8f, 0x9c, 0x07, 0xaa, 0x52, 0x00, 0x6d, 0xa1,
            0xeb, 0x2a, 0xc6, 0xa0, 0x95, 0x76, 0x37, 0x78,
            0x9a, 0x81, 0x65, 0x7f, 0x46, 0x4b, 0x45, 0x5f,
            0xe1, 0x6d, 0x42, 0xe8, 0x01, 0x13, 0x5c, 0x38,
            0x51, 0xd4, 0xb4, 0x38, 0x49, 0x7e, 0xcb, 0x62,
            0x28, 0x1e, 0x3b, 0x82, 0x93, 0x54, 0x48, 0xf1,
            0xd2, 0x7d, 0xe4, 0x5a, 0xa3, 0xbc, 0x99, 0x83,
            0x44, 0x4f, 0x3a, 0x77, 0x36, 0x57, 0xce, 0xcf,
            0x2f, 0x56, 0xbe, 0x80, 0x90, 0x9e, 0x84, 0xea,
            0x51, 0x1f, 0x8f, 0xcf, 0x90, 0xd4, 0x60, 0xdc,
            0x5e, 0xb4, 0xf7, 0x10, 0x0b, 0x26, 0xe0, 0xff,
            0xc4, 0xd1, 0xe5, 0x67, 0x2e, 0xe7, 0xc8, 0x93,
            0x98, 0x05, 0xb8, 0xa8, 0x45, 0xc0, 0x4d, 0x09,
            0xdc, 0x84, 0x16, 0x2b, 0x0d, 0x9a, 0x21, 0x53,
            0x04, 0x8b, 0xd2, 0x0b, 0xbd, 0xa2, 0x4c, 0xa7,
            0x60, 0xee, 0xd9, 0xe1, 0x1d, 0xd1, 0xb7, 0x4a,
            0x30, 0x8f, 0x63, 0xd5, 0xa5, 0x8b, 0x33, 0x87,
            0xda, 0x1a, 0x18, 0x79, 0xf3, 0xe3, 0xa6, 0x17,
            0x94, 0x2e, 0xab, 0x6e, 0xa0, 0xe3, 0xcd, 0xac,
            0x50, 0x8c, 0xca, 0xa7, 0x0d, 0x76, 0x37, 0xd1,
            0x23, 0xe7, 0x05, 0x57, 0x8b, 0xa4, 0x22, 0x83,
            0xd9, 0x62, 0x52, 0x25, 0xad, 0x07, 0xbb, 0xbf,
            0xbf, 0xff, 0xbc, 0xfa, 0xee, 0x20, 0x73, 0x91,
            0x29, 0xff, 0x7f, 0x02, 0x71, 0x62, 0x84, 0xb5,
            0xf6, 0xb5, 0x25, 0x6b, 0x41, 0xde, 0x92, 0xb7,
            0x76, 0x3f, 0x91, 0x91, 0x31, 0x1b, 0x41, 0x84,
            0x62, 0x30, 0x0a, 0x37, 0xa4, 0x5e, 0x18, 0x3a,
            0x99, 0x08, 0xa5, 0xe6, 0x6d, 0x59, 0x22, 0xec,
            0x33, 0x39, 0x86, 0x26, 0xf5, 0xab, 0x66, 0xc8,
            0x08, 0x20, 0xcf, 0x0c, 0xd7, 0x47, 0x45, 0x21,
            0x0b, 0xf6, 0x59, 0xd5, 0xfe, 0x5c, 0x8d, 0xaa,
            0x12, 0x7b, 0x6f, 0xa1, 0xf0, 0x52, 0x33, 0x4f,
            0xf5, 0xce, 0x59, 0xd3, 0xab, 0x66, 0x10, 0xbf,
            0x06, 0xc4, 0x31, 0x06, 0x73, 0xd6, 0x80, 0xa2,
            0x78, 0xc2, 0x45, 0xcb, 0x03, 0x65, 0x39, 0xc9,
            0x09, 0xd1, 0x06, 0x04, 0x33, 0x1a, 0x5a, 0xf1,
            0xde, 0x01, 0xb8, 0x71, 0x83, 0xc4, 0xb5, 0xb3,
            0xc3, 0x54, 0x65, 0x33, 0x0d, 0x5a, 0xf7, 0x9b,
            0x90, 0x7c, 0x27, 0x1f, 0x3a, 0x58, 0xa3, 0xd8,
            0xfd, 0x30, 0x5f, 0xb7, 0xd2, 0x66, 0xa2, 0x93,
            0x1c, 0x28, 0xb7, 0xe9, 0x1b, 0x0c, 0xe1, 0x28,
            0x47, 0x26, 0xbb, 0xe9, 0x7d, 0x7e, 0xdc, 0x96,
            0x10, 0x92, 0x50, 0x56, 0x7c, 0x06, 0xe2, 0x27,
            0xb4, 0x08, 0xd3, 0xda, 0x7b, 0x98, 0x34, 0x73,
            0x9f, 0xdb, 0xf6, 0x62, 0xed, 0x31, 0x41, 0x13,
            0xd3, 0xa2, 0xa8, 0x4b, 0x3a, 0xc6, 0x1d, 0xe4,
            0x2f, 0x8c, 0xf8, 0xfb, 0x97, 0x64, 0xf4, 0xb6,
            0x2f, 0x80, 0x5a, 0xf3, 0x56, 0xe0, 0x40, 0x50,
            0xd5, 0x19, 0xd0, 0x1e, 0xfc, 0xca, 0xe5, 0xc9,
            0xd4, 0x60, 0x00, 0x81, 0x2e, 0xa3, 0xcc, 0xb6,
            0x52, 0xf0, 0xb4, 0xdb, 0x69, 0x99, 0xce, 0x7a,
            0x32, 0x4c, 0x08, 0xed, 0xaa, 0x10, 0x10, 0xe3,
            0x6f, 0xee, 0x99, 0x68, 0x95, 0x9f, 0x04, 0x71,
            0xb2, 0x49, 0x2f, 0x62, 0xa6, 0x5e, 0xb4, 0xef,
            0x02, 0xed, 0x4f, 0x27, 0xde, 0x4a, 0x0f, 0xfd,
            0xc1, 0xcc, 0xdd, 0x02, 0x8f, 0x08, 0x16, 0x54,
            0xdf, 0xda, 0xca, 0xe0, 0x82, 0xf1, 0xb4, 0x31,
            0x7a, 0xa9, 0x81, 0xfe, 0x90, 0xb7, 0x3e, 0xdb,
            0xd3, 0x35, 0xc0, 0x20, 0x80, 0x33, 0x46, 0x4a,
            0x63, 0xab, 0xd1, 0x0d, 0x29, 0xd2, 0xe2, 0x84,
            0xb8, 0xdb, 0xfa, 0xe9, 0x89, 0x44, 0x86, 0x7c,
            0xe8, 0x0b, 0xe6, 0x02, 0x6a, 0x07, 0x9b, 0x96,
            0xd0, 0xdb, 0x2e, 0x41, 0x4c, 0xa1, 0xd5, 0x57,
            0x45, 0x14, 0xfb, 0xe3, 0xa6, 0x72, 0x5b, 0x87,
            0x6e, 0x0c, 0x6d, 0x5b, 0xce, 0xe0, 0x2f, 0xe2,
            0x21, 0x81, 0x95, 0xb0, 0xe8, 0xb6, 0x32, 0x0b,
            0xb2, 0x98, 0x13, 0x52, 0x5d, 0xfb, 0xec, 0x63,
            0x17, 0x8a, 0x9e, 0x23, 0x22, 0x36, 0xee, 0xcd,
            0xda, 0xdb, 0xcf, 0x3e, 0xf1, 0xc7, 0xf1, 0x01,
            0x12, 0x93, 0x0a, 0xeb, 0x6f, 0xf2, 0x02, 0x15,
            0x96, 0x77, 0x5d, 0xef, 0x9c, 0xfb, 0x88, 0x91,
            0x59, 0xf9, 0x84, 0xdd, 0x9b, 0x26, 0x8d, 0x80,
            0xf9, 0x80, 0x66, 0x2d, 0xac, 0xf7, 0x1f, 0x06,
            0xba, 0x7f, 0xff, 0xee, 0xed, 0x40, 0x5f, 0xa5,
            0xd6, 0xbd, 0x8c, 0x5b, 0x46, 0xd2, 0x7e, 0x48,
            0x4a, 0x65, 0x8f, 0x08, 0x42, 0x60, 0xf7, 0x0f,
            0xb9, 0x16, 0x0b, 0x0c, 0x1a, 0x06, 0x00, 0x00,
        },
        nullptr,
    },
    { // has 1 non-empty fixed huffman block then garbage
        "hello.txt",
        "hello.txt + garbage",
        "hello world\n",
        std::vector<byte>{
            0x1f, 0x8b, 0x08, 0x08, 0xc8, 0x58, 0x13, 0x4a,
            0x00, 0x03, 0x68, 0x65, 0x6c, 0x6c, 0x6f, 0x2e,
            0x74, 0x78, 0x74, 0x00, 0xcb, 0x48, 0xcd, 0xc9,
            0xc9, 0x57, 0x28, 0xcf, 0x2f, 0xca, 0x49, 0xe1,
            0x02, 0x00, 0x2d, 0x3b, 0x08, 0xaf, 0x0c, 0x00,
            0x00, 0x00, 'g', 'a', 'r', 'b', 'a', 'g', 'e', '!', '!', '!',
        },
        gzip::ErrHeader,
    },
    { // has 1 non-empty fixed huffman block not enough header
        "hello.txt",
        "hello.txt + garbage",
        "hello world\n",
        std::vector<byte>{
            0x1f, 0x8b, 0x08, 0x08, 0xc8, 0x58, 0x13, 0x4a,
            0x00, 0x03, 0x68, 0x65, 0x6c, 0x6c, 0x6f, 0x2e,
            0x74, 0x78, 0x74, 0x00, 0xcb, 0x48, 0xcd, 0xc9,
            0xc9, 0x57, 0x28, 0xcf, 0x2f, 0xca, 0x49, 0xe1,
            0x02, 0x00, 0x2d, 0x3b, 0x08, 0xaf, 0x0c, 0x00,
            0x00, 0x00, 0x1f,
        },
        io::ErrUnexpectedEOF,
    },
    { // has 1 non-empty fixed huffman block but corrupt checksum
        "hello.txt",
        "hello.txt + corrupt checksum",
        "hello world\n",
        std::vector<byte>{
            0x1f, 0x8b, 0x08, 0x08, 0xc8, 0x58, 0x13, 0x4a,
            0x00, 0x03, 0x68, 0x65, 0x6c, 0x6c, 0x6f, 0x2e,
            0x74, 0x78, 0x74, 0x00, 0xcb, 0x48, 0xcd, 0xc9,
            0xc9, 0x57, 0x28, 0xcf, 0x2f, 0xca, 0x49, 0xe1,
            0x02, 0x00, 0xff, 0xff, 0xff, 0xff, 0x0c, 0x00,
            0x00, 0x00,
        },
        gzip::ErrChecksum,
    },
    { // has 1 non-empty fixed huffman block but corrupt size
        "hello.txt",
        "hello.txt + corrupt size",
        "hello world\n",
        std::vector<byte>{
            0x1f, 0x8b, 0x08, 0x08, 0xc8, 0x58, 0x13, 0x4a,
            0x00, 0x03, 0x68, 0x65, 0x6c, 0x6c, 0x6f, 0x2e,
            0x74, 0x78, 0x74, 0x00, 0xcb, 0x48, 0xcd, 0xc9,
            0xc9, 0x57, 0x28, 0xcf, 0x2f, 0xca, 0x49, 0xe1,
            0x02, 0x00, 0x2d, 0x3b, 0x08, 0xaf, 0xff, 0x00,
            0x00, 0x00,
        },
        gzip::ErrChecksum,
    },
    {
        "f1l3n4m3.tXt",
        "header with all fields used",
        "",
        std::vector<byte>{
            0x1f, 0x8b, 0x08, 0x1e, 0x70, 0xf0, 0xf9, 0x4a,
            0x00, 0xaa, 0x09, 0x00, 0x7a, 0x7a, 0x05, 0x00,
            0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x31, 0x6c,
            0x33, 0x6e, 0x34, 0x6d, 0x33, 0x2e, 0x74, 0x58,
            0x74, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
            0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e,
            0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16,
            0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e,
            0x1f, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26,
            0x27, 0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e,
            0x2f, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36,
            0x37, 0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e,
            0x3f, 0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46,
            0x47, 0x48, 0x49, 0x4a, 0x4b, 0x4c, 0x4d, 0x4e,
            0x4f, 0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56,
            0x57, 0x58, 0x59, 0x5a, 0x5b, 0x5c, 0x5d, 0x5e,
            0x5f, 0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66,
            0x67, 0x68, 0x69, 0x6a, 0x6b, 0x6c, 0x6d, 0x6e,
            0x6f, 0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76,
            0x77, 0x78, 0x79, 0x7a, 0x7b, 0x7c, 0x7d, 0x7e,
            0x7f, 0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86,
            0x87, 0x88, 0x89, 0x8a, 0x8b, 0x8c, 0x8d, 0x8e,
            0x8f, 0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96,
            0x97, 0x98, 0x99, 0x9a, 0x9b, 0x9c, 0x9d, 0x9e,
            0x9f, 0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6,
            0xa7, 0xa8, 0xa9, 0xaa, 0xab, 0xac, 0xad, 0xae,
            0xaf, 0xb0, 0xb1, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6,
            0xb7, 0xb8, 0xb9, 0xba, 0xbb, 0xbc, 0xbd, 0xbe,
            0xbf, 0xc0, 0xc1, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6,
            0xc7, 0xc8, 0xc9, 0xca, 0xcb, 0xcc, 0xcd, 0xce,
            0xcf, 0xd0, 0xd1, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6,
            0xd7, 0xd8, 0xd9, 0xda, 0xdb, 0xdc, 0xdd, 0xde,
            0xdf, 0xe0, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6,
            0xe7, 0xe8, 0xe9, 0xea, 0xeb, 0xec, 0xed, 0xee,
            0xef, 0xf0, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6,
            0xf7, 0xf8, 0xf9, 0xfa, 0xfb, 0xfc, 0xfd, 0xfe,
            0xff, 0x00, 0x92, 0xfd, 0x01, 0x00, 0x00, 0xff,
            0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00,
        },
        nullptr,
    },
    {
        "",
        "truncated gzip file amid raw-block",
        "hello",
        std::vector<byte>{
            0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff,
            0x00, 0x0c, 0x00, 0xf3, 0xff, 0x68, 0x65, 0x6c, 0x6c, 0x6f,
        },
        io::ErrUnexpectedEOF,
    },
    {
        "",
        "truncated gzip file amid fixed-block",
        "He",
        std::vector<byte>{
            0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff,
            0xf2, 0x48, 0xcd,
        },
        io::ErrUnexpectedEOF,
    },
    {
        "hello.txt",
        "gzip header with truncated name",
        "hello world\n",
        std::vector<byte>{
            0x1f, 0x8b, 0x08, 0x08, 0xc8, 0x58, 0x13, 0x4a,
            0x00, 0x03, 0x68, 0x65, 0x6c, 0x6c, 0x6f, 0x2e,
            0x74, 0x78, 0x74, 0x00, 0xcb, 0x48, 0xcd, 0xc9,
            0xc9, 0x57, 0x28, 0xcf, 0x2f, 0xca, 0x49, 0xe1,
            0x02, 0x00, 0x2d, 0x3b, 0x08, 0xaf, 0x0c, 0x00,
            0x00, 0x00,
            0x1f, 0x8b, 0x08, 0x08, 0x00, 0x00, 0x00, 0x00,
            0x00, 0xff, 0x01,
        },
        io::ErrUnexpectedEOF,
    },
    {
        "",
        "gzip header with truncated comment",
        "hello world\n",
        std::vector<byte>{
            0x1f, 0x8b, 0x08, 0x10, 0xc8, 0x58, 0x13, 0x4a,
            0x00, 0x03, 0x68, 0x65, 0x6c, 0x6c, 0x6f, 0x2e,
            0x74, 0x78, 0x74, 0x00, 0xcb, 0x48, 0xcd, 0xc9,
            0xc9, 0x57, 0x28, 0xcf, 0x2f, 0xca, 0x49, 0xe1,
            0x02, 0x00, 0x2d, 0x3b, 0x08, 0xaf, 0x0c, 0x00,
            0x00, 0x00,
            0x1f, 0x8b, 0x08, 0x10, 0x00, 0x00, 0x00, 0x00,
            0x00, 0xff, 0x01,
        },
        io::ErrUnexpectedEOF,
    },
};
}


TEST(GoGzip, Decompressor) {
    // Keep resetting this reader. It is intended behavior that Reader.Reset
    // can be called on a zero-value Reader and be the equivalent as if
    // NewReader was used instead.
    auto r1 = make_ptr<gzip::Reader>();
    bytes::Buffer b;
    for (const auto& tt : gunzipTests()) {
        // Test NewReader.
        auto in = bytes::NewReader(slice<byte>::of(tt.gzip));
        auto [r2, err] = gzip::NewReader(in);
        if (err) {
            ADD_FAILURE() << tt.desc << ": NewReader: " << err;
            continue;
        }
        EXPECT_EQ(r2->Name, tt.name) << tt.desc;
        b.Reset();
        auto [n, cerr] = io::Copy(b, *r2);
        EXPECT_EQ(cerr, tt.err) << tt.desc << ": io.Copy: " << cerr << " want " << tt.err;
        EXPECT_EQ(b.String(), tt.raw) << tt.desc;
        r2->Close();

        // Test Reader.Reset.
        in = bytes::NewReader(slice<byte>::of(tt.gzip));
        if (auto rerr = r1->Reset(in)) {
            ADD_FAILURE() << tt.desc << ": Reset: " << rerr;
            continue;
        }
        EXPECT_EQ(r1->Name, tt.name) << tt.desc;
        b.Reset();
        std::tie(n, cerr) = io::Copy(b, *r1);
        EXPECT_EQ(cerr, tt.err) << tt.desc << ": io.Copy: " << cerr << " want " << tt.err;
        EXPECT_EQ(b.String(), tt.raw) << tt.desc;
    }
}

TEST(GoGzip, MultistreamFalse) {
    // Find concatenation test.
    gunzipTest tt;
    for (const auto& t : gunzipTests()) {
        if (t.desc.ends_with(" x2")) tt = t;
    }
    ASSERT_FALSE(tt.gzip.empty()) << "cannot find hello.txt x2 in gunzip tests";
    auto br = bytes::NewReader(slice<byte>::of(tt.gzip));
    auto rp = make_ptr<gzip::Reader>();
    auto& r = *rp;
    ASSERT_FALSE(r.Reset(br)) << "first reset";

    // Expect two streams with "hello world\n", then real EOF.
    const std::string hello = "hello world\n";

    r.Multistream(false);
    auto [data, err] = io::ReadAll(r);
    ASSERT_EQ(str(data), hello);
    ASSERT_FALSE(err);

    ASSERT_FALSE(r.Reset(br)) << "second reset";
    r.Multistream(false);
    std::tie(data, err) = io::ReadAll(r);
    ASSERT_EQ(str(data), hello);
    ASSERT_FALSE(err);

    ASSERT_EQ(r.Reset(br), io::EOF_) << "third reset";
}

TEST(GoGzip, NilStream) {
    // Go liberally interprets RFC 1952 section 2.2 to mean that a gzip file
    // consist of zero or more members. Thus, we test that a nil stream is
    // okay.
    auto [_, err] = gzip::NewReader(bytes::NewReader({}));
    ASSERT_EQ(err, io::EOF_) << "NewReader(nil) on empty stream: got " << err;
}

TEST(GoGzip, TruncatedStreams) {
    struct test {
        std::string name;
        std::string data;
    };
    std::vector<test> cases = {
        {"original", std::string("\x1f\x8b\b\x04\x00\tn\x88\x00\xff\a\x00" "foo bar\xcbH\xcd\xc9\xc9\xd7Q(\xcf/\xcaI\x01\x04:r\xab\xff\f\x00\x00\x00", 41)},
        {"truncated name", std::string("\x1f\x8b\x08\x10\x00\x00\x00\x00\x00\xff\x01", 11)},
        {"truncated comment", std::string("\x1f\x8b\x08\x08\x00\x00\x00\x00\x00\xff\x01", 11)},
    };
    // Intentionally iterate starting with at least one byte in the stream.
    for (const auto& tc : cases) {
        for (std::size_t i = 1; i < tc.data.size(); i++) {
            auto [r, err] = gzip::NewReader(strings::NewReader(string(tc.data.substr(0, i))));
            if (err) {
                EXPECT_EQ(err, io::ErrUnexpectedEOF) << "NewReader(" << tc.name << "-" << i << ") on truncated stream";
                continue;
            }
            auto [_, cerr] = io::Copy(io::Discard(), *r);
            EXPECT_EQ(cerr, io::ErrUnexpectedEOF) << "io.Copy(" << tc.name << "-" << i << " of " << tc.data.size() << ") on truncated stream: " << cerr;
        }
    }
}

TEST(GoGzip, CVE202230631) {
    const std::string empty("\x1f\x8b\x08\x00\xa7\x8f\x43\x62\x00\x03\x03\x00\x00\x00\x00\x00\x00\x00\x00\x00", 20);
    auto r = bytes::NewReader(toBytes(repeat(empty, 4000000)));
    auto [z, err] = gzip::NewReader(r);
    ASSERT_FALSE(err) << "NewReader: got " << err;
    // Prior to CVE-2022-30631 fix, this would cause an unrecoverable panic
    // due to stack exhaustion.
    byte buf[10];
    auto [_, rerr] = z->Read(buf);
    EXPECT_EQ(rerr, io::EOF_) << "Reader.Read: got " << rerr;
}

TEST(GoGzip, Empty) {
    auto buf = make_ptr<bytes::Buffer>();
    ASSERT_FALSE(gzip::NewWriter(buf)->Close()) << "Writer.Close";
    auto [r, err] = gzip::NewReader(buf);
    ASSERT_FALSE(err) << "NewReader: " << err;
    EXPECT_EQ(r->Comment, "");
    EXPECT_TRUE(r->Extra.isNil());
    EXPECT_TRUE(r->ModTime.IsZero());
    EXPECT_EQ(r->Name, "");
    EXPECT_EQ(r->OS, 255);
    auto [b, rerr] = io::ReadAll(*r);
    ASSERT_FALSE(rerr) << "ReadAll: " << rerr;
    ASSERT_EQ(b.len(), 0u);
    ASSERT_FALSE(r->Close()) << "Reader.Close";
}

// TestRoundTrip tests that gzipping and then gunzipping is the identity
// function.
TEST(GoGzip, RoundTrip) {
    auto buf = make_ptr<bytes::Buffer>();
    auto w = gzip::NewWriter(buf);
    w->Comment = "comment";
    w->Extra = toBytes("extra");
    w->ModTime = time::Unix(100000000, 0);
    w->Name = "name";
    ASSERT_FALSE(io::WriteString(*w, "payload").second) << "Write";
    ASSERT_FALSE(w->Close()) << "Writer.Close";

    auto [r, err] = gzip::NewReader(buf);
    ASSERT_FALSE(err) << "NewReader: " << err;
    auto [b, rerr] = io::ReadAll(*r);
    ASSERT_FALSE(rerr) << "ReadAll: " << rerr;
    ASSERT_EQ(str(b), "payload");
    ASSERT_EQ(r->Comment, "comment");
    ASSERT_EQ(str(r->Extra), "extra");
    ASSERT_EQ(r->ModTime.Unix(), 100000000);
    ASSERT_EQ(r->Name, "name");
    ASSERT_FALSE(r->Close()) << "Reader.Close";
}

// TestLatin1RoundTrip tests that metadata that is representable in Latin-1
// survives a round trip. (Go's TestLatin1, of the unexported functions, is
// covered by the "Látin-1" case.)
TEST(GoGzip, Latin1RoundTrip) {
    struct test {
        std::string name;
        bool ok;
    };
    std::vector<test> testCases = {
        {"", true},
        {"ASCII is OK", true},
        {std::string("unless it contains a NUL\x00", 25), false},
        {std::string("no matter where \x00 occurs", 25), false},
        {std::string("\x00\x00\x00", 3), false},
        {"Látin-1 also passes (U+00E1)", true},
        {"but LĀtin Extended-A (U+0100) does not", false},
        {"neither does 日本語", false},
        {"invalid UTF-8 also \xff" "ails", false},
        {std::string("\x00 as does Látin-1 with NUL", 27), false},
    };
    for (const auto& tc : testCases) {
        auto buf = make_ptr<bytes::Buffer>();
        auto w = gzip::NewWriter(buf);
        w->Name = string(tc.name);
        auto err = w->Close();
        if (bool(!err) != tc.ok) {
            ADD_FAILURE() << "Writer.Close: name = " << strconv::Quote(tc.name) << ", err = " << err;
            continue;
        }
        if (!tc.ok) continue;
        auto [r, rerr] = gzip::NewReader(buf);
        if (rerr) {
            ADD_FAILURE() << "NewReader: " << rerr;
            continue;
        }
        if (auto [_, aerr] = io::ReadAll(*r); aerr) {
            ADD_FAILURE() << "ReadAll: " << aerr;
            continue;
        }
        EXPECT_EQ(r->Name, tc.name);
        EXPECT_FALSE(r->Close());
    }
}

TEST(GoGzip, WriterFlush) {
    auto buf = make_ptr<bytes::Buffer>();
    auto w = gzip::NewWriter(buf);
    w->Comment = "comment";
    w->Extra = toBytes("extra");
    w->ModTime = time::Unix(100000000, 0);
    w->Name = "name";

    auto n0 = buf->Len();
    ASSERT_EQ(n0, 0) << "buffer size before writes";
    ASSERT_FALSE(w->Flush());
    auto n1 = buf->Len();
    ASSERT_NE(n1, 0) << "no data after first flush";
    io::WriteString(*w, "x");
    auto n2 = buf->Len();
    ASSERT_EQ(n1, n2) << "after writing a single byte, size changed; want no change";
    ASSERT_FALSE(w->Flush());
    auto n3 = buf->Len();
    ASSERT_NE(n2, n3) << "Flush didn't flush any data";
    ASSERT_FALSE(w->Close());
}

// Multiple gzip files concatenated form a valid gzip file.
TEST(GoGzip, Concat) {
    auto buf = make_ptr<bytes::Buffer>();
    auto w = gzip::NewWriter(buf);
    io::WriteString(*w, "hello ");
    w->Close();
    w = gzip::NewWriter(buf);
    io::WriteString(*w, "world\n");
    w->Close();
    auto [r, err] = gzip::NewReader(buf);
    ASSERT_FALSE(err) << err;
    auto [data, rerr] = io::ReadAll(*r);
    ASSERT_EQ(str(data), "hello world\n");
    ASSERT_FALSE(rerr);
}

TEST(GoGzip, WriterReset) {
    auto buf = make_ptr<bytes::Buffer>();
    auto buf2 = make_ptr<bytes::Buffer>();
    auto z = gzip::NewWriter(buf);
    io::WriteString(*z, "hello world");
    z->Close();
    z->Reset(buf2);
    io::WriteString(*z, "hello world");
    z->Close();
    EXPECT_EQ(buf->String(), buf2->String());
}

struct limitedWriter final : io::Writer {
    isize N;
    explicit limitedWriter(isize n) : N(n) {}
    std::pair<isize, error> Write(std::span<const byte> p) override {
        if (isize n = N; n < isize(p.size())) {
            N = 0;
            return {n, io::ErrShortWrite};
        }
        N -= isize(p.size());
        return {isize(p.size()), nullptr};
    }
};

// Write should never return more bytes than the input slice.
TEST(GoGzip, LimitedWrite) {
    for (int lim = 2; lim < 20; lim++) {
        auto z = gzip::NewWriter(make_ptr<limitedWriter>(lim));
        EXPECT_LE(io::WriteString(*z, "a").first, 1);
        z->Reset(make_ptr<limitedWriter>(lim));
        z->Comment = "comment";
        z->Extra = toBytes("extra");
        z->ModTime = time::Now();
        z->Name = "name";
        z->OS = 1;
        EXPECT_LE(io::WriteString(*z, "a").first, 1);
    }
}

// A gzip member followed by other data: a Multistream(false) reader stops
// at its end, and the rest can be read from the source (go++'s, as Go's
// does with an io.ByteReader).
TEST(Gzip, SourceAfterMember) {
    auto buf = make_ptr<bytes::Buffer>();
    auto w = gzip::NewWriter(buf);
    io::WriteString(*w, "compressed");
    w->Close();
    buf->WriteString("trailing data");
    auto src = bytes::NewReader(bytes::Clone(buf->Bytes()));
    auto [r, err] = gzip::NewReader(src);
    ASSERT_FALSE(err);
    r->Multistream(false);
    auto [data, rerr] = io::ReadAll(*r);
    ASSERT_FALSE(rerr);
    EXPECT_EQ(str(data), "compressed");
    auto [rest, _] = io::ReadAll(*src);
    EXPECT_EQ(str(rest), "trailing data");
}

// ---------------------------------------------------------------------------
// zlib

struct zlibTest {
    std::string desc;
    std::string raw;
    std::vector<byte> compressed;
    std::vector<byte> dict;
    error err;
};

// Compare-to-golden test data was generated by the ZLIB example program at
// https://www.zlib.net/zpipe.c
std::vector<zlibTest> zlibTests() {
    return {
        {"truncated empty", "", {}, {}, io::ErrUnexpectedEOF},
        {"truncated dict", "", {0x78, 0xbb}, {0x00}, io::ErrUnexpectedEOF},
        {"truncated checksum", "",
         {0x78, 0xbb, 0x00, 0x01, 0x00, 0x01, 0xca, 0x48, 0xcd, 0xc9, 0xc9, 0xd7, 0x51, 0x28, 0xcf, 0x2f, 0xca, 0x49,
          0x01, 0x04, 0x00, 0x00, 0xff, 0xff},
         {0x00}, io::ErrUnexpectedEOF},
        {"empty", "", {0x78, 0x9c, 0x03, 0x00, 0x00, 0x00, 0x00, 0x01}, {}, nullptr},
        {"goodbye", "goodbye, world",
         {0x78, 0x9c, 0x4b, 0xcf, 0xcf, 0x4f, 0x49, 0xaa, 0x4c, 0xd5, 0x51, 0x28, 0xcf, 0x2f, 0xca, 0x49, 0x01, 0x00,
          0x28, 0xa5, 0x05, 0x5e},
         {}, nullptr},
        {"bad header (CINFO)", "", {0x88, 0x98, 0x03, 0x00, 0x00, 0x00, 0x00, 0x01}, {}, zlib::ErrHeader},
        {"bad header (FCHECK)", "", {0x78, 0x9f, 0x03, 0x00, 0x00, 0x00, 0x00, 0x01}, {}, zlib::ErrHeader},
        {"bad checksum", "", {0x78, 0x9c, 0x03, 0x00, 0x00, 0x00, 0x00, 0xff}, {}, zlib::ErrChecksum},
        {"not enough data", "", {0x78, 0x9c, 0x03, 0x00, 0x00, 0x00}, {}, io::ErrUnexpectedEOF},
        {"excess data is silently ignored", "", {0x78, 0x9c, 0x03, 0x00, 0x00, 0x00, 0x00, 0x01, 0x78, 0x9c, 0xff}, {},
         nullptr},
        {"dictionary", "Hello, World!\n",
         {0x78, 0xbb, 0x1c, 0x32, 0x04, 0x27, 0xf3, 0x00, 0xb1, 0x75, 0x20, 0x1c, 0x45, 0x2e, 0x00, 0x24, 0x12, 0x04,
          0x74},
         {0x48, 0x65, 0x6c, 0x6c, 0x6f, 0x20, 0x57, 0x6f, 0x72, 0x6c, 0x64, 0x0a}, nullptr},
        {"wrong dictionary", "",
         {0x78, 0xbb, 0x1c, 0x32, 0x04, 0x27, 0xf3, 0x00, 0xb1, 0x75, 0x20, 0x1c, 0x45, 0x2e, 0x00, 0x24, 0x12, 0x04,
          0x74},
         {0x48, 0x65, 0x6c, 0x6c}, zlib::ErrDictionary},
        {"truncated zlib stream amid raw-block", "hello",
         {0x78, 0x9c, 0x00, 0x0c, 0x00, 0xf3, 0xff, 0x68, 0x65, 0x6c, 0x6c, 0x6f}, {}, io::ErrUnexpectedEOF},
        {"truncated zlib stream amid fixed-block", "He", {0x78, 0x9c, 0xf2, 0x48, 0xcd}, {}, io::ErrUnexpectedEOF},
    };
}

TEST(GoZlib, Decompressor) {
    bytes::Buffer b;
    for (const auto& tt : zlibTests()) {
        auto in = bytes::NewReader(slice<byte>::of(tt.compressed));
        auto [zr, err] = zlib::NewReaderDict(in, tt.dict);
        if (err) {
            EXPECT_EQ(err, tt.err) << tt.desc << ": NewReader: " << err;
            continue;
        }
        b.Reset();
        auto [n, cerr] = io::Copy(b, *zr);
        if (cerr) {
            EXPECT_EQ(cerr, tt.err) << tt.desc << ": io.Copy: " << cerr;
            continue;
        }
        EXPECT_EQ(b.String(), tt.raw) << tt.desc;
        byte one[1];
        auto [n2, rerr] = zr->Read(one);
        EXPECT_EQ(n2, 0) << tt.desc;
        EXPECT_EQ(rerr, io::EOF_) << tt.desc << ": Read() = " << rerr;
        EXPECT_FALSE(zr->Close()) << tt.desc << ": Close()";
    }
}

// Text for the writer tests: Go's read gettysburg.txt, e.txt and pi.txt.
std::vector<std::pair<std::string, std::string>> testFiles() {
    std::string digits;
    uint32 x = 12345;
    for (int i = 0; i < 100000; i++) {
        x = x * 1103515245 + 12345;
        digits += char('0' + (x >> 16) % 10);
    }
    return {
        {"gettysburg", repeat("  Four score and seven years ago our fathers brought forth on\n"
                              "this continent, a new nation, conceived in Liberty, and dedicated\n"
                              "to the proposition that all men are created equal.\n", 20)},
        {"digits", digits},
    };
}

void testLevelDict(const std::string& fn, const std::string& b0, int level, const std::string& d) {
    slice<byte> dict;
    if (!d.empty()) dict = toBytes(d);
    auto [piper, pipew] = io::Pipe();
    go([pipew, b0, level, dict, fn, d] {
        auto [zlibw, err] = zlib::NewWriterLevelDict(pipew, level, dict);
        if (err) {
            ADD_FAILURE() << fn << " (level=" << level << ", dict=" << d << "): " << err;
            pipew->Close();
            return;
        }
        if (auto [_, werr] = io::WriteString(*zlibw, b0); werr) ADD_FAILURE() << fn << ": " << werr;
        zlibw->Close();
        pipew->Close();
    });
    auto [zlibr, err] = zlib::NewReaderDict(piper, dict);
    ASSERT_FALSE(err) << fn << " (level=" << level << ", dict=" << d << "): " << err;
    auto [b1, err1] = io::ReadAll(*zlibr);
    ASSERT_FALSE(err1) << fn << " (level=" << level << ", dict=" << d << "): " << err1;
    ASSERT_TRUE(str(b1) == b0) << fn << " (level=" << level << ", dict=" << d << "): mismatch";
    zlibr->Close();
}

TEST(GoZlib, Writer) {
    std::string b = "test a reasonable sized string that can be compressed";
    testLevelDict("#0", b, zlib::DefaultCompression, "");
    testLevelDict("#0", b, zlib::NoCompression, "");
    testLevelDict("#0", b, zlib::HuffmanOnly, "");
    for (int level = zlib::BestSpeed; level <= zlib::BestCompression; level++) testLevelDict("#0", b, level, "");
}

TEST(GoZlib, WriterBig) {
    for (const auto& [fn, b0] : testFiles()) {
        testLevelDict(fn, b0, zlib::DefaultCompression, "");
        testLevelDict(fn, b0, zlib::NoCompression, "");
        testLevelDict(fn, b0, zlib::HuffmanOnly, "");
        for (int level = zlib::BestSpeed; level <= zlib::BestCompression; level++) testLevelDict(fn, b0, level, "");
    }
}

TEST(GoZlib, WriterDict) {
    const std::string dictionary = "0123456789.";
    for (const auto& [fn, b0] : testFiles()) {
        testLevelDict(fn, b0, zlib::DefaultCompression, dictionary);
        testLevelDict(fn, b0, zlib::NoCompression, dictionary);
        testLevelDict(fn, b0, zlib::HuffmanOnly, dictionary);
        for (int level = zlib::BestSpeed; level <= zlib::BestCompression; level++) testLevelDict(fn, b0, level, dictionary);
    }
}

void testFileLevelDictReset(const std::string& fn, const std::string& b0, int level, slice<byte> dict) {
    auto buf = make_ptr<bytes::Buffer>();
    auto [zlibw, err] = zlib::NewWriterLevelDict(buf, level, dict);
    if (!err) err = io::WriteString(*zlibw, b0).second;
    if (!err) err = zlibw->Close();
    ASSERT_FALSE(err) << fn << " (level=" << level << "): " << err;
    auto out = buf->String();

    auto buf2 = make_ptr<bytes::Buffer>();
    zlibw->Reset(buf2);
    err = io::WriteString(*zlibw, b0).second;
    if (!err) err = zlibw->Close();
    ASSERT_FALSE(err) << fn << " (level=" << level << "): " << err;
    EXPECT_TRUE(buf2->String() == out) << fn << " (level=" << level << "): different output after reset";
}

TEST(GoZlib, WriterReset) {
    const std::string dictionary = "0123456789.";
    for (const auto& [fn, b0] : testFiles()) {
        testFileLevelDictReset(fn, b0, zlib::NoCompression, {});
        testFileLevelDictReset(fn, b0, zlib::DefaultCompression, {});
        testFileLevelDictReset(fn, b0, zlib::HuffmanOnly, {});
        testFileLevelDictReset(fn, b0, zlib::NoCompression, toBytes(dictionary));
        testFileLevelDictReset(fn, b0, zlib::DefaultCompression, toBytes(dictionary));
        testFileLevelDictReset(fn, b0, zlib::HuffmanOnly, toBytes(dictionary));
        for (int level = zlib::BestSpeed; level <= zlib::BestCompression; level++) {
            testFileLevelDictReset(fn, b0, level, {});
        }
    }
}

TEST(GoZlib, WriterDictIsUsed) {
    std::string input = "Lorem ipsum dolor sit amet, consectetur adipisicing elit, sed do eiusmod tempor incididunt ut labore et dolore magna aliqua.";
    auto buf = make_ptr<bytes::Buffer>();
    auto [compressor, err] = zlib::NewWriterLevelDict(buf, zlib::BestCompression, toBytes(input));
    ASSERT_FALSE(err) << "error in NewWriterLevelDict: " << err;
    io::WriteString(*compressor, input);
    compressor->Close();
    constexpr isize expectedMaxSize = 25;
    EXPECT_LE(buf->Len(), expectedMaxSize) << "result too large. Is the dictionary being used?";
}

// ---------------------------------------------------------------------------
// flate

TEST(GoFlate, Reset) {
    std::vector<std::string> ss = {"lorem ipsum izzle fo rizzle", "the quick brown fox jumped over"};
    std::vector<ptr<bytes::Buffer>> deflated = {make_ptr<bytes::Buffer>(), make_ptr<bytes::Buffer>()};
    for (std::size_t i = 0; i < ss.size(); i++) {
        auto [w, _] = flate::NewWriter(deflated[i], 1);
        io::WriteString(*w, ss[i]);
        w->Close();
    }
    std::vector<bytes::Buffer> inflated(2);
    auto f = flate::NewReader(deflated[0]);
    io::Copy(inflated[0], *f);
    dynamic_pointer_cast<flate::Resetter>(f)->Reset(deflated[1], {});
    io::Copy(inflated[1], *f);
    f->Close();
    for (std::size_t i = 0; i < ss.size(); i++) EXPECT_EQ(inflated[i].String(), ss[i]) << "inflated[" << i << "]";
}

TEST(GoFlate, ReaderTruncated) {
    std::vector<std::pair<std::string, std::string>> vectors = {
        {std::string("\x00", 1), ""},
        {std::string("\x00\f", 2), ""},
        {std::string("\x00\f\x00", 3), ""},
        {std::string("\x00\f\x00\xf3\xff", 5), ""},
        {std::string("\x00\f\x00\xf3\xffhello", 10), "hello"},
        {std::string("\x00\f\x00\xf3\xffhello, world", 17), "hello, world"},
        {"\x02", ""},
        {"\xf2H\xcd", "He"},
        {"\xf2H͙0a\u0084\t", "Hel\x90\x90\x90\x90\x90"},
        {std::string("\xf2H͙0a\u0084\t\x00", 10), "Hel\x90\x90\x90\x90\x90"},
    };
    int i = 0;
    for (const auto& [input, output] : vectors) {
        auto zr = flate::NewReader(strings::NewReader(string(input)));
        auto [b, err] = io::ReadAll(*zr);
        EXPECT_EQ(err, io::ErrUnexpectedEOF) << "test " << i << ", error mismatch: got " << err;
        EXPECT_EQ(str(b), output) << "test " << i << ", output mismatch";
        i++;
    }
}

TEST(GoFlate, ResetDict) {
    std::string d = "the lorem fox";
    auto dict = toBytes(d);
    std::vector<std::string> ss = {"lorem ipsum izzle fo rizzle", "the quick brown fox jumped over"};
    std::vector<ptr<bytes::Buffer>> deflated;
    for (const auto& s : ss) {
        deflated.push_back(make_ptr<bytes::Buffer>());
        auto [w, _] = flate::NewWriterDict(deflated.back(), flate::DefaultCompression, dict);
        io::WriteString(*w, s);
        w->Close();
    }
    std::vector<bytes::Buffer> inflated(ss.size());
    auto f = flate::NewReader(nullptr);
    for (std::size_t i = 0; i < inflated.size(); i++) {
        dynamic_pointer_cast<flate::Resetter>(f)->Reset(deflated[i], dict);
        io::Copy(inflated[i], *f);
    }
    f->Close();
    for (std::size_t i = 0; i < ss.size(); i++) EXPECT_EQ(inflated[i].String(), ss[i]) << "inflated[" << i << "]";
}

TEST(GoFlate, DeflateInflateString) {
    for (const auto& [fn, gold] : testFiles()) {
        for (int level = -2; level <= 9; level++) {
            auto buf = make_ptr<bytes::Buffer>();
            auto [w, err] = flate::NewWriter(buf, level);
            ASSERT_FALSE(err) << level;
            io::WriteString(*w, gold);
            ASSERT_FALSE(w->Close());
            auto r = flate::NewReader(buf);
            auto [out, rerr] = io::ReadAll(*r);
            ASSERT_FALSE(rerr) << fn << " level " << level << ": " << rerr;
            ASSERT_TRUE(str(out) == gold) << fn << " level " << level;
        }
    }
}

TEST(GoFlate, ReaderDict) {
    const std::string dict = "hello world";
    const std::string text = "hello again world";
    auto b = make_ptr<bytes::Buffer>();
    auto [w, err] = flate::NewWriter(b, 5);
    ASSERT_FALSE(err) << "NewWriter: " << err;
    io::WriteString(*w, dict);
    w->Flush();
    b->Reset();
    io::WriteString(*w, text);
    w->Close();
    auto r = flate::NewReaderDict(b, toBytes(dict));
    auto [data, rerr] = io::ReadAll(*r);
    ASSERT_FALSE(rerr) << rerr;
    EXPECT_EQ(str(data), text);
}

TEST(GoFlate, WriterDict) {
    const std::string dict = "hello world";
    const std::string text = "hello again world";
    auto b = make_ptr<bytes::Buffer>();
    auto [w, err] = flate::NewWriter(b, 5);
    ASSERT_FALSE(err) << "NewWriter: " << err;
    io::WriteString(*w, dict);
    w->Flush();
    b->Reset();
    io::WriteString(*w, text);
    w->Close();

    auto b1 = make_ptr<bytes::Buffer>();
    auto [w1, err1] = flate::NewWriterDict(b1, 5, toBytes(dict));
    ASSERT_FALSE(err1);
    io::WriteString(*w1, text);
    w1->Close();
    EXPECT_EQ(b1->String(), b->String()) << "writer wrote different output with a dictionary";
}

// See https://golang.org/issue/2508
TEST(GoFlate, Regression2508) {
    auto [w, err] = flate::NewWriter(make_ptr<bytes::Buffer>(), 1);
    ASSERT_FALSE(err) << "NewWriter: " << err;
    std::vector<byte> buf(1024);
    for (int i = 0; i < 131072; i++) {
        if (auto [_, werr] = w->Write(buf); werr) FAIL() << "writer failed: " << werr;
    }
    w->Close();
}

struct errorWriter final : io::Writer {
    int N;
    explicit errorWriter(int n) : N(n) {}
    std::pair<isize, error> Write(std::span<const byte> p) override {
        if (N <= 0) return {0, io::ErrClosedPipe};
        N--;
        return {isize(p.size()), nullptr};
    }
};

struct countWriter final : io::Writer {
    int writes = 0;
    std::pair<isize, error> Write(std::span<const byte> p) override {
        writes++;
        return {isize(p.size()), nullptr};
    }
};

// Test if errors from the underlying writer is passed upwards. (zlib
// buffers more than Go's compressor and writes this text in a few writes,
// so the failing write may come only at Flush or Close, and there may be
// fewer writes than 'fail': go++ checks that some call fails if there are
// more writes, and that all calls fail after it.)
TEST(GoFlate, WriteError) {
    std::string in = repeat("0123456789", 10000);
    for (int l = 0; l < 10; l++) {
        auto cw = make_ptr<countWriter>();
        auto [cwf, _] = flate::NewWriter(cw, l);
        io::Copy(*cwf, *strings::NewReader(string(in)));
        cwf->Flush();
        cwf->Close();
        for (int fail = 1; fail <= 32; fail *= 2) {
            if (fail >= cw->writes) continue;
            // Fail after 'fail' writes.
            auto ew = make_ptr<errorWriter>(fail);
            auto [w, err] = flate::NewWriter(ew, l);
            ASSERT_FALSE(err) << "NewWriter: level " << l << ": " << err;
            auto [n, cerr] = io::Copy(*w, *strings::NewReader(string(in)));
            auto ferr = w->Flush();
            auto clerr = w->Close();
            if (!cerr && !ferr && !clerr) {
                ADD_FAILURE() << "Level " << l << ": Expected an error, writer was " << ew->N;
                continue;
            }
            auto [n2, err2] = io::WriteString(*w, "0123456789");
            EXPECT_TRUE(n2 == 0 && err2) << "Level " << l << ": Expected an error";
            EXPECT_TRUE(w->Flush()) << "Level " << l << ": Expected an error on flush";
            EXPECT_TRUE(w->Close()) << "Level " << l << ": Expected an error on close";
            w->Reset(make_ptr<errorWriter>(1 << 30));
            auto [n3, err3] = io::WriteString(*w, "0123456789");
            EXPECT_FALSE(err3) << "Level " << l << ": Got unexpected error after reset";
            EXPECT_NE(n3, 0) << "Level " << l << ": Got no data written after reset";
            (void)n;
        }
    }
}

// ---------------------------------------------------------------------------
// crc32, adler32

struct crcTest {
    uint32 ieee, castagnoli;
    std::string in;
};

std::vector<crcTest> crcGolden() {
    return {
        {0x0, 0x0, ""},
        {0xe8b7be43, 0xc1d04330, "a"},
        {0x9e83486d, 0xe2a22936, "ab"},
        {0x352441c2, 0x364b3fb7, "abc"},
        {0xed82cd11, 0x92c80a31, "abcd"},
        {0x8587d865, 0xc450d697, "abcde"},
        {0x4b8e39ef, 0x53bceff1, "abcdef"},
        {0x312a6aa6, 0xe627f441, "abcdefg"},
        {0xaeef2a50, 0xa9421b7, "abcdefgh"},
        {0x8da988af, 0x2ddc99fc, "abcdefghi"},
        {0x3981703a, 0xe6599437, "abcdefghij"},
        {0x6b9cdfe7, 0xb2cc01fe, "Discard medicine more than two years old."},
        {0xc90ef73f, 0xe28207f, "He who has a shady past knows that nice guys finish last."},
        {0xb902341f, 0xbe93f964, "I wouldn't marry him with a ten foot pole."},
        {0x42080e8, 0x9e3be0c3, "Free! Free!/A trip/to Mars/for 900/empty jars/Burma Shave"},
        {0x154c6d11, 0xf505ef04, "The days of the digital watch are numbered.  -Tom Stoppard"},
        {0x4c418325, 0x85d3dc82, "Nepal premier won't resign."},
        {0x33955150, 0xc5142380, "For every action there is an equal and opposite government program."},
        {0x26216a4b, 0x75eb77dd, "His money is twice tainted: 'taint yours and 'taint mine."},
        {0x1abbe45e, 0x91ebe9f7, "There is no reason for any individual to have a computer in their home. -Ken Olsen, 1977"},
        {0xc89a94f7, 0xf0b1168e, "It's a tiny change to the code and not completely disgusting. - Bob Manchek"},
        {0xab3abe14, 0x572b74e2, "size:  a.out:  bad magic"},
        {0xbab102b6, 0x8a58a6d5, "The major problem is with sendmail.  -Mark Horton"},
        {0x999149d7, 0x9c426c50, "Give me a rock, paper and scissors and I will move the world.  CCFestoon"},
        {0x6d52a33c, 0x735400a4, "If the enemy is within range, then so are you."},
        {0x90631e8d, 0xbec49c95, "It's well we cannot hear the screams/That we create in others' dreams."},
        {0x78309130, 0xa95a2079, "You remind me of a TV show, but that's all right: I watch it anyway."},
        {0x7d0a377f, 0xde2e65c5, "C is as portable as Stonehedge!!"},
        {0x8c79fd79, 0x297a88ed, "Even if I could be Shakespeare, I think I should still choose to be Faraday. - A. Huxley"},
        {0xa20b7167, 0x66ed1d8b, "The fugacity of a constituent in a mixture of gases at a given temperature is proportional to its mole fraction.  Lewis-Randall Rule"},
        {0x8e0bb443, 0xdcded527, "How can you write a big system without C++?  -Paul Glick"},
        {0x1010dab0, 0x8a11661f, repeat("01234567", 1024)},
        {0x772d04d7, 0x5a6f5c45, repeat("a", 1024+65)},
    };
}

TEST(GoCRC32, Golden) {
    auto castagnoliTab = crc32::MakeTable(crc32::Castagnoli);
    for (const auto& g : crcGolden()) {
        EXPECT_EQ(crc32::ChecksumIEEE(as_bytes(g.in)), g.ieee) << "IEEE(" << g.in << ")";
        EXPECT_EQ(crc32::Checksum(as_bytes(g.in), castagnoliTab), g.castagnoli) << "Castagnoli(" << g.in << ")";
        // The checksum of the halves, through a Hash32.
        for (auto [tab, want] : {std::pair{crc32::IEEETable, g.ieee}, std::pair{castagnoliTab, g.castagnoli}}) {
            auto h = crc32::New(tab);
            io::WriteString(*h, std::string_view(g.in).substr(0, g.in.size() / 2));
            io::WriteString(*h, std::string_view(g.in).substr(g.in.size() / 2));
            EXPECT_EQ(h->Sum32(), want) << g.in;
            auto sum = h->Sum({});
            EXPECT_EQ(sum.len(), 4u);
            EXPECT_EQ(uint32(sum[0]) << 24 | uint32(sum[1]) << 16 | uint32(sum[2]) << 8 | sum[3], want);
        }
    }
}

// TestSimple tests the simple generic algorithm, through tables made from
// the polynomials by MakeTable, here Koopman's against a bitwise CRC.
TEST(GoCRC32, Simple) {
    auto tab = crc32::MakeTable(crc32::Koopman);
    EXPECT_EQ(tab, crc32::MakeTable(crc32::Koopman)) << "tables are made once";
    for (const auto& g : crcGolden()) {
        uint32 crc = ~uint32(0);
        for (unsigned char c : g.in) {
            crc ^= c;
            for (int k = 0; k < 8; k++) crc = (crc & 1) ? (crc >> 1) ^ crc32::Koopman : crc >> 1;
        }
        EXPECT_EQ(crc32::Checksum(as_bytes(g.in), tab), ~crc) << g.in;
    }
}

TEST(GoCRC32, CastagnoliRace) {
    // MakeTable(Castagnoli) racing with an IEEE hash.
    auto ieee = crc32::NewIEEE();
    go([] { crc32::MakeTable(crc32::Castagnoli); });
    io::WriteString(*ieee, "hello");
}

struct adlerTest {
    uint32 out;
    std::string in;
};

std::vector<adlerTest> adlerGolden() {
    return {
        {0x00000001, ""},
        {0x00620062, "a"},
        {0x012600c4, "ab"},
        {0x024d0127, "abc"},
        {0x03d8018b, "abcd"},
        {0x05c801f0, "abcde"},
        {0x081e0256, "abcdef"},
        {0x0adb02bd, "abcdefg"},
        {0x0e000325, "abcdefgh"},
        {0x118e038e, "abcdefghi"},
        {0x158603f8, "abcdefghij"},
        {0x3f090f02, "Discard medicine more than two years old."},
        {0x46d81477, "He who has a shady past knows that nice guys finish last."},
        {0x40ee0ee1, "I wouldn't marry him with a ten foot pole."},
        {0x16661315, "Free! Free!/A trip/to Mars/for 900/empty jars/Burma Shave"},
        {0x5b2e1480, "The days of the digital watch are numbered.  -Tom Stoppard"},
        {0x8c3c09ea, "Nepal premier won't resign."},
        {0x45ac18fd, "For every action there is an equal and opposite government program."},
        {0x53c61462, "His money is twice tainted: 'taint yours and 'taint mine."},
        {0x7e511e63, "There is no reason for any individual to have a computer in their home. -Ken Olsen, 1977"},
        {0xe4801a6a, "It's a tiny change to the code and not completely disgusting. - Bob Manchek"},
        {0x61b507df, "size:  a.out:  bad magic"},
        {0xb8631171, "The major problem is with sendmail.  -Mark Horton"},
        {0x8b5e1904, "Give me a rock, paper and scissors and I will move the world.  CCFestoon"},
        {0x7cc6102b, "If the enemy is within range, then so are you."},
        {0x700318e7, "It's well we cannot hear the screams/That we create in others' dreams."},
        {0x1e601747, "You remind me of a TV show, but that's all right: I watch it anyway."},
        {0xb55b0b09, "C is as portable as Stonehedge!!"},
        {0x39111dd0, "Even if I could be Shakespeare, I think I should still choose to be Faraday. - A. Huxley"},
        {0x91dd304f, "The fugacity of a constituent in a mixture of gases at a given temperature is proportional to its mole fraction.  Lewis-Randall Rule"},
        {0x2e5d1316, "How can you write a big system without C++?  -Paul Glick"},
        {0xd0201df6, "'Invariant assertions' is the most elegant programming technique!  -Tom Szymanski"},
        {0x211297c8, repeat("\xff", 5548) + std::string("8")},
        {0xbaa198c8, repeat("\xff", 5549) + std::string("9")},
        {0x553499be, repeat("\xff", 5550) + std::string("0")},
        {0xf0c19abe, repeat("\xff", 5551) + std::string("1")},
        {0x8d5c9bbe, repeat("\xff", 5552) + std::string("2")},
        {0x2af69cbe, repeat("\xff", 5553) + std::string("3")},
        {0xc9809dbe, repeat("\xff", 5554) + std::string("4")},
        {0x69189ebe, repeat("\xff", 5555) + std::string("5")},
        {0x86af0001, std::string(100000, '\0')},
        {0x79660b4d, repeat("a", 100000)},
        {0x110588ee, repeat("ABCDEFGHIJKLMNOPQRSTUVWXYZ", 10000)},
    };
}

TEST(GoAdler32, Golden) {
    for (const auto& g : adlerGolden()) {
        // The simple implementation.
        uint32 s1 = 1, s2 = 0;
        for (unsigned char x : g.in) {
            s1 = (s1 + x) % 65521;
            s2 = (s2 + s1) % 65521;
        }
        ASSERT_EQ(s2 << 16 | s1, g.out) << "simple implementation";
        EXPECT_EQ(adler32::Checksum(as_bytes(g.in)), g.out) << g.in.substr(0, 100);
        auto h = adler32::New();
        io::WriteString(*h, std::string_view(g.in).substr(0, g.in.size() / 2));
        io::WriteString(*h, std::string_view(g.in).substr(g.in.size() / 2));
        EXPECT_EQ(h->Sum32(), g.out);
    }
}

}  // namespace
