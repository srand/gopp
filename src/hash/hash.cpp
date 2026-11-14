// The hash/crc32 and hash/adler32 packages, ported from Go 1.27.3's
// hash/crc32/crc32.go, crc32_generic.go and hash/adler32/adler32.go, over
// zlib's crc32 and adler32.
#include <gopp/hash/adler32.hpp>
#include <gopp/hash/crc32.hpp>
#include <gopp/sync.hpp>

#include <cstring>
#include <map>
#include <mutex>

#include <zlib.h>

namespace gopp {

namespace crc32 {

namespace {

constexpr std::size_t slicing8Cutoff = 16;

using slicing8Table = std::array<Table, 8>;

consteval slicing8Table slicingMakeTable(uint32 poly) {
    slicing8Table t{};
    t[0] = detail::makeTable(poly);
    for (int i = 0; i < 256; i++) {
        uint32 crc = t[0][i];
        for (int j = 1; j < 8; j++) {
            crc = t[0][crc & 0xFF] ^ (crc >> 8);
            t[j][i] = crc;
        }
    }
    return t;
}

constexpr slicing8Table castagnoliTable8 = slicingMakeTable(Castagnoli);

uint32 simpleUpdate(uint32 crc, const Table& tab, std::span<const byte> p) {
    crc = ~crc;
    for (byte v : p) crc = tab[byte(crc) ^ v] ^ (crc >> 8);
    return ~crc;
}

uint32 le32(const byte* p) {
    uint32 v;
    std::memcpy(&v, p, 4);
    if constexpr (std::endian::native == std::endian::big) v = __builtin_bswap32(v);
    return v;
}

uint32 slicingUpdate(uint32 crc, const slicing8Table& tab, std::span<const byte> p) {
    if (p.size() >= slicing8Cutoff) {
        crc = ~crc;
        while (p.size() > 8) {
            crc ^= le32(p.data());
            crc = tab[0][p[7]] ^ tab[1][p[6]] ^ tab[2][p[5]] ^ tab[3][p[4]] ^ tab[4][crc >> 24] ^
                  tab[5][(crc >> 16) & 0xFF] ^ tab[6][(crc >> 8) & 0xFF] ^ tab[7][crc & 0xFF];
            p = p.subspan(8);
        }
        crc = ~crc;
    }
    if (p.empty()) return crc;
    return simpleUpdate(crc, tab[0], p);
}

uint32 updateIEEE(uint32 crc, std::span<const byte> p) {
    // zlib's crc32 takes and returns the checksum as Go's Update does.
    return uint32(::crc32_z(crc, p.data(), p.size()));
}

class digest final : public hash::Hash32 {
public:
    explicit digest(const Table* tab) : tab_(tab) {}
    std::pair<isize, error> Write(std::span<const byte> p) override {
        crc_ = Update(crc_, tab_, p);
        return {isize(p.size()), nullptr};
    }
    uint32 Sum32() const override { return crc_; }
    slice<byte> Sum(slice<byte> in) const override {
        uint32 s = crc_;
        return append(std::move(in), byte(s >> 24), byte(s >> 16), byte(s >> 8), byte(s));
    }
    void Reset() override { crc_ = 0; }
    isize Size() const override { return crc32::Size; }
    isize BlockSize() const override { return 1; }

private:
    uint32 crc_ = 0;
    const Table* tab_;
};

}  // namespace

const Table* MakeTable(uint32 poly) {
    static constexpr Table castagnoli = castagnoliTable8[0];
    if (poly == IEEE) return IEEETable;
    if (poly == Castagnoli) return &castagnoli;
    // Other tables are kept, as they're made once and used for long.
    static sync::Mutex mu;
    static auto* tables = new std::map<uint32, Table>();  // never freed
    std::lock_guard l(mu);
    auto [it, made] = tables->try_emplace(poly);
    if (made) {
        for (uint32 i = 0; i < 256; i++) {
            uint32 crc = i;
            for (int j = 0; j < 8; j++) crc = (crc & 1) ? (crc >> 1) ^ poly : crc >> 1;
            it->second[i] = crc;
        }
    }
    return &it->second;
}

uint32 Update(uint32 crc, const Table* tab, std::span<const byte> p) {
    if (tab == IEEETable || *tab == *IEEETable) return updateIEEE(crc, p);
    if (*tab == castagnoliTable8[0]) return slicingUpdate(crc, castagnoliTable8, p);
    return simpleUpdate(crc, *tab, p);
}

uint32 Checksum(std::span<const byte> data, const Table* tab) { return Update(0, tab, data); }

uint32 ChecksumIEEE(std::span<const byte> data) { return updateIEEE(0, data); }

ptr<hash::Hash32> New(const Table* tab) { return make_ptr<digest>(tab); }

ptr<hash::Hash32> NewIEEE() { return New(IEEETable); }

}  // namespace crc32

namespace adler32 {

namespace {

class digest final : public hash::Hash32 {
public:
    std::pair<isize, error> Write(std::span<const byte> p) override {
        d_ = uint32(::adler32_z(d_, p.data(), p.size()));
        return {isize(p.size()), nullptr};
    }
    uint32 Sum32() const override { return d_; }
    slice<byte> Sum(slice<byte> in) const override {
        uint32 s = d_;
        return append(std::move(in), byte(s >> 24), byte(s >> 16), byte(s >> 8), byte(s));
    }
    void Reset() override { d_ = 1; }
    isize Size() const override { return adler32::Size; }
    isize BlockSize() const override { return 4; }

private:
    uint32 d_ = 1;
};

}  // namespace

ptr<hash::Hash32> New() { return make_ptr<digest>(); }

uint32 Checksum(std::span<const byte> data) { return uint32(::adler32_z(1, data.data(), data.size())); }

}  // namespace adler32

}  // namespace gopp
