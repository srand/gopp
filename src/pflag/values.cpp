// After spf13/pflag v1.0.10's value types: bool.go, string.go, int*.go,
// uint*.go, float*.go, duration.go, count.go, bytes.go, ip.go, ipmask.go,
// ipnet.go, time.go, func.go, bool_func.go, text.go, and the slices and
// maps (*_slice.go, string_array.go, string_to_*.go).
#include <gopp/bytes.hpp>
#include <gopp/encoding/base64.hpp>
#include <gopp/encoding/csv.hpp>
#include <gopp/encoding/hex.hpp>
#include <gopp/fmt.hpp>
#include <gopp/pflag.hpp>
#include <gopp/strings.hpp>

#include <cctype>
#include <string>

namespace gopp::pflag {

namespace {

using std::string_view;

// The types' names, as template arguments.
constexpr char kInt[] = "int";
constexpr char kInt8[] = "int8";
constexpr char kInt16[] = "int16";
constexpr char kInt32[] = "int32";
constexpr char kInt64[] = "int64";
constexpr char kUint[] = "uint";
constexpr char kUint8[] = "uint8";
constexpr char kUint16[] = "uint16";
constexpr char kUint32[] = "uint32";
constexpr char kUint64[] = "uint64";
constexpr char kFloat32[] = "float32";
constexpr char kFloat64[] = "float64";

// A Value that sets a variable: the caller's, or one of its own (for the
// forms that return a pointer).
template <typename T>
class basicValue : public Value {
public:
    using valueType = T;
    basicValue(T* p, T value) : p_(p ? p : &own_) { *p_ = std::move(value); }
    T* target() const noexcept { return p_; }

protected:
    T own_{};
    T* p_;
};

// readAsCSV: the fields of one CSV record.
std::pair<std::vector<gopp::string>, gopp::error> readAsCSV(gopp::string val) {
    if (val.empty()) return {{}, nullptr};
    auto r = csv::NewReader(strings::NewReader(std::move(val)));
    return r->Read();
}

// writeAsCSV: one CSV record, without the newline.
gopp::string writeAsCSV(const std::vector<gopp::string>& vals) {
    auto b = make_ptr<bytes::Buffer>();
    auto w = csv::NewWriter(b);
    w->Write(vals);
    w->Flush();
    return strings::TrimSuffix(b->String(), "\n");
}

gopp::string removeQuotes(string_view s) {
    std::string out;
    for (char c : s) {
        if (c != '"' && c != '\'' && c != '`') out += c;
    }
    return gopp::string(out);
}

std::vector<gopp::string> split(const gopp::string& s, string_view sep) {
    auto parts = strings::Split(s, sep);
    return {parts.begin(), parts.end()};
}

// ---------------------------------------------------------------------------
// Scalars

class boolValue final : public basicValue<bool> {
public:
    using basicValue::basicValue;
    static constexpr string_view typeName = "bool";
    gopp::error Set(gopp::string s) override {
        auto [v, err] = strconv::ParseBool(s);
        *p_ = v;
        return err;
    }
    gopp::string Type() const override { return gopp::string::fromStatic(typeName); }
    gopp::string String() const override { return *p_ ? "true"_s : "false"_s; }
};

class stringValue final : public basicValue<gopp::string> {
public:
    using basicValue::basicValue;
    static constexpr string_view typeName = "string";
    gopp::error Set(gopp::string s) override {
        *p_ = std::move(s);
        return nullptr;
    }
    gopp::string Type() const override { return gopp::string::fromStatic(typeName); }
    gopp::string String() const override { return *p_; }
};

template <typename T, int bits, const char* name>
class intValue final : public basicValue<T> {
public:
    using basicValue<T>::basicValue;
    static constexpr string_view typeName = name;
    gopp::error Set(gopp::string s) override {
        auto [v, err] = strconv::ParseInt(s, 0, bits);
        *this->p_ = static_cast<T>(v);
        return err;
    }
    gopp::string Type() const override { return gopp::string::fromStatic(typeName); }
    gopp::string String() const override { return strconv::FormatInt(*this->p_, 10); }
};

template <typename T, int bits, const char* name>
class uintValue final : public basicValue<T> {
public:
    using basicValue<T>::basicValue;
    static constexpr string_view typeName = name;
    gopp::error Set(gopp::string s) override {
        auto [v, err] = strconv::ParseUint(s, 0, bits);
        *this->p_ = static_cast<T>(v);
        return err;
    }
    gopp::string Type() const override { return gopp::string::fromStatic(typeName); }
    gopp::string String() const override { return strconv::FormatUint(*this->p_, 10); }
};

template <typename T, int bits, const char* name>
class floatValue final : public basicValue<T> {
public:
    using basicValue<T>::basicValue;
    static constexpr string_view typeName = name;
    gopp::error Set(gopp::string s) override {
        auto [v, err] = strconv::ParseFloat(s, bits);
        *this->p_ = static_cast<T>(v);
        return err;
    }
    gopp::string Type() const override { return gopp::string::fromStatic(typeName); }
    gopp::string String() const override { return strconv::FormatFloat(*this->p_, 'g', -1, bits); }
};

class durationValue final : public basicValue<time::Duration> {
public:
    using basicValue::basicValue;
    static constexpr string_view typeName = "duration";
    gopp::error Set(gopp::string s) override {
        auto [v, err] = time::ParseDuration(s);
        *p_ = v;
        return err;
    }
    gopp::string Type() const override { return gopp::string::fromStatic(typeName); }
    gopp::string String() const override { return p_->String(); }
};

class countValue final : public basicValue<isize> {
public:
    using basicValue::basicValue;
    static constexpr string_view typeName = "count";
    gopp::error Set(gopp::string s) override {
        if (s == "+1") {
            *p_ += 1;
            return nullptr;
        }
        auto [v, err] = strconv::ParseInt(s, 0, 0);
        *p_ = static_cast<isize>(v);
        return err;
    }
    gopp::string Type() const override { return gopp::string::fromStatic(typeName); }
    gopp::string String() const override { return strconv::Itoa(*p_); }
};

class bytesHexValue final : public basicValue<slice<byte>> {
public:
    using basicValue::basicValue;
    static constexpr string_view typeName = "bytesHex";
    gopp::error Set(gopp::string s) override {
        auto [bin, err] = hex::DecodeString(strings::TrimSpace(s));
        if (err) return err;
        *p_ = std::move(bin);
        return nullptr;
    }
    gopp::string Type() const override { return gopp::string::fromStatic(typeName); }
    // fmt.Sprintf("%X", b)
    gopp::string String() const override { return strings::ToUpper(hex::EncodeToString(*p_)); }
};

class bytesBase64Value final : public basicValue<slice<byte>> {
public:
    using basicValue::basicValue;
    static constexpr string_view typeName = "bytesBase64";
    gopp::error Set(gopp::string s) override {
        auto [bin, err] = base64::StdEncoding.DecodeString(strings::TrimSpace(s));
        if (err) return err;
        *p_ = std::move(bin);
        return nullptr;
    }
    gopp::string Type() const override { return gopp::string::fromStatic(typeName); }
    gopp::string String() const override { return base64::StdEncoding.EncodeToString(*p_); }
};

class ipValue final : public basicValue<net::IP> {
public:
    using basicValue::basicValue;
    static constexpr string_view typeName = "ip";
    gopp::error Set(gopp::string s) override {
        if (s.empty()) return nullptr;
        net::IP ip = net::ParseIP(strings::TrimSpace(s));
        if (ip.isNil()) return fmt::Errorf("failed to parse IP: {}", strconv::Quote(s));
        *p_ = std::move(ip);
        return nullptr;
    }
    gopp::string Type() const override { return gopp::string::fromStatic(typeName); }
    gopp::string String() const override { return p_->String(); }
};

class ipMaskValue final : public basicValue<net::IPMask> {
public:
    using basicValue::basicValue;
    static constexpr string_view typeName = "ipMask";
    gopp::error Set(gopp::string s) override {
        net::IPMask m = ParseIPv4Mask(s);
        if (m.isNil()) return fmt::Errorf("failed to parse IP mask: {}", strconv::Quote(s));
        *p_ = std::move(m);
        return nullptr;
    }
    gopp::string Type() const override { return gopp::string::fromStatic(typeName); }
    gopp::string String() const override { return p_->String(); }
};

class ipNetValue final : public basicValue<net::IPNet> {
public:
    using basicValue::basicValue;
    static constexpr string_view typeName = "ipNet";
    gopp::error Set(gopp::string s) override {
        auto [ip, n, err] = net::ParseCIDR(strings::TrimSpace(s));
        if (err) return err;
        *p_ = *n;
        return nullptr;
    }
    gopp::string Type() const override { return gopp::string::fromStatic(typeName); }
    gopp::string String() const override { return p_->String(); }
};

class timeValue final : public basicValue<time::Time> {
public:
    static constexpr string_view typeName = "time";
    timeValue(time::Time* p, time::Time value, std::vector<gopp::string> formats = {})
        : basicValue(p, value), formats_(std::move(formats)) {}
    gopp::error Set(gopp::string s) override {
        s = strings::TrimSpace(s);
        for (auto& f : formats_) {
            auto [v, err] = time::Parse(f, s);
            if (err) continue;
            *p_ = v;
            return nullptr;
        }
        std::string formats;
        for (std::size_t i = 0; i < formats_.size(); i++) {
            if (i > 0) formats += ", ";
            formats += std::format("`{}`", formats_[i]);
        }
        return fmt::Errorf("invalid time format `{}` must be one of: {}", s, formats);
    }
    gopp::string Type() const override { return gopp::string::fromStatic(typeName); }
    gopp::string String() const override {
        if (p_->IsZero()) return {};
        return p_->Format(time::RFC3339Nano);
    }

private:
    std::vector<gopp::string> formats_;
};

class funcValue final : public Value {
public:
    funcValue(std::function<gopp::error(gopp::string)> fn, string_view type) : fn_(std::move(fn)), type_(type) {}
    gopp::error Set(gopp::string s) override { return fn_(std::move(s)); }
    gopp::string Type() const override { return gopp::string::fromStatic(type_); }
    // The same as the flag package's.
    gopp::string String() const override { return {}; }

private:
    std::function<gopp::error(gopp::string)> fn_;
    string_view type_;
};

class textValue final : public Value {
public:
    textValue(std::function<gopp::string()> str, std::function<gopp::error(gopp::string)> set)
        : str_(std::move(str)), set_(std::move(set)) {}
    gopp::error Set(gopp::string s) override { return set_(std::move(s)); }
    // reflect.Type.Name() of a pointer type, which is empty.
    gopp::string Type() const override { return {}; }
    gopp::string String() const override { return str_(); }

private:
    std::function<gopp::string()> str_;
    std::function<gopp::error(gopp::string)> set_;
};

// ---------------------------------------------------------------------------
// Lists: C, the codec, says how to split the text, parse and format the
// elements, and format the list.

template <typename E, typename C>
class listValue final : public basicValue<std::vector<E>>, public SliceValue {
public:
    using basicValue<std::vector<E>>::basicValue;
    static constexpr string_view typeName = C::typeName;
    static constexpr bool isList = true;

    gopp::error Set(gopp::string val) override {
        auto [parts, err] = C::split(val);
        if (err) return err;
        std::vector<E> out;
        out.reserve(parts.size());
        for (auto& part : parts) {
            E e{};
            if (auto err = C::parse(part, e)) return err;
            out.push_back(std::move(e));
        }
        if (!changed_) {
            *this->p_ = std::move(out);
        } else {
            for (auto&& e : out) this->p_->push_back(std::move(e));
        }
        changed_ = true;
        return nullptr;
    }
    gopp::string Type() const override { return gopp::string::fromStatic(typeName); }
    gopp::string String() const override { return C::format(*this->p_); }

    gopp::error Append(gopp::string val) override {
        E e{};
        if (auto err = C::fromString(val, e)) return err;
        this->p_->push_back(std::move(e));
        return nullptr;
    }
    gopp::error Replace(std::vector<gopp::string> val) override {
        std::vector<E> out;
        out.reserve(val.size());
        for (auto& d : val) {
            E e{};
            if (auto err = C::fromString(d, e)) return err;
            out.push_back(std::move(e));
        }
        *this->p_ = std::move(out);
        return nullptr;
    }
    std::vector<gopp::string> GetSlice() const override {
        std::vector<gopp::string> out;
        out.reserve(this->p_->size());
        for (const auto& e : *this->p_) out.push_back(C::toString(e));
        return out;
    }

private:
    bool changed_ = false;
};

// Codec parts.

// Comma-separated, without quoting.
struct commaSplit {
    static std::pair<std::vector<gopp::string>, gopp::error> split(const gopp::string& s) { return {pflag::split(s, ","), nullptr}; }
};

// One CSV record.
struct csvSplit {
    static std::pair<std::vector<gopp::string>, gopp::error> split(const gopp::string& s) { return readAsCSV(s); }
};

// A CSV record after removing quotes; EOF (an empty record) is no error.
struct unquotedCSVSplit {
    static std::pair<std::vector<gopp::string>, gopp::error> split(const gopp::string& s) {
        auto [parts, err] = readAsCSV(removeQuotes(s));
        if (err && err != io::EOF_) return {{}, err};
        return {std::move(parts), nullptr};
    }
};

// fromString is parse.
template <typename C, typename E>
struct sameParse {
    static gopp::error fromString(const gopp::string& s, E& e) { return C::parse(s, e); }
};

// "[" + the elements joined by commas + "]".
template <typename C, typename E>
struct joinedFormat {
    static gopp::string format(const std::vector<E>& v) {
        std::string out = "[";
        for (std::size_t i = 0; i < v.size(); i++) {
            if (i > 0) out += ',';
            out += C::toString(v[i]).view();
        }
        return gopp::string(out + "]");
    }
};

// "[" + the elements as a CSV record + "]".
template <typename C, typename E>
struct csvFormat {
    static gopp::string format(const std::vector<E>& v) {
        std::vector<gopp::string> strs;
        strs.reserve(v.size());
        for (const auto& e : v) strs.push_back(C::toString(e));
        return "["_s + writeAsCSV(strs) + "]";
    }
};

struct stringSliceCodec : csvSplit, sameParse<stringSliceCodec, gopp::string>, csvFormat<stringSliceCodec, gopp::string> {
    static constexpr string_view typeName = "stringSlice";
    static gopp::error parse(const gopp::string& s, gopp::string& e) {
        e = s;
        return nullptr;
    }
    static gopp::string toString(const gopp::string& e) { return e; }
};
using stringSliceValue = listValue<gopp::string, stringSliceCodec>;

// Each flag is one element, commas and all.
struct stringArrayCodec : sameParse<stringArrayCodec, gopp::string>, csvFormat<stringArrayCodec, gopp::string> {
    static constexpr string_view typeName = "stringArray";
    static std::pair<std::vector<gopp::string>, gopp::error> split(const gopp::string& s) { return {{s}, nullptr}; }
    static gopp::error parse(const gopp::string& s, gopp::string& e) {
        e = s;
        return nullptr;
    }
    static gopp::string toString(const gopp::string& e) { return e; }
};
using stringArrayValue = listValue<gopp::string, stringArrayCodec>;

struct intSliceCodec : commaSplit, sameParse<intSliceCodec, isize>, joinedFormat<intSliceCodec, isize> {
    static constexpr string_view typeName = "intSlice";
    static gopp::error parse(const gopp::string& s, isize& e) {
        auto [v, err] = strconv::Atoi(s);
        e = v;
        return err;
    }
    static gopp::string toString(isize e) { return strconv::Itoa(e); }
};
using intSliceValue = listValue<isize, intSliceCodec>;

struct int32SliceCodec : commaSplit, sameParse<int32SliceCodec, int32>, joinedFormat<int32SliceCodec, int32> {
    static constexpr string_view typeName = "int32Slice";
    static gopp::error parse(const gopp::string& s, int32& e) {
        auto [v, err] = strconv::ParseInt(s, 0, 32);
        e = static_cast<int32>(v);
        return err;
    }
    static gopp::string toString(int32 e) { return strconv::Itoa(e); }
};
using int32SliceValue = listValue<int32, int32SliceCodec>;

struct int64SliceCodec : commaSplit, sameParse<int64SliceCodec, int64>, joinedFormat<int64SliceCodec, int64> {
    static constexpr string_view typeName = "int64Slice";
    static gopp::error parse(const gopp::string& s, int64& e) {
        auto [v, err] = strconv::ParseInt(s, 0, 64);
        e = v;
        return err;
    }
    static gopp::string toString(int64 e) { return strconv::Itoa(e); }
};
using int64SliceValue = listValue<int64, int64SliceCodec>;

struct uintSliceCodec : commaSplit, sameParse<uintSliceCodec, std::size_t>, joinedFormat<uintSliceCodec, std::size_t> {
    static constexpr string_view typeName = "uintSlice";
    static gopp::error parse(const gopp::string& s, std::size_t& e) {
        auto [v, err] = strconv::ParseUint(s, 10, 0);
        e = static_cast<std::size_t>(v);
        return err;
    }
    static gopp::string toString(std::size_t e) { return strconv::FormatUint(e, 10); }
};
using uintSliceValue = listValue<std::size_t, uintSliceCodec>;

template <typename F, int bits, const char* name>
struct floatSliceCodec : commaSplit, sameParse<floatSliceCodec<F, bits, name>, F>, joinedFormat<floatSliceCodec<F, bits, name>, F> {
    static constexpr string_view typeName = name;
    static gopp::error parse(const gopp::string& s, F& e) {
        auto [v, err] = strconv::ParseFloat(s, bits);
        e = static_cast<F>(v);
        return err;
    }
    // fmt.Sprintf("%f", e)
    static gopp::string toString(F e) { return gopp::string(std::format("{:f}", e)); }
};
constexpr char kFloat32Slice[] = "float32Slice";
constexpr char kFloat64Slice[] = "float64Slice";
using float32SliceValue = listValue<float, floatSliceCodec<float, 32, kFloat32Slice>>;
using float64SliceValue = listValue<double, floatSliceCodec<double, 64, kFloat64Slice>>;

struct boolSliceCodec : unquotedCSVSplit, csvFormat<boolSliceCodec, bool> {
    static constexpr string_view typeName = "boolSlice";
    static gopp::error parse(const gopp::string& s, bool& e) {
        auto [v, err] = strconv::ParseBool(strings::TrimSpace(s));
        e = v;
        return err;
    }
    static gopp::error fromString(const gopp::string& s, bool& e) {
        auto [v, err] = strconv::ParseBool(s);
        e = v;
        return err;
    }
    static gopp::string toString(bool e) { return e ? "true"_s : "false"_s; }
};
using boolSliceValue = listValue<bool, boolSliceCodec>;

struct durationSliceCodec : commaSplit,
                            sameParse<durationSliceCodec, time::Duration>,
                            joinedFormat<durationSliceCodec, time::Duration> {
    static constexpr string_view typeName = "durationSlice";
    static gopp::error parse(const gopp::string& s, time::Duration& e) {
        auto [v, err] = time::ParseDuration(s);
        e = v;
        return err;
    }
    static gopp::string toString(const time::Duration& e) { return e.String(); }
};
using durationSliceValue = listValue<time::Duration, durationSliceCodec>;

struct ipSliceCodec : unquotedCSVSplit, csvFormat<ipSliceCodec, net::IP> {
    static constexpr string_view typeName = "ipSlice";
    static gopp::error parse(const gopp::string& s, net::IP& e) {
        e = net::ParseIP(strings::TrimSpace(s));
        if (e.isNil()) return fmt::Errorf("invalid string being converted to IP address: {}", s);
        return nullptr;
    }
    static gopp::error fromString(const gopp::string& s, net::IP& e) {
        e = net::ParseIP(strings::TrimSpace(s));
        return nullptr;
    }
    static gopp::string toString(const net::IP& e) { return e.String(); }
};
using ipSliceValue = listValue<net::IP, ipSliceCodec>;

struct ipNetSliceCodec : unquotedCSVSplit, sameParse<ipNetSliceCodec, net::IPNet>, csvFormat<ipNetSliceCodec, net::IPNet> {
    static constexpr string_view typeName = "ipNetSlice";
    static gopp::error parse(const gopp::string& s, net::IPNet& e) {
        auto [ip, n, err] = net::ParseCIDR(strings::TrimSpace(s));
        if (err) return fmt::Errorf("invalid string being converted to CIDR: {}", s);
        e = *n;
        return nullptr;
    }
    static gopp::string toString(const net::IPNet& e) { return e.String(); }
};
using ipNetSliceValue = listValue<net::IPNet, ipNetSliceCodec>;

// ---------------------------------------------------------------------------
// Maps

template <typename V, typename C>
class mapValue final : public basicValue<std::map<gopp::string, V>> {
public:
    using basicValue<std::map<gopp::string, V>>::basicValue;
    static constexpr string_view typeName = C::typeName;
    static constexpr bool isList = true;

    gopp::error Set(gopp::string val) override {
        auto [out, err] = C::parse(val);
        if (err) return err;
        if (!changed_) {
            *this->p_ = std::move(out);
        } else {
            for (auto& [k, v] : out) (*this->p_)[k] = v;
        }
        changed_ = true;
        return nullptr;
    }
    gopp::string Type() const override { return gopp::string::fromStatic(typeName); }
    gopp::string String() const override { return C::format(*this->p_); }

private:
    bool changed_ = false;
};

gopp::error notKeyValue(const gopp::string& pair) { return fmt::Errorf("{} must be formatted as key=value", pair); }

struct stringToStringCodec {
    static constexpr string_view typeName = "stringToString";
    static std::pair<std::map<gopp::string, gopp::string>, gopp::error> parse(const gopp::string& val) {
        std::vector<gopp::string> ss;
        switch (strings::Count(val, "=")) {
        case 0: return {{}, notKeyValue(val)};
        case 1: ss.push_back(strings::Trim(val, "\"")); break;
        default: {
            auto [rec, err] = readAsCSV(val);
            if (err) return {{}, err};
            ss = std::move(rec);
        }
        }
        std::map<gopp::string, gopp::string> out;
        for (auto& pair : ss) {
            auto eq = pair.find('=');
            if (eq == gopp::string::npos) return {{}, notKeyValue(pair)};
            out[pair.slice(0, eq)] = pair.slice(eq + 1);
        }
        return {std::move(out), nullptr};
    }
    static gopp::string format(const std::map<gopp::string, gopp::string>& m) {
        std::vector<gopp::string> records;
        for (auto& [k, v] : m) records.push_back(k + "=" + v);
        return "["_s + strings::TrimSpace(writeAsCSV(records)) + "]";
    }
};
using stringToStringValue = mapValue<gopp::string, stringToStringCodec>;

template <typename V, bool atoi, const char* name>
struct stringToIntCodec {
    static constexpr string_view typeName = name;
    static std::pair<std::map<gopp::string, V>, gopp::error> parse(const gopp::string& val) {
        std::map<gopp::string, V> out;
        for (auto& pair : split(val, ",")) {
            auto eq = pair.find('=');
            if (eq == gopp::string::npos) return {{}, notKeyValue(pair)};
            gopp::error err;
            if constexpr (atoi) {
                auto [n, e] = strconv::Atoi(pair.slice(eq + 1));
                out[pair.slice(0, eq)] = static_cast<V>(n);
                err = e;
            } else {
                auto [n, e] = strconv::ParseInt(pair.slice(eq + 1), 10, 64);
                out[pair.slice(0, eq)] = static_cast<V>(n);
                err = e;
            }
            if (err) return {{}, err};
        }
        return {std::move(out), nullptr};
    }
    static gopp::string format(const std::map<gopp::string, V>& m) {
        std::string out = "[";
        bool first = true;
        for (auto& [k, v] : m) {
            if (!first) out += ',';
            first = false;
            out += k.view();
            out += '=';
            out += std::to_string(v);
        }
        return gopp::string(out + "]");
    }
};
constexpr char kStringToInt[] = "stringToInt";
constexpr char kStringToInt64[] = "stringToInt64";
using stringToIntValue = mapValue<isize, stringToIntCodec<isize, true, kStringToInt>>;
using stringToInt64Value = mapValue<int64, stringToIntCodec<int64, false, kStringToInt64>>;

// ---------------------------------------------------------------------------
// Definitions and getters

template <typename V, typename T>
Flag* define(FlagSet& f, T* p, string_view name, string_view shorthand, T value, string_view usage) {
    return f.VarPF(make_ptr<V>(p, std::move(value)), name, shorthand, usage);
}

template <typename V>
std::pair<typename V::valueType, gopp::error> getValue(const FlagSet& f, string_view name) {
    using T = typename V::valueType;
    Flag* flag = f.Lookup(name);
    if (!flag) return {T{}, make_ptr<NotExistError>(gopp::string(name), gopp::string(), NotExistError::flagNotDefined)};
    if (flag->Value->Type() != V::typeName) {
        return {T{}, fmt::Errorf("trying to get {} value of flag of type {}", V::typeName, flag->Value->Type())};
    }
    if (auto* v = dynamic_cast<V*>(flag->Value.get())) return {*v->target(), nullptr};
    // Another Value of the type: convert its text, as pflag does.
    gopp::string s = flag->Value->String();
    if constexpr (requires { V::isList; }) s = strings::Trim(s, "[]");
    V tmp(nullptr, T{});
    if (auto err = tmp.Set(s)) return {T{}, err};
    return {*tmp.target(), nullptr};
}

}  // namespace

ptr<Value> detail::newTextValue(std::function<gopp::string()> str, std::function<gopp::error(gopp::string)> set) {
    return make_ptr<textValue>(std::move(str), std::move(set));
}

net::IPMask ParseIPv4Mask(string_view s) {
    net::IP mask = net::ParseIP(s);
    if (mask.isNil()) {
        if (s.size() != 8) return {};
        // net::IPMask::String() gives ffffff00: parse that as well.
        int m[4];
        for (int i = 0; i < 4; i++) {
            auto [d, err] = strconv::ParseInt(gopp::string("0x"_s + gopp::string(s.substr(static_cast<std::size_t>(2 * i), 2))), 0, 0);
            if (err) return {};
            m[i] = static_cast<int>(d);
        }
        mask = net::ParseIP(std::format("{}.{}.{}.{}", m[0], m[1], m[2], m[3]));
        if (mask.isNil()) return {};
    }
    return net::IPv4Mask(mask[12], mask[13], mask[14], mask[15]);
}

// Count, Time, Func, BoolFunc.

void FlagSet::CountVar(isize* p, string_view name, string_view usage) { CountVarP(p, name, {}, usage); }
void FlagSet::CountVarP(isize* p, string_view name, string_view shorthand, string_view usage) {
    Flag* f = define<countValue>(*this, p, name, shorthand, isize(0), usage);
    f->NoOptDefVal = "+1"_s;
}
isize* FlagSet::Count(string_view name, string_view usage) { return CountP(name, {}, usage); }
isize* FlagSet::CountP(string_view name, string_view shorthand, string_view usage) {
    auto v = make_ptr<countValue>(nullptr, 0);
    isize* p = v->target();
    VarPF(v, name, shorthand, usage)->NoOptDefVal = "+1"_s;
    return p;
}
std::pair<isize, gopp::error> FlagSet::GetCount(string_view name) const { return getValue<countValue>(*this, name); }

void FlagSet::TimeVar(time::Time* p, string_view name, time::Time value, std::vector<gopp::string> formats, string_view usage) {
    TimeVarP(p, name, {}, value, std::move(formats), usage);
}
void FlagSet::TimeVarP(time::Time* p, string_view name, string_view shorthand, time::Time value, std::vector<gopp::string> formats,
                       string_view usage) {
    VarP(make_ptr<timeValue>(p, value, std::move(formats)), name, shorthand, usage);
}
time::Time* FlagSet::Time(string_view name, time::Time value, std::vector<gopp::string> formats, string_view usage) {
    return TimeP(name, {}, value, std::move(formats), usage);
}
time::Time* FlagSet::TimeP(string_view name, string_view shorthand, time::Time value, std::vector<gopp::string> formats,
                           string_view usage) {
    auto v = make_ptr<timeValue>(nullptr, value, std::move(formats));
    time::Time* p = v->target();
    VarP(v, name, shorthand, usage);
    return p;
}
std::pair<time::Time, gopp::error> FlagSet::GetTime(string_view name) const {
    Flag* flag = Lookup(name);
    if (!flag) return {{}, fmt::Errorf("flag accessed but not defined: {}", name)};
    if (flag->Value->Type() != "time") return {{}, fmt::Errorf("trying to get {} value of flag of type {}", "time", flag->Value->Type())};
    auto* v = dynamic_cast<timeValue*>(flag->Value.get());
    if (!v) return {{}, fmt::Errorf("value {} is not a time", flag->Value->String())};
    return {*v->target(), nullptr};
}

void FlagSet::Func(string_view name, string_view usage, std::function<gopp::error(gopp::string)> fn) { FuncP(name, {}, usage, std::move(fn)); }
void FlagSet::FuncP(string_view name, string_view shorthand, string_view usage, std::function<gopp::error(gopp::string)> fn) {
    VarP(make_ptr<funcValue>(std::move(fn), "func"), name, shorthand, usage);
}
void FlagSet::BoolFunc(string_view name, string_view usage, std::function<gopp::error(gopp::string)> fn) {
    BoolFuncP(name, {}, usage, std::move(fn));
}
void FlagSet::BoolFuncP(string_view name, string_view shorthand, string_view usage, std::function<gopp::error(gopp::string)> fn) {
    VarPF(make_ptr<funcValue>(std::move(fn), "boolfunc"), name, shorthand, usage)->NoOptDefVal = "true"_s;
}

void CountVar(isize* p, string_view name, string_view usage) { CommandLine().CountVarP(p, name, {}, usage); }
void CountVarP(isize* p, string_view name, string_view shorthand, string_view usage) { CommandLine().CountVarP(p, name, shorthand, usage); }
isize* Count(string_view name, string_view usage) { return CommandLine().CountP(name, {}, usage); }
isize* CountP(string_view name, string_view shorthand, string_view usage) { return CommandLine().CountP(name, shorthand, usage); }
void TimeVar(time::Time* p, string_view name, time::Time value, std::vector<gopp::string> formats, string_view usage) {
    CommandLine().TimeVarP(p, name, {}, value, std::move(formats), usage);
}
void TimeVarP(time::Time* p, string_view name, string_view shorthand, time::Time value, std::vector<gopp::string> formats,
              string_view usage) {
    CommandLine().TimeVarP(p, name, shorthand, value, std::move(formats), usage);
}
time::Time* Time(string_view name, time::Time value, std::vector<gopp::string> formats, string_view usage) {
    return CommandLine().TimeP(name, {}, value, std::move(formats), usage);
}
time::Time* TimeP(string_view name, string_view shorthand, time::Time value, std::vector<gopp::string> formats, string_view usage) {
    return CommandLine().TimeP(name, shorthand, value, std::move(formats), usage);
}
void Func(string_view name, string_view usage, std::function<gopp::error(gopp::string)> fn) {
    CommandLine().FuncP(name, {}, usage, std::move(fn));
}
void FuncP(string_view name, string_view shorthand, string_view usage, std::function<gopp::error(gopp::string)> fn) {
    CommandLine().FuncP(name, shorthand, usage, std::move(fn));
}
void BoolFunc(string_view name, string_view usage, std::function<gopp::error(gopp::string)> fn) {
    CommandLine().BoolFuncP(name, {}, usage, std::move(fn));
}
void BoolFuncP(string_view name, string_view shorthand, string_view usage, std::function<gopp::error(gopp::string)> fn) {
    CommandLine().BoolFuncP(name, shorthand, usage, std::move(fn));
}

// The flags of each type.

void FlagSet::BoolVar(bool* p, std::string_view name, bool value, std::string_view usage) { BoolVarP(p, name, {}, std::move(value), usage); }
void FlagSet::BoolVarP(bool* p, std::string_view name, std::string_view shorthand, bool value, std::string_view usage) {
    Flag* f = define<boolValue>(*this, p, name, shorthand, std::move(value), usage);
    f->NoOptDefVal = "true"_s;
}
bool* FlagSet::Bool(std::string_view name, bool value, std::string_view usage) { return BoolP(name, {}, std::move(value), usage); }
bool* FlagSet::BoolP(std::string_view name, std::string_view shorthand, bool value, std::string_view usage) {
    auto v = make_ptr<boolValue>(nullptr, std::move(value));
    bool* p = v->target();
    Flag* f = VarPF(v, name, shorthand, usage);
    f->NoOptDefVal = "true"_s;
    (void)f;
    return p;
}
std::pair<bool, gopp::error> FlagSet::GetBool(std::string_view name) const { return getValue<boolValue>(*this, name); }
void BoolVar(bool* p, std::string_view name, bool value, std::string_view usage) { CommandLine().BoolVarP(p, name, {}, std::move(value), usage); }
void BoolVarP(bool* p, std::string_view name, std::string_view shorthand, bool value, std::string_view usage) { CommandLine().BoolVarP(p, name, shorthand, std::move(value), usage); }
bool* Bool(std::string_view name, bool value, std::string_view usage) { return CommandLine().BoolP(name, {}, std::move(value), usage); }
bool* BoolP(std::string_view name, std::string_view shorthand, bool value, std::string_view usage) { return CommandLine().BoolP(name, shorthand, std::move(value), usage); }

void FlagSet::StringVar(gopp::string* p, std::string_view name, gopp::string value, std::string_view usage) { StringVarP(p, name, {}, std::move(value), usage); }
void FlagSet::StringVarP(gopp::string* p, std::string_view name, std::string_view shorthand, gopp::string value, std::string_view usage) {
    define<stringValue>(*this, p, name, shorthand, std::move(value), usage);
}
gopp::string* FlagSet::String(std::string_view name, gopp::string value, std::string_view usage) { return StringP(name, {}, std::move(value), usage); }
gopp::string* FlagSet::StringP(std::string_view name, std::string_view shorthand, gopp::string value, std::string_view usage) {
    auto v = make_ptr<stringValue>(nullptr, std::move(value));
    gopp::string* p = v->target();
    VarPF(v, name, shorthand, usage);
    return p;
}
std::pair<gopp::string, gopp::error> FlagSet::GetString(std::string_view name) const { return getValue<stringValue>(*this, name); }
void StringVar(gopp::string* p, std::string_view name, gopp::string value, std::string_view usage) { CommandLine().StringVarP(p, name, {}, std::move(value), usage); }
void StringVarP(gopp::string* p, std::string_view name, std::string_view shorthand, gopp::string value, std::string_view usage) { CommandLine().StringVarP(p, name, shorthand, std::move(value), usage); }
gopp::string* String(std::string_view name, gopp::string value, std::string_view usage) { return CommandLine().StringP(name, {}, std::move(value), usage); }
gopp::string* StringP(std::string_view name, std::string_view shorthand, gopp::string value, std::string_view usage) { return CommandLine().StringP(name, shorthand, std::move(value), usage); }

void FlagSet::IntVar(isize* p, std::string_view name, isize value, std::string_view usage) { IntVarP(p, name, {}, std::move(value), usage); }
void FlagSet::IntVarP(isize* p, std::string_view name, std::string_view shorthand, isize value, std::string_view usage) {
    define<intValue<isize, 64, kInt>>(*this, p, name, shorthand, std::move(value), usage);
}
isize* FlagSet::Int(std::string_view name, isize value, std::string_view usage) { return IntP(name, {}, std::move(value), usage); }
isize* FlagSet::IntP(std::string_view name, std::string_view shorthand, isize value, std::string_view usage) {
    auto v = make_ptr<intValue<isize, 64, kInt>>(nullptr, std::move(value));
    isize* p = v->target();
    VarPF(v, name, shorthand, usage);
    return p;
}
std::pair<isize, gopp::error> FlagSet::GetInt(std::string_view name) const { return getValue<intValue<isize, 64, kInt>>(*this, name); }
void IntVar(isize* p, std::string_view name, isize value, std::string_view usage) { CommandLine().IntVarP(p, name, {}, std::move(value), usage); }
void IntVarP(isize* p, std::string_view name, std::string_view shorthand, isize value, std::string_view usage) { CommandLine().IntVarP(p, name, shorthand, std::move(value), usage); }
isize* Int(std::string_view name, isize value, std::string_view usage) { return CommandLine().IntP(name, {}, std::move(value), usage); }
isize* IntP(std::string_view name, std::string_view shorthand, isize value, std::string_view usage) { return CommandLine().IntP(name, shorthand, std::move(value), usage); }

void FlagSet::Int8Var(int8* p, std::string_view name, int8 value, std::string_view usage) { Int8VarP(p, name, {}, std::move(value), usage); }
void FlagSet::Int8VarP(int8* p, std::string_view name, std::string_view shorthand, int8 value, std::string_view usage) {
    define<intValue<int8, 8, kInt8>>(*this, p, name, shorthand, std::move(value), usage);
}
int8* FlagSet::Int8(std::string_view name, int8 value, std::string_view usage) { return Int8P(name, {}, std::move(value), usage); }
int8* FlagSet::Int8P(std::string_view name, std::string_view shorthand, int8 value, std::string_view usage) {
    auto v = make_ptr<intValue<int8, 8, kInt8>>(nullptr, std::move(value));
    int8* p = v->target();
    VarPF(v, name, shorthand, usage);
    return p;
}
std::pair<int8, gopp::error> FlagSet::GetInt8(std::string_view name) const { return getValue<intValue<int8, 8, kInt8>>(*this, name); }
void Int8Var(int8* p, std::string_view name, int8 value, std::string_view usage) { CommandLine().Int8VarP(p, name, {}, std::move(value), usage); }
void Int8VarP(int8* p, std::string_view name, std::string_view shorthand, int8 value, std::string_view usage) { CommandLine().Int8VarP(p, name, shorthand, std::move(value), usage); }
int8* Int8(std::string_view name, int8 value, std::string_view usage) { return CommandLine().Int8P(name, {}, std::move(value), usage); }
int8* Int8P(std::string_view name, std::string_view shorthand, int8 value, std::string_view usage) { return CommandLine().Int8P(name, shorthand, std::move(value), usage); }

void FlagSet::Int16Var(int16* p, std::string_view name, int16 value, std::string_view usage) { Int16VarP(p, name, {}, std::move(value), usage); }
void FlagSet::Int16VarP(int16* p, std::string_view name, std::string_view shorthand, int16 value, std::string_view usage) {
    define<intValue<int16, 16, kInt16>>(*this, p, name, shorthand, std::move(value), usage);
}
int16* FlagSet::Int16(std::string_view name, int16 value, std::string_view usage) { return Int16P(name, {}, std::move(value), usage); }
int16* FlagSet::Int16P(std::string_view name, std::string_view shorthand, int16 value, std::string_view usage) {
    auto v = make_ptr<intValue<int16, 16, kInt16>>(nullptr, std::move(value));
    int16* p = v->target();
    VarPF(v, name, shorthand, usage);
    return p;
}
std::pair<int16, gopp::error> FlagSet::GetInt16(std::string_view name) const { return getValue<intValue<int16, 16, kInt16>>(*this, name); }
void Int16Var(int16* p, std::string_view name, int16 value, std::string_view usage) { CommandLine().Int16VarP(p, name, {}, std::move(value), usage); }
void Int16VarP(int16* p, std::string_view name, std::string_view shorthand, int16 value, std::string_view usage) { CommandLine().Int16VarP(p, name, shorthand, std::move(value), usage); }
int16* Int16(std::string_view name, int16 value, std::string_view usage) { return CommandLine().Int16P(name, {}, std::move(value), usage); }
int16* Int16P(std::string_view name, std::string_view shorthand, int16 value, std::string_view usage) { return CommandLine().Int16P(name, shorthand, std::move(value), usage); }

void FlagSet::Int32Var(int32* p, std::string_view name, int32 value, std::string_view usage) { Int32VarP(p, name, {}, std::move(value), usage); }
void FlagSet::Int32VarP(int32* p, std::string_view name, std::string_view shorthand, int32 value, std::string_view usage) {
    define<intValue<int32, 32, kInt32>>(*this, p, name, shorthand, std::move(value), usage);
}
int32* FlagSet::Int32(std::string_view name, int32 value, std::string_view usage) { return Int32P(name, {}, std::move(value), usage); }
int32* FlagSet::Int32P(std::string_view name, std::string_view shorthand, int32 value, std::string_view usage) {
    auto v = make_ptr<intValue<int32, 32, kInt32>>(nullptr, std::move(value));
    int32* p = v->target();
    VarPF(v, name, shorthand, usage);
    return p;
}
std::pair<int32, gopp::error> FlagSet::GetInt32(std::string_view name) const { return getValue<intValue<int32, 32, kInt32>>(*this, name); }
void Int32Var(int32* p, std::string_view name, int32 value, std::string_view usage) { CommandLine().Int32VarP(p, name, {}, std::move(value), usage); }
void Int32VarP(int32* p, std::string_view name, std::string_view shorthand, int32 value, std::string_view usage) { CommandLine().Int32VarP(p, name, shorthand, std::move(value), usage); }
int32* Int32(std::string_view name, int32 value, std::string_view usage) { return CommandLine().Int32P(name, {}, std::move(value), usage); }
int32* Int32P(std::string_view name, std::string_view shorthand, int32 value, std::string_view usage) { return CommandLine().Int32P(name, shorthand, std::move(value), usage); }

void FlagSet::Int64Var(int64* p, std::string_view name, int64 value, std::string_view usage) { Int64VarP(p, name, {}, std::move(value), usage); }
void FlagSet::Int64VarP(int64* p, std::string_view name, std::string_view shorthand, int64 value, std::string_view usage) {
    define<intValue<int64, 64, kInt64>>(*this, p, name, shorthand, std::move(value), usage);
}
int64* FlagSet::Int64(std::string_view name, int64 value, std::string_view usage) { return Int64P(name, {}, std::move(value), usage); }
int64* FlagSet::Int64P(std::string_view name, std::string_view shorthand, int64 value, std::string_view usage) {
    auto v = make_ptr<intValue<int64, 64, kInt64>>(nullptr, std::move(value));
    int64* p = v->target();
    VarPF(v, name, shorthand, usage);
    return p;
}
std::pair<int64, gopp::error> FlagSet::GetInt64(std::string_view name) const { return getValue<intValue<int64, 64, kInt64>>(*this, name); }
void Int64Var(int64* p, std::string_view name, int64 value, std::string_view usage) { CommandLine().Int64VarP(p, name, {}, std::move(value), usage); }
void Int64VarP(int64* p, std::string_view name, std::string_view shorthand, int64 value, std::string_view usage) { CommandLine().Int64VarP(p, name, shorthand, std::move(value), usage); }
int64* Int64(std::string_view name, int64 value, std::string_view usage) { return CommandLine().Int64P(name, {}, std::move(value), usage); }
int64* Int64P(std::string_view name, std::string_view shorthand, int64 value, std::string_view usage) { return CommandLine().Int64P(name, shorthand, std::move(value), usage); }

void FlagSet::UintVar(std::size_t* p, std::string_view name, std::size_t value, std::string_view usage) { UintVarP(p, name, {}, std::move(value), usage); }
void FlagSet::UintVarP(std::size_t* p, std::string_view name, std::string_view shorthand, std::size_t value, std::string_view usage) {
    define<uintValue<std::size_t, 64, kUint>>(*this, p, name, shorthand, std::move(value), usage);
}
std::size_t* FlagSet::Uint(std::string_view name, std::size_t value, std::string_view usage) { return UintP(name, {}, std::move(value), usage); }
std::size_t* FlagSet::UintP(std::string_view name, std::string_view shorthand, std::size_t value, std::string_view usage) {
    auto v = make_ptr<uintValue<std::size_t, 64, kUint>>(nullptr, std::move(value));
    std::size_t* p = v->target();
    VarPF(v, name, shorthand, usage);
    return p;
}
std::pair<std::size_t, gopp::error> FlagSet::GetUint(std::string_view name) const { return getValue<uintValue<std::size_t, 64, kUint>>(*this, name); }
void UintVar(std::size_t* p, std::string_view name, std::size_t value, std::string_view usage) { CommandLine().UintVarP(p, name, {}, std::move(value), usage); }
void UintVarP(std::size_t* p, std::string_view name, std::string_view shorthand, std::size_t value, std::string_view usage) { CommandLine().UintVarP(p, name, shorthand, std::move(value), usage); }
std::size_t* Uint(std::string_view name, std::size_t value, std::string_view usage) { return CommandLine().UintP(name, {}, std::move(value), usage); }
std::size_t* UintP(std::string_view name, std::string_view shorthand, std::size_t value, std::string_view usage) { return CommandLine().UintP(name, shorthand, std::move(value), usage); }

void FlagSet::Uint8Var(uint8* p, std::string_view name, uint8 value, std::string_view usage) { Uint8VarP(p, name, {}, std::move(value), usage); }
void FlagSet::Uint8VarP(uint8* p, std::string_view name, std::string_view shorthand, uint8 value, std::string_view usage) {
    define<uintValue<uint8, 8, kUint8>>(*this, p, name, shorthand, std::move(value), usage);
}
uint8* FlagSet::Uint8(std::string_view name, uint8 value, std::string_view usage) { return Uint8P(name, {}, std::move(value), usage); }
uint8* FlagSet::Uint8P(std::string_view name, std::string_view shorthand, uint8 value, std::string_view usage) {
    auto v = make_ptr<uintValue<uint8, 8, kUint8>>(nullptr, std::move(value));
    uint8* p = v->target();
    VarPF(v, name, shorthand, usage);
    return p;
}
std::pair<uint8, gopp::error> FlagSet::GetUint8(std::string_view name) const { return getValue<uintValue<uint8, 8, kUint8>>(*this, name); }
void Uint8Var(uint8* p, std::string_view name, uint8 value, std::string_view usage) { CommandLine().Uint8VarP(p, name, {}, std::move(value), usage); }
void Uint8VarP(uint8* p, std::string_view name, std::string_view shorthand, uint8 value, std::string_view usage) { CommandLine().Uint8VarP(p, name, shorthand, std::move(value), usage); }
uint8* Uint8(std::string_view name, uint8 value, std::string_view usage) { return CommandLine().Uint8P(name, {}, std::move(value), usage); }
uint8* Uint8P(std::string_view name, std::string_view shorthand, uint8 value, std::string_view usage) { return CommandLine().Uint8P(name, shorthand, std::move(value), usage); }

void FlagSet::Uint16Var(uint16* p, std::string_view name, uint16 value, std::string_view usage) { Uint16VarP(p, name, {}, std::move(value), usage); }
void FlagSet::Uint16VarP(uint16* p, std::string_view name, std::string_view shorthand, uint16 value, std::string_view usage) {
    define<uintValue<uint16, 16, kUint16>>(*this, p, name, shorthand, std::move(value), usage);
}
uint16* FlagSet::Uint16(std::string_view name, uint16 value, std::string_view usage) { return Uint16P(name, {}, std::move(value), usage); }
uint16* FlagSet::Uint16P(std::string_view name, std::string_view shorthand, uint16 value, std::string_view usage) {
    auto v = make_ptr<uintValue<uint16, 16, kUint16>>(nullptr, std::move(value));
    uint16* p = v->target();
    VarPF(v, name, shorthand, usage);
    return p;
}
std::pair<uint16, gopp::error> FlagSet::GetUint16(std::string_view name) const { return getValue<uintValue<uint16, 16, kUint16>>(*this, name); }
void Uint16Var(uint16* p, std::string_view name, uint16 value, std::string_view usage) { CommandLine().Uint16VarP(p, name, {}, std::move(value), usage); }
void Uint16VarP(uint16* p, std::string_view name, std::string_view shorthand, uint16 value, std::string_view usage) { CommandLine().Uint16VarP(p, name, shorthand, std::move(value), usage); }
uint16* Uint16(std::string_view name, uint16 value, std::string_view usage) { return CommandLine().Uint16P(name, {}, std::move(value), usage); }
uint16* Uint16P(std::string_view name, std::string_view shorthand, uint16 value, std::string_view usage) { return CommandLine().Uint16P(name, shorthand, std::move(value), usage); }

void FlagSet::Uint32Var(uint32* p, std::string_view name, uint32 value, std::string_view usage) { Uint32VarP(p, name, {}, std::move(value), usage); }
void FlagSet::Uint32VarP(uint32* p, std::string_view name, std::string_view shorthand, uint32 value, std::string_view usage) {
    define<uintValue<uint32, 32, kUint32>>(*this, p, name, shorthand, std::move(value), usage);
}
uint32* FlagSet::Uint32(std::string_view name, uint32 value, std::string_view usage) { return Uint32P(name, {}, std::move(value), usage); }
uint32* FlagSet::Uint32P(std::string_view name, std::string_view shorthand, uint32 value, std::string_view usage) {
    auto v = make_ptr<uintValue<uint32, 32, kUint32>>(nullptr, std::move(value));
    uint32* p = v->target();
    VarPF(v, name, shorthand, usage);
    return p;
}
std::pair<uint32, gopp::error> FlagSet::GetUint32(std::string_view name) const { return getValue<uintValue<uint32, 32, kUint32>>(*this, name); }
void Uint32Var(uint32* p, std::string_view name, uint32 value, std::string_view usage) { CommandLine().Uint32VarP(p, name, {}, std::move(value), usage); }
void Uint32VarP(uint32* p, std::string_view name, std::string_view shorthand, uint32 value, std::string_view usage) { CommandLine().Uint32VarP(p, name, shorthand, std::move(value), usage); }
uint32* Uint32(std::string_view name, uint32 value, std::string_view usage) { return CommandLine().Uint32P(name, {}, std::move(value), usage); }
uint32* Uint32P(std::string_view name, std::string_view shorthand, uint32 value, std::string_view usage) { return CommandLine().Uint32P(name, shorthand, std::move(value), usage); }

void FlagSet::Uint64Var(uint64* p, std::string_view name, uint64 value, std::string_view usage) { Uint64VarP(p, name, {}, std::move(value), usage); }
void FlagSet::Uint64VarP(uint64* p, std::string_view name, std::string_view shorthand, uint64 value, std::string_view usage) {
    define<uintValue<uint64, 64, kUint64>>(*this, p, name, shorthand, std::move(value), usage);
}
uint64* FlagSet::Uint64(std::string_view name, uint64 value, std::string_view usage) { return Uint64P(name, {}, std::move(value), usage); }
uint64* FlagSet::Uint64P(std::string_view name, std::string_view shorthand, uint64 value, std::string_view usage) {
    auto v = make_ptr<uintValue<uint64, 64, kUint64>>(nullptr, std::move(value));
    uint64* p = v->target();
    VarPF(v, name, shorthand, usage);
    return p;
}
std::pair<uint64, gopp::error> FlagSet::GetUint64(std::string_view name) const { return getValue<uintValue<uint64, 64, kUint64>>(*this, name); }
void Uint64Var(uint64* p, std::string_view name, uint64 value, std::string_view usage) { CommandLine().Uint64VarP(p, name, {}, std::move(value), usage); }
void Uint64VarP(uint64* p, std::string_view name, std::string_view shorthand, uint64 value, std::string_view usage) { CommandLine().Uint64VarP(p, name, shorthand, std::move(value), usage); }
uint64* Uint64(std::string_view name, uint64 value, std::string_view usage) { return CommandLine().Uint64P(name, {}, std::move(value), usage); }
uint64* Uint64P(std::string_view name, std::string_view shorthand, uint64 value, std::string_view usage) { return CommandLine().Uint64P(name, shorthand, std::move(value), usage); }

void FlagSet::Float32Var(float* p, std::string_view name, float value, std::string_view usage) { Float32VarP(p, name, {}, std::move(value), usage); }
void FlagSet::Float32VarP(float* p, std::string_view name, std::string_view shorthand, float value, std::string_view usage) {
    define<floatValue<float, 32, kFloat32>>(*this, p, name, shorthand, std::move(value), usage);
}
float* FlagSet::Float32(std::string_view name, float value, std::string_view usage) { return Float32P(name, {}, std::move(value), usage); }
float* FlagSet::Float32P(std::string_view name, std::string_view shorthand, float value, std::string_view usage) {
    auto v = make_ptr<floatValue<float, 32, kFloat32>>(nullptr, std::move(value));
    float* p = v->target();
    VarPF(v, name, shorthand, usage);
    return p;
}
std::pair<float, gopp::error> FlagSet::GetFloat32(std::string_view name) const { return getValue<floatValue<float, 32, kFloat32>>(*this, name); }
void Float32Var(float* p, std::string_view name, float value, std::string_view usage) { CommandLine().Float32VarP(p, name, {}, std::move(value), usage); }
void Float32VarP(float* p, std::string_view name, std::string_view shorthand, float value, std::string_view usage) { CommandLine().Float32VarP(p, name, shorthand, std::move(value), usage); }
float* Float32(std::string_view name, float value, std::string_view usage) { return CommandLine().Float32P(name, {}, std::move(value), usage); }
float* Float32P(std::string_view name, std::string_view shorthand, float value, std::string_view usage) { return CommandLine().Float32P(name, shorthand, std::move(value), usage); }

void FlagSet::Float64Var(double* p, std::string_view name, double value, std::string_view usage) { Float64VarP(p, name, {}, std::move(value), usage); }
void FlagSet::Float64VarP(double* p, std::string_view name, std::string_view shorthand, double value, std::string_view usage) {
    define<floatValue<double, 64, kFloat64>>(*this, p, name, shorthand, std::move(value), usage);
}
double* FlagSet::Float64(std::string_view name, double value, std::string_view usage) { return Float64P(name, {}, std::move(value), usage); }
double* FlagSet::Float64P(std::string_view name, std::string_view shorthand, double value, std::string_view usage) {
    auto v = make_ptr<floatValue<double, 64, kFloat64>>(nullptr, std::move(value));
    double* p = v->target();
    VarPF(v, name, shorthand, usage);
    return p;
}
std::pair<double, gopp::error> FlagSet::GetFloat64(std::string_view name) const { return getValue<floatValue<double, 64, kFloat64>>(*this, name); }
void Float64Var(double* p, std::string_view name, double value, std::string_view usage) { CommandLine().Float64VarP(p, name, {}, std::move(value), usage); }
void Float64VarP(double* p, std::string_view name, std::string_view shorthand, double value, std::string_view usage) { CommandLine().Float64VarP(p, name, shorthand, std::move(value), usage); }
double* Float64(std::string_view name, double value, std::string_view usage) { return CommandLine().Float64P(name, {}, std::move(value), usage); }
double* Float64P(std::string_view name, std::string_view shorthand, double value, std::string_view usage) { return CommandLine().Float64P(name, shorthand, std::move(value), usage); }

void FlagSet::DurationVar(time::Duration* p, std::string_view name, time::Duration value, std::string_view usage) { DurationVarP(p, name, {}, std::move(value), usage); }
void FlagSet::DurationVarP(time::Duration* p, std::string_view name, std::string_view shorthand, time::Duration value, std::string_view usage) {
    define<durationValue>(*this, p, name, shorthand, std::move(value), usage);
}
time::Duration* FlagSet::Duration(std::string_view name, time::Duration value, std::string_view usage) { return DurationP(name, {}, std::move(value), usage); }
time::Duration* FlagSet::DurationP(std::string_view name, std::string_view shorthand, time::Duration value, std::string_view usage) {
    auto v = make_ptr<durationValue>(nullptr, std::move(value));
    time::Duration* p = v->target();
    VarPF(v, name, shorthand, usage);
    return p;
}
std::pair<time::Duration, gopp::error> FlagSet::GetDuration(std::string_view name) const { return getValue<durationValue>(*this, name); }
void DurationVar(time::Duration* p, std::string_view name, time::Duration value, std::string_view usage) { CommandLine().DurationVarP(p, name, {}, std::move(value), usage); }
void DurationVarP(time::Duration* p, std::string_view name, std::string_view shorthand, time::Duration value, std::string_view usage) { CommandLine().DurationVarP(p, name, shorthand, std::move(value), usage); }
time::Duration* Duration(std::string_view name, time::Duration value, std::string_view usage) { return CommandLine().DurationP(name, {}, std::move(value), usage); }
time::Duration* DurationP(std::string_view name, std::string_view shorthand, time::Duration value, std::string_view usage) { return CommandLine().DurationP(name, shorthand, std::move(value), usage); }

void FlagSet::BytesHexVar(slice<byte>* p, std::string_view name, slice<byte> value, std::string_view usage) { BytesHexVarP(p, name, {}, std::move(value), usage); }
void FlagSet::BytesHexVarP(slice<byte>* p, std::string_view name, std::string_view shorthand, slice<byte> value, std::string_view usage) {
    define<bytesHexValue>(*this, p, name, shorthand, std::move(value), usage);
}
slice<byte>* FlagSet::BytesHex(std::string_view name, slice<byte> value, std::string_view usage) { return BytesHexP(name, {}, std::move(value), usage); }
slice<byte>* FlagSet::BytesHexP(std::string_view name, std::string_view shorthand, slice<byte> value, std::string_view usage) {
    auto v = make_ptr<bytesHexValue>(nullptr, std::move(value));
    slice<byte>* p = v->target();
    VarPF(v, name, shorthand, usage);
    return p;
}
std::pair<slice<byte>, gopp::error> FlagSet::GetBytesHex(std::string_view name) const { return getValue<bytesHexValue>(*this, name); }
void BytesHexVar(slice<byte>* p, std::string_view name, slice<byte> value, std::string_view usage) { CommandLine().BytesHexVarP(p, name, {}, std::move(value), usage); }
void BytesHexVarP(slice<byte>* p, std::string_view name, std::string_view shorthand, slice<byte> value, std::string_view usage) { CommandLine().BytesHexVarP(p, name, shorthand, std::move(value), usage); }
slice<byte>* BytesHex(std::string_view name, slice<byte> value, std::string_view usage) { return CommandLine().BytesHexP(name, {}, std::move(value), usage); }
slice<byte>* BytesHexP(std::string_view name, std::string_view shorthand, slice<byte> value, std::string_view usage) { return CommandLine().BytesHexP(name, shorthand, std::move(value), usage); }

void FlagSet::BytesBase64Var(slice<byte>* p, std::string_view name, slice<byte> value, std::string_view usage) { BytesBase64VarP(p, name, {}, std::move(value), usage); }
void FlagSet::BytesBase64VarP(slice<byte>* p, std::string_view name, std::string_view shorthand, slice<byte> value, std::string_view usage) {
    define<bytesBase64Value>(*this, p, name, shorthand, std::move(value), usage);
}
slice<byte>* FlagSet::BytesBase64(std::string_view name, slice<byte> value, std::string_view usage) { return BytesBase64P(name, {}, std::move(value), usage); }
slice<byte>* FlagSet::BytesBase64P(std::string_view name, std::string_view shorthand, slice<byte> value, std::string_view usage) {
    auto v = make_ptr<bytesBase64Value>(nullptr, std::move(value));
    slice<byte>* p = v->target();
    VarPF(v, name, shorthand, usage);
    return p;
}
std::pair<slice<byte>, gopp::error> FlagSet::GetBytesBase64(std::string_view name) const { return getValue<bytesBase64Value>(*this, name); }
void BytesBase64Var(slice<byte>* p, std::string_view name, slice<byte> value, std::string_view usage) { CommandLine().BytesBase64VarP(p, name, {}, std::move(value), usage); }
void BytesBase64VarP(slice<byte>* p, std::string_view name, std::string_view shorthand, slice<byte> value, std::string_view usage) { CommandLine().BytesBase64VarP(p, name, shorthand, std::move(value), usage); }
slice<byte>* BytesBase64(std::string_view name, slice<byte> value, std::string_view usage) { return CommandLine().BytesBase64P(name, {}, std::move(value), usage); }
slice<byte>* BytesBase64P(std::string_view name, std::string_view shorthand, slice<byte> value, std::string_view usage) { return CommandLine().BytesBase64P(name, shorthand, std::move(value), usage); }

void FlagSet::IPVar(net::IP* p, std::string_view name, net::IP value, std::string_view usage) { IPVarP(p, name, {}, std::move(value), usage); }
void FlagSet::IPVarP(net::IP* p, std::string_view name, std::string_view shorthand, net::IP value, std::string_view usage) {
    define<ipValue>(*this, p, name, shorthand, std::move(value), usage);
}
net::IP* FlagSet::IP(std::string_view name, net::IP value, std::string_view usage) { return IPP(name, {}, std::move(value), usage); }
net::IP* FlagSet::IPP(std::string_view name, std::string_view shorthand, net::IP value, std::string_view usage) {
    auto v = make_ptr<ipValue>(nullptr, std::move(value));
    net::IP* p = v->target();
    VarPF(v, name, shorthand, usage);
    return p;
}
std::pair<net::IP, gopp::error> FlagSet::GetIP(std::string_view name) const { return getValue<ipValue>(*this, name); }
void IPVar(net::IP* p, std::string_view name, net::IP value, std::string_view usage) { CommandLine().IPVarP(p, name, {}, std::move(value), usage); }
void IPVarP(net::IP* p, std::string_view name, std::string_view shorthand, net::IP value, std::string_view usage) { CommandLine().IPVarP(p, name, shorthand, std::move(value), usage); }
net::IP* IP(std::string_view name, net::IP value, std::string_view usage) { return CommandLine().IPP(name, {}, std::move(value), usage); }
net::IP* IPP(std::string_view name, std::string_view shorthand, net::IP value, std::string_view usage) { return CommandLine().IPP(name, shorthand, std::move(value), usage); }

void FlagSet::IPMaskVar(net::IPMask* p, std::string_view name, net::IPMask value, std::string_view usage) { IPMaskVarP(p, name, {}, std::move(value), usage); }
void FlagSet::IPMaskVarP(net::IPMask* p, std::string_view name, std::string_view shorthand, net::IPMask value, std::string_view usage) {
    define<ipMaskValue>(*this, p, name, shorthand, std::move(value), usage);
}
net::IPMask* FlagSet::IPMask(std::string_view name, net::IPMask value, std::string_view usage) { return IPMaskP(name, {}, std::move(value), usage); }
net::IPMask* FlagSet::IPMaskP(std::string_view name, std::string_view shorthand, net::IPMask value, std::string_view usage) {
    auto v = make_ptr<ipMaskValue>(nullptr, std::move(value));
    net::IPMask* p = v->target();
    VarPF(v, name, shorthand, usage);
    return p;
}
std::pair<net::IPMask, gopp::error> FlagSet::GetIPv4Mask(std::string_view name) const { return getValue<ipMaskValue>(*this, name); }
void IPMaskVar(net::IPMask* p, std::string_view name, net::IPMask value, std::string_view usage) { CommandLine().IPMaskVarP(p, name, {}, std::move(value), usage); }
void IPMaskVarP(net::IPMask* p, std::string_view name, std::string_view shorthand, net::IPMask value, std::string_view usage) { CommandLine().IPMaskVarP(p, name, shorthand, std::move(value), usage); }
net::IPMask* IPMask(std::string_view name, net::IPMask value, std::string_view usage) { return CommandLine().IPMaskP(name, {}, std::move(value), usage); }
net::IPMask* IPMaskP(std::string_view name, std::string_view shorthand, net::IPMask value, std::string_view usage) { return CommandLine().IPMaskP(name, shorthand, std::move(value), usage); }

void FlagSet::IPNetVar(net::IPNet* p, std::string_view name, net::IPNet value, std::string_view usage) { IPNetVarP(p, name, {}, std::move(value), usage); }
void FlagSet::IPNetVarP(net::IPNet* p, std::string_view name, std::string_view shorthand, net::IPNet value, std::string_view usage) {
    define<ipNetValue>(*this, p, name, shorthand, std::move(value), usage);
}
net::IPNet* FlagSet::IPNet(std::string_view name, net::IPNet value, std::string_view usage) { return IPNetP(name, {}, std::move(value), usage); }
net::IPNet* FlagSet::IPNetP(std::string_view name, std::string_view shorthand, net::IPNet value, std::string_view usage) {
    auto v = make_ptr<ipNetValue>(nullptr, std::move(value));
    net::IPNet* p = v->target();
    VarPF(v, name, shorthand, usage);
    return p;
}
std::pair<net::IPNet, gopp::error> FlagSet::GetIPNet(std::string_view name) const { return getValue<ipNetValue>(*this, name); }
void IPNetVar(net::IPNet* p, std::string_view name, net::IPNet value, std::string_view usage) { CommandLine().IPNetVarP(p, name, {}, std::move(value), usage); }
void IPNetVarP(net::IPNet* p, std::string_view name, std::string_view shorthand, net::IPNet value, std::string_view usage) { CommandLine().IPNetVarP(p, name, shorthand, std::move(value), usage); }
net::IPNet* IPNet(std::string_view name, net::IPNet value, std::string_view usage) { return CommandLine().IPNetP(name, {}, std::move(value), usage); }
net::IPNet* IPNetP(std::string_view name, std::string_view shorthand, net::IPNet value, std::string_view usage) { return CommandLine().IPNetP(name, shorthand, std::move(value), usage); }

void FlagSet::StringSliceVar(std::vector<gopp::string>* p, std::string_view name, std::vector<gopp::string> value, std::string_view usage) { StringSliceVarP(p, name, {}, std::move(value), usage); }
void FlagSet::StringSliceVarP(std::vector<gopp::string>* p, std::string_view name, std::string_view shorthand, std::vector<gopp::string> value, std::string_view usage) {
    define<stringSliceValue>(*this, p, name, shorthand, std::move(value), usage);
}
std::vector<gopp::string>* FlagSet::StringSlice(std::string_view name, std::vector<gopp::string> value, std::string_view usage) { return StringSliceP(name, {}, std::move(value), usage); }
std::vector<gopp::string>* FlagSet::StringSliceP(std::string_view name, std::string_view shorthand, std::vector<gopp::string> value, std::string_view usage) {
    auto v = make_ptr<stringSliceValue>(nullptr, std::move(value));
    std::vector<gopp::string>* p = v->target();
    VarPF(v, name, shorthand, usage);
    return p;
}
std::pair<std::vector<gopp::string>, gopp::error> FlagSet::GetStringSlice(std::string_view name) const { return getValue<stringSliceValue>(*this, name); }
void StringSliceVar(std::vector<gopp::string>* p, std::string_view name, std::vector<gopp::string> value, std::string_view usage) { CommandLine().StringSliceVarP(p, name, {}, std::move(value), usage); }
void StringSliceVarP(std::vector<gopp::string>* p, std::string_view name, std::string_view shorthand, std::vector<gopp::string> value, std::string_view usage) { CommandLine().StringSliceVarP(p, name, shorthand, std::move(value), usage); }
std::vector<gopp::string>* StringSlice(std::string_view name, std::vector<gopp::string> value, std::string_view usage) { return CommandLine().StringSliceP(name, {}, std::move(value), usage); }
std::vector<gopp::string>* StringSliceP(std::string_view name, std::string_view shorthand, std::vector<gopp::string> value, std::string_view usage) { return CommandLine().StringSliceP(name, shorthand, std::move(value), usage); }

void FlagSet::StringArrayVar(std::vector<gopp::string>* p, std::string_view name, std::vector<gopp::string> value, std::string_view usage) { StringArrayVarP(p, name, {}, std::move(value), usage); }
void FlagSet::StringArrayVarP(std::vector<gopp::string>* p, std::string_view name, std::string_view shorthand, std::vector<gopp::string> value, std::string_view usage) {
    define<stringArrayValue>(*this, p, name, shorthand, std::move(value), usage);
}
std::vector<gopp::string>* FlagSet::StringArray(std::string_view name, std::vector<gopp::string> value, std::string_view usage) { return StringArrayP(name, {}, std::move(value), usage); }
std::vector<gopp::string>* FlagSet::StringArrayP(std::string_view name, std::string_view shorthand, std::vector<gopp::string> value, std::string_view usage) {
    auto v = make_ptr<stringArrayValue>(nullptr, std::move(value));
    std::vector<gopp::string>* p = v->target();
    VarPF(v, name, shorthand, usage);
    return p;
}
std::pair<std::vector<gopp::string>, gopp::error> FlagSet::GetStringArray(std::string_view name) const { return getValue<stringArrayValue>(*this, name); }
void StringArrayVar(std::vector<gopp::string>* p, std::string_view name, std::vector<gopp::string> value, std::string_view usage) { CommandLine().StringArrayVarP(p, name, {}, std::move(value), usage); }
void StringArrayVarP(std::vector<gopp::string>* p, std::string_view name, std::string_view shorthand, std::vector<gopp::string> value, std::string_view usage) { CommandLine().StringArrayVarP(p, name, shorthand, std::move(value), usage); }
std::vector<gopp::string>* StringArray(std::string_view name, std::vector<gopp::string> value, std::string_view usage) { return CommandLine().StringArrayP(name, {}, std::move(value), usage); }
std::vector<gopp::string>* StringArrayP(std::string_view name, std::string_view shorthand, std::vector<gopp::string> value, std::string_view usage) { return CommandLine().StringArrayP(name, shorthand, std::move(value), usage); }

void FlagSet::IntSliceVar(std::vector<isize>* p, std::string_view name, std::vector<isize> value, std::string_view usage) { IntSliceVarP(p, name, {}, std::move(value), usage); }
void FlagSet::IntSliceVarP(std::vector<isize>* p, std::string_view name, std::string_view shorthand, std::vector<isize> value, std::string_view usage) {
    define<intSliceValue>(*this, p, name, shorthand, std::move(value), usage);
}
std::vector<isize>* FlagSet::IntSlice(std::string_view name, std::vector<isize> value, std::string_view usage) { return IntSliceP(name, {}, std::move(value), usage); }
std::vector<isize>* FlagSet::IntSliceP(std::string_view name, std::string_view shorthand, std::vector<isize> value, std::string_view usage) {
    auto v = make_ptr<intSliceValue>(nullptr, std::move(value));
    std::vector<isize>* p = v->target();
    VarPF(v, name, shorthand, usage);
    return p;
}
std::pair<std::vector<isize>, gopp::error> FlagSet::GetIntSlice(std::string_view name) const { return getValue<intSliceValue>(*this, name); }
void IntSliceVar(std::vector<isize>* p, std::string_view name, std::vector<isize> value, std::string_view usage) { CommandLine().IntSliceVarP(p, name, {}, std::move(value), usage); }
void IntSliceVarP(std::vector<isize>* p, std::string_view name, std::string_view shorthand, std::vector<isize> value, std::string_view usage) { CommandLine().IntSliceVarP(p, name, shorthand, std::move(value), usage); }
std::vector<isize>* IntSlice(std::string_view name, std::vector<isize> value, std::string_view usage) { return CommandLine().IntSliceP(name, {}, std::move(value), usage); }
std::vector<isize>* IntSliceP(std::string_view name, std::string_view shorthand, std::vector<isize> value, std::string_view usage) { return CommandLine().IntSliceP(name, shorthand, std::move(value), usage); }

void FlagSet::Int32SliceVar(std::vector<int32>* p, std::string_view name, std::vector<int32> value, std::string_view usage) { Int32SliceVarP(p, name, {}, std::move(value), usage); }
void FlagSet::Int32SliceVarP(std::vector<int32>* p, std::string_view name, std::string_view shorthand, std::vector<int32> value, std::string_view usage) {
    define<int32SliceValue>(*this, p, name, shorthand, std::move(value), usage);
}
std::vector<int32>* FlagSet::Int32Slice(std::string_view name, std::vector<int32> value, std::string_view usage) { return Int32SliceP(name, {}, std::move(value), usage); }
std::vector<int32>* FlagSet::Int32SliceP(std::string_view name, std::string_view shorthand, std::vector<int32> value, std::string_view usage) {
    auto v = make_ptr<int32SliceValue>(nullptr, std::move(value));
    std::vector<int32>* p = v->target();
    VarPF(v, name, shorthand, usage);
    return p;
}
std::pair<std::vector<int32>, gopp::error> FlagSet::GetInt32Slice(std::string_view name) const { return getValue<int32SliceValue>(*this, name); }
void Int32SliceVar(std::vector<int32>* p, std::string_view name, std::vector<int32> value, std::string_view usage) { CommandLine().Int32SliceVarP(p, name, {}, std::move(value), usage); }
void Int32SliceVarP(std::vector<int32>* p, std::string_view name, std::string_view shorthand, std::vector<int32> value, std::string_view usage) { CommandLine().Int32SliceVarP(p, name, shorthand, std::move(value), usage); }
std::vector<int32>* Int32Slice(std::string_view name, std::vector<int32> value, std::string_view usage) { return CommandLine().Int32SliceP(name, {}, std::move(value), usage); }
std::vector<int32>* Int32SliceP(std::string_view name, std::string_view shorthand, std::vector<int32> value, std::string_view usage) { return CommandLine().Int32SliceP(name, shorthand, std::move(value), usage); }

void FlagSet::Int64SliceVar(std::vector<int64>* p, std::string_view name, std::vector<int64> value, std::string_view usage) { Int64SliceVarP(p, name, {}, std::move(value), usage); }
void FlagSet::Int64SliceVarP(std::vector<int64>* p, std::string_view name, std::string_view shorthand, std::vector<int64> value, std::string_view usage) {
    define<int64SliceValue>(*this, p, name, shorthand, std::move(value), usage);
}
std::vector<int64>* FlagSet::Int64Slice(std::string_view name, std::vector<int64> value, std::string_view usage) { return Int64SliceP(name, {}, std::move(value), usage); }
std::vector<int64>* FlagSet::Int64SliceP(std::string_view name, std::string_view shorthand, std::vector<int64> value, std::string_view usage) {
    auto v = make_ptr<int64SliceValue>(nullptr, std::move(value));
    std::vector<int64>* p = v->target();
    VarPF(v, name, shorthand, usage);
    return p;
}
std::pair<std::vector<int64>, gopp::error> FlagSet::GetInt64Slice(std::string_view name) const { return getValue<int64SliceValue>(*this, name); }
void Int64SliceVar(std::vector<int64>* p, std::string_view name, std::vector<int64> value, std::string_view usage) { CommandLine().Int64SliceVarP(p, name, {}, std::move(value), usage); }
void Int64SliceVarP(std::vector<int64>* p, std::string_view name, std::string_view shorthand, std::vector<int64> value, std::string_view usage) { CommandLine().Int64SliceVarP(p, name, shorthand, std::move(value), usage); }
std::vector<int64>* Int64Slice(std::string_view name, std::vector<int64> value, std::string_view usage) { return CommandLine().Int64SliceP(name, {}, std::move(value), usage); }
std::vector<int64>* Int64SliceP(std::string_view name, std::string_view shorthand, std::vector<int64> value, std::string_view usage) { return CommandLine().Int64SliceP(name, shorthand, std::move(value), usage); }

void FlagSet::UintSliceVar(std::vector<std::size_t>* p, std::string_view name, std::vector<std::size_t> value, std::string_view usage) { UintSliceVarP(p, name, {}, std::move(value), usage); }
void FlagSet::UintSliceVarP(std::vector<std::size_t>* p, std::string_view name, std::string_view shorthand, std::vector<std::size_t> value, std::string_view usage) {
    define<uintSliceValue>(*this, p, name, shorthand, std::move(value), usage);
}
std::vector<std::size_t>* FlagSet::UintSlice(std::string_view name, std::vector<std::size_t> value, std::string_view usage) { return UintSliceP(name, {}, std::move(value), usage); }
std::vector<std::size_t>* FlagSet::UintSliceP(std::string_view name, std::string_view shorthand, std::vector<std::size_t> value, std::string_view usage) {
    auto v = make_ptr<uintSliceValue>(nullptr, std::move(value));
    std::vector<std::size_t>* p = v->target();
    VarPF(v, name, shorthand, usage);
    return p;
}
std::pair<std::vector<std::size_t>, gopp::error> FlagSet::GetUintSlice(std::string_view name) const { return getValue<uintSliceValue>(*this, name); }
void UintSliceVar(std::vector<std::size_t>* p, std::string_view name, std::vector<std::size_t> value, std::string_view usage) { CommandLine().UintSliceVarP(p, name, {}, std::move(value), usage); }
void UintSliceVarP(std::vector<std::size_t>* p, std::string_view name, std::string_view shorthand, std::vector<std::size_t> value, std::string_view usage) { CommandLine().UintSliceVarP(p, name, shorthand, std::move(value), usage); }
std::vector<std::size_t>* UintSlice(std::string_view name, std::vector<std::size_t> value, std::string_view usage) { return CommandLine().UintSliceP(name, {}, std::move(value), usage); }
std::vector<std::size_t>* UintSliceP(std::string_view name, std::string_view shorthand, std::vector<std::size_t> value, std::string_view usage) { return CommandLine().UintSliceP(name, shorthand, std::move(value), usage); }

void FlagSet::Float32SliceVar(std::vector<float>* p, std::string_view name, std::vector<float> value, std::string_view usage) { Float32SliceVarP(p, name, {}, std::move(value), usage); }
void FlagSet::Float32SliceVarP(std::vector<float>* p, std::string_view name, std::string_view shorthand, std::vector<float> value, std::string_view usage) {
    define<float32SliceValue>(*this, p, name, shorthand, std::move(value), usage);
}
std::vector<float>* FlagSet::Float32Slice(std::string_view name, std::vector<float> value, std::string_view usage) { return Float32SliceP(name, {}, std::move(value), usage); }
std::vector<float>* FlagSet::Float32SliceP(std::string_view name, std::string_view shorthand, std::vector<float> value, std::string_view usage) {
    auto v = make_ptr<float32SliceValue>(nullptr, std::move(value));
    std::vector<float>* p = v->target();
    VarPF(v, name, shorthand, usage);
    return p;
}
std::pair<std::vector<float>, gopp::error> FlagSet::GetFloat32Slice(std::string_view name) const { return getValue<float32SliceValue>(*this, name); }
void Float32SliceVar(std::vector<float>* p, std::string_view name, std::vector<float> value, std::string_view usage) { CommandLine().Float32SliceVarP(p, name, {}, std::move(value), usage); }
void Float32SliceVarP(std::vector<float>* p, std::string_view name, std::string_view shorthand, std::vector<float> value, std::string_view usage) { CommandLine().Float32SliceVarP(p, name, shorthand, std::move(value), usage); }
std::vector<float>* Float32Slice(std::string_view name, std::vector<float> value, std::string_view usage) { return CommandLine().Float32SliceP(name, {}, std::move(value), usage); }
std::vector<float>* Float32SliceP(std::string_view name, std::string_view shorthand, std::vector<float> value, std::string_view usage) { return CommandLine().Float32SliceP(name, shorthand, std::move(value), usage); }

void FlagSet::Float64SliceVar(std::vector<double>* p, std::string_view name, std::vector<double> value, std::string_view usage) { Float64SliceVarP(p, name, {}, std::move(value), usage); }
void FlagSet::Float64SliceVarP(std::vector<double>* p, std::string_view name, std::string_view shorthand, std::vector<double> value, std::string_view usage) {
    define<float64SliceValue>(*this, p, name, shorthand, std::move(value), usage);
}
std::vector<double>* FlagSet::Float64Slice(std::string_view name, std::vector<double> value, std::string_view usage) { return Float64SliceP(name, {}, std::move(value), usage); }
std::vector<double>* FlagSet::Float64SliceP(std::string_view name, std::string_view shorthand, std::vector<double> value, std::string_view usage) {
    auto v = make_ptr<float64SliceValue>(nullptr, std::move(value));
    std::vector<double>* p = v->target();
    VarPF(v, name, shorthand, usage);
    return p;
}
std::pair<std::vector<double>, gopp::error> FlagSet::GetFloat64Slice(std::string_view name) const { return getValue<float64SliceValue>(*this, name); }
void Float64SliceVar(std::vector<double>* p, std::string_view name, std::vector<double> value, std::string_view usage) { CommandLine().Float64SliceVarP(p, name, {}, std::move(value), usage); }
void Float64SliceVarP(std::vector<double>* p, std::string_view name, std::string_view shorthand, std::vector<double> value, std::string_view usage) { CommandLine().Float64SliceVarP(p, name, shorthand, std::move(value), usage); }
std::vector<double>* Float64Slice(std::string_view name, std::vector<double> value, std::string_view usage) { return CommandLine().Float64SliceP(name, {}, std::move(value), usage); }
std::vector<double>* Float64SliceP(std::string_view name, std::string_view shorthand, std::vector<double> value, std::string_view usage) { return CommandLine().Float64SliceP(name, shorthand, std::move(value), usage); }

void FlagSet::BoolSliceVar(std::vector<bool>* p, std::string_view name, std::vector<bool> value, std::string_view usage) { BoolSliceVarP(p, name, {}, std::move(value), usage); }
void FlagSet::BoolSliceVarP(std::vector<bool>* p, std::string_view name, std::string_view shorthand, std::vector<bool> value, std::string_view usage) {
    define<boolSliceValue>(*this, p, name, shorthand, std::move(value), usage);
}
std::vector<bool>* FlagSet::BoolSlice(std::string_view name, std::vector<bool> value, std::string_view usage) { return BoolSliceP(name, {}, std::move(value), usage); }
std::vector<bool>* FlagSet::BoolSliceP(std::string_view name, std::string_view shorthand, std::vector<bool> value, std::string_view usage) {
    auto v = make_ptr<boolSliceValue>(nullptr, std::move(value));
    std::vector<bool>* p = v->target();
    VarPF(v, name, shorthand, usage);
    return p;
}
std::pair<std::vector<bool>, gopp::error> FlagSet::GetBoolSlice(std::string_view name) const { return getValue<boolSliceValue>(*this, name); }
void BoolSliceVar(std::vector<bool>* p, std::string_view name, std::vector<bool> value, std::string_view usage) { CommandLine().BoolSliceVarP(p, name, {}, std::move(value), usage); }
void BoolSliceVarP(std::vector<bool>* p, std::string_view name, std::string_view shorthand, std::vector<bool> value, std::string_view usage) { CommandLine().BoolSliceVarP(p, name, shorthand, std::move(value), usage); }
std::vector<bool>* BoolSlice(std::string_view name, std::vector<bool> value, std::string_view usage) { return CommandLine().BoolSliceP(name, {}, std::move(value), usage); }
std::vector<bool>* BoolSliceP(std::string_view name, std::string_view shorthand, std::vector<bool> value, std::string_view usage) { return CommandLine().BoolSliceP(name, shorthand, std::move(value), usage); }

void FlagSet::DurationSliceVar(std::vector<time::Duration>* p, std::string_view name, std::vector<time::Duration> value, std::string_view usage) { DurationSliceVarP(p, name, {}, std::move(value), usage); }
void FlagSet::DurationSliceVarP(std::vector<time::Duration>* p, std::string_view name, std::string_view shorthand, std::vector<time::Duration> value, std::string_view usage) {
    define<durationSliceValue>(*this, p, name, shorthand, std::move(value), usage);
}
std::vector<time::Duration>* FlagSet::DurationSlice(std::string_view name, std::vector<time::Duration> value, std::string_view usage) { return DurationSliceP(name, {}, std::move(value), usage); }
std::vector<time::Duration>* FlagSet::DurationSliceP(std::string_view name, std::string_view shorthand, std::vector<time::Duration> value, std::string_view usage) {
    auto v = make_ptr<durationSliceValue>(nullptr, std::move(value));
    std::vector<time::Duration>* p = v->target();
    VarPF(v, name, shorthand, usage);
    return p;
}
std::pair<std::vector<time::Duration>, gopp::error> FlagSet::GetDurationSlice(std::string_view name) const { return getValue<durationSliceValue>(*this, name); }
void DurationSliceVar(std::vector<time::Duration>* p, std::string_view name, std::vector<time::Duration> value, std::string_view usage) { CommandLine().DurationSliceVarP(p, name, {}, std::move(value), usage); }
void DurationSliceVarP(std::vector<time::Duration>* p, std::string_view name, std::string_view shorthand, std::vector<time::Duration> value, std::string_view usage) { CommandLine().DurationSliceVarP(p, name, shorthand, std::move(value), usage); }
std::vector<time::Duration>* DurationSlice(std::string_view name, std::vector<time::Duration> value, std::string_view usage) { return CommandLine().DurationSliceP(name, {}, std::move(value), usage); }
std::vector<time::Duration>* DurationSliceP(std::string_view name, std::string_view shorthand, std::vector<time::Duration> value, std::string_view usage) { return CommandLine().DurationSliceP(name, shorthand, std::move(value), usage); }

void FlagSet::IPSliceVar(std::vector<net::IP>* p, std::string_view name, std::vector<net::IP> value, std::string_view usage) { IPSliceVarP(p, name, {}, std::move(value), usage); }
void FlagSet::IPSliceVarP(std::vector<net::IP>* p, std::string_view name, std::string_view shorthand, std::vector<net::IP> value, std::string_view usage) {
    define<ipSliceValue>(*this, p, name, shorthand, std::move(value), usage);
}
std::vector<net::IP>* FlagSet::IPSlice(std::string_view name, std::vector<net::IP> value, std::string_view usage) { return IPSliceP(name, {}, std::move(value), usage); }
std::vector<net::IP>* FlagSet::IPSliceP(std::string_view name, std::string_view shorthand, std::vector<net::IP> value, std::string_view usage) {
    auto v = make_ptr<ipSliceValue>(nullptr, std::move(value));
    std::vector<net::IP>* p = v->target();
    VarPF(v, name, shorthand, usage);
    return p;
}
std::pair<std::vector<net::IP>, gopp::error> FlagSet::GetIPSlice(std::string_view name) const { return getValue<ipSliceValue>(*this, name); }
void IPSliceVar(std::vector<net::IP>* p, std::string_view name, std::vector<net::IP> value, std::string_view usage) { CommandLine().IPSliceVarP(p, name, {}, std::move(value), usage); }
void IPSliceVarP(std::vector<net::IP>* p, std::string_view name, std::string_view shorthand, std::vector<net::IP> value, std::string_view usage) { CommandLine().IPSliceVarP(p, name, shorthand, std::move(value), usage); }
std::vector<net::IP>* IPSlice(std::string_view name, std::vector<net::IP> value, std::string_view usage) { return CommandLine().IPSliceP(name, {}, std::move(value), usage); }
std::vector<net::IP>* IPSliceP(std::string_view name, std::string_view shorthand, std::vector<net::IP> value, std::string_view usage) { return CommandLine().IPSliceP(name, shorthand, std::move(value), usage); }

void FlagSet::IPNetSliceVar(std::vector<net::IPNet>* p, std::string_view name, std::vector<net::IPNet> value, std::string_view usage) { IPNetSliceVarP(p, name, {}, std::move(value), usage); }
void FlagSet::IPNetSliceVarP(std::vector<net::IPNet>* p, std::string_view name, std::string_view shorthand, std::vector<net::IPNet> value, std::string_view usage) {
    define<ipNetSliceValue>(*this, p, name, shorthand, std::move(value), usage);
}
std::vector<net::IPNet>* FlagSet::IPNetSlice(std::string_view name, std::vector<net::IPNet> value, std::string_view usage) { return IPNetSliceP(name, {}, std::move(value), usage); }
std::vector<net::IPNet>* FlagSet::IPNetSliceP(std::string_view name, std::string_view shorthand, std::vector<net::IPNet> value, std::string_view usage) {
    auto v = make_ptr<ipNetSliceValue>(nullptr, std::move(value));
    std::vector<net::IPNet>* p = v->target();
    VarPF(v, name, shorthand, usage);
    return p;
}
std::pair<std::vector<net::IPNet>, gopp::error> FlagSet::GetIPNetSlice(std::string_view name) const { return getValue<ipNetSliceValue>(*this, name); }
void IPNetSliceVar(std::vector<net::IPNet>* p, std::string_view name, std::vector<net::IPNet> value, std::string_view usage) { CommandLine().IPNetSliceVarP(p, name, {}, std::move(value), usage); }
void IPNetSliceVarP(std::vector<net::IPNet>* p, std::string_view name, std::string_view shorthand, std::vector<net::IPNet> value, std::string_view usage) { CommandLine().IPNetSliceVarP(p, name, shorthand, std::move(value), usage); }
std::vector<net::IPNet>* IPNetSlice(std::string_view name, std::vector<net::IPNet> value, std::string_view usage) { return CommandLine().IPNetSliceP(name, {}, std::move(value), usage); }
std::vector<net::IPNet>* IPNetSliceP(std::string_view name, std::string_view shorthand, std::vector<net::IPNet> value, std::string_view usage) { return CommandLine().IPNetSliceP(name, shorthand, std::move(value), usage); }

void FlagSet::StringToStringVar(std::map<gopp::string, gopp::string>* p, std::string_view name, std::map<gopp::string, gopp::string> value, std::string_view usage) { StringToStringVarP(p, name, {}, std::move(value), usage); }
void FlagSet::StringToStringVarP(std::map<gopp::string, gopp::string>* p, std::string_view name, std::string_view shorthand, std::map<gopp::string, gopp::string> value, std::string_view usage) {
    define<stringToStringValue>(*this, p, name, shorthand, std::move(value), usage);
}
std::map<gopp::string, gopp::string>* FlagSet::StringToString(std::string_view name, std::map<gopp::string, gopp::string> value, std::string_view usage) { return StringToStringP(name, {}, std::move(value), usage); }
std::map<gopp::string, gopp::string>* FlagSet::StringToStringP(std::string_view name, std::string_view shorthand, std::map<gopp::string, gopp::string> value, std::string_view usage) {
    auto v = make_ptr<stringToStringValue>(nullptr, std::move(value));
    std::map<gopp::string, gopp::string>* p = v->target();
    VarPF(v, name, shorthand, usage);
    return p;
}
std::pair<std::map<gopp::string, gopp::string>, gopp::error> FlagSet::GetStringToString(std::string_view name) const { return getValue<stringToStringValue>(*this, name); }
void StringToStringVar(std::map<gopp::string, gopp::string>* p, std::string_view name, std::map<gopp::string, gopp::string> value, std::string_view usage) { CommandLine().StringToStringVarP(p, name, {}, std::move(value), usage); }
void StringToStringVarP(std::map<gopp::string, gopp::string>* p, std::string_view name, std::string_view shorthand, std::map<gopp::string, gopp::string> value, std::string_view usage) { CommandLine().StringToStringVarP(p, name, shorthand, std::move(value), usage); }
std::map<gopp::string, gopp::string>* StringToString(std::string_view name, std::map<gopp::string, gopp::string> value, std::string_view usage) { return CommandLine().StringToStringP(name, {}, std::move(value), usage); }
std::map<gopp::string, gopp::string>* StringToStringP(std::string_view name, std::string_view shorthand, std::map<gopp::string, gopp::string> value, std::string_view usage) { return CommandLine().StringToStringP(name, shorthand, std::move(value), usage); }

void FlagSet::StringToIntVar(std::map<gopp::string, isize>* p, std::string_view name, std::map<gopp::string, isize> value, std::string_view usage) { StringToIntVarP(p, name, {}, std::move(value), usage); }
void FlagSet::StringToIntVarP(std::map<gopp::string, isize>* p, std::string_view name, std::string_view shorthand, std::map<gopp::string, isize> value, std::string_view usage) {
    define<stringToIntValue>(*this, p, name, shorthand, std::move(value), usage);
}
std::map<gopp::string, isize>* FlagSet::StringToInt(std::string_view name, std::map<gopp::string, isize> value, std::string_view usage) { return StringToIntP(name, {}, std::move(value), usage); }
std::map<gopp::string, isize>* FlagSet::StringToIntP(std::string_view name, std::string_view shorthand, std::map<gopp::string, isize> value, std::string_view usage) {
    auto v = make_ptr<stringToIntValue>(nullptr, std::move(value));
    std::map<gopp::string, isize>* p = v->target();
    VarPF(v, name, shorthand, usage);
    return p;
}
std::pair<std::map<gopp::string, isize>, gopp::error> FlagSet::GetStringToInt(std::string_view name) const { return getValue<stringToIntValue>(*this, name); }
void StringToIntVar(std::map<gopp::string, isize>* p, std::string_view name, std::map<gopp::string, isize> value, std::string_view usage) { CommandLine().StringToIntVarP(p, name, {}, std::move(value), usage); }
void StringToIntVarP(std::map<gopp::string, isize>* p, std::string_view name, std::string_view shorthand, std::map<gopp::string, isize> value, std::string_view usage) { CommandLine().StringToIntVarP(p, name, shorthand, std::move(value), usage); }
std::map<gopp::string, isize>* StringToInt(std::string_view name, std::map<gopp::string, isize> value, std::string_view usage) { return CommandLine().StringToIntP(name, {}, std::move(value), usage); }
std::map<gopp::string, isize>* StringToIntP(std::string_view name, std::string_view shorthand, std::map<gopp::string, isize> value, std::string_view usage) { return CommandLine().StringToIntP(name, shorthand, std::move(value), usage); }

void FlagSet::StringToInt64Var(std::map<gopp::string, int64>* p, std::string_view name, std::map<gopp::string, int64> value, std::string_view usage) { StringToInt64VarP(p, name, {}, std::move(value), usage); }
void FlagSet::StringToInt64VarP(std::map<gopp::string, int64>* p, std::string_view name, std::string_view shorthand, std::map<gopp::string, int64> value, std::string_view usage) {
    define<stringToInt64Value>(*this, p, name, shorthand, std::move(value), usage);
}
std::map<gopp::string, int64>* FlagSet::StringToInt64(std::string_view name, std::map<gopp::string, int64> value, std::string_view usage) { return StringToInt64P(name, {}, std::move(value), usage); }
std::map<gopp::string, int64>* FlagSet::StringToInt64P(std::string_view name, std::string_view shorthand, std::map<gopp::string, int64> value, std::string_view usage) {
    auto v = make_ptr<stringToInt64Value>(nullptr, std::move(value));
    std::map<gopp::string, int64>* p = v->target();
    VarPF(v, name, shorthand, usage);
    return p;
}
std::pair<std::map<gopp::string, int64>, gopp::error> FlagSet::GetStringToInt64(std::string_view name) const { return getValue<stringToInt64Value>(*this, name); }
void StringToInt64Var(std::map<gopp::string, int64>* p, std::string_view name, std::map<gopp::string, int64> value, std::string_view usage) { CommandLine().StringToInt64VarP(p, name, {}, std::move(value), usage); }
void StringToInt64VarP(std::map<gopp::string, int64>* p, std::string_view name, std::string_view shorthand, std::map<gopp::string, int64> value, std::string_view usage) { CommandLine().StringToInt64VarP(p, name, shorthand, std::move(value), usage); }
std::map<gopp::string, int64>* StringToInt64(std::string_view name, std::map<gopp::string, int64> value, std::string_view usage) { return CommandLine().StringToInt64P(name, {}, std::move(value), usage); }
std::map<gopp::string, int64>* StringToInt64P(std::string_view name, std::string_view shorthand, std::map<gopp::string, int64> value, std::string_view usage) { return CommandLine().StringToInt64P(name, shorthand, std::move(value), usage); }

}  // namespace gopp::pflag
