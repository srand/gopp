// The log/slog package: structured logging, ported from Go 1.27.3.
//
//     slog::Info("hello", "count", 3);              // 2022/11/08 15:28:26 INFO hello count=3
//     auto logger = slog::New(slog::NewJSONHandler(ptr<io::Writer>(&os::Stderr)));
//     logger->Info("request", "method", "GET", slog::Int("status", 200),
//                  slog::Group("user", "id", 42));
//     // {"time":"...","level":"INFO","msg":"request","method":"GET","status":200,"user":{"id":42}}
//     auto db = logger->With("component", "db");
//     slog::SetDefault(logger);   // log::Print now goes through the JSON handler too
//
// The arguments after the message are keys and values, sorted out at
// compile time with Go's rules: a string is a key, and the argument after
// it its value; an Attr is itself; anything else is a value with the key
// "!BADKEY".
//
// AnyValue picks the Kind from the type as Go's does: strings, integers,
// floats, bool, time::Time and time::Duration have their own kinds, and a
// type with a `slog::Value LogValue() const` member is a LogValuer. Values
// of any other type are kept as KindAny. The text handler writes them with
// their MarshalText or AppendText member if they have one, a byte range as
// a quoted string, then with std::format if the type is formattable, and
// else as Go's %+v would ({Field:value ...}, by reflection). The JSON
// handler writes an error as its Error() string, and anything else with
// MarshalJSON or json::Marshal.
//
// The source of a record (HandlerOptions::AddSource) is where the logging
// call is, from std::source_location: Record::PC holds it. Writers are
// ptr<io::Writer>, and a context is a const ptr<context::Context>&, which
// may be nullptr. Unlike Go, a panic in a LogValue or MarshalText member
// isn't recovered.
#pragma once

// The module interfaces (modules/) import these instead.
#ifndef GOPP_MODULE
#include <gopp/context.hpp>
#include <gopp/encoding/json.hpp>
#include <gopp/errors.hpp>
#include <gopp/io.hpp>
#include <gopp/log.hpp>
#include <gopp/ptr.hpp>
#include <gopp/slice.hpp>
#include <gopp/string.hpp>
#include <gopp/text/template.hpp>
#include <gopp/time.hpp>

#include <any>
#include <array>
#include <atomic>
#include <chrono>
#include <concepts>
#include <format>
#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
#include <ostream>
#include <ranges>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeinfo>
#include <utility>
#include <variant>
#include <vector>
#endif

namespace gopp::slog {

// ---------------------------------------------------------------------------
// Levels

// slog.Level: the importance of a record. The higher, the more severe.
struct Level {
    int value = 0;

    constexpr Level() noexcept = default;
    constexpr explicit Level(int v) noexcept : value(v) {}
    constexpr explicit operator int() const noexcept { return value; }

    // "DEBUG", "INFO", "WARN" or "ERROR", with the offset from the nearest
    // one below: "INFO+2", "DEBUG-2".
    string String() const;
    std::pair<slice<byte>, error> MarshalJSON() const;
    error UnmarshalJSON(std::span<const byte> data);
    std::pair<slice<byte>, error> MarshalText() const;
    std::pair<slice<byte>, error> AppendText(slice<byte> b) const;
    // Parses the output of String, ignoring case: "warn", "Info+2".
    error UnmarshalText(std::span<const byte> data);

    friend constexpr bool operator==(Level, Level) noexcept = default;
    friend constexpr auto operator<=>(Level, Level) noexcept = default;
    friend constexpr Level operator+(Level a, int b) noexcept { return Level(a.value + b); }
    friend constexpr Level operator+(Level a, Level b) noexcept { return Level(a.value + b.value); }
    friend constexpr Level operator-(Level a, int b) noexcept { return Level(a.value - b); }
    friend constexpr Level operator-(Level a, Level b) noexcept { return Level(a.value - b.value); }
    friend constexpr Level operator-(Level a) noexcept { return Level(-a.value); }
    constexpr Level& operator+=(int b) noexcept {
        value += b;
        return *this;
    }
};

inline constexpr Level LevelDebug{-4};
inline constexpr Level LevelInfo{0};
inline constexpr Level LevelWarn{4};
inline constexpr Level LevelError{8};

// slog.Leveler: provides a Level. LevelVar is one.
struct Leveler : public virtual object {
    virtual slog::Level Level() const = 0;
};

// slog.LevelVar: a Level that can be changed while handlers use it.
class LevelVar : public Leveler {
public:
    slog::Level Level() const override { return slog::Level(static_cast<int>(v_.load(std::memory_order_relaxed))); }
    void Set(slog::Level l) noexcept { v_.store(l.value, std::memory_order_relaxed); }
    // "LevelVar(INFO)"
    string String() const;
    std::pair<slice<byte>, error> MarshalText() const;
    std::pair<slice<byte>, error> AppendText(slice<byte> b) const;
    error UnmarshalText(std::span<const byte> data);

private:
    std::atomic<int64> v_{0};
};

// HandlerOptions::Level: a Level, a Leveler such as a LevelVar, or nothing
// (LevelInfo). Go's field is a Leveler, which a Level is too.
class leveler {
public:
    leveler() noexcept = default;
    leveler(std::nullptr_t) noexcept {}
    leveler(slog::Level l) noexcept : l_(l), set_(true) {}
    template <typename L>
        requires std::derived_from<L, Leveler>
    leveler(ptr<L> p) noexcept : p_(std::move(p)), set_(static_cast<bool>(p_)) {}

    explicit operator bool() const noexcept { return set_; }
    slog::Level Level() const { return p_ ? p_->Level() : l_; }

private:
    ptr<Leveler> p_;
    slog::Level l_;
    bool set_ = false;
};

// ---------------------------------------------------------------------------
// Values

// slog.Kind: the kind of a Value.
struct Kind {
    int value = 0;
    constexpr operator int() const noexcept { return value; }
    string String() const;
};

inline constexpr Kind KindAny{0};
inline constexpr Kind KindBool{1};
inline constexpr Kind KindDuration{2};
inline constexpr Kind KindFloat64{3};
inline constexpr Kind KindInt64{4};
inline constexpr Kind KindString{5};
inline constexpr Kind KindTime{6};
inline constexpr Kind KindUint64{7};
inline constexpr Kind KindGroup{8};
inline constexpr Kind KindLogValuer{9};

struct Attr;
class Value;

namespace detail {

struct handle_state;

// What KindAny and KindLogValuer values hold: a T, and how to print it.
struct any_value : public virtual object {
    virtual const std::type_info& type() const noexcept = 0;
    virtual std::string_view type_name() const noexcept = 0;
    virtual const void* get() const noexcept = 0;
    virtual std::any to_any() const = 0;
    virtual bool equal(const any_value& o) const noexcept = 0;
    virtual bool log_valuer() const noexcept = 0;
    virtual Value log_value() const = 0;
    // Go's fmt %v, or %+v with plus.
    virtual void print(std::string& out, bool plus) const = 0;
    // The text handler's form: the string to quote as needed, or with raw
    // the text to write as it is.
    virtual error text(std::string& out, bool& raw) const = 0;
    // The JSON handler's: an error (and not a json.Marshaler) is written as
    // the string its Error() returns.
    virtual bool error_string(std::string& out) const = 0;
    virtual error json(std::string& out) const = 0;
};

}  // namespace detail

// slog.LogValuer, as Value::LogValuer returns it: a value whose LogValue
// gives the Value to log. A type is a LogValuer if it has a member
// `slog::Value LogValue() const`.
class LogValuer {
public:
    explicit LogValuer(ptr<const detail::any_value> p) noexcept : p_(std::move(p)) {}
    Value LogValue() const;

private:
    ptr<const detail::any_value> p_;
};

// slog.Value: a value of any type, with its Kind. The zero Value is Go's
// nil: KindAny, holding nothing.
class Value {
public:
    Value() noexcept = default;

    slog::Kind Kind() const noexcept;
    // The value as Go's Any returns it: int64, uint64, double, gopp::string,
    // bool, time::Duration, time::Time, std::vector<Attr> or Kind, or the
    // value of a KindAny or KindLogValuer (empty for the nil Value, or a
    // type that can't be copied).
    std::any Any() const;
    // The string of a KindString, or else the value formatted as Go's %v.
    string String() const;

    // The value of each kind; they panic if the Value has another kind.
    int64 Int64() const;
    uint64 Uint64() const;
    bool Bool() const;
    time::Duration Duration() const;
    double Float64() const;
    time::Time Time() const;
    slog::LogValuer LogValuer() const;
    std::span<const Attr> Group() const;

    // Whether v and w hold the same value: KindAny values are compared with
    // ==, or if they can't be, by identity.
    bool Equal(const Value& w) const;

    // Calls LogValue until the value isn't a LogValuer, at most 100 times;
    // then the result is an error value.
    Value Resolve() const;

    // The value of a KindAny or KindLogValuer value, if it's a T.
    template <typename T>
    const T* As() const noexcept;

private:
    friend Value StringValue(string value);
    friend Value Int64Value(int64 v) noexcept;
    friend Value Uint64Value(uint64 v) noexcept;
    friend Value Float64Value(double v) noexcept;
    friend Value BoolValue(bool v) noexcept;
    friend Value TimeValue(time::Time v);
    friend Value DurationValue(time::Duration v) noexcept;
    friend Value GroupValue(std::span<const Attr> as);
    friend struct Attr;
    friend class Record;
    friend struct detail::handle_state;
    template <typename T>
    friend Value AnyValue(T&& v);

    using group = std::shared_ptr<const std::vector<Attr>>;

    Value(slog::Kind k, uint64 num) noexcept : kind_(k), num_(num) {}
    static Value of_any(ptr<const detail::any_value> p);

    bool is_nil() const noexcept { return kind_ == KindAny && std::holds_alternative<std::monostate>(data_); }
    bool is_empty_group() const noexcept;
    void append(std::string& dst) const;

    slog::Kind kind_ = KindAny;
    uint64 num_ = 0;
    std::variant<std::monostate, string, time::Time, group, ptr<const detail::any_value>> data_;
};

// slog.Attr: a key and a value.
struct Attr {
    string Key;
    slog::Value Value;

    bool Equal(const Attr& b) const { return Key == b.Key && Value.Equal(b.Value); }
    // "key=value"
    string String() const;

    // Whether the Attr is the zero Attr, which handlers drop.
    bool is_empty() const noexcept { return Key.empty() && Value.is_nil(); }
};

// The Value constructors.
Value StringValue(string value);
inline Value IntValue(int64 v) noexcept;
Value Int64Value(int64 v) noexcept;
Value Uint64Value(uint64 v) noexcept;
Value Float64Value(double v) noexcept;
Value BoolValue(bool v) noexcept;
// A time Value; the monotonic reading is dropped.
Value TimeValue(time::Time v);
Value DurationValue(time::Duration v) noexcept;
// A group of attrs, without the empty groups among them.
Value GroupValue(std::span<const Attr> as);
inline Value GroupValue(std::initializer_list<Attr> as) { return GroupValue(std::span<const Attr>(as.begin(), as.size())); }
template <typename... A>
    requires(std::same_as<std::remove_cvref_t<A>, Attr> && ...)
Value GroupValue(const A&... as) {
    if constexpr (sizeof...(A) == 0) {
        return GroupValue(std::span<const Attr>());
    } else {
        const Attr a[] = {as...};
        return GroupValue(std::span<const Attr>(a));
    }
}

inline Value IntValue(int64 v) noexcept { return Int64Value(v); }

namespace detail {

// strconv.Quote, with Go's escapes.
std::string quote(std::string_view s);

template <typename T>
inline constexpr bool is_string_like_v =
    std::is_same_v<T, gopp::string> || std::is_same_v<T, std::string> || std::is_same_v<T, std::string_view> ||
    std::is_same_v<T, const char*> || std::is_same_v<T, char*> ||
    (std::is_array_v<T> && std::is_same_v<std::remove_cv_t<std::remove_extent_t<T>>, char>);

template <typename T>
inline constexpr bool is_attr_v = std::is_same_v<std::remove_cvref_t<T>, Attr>;

template <typename T>
inline constexpr bool is_duration_v = false;
template <typename R, typename P>
inline constexpr bool is_duration_v<std::chrono::duration<R, P>> = true;

template <typename T>
concept has_log_value = requires(const T& t) {
    { t.LogValue() } -> std::same_as<Value>;
};

template <typename T>
concept text_appender = requires(const T& t, slice<byte> b) {
    { t.AppendText(b) } -> std::same_as<std::pair<slice<byte>, gopp::error>>;
};

template <typename T>
concept text_marshaler = requires(const T& t) {
    { t.MarshalText() } -> std::same_as<std::pair<slice<byte>, gopp::error>>;
};

template <typename T>
concept pointer_like = std::is_pointer_v<T> || json::detail::is_ptr<T>::value;

template <typename T>
concept byte_range = std::ranges::contiguous_range<const T> &&
                     std::is_same_v<std::remove_cv_t<std::ranges::range_value_t<const T>>, byte>;

// The type a pointer-like T points to.
template <typename T>
using pointee_t = std::remove_cvref_t<decltype(*std::declval<const T&>())>;

// Methods with pointer receivers, in Go: a pointer to a LogValuer or
// TextMarshaler is one too.
template <typename T>
concept ptr_to_log_value = pointer_like<T> && has_log_value<pointee_t<T>>;
template <typename T>
concept ptr_to_text_appender = pointer_like<T> && text_appender<pointee_t<T>>;
template <typename T>
concept ptr_to_text_marshaler = pointer_like<T> && text_marshaler<pointee_t<T>>;

template <typename T>
inline constexpr bool is_error_v = [] {
    if constexpr (std::is_same_v<T, gopp::error>) return true;
    else if constexpr (pointer_like<T>) return std::derived_from<std::remove_cvref_t<decltype(*std::declval<T>())>, error_interface>;
    else return false;
}();

template <typename T>
struct any_holder final : any_value {
    T v;

    template <typename U>
    explicit any_holder(U&& u) : v(std::forward<U>(u)) {}

    const std::type_info& type() const noexcept override { return typeid(T); }
    std::string_view type_name() const noexcept override { return gopp::json::detail::type_name<T>(); }
    const void* get() const noexcept override { return &v; }

    std::any to_any() const override {
        if constexpr (std::is_copy_constructible_v<T>) return std::any(v);
        else return {};
    }

    bool equal(const any_value& o) const noexcept override {
        if (&o == this) return true;
        if constexpr (std::equality_comparable<T>) {
            if (auto* h = dynamic_cast<const any_holder*>(&o)) return static_cast<bool>(v == h->v);
        }
        return false;
    }

    bool log_valuer() const noexcept override { return has_log_value<T> || ptr_to_log_value<T>; }

    Value log_value() const override {
        if constexpr (has_log_value<T>) return v.LogValue();
        else if constexpr (ptr_to_log_value<T>) return v ? v->LogValue() : Value();
        else return Value();
    }

    void print(std::string& out, bool plus) const override {
        if constexpr (std::formattable<T, char> && !std::ranges::range<T>) {
            std::format_to(std::back_inserter(out), "{}", v);
        } else {
            gopp::text::tmpl::Value tv = gopp::text::tmpl::Value::Ref(v);
            out += gopp::text::tmpl::Sprintf(plus ? "%+v" : "%v", std::span<const gopp::text::tmpl::Value>(&tv, 1)).view();
        }
    }

    error text(std::string& out, bool& raw) const override {
        if constexpr (text_appender<T>) {
            auto [b, err] = v.AppendText(slice<byte>());
            if (err) return err;
            out = as_string_view(b);
        } else if constexpr (text_marshaler<T>) {
            auto [b, err] = v.MarshalText();
            if (err) return err;
            out = as_string_view(b);
        } else if constexpr (ptr_to_text_appender<T> || ptr_to_text_marshaler<T>) {
            // Go's handler recovers from the nil dereference and writes <nil>.
            if (!v) {
                out = "<nil>";
                return nullptr;
            }
            auto [b, err] = [&] {
                if constexpr (ptr_to_text_appender<T>) return v->AppendText(slice<byte>());
                else return v->MarshalText();
            }();
            if (err) return err;
            out = as_string_view(b);
        } else if constexpr (byte_range<T>) {
            raw = true;
            out = quote(std::string_view(reinterpret_cast<const char*>(std::ranges::data(v)), std::ranges::size(v)));
        } else {
            print(out, true);
        }
        return nullptr;
    }

    bool error_string(std::string& out) const override {
        if constexpr (is_error_v<T> && !gopp::json::detail::marshaler<T>) {
            if constexpr (std::is_same_v<T, gopp::error>) out = v.Error().view();
            else if (!v) out = "<nil>";
            else out = v->Error().view();
            return true;
        } else {
            return false;
        }
    }

    error json(std::string& out) const override {
        gopp::json::detail::encoder e;
        e.escape_html = false;
        if constexpr (std::is_pointer_v<T>) {
            if (!v) e.out = "null";
            else gopp::json::detail::encode(e, *v);
        } else {
            gopp::json::detail::encode(e, v);
        }
        if (e.err) return e.err;
        out += e.out;
        return nullptr;
    }
};

// Go's argsToAttr, at compile time: calls emit with each Attr of args.
template <typename F>
void args_to_attrs(F&&) {}

template <typename F, typename A, typename... Rest>
void args_to_attrs(F&& emit, A&& a, Rest&&... rest);

template <typename F, typename K, typename V, typename... Rest>
void args_kv(F&& emit, K&& k, V&& v, Rest&&... rest);

}  // namespace detail

// AnyValue: a Value of v's kind.
template <typename T>
Value AnyValue(T&& v) {
    using U = std::remove_cvref_t<T>;
    if constexpr (std::is_same_v<U, Value>) {
        return v;
    } else if constexpr (std::is_same_v<U, std::nullptr_t>) {
        return Value();
    } else if constexpr (std::is_same_v<U, const char*> || std::is_same_v<U, char*>) {
        return StringValue(v ? string(v) : string());
    } else if constexpr (detail::is_string_like_v<U>) {
        return StringValue(string(std::string_view(v)));
    } else if constexpr (std::is_same_v<U, bool>) {
        return BoolValue(v);
    } else if constexpr (std::is_integral_v<U> && std::is_signed_v<U>) {
        return Int64Value(static_cast<int64>(v));
    } else if constexpr (std::is_integral_v<U>) {
        return Uint64Value(static_cast<uint64>(v));
    } else if constexpr (std::is_floating_point_v<U>) {
        return Float64Value(static_cast<double>(v));
    } else if constexpr (std::is_same_v<U, time::Duration>) {
        return DurationValue(v);
    } else if constexpr (detail::is_duration_v<U>) {
        return DurationValue(time::Duration(v));
    } else if constexpr (std::is_same_v<U, time::Time>) {
        return TimeValue(v);
    } else if constexpr (std::is_same_v<U, std::vector<Attr>> || std::is_same_v<U, slice<Attr>> ||
                         std::is_same_v<U, std::span<const Attr>> || std::is_same_v<U, std::span<Attr>>) {
        return GroupValue(std::span<const Attr>(v.data(), v.size()));
    } else if constexpr (std::is_same_v<U, gopp::error>) {
        // A nil error is Go's nil interface: the nil Value.
        if (!v) return Value();
        return Value::of_any(make_ptr<detail::any_holder<U>>(std::forward<T>(v)));
    } else {
        return Value::of_any(make_ptr<detail::any_holder<U>>(std::forward<T>(v)));
    }
}

template <typename T>
const T* Value::As() const noexcept {
    auto* p = std::get_if<ptr<const detail::any_value>>(&data_);
    if (!p || (*p)->type() != typeid(T)) return nullptr;
    return static_cast<const T*>((*p)->get());
}

// The Attr constructors.
inline Attr String(string key, string value) { return Attr{std::move(key), StringValue(std::move(value))}; }
inline Attr Int64(string key, int64 value) { return Attr{std::move(key), Int64Value(value)}; }
inline Attr Int(string key, int64 value) { return Int64(std::move(key), value); }
inline Attr Uint64(string key, uint64 v) { return Attr{std::move(key), Uint64Value(v)}; }
inline Attr Float64(string key, double v) { return Attr{std::move(key), Float64Value(v)}; }
inline Attr Bool(string key, bool v) { return Attr{std::move(key), BoolValue(v)}; }
inline Attr Time(string key, time::Time v) { return Attr{std::move(key), TimeValue(v)}; }
inline Attr Duration(string key, time::Duration v) { return Attr{std::move(key), DurationValue(v)}; }
template <typename T>
Attr Any(string key, T&& value) {
    return Attr{std::move(key), AnyValue(std::forward<T>(value))};
}
// A group of the Attrs that args make, as a Logger's arguments do.
template <typename... A>
Attr Group(string key, A&&... args) {
    std::vector<Attr> attrs;
    attrs.reserve(sizeof...(A));
    detail::args_to_attrs([&](Attr a) { attrs.push_back(std::move(a)); }, std::forward<A>(args)...);
    return Attr{std::move(key), GroupValue(attrs)};
}
inline Attr GroupAttrs(string key, std::span<const Attr> attrs) { return Attr{std::move(key), GroupValue(attrs)}; }
template <typename... A>
    requires(std::same_as<std::remove_cvref_t<A>, Attr> && ...)
Attr GroupAttrs(string key, const A&... attrs) {
    return Attr{std::move(key), GroupValue(attrs...)};
}

namespace detail {

inline constexpr std::string_view bad_key = "!BADKEY";

template <typename F, typename A, typename... Rest>
void args_to_attrs(F&& emit, A&& a, Rest&&... rest) {
    if constexpr (is_string_like_v<std::remove_cvref_t<A>>) {
        if constexpr (sizeof...(Rest) == 0) {
            emit(slog::String(string::from_static(bad_key), string(std::string_view(a))));
        } else {
            args_kv(emit, std::forward<A>(a), std::forward<Rest>(rest)...);
        }
    } else if constexpr (is_attr_v<A>) {
        emit(Attr(std::forward<A>(a)));
        args_to_attrs(emit, std::forward<Rest>(rest)...);
    } else {
        emit(Any(string::from_static(bad_key), std::forward<A>(a)));
        args_to_attrs(emit, std::forward<Rest>(rest)...);
    }
}

template <typename F, typename K, typename V, typename... Rest>
void args_kv(F&& emit, K&& k, V&& v, Rest&&... rest) {
    emit(Any(string(std::string_view(k)), std::forward<V>(v)));
    args_to_attrs(emit, std::forward<Rest>(rest)...);
}

}  // namespace detail

// ---------------------------------------------------------------------------
// Records

// slog.Source: where a record was logged. Function is the function's
// signature, as std::source_location gives it.
struct Source {
    [[= json::tag("function")]] string Function;
    [[= json::tag("file")]] string File;
    [[= json::tag("line")]] int Line = 0;

    friend bool operator==(const Source&, const Source&) = default;
};

// slog.Record: a log event, as a Logger hands it to its Handler.
class Record {
public:
    time::Time Time;  // when the record was logged; the zero time is left out
    string Message;
    slog::Level Level;
    std::source_location PC;  // where Logger's method was called; line 0 if unknown

    Record() = default;
    Record(time::Time t, slog::Level level, string msg, std::source_location pc)
        : Time(t), Message(std::move(msg)), Level(level), PC(pc) {}

    // A copy that doesn't share state with r: Record copies never do in
    // C++, so it's a copy.
    Record Clone() const { return *this; }

    int NumAttrs() const noexcept { return n_front_ + static_cast<int>(back_.size()); }

    // Calls f with each Attr until it returns false.
    template <typename F>
    void Attrs(F&& f) const {
        for (int i = 0; i < n_front_; i++) {
            if (!f(front_[i])) return;
        }
        for (const Attr& a : back_) {
            if (!f(a)) return;
        }
    }

    // Appends attrs, but not the empty groups among them.
    void AddAttrs(std::span<const Attr> attrs);
    template <typename... A>
        requires(std::same_as<std::remove_cvref_t<A>, Attr> && ...)
    void AddAttrs(const A&... attrs) {
        if constexpr (sizeof...(A) > 0) {
            const Attr a[] = {attrs...};
            AddAttrs(std::span<const Attr>(a));
        }
    }

    // Appends the Attrs that args make, as a Logger's arguments do.
    template <typename... A>
    void Add(A&&... args) {
        detail::args_to_attrs([this](Attr a) { add(std::move(a)); }, std::forward<A>(args)...);
    }

    // Where the record was logged, if PC knows.
    std::optional<slog::Source> Source() const;

private:
    void add(Attr a);

    static constexpr int n_attrs_inline = 5;
    std::array<Attr, n_attrs_inline> front_;
    int n_front_ = 0;
    std::vector<Attr> back_;
};

// slog.NewRecord
inline Record NewRecord(time::Time t, Level level, string msg, std::source_location pc) {
    return Record(t, level, std::move(msg), pc);
}

// ---------------------------------------------------------------------------
// Handlers

// slog.Handler: handles the records of a Logger. See Go's documentation
// for the rules a handler follows.
struct Handler : public virtual object {
    // Whether records at level are handled. Loggers call it first.
    virtual bool Enabled(const ptr<context::Context>& ctx, slog::Level level) = 0;
    virtual error Handle(const ptr<context::Context>& ctx, const Record& r) = 0;
    // A handler that adds attrs to each record, within its groups.
    virtual ptr<Handler> WithAttrs(std::span<const Attr> attrs) = 0;
    // A handler whose attrs from now on go in group name.
    virtual ptr<Handler> WithGroup(std::string_view name) = 0;
};

// The keys of the built-in attributes, for ReplaceAttr.
inline constexpr std::string_view TimeKey = "time";
inline constexpr std::string_view LevelKey = "level";
inline constexpr std::string_view MessageKey = "msg";
inline constexpr std::string_view SourceKey = "source";

// slog.HandlerOptions, for TextHandler and JSONHandler.
struct HandlerOptions {
    // Adds the source of each record, as SourceKey.
    bool AddSource = false;
    // The minimum level to handle; LevelInfo if not set.
    leveler Level{};
    // Called with each non-group attribute (with the groups it is in, and
    // the built-in ones with none) to rewrite it; returning an Attr with an
    // empty key and the nil Value drops it.
    std::function<Attr(std::span<const string> groups, Attr a)> ReplaceAttr{};
};

namespace detail {
struct common_handler;
}

// slog.TextHandler: writes records as lines of key=value pairs:
// time=2022-11-08T15:28:26.000-05:00 level=INFO msg=hello count=3
class TextHandler final : public Handler {
public:
    explicit TextHandler(std::shared_ptr<const detail::common_handler> h) noexcept : h_(std::move(h)) {}
    bool Enabled(const ptr<context::Context>& ctx, slog::Level level) override;
    error Handle(const ptr<context::Context>& ctx, const Record& r) override;
    ptr<Handler> WithAttrs(std::span<const Attr> attrs) override;
    ptr<Handler> WithGroup(std::string_view name) override;

private:
    std::shared_ptr<const detail::common_handler> h_;
};

// slog.JSONHandler: writes records as lines of JSON objects:
// {"time":"2022-11-08T15:28:26.000000000-05:00","level":"INFO","msg":"hello","count":3}
class JSONHandler final : public Handler {
public:
    explicit JSONHandler(std::shared_ptr<const detail::common_handler> h) noexcept : h_(std::move(h)) {}
    bool Enabled(const ptr<context::Context>& ctx, slog::Level level) override;
    error Handle(const ptr<context::Context>& ctx, const Record& r) override;
    ptr<Handler> WithAttrs(std::span<const Attr> attrs) override;
    ptr<Handler> WithGroup(std::string_view name) override;

private:
    std::shared_ptr<const detail::common_handler> h_;
};

ptr<TextHandler> NewTextHandler(ptr<io::Writer> w, const HandlerOptions& opts = {});
inline ptr<TextHandler> NewTextHandler(ptr<io::Writer> w, std::nullptr_t) { return NewTextHandler(std::move(w)); }
ptr<JSONHandler> NewJSONHandler(ptr<io::Writer> w, const HandlerOptions& opts = {});
inline ptr<JSONHandler> NewJSONHandler(ptr<io::Writer> w, std::nullptr_t) { return NewJSONHandler(std::move(w)); }

// slog.MultiHandler: hands each record to all of its handlers that are
// enabled for it.
class MultiHandler final : public Handler {
public:
    explicit MultiHandler(std::vector<ptr<Handler>> hs) noexcept : multi_(std::move(hs)) {}
    bool Enabled(const ptr<context::Context>& ctx, slog::Level level) override;
    // The errors of the handlers, joined.
    error Handle(const ptr<context::Context>& ctx, const Record& r) override;
    ptr<Handler> WithAttrs(std::span<const Attr> attrs) override;
    ptr<Handler> WithGroup(std::string_view name) override;

private:
    std::vector<ptr<Handler>> multi_;
};

ptr<MultiHandler> NewMultiHandler(std::span<const ptr<Handler>> handlers);
template <typename... H>
ptr<MultiHandler> NewMultiHandler(H&&... handlers) {
    std::vector<ptr<Handler>> hs{ptr<Handler>(std::forward<H>(handlers))...};
    return NewMultiHandler(std::span<const ptr<Handler>>(hs));
}

// slog.DiscardHandler: drops every record (Enabled is false).
extern const ptr<Handler> DiscardHandler;

// ---------------------------------------------------------------------------
// Loggers

// The message of a logging call, and where the call is.
struct message {
    template <typename S>
        requires std::convertible_to<const S&, std::string_view>
    message(const S& s, std::source_location l = std::source_location::current()) : text(s), loc(l) {}

    std::string_view text;
    std::source_location loc;
};

// slog.Logger: makes records of its methods' arguments and hands them to
// its Handler.
class Logger : public object {
public:
    explicit Logger(ptr<slog::Handler> h);

    ptr<slog::Handler> Handler() const noexcept { return handler_; }

    // A Logger that adds the Attrs args make to each record.
    template <typename... A>
    ptr<Logger> With(A&&... args) const {
        if constexpr (sizeof...(A) == 0) {
            return self();
        } else {
            std::vector<Attr> attrs;
            attrs.reserve(sizeof...(A));
            detail::args_to_attrs([&](Attr a) { attrs.push_back(std::move(a)); }, std::forward<A>(args)...);
            return make_ptr<Logger>(handler_->WithAttrs(attrs));
        }
    }
    // A Logger whose attributes from now on go in group name.
    ptr<Logger> WithGroup(std::string_view name) const;

    bool Enabled(const ptr<context::Context>& ctx, slog::Level level) const;

    template <typename... A>
    void Log(const ptr<context::Context>& ctx, slog::Level level, message msg, A&&... args) const {
        write(ctx, level, msg, std::forward<A>(args)...);
    }
    void LogAttrs(const ptr<context::Context>& ctx, slog::Level level, message msg, std::span<const Attr> attrs) const;
    template <typename... A>
        requires(std::same_as<std::remove_cvref_t<A>, Attr> && ...)
    void LogAttrs(const ptr<context::Context>& ctx, slog::Level level, message msg, const A&... attrs) const {
        if constexpr (sizeof...(A) == 0) {
            LogAttrs(ctx, level, msg, std::span<const Attr>());
        } else {
            const Attr a[] = {attrs...};
            LogAttrs(ctx, level, msg, std::span<const Attr>(a));
        }
    }

    template <typename... A>
    void Debug(message msg, A&&... args) const {
        write(nullptr, LevelDebug, msg, std::forward<A>(args)...);
    }
    template <typename... A>
    void DebugContext(const ptr<context::Context>& ctx, message msg, A&&... args) const {
        write(ctx, LevelDebug, msg, std::forward<A>(args)...);
    }
    template <typename... A>
    void Info(message msg, A&&... args) const {
        write(nullptr, LevelInfo, msg, std::forward<A>(args)...);
    }
    template <typename... A>
    void InfoContext(const ptr<context::Context>& ctx, message msg, A&&... args) const {
        write(ctx, LevelInfo, msg, std::forward<A>(args)...);
    }
    template <typename... A>
    void Warn(message msg, A&&... args) const {
        write(nullptr, LevelWarn, msg, std::forward<A>(args)...);
    }
    template <typename... A>
    void WarnContext(const ptr<context::Context>& ctx, message msg, A&&... args) const {
        write(ctx, LevelWarn, msg, std::forward<A>(args)...);
    }
    template <typename... A>
    void Error(message msg, A&&... args) const {
        write(nullptr, LevelError, msg, std::forward<A>(args)...);
    }
    template <typename... A>
    void ErrorContext(const ptr<context::Context>& ctx, message msg, A&&... args) const {
        write(ctx, LevelError, msg, std::forward<A>(args)...);
    }

private:
    ptr<Logger> self() const noexcept { return ptr<Logger>(const_cast<Logger*>(this)); }

    // Go's Logger.log.
    template <typename... A>
    void write(const ptr<context::Context>& ctx, slog::Level level, const message& msg, A&&... args) const {
        const ptr<context::Context>& c = ctx ? ctx : background();
        if (!handler_->Enabled(c, level)) return;
        Record r(time::Now(), level, string(msg.text), msg.loc);
        r.Add(std::forward<A>(args)...);
        (void)handler_->Handle(c, r);
    }
    static const ptr<context::Context>& background();

    ptr<slog::Handler> handler_;
};

// slog.New: a Logger with handler h, which must not be nil.
ptr<Logger> New(ptr<Handler> h);

// slog.Default: the default Logger, which the package functions use. Its
// handler writes through log's standard logger, as "LEVEL msg k=v", for
// the levels from SetLogLoggerLevel's (LevelInfo) up.
ptr<Logger> Default();
// slog.SetDefault: makes l the default Logger. Unless l's handler is the
// default one, log's standard logger then writes through it too, at the
// level SetLogLoggerLevel sets, without a header of its own.
void SetDefault(ptr<Logger> l);
// slog.SetLogLoggerLevel: the level of the bridge between log and slog,
// either way; returns the old one.
Level SetLogLoggerLevel(Level level);

// slog.NewLogLogger: a log::Logger whose lines go to h as records at level.
ptr<log::Logger> NewLogLogger(ptr<Handler> h, Level level);

// The default Logger's methods.
template <typename... A>
ptr<Logger> With(A&&... args) {
    return Default()->With(std::forward<A>(args)...);
}
template <typename... A>
void Debug(message msg, A&&... args) {
    Default()->Debug(msg, std::forward<A>(args)...);
}
template <typename... A>
void DebugContext(const ptr<context::Context>& ctx, message msg, A&&... args) {
    Default()->DebugContext(ctx, msg, std::forward<A>(args)...);
}
template <typename... A>
void Info(message msg, A&&... args) {
    Default()->Info(msg, std::forward<A>(args)...);
}
template <typename... A>
void InfoContext(const ptr<context::Context>& ctx, message msg, A&&... args) {
    Default()->InfoContext(ctx, msg, std::forward<A>(args)...);
}
template <typename... A>
void Warn(message msg, A&&... args) {
    Default()->Warn(msg, std::forward<A>(args)...);
}
template <typename... A>
void WarnContext(const ptr<context::Context>& ctx, message msg, A&&... args) {
    Default()->WarnContext(ctx, msg, std::forward<A>(args)...);
}
template <typename... A>
void Error(message msg, A&&... args) {
    Default()->Error(msg, std::forward<A>(args)...);
}
template <typename... A>
void ErrorContext(const ptr<context::Context>& ctx, message msg, A&&... args) {
    Default()->ErrorContext(ctx, msg, std::forward<A>(args)...);
}
template <typename... A>
void Log(const ptr<context::Context>& ctx, Level level, message msg, A&&... args) {
    Default()->Log(ctx, level, msg, std::forward<A>(args)...);
}
inline void LogAttrs(const ptr<context::Context>& ctx, Level level, message msg, std::span<const Attr> attrs) {
    Default()->LogAttrs(ctx, level, msg, attrs);
}
template <typename... A>
    requires(std::same_as<std::remove_cvref_t<A>, Attr> && ...)
void LogAttrs(const ptr<context::Context>& ctx, Level level, message msg, const A&... attrs) {
    Default()->LogAttrs(ctx, level, msg, attrs...);
}

inline std::ostream& operator<<(std::ostream& os, Level l) { return os << l.String(); }
inline std::ostream& operator<<(std::ostream& os, Kind k) { return os << k.String(); }
inline std::ostream& operator<<(std::ostream& os, const Value& v) { return os << v.String(); }
inline std::ostream& operator<<(std::ostream& os, const Attr& a) { return os << a.String(); }

}  // namespace gopp::slog

template <>
struct std::formatter<gopp::slog::Level> : std::formatter<std::string_view> {
    auto format(gopp::slog::Level l, auto& ctx) const {
        return std::formatter<std::string_view>::format(l.String().view(), ctx);
    }
};

template <>
struct std::formatter<gopp::slog::Kind> : std::formatter<std::string_view> {
    auto format(gopp::slog::Kind k, auto& ctx) const {
        return std::formatter<std::string_view>::format(k.String().view(), ctx);
    }
};

template <>
struct std::formatter<gopp::slog::Value> : std::formatter<std::string_view> {
    auto format(const gopp::slog::Value& v, auto& ctx) const {
        return std::formatter<std::string_view>::format(v.String().view(), ctx);
    }
};

template <>
struct std::formatter<gopp::slog::Attr> : std::formatter<std::string_view> {
    auto format(const gopp::slog::Attr& a, auto& ctx) const {
        return std::formatter<std::string_view>::format(a.String().view(), ctx);
    }
};
