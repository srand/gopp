// After Go's database/sql/driver: driver.go and types.go.
#include "internal.hpp"

#include <gopp/strings.hpp>

#include <cstring>
#include <ctime>

namespace gopp::sql::driver {

const error ErrSkip = errors::New("driver: skip fast-path; continue as if unimplemented");
const error ErrBadConn = errors::New("driver: bad connection");
const error ErrRemoveArgument = errors::New("driver: remove argument from query");

std::string_view TypeName(const Value& v) noexcept {
    static constexpr std::string_view names[] = {"<nil>", "int64", "float64", "bool", "[]uint8", "string", "time.Time"};
    return names[v.index()];
}

namespace {

// Go's Time.String: "2006-01-02 15:04:05.999999999 +0000 UTC".
std::string time_string(const time::Time& t) {
    int64 ns = t.UnixNano();
    int64 sec = ns >= 0 ? ns / 1000000000 : -((-ns + 999999999) / 1000000000);
    int64 frac = ns - sec * 1000000000;
    std::time_t tt = static_cast<std::time_t>(sec);
    std::tm tm;
    gmtime_r(&tt, &tm);
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    std::string s = buf;
    if (frac) {
        char f[16];
        std::snprintf(f, sizeof(f), ".%09lld", static_cast<long long>(frac));
        std::string fs = f;
        while (fs.back() == '0') fs.pop_back();
        s += fs;
    }
    return s + " +0000 UTC";
}

}  // namespace

string Format(const Value& v) {
    switch (v.index()) {
    case 0: return string::from_static("<nil>");
    case 1: return strconv::Itoa(std::get<int64>(v));
    case 2: return string(sql::detail::format_float_g(std::get<double>(v), 64));
    case 3: return string::from_static(std::get<bool>(v) ? "true" : "false");
    case 4: {
        // Go's %v of a []byte: [104 105]
        std::string s = "[";
        bool first = true;
        for (byte b : std::get<bytes>(v)) {
            if (!first) s += ' ';
            s += std::to_string(b);
            first = false;
        }
        return string(s + "]");
    }
    case 5: return std::get<string>(v);
    default: return string(time_string(std::get<time::Time>(v)));
    }
}

bool Equal(const Value& a, const Value& b) noexcept {
    if (a.index() != b.index()) return false;
    switch (a.index()) {
    case 0: return true;
    case 1: return std::get<int64>(a) == std::get<int64>(b);
    case 2: return std::get<double>(a) == std::get<double>(b);
    case 3: return std::get<bool>(a) == std::get<bool>(b);
    case 4: return as_string_view(std::get<bytes>(a).span()) == as_string_view(std::get<bytes>(b).span());
    case 5: return std::get<string>(a) == std::get<string>(b);
    default: return std::get<time::Time>(a).Equal(std::get<time::Time>(b));
    }
}

// ---------------------------------------------------------------------------
// Results

namespace {

struct rows_affected final : Result {
    int64 n;
    explicit rows_affected(int64 v) : n(v) {}
    std::pair<int64, error> LastInsertId() override {
        return {0, errors::New("LastInsertId is not supported by this driver")};
    }
    std::pair<int64, error> RowsAffected() override { return {n, nullptr}; }
};

struct no_rows final : Result {
    std::pair<int64, error> LastInsertId() override {
        return {0, errors::New("no LastInsertId available after DDL statement")};
    }
    std::pair<int64, error> RowsAffected() override {
        return {0, errors::New("no RowsAffected available after DDL statement")};
    }
};

}  // namespace

ptr<Result> RowsAffected(int64 n) { return make_ptr<rows_affected>(n); }
const ptr<Result> ResultNoRows = make_ptr<no_rows>();

// ---------------------------------------------------------------------------
// Converters

namespace {

error errorf(std::string msg) { return errors::New(string(msg)); }

struct bool_type final : ValueConverter {
    std::pair<Value, error> ConvertValue(const Value& src) const override {
        switch (src.index()) {
        case 3: return {src, nullptr};
        case 5:
        case 4: {
            std::string_view s = src.index() == 5 ? std::get<string>(src).view() : as_string_view(std::get<bytes>(src).span());
            auto [b, err] = strconv::ParseBool(s);
            if (err) return {nullptr, errorf("sql/driver: couldn't convert " + sql::detail::quote(s).str() + " into type bool")};
            return {b, nullptr};
        }
        case 1: {
            int64 iv = std::get<int64>(src);
            if (iv == 1 || iv == 0) return {iv == 1, nullptr};
            return {nullptr, errorf("sql/driver: couldn't convert " + std::to_string(iv) + " into type bool")};
        }
        }
        return {nullptr, errorf("sql/driver: couldn't convert " + Format(src).str() + " (" + std::string(TypeName(src)) +
                                ") into type bool")};
    }
};

struct int32_type final : ValueConverter {
    std::pair<Value, error> ConvertValue(const Value& v) const override {
        if (auto* i = std::get_if<int64>(&v)) {
            if (*i > (int64(1) << 31) - 1 || *i < -(int64(1) << 31)) {
                return {nullptr, errorf("sql/driver: value " + std::to_string(*i) + " overflows int32")};
            }
            return {*i, nullptr};
        }
        if (auto* s = std::get_if<string>(&v)) {
            auto [i, err] = strconv::Atoi(*s);
            if (err) return {nullptr, errorf("sql/driver: value " + sql::detail::quote(*s).str() + " can't be converted to int32")};
            return {static_cast<int64>(i), nullptr};
        }
        return {nullptr, errorf("sql/driver: unsupported value " + Format(v).str() + " (type " + std::string(TypeName(v)) +
                                ") converting to int32")};
    }
};

struct string_type final : ValueConverter {
    std::pair<Value, error> ConvertValue(const Value& v) const override {
        if (v.index() == 5 || v.index() == 4) return {v, nullptr};
        return {Format(v), nullptr};
    }
};

struct default_converter final : ValueConverter {
    std::pair<Value, error> ConvertValue(const Value& v) const override { return {v, nullptr}; }
};

const bool_type bool_v;
const int32_type int32_v;
const string_type string_v;
const default_converter default_v;

}  // namespace

const ValueConverter& Bool = bool_v;
const ValueConverter& Int32 = int32_v;
const ValueConverter& String = string_v;
const ValueConverter& DefaultParameterConverter = default_v;

std::pair<Value, error> Null::ConvertValue(const Value& v) const {
    if (std::holds_alternative<std::nullptr_t>(v)) return {nullptr, nullptr};
    return Converter.ConvertValue(v);
}

std::pair<Value, error> NotNull::ConvertValue(const Value& v) const {
    if (std::holds_alternative<std::nullptr_t>(v)) return {nullptr, errors::New("nil value not allowed")};
    return Converter.ConvertValue(v);
}

}  // namespace gopp::sql::driver
