// The encoding/json package, with C++26 reflection in place of Go's.
//
// Structs map to JSON objects without extra code: their public data
// members are the fields, named as declared. Go's struct tags are
// annotations:
//
//     struct item {
//         string name;
//         [[=json::tag("n,omitempty")]] int64 count;
//         [[=json::tag("-")]] string secret;
//     };
//
//     auto [data, err] = json::Marshal(item{"x", 3});   // {"name":"x","n":3}
//     item it;
//     err = json::Unmarshal(data, &it);
//     json::Encode(w, it);                              // to an io::Writer, with "\n"
//
// Supported: bool, integers, floating point (not NaN or infinity), strings,
// slice<byte> (as base64, like Go's []byte), sequences (std::vector,
// gopp::slice, std::array), maps with string or integer keys (sorted by key
// when encoded), std::optional, gopp::ptr, std::shared_ptr and
// std::unique_ptr (null when empty, like Go's *T), structs, json::Value
// (Go's any),
// json::RawMessage, time::Time (RFC 3339) and
// time::Duration (nanoseconds). Types with MarshalJSON/UnmarshalJSON
// members encode themselves (json.Marshaler and json.Unmarshaler).
#pragma once

// The module interfaces (modules/) import these instead.
#ifndef GOPP_MODULE
#include <gopp/errors.hpp>
#include <gopp/io.hpp>
#include <gopp/ptr.hpp>
#include <gopp/slice.hpp>
#include <gopp/string.hpp>
#include <gopp/time.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <concepts>
#include <cstring>
#include <map>
#include <memory>
#include <meta>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>
#endif

namespace gopp::json {

// ---------------------------------------------------------------------------
// Errors

// json.SyntaxError: malformed JSON, found after reading offset bytes.
struct SyntaxError : errorInterface {
    string msg;
    int64 offset = 0;
    string Error() const override { return msg; }
};

// json.UnmarshalTypeError: a JSON value that doesn't fit the C++ type.
struct UnmarshalTypeError : errorInterface {
    string value;   // "string", "number 1.5", "array", ...
    string type;    // the C++ type
    int64 offset = 0;
    string structName;
    string field;
    UnmarshalTypeError(string value, string type, int64 offset, string structName, string field)
        : value(std::move(value)), type(std::move(type)), offset(offset), structName(std::move(structName)),
          field(std::move(field)) {}
    string Error() const override;
};

// json.UnsupportedValueError: NaN or infinity.
struct UnsupportedValueError : errorInterface {
    string str;
    string Error() const override { return "json: unsupported value: " + str; }
};

// json.InvalidUnmarshalError: unmarshal into a null pointer.
struct InvalidUnmarshalError : errorInterface {
    string type;
    explicit InvalidUnmarshalError(string t) : type(std::move(t)) {}
    string Error() const override { return "json: Unmarshal(nil *" + type + ")"; }
};

// json.MarshalerError: a MarshalJSON member failed or returned bad JSON.
struct MarshalerError : errorInterface {
    string type;
    gopp::error Err;
    string sourceFunc;  // "MarshalJSON" if empty
    string Error() const override {
        return "json: error calling " + (sourceFunc.empty() ? string::fromStatic("MarshalJSON") : sourceFunc) +
               " for type " + type + ": " + Err.Error();
    }
    const errorInterface* Unwrap() const noexcept override { return Err.get(); }
};

// ---------------------------------------------------------------------------
// Annotations

// A field's JSON options, as in a Go struct tag: "name", "name,omitempty",
// ",omitempty", "name,string" (a number or bool as a JSON string), or "-"
// to skip the field.
struct tag {
    char spec[128]{};
    consteval tag(const char* s) {
        std::size_t n = 0;
        while (s[n]) {
            if (n + 1 >= sizeof(spec)) throw "json::tag too long";
            spec[n] = s[n];
            n++;
        }
    }
};

// ---------------------------------------------------------------------------
// Value: any JSON value, like Go's any after decoding (nil, bool, float64,
// string, []any, map[string]any).

class Value {
public:
    using array = std::vector<Value>;
    using object = std::map<string, Value, std::less<>>;
    enum class kind { null, boolean, number, string, array, object };

    Value() noexcept = default;
    Value(std::nullptr_t) noexcept {}
    Value(bool b) noexcept : v_(b) {}
    template <typename N>
        requires(std::is_arithmetic_v<N> && !std::is_same_v<N, bool>)
    Value(N n) noexcept : v_(static_cast<double>(n)) {}
    Value(const char* s) : v_(gopp::string(s)) {}
    Value(gopp::string s) : v_(std::move(s)) {}
    Value(std::string_view s) : v_(gopp::string(s)) {}
    Value(array a) : v_(std::move(a)) {}
    Value(object o) : v_(std::move(o)) {}

    Value(const Value&) = default;
    Value(Value&&) noexcept = default;
    Value& operator=(const Value&) = default;
    Value& operator=(Value&&) noexcept = default;
    // Iterative, so that deeply nested values don't overflow a fiber's stack.
    ~Value();

    kind type() const noexcept { return static_cast<kind>(v_.index()); }
    bool IsNull() const noexcept { return v_.index() == 0; }
    bool IsBool() const noexcept { return v_.index() == 1; }
    bool IsNumber() const noexcept { return v_.index() == 2; }
    bool IsString() const noexcept { return v_.index() == 3; }
    bool IsArray() const noexcept { return v_.index() == 4; }
    bool IsObject() const noexcept { return v_.index() == 5; }

    // Accessors panic if the value has another kind.
    bool AsBool() const { return get<bool>(); }
    double AsNumber() const { return get<double>(); }
    const gopp::string& AsString() const { return get<gopp::string>(); }
    const array& AsArray() const { return get<array>(); }
    array& AsArray() { return get<array>(); }
    const object& AsObject() const { return get<object>(); }
    object& AsObject() { return get<object>(); }

    // The member key; a null value if missing or not an object.
    const Value& operator[](std::string_view key) const;
    // The member key, inserted if missing; a null value becomes an object.
    Value& operator[](std::string_view key);
    // Element i of an array; panics when out of range.
    const Value& operator[](std::size_t i) const;
    // Elements of an array or members of an object; 0 otherwise.
    std::size_t size() const noexcept;

    friend bool operator==(const Value& a, const Value& b) { return a.v_ == b.v_; }

private:
    template <typename T>
    T& get() {
        if (auto* p = std::get_if<T>(&v_)) return *p;
        panic("json::Value: wrong kind");
    }
    template <typename T>
    const T& get() const {
        if (auto* p = std::get_if<T>(&v_)) return *p;
        panic("json::Value: wrong kind");
    }

    std::variant<std::nullptr_t, bool, double, gopp::string, array, object> v_;
};

// json.RawMessage: encoded JSON, kept as is.
struct RawMessage {
    slice<byte> data;
    std::pair<slice<byte>, gopp::error> MarshalJSON() const;
    gopp::error UnmarshalJSON(std::span<const byte> d);
};

// ---------------------------------------------------------------------------
// Implementation details used by the templates below.

namespace detail {

class encoder {
public:
    std::string out;
    bool escapeHtml = true;
    gopp::error err;
    // Pointer cycles are looked for once pointers nest this deep, as in Go.
    static constexpr int startDetectingCyclesAfter = 1000;
    int ptrLevel = 0;
    std::vector<const void*> ptrSeen;

    void WriteString(std::string_view s);
    void writeFloat(double f, int bits);
    void writeInt(int64 v) {
        char buf[24];
        auto r = std::to_chars(buf, buf + sizeof(buf), v);
        out.append(buf, r.ptr);
    }
    void writeUint(uint64 v) {
        char buf[24];
        auto r = std::to_chars(buf, buf + sizeof(buf), v);
        out.append(buf, r.ptr);
    }
    void writeBase64(std::span<const byte> b);
    void writeValue(const Value& v);
    // Appends the output of a MarshalJSON member, validated and compacted.
    void writeMarshaled(std::pair<slice<byte>, gopp::error> r, std::string_view type);
    void cycleError(std::string_view type);
};

// Parses JSON that was validated already, so only type mismatches can go
// wrong. The first mismatch is recorded and decoding goes on, as in Go.
class decoder {
public:
    explicit decoder(std::string_view data) : data_(data) {}

    bool disallowUnknown = false;
    gopp::error err;
    std::string_view structName;
    std::string field;

    // The first byte of the next value, after whitespace.
    char peek() noexcept;
    // Consumes null if it comes next.
    bool consumeNull() noexcept;
    // Consumes the next value and returns its text.
    std::string_view raw();
    void skip() { raw(); }
    // Consumes a literal (number, true, false) and returns its text.
    std::string_view literal();
    // Consumes and unquotes a string.
    void ReadString(std::string& out);
    void beginObject() noexcept;
    // The next key of the object, or false at its end.
    bool nextKey(std::string& key);
    void beginArray() noexcept;
    // Whether the array has another element.
    bool nextElement() noexcept;
    void readValue(Value& v);
    bool readBase64(slice<byte>& b);

    // Records a mismatch and skips the value.
    void mismatch(std::string_view type);
    // Records a value that doesn't fit type, such as an out of range number.
    void badValue(std::string_view value, std::string_view type);
    void fail(gopp::error e) {
        if (!err) err = std::move(e);
    }
    std::size_t offset() const noexcept { return pos_; }

private:
    void skipWs() noexcept;
    std::string_view data_;
    std::size_t pos_ = 0;
};

// Validates data as one JSON value; a SyntaxError if it isn't.
gopp::error validate(std::string_view data);
// For streams: the length of the first value in s (after whitespace), 0 if
// s ends before the value does, or a SyntaxError.
std::pair<std::size_t, gopp::error> valueLength(std::string_view s, bool atEof);
void indent(std::string& dst, std::string_view src, std::string_view prefix, std::string_view indent);
void appendCompact(std::string& dst, std::string_view src, bool escapeHtml);

template <typename T>
consteval std::string_view typeName() {
    return std::define_static_string(std::meta::display_string_of(^^T));
}

// ---------------------------------------------------------------------------
// Type categories

template <typename T>
struct isSlice : std::false_type {};
template <typename T>
struct isSlice<gopp::slice<T>> : std::true_type {};
template <typename T>
struct isVector : std::false_type {};
template <typename T, typename A>
struct isVector<std::vector<T, A>> : std::true_type {};
template <typename T>
struct isStdArray : std::false_type {};
template <typename T, std::size_t N>
struct isStdArray<std::array<T, N>> : std::true_type {};
template <typename T>
struct isMap : std::false_type {};
template <typename K, typename V, typename C, typename A>
struct isMap<std::map<K, V, C, A>> : std::true_type {};
template <typename K, typename V, typename H, typename E, typename A>
struct isMap<std::unordered_map<K, V, H, E, A>> : std::true_type {};
template <typename T>
struct isOptional : std::false_type {};
template <typename T>
struct isOptional<std::optional<T>> : std::true_type {};
// Pointers: gopp::ptr, std::shared_ptr and std::unique_ptr, like Go's *T.
template <typename T>
struct isPtr : std::false_type {};
template <typename T>
struct isPtr<gopp::ptr<T>> : std::true_type {};
template <typename T>
struct isPtr<std::shared_ptr<T>> : std::true_type {};
template <typename T, typename D>
struct isPtr<std::unique_ptr<T, D>> : std::true_type {};

template <typename P>
void allocate(P& p) {
    using E = typename P::element_type;
    if constexpr (std::is_same_v<P, gopp::ptr<E>>) p = make_ptr<E>();
    else if constexpr (std::is_same_v<P, std::shared_ptr<E>>) p = std::make_shared<E>();
    else p = P(new E());
}

// A pointer or optional holding a number, for the ",string" option.
template <typename T>
concept numberPointer = (isOptional<T>::value || isPtr<T>::value) &&
                         std::is_arithmetic_v<std::remove_cvref_t<decltype(*std::declval<T&>())>>;

template <typename T>
concept stringLike = std::is_same_v<T, gopp::string> || std::is_same_v<T, std::string> || std::is_same_v<T, std::string_view>;

template <typename T>
concept marshaler = requires(const T& t) {
    { t.MarshalJSON() } -> std::same_as<std::pair<slice<byte>, gopp::error>>;
};
template <typename T>
concept unmarshaler = requires(T& t, std::span<const byte> d) {
    { t.UnmarshalJSON(d) } -> std::same_as<gopp::error>;
};

// ---------------------------------------------------------------------------
// Struct fields by reflection

struct fieldSpec {
    std::string_view name;
    bool skip = false;
    bool omitempty = false;
    bool AsString = false;
};

template <typename T>
consteval auto fieldsOf() {
    return std::define_static_array(
        std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::unprivileged()));
}

// Go's isValidTag: letters, digits and most punctuation; not quotes or
// backslashes. Bytes of UTF-8 sequences count as letters.
consteval bool validTagName(std::string_view s) {
    if (s.empty()) return false;
    for (char ch : s) {
        unsigned char c = static_cast<unsigned char>(ch);
        if (std::string_view("!#$%&()*+-./:;<=>?@[]^_{|}~ ").find(ch) != std::string_view::npos) continue;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c >= 0x80) continue;
        return false;
    }
    return true;
}

consteval fieldSpec parseTag(std::string_view member, std::string_view spec) {
    fieldSpec f{member};
    std::size_t comma = spec.find(',');
    std::string_view name = spec.substr(0, comma);
    if (name == "-" && comma == std::string_view::npos) {
        f.skip = true;
        return f;
    }
    if (validTagName(name)) f.name = std::define_static_string(name);
    while (comma != std::string_view::npos) {
        spec = spec.substr(comma + 1);
        comma = spec.find(',');
        std::string_view opt = spec.substr(0, comma);
        if (opt == "omitempty") f.omitempty = true;
        else if (opt == "string") f.AsString = true;
    }
    return f;
}

template <std::meta::info M>
consteval fieldSpec specOf() {
    std::string_view member = std::define_static_string(std::meta::identifier_of(M));
    for (std::meta::info a : std::meta::annotations_of_with_type(M, ^^json::tag)) {
        json::tag t = std::meta::extract<json::tag>(a);
        return parseTag(member, std::string_view(t.spec));
    }
    return fieldSpec{member};
}

// "name": with the quotes and escapes, for writing keys. Tag names can't
// hold quotes or backslashes; with html, <, > and & are escaped too.
template <std::meta::info M, bool html>
consteval std::string_view quotedKey() {
    std::string s = "\"";
    for (char c : specOf<M>().name) {
        if (html && c == '<') s += "\\u003c";
        else if (html && c == '>') s += "\\u003e";
        else if (html && c == '&') s += "\\u0026";
        else s += c;
    }
    s += "\":";
    return std::define_static_string(s);
}

bool EqualFold(std::string_view a, std::string_view b) noexcept;

// ---------------------------------------------------------------------------
// Encoding

template <typename T>
bool isEmptyValue(const T& v) {
    if constexpr (std::is_same_v<T, bool>) return !v;
    else if constexpr (std::is_arithmetic_v<T> || std::is_enum_v<T>) return v == T{};
    else if constexpr (stringLike<T>) return v.empty();
    else if constexpr (isSlice<T>::value || isVector<T>::value || isMap<T>::value) return v.empty();
    else if constexpr (isStdArray<T>::value) return std::tuple_size_v<T> == 0;
    else if constexpr (isOptional<T>::value) return !v.has_value();
    else if constexpr (isPtr<T>::value) return !v;
    else if constexpr (std::is_same_v<T, Value>) return v.IsNull();
    else if constexpr (std::is_same_v<T, time::Duration>) return v.ns == 0;
    else if constexpr (std::is_same_v<T, RawMessage>) return v.data.empty();  // a []byte in Go
    else return false;  // structs and times are never empty, as in Go
}

template <typename T>
void encode(encoder& e, const T& v);

template <typename K>
std::string mapKey(const K& k) {
    if constexpr (stringLike<K>) return std::string(std::string_view(k));
    else if constexpr (std::is_integral_v<K>) return std::to_string(k);
    else static_assert(sizeof(K) == 0, "json: map keys must be strings or integers");
}

template <typename T>
void encodeStruct(encoder& e, const T& v) {
    e.out.push_back('{');
    bool first = true;
    template for (constexpr std::meta::info m : fieldsOf<T>()) {
        constexpr fieldSpec f = specOf<m>();
        if constexpr (!f.skip) {
            const auto& fv = v.[:m:];
            if (!f.omitempty || !isEmptyValue(fv)) {
                if (!first) e.out.push_back(',');
                first = false;
                e.out.append(e.escapeHtml ? quotedKey<m, true>() : quotedKey<m, false>());
                if constexpr (f.AsString) {
                    using F = std::remove_cvref_t<decltype(fv)>;
                    if constexpr (std::is_arithmetic_v<F>) {
                        e.out.push_back('"');
                        encode(e, fv);
                        e.out.push_back('"');
                    } else if constexpr (numberPointer<F>) {
                        // A pointer to a number: quoted, or null.
                        if (!fv) {
                            e.out.append("null");
                        } else {
                            e.out.push_back('"');
                            encode(e, *fv);
                            e.out.push_back('"');
                        }
                    } else if constexpr (stringLike<F>) {
                        encoder inner;
                        inner.escapeHtml = e.escapeHtml;
                        inner.WriteString(fv);
                        e.WriteString(inner.out);
                    } else {
                        encode(e, fv);
                    }
                } else {
                    encode(e, fv);
                }
            }
        }
    }
    e.out.push_back('}');
}

template <typename T>
void encode(encoder& e, const T& v) {
    using U = std::remove_cvref_t<T>;
    if constexpr (marshaler<U>) {
        e.writeMarshaled(v.MarshalJSON(), typeName<U>());
    } else if constexpr (std::is_same_v<U, bool>) {
        e.out.append(v ? "true" : "false");
    } else if constexpr (std::is_floating_point_v<U>) {
        e.writeFloat(static_cast<double>(v), sizeof(U) == 4 ? 32 : 64);
    } else if constexpr (std::is_integral_v<U>) {
        if constexpr (std::is_signed_v<U>) e.writeInt(static_cast<int64>(v));
        else e.writeUint(static_cast<uint64>(v));
    } else if constexpr (std::is_enum_v<U>) {
        encode(e, static_cast<std::underlying_type_t<U>>(v));
    } else if constexpr (stringLike<U>) {
        e.WriteString(std::string_view(v));
    } else if constexpr (std::is_same_v<U, const char*> || std::is_same_v<U, char*>) {
        e.WriteString(v ? std::string_view(v) : std::string_view());
    } else if constexpr (std::is_same_v<U, slice<byte>>) {
        if (v.isNil()) e.out.append("null");
        else e.writeBase64(v);
    } else if constexpr (std::is_same_v<U, Value>) {
        e.writeValue(v);
    } else if constexpr (std::is_same_v<U, time::Duration>) {
        e.writeInt(v.ns);
    } else if constexpr (std::is_same_v<U, std::nullptr_t>) {
        e.out.append("null");
    } else if constexpr (isSlice<U>::value || isVector<U>::value || isStdArray<U>::value) {
        if constexpr (isSlice<U>::value) {
            if (v.isNil()) {
                e.out.append("null");
                return;
            }
        }
        e.out.push_back('[');
        bool first = true;
        for (const auto& x : v) {
            if (!first) e.out.push_back(',');
            first = false;
            encode(e, x);
        }
        e.out.push_back(']');
    } else if constexpr (isMap<U>::value) {
        // Sorted by key, as Go sorts map keys.
        std::vector<std::pair<std::string, const typename U::mapped_type*>> kv;
        kv.reserve(v.size());
        for (const auto& [k, x] : v) kv.emplace_back(mapKey(k), &x);
        std::sort(kv.begin(), kv.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        e.out.push_back('{');
        bool first = true;
        for (const auto& [k, x] : kv) {
            if (!first) e.out.push_back(',');
            first = false;
            e.WriteString(k);
            e.out.push_back(':');
            encode(e, *x);
        }
        e.out.push_back('}');
    } else if constexpr (isOptional<U>::value) {
        if (!v) e.out.append("null");
        else encode(e, *v);
    } else if constexpr (isPtr<U>::value) {
        if (!v) {
            e.out.append("null");
            return;
        }
        // Past a nesting depth, remember the pointers to find cycles.
        const void* p = v.get();
        bool tracked = ++e.ptrLevel > encoder::startDetectingCyclesAfter;
        if (tracked) {
            if (std::find(e.ptrSeen.begin(), e.ptrSeen.end(), p) != e.ptrSeen.end()) {
                e.cycleError(typeName<U>());
                e.ptrLevel--;
                return;
            }
            e.ptrSeen.push_back(p);
        }
        if (!e.err) encode(e, *v);
        if (tracked) e.ptrSeen.pop_back();
        e.ptrLevel--;
    } else if constexpr (std::is_class_v<U>) {
        encodeStruct(e, v);
    } else {
        static_assert(sizeof(U) == 0, "json: unsupported type");
    }
}

// ---------------------------------------------------------------------------
// Decoding

template <typename T>
void decode(decoder& d, T& v);

template <typename T>
void decodeNumber(decoder& d, T& v) {
    char c = d.peek();
    if (c != '-' && !(c >= '0' && c <= '9')) {
        d.mismatch(typeName<T>());
        return;
    }
    std::string_view lit = d.literal();
    if constexpr (std::is_floating_point_v<T>) {
        double x = 0;
        auto r = std::from_chars(lit.data(), lit.data() + lit.size(), x);
        if (r.ec != std::errc() || (sizeof(T) == 4 && (x > 3.4028234663852886e38 || x < -3.4028234663852886e38))) {
            d.badValue("number " + std::string(lit), typeName<T>());
            return;
        }
        v = static_cast<T>(x);
    } else {
        T x{};
        auto r = std::from_chars(lit.data(), lit.data() + lit.size(), x);
        if (r.ec != std::errc() || r.ptr != lit.data() + lit.size()) {
            d.badValue("number " + std::string(lit), typeName<T>());
            return;
        }
        v = x;
    }
}

template <typename K>
bool parseMapKey(decoder& d, const std::string& s, K& k) {
    if constexpr (std::is_same_v<K, gopp::string>) {
        k = gopp::string(s);
        return true;
    } else if constexpr (std::is_same_v<K, std::string>) {
        k = s;
        return true;
    } else if constexpr (std::is_integral_v<K>) {
        auto r = std::from_chars(s.data(), s.data() + s.size(), k);
        if (r.ec != std::errc() || r.ptr != s.data() + s.size()) {
            d.badValue("number " + s, typeName<K>());
            return false;
        }
        return true;
    } else {
        static_assert(sizeof(K) == 0, "json: map keys must be strings or integers");
    }
}

// A value of a ",string" field: JSON inside a JSON string. null leaves the
// value alone.
template <typename T>
void decodeQuoted(decoder& d, T& v) {
    if (d.peek() == '"') {
        std::string inner;
        d.ReadString(inner);
        if (gopp::error err = validate(inner)) {
            d.badValue("string", typeName<T>());
            return;
        }
        decoder sub(inner);
        decode(sub, v);
        if (sub.err) d.fail(sub.err);
    } else if (!d.consumeNull()) {
        d.mismatch(typeName<T>());
    }
}

template <typename T>
void decodeStruct(decoder& d, T& v) {
    if (d.peek() != '{') {
        d.mismatch(typeName<T>());
        return;
    }
    d.beginObject();
    std::string key;
    auto savedStruct = d.structName;
    while (d.nextKey(key)) {
        bool found = false;
        // An exact match wins; otherwise a case-insensitive one, as in Go.
        for (int pass = 0; pass < 2 && !found; pass++) {
            template for (constexpr std::meta::info m : fieldsOf<T>()) {
                constexpr fieldSpec f = specOf<m>();
                if constexpr (!f.skip) {
                    if (!found && (pass == 0 ? key == f.name : EqualFold(key, f.name))) {
                        found = true;
                        d.structName = typeName<T>();
                        std::string savedField = d.field;
                        d.field = d.field.empty() ? std::string(f.name) : d.field + "." + std::string(f.name);
                        using F = std::remove_cvref_t<decltype(v.[:m:])>;
                        if constexpr (f.AsString && (std::is_arithmetic_v<F> || stringLike<F>)) {
                            decodeQuoted(d, v.[:m:]);
                        } else if constexpr (f.AsString && numberPointer<F>) {
                            auto& p = v.[:m:];
                            if (d.consumeNull()) {
                                p.reset();
                            } else {
                                if constexpr (isOptional<F>::value) {
                                    if (!p) p.emplace();
                                } else {
                                    if (!p) allocate(p);
                                }
                                decodeQuoted(d, *p);
                            }
                        } else {
                            decode(d, v.[:m:]);
                        }
                        d.field = std::move(savedField);
                        d.structName = savedStruct;
                    }
                }
            }
        }
        if (!found) {
            if (d.disallowUnknown) d.fail(errors::New("json: unknown field \"" + string(key) + "\""));
            d.skip();
        }
    }
}

template <typename T>
void decode(decoder& d, T& v) {
    using U = std::remove_cvref_t<T>;
    if constexpr (unmarshaler<U>) {
        std::string_view raw = d.raw();
        if (gopp::error err = v.UnmarshalJSON(as_bytes(raw))) d.fail(err);
    } else if constexpr (std::is_same_v<U, Value>) {
        d.readValue(v);
    } else if constexpr (isOptional<U>::value) {
        if (d.consumeNull()) {
            v.reset();
            return;
        }
        if (!v) v.emplace();
        decode(d, *v);
    } else if constexpr (isPtr<U>::value) {
        if (d.consumeNull()) {
            v.reset();
            return;
        }
        if (!v) allocate(v);
        decode(d, *v);
    } else if constexpr (std::is_same_v<U, bool>) {
        if (d.consumeNull()) return;  // null leaves the value, as in Go
        char c = d.peek();
        if (c == 't' || c == 'f') v = d.literal() == "true";
        else d.mismatch("bool");
    } else if constexpr (std::is_arithmetic_v<U>) {
        if (d.consumeNull()) return;
        decodeNumber(d, v);
    } else if constexpr (std::is_enum_v<U>) {
        if (d.consumeNull()) return;
        std::underlying_type_t<U> x{};
        decodeNumber(d, x);
        v = static_cast<U>(x);
    } else if constexpr (std::is_same_v<U, time::Duration>) {
        if (d.consumeNull()) return;
        decodeNumber(d, v.ns);
    } else if constexpr (std::is_same_v<U, gopp::string> || std::is_same_v<U, std::string>) {
        if (d.consumeNull()) return;
        if (d.peek() != '"') {
            d.mismatch(typeName<U>());
            return;
        }
        std::string s;
        d.ReadString(s);
        v = U(s);
    } else if constexpr (std::is_same_v<U, slice<byte>>) {
        if (d.consumeNull()) {
            v = slice<byte>();
            return;
        }
        if (d.peek() != '"' || !d.readBase64(v)) d.mismatch("[]byte");
    } else if constexpr (isSlice<U>::value || isVector<U>::value) {
        if (d.consumeNull()) {
            v = U();
            return;
        }
        if (d.peek() != '[') {
            d.mismatch(typeName<U>());
            return;
        }
        d.beginArray();
        if constexpr (isVector<U>::value) {
            v.clear();
            while (d.nextElement()) decode(d, v.emplace_back());
        } else {
            U out = U::make(0);
            while (d.nextElement()) {
                typename U::value_type x{};
                decode(d, x);
                out = append(std::move(out), std::move(x));
            }
            v = std::move(out);
        }
    } else if constexpr (isStdArray<U>::value) {
        if (d.consumeNull()) return;
        if (d.peek() != '[') {
            d.mismatch(typeName<U>());
            return;
        }
        d.beginArray();
        std::size_t i = 0;
        while (d.nextElement()) {
            if (i < v.size()) decode(d, v[i++]);
            else d.skip();
        }
        for (; i < v.size(); i++) v[i] = typename U::value_type{};
    } else if constexpr (isMap<U>::value) {
        if (d.consumeNull()) {
            v.clear();
            return;
        }
        if (d.peek() != '{') {
            d.mismatch(typeName<U>());
            return;
        }
        d.beginObject();
        std::string key;
        while (d.nextKey(key)) {
            typename U::key_type k{};
            if (!parseMapKey(d, key, k)) {
                d.skip();
                continue;
            }
            decode(d, v[k]);
        }
    } else if constexpr (std::is_class_v<U>) {
        if (d.consumeNull()) return;
        decodeStruct(d, v);
    } else {
        static_assert(sizeof(U) == 0, "json: unsupported type");
    }
}

}  // namespace detail

// ---------------------------------------------------------------------------
// API

// json.Marshal
template <typename T>
std::pair<slice<byte>, error> Marshal(const T& v) {
    detail::encoder e;
    detail::encode(e, v);
    if (e.err) return {slice<byte>(), e.err};
    return {toBytes(e.out), nullptr};
}

// json.MarshalIndent
template <typename T>
std::pair<slice<byte>, error> MarshalIndent(const T& v, std::string_view prefix, std::string_view indent) {
    detail::encoder e;
    detail::encode(e, v);
    if (e.err) return {slice<byte>(), e.err};
    std::string out;
    detail::indent(out, e.out, prefix, indent);
    return {toBytes(out), nullptr};
}

// json.Unmarshal: data must hold one JSON value. Fields missing from data
// keep their values; unknown keys are ignored.
template <typename T>
error Unmarshal(std::span<const byte> data, T* v) {
    if (!v) return make_ptr<InvalidUnmarshalError>(string(detail::typeName<T>()));
    std::string_view s = asStringView(data);
    if (error err = detail::validate(s)) return err;
    detail::decoder d(s);
    detail::decode(d, *v);
    return d.err;
}

template <typename T>
error Unmarshal(std::string_view data, T* v) {
    return Unmarshal(as_bytes(data), v);
}

// json.Valid
bool Valid(std::span<const byte> data);
inline bool Valid(std::string_view data) { return Valid(as_bytes(data)); }

// json.Compact: appends src without insignificant space to dst. On
// error, dst is unchanged.
error Compact(std::string& dst, std::string_view src);
inline error Compact(std::string& dst, std::span<const byte> src) { return Compact(dst, asStringView(src)); }

// json.Indent: appends src to dst with each element on its own line,
// starting with prefix and nested with indent. Leading space in src is
// dropped and trailing space kept. On error, dst is unchanged.
error Indent(std::string& dst, std::string_view src, std::string_view prefix, std::string_view indent);
inline error Indent(std::string& dst, std::span<const byte> src, std::string_view prefix, std::string_view ind) {
    return Indent(dst, asStringView(src), prefix, ind);
}

// json.HTMLEscape: appends src to dst with <, >, &, U+2028 and U+2029 in
// strings escaped, so that it can be embedded in HTML <script> tags.
void HTMLEscape(std::string& dst, std::string_view src);
inline void HTMLEscape(std::string& dst, std::span<const byte> src) { HTMLEscape(dst, asStringView(src)); }

// json.Encoder: writes values to a stream, each followed by a newline.
class Encoder {
public:
    explicit Encoder(io::Writer& w) : w_(&w) {}

    template <typename T>
    error Encode(const T& v) {
        detail::encoder e;
        e.escapeHtml = escapeHtml_;
        detail::encode(e, v);
        if (e.err) return e.err;
        if (!prefix_.empty() || !indent_.empty()) {
            std::string out;
            detail::indent(out, e.out, prefix_, indent_);
            e.out = std::move(out);
        }
        e.out.push_back('\n');
        return io::WriteString(*w_, e.out).second;
    }

    void SetIndent(std::string_view prefix, std::string_view indent) {
        prefix_ = prefix;
        indent_ = indent;
    }
    // Whether to escape <, > and & in strings (the default), so that JSON
    // can be embedded in HTML.
    void SetEscapeHTML(bool on) noexcept { escapeHtml_ = on; }

private:
    io::Writer* w_;
    std::string prefix_, indent_;
    bool escapeHtml_ = true;
};

inline Encoder NewEncoder(io::Writer& w) { return Encoder(w); }

// Writes v to w as JSON and a newline: json.NewEncoder(w).Encode(v).
template <typename T>
error Encode(io::Writer& w, const T& v) {
    return Encoder(w).Encode(v);
}

// json.Decoder: reads successive values from a stream.
class Decoder {
public:
    explicit Decoder(io::Reader& r) : r_(&r) {}

    template <typename T>
    error Decode(T* v) {
        if (!v) return make_ptr<InvalidUnmarshalError>(string(detail::typeName<T>()));
        auto [value, err] = readValue();
        if (err) return err;
        detail::decoder d(value);
        d.disallowUnknown = disallowUnknown_;
        detail::decode(d, *v);
        return d.err;
    }

    // Whether another value follows in the stream.
    bool More();
    // Makes unknown object keys an error when decoding into structs.
    void DisallowUnknownFields() noexcept { disallowUnknown_ = true; }
    // Bytes consumed so far.
    int64 InputOffset() const noexcept { return offset_ + static_cast<int64>(scanp_); }

private:
    std::pair<std::string_view, error> readValue();
    error refill();

    io::Reader* r_;
    std::string buf_;
    std::size_t scanp_ = 0;
    int64 offset_ = 0;
    error err_;
    bool disallowUnknown_ = false;
};

inline Decoder NewDecoder(io::Reader& r) { return Decoder(r); }

// Reads one JSON value from r into v: json.NewDecoder(r).Decode(v). Reads
// ahead, so use a Decoder for several values from one stream.
template <typename T>
error Decode(io::Reader& r, T* v) {
    return Decoder(r).Decode(v);
}

}  // namespace gopp::json
