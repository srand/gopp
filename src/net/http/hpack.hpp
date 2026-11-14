// HPACK, header compression for HTTP/2 (RFC 7541), after Go's
// vendor/golang.org/x/net/http2/hpack.
#pragma once

#include <gopp/errors.hpp>
#include <gopp/string.hpp>

#include <deque>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace gopp::http::hpack {

// A name-value pair. Sensitive fields are never indexed.
struct HeaderField {
    string name;
    string value;
    bool sensitive = false;

    bool isPseudo() const noexcept { return !name.empty() && name[0] == ':'; }
    // The size the field counts for in a dynamic table: 32 bytes of
    // overhead plus the name and value.
    uint32 size() const noexcept { return static_cast<uint32>(name.size() + value.size() + 32); }
    string String() const;
    friend bool operator==(const HeaderField& a, const HeaderField& b) noexcept {
        return a.name == b.name && a.value == b.value && a.sensitive == b.sensitive;
    }
};

// hpack.DecodingError
struct DecodingError : errorInterface {
    gopp::error err;
    explicit DecodingError(gopp::error e) : err(std::move(e)) {}
    string Error() const override;
    const errorInterface* Unwrap() const noexcept override { return err.get(); }
};

// hpack.InvalidIndexError
struct InvalidIndexError : errorInterface {
    uint64 index;
    explicit InvalidIndexError(uint64 i) : index(i) {}
    string Error() const override;
};

extern const gopp::error err_string_length;     // "hpack: string too long"
extern const gopp::error err_invalid_huffman;   // "hpack: invalid Huffman-encoded data"
// For tests: an incomplete representation, and an integer too large.
extern const gopp::error err_need_more;
extern const gopp::error err_varint_overflow;

namespace detail {

// A table of header fields with lookup by name and by name and value
// (headerFieldTable). Entries get increasing ids; indexes count from the
// newest entry for the dynamic table and from the first for the static one.
class header_field_table {
public:
    explicit header_field_table(bool is_static = false) : static_(is_static) {}
    std::size_t len() const noexcept { return ents_.size(); }
    const HeaderField& at(std::size_t k) const noexcept { return ents_[k]; }
    void addEntry(HeaderField f);
    void evict_oldest(std::size_t n);
    // The index of f and whether the value matched too; 0 if not found.
    std::pair<uint64, bool> search(const HeaderField& f) const;

private:
    uint64 idToIndex(uint64 id) const;
    struct pair_hash {
        std::size_t operator()(const std::pair<std::string, std::string>& p) const noexcept {
            return std::hash<std::string>{}(p.first) * 31 + std::hash<std::string>{}(p.second);
        }
    };
    bool static_;
    std::deque<HeaderField> ents_;
    uint64 evict_count_ = 0;
    std::unordered_map<std::string, uint64> by_name_;
    std::unordered_map<std::pair<std::string, std::string>, uint64, pair_hash> by_name_value_;
};

struct dynamicTable {
    header_field_table table;
    uint32 size = 0;              // in bytes
    uint32 max_size = 0;          // current maximum
    uint32 allowed_max_size = 0;  // max_size may go up to this

    void setMaxSize(uint32 v);
    void add(HeaderField f);
    void evict();
};

}  // namespace detail

// hpack.Decoder: decodes header blocks, calling emit for each field.
class Decoder {
public:
    using emit_func = std::function<void(const HeaderField&)>;

    Decoder(uint32 maxDynamicTableSize, emit_func emit);

    // Limits decoded strings to n bytes; 0 means no limit.
    void setMaxStringLength(int n) noexcept { max_str_len_ = n; }
    void setEmitFunc(emit_func f) { emit_ = std::move(f); }
    // Whether to call emit; with emitting off, literal strings are not
    // decoded unless they are indexed.
    void setEmitEnabled(bool v) noexcept { emit_enabled_ = v; }
    bool emitEnabled() const noexcept { return emit_enabled_; }
    void setMaxDynamicTableSize(uint32 v) { dyn_.setMaxSize(v); }
    // The largest table size the peer may switch to (our SETTINGS).
    void setAllowedMaxDynamicTableSize(uint32 v) noexcept { dyn_.allowed_max_size = v; }

    // Decodes a whole header block.
    std::pair<std::vector<HeaderField>, gopp::error> decodeFull(std::span<const byte> p);
    // Decodes part of a header block; a field split across writes is
    // completed by the next one.
    std::pair<isize, gopp::error> Write(std::span<const byte> p);
    // Ends a header block; an error if it ended within a field.
    gopp::error Close();

    // For tests: the dynamic table.
    const detail::dynamicTable& dynamicTable() const noexcept { return dyn_; }

private:
    std::size_t maxTableIndex() const noexcept;
    const HeaderField* at(uint64 i) const noexcept;
    gopp::error parseHeaderFieldRepr();
    gopp::error parseFieldIndexed();
    gopp::error parseFieldLiteral(uint8 n, int index_type);
    gopp::error parseDynamicTableSizeUpdate();
    gopp::error callEmit(const HeaderField& hf);
    struct undecoded {
        bool huff = false;
        std::span<const byte> b;
    };
    gopp::error ReadString(std::span<const byte>& p, undecoded& u);
    gopp::error decodeString(const undecoded& u, string& out);

    detail::dynamicTable dyn_;
    emit_func emit_;
    bool emit_enabled_ = true;
    int max_str_len_ = 0;
    std::span<const byte> buf_;   // the input being parsed
    std::vector<byte> save_buf_;  // a partial field from the last write
    bool first_field_ = true;
};

// hpack.Encoder: encodes header fields, appending to *out.
class Encoder {
public:
    explicit Encoder(std::string* out);

    gopp::error writeField(const HeaderField& f);
    // Changes the dynamic table size, up to the limit; the next field
    // starts with a size update.
    void setMaxDynamicTableSize(uint32 v);
    uint32 maxDynamicTableSize() const noexcept { return dyn_.max_size; }
    // The peer's SETTINGS_HEADER_TABLE_SIZE.
    void setMaxDynamicTableSizeLimit(uint32 v);
    void setOutput(std::string* out) noexcept { out_ = out; }

private:
    friend struct test_access;
    std::pair<uint64, bool> searchTable(const HeaderField& f) const;

    detail::dynamicTable dyn_;
    uint32 min_size_;
    uint32 max_size_limit_;
    bool table_size_update_ = false;
    std::string* out_;
};

// Huffman coding (RFC 7541 appendix B). max_len limits the output; 0
// means no limit.
gopp::error huffmanDecode(std::string& out, int max_len, std::span<const byte> v);
std::pair<string, gopp::error> huffmanDecodeToString(std::span<const byte> v);
void appendHuffmanString(std::string& dst, std::string_view s);
uint64 huffmanEncodeLength(std::string_view s) noexcept;

// Integer and string representations, for the encoder and tests.
void appendVarInt(std::string& dst, uint8 n, uint64 i);
// Reads an n-bit-prefix integer from p, advancing it. Errors: need more
// data (returns false in *complete) or overflow.
gopp::error readVarInt(uint8 n, std::span<const byte>& p, uint64& out);

}  // namespace gopp::http::hpack
