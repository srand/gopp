// encoding/binary, ported from Go 1.27.3's encoding/binary: its errors and
// varint.go (the rest is templates in the header).
#include <gopp/encoding/binary.hpp>

namespace gopp::binary {

namespace detail {

error errBufferTooSmall() {
    static const error err = errors::New("buffer too small");
    return err;
}

error errOverflow() {
    static const error err = errors::New("binary: varint overflows a 64-bit integer");
    return err;
}

}  // namespace detail

slice<byte> AppendUvarint(slice<byte> buf, uint64 x) {
    byte tmp[MaxVarintLen64];
    int n = PutUvarint(tmp, x);
    return appendSpan(std::move(buf), std::span<const byte>(tmp, std::size_t(n)));
}

int PutUvarint(std::span<byte> buf, uint64 x) {
    std::size_t i = 0;
    while (x >= 0x80) {
        if (i >= buf.size()) panic("index out of range");
        buf[i] = byte(x) | 0x80;
        x >>= 7;
        i++;
    }
    if (i >= buf.size()) panic("index out of range");
    buf[i] = byte(x);
    return int(i + 1);
}

std::pair<uint64, int> Uvarint(std::span<const byte> buf) {
    uint64 x = 0;
    unsigned s = 0;
    for (std::size_t i = 0; i < buf.size(); i++) {
        if (i == MaxVarintLen64) {
            return {0, -int(i + 1)};  // overflow
        }
        byte b = buf[i];
        if (b < 0x80) {
            if (i == MaxVarintLen64 - 1 && b > 1) {
                return {0, -int(i + 1)};  // overflow
            }
            return {x | uint64(b) << s, int(i + 1)};
        }
        x |= uint64(b & 0x7f) << s;
        s += 7;
    }
    return {0, 0};
}

slice<byte> AppendVarint(slice<byte> buf, int64 x) {
    uint64 ux = uint64(x) << 1;
    if (x < 0) ux = ~ux;
    return AppendUvarint(std::move(buf), ux);
}

int PutVarint(std::span<byte> buf, int64 x) {
    uint64 ux = uint64(x) << 1;
    if (x < 0) ux = ~ux;
    return PutUvarint(buf, ux);
}

std::pair<int64, int> Varint(std::span<const byte> buf) {
    auto [ux, n] = Uvarint(buf);  // ok to continue in presence of error
    int64 x = int64(ux >> 1);
    if (ux & 1) x = ~x;
    return {x, n};
}

}  // namespace gopp::binary
