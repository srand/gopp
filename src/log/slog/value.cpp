// slog's levels, values, attributes and records, after Go 1.27.3's
// log/slog/level.go, value.go, attr.go and record.go.
#include <gopp/fmt.hpp>
#include <gopp/log/slog.hpp>
#include <gopp/strings.hpp>

#include "text/template/goconv.hpp"

#include <algorithm>

namespace gopp::slog {

namespace conv = text::tmpl::detail;

// ---------------------------------------------------------------------------
// Level

string Level::String() const {
    auto str = [](std::string_view base, Level val) -> string {
        if (val.value == 0) return string(base);
        std::string s(base);
        if (val.value > 0) s.push_back('+');
        s += std::to_string(val.value);
        return string(s);
    };
    if (*this < LevelInfo) return str("DEBUG", *this - LevelDebug);
    if (*this < LevelWarn) return str("INFO", *this - LevelInfo);
    if (*this < LevelError) return str("WARN", *this - LevelWarn);
    return str("ERROR", *this - LevelError);
}

std::pair<slice<byte>, error> Level::MarshalJSON() const { return {to_bytes(conv::quote(String().view())), nullptr}; }

namespace {

// Go's Level.parse.
error parse_level(std::string_view s, Level& l) {
    auto wrap = [&](error err) { return fmt::Errorf("slog: level string {}: {}", conv::quote(s), fmt::W(err)); };
    std::string_view name = s;
    int offset = 0;
    if (auto i = s.find_first_of("+-"); i != s.npos) {
        name = s.substr(0, i);
        auto [n, err] = strconv::Atoi(s.substr(i));
        if (err) return wrap(err);
        offset = static_cast<int>(n);
    }
    string upper = strings::ToUpper(name);
    if (upper == "DEBUG") l = LevelDebug;
    else if (upper == "INFO") l = LevelInfo;
    else if (upper == "WARN") l = LevelWarn;
    else if (upper == "ERROR") l = LevelError;
    else return wrap(errors::New("unknown name"));
    l += offset;
    return nullptr;
}

}  // namespace

error Level::UnmarshalJSON(std::span<const byte> data) {
    auto [s, err] = conv::unquote(as_string_view(data));
    if (err) return err;
    return parse_level(s, *this);
}

std::pair<slice<byte>, error> Level::AppendText(slice<byte> b) const { return {append_span(std::move(b), as_bytes(String().view())), nullptr}; }

std::pair<slice<byte>, error> Level::MarshalText() const { return AppendText(slice<byte>()); }

error Level::UnmarshalText(std::span<const byte> data) { return parse_level(as_string_view(data), *this); }

string LevelVar::String() const { return fmt::Sprint("LevelVar({})", Level()); }

std::pair<slice<byte>, error> LevelVar::AppendText(slice<byte> b) const { return Level().AppendText(std::move(b)); }

std::pair<slice<byte>, error> LevelVar::MarshalText() const { return AppendText(slice<byte>()); }

error LevelVar::UnmarshalText(std::span<const byte> data) {
    slog::Level l;
    if (error err = l.UnmarshalText(data)) return err;
    Set(l);
    return nullptr;
}

// ---------------------------------------------------------------------------
// Kind and Value

string Kind::String() const {
    static constexpr std::string_view names[] = {"Any",    "Bool", "Duration", "Float64", "Int64",
                                                 "String", "Time", "Uint64",   "Group",   "LogValuer"};
    if (value >= 0 && value < static_cast<int>(std::size(names))) return string::from_static(names[value]);
    return string::from_static("<unknown slog.Kind>");
}

Value LogValuer::LogValue() const { return p_->log_value(); }

Value StringValue(string value) {
    Value v(KindString, value.size());
    v.data_ = std::move(value);
    return v;
}

Value Int64Value(int64 v) noexcept { return Value(KindInt64, static_cast<uint64>(v)); }
Value Uint64Value(uint64 v) noexcept { return Value(KindUint64, v); }
Value Float64Value(double v) noexcept { return Value(KindFloat64, std::bit_cast<uint64>(v)); }
Value BoolValue(bool v) noexcept { return Value(KindBool, v ? 1 : 0); }
Value DurationValue(time::Duration v) noexcept { return Value(KindDuration, static_cast<uint64>(v.Nanoseconds())); }

Value TimeValue(time::Time v) {
    Value r(KindTime, 0);
    // Round(0) drops the monotonic reading, as Go's Value does.
    r.data_ = v.IsZero() ? time::Time() : v.Round(time::Duration(0));
    return r;
}

Value GroupValue(std::span<const Attr> as) {
    auto g = std::make_shared<std::vector<Attr>>();
    g->reserve(as.size());
    for (const Attr& a : as) {
        if (!a.Value.is_empty_group()) g->push_back(a);
    }
    Value v(KindGroup, g->size());
    v.data_ = Value::group(std::move(g));
    return v;
}

Value Value::of_any(ptr<const detail::any_value> p) {
    Value v(p->log_valuer() ? KindLogValuer : KindAny, 0);
    v.data_ = std::move(p);
    return v;
}

slog::Kind Value::Kind() const noexcept { return kind_; }

namespace {

[[noreturn]] void bad_kind(Kind got, Kind want) {
    panic(fmt::Sprint("Value kind is {}, not {}", got, want).view());
}

}  // namespace

int64 Value::Int64() const {
    if (kind_ != KindInt64) bad_kind(kind_, KindInt64);
    return static_cast<int64>(num_);
}

uint64 Value::Uint64() const {
    if (kind_ != KindUint64) bad_kind(kind_, KindUint64);
    return num_;
}

bool Value::Bool() const {
    if (kind_ != KindBool) bad_kind(kind_, KindBool);
    return num_ == 1;
}

time::Duration Value::Duration() const {
    if (kind_ != KindDuration) bad_kind(kind_, KindDuration);
    return time::Duration(static_cast<int64>(num_));
}

double Value::Float64() const {
    if (kind_ != KindFloat64) bad_kind(kind_, KindFloat64);
    return std::bit_cast<double>(num_);
}

time::Time Value::Time() const {
    if (kind_ != KindTime) bad_kind(kind_, KindTime);
    return std::get<time::Time>(data_);
}

slog::LogValuer Value::LogValuer() const {
    if (kind_ != KindLogValuer) bad_kind(kind_, KindLogValuer);
    return slog::LogValuer(std::get<ptr<const detail::any_value>>(data_));
}

std::span<const Attr> Value::Group() const {
    if (kind_ != KindGroup) panic("Group: bad kind");
    return *std::get<group>(data_);
}

std::any Value::Any() const {
    switch (kind_) {
    case KindAny:
    case KindLogValuer:
        if (auto* p = std::get_if<ptr<const detail::any_value>>(&data_)) return (*p)->to_any();
        return {};
    case KindGroup: return std::vector<Attr>(Group().begin(), Group().end());
    case KindInt64: return static_cast<int64>(num_);
    case KindUint64: return num_;
    case KindFloat64: return std::bit_cast<double>(num_);
    case KindString: return std::get<string>(data_);
    case KindBool: return num_ == 1;
    case KindDuration: return Duration();
    case KindTime: return Time();
    }
    panic(fmt::Sprint("bad kind: {}", kind_).view());
}

string Value::String() const {
    if (kind_ == KindString) return std::get<string>(data_);
    std::string buf;
    append(buf);
    return string(buf);
}

bool Value::is_empty_group() const noexcept { return kind_ == KindGroup && std::get<group>(data_)->empty(); }

void Value::append(std::string& dst) const {
    switch (kind_) {
    case KindString: dst += std::get<string>(data_).view(); return;
    case KindInt64: dst += std::to_string(static_cast<int64>(num_)); return;
    case KindUint64: dst += std::to_string(num_); return;
    case KindFloat64: dst += strconv::FormatFloat(std::bit_cast<double>(num_), 'g', -1, 64).view(); return;
    case KindBool: dst += num_ == 1 ? "true" : "false"; return;
    case KindDuration: dst += Duration().String().view(); return;
    case KindTime: dst += Time().Format("2006-01-02 15:04:05.999999999 -0700 MST").view(); return;
    case KindGroup: {
        // fmt's %v of a []Attr: each Attr's String.
        dst.push_back('[');
        bool first = true;
        for (const Attr& a : Group()) {
            if (!first) dst.push_back(' ');
            first = false;
            dst += a.String().view();
        }
        dst.push_back(']');
        return;
    }
    case KindAny:
    case KindLogValuer:
        if (auto* p = std::get_if<ptr<const detail::any_value>>(&data_)) (*p)->print(dst, false);
        else dst += "<nil>";
        return;
    }
    panic(fmt::Sprint("bad kind: {}", kind_).view());
}

bool Value::Equal(const Value& w) const {
    if (kind_ != w.kind_) return false;
    switch (kind_) {
    case KindInt64:
    case KindUint64:
    case KindBool:
    case KindDuration: return num_ == w.num_;
    case KindString: return std::get<string>(data_) == std::get<string>(w.data_);
    case KindFloat64: return std::bit_cast<double>(num_) == std::bit_cast<double>(w.num_);
    case KindTime: return Time().Equal(w.Time());
    case KindAny:
    case KindLogValuer: {
        auto* a = std::get_if<ptr<const detail::any_value>>(&data_);
        auto* b = std::get_if<ptr<const detail::any_value>>(&w.data_);
        if (!a || !b) return !a && !b;
        return (*a)->equal(**b);
    }
    case KindGroup: {
        auto g1 = Group(), g2 = w.Group();
        return std::equal(g1.begin(), g1.end(), g2.begin(), g2.end(),
                          [](const Attr& x, const Attr& y) { return x.Equal(y); });
    }
    }
    panic(fmt::Sprint("bad kind: {}", kind_).view());
}

Value Value::Resolve() const {
    constexpr int max_log_values = 100;
    Value v = *this;
    for (int i = 0; i < max_log_values; i++) {
        if (v.kind_ != KindLogValuer) return v;
        v = std::get<ptr<const detail::any_value>>(v.data_)->log_value();
    }
    auto* p = std::get_if<ptr<const detail::any_value>>(&data_);
    std::string_view type = p ? (*p)->type_name() : std::string_view("<nil>");
    return AnyValue(fmt::Errorf("LogValue called too many times on Value of type {}", type));
}

string Attr::String() const { return Key + "=" + Value.String(); }

namespace detail {

std::string quote(std::string_view s) { return conv::quote(s); }

}  // namespace detail

// ---------------------------------------------------------------------------
// Record

void Record::AddAttrs(std::span<const Attr> attrs) {
    std::size_t i = 0;
    for (; i < attrs.size() && n_front_ < n_attrs_inline; i++) {
        if (attrs[i].Value.is_empty_group()) continue;
        front_[static_cast<std::size_t>(n_front_++)] = attrs[i];
    }
    for (; i < attrs.size(); i++) {
        if (!attrs[i].Value.is_empty_group()) back_.push_back(attrs[i]);
    }
}

void Record::add(Attr a) {
    if (a.Value.is_empty_group()) return;
    if (n_front_ < n_attrs_inline) front_[static_cast<std::size_t>(n_front_++)] = std::move(a);
    else back_.push_back(std::move(a));
}

std::optional<slog::Source> Record::Source() const {
    if (PC.line() == 0) return std::nullopt;
    return slog::Source{string(PC.function_name()), string(PC.file_name()), static_cast<int>(PC.line())};
}

}  // namespace gopp::slog
