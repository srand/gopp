// After Go's vendor/golang.org/x/net/http2/hpack: hpack.Go, encode.Go,
// huffman.Go and tables.Go.
#include "hpack.hpp"

#include <gopp/slice.hpp>
#include <gopp/strings.hpp>

#include <array>
#include <memory>

namespace gopp::http::hpack {

namespace {

#include "hpack_tables.hpp"

constexpr uint32 uint32_max = ~uint32(0);
constexpr uint32 initial_header_table_size = 4096;

gopp::error decodingError(std::string_view msg) { return make_ptr<DecodingError>(errors::New(string(msg))); }

const detail::header_field_table& staticTable() {
    static const detail::header_field_table* t = [] {
        auto* t = new detail::header_field_table(true);
        for (auto& [n, v] : static_table_entries) t->addEntry(HeaderField{string::fromStatic(n), string::fromStatic(v)});
        return t;
    }();
    return *t;
}

// The Huffman decoding tree: a node per 8 bits of code, whose children
// are indexed by the next byte (huffman.Go).
struct node {
    std::unique_ptr<std::array<node*, 256>> children;  // nil for a leaf
    uint8 code_len = 0;  // for a leaf: the bits that led to sym
    byte sym = 0;
};

node* newInternalNode() {
    auto* n = new node;
    n->children.reset(new std::array<node*, 256>());
    n->children->fill(nullptr);
    return n;
}

const node* rootHuffmanNode() {
    static const node* root = [] {
        node* root = newInternalNode();
        static node leaves[256];
        for (int sym = 0; sym < 256; sym++) {
            uint32 code = huffman_codes[sym];
            uint8 code_len = huffman_code_len[sym];
            node* cur = root;
            while (code_len > 8) {
                code_len -= 8;
                uint8 i = static_cast<uint8>(code >> code_len);
                if (!(*cur->children)[i]) (*cur->children)[i] = newInternalNode();
                cur = (*cur->children)[i];
            }
            uint8 shift = 8 - code_len;
            int start = static_cast<uint8>(code << shift), end = 1 << shift;
            leaves[sym].sym = static_cast<byte>(sym);
            leaves[sym].code_len = code_len;
            for (int i = start; i < start + end; i++) (*cur->children)[i] = &leaves[sym];
        }
        return root;
    }();
    return root;
}

}  // namespace

gopp::error huffmanDecode(std::string& buf, int max_len, std::span<const byte> v) {
    const node* root = rootHuffmanNode();
    const node* n = root;
    // cur is the bit buffer of unconsumed input, cbits how many bits it
    // holds, and sbits the bits since the last symbol (to detect padding
    // longer than 7 bits).
    uint64 cur = 0;
    uint8 cbits = 0, sbits = 0;
    for (byte b : v) {
        cur = cur << 8 | b;
        cbits += 8;
        sbits += 8;
        while (cbits >= 8) {
            uint8 idx = static_cast<uint8>(cur >> (cbits - 8));
            n = (*n->children)[idx];
            if (!n) return err_invalid_huffman;
            if (!n->children) {
                if (max_len != 0 && static_cast<int>(buf.size()) == max_len) return err_string_length;
                buf.push_back(static_cast<char>(n->sym));
                cbits -= n->code_len;
                n = root;
                sbits = cbits;
            } else {
                cbits -= 8;
            }
        }
    }
    while (cbits > 0) {
        n = (*n->children)[static_cast<uint8>(cur << (8 - cbits))];
        if (!n) return err_invalid_huffman;
        if (n->children || n->code_len > cbits) break;
        if (max_len != 0 && static_cast<int>(buf.size()) == max_len) return err_string_length;
        buf.push_back(static_cast<char>(n->sym));
        cbits -= n->code_len;
        n = root;
        sbits = cbits;
    }
    if (sbits > 7) return err_invalid_huffman;  // padding longer than 7 bits
    // The padding must be the most significant bits of EOS: all ones.
    if (uint64 mask = (uint64(1) << cbits) - 1; (cur & mask) != mask) return err_invalid_huffman;
    return nullptr;
}

namespace {

void appendIndexed(std::string& dst, uint64 i) {
    std::size_t first = dst.size();
    appendVarInt(dst, 7, i);
    dst[first] = static_cast<char>(dst[first] | 0x80);
}

void appendHpackString(std::string& dst, std::string_view s) {
    uint64 huffman_length = huffmanEncodeLength(s);
    if (huffman_length < s.size()) {
        std::size_t first = dst.size();
        appendVarInt(dst, 7, huffman_length);
        appendHuffmanString(dst, s);
        dst[first] = static_cast<char>(dst[first] | 0x80);
    } else {
        appendVarInt(dst, 7, s.size());
        dst.append(s);
    }
}

byte encodeTypeByte(bool indexing, bool sensitive) {
    if (sensitive) return 0x10;
    if (indexing) return 0x40;
    return 0;
}

void appendNewName(std::string& dst, const HeaderField& f, bool indexing) {
    dst.push_back(static_cast<char>(encodeTypeByte(indexing, f.sensitive)));
    appendHpackString(dst, f.name);
    appendHpackString(dst, f.value);
}

void appendIndexedName(std::string& dst, const HeaderField& f, uint64 i, bool indexing) {
    std::size_t first = dst.size();
    appendVarInt(dst, indexing ? 6 : 4, i);
    dst[first] = static_cast<char>(dst[first] | encodeTypeByte(indexing, f.sensitive));
    appendHpackString(dst, f.value);
}

void appendTableSize(std::string& dst, uint32 v) {
    std::size_t first = dst.size();
    appendVarInt(dst, 5, v);
    dst[first] = static_cast<char>(dst[first] | 0x20);
}

enum index_type { indexed_true, indexed_false, indexed_never };

}  // namespace

const gopp::error err_need_more = errors::New("need more data");
const gopp::error err_varint_overflow = decodingError("varint integer overflow");
const gopp::error err_string_length = errors::New("hpack: string too long");
const gopp::error err_invalid_huffman = errors::New("hpack: invalid Huffman-encoded data");

string HeaderField::String() const {
    return "header field " + strconv::Quote(name) + " = " + strconv::Quote(value) + (sensitive ? " (sensitive)" : "");
}

string DecodingError::Error() const { return "decoding error: " + err.Error(); }

string InvalidIndexError::Error() const { return "invalid indexed representation index " + strconv::Itoa(static_cast<int64>(index)); }

// ---------------------------------------------------------------------------
// Tables

namespace detail {

void header_field_table::addEntry(HeaderField f) {
    uint64 id = ents_.size() + evict_count_ + 1;
    by_name_[f.name.str()] = id;
    by_name_value_[{f.name.str(), f.value.str()}] = id;
    ents_.push_back(std::move(f));
}

void header_field_table::evict_oldest(std::size_t n) {
    if (n > ents_.size()) panic("evictOldest on a table with fewer entries");
    for (std::size_t k = 0; k < n; k++) {
        const HeaderField& f = ents_.front();
        uint64 id = evict_count_ + k + 1;
        if (auto it = by_name_.find(f.name.str()); it != by_name_.end() && it->second == id) by_name_.erase(it);
        if (auto it = by_name_value_.find({f.name.str(), f.value.str()}); it != by_name_value_.end() && it->second == id) {
            by_name_value_.erase(it);
        }
        ents_.pop_front();
    }
    evict_count_ += n;
}

std::pair<uint64, bool> header_field_table::search(const HeaderField& f) const {
    if (!f.sensitive) {
        if (auto it = by_name_value_.find({f.name.str(), f.value.str()}); it != by_name_value_.end()) {
            return {idToIndex(it->second), true};
        }
    }
    if (auto it = by_name_.find(f.name.str()); it != by_name_.end()) return {idToIndex(it->second), false};
    return {0, false};
}

uint64 header_field_table::idToIndex(uint64 id) const {
    if (id <= evict_count_) panic("hpack: id <= evictCount");
    uint64 k = id - evict_count_ - 1;  // the index in ents_
    if (!static_) return ents_.size() - k;  // the dynamic table counts from the newest
    return k + 1;
}

void dynamicTable::setMaxSize(uint32 v) {
    max_size = v;
    evict();
}

void dynamicTable::add(HeaderField f) {
    size += f.size();
    table.addEntry(std::move(f));
    evict();
}

void dynamicTable::evict() {
    std::size_t n = 0;
    while (size > max_size && n < table.len()) {
        size -= table.at(n).size();
        n++;
    }
    table.evict_oldest(n);
}

}  // namespace detail

// ---------------------------------------------------------------------------
// Decoder

Decoder::Decoder(uint32 maxDynamicTableSize, emit_func emit) : emit_(std::move(emit)) {
    dyn_.allowed_max_size = maxDynamicTableSize;
    dyn_.setMaxSize(maxDynamicTableSize);
}

std::size_t Decoder::maxTableIndex() const noexcept { return dyn_.table.len() + staticTable().len(); }

const HeaderField* Decoder::at(uint64 i) const noexcept {
    if (i == 0) return nullptr;
    if (i <= staticTable().len()) return &staticTable().at(i - 1);
    if (i > maxTableIndex()) return nullptr;
    // The dynamic table's index 1 is its newest entry, at the end.
    return &dyn_.table.at(dyn_.table.len() - (i - staticTable().len()));
}

std::pair<std::vector<HeaderField>, gopp::error> Decoder::decodeFull(std::span<const byte> p) {
    std::vector<HeaderField> hf;
    emit_func save = std::move(emit_);
    emit_ = [&](const HeaderField& f) { hf.push_back(f); };
    auto [n, err] = Write(p);
    if (!err) err = Close();
    emit_ = std::move(save);
    if (err) return {{}, err};
    return {std::move(hf), nullptr};
}

gopp::error Decoder::Close() {
    if (!save_buf_.empty()) {
        save_buf_.clear();
        return decodingError("truncated headers");
    }
    first_field_ = true;
    return nullptr;
}

std::pair<isize, gopp::error> Decoder::Write(std::span<const byte> p) {
    if (p.empty()) return {0, nullptr};  // prevent state changes on empty writes
    std::vector<byte> joined;
    if (save_buf_.empty()) {
        buf_ = p;
    } else {
        joined = std::move(save_buf_);
        save_buf_.clear();
        joined.insert(joined.end(), p.begin(), p.end());
        buf_ = joined;
    }
    gopp::error err;
    while (!buf_.empty()) {
        err = parseHeaderFieldRepr();
        if (err == err_need_more) {
            // Keep the partial field for the next write, unless it is
            // already longer than any field could be.
            constexpr int64 var_int_overhead = 8;
            if (max_str_len_ != 0 && static_cast<int64>(buf_.size()) > 2 * (max_str_len_ + var_int_overhead)) {
                return {0, err_string_length};
            }
            save_buf_.assign(buf_.begin(), buf_.end());
            buf_ = {};
            return {static_cast<isize>(p.size()), nullptr};
        }
        first_field_ = false;
        if (err) break;
    }
    buf_ = {};
    return {static_cast<isize>(p.size()), err};
}

gopp::error Decoder::parseHeaderFieldRepr() {
    byte b = buf_[0];
    if (b & 128) return parseFieldIndexed();                         // 6.1 indexed
    if ((b & 192) == 64) return parseFieldLiteral(6, indexed_true);  // 6.2.1 with indexing
    if ((b & 240) == 0) return parseFieldLiteral(4, indexed_false);  // 6.2.2 without indexing
    if ((b & 240) == 16) return parseFieldLiteral(4, indexed_never); // 6.2.3 never indexed
    if ((b & 224) == 32) return parseDynamicTableSizeUpdate();     // 6.3
    return decodingError("invalid encoding");
}

gopp::error Decoder::parseFieldIndexed() {
    std::span<const byte> buf = buf_;
    uint64 idx;
    if (gopp::error err = readVarInt(7, buf, idx)) return err;
    const HeaderField* hf = at(idx);
    if (!hf) return make_ptr<DecodingError>(make_ptr<InvalidIndexError>(idx));
    buf_ = buf;
    return callEmit(HeaderField{hf->name, hf->value});
}

gopp::error Decoder::parseFieldLiteral(uint8 n, int it) {
    std::span<const byte> buf = buf_;
    uint64 name_idx;
    if (gopp::error err = readVarInt(n, buf, name_idx)) return err;

    HeaderField hf;
    bool want_str = emit_enabled_ || it == indexed_true;
    undecoded undecoded_name;
    if (name_idx > 0) {
        const HeaderField* ihf = at(name_idx);
        if (!ihf) return make_ptr<DecodingError>(make_ptr<InvalidIndexError>(name_idx));
        hf.name = ihf->name;
    } else if (gopp::error err = ReadString(buf, undecoded_name)) {
        return err;
    }
    undecoded undecoded_value;
    if (gopp::error err = ReadString(buf, undecoded_value)) return err;
    if (want_str) {
        if (name_idx == 0) {
            if (gopp::error err = decodeString(undecoded_name, hf.name)) return err;
        }
        if (gopp::error err = decodeString(undecoded_value, hf.value)) return err;
    }
    buf_ = buf;
    if (it == indexed_true) dyn_.add(hf);
    hf.sensitive = it == indexed_never;
    return callEmit(hf);
}

gopp::error Decoder::callEmit(const HeaderField& hf) {
    if (max_str_len_ != 0) {
        if (static_cast<int>(hf.name.size()) > max_str_len_ || static_cast<int>(hf.value.size()) > max_str_len_) {
            return err_string_length;
        }
    }
    if (emit_enabled_ && emit_) emit_(hf);
    return nullptr;
}

gopp::error Decoder::parseDynamicTableSizeUpdate() {
    // RFC 7541 4.2: a size update must come first in a header block. Go
    // allows it later while the table is empty, as some peers send it so.
    if (!first_field_ && dyn_.size > 0) {
        return decodingError("dynamic table size update MUST occur at the beginning of a header block");
    }
    std::span<const byte> buf = buf_;
    uint64 size;
    if (gopp::error err = readVarInt(5, buf, size)) return err;
    if (size > dyn_.allowed_max_size) return decodingError("dynamic table size update too large");
    dyn_.setMaxSize(static_cast<uint32>(size));
    buf_ = buf;
    return nullptr;
}

gopp::error Decoder::ReadString(std::span<const byte>& p, undecoded& u) {
    if (p.empty()) return err_need_more;
    bool is_huff = p[0] & 128;
    uint64 str_len;
    if (gopp::error err = readVarInt(7, p, str_len)) return err;
    if (max_str_len_ != 0 && str_len > static_cast<uint64>(max_str_len_)) return err_string_length;
    if (p.size() < str_len) return err_need_more;
    u.huff = is_huff;
    u.b = p.first(str_len);
    p = p.subspan(str_len);
    return nullptr;
}

gopp::error Decoder::decodeString(const undecoded& u, string& out) {
    if (!u.huff) {
        out = string(asStringView(u.b));
        return nullptr;
    }
    std::string buf;
    if (gopp::error err = huffmanDecode(buf, max_str_len_, u.b)) return err;
    out = string(buf);
    return nullptr;
}

gopp::error readVarInt(uint8 n, std::span<const byte>& p, uint64& out) {
    if (n < 1 || n > 8) panic("bad n");
    if (p.empty()) return err_need_more;
    uint64 i = p[0];
    if (n < 8) i &= (uint64(1) << n) - 1;
    if (i < (uint64(1) << n) - 1) {
        p = p.subspan(1);
        out = i;
        return nullptr;
    }
    std::span<const byte> q = p.subspan(1);
    uint64 m = 0;
    while (!q.empty()) {
        byte b = q[0];
        q = q.subspan(1);
        i += uint64(b & 127) << m;
        if ((b & 128) == 0) {
            p = q;
            out = i;
            return nullptr;
        }
        m += 7;
        if (m >= 63) return err_varint_overflow;
    }
    return err_need_more;
}

// ---------------------------------------------------------------------------
// Encoder

Encoder::Encoder(std::string* out)
    : min_size_(uint32_max), max_size_limit_(initial_header_table_size), out_(out) {
    dyn_.setMaxSize(initial_header_table_size);
}

gopp::error Encoder::writeField(const HeaderField& f) {
    std::string& dst = *out_;
    if (table_size_update_) {
        table_size_update_ = false;
        if (min_size_ < dyn_.max_size) appendTableSize(dst, min_size_);
        min_size_ = uint32_max;
        appendTableSize(dst, dyn_.max_size);
    }
    auto [idx, name_value_match] = searchTable(f);
    if (name_value_match) {
        appendIndexed(dst, idx);
    } else {
        bool indexing = !f.sensitive && f.size() <= dyn_.max_size;
        if (indexing) dyn_.add(f);
        if (idx == 0) appendNewName(dst, f, indexing);
        else appendIndexedName(dst, f, idx, indexing);
    }
    return nullptr;
}

std::pair<uint64, bool> Encoder::searchTable(const HeaderField& f) const {
    auto [i, match] = staticTable().search(f);
    if (match) return {i, true};
    auto [j, dmatch] = dyn_.table.search(f);
    if (dmatch || (i == 0 && j != 0)) return {j + staticTable().len(), dmatch};
    return {i, false};
}

void Encoder::setMaxDynamicTableSize(uint32 v) {
    if (v > max_size_limit_) v = max_size_limit_;
    if (v < min_size_) min_size_ = v;
    table_size_update_ = true;
    dyn_.setMaxSize(v);
}

void Encoder::setMaxDynamicTableSizeLimit(uint32 v) {
    max_size_limit_ = v;
    if (dyn_.max_size > v) {
        table_size_update_ = true;
        dyn_.setMaxSize(v);
    }
}

void appendVarInt(std::string& dst, uint8 n, uint64 i) {
    uint64 k = (uint64(1) << n) - 1;
    if (i < k) {
        dst.push_back(static_cast<char>(i));
        return;
    }
    dst.push_back(static_cast<char>(k));
    i -= k;
    for (; i >= 128; i >>= 7) dst.push_back(static_cast<char>(0x80 | (i & 0x7f)));
    dst.push_back(static_cast<char>(i));
}

// ---------------------------------------------------------------------------
// Huffman

std::pair<string, gopp::error> huffmanDecodeToString(std::span<const byte> v) {
    std::string buf;
    if (gopp::error err = huffmanDecode(buf, 0, v)) return {{}, err};
    return {string(buf), nullptr};
}

void appendHuffmanString(std::string& dst, std::string_view s) {
    uint64 x = 0;  // bit buffer
    unsigned n = 0;  // valid bits in x
    for (unsigned char c : s) {
        n += huffman_code_len[c];
        x <<= huffman_code_len[c] % 64;
        x |= huffman_codes[c];
        if (n >= 32) {
            n %= 32;
            uint32 y = static_cast<uint32>(x >> n);
            char b[4] = {static_cast<char>(y >> 24), static_cast<char>(y >> 16), static_cast<char>(y >> 8), static_cast<char>(y)};
            dst.append(b, 4);
        }
    }
    // Pad with the most significant bits of EOS.
    if (unsigned over = n % 8; over > 0) {
        constexpr uint64 eos_code = 0x3fffffff;
        constexpr unsigned eos_nbits = 30;
        constexpr uint64 eos_pad_byte = eos_code >> (eos_nbits - 8);
        unsigned pad = 8 - over;
        x = (x << pad) | (eos_pad_byte >> over);
        n += pad;
    }
    for (unsigned k = n / 8; k > 0; k--) dst.push_back(static_cast<char>(x >> (8 * (k - 1))));
}

uint64 huffmanEncodeLength(std::string_view s) noexcept {
    uint64 n = 0;
    for (unsigned char c : s) n += huffman_code_len[c];
    return (n + 7) / 8;
}

}  // namespace gopp::http::hpack
