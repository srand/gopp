// The builtin functions and escapers, after Go's text/template/funcs.go.
#include "goconv.hpp"
#include "internal.hpp"

#include <gopp/net/url.hpp>
#include <gopp/strings.hpp>

#include <format>

namespace gopp::text::tmpl {

using namespace detail;

namespace {

using result = std::pair<Value, gopp::error>;

result ok(Value v) { return {std::move(v), nullptr}; }
result fail(std::string msg) { return {Value(), errors::New(string(msg))}; }

Value intValue(int64 i) { return Value::scalarInt(intOps(), i); }
Value boolValue(bool b) { return Value::scalar(boolOps(), b); }

std::pair<int, gopp::error> indexArg(const Value& index, std::size_t cap) {
    int64 x;
    switch (index.Kind()) {
    case Kind::Int: x = index.Int(); break;
    case Kind::Uint: x = static_cast<int64>(index.Uint()); break;
    case Kind::Invalid: return {0, errors::New("cannot index slice/array with nil")};
    default: return {0, errors::New(string("cannot index slice/array with type " + std::string(index.TypeName())))};
    }
    if (x < 0 || static_cast<uint64>(x) > cap) return {0, errors::New(string(std::format("index out of range: {}", x)))};
    return {static_cast<int>(x), nullptr};
}

// A slice or string value of items [i, j) of v: as an owned std::vector of
// values (Go's slices share the array; templates only read them).
Value sliceOf(const Value& item, std::size_t i, std::size_t j) {
    if (item.Kind() == Kind::String) return Value::scalarString(stringOps(), item.Str().slice(i, j));
    std::vector<Value> out;
    for (std::size_t k = i; k < j; k++) out.push_back(item.Index(k));
    return Value::Of(std::move(out));
}

result indexFn(std::span<const Value> a) {
    Value item = indirectInterface(a[0]);
    if (!item.IsValid()) return fail("index of untyped nil");
    for (std::size_t k = 1; k < a.size(); k++) {
        Value index = indirectInterface(a[k]);
        auto [it, isNil] = indirect(item);
        if (isNil) return fail("index of nil pointer");
        item = it;
        switch (item.Kind()) {
        case Kind::Array: case Kind::Slice: case Kind::String: {
            auto [x, err] = indexArg(index, item.Len());
            if (err) return {Value(), err};
            if (static_cast<std::size_t>(x) == item.Len()) return fail(std::format("index out of range: {}", x));
            item = item.Index(static_cast<std::size_t>(x));
            break;
        }
        case Kind::Map: {
            const type_ops* kt = item.Type()->key ? item.Type()->key() : nullptr;
            if (!index.IsValid()) {
                return fail("value is nil; should be of type " + std::string(kt ? kt->name : "?"));
            }
            bool fits = kt && (index.Type() == kt || index.Kind() == kt->kind ||
                               ((kt->kind == Kind::Int || kt->kind == Kind::Uint) &&
                                (index.Kind() == Kind::Int || index.Kind() == Kind::Uint)));
            if (!fits) {
                return fail("value has type " + std::string(index.TypeName()) + "; should be " + std::string(kt ? kt->name : "?"));
            }
            Value x = item.MapIndex(index);
            if (x.IsValid()) {
                item = x;
            } else {
                const type_ops* et = item.Type()->elem ? item.Type()->elem() : nullptr;
                item = et && et->zero ? et->zero() : Value();
            }
            break;
        }
        default:
            return fail("can't index item of type " + std::string(item.TypeName()));
        }
    }
    return ok(item);
}

result sliceFn(std::span<const Value> a) {
    Value item = indirectInterface(a[0]);
    if (!item.IsValid()) return fail("slice of untyped nil");
    auto [it, isNil] = indirect(item);
    if (isNil) return fail("slice of nil pointer");
    item = it;
    std::size_t n_indexes = a.size() - 1;
    if (n_indexes > 3) return fail(std::format("too many slice indexes: {}", n_indexes));
    std::size_t cap;
    switch (item.Kind()) {
    case Kind::String:
        if (n_indexes == 3) return fail("cannot 3-index slice a string");
        cap = item.Len();
        break;
    case Kind::Array: case Kind::Slice:
        cap = item.Len();
        break;
    default:
        return fail("can't slice item of type " + std::string(item.TypeName()));
    }
    int idx[3] = {0, static_cast<int>(item.Len()), 0};
    for (std::size_t k = 0; k < n_indexes; k++) {
        auto [x, err] = indexArg(a[k + 1], cap);
        if (err) return {Value(), err};
        idx[k] = x;
    }
    // Given item[i:j], make sure i <= j.
    if (idx[0] > idx[1]) return fail(std::format("invalid slice index: {} > {}", idx[0], idx[1]));
    if (n_indexes < 3) return ok(sliceOf(item, static_cast<std::size_t>(idx[0]), static_cast<std::size_t>(idx[1])));
    // Given item[i:j:k], make sure i <= j <= k.
    if (idx[1] > idx[2]) return fail(std::format("invalid slice index: {} > {}", idx[1], idx[2]));
    return ok(sliceOf(item, static_cast<std::size_t>(idx[0]), static_cast<std::size_t>(idx[1])));
}

result lenFn(std::span<const Value> a) {
    auto [item, isNil] = indirect(a[0]);
    if (isNil) return fail("len of nil pointer");
    switch (item.Kind()) {
    case Kind::Array: case Kind::Map: case Kind::Slice: case Kind::String:
        return ok(intValue(static_cast<int64>(item.Len())));
    default:
        break;
    }
    return fail("len of type " + std::string(item.TypeName()));
}

// call: the first argument is the callee's name, added by the executor.
result callFn(std::span<const Value> a) {
    std::string name = a[0].Str().str();
    Value fn = indirectInterface(a[1]);
    if (!fn.IsValid()) return fail("call of nil");
    if (fn.Kind() != Kind::Func) return fail("non-function " + name + " of type " + std::string(fn.TypeName()));
    const Func* f = fn.AsFunc();
    if (!f || !*f) return fail("call of nil");
    std::span<const Value> args = a.subspan(2);
    int num_in = f->NumIn();
    if (f->IsVariadic()) {
        if (static_cast<int>(args.size()) < num_in - 1) {
            return fail(std::format("wrong number of args for {}: got {} want at least {}", name, args.size(), num_in - 1));
        }
    } else if (static_cast<int>(args.size()) != num_in) {
        return fail(std::format("wrong number of args for {}: got {} want {}", name, args.size(), num_in));
    }
    std::vector<Value> argv;
    for (std::size_t i = 0; i < args.size(); i++) {
        Value arg = indirectInterface(args[i]);
        const type_ops* t = (!f->IsVariadic() || static_cast<int>(i) < num_in - 1) ? f->In(static_cast<int>(i)) : f->In(num_in - 1);
        // prepareArg
        if (!arg.IsValid()) {
            bool nilable = !t || t->kind == Kind::Pointer || t->kind == Kind::Map || t->kind == Kind::Slice ||
                           t->kind == Kind::Func || t->kind == Kind::Interface;
            if (!nilable) return fail(std::format("arg {}: value is nil; should be of type {}", i, t->name));
        } else if (t && arg.Type() != t && arg.Kind() != t->kind &&
                   !((arg.Kind() == Kind::Int || arg.Kind() == Kind::Uint) && (t->kind == Kind::Int || t->kind == Kind::Uint)) &&
                   !(t->kind == Kind::Interface && t->name == "interface {}")) {
            return fail(std::format("arg {}: value has type {}; should be {}", i, arg.TypeName(), t->name));
        }
        argv.push_back(arg);
    }
    return f->Call(argv);
}

result notFn(std::span<const Value> a) { return ok(boolValue(!isTrue(indirectInterface(a[0])).first)); }

enum class basic { invalid, boolean, complex, integer, floating, str, uinteger };

basic basicKind(const Value& v) {
    switch (v.Kind()) {
    case Kind::Bool: return basic::boolean;
    case Kind::Int: return basic::integer;
    case Kind::Uint: return basic::uinteger;
    case Kind::Float: return basic::floating;
    case Kind::Complex: return basic::complex;
    case Kind::String: return basic::str;
    default: return basic::invalid;
    }
}

bool isNilValue(const Value& v) {
    if (!v.IsValid()) return true;
    switch (v.Kind()) {
    case Kind::Func: case Kind::Interface: case Kind::Map: case Kind::Pointer: case Kind::Slice: return v.IsNil();
    default: return false;
    }
}

const gopp::error err_bad_comparison_type = errors::New("invalid type for comparison");
const gopp::error err_no_comparison = errors::New("missing argument for comparison");

std::pair<bool, gopp::error> eq(const Value& a1, std::span<const Value> a2) {
    Value arg1 = indirectInterface(a1);
    if (a2.empty()) return {false, err_no_comparison};
    basic k1 = basicKind(arg1);
    for (const Value& a : a2) {
        Value arg = indirectInterface(a);
        basic k2 = basicKind(arg);
        bool truth = false;
        if (k1 != k2) {
            // Special case: Can compare integer values regardless of type's
            // sign.
            if (k1 == basic::integer && k2 == basic::uinteger) {
                truth = arg1.Int() >= 0 && static_cast<uint64>(arg1.Int()) == arg.Uint();
            } else if (k1 == basic::uinteger && k2 == basic::integer) {
                truth = arg.Int() >= 0 && arg1.Uint() == static_cast<uint64>(arg.Int());
            } else if (arg1.IsValid() && arg.IsValid()) {
                return {false, errors::New(string("incompatible types for comparison: " + std::string(arg1.TypeName()) + " and " +
                                                  std::string(arg.TypeName())))};
            }
        } else {
            switch (k1) {
            case basic::boolean: truth = arg1.Bool() == arg.Bool(); break;
            case basic::complex: truth = arg1.Complex() == arg.Complex(); break;
            case basic::floating: truth = arg1.Float() == arg.Float(); break;
            case basic::integer: truth = arg1.Int() == arg.Int(); break;
            case basic::str: truth = arg1.Str() == arg.Str(); break;
            case basic::uinteger: truth = arg1.Uint() == arg.Uint(); break;
            default: {
                if (arg1.IsValid() && arg.IsValid() && arg1.Kind() != arg.Kind()) {
                    return {false, errors::New(string("non-comparable types " + Sprint(std::span(&arg1, 1)).str() + ": " +
                                                      std::string(arg1.TypeName()) + ", " + std::string(arg.TypeName()) + ": " +
                                                      Sprint(std::span(&arg, 1)).str()))};
                }
                if (isNilValue(arg1) || isNilValue(arg)) {
                    truth = isNilValue(arg) == isNilValue(arg1);
                } else {
                    if (arg.Type() != arg1.Type() || !arg.Type()->equal || arg.Kind() == Kind::Slice || arg.Kind() == Kind::Map ||
                        arg.Kind() == Kind::Func) {
                        return {false, errors::New(string("non-comparable type " + Sprint(std::span(&arg, 1)).str() + ": " +
                                                          std::string(arg.TypeName())))};
                    }
                    truth = arg.Type()->equal(arg1.ptr(), arg.ptr());
                }
            }
            }
        }
        if (truth) return {true, nullptr};
    }
    return {false, nullptr};
}

std::pair<bool, gopp::error> lt(const Value& a1, const Value& a2) {
    Value arg1 = indirectInterface(a1);
    basic k1 = basicKind(arg1);
    if (k1 == basic::invalid) return {false, err_bad_comparison_type};
    Value arg2 = indirectInterface(a2);
    basic k2 = basicKind(arg2);
    if (k2 == basic::invalid) return {false, err_bad_comparison_type};
    bool truth = false;
    if (k1 != k2) {
        // Special case: Can compare integer values regardless of type's
        // sign.
        if (k1 == basic::integer && k2 == basic::uinteger) {
            truth = arg1.Int() < 0 || static_cast<uint64>(arg1.Int()) < arg2.Uint();
        } else if (k1 == basic::uinteger && k2 == basic::integer) {
            truth = arg2.Int() >= 0 && arg1.Uint() < static_cast<uint64>(arg2.Int());
        } else {
            return {false, errors::New(string("incompatible types for comparison: " + std::string(arg1.TypeName()) + " and " +
                                              std::string(arg2.TypeName())))};
        }
    } else {
        switch (k1) {
        case basic::boolean: case basic::complex: return {false, err_bad_comparison_type};
        case basic::floating: truth = arg1.Float() < arg2.Float(); break;
        case basic::integer: truth = arg1.Int() < arg2.Int(); break;
        case basic::str: truth = arg1.Str().view() < arg2.Str().view(); break;
        case basic::uinteger: truth = arg1.Uint() < arg2.Uint(); break;
        default: panic("invalid kind");
        }
    }
    return {truth, nullptr};
}

std::pair<bool, gopp::error> le(const Value& a, const Value& b) {
    auto [less, err] = lt(a, b);
    if (less || err) return {less, err};
    return eq(a, std::span(&b, 1));
}

result boolResult(std::pair<bool, gopp::error> r) {
    if (r.second) return {Value(), r.second};
    return ok(boolValue(r.first));
}

// evalArgs formats the arguments of html, js and urlquery: a single
// string is used as is; anything else is printed like print does.
std::string eval_args(std::span<const Value> args) {
    if (args.size() == 1 && args[0].Kind() == Kind::String) return args[0].Str().str();
    // printableValue: pointers print as what they point to, nothing as
    // <no value>.
    std::vector<Value> printable;
    for (const Value& a : args) {
        Value v = a;
        if (v.Kind() == Kind::Pointer) v = indirect(v).first;
        if (!v.IsValid()) v = Value::scalarString(stringOps(), string::fromStatic("<no value>"));
        printable.push_back(v);
    }
    return Sprint(printable).str();
}

}  // namespace

namespace detail {

const FuncMap& builtins() {
    static const FuncMap* m = [] {
        auto* f = new FuncMap();
        auto& b = *f;
        b["and"] = Func::Raw(2, true, [](std::span<const Value>) -> result { panic("unreachable"); });
        b["call"] = Func::Raw(2, true, callFn);
        b["html"] = Func::Raw(1, true, [](std::span<const Value> a) {
            return ok(Value::scalarString(stringOps(), HTMLEscapeString(eval_args(a))));
        });
        b["index"] = Func::Raw(2, true, indexFn);
        b["slice"] = Func::Raw(2, true, sliceFn);
        b["js"] = Func::Raw(1, true, [](std::span<const Value> a) {
            return ok(Value::scalarString(stringOps(), JSEscapeString(eval_args(a))));
        });
        b["len"] = Func::Raw(1, false, lenFn);
        b["not"] = Func::Raw(1, false, notFn);
        b["or"] = Func::Raw(2, true, [](std::span<const Value>) -> result { panic("unreachable"); });
        b["print"] = Func::Raw(1, true, [](std::span<const Value> a) { return ok(Value::scalarString(stringOps(), Sprint(a))); });
        b["printf"] = Func::Raw(std::vector<ops_fn>{&stringOps, nullptr}, true, [](std::span<const Value> a) {
            return ok(Value::scalarString(stringOps(), Sprintf(a[0].Str(), a.subspan(1))));
        });
        b["println"] = Func::Raw(1, true, [](std::span<const Value> a) { return ok(Value::scalarString(stringOps(), Sprintln(a))); });
        b["urlquery"] = Func::Raw(1, true, [](std::span<const Value> a) {
            return ok(Value::scalarString(stringOps(), url::QueryEscape(eval_args(a))));
        });
        // Comparisons.
        b["eq"] = Func::Raw(2, true, [](std::span<const Value> a) { return boolResult(eq(a[0], a.subspan(1))); });
        b["ge"] = Func::Raw(2, false, [](std::span<const Value> a) {
            auto [less, err] = lt(a[0], a[1]);
            return boolResult({!less && !err, err});
        });
        b["gt"] = Func::Raw(2, false, [](std::span<const Value> a) {
            auto [le_, err] = le(a[0], a[1]);
            return boolResult({!le_ && !err, err});
        });
        b["le"] = Func::Raw(2, false, [](std::span<const Value> a) { return boolResult(le(a[0], a[1])); });
        b["lt"] = Func::Raw(2, false, [](std::span<const Value> a) { return boolResult(lt(a[0], a[1])); });
        b["ne"] = Func::Raw(2, false, [](std::span<const Value> a) {
            auto [equal, err] = eq(a[0], a.subspan(1, 1));
            return boolResult({!equal && !err, err});
        });
        return f;
    }();
    return *m;
}

const parse::FuncNames& builtinNames() {
    static const parse::FuncNames* n = [] {
        auto* s = new parse::FuncNames();
        for (auto& [name, f] : builtins()) s->insert(name);
        return s;
    }();
    return *n;
}

}  // namespace detail

// ---------------------------------------------------------------------------
// Escapers

void HTMLEscape(io::Writer& w, std::span<const byte> b) {
    std::size_t last = 0;
    for (std::size_t i = 0; i < b.size(); i++) {
        std::string_view html;
        switch (b[i]) {
        case '\0': html = "\xEF\xBF\xBD"; break;  // U+FFFD
        case '"': html = "&#34;"; break;          // shorter than "&quot;"
        case '\'': html = "&#39;"; break;         // shorter than "&apos;" and apos was not in HTML until HTML5
        case '&': html = "&amp;"; break;
        case '<': html = "&lt;"; break;
        case '>': html = "&gt;"; break;
        default: continue;
        }
        w.Write(b.subspan(last, i - last));
        io::WriteString(w, html);
        last = i + 1;
    }
    w.Write(b.subspan(last));
}

string HTMLEscapeString(std::string_view s) {
    // Avoid allocation if we can.
    if (s.find_first_of(std::string_view("'\"&<>\0", 6)) == std::string_view::npos) return string(s);
    strings::Builder b;
    HTMLEscape(b, std::span<const byte>(reinterpret_cast<const byte*>(s.data()), s.size()));
    return b.str();
}

namespace {

bool jsIsSpecial(detail::rune r) {
    switch (r) {
    case '\\': case '\'': case '"': case '<': case '>': case '&': case '=':
        return true;
    }
    return r < ' ' || r >= 0x80;
}

}  // namespace

void JSEscape(io::Writer& w, std::span<const byte> b) {
    std::string_view s(reinterpret_cast<const char*>(b.data()), b.size());
    std::string out;
    std::size_t last = 0;
    static const char* hex = "0123456789ABCDEF";
    for (std::size_t i = 0; i < s.size(); i++) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (!jsIsSpecial(c)) continue;
        out.append(s.substr(last, i - last));
        if (c < 0x80) {
            switch (c) {
            case '\\': out += "\\\\"; break;
            case '\'': out += "\\'"; break;
            case '"': out += "\\\""; break;
            case '<': out += "\\u003C"; break;
            case '>': out += "\\u003E"; break;
            case '&': out += "\\u0026"; break;
            case '=': out += "\\u003D"; break;
            default:
                out += "\\u00";
                out.push_back(hex[c >> 4]);
                out.push_back(hex[c & 0x0f]);
            }
        } else {
            // Unicode rune.
            auto [r, size] = detail::decodeRune(s, i);
            if (detail::isPrint(r)) {
                out.append(s.substr(i, static_cast<std::size_t>(size)));
            } else {
                out += std::format("\\u{:04X}", static_cast<unsigned>(r));
            }
            i += static_cast<std::size_t>(size) - 1;
        }
        last = i + 1;
    }
    out.append(s.substr(last));
    io::WriteString(w, out);
}

string JSEscapeString(std::string_view s) {
    bool special = false;
    for (unsigned char c : s) special |= jsIsSpecial(c);
    if (!special) return string(s);
    strings::Builder b;
    JSEscape(b, std::span<const byte>(reinterpret_cast<const byte*>(s.data()), s.size()));
    return b.str();
}

string HTMLEscaper(std::span<const Value> args) { return HTMLEscapeString(eval_args(args)); }
string JSEscaper(std::span<const Value> args) { return JSEscapeString(eval_args(args)); }
string URLQueryEscaper(std::span<const Value> args) { return url::QueryEscape(eval_args(args)); }

}  // namespace gopp::text::tmpl
