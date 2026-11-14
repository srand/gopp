// The text/template package: data-driven templates, ported from Go 1.27.3.
//
//     struct Inventory {
//         string Material;
//         int Count;
//     };
//     auto t = tmpl::Must(tmpl::New("test")->Parse("{{.Count}} items are made of {{.Material}}"));
//     t->Execute(w, Inventory{"wool", 17});    // 17 items are made of wool
//
// `template` is a C++ keyword, so the package is gopp::text::tmpl
// (alias it: namespace tmpl = gopp::text::tmpl;).
//
// Data model. Go's executor works on reflect.Value; go++'s works on
// tmpl::Value, a view of C++ data that C++26 reflection describes:
//
//   - bool, integers, floating point and std::complex<double> are Go's
//     bool, int and uint kinds, float and complex. Integer types of any
//     size convert to one another when passed to functions (Go would
//     insist on the exact type).
//   - gopp::string, std::string, std::string_view and const char* are
//     strings.
//   - Classes are structs: their public non-static data members are fields
//     ({{.Name}}), and their public const non-static member functions are
//     methods ({{.Method arg1 arg2}}) taking any of these types and
//     returning a value, or std::pair<value, gopp::error> (a non-nil error
//     stops execution). A last parameter of type tmpl::Variadic<T>
//     takes any number of T, as Go's ...T. A String() const method makes
//     the type a fmt.Stringer; an Error() const one an error.
//   - std::vector, gopp::slice, std::array and std::span are slices;
//     std::map and std::unordered_map are maps (ranged over in sorted key
//     order, as Go does).
//   - T*, ptr<T>, std::shared_ptr<T>, std::unique_ptr<T> and
//     std::optional<T> are pointers: fields are reached through them, and
//     null is nil.
//   - gopp::error is Go's error; json::Value and tmpl::Value are
//     Go's any.
//   - Callables (std::function and function pointers) are funcs, for
//     {{call .F args}}; FuncMap entries are any callable with a
//     non-template call operator.
//
// Data passed to Execute is referenced, not copied: it must outlive the
// call. Values that functions and methods return are owned by the Value.
//
// Differences from Go: no channels or iterator functions to range over; no
// methods on nil receivers; int-like arguments convert between sizes; and
// {{template}} recursion stops at 200 levels (Go's limit is 100000), which
// fits a 256 KiB fiber stack with room to spare. The set of associated
// templates (New, Parse, Lookup) lives as long as any of them, and the
// templates refer to each other, so it is never freed: like Go's, parse
// templates once, not per request.
#pragma once

// The module interfaces (modules/) import these instead.
#ifndef GOPP_MODULE
#include <gopp/encoding/json.hpp>
#include <gopp/errors.hpp>
#include <gopp/io.hpp>
#include <gopp/ptr.hpp>
#include <gopp/slice.hpp>
#include <gopp/string.hpp>
#include <gopp/sync.hpp>
#include <gopp/text/template/parse.hpp>

#include <array>
#include <complex>
#include <concepts>
#include <cstdint>
#include <functional>
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
#include <vector>
#endif

namespace gopp::text::tmpl {

// reflect.Kind, for the kinds the executor tells apart.
enum class Kind { Invalid, Bool, Int, Uint, Float, Complex, String, Slice, Array, Map, Struct, Pointer, Func, Interface };

class Value;
class Func;

// The last parameter of a variadic method or function: Go's ...T.
template <typename T>
struct Variadic : std::vector<T> {
    using std::vector<T>::vector;
};

namespace detail {

struct type_ops;
using ops_fn = const type_ops* (*)();
using owner_t = std::shared_ptr<const void>;

struct field_info {
    std::string_view name;
    Value (*get)(const void* obj, const owner_t& owner);
};

struct method_info {
    std::string_view name;
    std::span<const ops_fn> in;  // parameter types; for variadics the last is the element type
    bool variadic;
    std::pair<Value, gopp::error> (*call)(const void* obj, const owner_t& owner, std::span<const Value> args);
};

// What the executor knows about a C++ type: one per type, made by
// reflection (Go's reflect.Type).
struct type_ops {
    std::string_view name;  // Go-style: int, string, []int, map[string]int, *T, T
    Kind kind;
    // Struct: fields; any type: methods.
    std::span<const field_info> fields;
    std::span<const method_info> methods;
    // Slice, Array, Map, Pointer: the element (value) type; Map: the key.
    ops_fn elem = nullptr;
    ops_fn key = nullptr;
    // Slice, Array, Map: the length; String values carry their own.
    std::size_t (*len)(const void* obj) = nullptr;
    // Slice, Array: element i.
    Value (*index)(const void* obj, const owner_t& owner, std::size_t i) = nullptr;
    // Map: the value for key (invalid if absent), and the entries in sorted
    // key order.
    Value (*map_index)(const void* obj, const owner_t& owner, const Value& key) = nullptr;
    void (*entries)(const void* obj, const owner_t& owner, std::vector<std::pair<Value, Value>>& out) = nullptr;
    // Pointer, Interface: whether nil, and what it points to (the dynamic
    // value of an interface).
    bool (*isNil)(const void* obj) = nullptr;
    Value (*deref)(const void* obj, const owner_t& owner) = nullptr;
    // Pointer: the address of what it points to.
    const void* (*raw)(const void* obj) = nullptr;
    // Func: the callable.
    const Func* (*func)(const void* obj) = nullptr;
    // The zero value, for missingkey=zero and nil arguments.
    Value (*zero)() = nullptr;
    // Whether == applies (eq on non-basic values).
    bool (*equal)(const void* a, const void* b) = nullptr;
};

template <typename T>
const type_ops* opsOf();

}  // namespace detail

// A value the executor works on: Go's reflect.Value for go++'s data model.
class Value {
public:
    // The invalid value: Go's zero reflect.Value, and untyped nil.
    Value() noexcept = default;

    // A value holding a copy of v (scalars are always copied).
    template <typename T>
        requires(!std::is_same_v<std::remove_cvref_t<T>, Value>)
    static Value Of(T&& v);
    // A value referring to v, which must outlive it; owner keeps it alive
    // if set.
    template <typename T>
    static Value Ref(const T& v, detail::owner_t owner = nullptr);

    bool IsValid() const noexcept { return t_ != nullptr; }
    tmpl::Kind Kind() const noexcept;
    const detail::type_ops* Type() const noexcept { return t_; }
    // The type's name, as Go would print it ("<nil>" if invalid).
    std::string_view TypeName() const noexcept;

    bool Bool() const noexcept { return s_.b; }
    int64 Int() const noexcept { return s_.i; }
    uint64 Uint() const noexcept { return s_.u; }
    double Float() const noexcept { return s_.f; }
    std::complex<double> Complex() const noexcept { return {s_.f, im_}; }
    // The string of a String value.
    const string& Str() const noexcept { return str_; }

    // Slice, Array, Map, String: the length.
    std::size_t Len() const;
    // Slice, Array: element i; String: byte i (a uint8).
    Value Index(std::size_t i) const;
    // Map: the value for key, invalid if absent.
    Value MapIndex(const Value& key) const;
    // Map: the entries in sorted key order.
    std::vector<std::pair<Value, Value>> MapEntries() const;
    // Struct: the field named name, if any.
    std::optional<Value> FieldByName(std::string_view name) const;
    // A method of the value's type, if any.
    const detail::method_info* MethodByName(std::string_view name) const;
    // Pointer, Interface: whether nil; other kinds: false.
    bool IsNil() const;
    // Pointer, Interface: what it points to.
    Value Elem() const;
    // Func: the callable.
    const Func* AsFunc() const;
    // The C++ object, if the value refers to one of type T.
    template <typename T>
    const T* As() const noexcept;
    // The object pointer and its owner, for the reflection glue.
    const void* ptr() const noexcept { return p_; }
    const detail::owner_t& owner() const noexcept { return owner_; }

    // Internals: scalar construction.
    static Value scalar(const detail::type_ops* t, bool b);
    static Value scalarInt(const detail::type_ops* t, int64 i);
    static Value scalarUint(const detail::type_ops* t, uint64 u);
    static Value scalarFloat(const detail::type_ops* t, double f, double im = 0);
    static Value scalarString(const detail::type_ops* t, string s);
    static Value object(const detail::type_ops* t, const void* p, detail::owner_t owner);

private:
    const detail::type_ops* t_ = nullptr;
    union {
        bool b;
        int64 i;
        uint64 u;
        double f;
    } s_{.i = 0};
    double im_ = 0;
    string str_;
    const void* p_ = nullptr;
    detail::owner_t owner_;
};

// A function callable from templates: from any callable whose call
// operator isn't a template, with arguments and a result of the data
// model's types; the result may be std::pair<T, gopp::error>.
class Func {
public:
    Func() = default;
    template <typename F>
        requires(!std::is_same_v<std::remove_cvref_t<F>, Func>)
    Func(F f);  // NOLINT: implicit, for FuncMap{{"name", lambda}}

    // A function on raw values, as the builtins are: n_in parameters
    // (the last variadic if variadic), any types.
    static Func Raw(int n_in, bool variadic,
                    std::function<std::pair<Value, gopp::error>(std::span<const Value>)> fn);
    // The same with parameter types (nullptr for any).
    static Func Raw(std::vector<detail::ops_fn> in, bool variadic,
                    std::function<std::pair<Value, gopp::error>(std::span<const Value>)> fn);

    int NumIn() const noexcept { return static_cast<int>(in_.size()); }
    bool IsVariadic() const noexcept { return variadic_; }
    // Parameter i's type; nullptr for any (Value). For a variadic
    // function, the last is the element type.
    const detail::type_ops* In(int i) const { return in_[static_cast<std::size_t>(i)] ? in_[static_cast<std::size_t>(i)]() : nullptr; }
    explicit operator bool() const noexcept { return static_cast<bool>(fn_); }

    // Calls with arguments of the parameter types (the executor checks).
    std::pair<Value, gopp::error> Call(std::span<const Value> args) const { return fn_(args); }

private:
    std::vector<detail::ops_fn> in_;
    bool variadic_ = false;
    std::function<std::pair<Value, gopp::error>(std::span<const Value>)> fn_;
};

// template.FuncMap: names to functions.
using FuncMap = std::map<std::string, Func, std::less<>>;

// template.ExecError: an error from executing a template (not from writing
// its output).
struct ExecError : errorInterface {
    string Name;  // the template's name
    gopp::error Err;
    ExecError(string name, gopp::error err) : Name(std::move(name)), Err(std::move(err)) {}
    string Error() const override { return Err.Error(); }
    const errorInterface* Unwrap() const noexcept override { return Err.get(); }
};

class Template;

namespace detail {
struct common;
struct template_access;
}

// template.Template: a parsed template, and the templates associated with
// it (defined in it, or added with New, Parse or AddParseTree).
//
// Associated templates form a set that refers to its templates by name,
// and they refer to the set. Go's garbage collector frees such a set when
// none of its templates is referenced any more; here the templates of a set
// share one reference count, the set's, so the set and all its templates go
// with the last ptr<Template> to any of them. Templates must therefore live
// in ptr<Template> (as New returns them), not on the stack.
class Template final : public object {
public:
    // The parse tree; nil until parsed.
    ptr<parse::Tree> Tree;

    // template.New
    explicit Template(string name);
    ~Template() override;

    // Reference counting (intrusive_ptr calls these): a template in a set
    // counts on the set.
    void addRef() const noexcept;
    void releaseRef() const noexcept;

    string Name() const { return name_; }
    // A new template associated with this one, with its delimiters.
    ptr<Template> New(string name);
    // Parses text as the template's body; {{define}}d templates become
    // associated templates.
    std::pair<ptr<Template>, gopp::error> Parse(std::string_view text);
    // Parses the named files: the first defines this template (or the one
    // named like it), the others are associated, by base name.
    std::pair<ptr<Template>, gopp::error> ParseFiles(std::initializer_list<std::string_view> filenames);
    std::pair<ptr<Template>, gopp::error> ParseFiles(std::span<const string> filenames);
    // Parses the files matching a glob(3) pattern.
    std::pair<ptr<Template>, gopp::error> ParseGlob(std::string_view pattern);

    // Applies the template to data, writing to w. Data is referenced.
    template <typename T>
    gopp::error Execute(io::Writer& w, const T& data) {
        return Execute(w, Value::Ref(data));
    }
    gopp::error Execute(io::Writer& w, const Value& data);
    gopp::error Execute(io::Writer& w, std::nullptr_t) { return Execute(w, Value()); }
    template <typename T>
    gopp::error ExecuteTemplate(io::Writer& w, std::string_view name, const T& data) {
        return ExecuteTemplate(w, name, Value::Ref(data));
    }
    gopp::error ExecuteTemplate(io::Writer& w, std::string_view name, const Value& data);

    // The associated template named name, or nil.
    ptr<Template> Lookup(std::string_view name) const;
    // All associated templates, including this one.
    std::vector<ptr<Template>> Templates() const;
    // "; defined templates are: "a", "b"", for error messages.
    string DefinedTemplates() const;
    // A copy of the template and its associated templates, for adding to.
    std::pair<ptr<Template>, gopp::error> Clone() const;
    // Associates tree with name: this template if the name matches.
    std::pair<ptr<Template>, gopp::error> AddParseTree(std::string_view name, ptr<parse::Tree> tree);

    // Sets the action delimiters ("" for the default {{ and }}).
    Template* Delims(std::string_view left, std::string_view right);
    // Adds functions; must happen before parsing templates that use them.
    // Panics if a name isn't an identifier.
    Template* Funcs(const FuncMap& funcs);
    // Sets options: "missingkey=default", "missingkey=invalid",
    // "missingkey=zero" or "missingkey=error".
    Template* Option(std::initializer_list<std::string_view> opts);
    Template* Option(std::string_view opt) { return Option({opt}); }

private:
    friend struct detail::template_access;
    friend std::pair<ptr<Template>, gopp::error> ParseFiles(std::initializer_list<std::string_view> filenames);
    friend ptr<Template> New(string name);
    void init();
    // Makes this template, so far on its own, a member of set c: its
    // references move to the set, which frees it.
    void join(detail::common* c);
    ptr<Template> copy(detail::common* c) const;
    bool associate(const ptr<Template>& nt, const ptr<parse::Tree>& tree);
    detail::common* common_ = nullptr;  // the set; owns this template
    string left_delim_, right_delim_;
    string name_;
};

// template.New
ptr<Template> New(string name);

// template.Must: panics if err is non-nil.
ptr<Template> Must(std::pair<ptr<Template>, gopp::error> r);

// template.ParseFiles, ParseGlob
std::pair<ptr<Template>, gopp::error> ParseFiles(std::initializer_list<std::string_view> filenames);
std::pair<ptr<Template>, gopp::error> ParseGlob(std::string_view pattern);

// template.IsTrue: Go's truth of a value (non-zero, non-empty), and
// whether it has one.
std::pair<bool, bool> IsTrue(const Value& v);
template <typename T>
std::pair<bool, bool> IsTrue(const T& v) {
    return IsTrue(Value::Ref(v));
}

// The escapers of the html, js and urlquery functions.
string HTMLEscapeString(std::string_view s);
void HTMLEscape(io::Writer& w, std::span<const byte> b);
string JSEscapeString(std::string_view s);
void JSEscape(io::Writer& w, std::span<const byte> b);
string HTMLEscaper(std::span<const Value> args);
string JSEscaper(std::span<const Value> args);
string URLQueryEscaper(std::span<const Value> args);

// fmt.Sprint, Sprintln and Sprintf (with Go's verbs) on template values,
// as the print, println and printf functions use them.
string Sprint(std::span<const Value> args);
string Sprintln(std::span<const Value> args);
string Sprintf(std::string_view format, std::span<const Value> args);

// ---------------------------------------------------------------------------
// The reflection glue: type_ops for C++ types.

namespace detail {

template <typename T>
struct is_vector_like : std::false_type {};
template <typename T, typename A>
struct is_vector_like<std::vector<T, A>> : std::true_type {};
template <typename T>
struct is_vector_like<Variadic<T>> : std::true_type {};
template <typename T>
struct is_vector_like<gopp::slice<T>> : std::true_type {};
template <typename T, std::size_t N>
struct is_vector_like<std::span<T, N>> : std::true_type {};

template <typename T>
struct is_std_array : std::false_type {};
template <typename T, std::size_t N>
struct is_std_array<std::array<T, N>> : std::true_type {};

template <typename T>
struct is_map_like : std::false_type {};
template <typename K, typename V, typename C, typename A>
struct is_map_like<std::map<K, V, C, A>> : std::true_type {};
template <typename K, typename V, typename H, typename E, typename A>
struct is_map_like<std::unordered_map<K, V, H, E, A>> : std::true_type {};

template <typename T>
struct pointer_traits {
    static constexpr bool value = false;
};
template <typename T>
struct pointer_traits<T*> {
    static constexpr bool value = true;
    using elem = std::remove_cv_t<T>;
    static const T* get(T* const& p) { return p; }
};
template <typename T>
struct pointer_traits<gopp::intrusive_ptr<T>> {
    static constexpr bool value = true;
    using elem = std::remove_cv_t<T>;
    static const T* get(const gopp::intrusive_ptr<T>& p) { return p.get(); }
};
template <typename T>
struct pointer_traits<std::shared_ptr<T>> {
    static constexpr bool value = true;
    using elem = std::remove_cv_t<T>;
    static const T* get(const std::shared_ptr<T>& p) { return p.get(); }
};
template <typename T, typename D>
struct pointer_traits<std::unique_ptr<T, D>> {
    static constexpr bool value = true;
    using elem = std::remove_cv_t<T>;
    static const T* get(const std::unique_ptr<T, D>& p) { return p.get(); }
};
template <typename T>
struct pointer_traits<std::optional<T>> {
    static constexpr bool value = true;
    using elem = T;
    static const T* get(const std::optional<T>& p) { return p ? &*p : nullptr; }
};

template <typename T>
inline constexpr bool is_string_like_v = std::is_same_v<T, gopp::string> || std::is_same_v<T, std::string> ||
                                         std::is_same_v<T, std::string_view> || std::is_same_v<T, const char*> ||
                                         std::is_same_v<T, char*>;

template <typename T>
inline constexpr bool is_int_v = std::is_integral_v<T> && !std::is_same_v<T, bool> && std::is_signed_v<T>;
template <typename T>
inline constexpr bool is_uint_v = std::is_integral_v<T> && !std::is_same_v<T, bool> && std::is_unsigned_v<T>;

// Function signatures of callables.
template <typename F>
struct fn_traits : fn_traits<decltype(&F::operator())> {};
template <typename R, typename... A>
struct fn_traits<R (*)(A...)> {
    using result = R;
    using args = std::tuple<A...>;
};
template <typename R, typename... A>
struct fn_traits<R(A...)> : fn_traits<R (*)(A...)> {};
template <typename C, typename R, typename... A>
struct fn_traits<R (C::*)(A...) const> : fn_traits<R (*)(A...)> {};
template <typename C, typename R, typename... A>
struct fn_traits<R (C::*)(A...)> : fn_traits<R (*)(A...)> {};
template <typename C, typename R, typename... A>
struct fn_traits<R (C::*)(A...) const noexcept> : fn_traits<R (*)(A...)> {};
template <typename C, typename R, typename... A>
struct fn_traits<R (C::*)(A...) noexcept> : fn_traits<R (*)(A...)> {};
template <typename R, typename... A>
struct fn_traits<R (*)(A...) noexcept> : fn_traits<R (*)(A...)> {};

template <typename T>
struct is_std_function : std::false_type {};
template <typename S>
struct is_std_function<std::function<S>> : std::true_type {};

template <typename T>
struct is_variadic_param : std::false_type {};
template <typename T>
struct is_variadic_param<Variadic<T>> : std::true_type {
    using elem = T;
};

template <typename T>
struct is_result_pair : std::false_type {};
template <typename T>
struct is_result_pair<std::pair<T, gopp::error>> : std::true_type {
    using elem = T;
};

// Names.
std::string_view intern(std::string s);

template <typename T>
std::string_view typeName();

// The value a C++ object becomes, referring to it with owner (or
// copying scalars).
template <typename T>
Value toValue(const T& v, const owner_t& owner);

// Conversions from values to parameter types; the executor has checked the
// kinds. An object parameter takes a value of exactly its type.
template <typename P>
struct from_value {
    static P get(const Value& v);
};

template <typename T>
inline constexpr bool is_object_v = !std::is_arithmetic_v<T> && !is_string_like_v<T> && !std::is_same_v<T, Value>;

template <typename R>
std::pair<Value, gopp::error> wrapResult(R&& r) {
    using U = std::remove_cvref_t<R>;
    if constexpr (is_result_pair<U>::value) {
        if (r.second) return {Value(), r.second};
        if constexpr (std::is_same_v<typename is_result_pair<U>::elem, Value>) return {std::move(r.first), nullptr};
        else return {Value::Of(std::move(r.first)), nullptr};
    } else if constexpr (std::is_same_v<U, Value>) {
        return {std::forward<R>(r), nullptr};
    } else {
        return {Value::Of(std::forward<R>(r)), nullptr};
    }
}

template <typename Tuple, std::size_t... I>
auto argsFrom(std::span<const Value> args, std::index_sequence<I...>) {
    constexpr std::size_t n = sizeof...(I);
    [[maybe_unused]] auto one = [&]<std::size_t K>() {  // unused without arguments
        using P = std::remove_cvref_t<std::tuple_element_t<K, Tuple>>;
        if constexpr (is_variadic_param<P>::value && K == n - 1) {
            P rest;
            for (std::size_t j = K; j < args.size(); j++) rest.push_back(from_value<typename is_variadic_param<P>::elem>::get(args[j]));
            return rest;
        } else {
            return from_value<P>::get(args[K]);
        }
    };
    return std::tuple<std::remove_cvref_t<std::tuple_element_t<I, Tuple>>...>(one.template operator()<I>()...);
}

template <typename Tuple, std::size_t... I>
std::array<ops_fn, sizeof...(I)> paramsOf(std::index_sequence<I...>) {
    [[maybe_unused]] auto one = []<std::size_t K>() -> ops_fn {  // unused without arguments
        using P = std::remove_cvref_t<std::tuple_element_t<K, Tuple>>;
        if constexpr (is_variadic_param<P>::value) {
            using E = typename is_variadic_param<P>::elem;
            if constexpr (std::is_same_v<E, Value>) return nullptr;
            else return &opsOf<E>;
        } else if constexpr (std::is_same_v<P, Value>) {
            return nullptr;
        } else {
            return &opsOf<P>;
        }
    };
    return {one.template operator()<I>()...};
}

template <typename Tuple>
constexpr bool lastIsVariadic() {
    constexpr std::size_t n = std::tuple_size_v<Tuple>;
    if constexpr (n == 0) return false;
    else return is_variadic_param<std::remove_cvref_t<std::tuple_element_t<n - 1, Tuple>>>::value;
}

// Reflection over classes.
template <typename T>
consteval auto fieldsOf() {
    return std::define_static_array(std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::unprivileged()));
}

// Whether t is declared in namespace std (or one nested in it).
consteval bool inStd(std::meta::info t) {
    for (std::meta::info p = std::meta::parent_of(t); p != ^^::; p = std::meta::parent_of(p)) {
        if (p == ^^std) return true;
    }
    return false;
}

consteval bool usableMethod(std::meta::info m) {
    using namespace std::meta;
    if (!is_function(m) || is_special_member_function(m) || is_operator_function(m) || is_static_member(m)) return false;
    if (!has_identifier(m) || !is_const(m) || is_function_template(m)) return false;
    if (is_volatile(m) || is_rvalue_reference_qualified(m)) return false;
    // Methods that return nothing aren't callable from templates (Go wants
    // one or two results).
    if (return_type_of(m) == ^^void) return false;
    // Arguments are converted into copies of the parameters' types.
    for (info p : parameters_of(m)) {
        info t = remove_cvref(type_of(p));
        if (!is_complete_type(t) || !is_copy_constructible_type(t)) return false;
    }
    return true;
}

template <typename T>
consteval auto methodsOf() {
    std::vector<std::meta::info> out;
    for (auto m : std::meta::members_of(^^T, std::meta::access_context::unprivileged())) {
        if (usableMethod(m)) out.push_back(m);
    }
    return std::define_static_array(out);
}

template <std::meta::info M>
struct method_glue {
    using R = typename[:std::meta::return_type_of(M):];
    static constexpr std::size_t nparams = std::meta::parameters_of(M).size();

    template <std::size_t K>
    using param = std::remove_cvref_t<typename[:std::meta::type_of(std::meta::parameters_of(M)[K]):]>;

    template <std::size_t... I>
    static auto tuple_type(std::index_sequence<I...>) -> std::tuple<param<I>...>;
    using args_t = decltype(tuple_type(std::make_index_sequence<nparams>()));

    static inline const std::array<ops_fn, nparams> params = paramsOf<args_t>(std::make_index_sequence<nparams>());

    template <typename C>
    static std::pair<Value, gopp::error> call(const void* obj, const owner_t& owner, std::span<const Value> args) {
        const C& self = *static_cast<const C*>(obj);
        auto tup = argsFrom<args_t>(args, std::make_index_sequence<nparams>());
        if constexpr (std::is_reference_v<R>) {
            // A reference into the receiver lives as long as it does.
            const auto& r = std::apply([&](auto&&... a) -> decltype(auto) { return (self.[:M:])(std::move(a)...); }, tup);
            if constexpr (is_result_pair<std::remove_cvref_t<R>>::value) {
                if (r.second) return {Value(), r.second};
                return {toValue(r.first, owner), nullptr};
            } else {
                return {toValue(r, owner), nullptr};
            }
        } else {
            return wrapResult(std::apply([&](auto&&... a) { return (self.[:M:])(std::move(a)...); }, tup));
        }
    }
};

template <typename T>
const std::span<const field_info> structFields() {
    static constexpr auto fs = fieldsOf<T>();
    static const auto arr = [] {
        std::array<field_info, fs.size()> a{};
        [[maybe_unused]] std::size_t i = 0;  // unused without fields
        template for (constexpr std::meta::info m : fs) {
            a[i++] = field_info{std::define_static_string(std::meta::identifier_of(m)),
                                [](const void* obj, const owner_t& owner) {
                                    return toValue(static_cast<const T*>(obj)->[:m:], owner);
                                }};
        }
        return a;
    }();
    return arr;
}

template <typename T>
const std::span<const method_info> structMethods() {
    static constexpr auto ms = methodsOf<T>();
    static const auto arr = [] {
        std::array<method_info, ms.size()> a{};
        [[maybe_unused]] std::size_t i = 0;  // unused without methods
        template for (constexpr std::meta::info m : ms) {
            using G = method_glue<m>;
            a[i++] = method_info{std::define_static_string(std::meta::identifier_of(m)), std::span<const ops_fn>(G::params),
                                 lastIsVariadic<typename G::args_t>(), &G::template call<T>};
        }
        return a;
    }();
    return arr;
}

template <typename T>
bool equalOf(const void* a, const void* b) {
    if constexpr (std::equality_comparable<T>) return *static_cast<const T*>(a) == *static_cast<const T*>(b);
    else return false;
}

template <typename T>
Value zeroOf() {
    if constexpr (std::is_same_v<T, Value>) return Value();
    else if constexpr (std::is_default_constructible_v<T> && std::is_move_constructible_v<T>) return Value::Of(T{});
    else return Value();
}

template <typename T>
type_ops makeOps();

template <typename T>
const type_ops* opsOf() {
    static const type_ops ops = makeOps<T>();
    return &ops;
}

// The builtin scalar descriptors (defined in value.cpp).
const type_ops* boolOps();
const type_ops* intOps();      // Go's int: template constants, C++ int
const type_ops* stringOps();   // Go's string
const type_ops* float64Ops();
const type_ops* complexOps();
const type_ops* errorOps();
const type_ops* anyOps();      // a Value field: Go's any
const type_ops* jsonOps();     // json::Value: Go's any
const type_ops* funcOps();     // tmpl::Func
const type_ops* scalarOps(Kind k, std::string_view name);

template <typename T>
type_ops makeOps() {
    using U = std::remove_cv_t<T>;
    type_ops o{};
    o.name = typeName<U>();
    o.zero = &zeroOf<U>;
    if constexpr (std::is_same_v<U, bool>) {
        o.kind = Kind::Bool;
    } else if constexpr (is_int_v<U> || std::is_enum_v<U>) {
        o.kind = Kind::Int;
    } else if constexpr (is_uint_v<U>) {
        o.kind = Kind::Uint;
    } else if constexpr (std::is_floating_point_v<U>) {
        o.kind = Kind::Float;
    } else if constexpr (std::is_same_v<U, std::complex<double>>) {
        o.kind = Kind::Complex;
    } else if constexpr (is_string_like_v<U>) {
        o.kind = Kind::String;
    } else if constexpr (std::is_same_v<U, Value> || std::is_same_v<U, json::Value> || std::is_same_v<U, gopp::error>) {
        o.kind = Kind::Interface;
    } else if constexpr (is_vector_like<U>::value || is_std_array<U>::value) {
        o.kind = is_std_array<U>::value ? Kind::Array : Kind::Slice;
        using E = std::remove_cv_t<typename U::value_type>;
        o.elem = &opsOf<E>;
        o.len = [](const void* p) -> std::size_t { return static_cast<const U*>(p)->size(); };
        o.index = [](const void* p, const owner_t& owner, std::size_t i) {
            return toValue((*static_cast<const U*>(p))[i], owner);
        };
        if constexpr (is_vector_like<U>::value) {
            o.isNil = [](const void* p) {
                if constexpr (std::is_same_v<U, gopp::slice<E>>) return static_cast<const U*>(p)->isNil();
                else return false;
            };
        }
    } else if constexpr (is_map_like<U>::value) {
        o.kind = Kind::Map;
        using K = std::remove_cv_t<typename U::key_type>;
        using V = std::remove_cv_t<typename U::mapped_type>;
        o.key = &opsOf<K>;
        o.elem = &opsOf<V>;
        o.len = [](const void* p) -> std::size_t { return static_cast<const U*>(p)->size(); };
        o.map_index = [](const void* p, const owner_t& owner, const Value& key) -> Value {
            const U& m = *static_cast<const U*>(p);
            auto it = m.find(from_value<K>::get(key));
            if (it == m.end()) return Value();
            return toValue(it->second, owner);
        };
        o.entries = [](const void* p, const owner_t& owner, std::vector<std::pair<Value, Value>>& out) {
            const U& m = *static_cast<const U*>(p);
            std::vector<const typename U::value_type*> es;
            for (auto& e : m) es.push_back(&e);
            if constexpr (!is_map_like<U>::value || requires { typename U::key_compare; }) {
                // std::map is ordered already, by the key's operator<.
            } else {
                std::sort(es.begin(), es.end(), [](auto* a, auto* b) { return a->first < b->first; });
            }
            for (auto* e : es) out.emplace_back(toValue(e->first, owner), toValue(e->second, owner));
        };
    } else if constexpr (pointer_traits<U>::value) {
        o.kind = Kind::Pointer;
        using E = typename pointer_traits<U>::elem;
        o.elem = &opsOf<E>;
        o.isNil = [](const void* p) { return pointer_traits<U>::get(*static_cast<const U*>(p)) == nullptr; };
        o.raw = [](const void* p) -> const void* { return pointer_traits<U>::get(*static_cast<const U*>(p)); };
        o.deref = [](const void* p, const owner_t& owner) -> Value {
            const E* e = pointer_traits<U>::get(*static_cast<const U*>(p));
            if (!e) return Value();
            // The pointee lives as long as the pointer's owner does.
            return toValue(*e, owner);
        };
    } else if constexpr (is_std_function<U>::value || std::is_function_v<std::remove_pointer_t<U>>) {
        o.kind = Kind::Func;
        o.isNil = [](const void* p) { return !static_cast<bool>(*static_cast<const U*>(p)); };
        o.func = [](const void* p) -> const Func* {
            // A Func made from the callable, cached per object address is
            // not safe; make one per call (cheap: a std::function).
            static thread_local Func f;
            f = Func(*static_cast<const U*>(p));
            return &f;
        };
    } else if constexpr (std::is_class_v<U>) {
        o.kind = Kind::Struct;
        if constexpr (std::equality_comparable<U>) o.equal = &equalOf<U>;
        // The standard library's other classes are opaque: their members
        // are its internals (std::any reaches std::type_info's).
        if constexpr (!inStd(std::meta::dealias(^^U))) {
            o.fields = structFields<U>();
            o.methods = structMethods<U>();
        }
    } else {
        static_assert(sizeof(U) == 0, "text/template: unsupported type");
    }
    return o;
}

template <typename T>
std::string_view typeName() {
    using U = std::remove_cv_t<T>;
    if constexpr (std::is_same_v<U, bool>) return "bool";
    else if constexpr (std::is_same_v<U, signed char> || (std::is_same_v<U, char> && std::is_signed_v<char>)) return "int8";
    else if constexpr (std::is_same_v<U, short>) return "int16";
    else if constexpr (std::is_same_v<U, int>) return "int";
    else if constexpr (std::is_same_v<U, long> || std::is_same_v<U, long long>) return "int64";
    else if constexpr (std::is_same_v<U, unsigned char> || std::is_same_v<U, char8_t>) return "uint8";
    else if constexpr (std::is_same_v<U, unsigned short>) return "uint16";
    else if constexpr (std::is_same_v<U, unsigned>) return "uint";
    else if constexpr (std::is_same_v<U, unsigned long> || std::is_same_v<U, unsigned long long>) return "uint64";
    else if constexpr (std::is_same_v<U, float>) return "float32";
    else if constexpr (std::is_same_v<U, double> || std::is_same_v<U, long double>) return "float64";
    else if constexpr (is_string_like_v<U>) return "string";
    else if constexpr (std::is_same_v<U, gopp::error>) return "error";
    else if constexpr (std::is_same_v<U, Value> || std::is_same_v<U, json::Value>) return "interface {}";
    else if constexpr (is_vector_like<U>::value) return intern("[]" + std::string(typeName<typename U::value_type>()));
    else if constexpr (is_std_array<U>::value) {
        return intern("[" + std::to_string(std::tuple_size_v<U>) + "]" + std::string(typeName<typename U::value_type>()));
    } else if constexpr (is_map_like<U>::value) {
        return intern("map[" + std::string(typeName<typename U::key_type>()) + "]" +
                      std::string(typeName<typename U::mapped_type>()));
    } else if constexpr (pointer_traits<U>::value) {
        return intern("*" + std::string(typeName<typename pointer_traits<U>::elem>()));
    } else if constexpr (is_std_function<U>::value || std::is_function_v<std::remove_pointer_t<U>>) {
        return "func";
    } else if constexpr (std::is_class_v<U>) {
        constexpr std::meta::info t = std::meta::dealias(^^U);
        return std::define_static_string(std::meta::has_identifier(t) ? std::meta::identifier_of(t)
                                                                       : std::meta::display_string_of(t));
    } else {
        return "?";
    }
}

template <typename T>
Value toValue(const T& v, const owner_t& owner) {
    using U = std::remove_cv_t<T>;
    if constexpr (std::is_same_v<U, Value>) {
        return v;
    } else if constexpr (std::is_same_v<U, bool>) {
        return Value::scalar(boolOps(), v);
    } else if constexpr (std::is_same_v<U, int>) {
        return Value::scalarInt(intOps(), v);
    } else if constexpr (is_int_v<U>) {
        return Value::scalarInt(scalarOps(Kind::Int, typeName<U>()), static_cast<int64>(v));
    } else if constexpr (is_uint_v<U>) {
        return Value::scalarUint(scalarOps(Kind::Uint, typeName<U>()), static_cast<uint64>(v));
    } else if constexpr (std::is_same_v<U, double>) {
        return Value::scalarFloat(float64Ops(), v);
    } else if constexpr (std::is_floating_point_v<U>) {
        return Value::scalarFloat(scalarOps(Kind::Float, typeName<U>()), static_cast<double>(v));
    } else if constexpr (std::is_same_v<U, std::complex<double>>) {
        return Value::scalarFloat(complexOps(), v.real(), v.imag());
    } else if constexpr (std::is_same_v<U, const char*> || std::is_same_v<U, char*>) {
        return Value::scalarString(stringOps(), v ? string(v) : string());
    } else if constexpr (is_string_like_v<U>) {
        return Value::scalarString(stringOps(), string(v));
    } else if constexpr (std::is_array_v<U> && std::is_same_v<std::remove_cv_t<std::remove_extent_t<U>>, char>) {
        return Value::scalarString(stringOps(), string(std::string_view(v)));
    } else if constexpr (std::is_same_v<U, gopp::error>) {
        return Value::object(errorOps(), &v, owner);
    } else if constexpr (std::is_same_v<U, json::Value>) {
        return Value::object(jsonOps(), &v, owner);
    } else if constexpr (std::is_same_v<U, Func>) {
        return Value::object(funcOps(), &v, owner);
    } else if constexpr (std::is_enum_v<U>) {
        return Value::scalarInt(scalarOps(Kind::Int, "int"), static_cast<int64>(v));
    } else {
        return Value::object(opsOf<U>(), &v, owner);
    }
}

template <typename P>
P from_value<P>::get(const Value& v) {
    if constexpr (std::is_same_v<P, Value>) {
        return v;
    } else if constexpr (std::is_same_v<P, bool>) {
        return v.Bool();
    } else if constexpr (std::is_integral_v<P>) {
        return v.Kind() == Kind::Uint ? static_cast<P>(v.Uint()) : static_cast<P>(v.Int());
    } else if constexpr (std::is_floating_point_v<P>) {
        return static_cast<P>(v.Float());
    } else if constexpr (std::is_same_v<P, std::complex<double>>) {
        return v.Complex();
    } else if constexpr (std::is_same_v<P, gopp::string>) {
        return v.Str();
    } else if constexpr (std::is_same_v<P, std::string>) {
        return v.Str().str();
    } else if constexpr (std::is_same_v<P, std::string_view>) {
        return v.Str().view();
    } else if constexpr (std::is_same_v<P, const char*>) {
        return v.Str().data();  // valid while the argument lives
    } else if constexpr (std::is_enum_v<P>) {
        return static_cast<P>(v.Int());
    } else if constexpr (std::is_pointer_v<P>) {
        // A pointer parameter takes a pointer value or the object itself.
        using E = std::remove_cv_t<std::remove_pointer_t<P>>;
        if (!v.IsValid()) return nullptr;
        if (const E* e = v.template As<E>()) return const_cast<P>(e);
        if (v.Kind() == Kind::Pointer && !v.IsNil() && v.Type()->raw) {
            // A pointer of another kind (T*, ptr<T>, optional<T>) to an E.
            const type_ops* elem = v.Type()->elem ? v.Type()->elem() : nullptr;
            if (elem == opsOf<E>() || (elem && elem->kind == opsOf<E>()->kind && elem->name == opsOf<E>()->name)) {
                return static_cast<P>(const_cast<void*>(v.Type()->raw(v.ptr())));
            }
        }
        return nullptr;
    } else {
        if (!v.IsValid()) {
            if constexpr (std::is_default_constructible_v<P>) return P{};
        }
        if (const P* p = v.template As<P>()) return *p;
        if (v.Kind() == Kind::Pointer && !v.IsNil()) {
            if (const P* p = v.Elem().template As<P>()) return *p;
        }
        if constexpr (std::is_default_constructible_v<P>) return P{};
        else panic("text/template: can't convert argument");
    }
}

}  // namespace detail

template <typename T>
    requires(!std::is_same_v<std::remove_cvref_t<T>, Value>)
Value Value::Of(T&& v) {
    using U = std::remove_cvref_t<T>;
    if constexpr (std::is_arithmetic_v<U> || detail::is_string_like_v<U> || std::is_enum_v<U> ||
                  std::is_same_v<U, std::complex<double>> ||
                  (std::is_array_v<U> && std::is_same_v<std::remove_cv_t<std::remove_extent_t<U>>, char>)) {
        return detail::toValue(v, nullptr);
    } else {
        auto holder = std::make_shared<U>(std::forward<T>(v));
        const U* p = holder.get();
        return detail::toValue(*p, std::shared_ptr<const void>(std::move(holder)));
    }
}

template <typename T>
Value Value::Ref(const T& v, detail::owner_t owner) {
    if constexpr (std::is_same_v<T, Value>) return v;
    else return detail::toValue(v, owner);
}

template <typename T>
const T* Value::As() const noexcept {
    if (!t_ || !p_) return nullptr;
    if constexpr (std::is_arithmetic_v<T> || detail::is_string_like_v<T>) {
        return nullptr;
    } else if constexpr (std::is_same_v<T, gopp::error>) {
        return t_ == detail::errorOps() ? static_cast<const T*>(p_) : nullptr;
    } else if constexpr (std::is_same_v<T, json::Value>) {
        return t_ == detail::jsonOps() ? static_cast<const T*>(p_) : nullptr;
    } else if constexpr (std::is_same_v<T, Func>) {
        return t_ == detail::funcOps() ? static_cast<const T*>(p_) : nullptr;
    } else {
        return t_ == detail::opsOf<T>() ? static_cast<const T*>(p_) : nullptr;
    }
}

template <typename F>
    requires(!std::is_same_v<std::remove_cvref_t<F>, Func>)
Func::Func(F f) {
    using Tr = detail::fn_traits<std::remove_cvref_t<F>>;
    using Args = typename Tr::args;
    constexpr std::size_t n = std::tuple_size_v<Args>;
    auto params = detail::paramsOf<Args>(std::make_index_sequence<n>());
    in_.assign(params.begin(), params.end());
    variadic_ = detail::lastIsVariadic<Args>();
    fn_ = [f = std::move(f)](std::span<const Value> args) -> std::pair<Value, gopp::error> {
        auto tup = detail::argsFrom<Args>(args, std::make_index_sequence<n>());
        return detail::wrapResult(std::apply([&](auto&&... a) { return f(std::move(a)...); }, tup));
    };
}

}  // namespace gopp::text::tmpl
