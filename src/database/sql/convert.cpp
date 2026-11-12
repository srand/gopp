// After Go's database/sql/convert.go: argument conversion for drivers and
// value conversion for Scan.
#include "internal.hpp"

#include <gopp/strings.hpp>

#include <charconv>
#include <cmath>
#include <cstdio>
#include <ctime>

namespace gopp::sql {

namespace detail {

const error err_nil_ptr = errors::New("destination pointer is nil");
const error err_uint64_high_bit = errors::New("uint64 values with high bit set are not supported");

namespace {

struct wrapped_error final : error_interface {
    string msg;
    gopp::error inner;
    wrapped_error(string m, gopp::error e) : msg(std::move(m)), inner(std::move(e)) {}
    string Error() const override { return msg; }
    const error_interface* Unwrap() const noexcept override { return inner.get(); }
};

}  // namespace

error wrapf(string msg, error inner) { return make_ptr<wrapped_error>(std::move(msg), std::move(inner)); }
error errorf(string msg) { return errors::New(std::move(msg)); }

string quote(std::string_view s) { return strconv::Quote(s); }

std::string format_float_g(double f, int bits) {
    // strconv.FormatFloat(f, 'g', -1, bits): the shortest digits, in %e
    // form when the exponent is < -4 or >= 6 (Go decides with precision 6
    // when the precision is the shortest).
    if (std::isnan(f)) return "NaN";
    if (std::isinf(f)) return f > 0 ? "+Inf" : "-Inf";
    char buf[64];
    std::to_chars_result r;
    if (bits == 32) r = std::to_chars(buf, buf + sizeof(buf), static_cast<float>(f), std::chars_format::scientific);
    else r = std::to_chars(buf, buf + sizeof(buf), f, std::chars_format::scientific);
    std::string sci(buf, r.ptr);
    // "-d.ddde+XX": the digits and the exponent.
    bool neg = sci[0] == '-';
    std::string_view s = sci;
    if (neg) s.remove_prefix(1);
    auto e = s.find('e');
    std::string digits;
    for (char c : s.substr(0, e)) {
        if (c != '.') digits += c;
    }
    int exp = std::atoi(std::string(s.substr(e + 1)).c_str());
    while (digits.size() > 1 && digits.back() == '0') digits.pop_back();
    if (digits == "0") exp = 0;
    std::string out = neg ? "-" : "";
    if (exp < -4 || exp >= 6) {
        // %e: d.ddde±XX
        out += digits[0];
        if (digits.size() > 1) out += "." + digits.substr(1);
        char eb[16];  // e, sign and any int
        std::snprintf(eb, sizeof(eb), "e%c%02d", exp < 0 ? '-' : '+', exp < 0 ? -exp : exp);
        return out + eb;
    }
    // %f with as many digits as needed.
    if (exp < 0) {
        out += "0." + std::string(static_cast<std::size_t>(-exp - 1), '0') + digits;
    } else if (static_cast<int>(digits.size()) <= exp + 1) {
        out += digits + std::string(static_cast<std::size_t>(exp + 1 - static_cast<int>(digits.size())), '0');
    } else {
        out += digits.substr(0, static_cast<std::size_t>(exp + 1)) + "." + digits.substr(static_cast<std::size_t>(exp + 1));
    }
    return out;
}

std::pair<double, error> parse_float(std::string_view s, int bits) {
    auto fail = [&](const error& e) {
        return std::pair<double, error>{0, make_ptr<strconv::NumError>(string::from_static("ParseFloat"), string(s), e)};
    };
    std::string_view t = s;
    bool neg = false;
    if (!t.empty() && (t[0] == '+' || t[0] == '-')) {
        neg = t[0] == '-';
        t.remove_prefix(1);
    }
    if (t.empty()) return fail(strconv::ErrSyntax);
    std::string lower = strings::ToLower(t).str();
    if (lower == "inf" || lower == "infinity") return {neg ? -HUGE_VAL : HUGE_VAL, nullptr};
    if (lower == "nan") return {std::nan(""), nullptr};
    double v;
    if (bits == 32) {
        float f;
        auto r = std::from_chars(t.data(), t.data() + t.size(), f);
        if (r.ec == std::errc::invalid_argument || r.ptr != t.data() + t.size()) return fail(strconv::ErrSyntax);
        if (r.ec == std::errc::result_out_of_range) return fail(strconv::ErrRange);
        v = f;
    } else {
        auto r = std::from_chars(t.data(), t.data() + t.size(), v);
        if (r.ec == std::errc::invalid_argument || r.ptr != t.data() + t.size()) return fail(strconv::ErrSyntax);
        if (r.ec == std::errc::result_out_of_range) return fail(strconv::ErrRange);
    }
    return {neg ? -v : v, nullptr};
}

std::string format_rfc3339_nano(const time::Time& t) {
    int64 ns = t.UnixNano();
    int64 sec = ns >= 0 ? ns / 1000000000 : -((-ns + 999999999) / 1000000000);
    int64 frac = ns - sec * 1000000000;
    std::time_t tt = static_cast<std::time_t>(sec);
    std::tm tm;
    gmtime_r(&tt, &tm);
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
    std::string s = buf;
    if (frac) {
        char f[16];
        std::snprintf(f, sizeof(f), ".%09lld", static_cast<long long>(frac));
        std::string fs = f;
        while (fs.back() == '0') fs.pop_back();
        s += fs;
    }
    return s + "Z";
}

namespace {

// asString
std::string as_string(const driver::Value& src) {
    switch (src.index()) {
    case 5: return std::get<string>(src).str();
    case 4: return std::string(as_string_view(std::get<bytes>(src).span()));
    case 1: return std::to_string(std::get<int64>(src));
    case 2: return format_float_g(std::get<double>(src), 64);
    case 3: return std::get<bool>(src) ? "true" : "false";
    default: return driver::Format(src).str();
    }
}

// asBytes: appends src's text for numbers, bools and strings.
bool append_text(std::string& buf, const driver::Value& src) {
    switch (src.index()) {
    case 1: case 2: case 3: case 5: buf += as_string(src); return true;
    default: return false;
    }
}

bytes bytes_of(std::string_view s) { return bytes::of(gopp::as_bytes(s)); }

// strconvErr: the cause of a strconv.NumError.
error strconv_err(const error& err) {
    if (auto* ne = errors::As<strconv::NumError>(err)) return ne->Err;
    return err;
}

error null_unsupported(std::string_view kind) {
    return errorf("converting NULL to " + string(kind) + " is unsupported");
}

error convert_failed(const driver::Value& src, const std::string& s, std::string_view kind, const error& err) {
    return errorf("converting driver.Value type " + string(driver::TypeName(src)) + " (" + quote(s) + ") to a " +
                  string(kind) + ": " + strconv_err(err).Error());
}

}  // namespace

error unsupported_scan(const driver::Value& src, std::string_view dest_type) {
    return errorf("unsupported Scan, storing driver.Value type " + string(driver::TypeName(src)) + " into type " +
                  string(dest_type));
}

error convert_string(const driver::Value& src, gopp::string& out) {
    switch (src.index()) {
    case 5: out = std::get<string>(src); return nullptr;
    case 4: out = string(as_string_view(std::get<bytes>(src).span())); return nullptr;
    case 6: out = string(format_rfc3339_nano(std::get<time::Time>(src))); return nullptr;
    case 0: return null_unsupported("string");
    default: out = string(as_string(src)); return nullptr;
    }
}

error convert_bytes(const driver::Value& src, bytes& out) {
    switch (src.index()) {
    case 5: out = bytes_of(std::get<string>(src)); return nullptr;
    case 4: out = bytes::of(std::get<bytes>(src).span()); return nullptr;  // a copy
    case 6: out = bytes_of(format_rfc3339_nano(std::get<time::Time>(src))); return nullptr;
    case 0: out = bytes(); return nullptr;
    }
    std::string b;
    if (append_text(b, src)) {
        out = bytes_of(b);
        return nullptr;
    }
    return unsupported_scan(src, "*[]uint8");
}

error convert_raw_bytes(const driver::Value& src, RawBytes& out, Rows* rows) {
    auto append_raw = [&](std::string_view s) {
        bytes buf = rows ? rows->rawbuf() : bytes();
        bytes b = append_span(buf, gopp::as_bytes(s));
        out = rows ? rows->setrawbuf(b) : RawBytes(b);
    };
    switch (src.index()) {
    case 5: append_raw(std::get<string>(src)); return nullptr;
    case 4: out = RawBytes(std::get<bytes>(src)); return nullptr;  // the driver's memory, as in Go
    case 6: append_raw(format_rfc3339_nano(std::get<time::Time>(src))); return nullptr;
    case 0: out = RawBytes(); return nullptr;
    }
    std::string b;
    if (append_text(b, src)) {
        append_raw(b);
        return nullptr;
    }
    return unsupported_scan(src, "*sql.RawBytes");
}

error convert_bool(const driver::Value& src, bool& out) {
    auto [bv, err] = driver::Bool.ConvertValue(src);
    if (!err) out = std::get<bool>(bv);
    return err;
}

error convert_any(const driver::Value& src, driver::Value& out) {
    if (auto* b = std::get_if<bytes>(&src)) {
        out = bytes::of(b->span());  // a copy: the driver may reuse its memory
        return nullptr;
    }
    out = src;
    return nullptr;
}

error convert_int(const driver::Value& src, int bits, std::string_view kind, int64& out) {
    if (std::holds_alternative<std::nullptr_t>(src)) return null_unsupported(kind);
    std::string s = as_string(src);
    auto [i, err] = strconv::ParseInt(s, 10, bits);
    if (err) return convert_failed(src, s, kind, err);
    out = i;
    return nullptr;
}

error convert_uint(const driver::Value& src, int bits, std::string_view kind, uint64& out) {
    if (std::holds_alternative<std::nullptr_t>(src)) return null_unsupported(kind);
    std::string s = as_string(src);
    auto [u, err] = strconv::ParseUint(s, 10, bits);
    if (err) return convert_failed(src, s, kind, err);
    out = u;
    return nullptr;
}

error convert_float(const driver::Value& src, int bits, std::string_view kind, double& out) {
    if (std::holds_alternative<std::nullptr_t>(src)) return null_unsupported(kind);
    std::string s = as_string(src);
    auto [f, err] = parse_float(s, bits);
    if (err) return convert_failed(src, s, kind, err);
    out = f;
    return nullptr;
}

error convert_time(const driver::Value& src, time::Time& out) {
    if (auto* t = std::get_if<time::Time>(&src)) {
        out = *t;
        return nullptr;
    }
    return unsupported_scan(src, "*time.Time");
}

// ---------------------------------------------------------------------------
// Arguments for drivers

namespace {

string describe_named_value(const driver::NamedValue& nv) {
    if (nv.Name.empty()) return "$" + strconv::Itoa(nv.Ordinal);
    return "with name " + quote(nv.Name);
}

error validate_named_value_name(std::string_view name) {
    if (name.empty()) return nullptr;
    unsigned char c = static_cast<unsigned char>(name[0]);
    // unicode.IsLetter on the first rune; non-ASCII runes count as letters.
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c >= 0x80) return nullptr;
    return errorf("name " + quote(name) + " does not begin with a letter");
}

// ccChecker: arguments through a driver's ColumnConverter.
struct cc_checker {
    driver::ColumnConverter* cci = nullptr;
    int want = -1;

    error check(driver::NamedValue& nv) const {
        if (!cci) return driver::ErrSkip;
        int index = nv.Ordinal - 1;
        // The number of arguments is checked later; don't index past
        // what the statement takes.
        if (want >= 0 && want <= index) return nullptr;
        auto [v, err] = cci->Converter(index).ConvertValue(nv.Value);
        if (err) return err;
        nv.Value = std::move(v);
        return nullptr;
    }
};

error default_check_named_value(driver::NamedValue& nv) {
    auto [v, err] = driver::DefaultParameterConverter.ConvertValue(nv.Value);
    nv.Value = std::move(v);
    return err;
}

}  // namespace

std::pair<std::vector<driver::NamedValue>, error> driver_args_conn_locked(driver::Conn* ci, driver_stmt* ds,
                                                                          std::span<const arg> args) {
    std::vector<driver::NamedValue> nvargs;
    nvargs.reserve(args.size());
    int want = -1;
    driver::Stmt* si = nullptr;
    cc_checker cc;
    if (ds) {
        si = ds->si.get();
        want = si->NumInput();
        cc.want = want;
    }
    driver::NamedValueChecker* nvc = si ? dynamic_cast<driver::NamedValueChecker*>(si) : nullptr;
    if (!nvc) nvc = dynamic_cast<driver::NamedValueChecker*>(ci);
    auto* cci = si ? dynamic_cast<driver::ColumnConverter*>(si) : nullptr;
    cc.cci = cci;

    for (const arg& a : args) {
        driver::NamedValue nv;
        if (!a.name.empty()) {
            if (error err = validate_named_value_name(a.name)) return {{}, err};
            nv.Name = a.name;
        }
        nv.Ordinal = static_cast<int>(nvargs.size()) + 1;
        nv.Value = a.value;
        if (a.err) {
            return {{}, wrapf("sql: converting argument " + describe_named_value(nv) + " type: " + a.err.Error(), a.err)};
        }
        enum { default_check, nvc_check, cc_check } checker = default_check;
        bool next_cc = false;
        if (nvc) {
            next_cc = cci != nullptr;
            checker = nvc_check;
        } else if (cci) {
            checker = cc_check;
        }
        for (;;) {
            error err;
            switch (checker) {
            case nvc_check: err = nvc->CheckNamedValue(nv); break;
            case cc_check: err = cc.check(nv); break;
            default: err = default_check_named_value(nv); break;
            }
            if (!err) {
                nvargs.push_back(std::move(nv));
                break;
            }
            if (err == driver::ErrRemoveArgument) break;
            if (err == driver::ErrSkip) {
                if (next_cc) {
                    next_cc = false;
                    checker = cc_check;
                } else {
                    checker = default_check;
                }
                continue;
            }
            return {{}, wrapf("sql: converting argument " + describe_named_value(nv) + " type: " + err.Error(), err)};
        }
    }
    if (want != -1 && static_cast<int>(nvargs.size()) != want) {
        return {{}, errorf("sql: expected " + strconv::Itoa(want) + " arguments, got " +
                           strconv::Itoa(static_cast<int64>(nvargs.size())))};
    }
    return {std::move(nvargs), nullptr};
}

}  // namespace detail

// ---------------------------------------------------------------------------
// The Null types

namespace {

bool is_null(const driver::Value& v) { return std::holds_alternative<std::nullptr_t>(v); }

}  // namespace

error NullString::Scan(const driver::Value& value) {
    if (is_null(value)) {
        String = {};
        Valid = false;
        return nullptr;
    }
    error err = convert_assign(&String, value);
    Valid = !err;
    return err;
}

std::pair<driver::Value, error> NullString::Value() const {
    if (!Valid) return {nullptr, nullptr};
    return {String, nullptr};
}

error NullInt64::Scan(const driver::Value& value) {
    if (is_null(value)) {
        Int64 = 0;
        Valid = false;
        return nullptr;
    }
    error err = convert_assign(&Int64, value);
    Valid = !err;
    return err;
}

std::pair<driver::Value, error> NullInt64::Value() const {
    if (!Valid) return {nullptr, nullptr};
    return {Int64, nullptr};
}

error NullInt32::Scan(const driver::Value& value) {
    if (is_null(value)) {
        Int32 = 0;
        Valid = false;
        return nullptr;
    }
    error err = convert_assign(&Int32, value);
    Valid = !err;
    return err;
}

std::pair<driver::Value, error> NullInt32::Value() const {
    if (!Valid) return {nullptr, nullptr};
    return {static_cast<int64>(Int32), nullptr};
}

error NullInt16::Scan(const driver::Value& value) {
    if (is_null(value)) {
        Int16 = 0;
        Valid = false;
        return nullptr;
    }
    error err = convert_assign(&Int16, value);
    Valid = !err;
    return err;
}

std::pair<driver::Value, error> NullInt16::Value() const {
    if (!Valid) return {nullptr, nullptr};
    return {static_cast<int64>(Int16), nullptr};
}

error NullByte::Scan(const driver::Value& value) {
    if (is_null(value)) {
        Byte = 0;
        Valid = false;
        return nullptr;
    }
    error err = convert_assign(&Byte, value);
    Valid = !err;
    return err;
}

std::pair<driver::Value, error> NullByte::Value() const {
    if (!Valid) return {nullptr, nullptr};
    return {static_cast<int64>(Byte), nullptr};
}

error NullFloat64::Scan(const driver::Value& value) {
    if (is_null(value)) {
        Float64 = 0;
        Valid = false;
        return nullptr;
    }
    error err = convert_assign(&Float64, value);
    Valid = !err;
    return err;
}

std::pair<driver::Value, error> NullFloat64::Value() const {
    if (!Valid) return {nullptr, nullptr};
    return {Float64, nullptr};
}

error NullBool::Scan(const driver::Value& value) {
    if (is_null(value)) {
        Bool = false;
        Valid = false;
        return nullptr;
    }
    error err = convert_assign(&Bool, value);
    Valid = !err;
    return err;
}

std::pair<driver::Value, error> NullBool::Value() const {
    if (!Valid) return {nullptr, nullptr};
    return {Bool, nullptr};
}

error NullTime::Scan(const driver::Value& value) {
    if (is_null(value)) {
        Time = {};
        Valid = false;
        return nullptr;
    }
    error err = detail::convert_time(value, Time);
    Valid = !err;
    return err;
}

std::pair<driver::Value, error> NullTime::Value() const {
    if (!Valid) return {nullptr, nullptr};
    return {Time, nullptr};
}

string IsolationLevel::String() const {
    switch (v_) {
    case 0: return string::from_static("Default");
    case 1: return string::from_static("Read Uncommitted");
    case 2: return string::from_static("Read Committed");
    case 3: return string::from_static("Write Committed");
    case 4: return string::from_static("Repeatable Read");
    case 5: return string::from_static("Snapshot");
    case 6: return string::from_static("Serializable");
    case 7: return string::from_static("Linearizable");
    default: return "IsolationLevel(" + strconv::Itoa(v_) + ")";
    }
}

}  // namespace gopp::sql
