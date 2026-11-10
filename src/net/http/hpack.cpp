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

gopp::error decoding_error(std::string_view msg) { return make_ptr<DecodingError>(errors::New(string(msg))); }

const detail::header_field_table& static_table() {
    static const detail::header_field_table* t = [] {
        auto* t = new detail::header_field_table(true);
        for (auto& [n, v] : static_table_entries) t->add_entry(HeaderField{string::from_static(n), string::from_static(v)});
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

node* new_internal_node() {
    auto* n = new node;
    n->children.reset(new std::array<node*, 256>());
    n->children->fill(nullptr);
    return n;
}

const node* root_huffman_node() {
    static const node* root = [] {
        node* root = new_internal_node();
        static node leaves[256];
        for (int sym = 0; sym < 256; sym++) {
            uint32 code = huffman_codes[sym];
            uint8 code_len = huffman_code_len[sym];
            node* cur = root;
            while (code_len > 8) {
                code_len -= 8;
                uint8 i = static_cast<uint8>(code >> code_len);
                if (!(*cur->children)[i]) (*cur->children)[i] = new_internal_node();
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

gopp::error huffman_decode(std::string& buf, int max_len, std::span<const byte> v) {
    const node* root = root_huffman_node();
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

void append_indexed(std::string& dst, uint64 i) {
    std::size_t first = dst.size();
    append_var_int(dst, 7, i);
    dst[first] = static_cast<char>(dst[first] | 0x80);
}

void append_hpack_string(std::string& dst, std::string_view s) {
    uint64 huffman_length = huffman_encode_length(s);
    if (huffman_length < s.size()) {
        std::size_t first = dst.size();
        append_var_int(dst, 7, huffman_length);
        append_huffman_string(dst, s);
        dst[first] = static_cast<char>(dst[first] | 0x80);
    } else {
        append_var_int(dst, 7, s.size());
        dst.append(s);
    }
}

byte encode_type_byte(bool indexing, bool sensitive) {
    if (sensitive) return 0x10;
    if (indexing) return 0x40;
    return 0;
}

void append_new_name(std::string& dst, const HeaderField& f, bool indexing) {
    dst.push_back(static_cast<char>(encode_type_byte(indexing, f.sensitive)));
    append_hpack_string(dst, f.name);
    append_hpack_string(dst, f.value);
}

void append_indexed_name(std::string& dst, const HeaderField& f, uint64 i, bool indexing) {
    std::size_t first = dst.size();
    append_var_int(dst, indexing ? 6 : 4, i);
    dst[first] = static_cast<char>(dst[first] | encode_type_byte(indexing, f.sensitive));
    append_hpack_string(dst, f.value);
}

void append_table_size(std::string& dst, uint32 v) {
    std::size_t first = dst.size();
    append_var_int(dst, 5, v);
    dst[first] = static_cast<char>(dst[first] | 0x20);
}

enum index_type { indexed_true, indexed_false, indexed_never };

}  // namespace

const gopp::error err_need_more = errors::New("need more data");
const gopp::error err_varint_overflow = decoding_error("varint integer overflow");
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

void header_field_table::add_entry(HeaderField f) {
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
            return {id_to_index(it->second), true};
        }
    }
    if (auto it = by_name_.find(f.name.str()); it != by_name_.end()) return {id_to_index(it->second), false};
    return {0, false};
}

uint64 header_field_table::id_to_index(uint64 id) const {
    if (id <= evict_count_) panic("hpack: id <= evictCount");
    uint64 k = id - evict_count_ - 1;  // the index in ents_
    if (!static_) return ents_.size() - k;  // the dynamic table counts from the newest
    return k + 1;
}

void dynamic_table::set_max_size(uint32 v) {
    max_size = v;
    evict();
}

void dynamic_table::add(HeaderField f) {
    size += f.size();
    table.add_entry(std::move(f));
    evict();
}

void dynamic_table::evict() {
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

Decoder::Decoder(uint32 max_dynamic_table_size, emit_func emit) : emit_(std::move(emit)) {
    dyn_.allowed_max_size = max_dynamic_table_size;
    dyn_.set_max_size(max_dynamic_table_size);
}

std::size_t Decoder::max_table_index() const noexcept { return dyn_.table.len() + static_table().len(); }

const HeaderField* Decoder::at(uint64 i) const noexcept {
    if (i == 0) return nullptr;
    if (i <= static_table().len()) return &static_table().at(i - 1);
    if (i > max_table_index()) return nullptr;
    // The dynamic table's index 1 is its newest entry, at the end.
    return &dyn_.table.at(dyn_.table.len() - (i - static_table().len()));
}

std::pair<std::vector<HeaderField>, gopp::error> Decoder::decode_full(std::span<const byte> p) {
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
        return decoding_error("truncated headers");
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
        err = parse_header_field_repr();
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

gopp::error Decoder::parse_header_field_repr() {
    byte b = buf_[0];
    if (b & 128) return parse_field_indexed();                         // 6.1 indexed
    if ((b & 192) == 64) return parse_field_literal(6, indexed_true);  // 6.2.1 with indexing
    if ((b & 240) == 0) return parse_field_literal(4, indexed_false);  // 6.2.2 without indexing
    if ((b & 240) == 16) return parse_field_literal(4, indexed_never); // 6.2.3 never indexed
    if ((b & 224) == 32) return parse_dynamic_table_size_update();     // 6.3
    return decoding_error("invalid encoding");
}

gopp::error Decoder::parse_field_indexed() {
    std::span<const byte> buf = buf_;
    uint64 idx;
    if (gopp::error err = read_var_int(7, buf, idx)) return err;
    const HeaderField* hf = at(idx);
    if (!hf) return make_ptr<DecodingError>(make_ptr<InvalidIndexError>(idx));
    buf_ = buf;
    return call_emit(HeaderField{hf->name, hf->value});
}

gopp::error Decoder::parse_field_literal(uint8 n, int it) {
    std::span<const byte> buf = buf_;
    uint64 name_idx;
    if (gopp::error err = read_var_int(n, buf, name_idx)) return err;

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
            if (gopp::error err = decode_string(undecoded_name, hf.name)) return err;
        }
        if (gopp::error err = decode_string(undecoded_value, hf.value)) return err;
    }
    buf_ = buf;
    if (it == indexed_true) dyn_.add(hf);
    hf.sensitive = it == indexed_never;
    return call_emit(hf);
}

gopp::error Decoder::call_emit(const HeaderField& hf) {
    if (max_str_len_ != 0) {
        if (static_cast<int>(hf.name.size()) > max_str_len_ || static_cast<int>(hf.value.size()) > max_str_len_) {
            return err_string_length;
        }
    }
    if (emit_enabled_ && emit_) emit_(hf);
    return nullptr;
}

gopp::error Decoder::parse_dynamic_table_size_update() {
    // RFC 7541 4.2: a size update must come first in a header block. Go
    // allows it later while the table is empty, as some peers send it so.
    if (!first_field_ && dyn_.size > 0) {
        return decoding_error("dynamic table size update MUST occur at the beginning of a header block");
    }
    std::span<const byte> buf = buf_;
    uint64 size;
    if (gopp::error err = read_var_int(5, buf, size)) return err;
    if (size > dyn_.allowed_max_size) return decoding_error("dynamic table size update too large");
    dyn_.set_max_size(static_cast<uint32>(size));
    buf_ = buf;
    return nullptr;
}

gopp::error Decoder::ReadString(std::span<const byte>& p, undecoded& u) {
    if (p.empty()) return err_need_more;
    bool is_huff = p[0] & 128;
    uint64 str_len;
    if (gopp::error err = read_var_int(7, p, str_len)) return err;
    if (max_str_len_ != 0 && str_len > static_cast<uint64>(max_str_len_)) return err_string_length;
    if (p.size() < str_len) return err_need_more;
    u.huff = is_huff;
    u.b = p.first(str_len);
    p = p.subspan(str_len);
    return nullptr;
}

gopp::error Decoder::decode_string(const undecoded& u, string& out) {
    if (!u.huff) {
        out = string(as_string_view(u.b));
        return nullptr;
    }
    std::string buf;
    if (gopp::error err = huffman_decode(buf, max_str_len_, u.b)) return err;
    out = string(buf);
    return nullptr;
}

gopp::error read_var_int(uint8 n, std::span<const byte>& p, uint64& out) {
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
    dyn_.set_max_size(initial_header_table_size);
}

gopp::error Encoder::write_field(const HeaderField& f) {
    std::string& dst = *out_;
    if (table_size_update_) {
        table_size_update_ = false;
        if (min_size_ < dyn_.max_size) append_table_size(dst, min_size_);
        min_size_ = uint32_max;
        append_table_size(dst, dyn_.max_size);
    }
    auto [idx, name_value_match] = search_table(f);
    if (name_value_match) {
        append_indexed(dst, idx);
    } else {
        bool indexing = !f.sensitive && f.size() <= dyn_.max_size;
        if (indexing) dyn_.add(f);
        if (idx == 0) append_new_name(dst, f, indexing);
        else append_indexed_name(dst, f, idx, indexing);
    }
    return nullptr;
}

std::pair<uint64, bool> Encoder::search_table(const HeaderField& f) const {
    auto [i, match] = static_table().search(f);
    if (match) return {i, true};
    auto [j, dmatch] = dyn_.table.search(f);
    if (dmatch || (i == 0 && j != 0)) return {j + static_table().len(), dmatch};
    return {i, false};
}

void Encoder::set_max_dynamic_table_size(uint32 v) {
    if (v > max_size_limit_) v = max_size_limit_;
    if (v < min_size_) min_size_ = v;
    table_size_update_ = true;
    dyn_.set_max_size(v);
}

void Encoder::set_max_dynamic_table_size_limit(uint32 v) {
    max_size_limit_ = v;
    if (dyn_.max_size > v) {
        table_size_update_ = true;
        dyn_.set_max_size(v);
    }
}

void append_var_int(std::string& dst, uint8 n, uint64 i) {
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

std::pair<string, gopp::error> huffman_decode_to_string(std::span<const byte> v) {
    std::string buf;
    if (gopp::error err = huffman_decode(buf, 0, v)) return {{}, err};
    return {string(buf), nullptr};
}

void append_huffman_string(std::string& dst, std::string_view s) {
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

uint64 huffman_encode_length(std::string_view s) noexcept {
    uint64 n = 0;
    for (unsigned char c : s) n += huffman_code_len[c];
    return (n + 7) / 8;
}

}  // namespace gopp::http::hpack
