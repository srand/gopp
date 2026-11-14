// pnginfo: takes PNG files apart. A PNG is a signature and a list of
// chunks, each a big-endian length, a type, the data and a CRC-32 of the
// type and data; the IHDR chunk describes the image, and the IDAT chunks
// are one zlib stream of the pixels, a filter byte before each row.
//
//     $ pnginfo /usr/share/pixmaps/sympylogo.png
//     sympylogo.png:
//       IHDR      13 B  crc ok
//       IDAT   48113 B  crc ok
//       IEND       0 B  crc ok
//       469x312, 8-bit RGB+alpha, not interlaced
//       pixels: 48113 B in 1 IDAT, 585624 B inflated (12.2x)
//
// encoding/binary reads the numbers (binary::Read fills the IHDR struct,
// field by field, by reflection), hash/crc32 checks the chunks,
// io::MultiReader joins the IDAT chunks into one stream for
// compress/zlib, and io::Copy into io::Discard counts what comes out.
#include <gopp/bytes.hpp>
#include <gopp/compress/zlib.hpp>
#include <gopp/encoding/binary.hpp>
#include <gopp/fmt.hpp>
#include <gopp/hash/crc32.hpp>
#include <gopp/io.hpp>
#include <gopp/os.hpp>
#include <gopp/path.hpp>

#include <array>
#include <vector>

using namespace gopp;

constexpr std::string_view signature = "\x89PNG\r\n\x1a\n";

// The IHDR chunk's data, as it is in the file.
struct IHDR {
    uint32 Width;
    uint32 Height;
    uint8 BitDepth;
    uint8 ColorType;
    uint8 Compression;
    uint8 Filter;
    uint8 Interlace;
};

std::string_view colorName(uint8 colorType) {
    switch (colorType) {
        case 0: return "gray";
        case 2: return "RGB";
        case 3: return "palette";
        case 4: return "gray+alpha";
        case 6: return "RGB+alpha";
        default: return "unknown color type";
    }
}

// The bytes of the inflated pixel data: a filter byte and the pixels of
// each row (for images that aren't interlaced).
int64 rawSize(const IHDR& h) {
    static constexpr int channels[] = {1, 0, 3, 1, 2, 0, 4};
    int bitsPerPixel = h.BitDepth * (h.ColorType < 7 ? channels[h.ColorType] : 0);
    return int64(h.Height) * (1 + (int64(h.Width) * bitsPerPixel + 7) / 8);
}

error inspect(const string& name) {
    auto [data, err] = os::ReadFile(name);
    if (err) return err;
    auto r = bytes::NewReader(data);

    std::array<byte, 8> sig;
    if (auto err = binary::Read(*r, binary::BigEndian, sig); err || asStringView(sig) != signature) {
        return fmt::Errorf("{}: not a PNG file", name);
    }

    IHDR header{};
    std::vector<ptr<io::Reader>> idat;
    int64 compressed = 0;
    fmt::Println("{}:", path::Base(name));
    for (;;) {
        uint32 length;
        std::array<byte, 4> type;
        if (auto err = binary::Read(*r, binary::BigEndian, length)) return fmt::Errorf("{}: {}", name, err);
        if (auto err = binary::Read(*r, binary::BigEndian, type)) return fmt::Errorf("{}: {}", name, err);
        if (length > uint32(r->Len())) return fmt::Errorf("{}: chunk {} is cut short", name, asStringView(type));
        auto chunk = slice<byte>::make(length);
        io::ReadFull(*r, chunk);
        uint32 crc;
        if (auto err = binary::Read(*r, binary::BigEndian, crc)) return fmt::Errorf("{}: {}", name, err);

        // The CRC covers the type and the data.
        auto h = crc32::NewIEEE();
        h->Write(type);
        h->Write(chunk);
        bool ok = h->Sum32() == crc;
        fmt::Println("  {}  {:>6} B  crc {}", asStringView(type), length, ok ? "ok" : "MISMATCH");

        std::string_view t = asStringView(type);
        if (t == "IHDR") {
            binary::Decode(chunk, binary::BigEndian, header);
        } else if (t == "IDAT") {
            idat.push_back(bytes::NewReader(chunk));
            compressed += length;
        } else if (t == "IEND") {
            break;
        }
    }
    fmt::Println("  {}x{}, {}-bit {}, {}", header.Width, header.Height, header.BitDepth, colorName(header.ColorType),
                 header.Interlace ? "interlaced" : "not interlaced");

    // The IDAT chunks are one zlib stream.
    std::size_t chunks = idat.size();
    auto [zr, zerr] = zlib::NewReader(io::MultiReader(std::move(idat)));
    if (zerr) return fmt::Errorf("{}: {}", name, zerr);
    auto [inflated, cerr] = io::Copy(io::Discard(), *zr);
    if (cerr) return fmt::Errorf("{}: pixels: {}", name, cerr);
    fmt::Println("  pixels: {} B in {} IDAT, {} B inflated ({:.1f}x){}", compressed, chunks, inflated,
                 double(inflated) / double(compressed),
                 !header.Interlace && inflated != rawSize(header) ? fmt::Sprint(", expected {}", rawSize(header)) : string());
    return nullptr;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fmt::Println(os::Stderr, "usage: pnginfo file.png...");
        return 2;
    }
    int status = 0;
    for (int i = 1; i < argc; i++) {
        if (auto err = inspect(argv[i])) {
            fmt::Println(os::Stderr, "pnginfo: {}", err);
            status = 1;
        }
    }
    return status;
}
