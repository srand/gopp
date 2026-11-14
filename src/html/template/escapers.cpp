// html/template's escaping functions, ported from Go 1.27.3's
// html/template/content.go, html.go, js.go, css.go and url.go.
#include "html/template/internal.hpp"

#include <gopp/encoding/base64.hpp>
#include <gopp/encoding/json.hpp>
#include <gopp/regexp.hpp>
#include <gopp/unicode/utf8.hpp>

#include <algorithm>
#include <cmath>
#include <format>

namespace gopp::html::tmpl::detail {

using ttmpl::Kind;

const std::string_view filterFailsafe = "ZgotmplZ";

namespace {

Value stringValue(std::string s) { return Value::scalarString(ttmpl::detail::stringOps(), string(s)); }

// The value a pointer or interface ends at.
Value indirect(Value v) {
    for (;;) {
        if (!v.IsValid()) return v;
        if (v.Kind() == Kind::Interface) {
            if (v.IsNil()) return Value();
            v = v.Elem();
        } else if (v.Kind() == Kind::Pointer && !v.IsNil()) {
            v = v.Elem();
        } else {
            return v;
        }
    }
}

bool hasMethod(const Value& v, std::string_view name) {
    const auto* m = v.MethodByName(name);
    return m && m->in.empty();
}

// v, dereferenced until it reaches a fmt.Stringer, an error, or a nil
// pointer.
Value indirectToStringerOrError(Value v) {
    while (v.IsValid() && !hasMethod(v, "String") && !hasMethod(v, "Error") &&
           ((v.Kind() == Kind::Pointer && !v.IsNil()) || (v.Kind() == Kind::Interface && !v.IsNil()))) {
        v = v.Elem();
    }
    return v;
}

// v, dereferenced until it reaches a json.Marshaler or a nil pointer.
Value indirectToJSONMarshaler(Value v) {
    while (v.IsValid() && !hasMethod(v, "MarshalJSON") &&
           ((v.Kind() == Kind::Pointer && !v.IsNil()) || (v.Kind() == Kind::Interface && !v.IsNil()))) {
        v = v.Elem();
    }
    return v;
}

contentType contentOf(const Value& v) {
    const auto* t = v.Type();
    using ttmpl::detail::opsOf;
    if (t == opsOf<CSS>()) return contentTypeCSS;
    if (t == opsOf<HTML>()) return contentTypeHTML;
    if (t == opsOf<HTMLAttr>()) return contentTypeHTMLAttr;
    if (t == opsOf<JS>()) return contentTypeJS;
    if (t == opsOf<JSStr>()) return contentTypeJSStr;
    if (t == opsOf<URL>()) return contentTypeURL;
    if (t == opsOf<Srcset>()) return contentTypeSrcset;
    return contentTypePlain;
}

// Go's evalArgs: the arguments as fmt.Sprint has them, for the predefined
// escapers' arguments.
std::string evalArgs(std::span<const Value> args) {
    if (args.size() == 1 && args[0].IsValid() && args[0].Type() == ttmpl::detail::stringOps()) {
        return args[0].Str().str();
    }
    std::vector<Value> a;
    for (const auto& arg : args) a.push_back(indirectToStringerOrError(arg));
    return ttmpl::Sprint(a).str();
}

}  // namespace

std::pair<std::string, contentType> stringify(std::span<const Value> args) {
    if (args.size() == 1) {
        Value v = indirect(args[0]);
        if (v.IsValid() && v.Kind() == Kind::String) return {v.Str().str(), contentOf(v)};
    }
    std::vector<Value> a;
    for (const auto& arg : args) {
        // A nil interface is a nil argument, as it is when Go passes it as
        // an any.
        if (!arg.IsValid() || (arg.Kind() == Kind::Interface && arg.IsNil())) continue;
        a.push_back(indirectToStringerOrError(arg));
    }
    return {ttmpl::Sprint(a).str(), contentTypePlain};
}

// ---------------------------------------------------------------------------
// html.go

namespace {

using table = std::span<const std::string_view>;

// The replacements of the runes below 0x80 that need them, by rune (Go's
// tables are as long as their last entry; only the low Unicode table's
// length matters).
template <std::size_t N>
constexpr std::array<std::string_view, N> makeTable(std::initializer_list<std::pair<char, std::string_view>> entries) {
    std::array<std::string_view, N> t{};
    for (auto [c, s] : entries) t[std::size_t(byte(c))] = s;
    return t;
}

constexpr auto htmlReplacementTable = makeTable<128>({
    {0, "�"},
    {'"', "&#34;"},
    {'&', "&amp;"},
    {'\'', "&#39;"},
    {'+', "&#43;"},
    {'<', "&lt;"},
    {'>', "&gt;"},
});

// Like htmlReplacementTable but without '&' to avoid over-encoding
// existing entities.
constexpr auto htmlNormReplacementTable = makeTable<128>({
    {0, "�"},
    {'"', "&#34;"},
    {'\'', "&#39;"},
    {'+', "&#43;"},
    {'<', "&lt;"},
    {'>', "&gt;"},
});

// The replacements for an unquoted attribute value: what ends it too.
constexpr auto htmlNospaceReplacementTable = makeTable<128>({
    {0, "&#xfffd;"},
    {'\t', "&#9;"},
    {'\n', "&#10;"},
    {'\v', "&#11;"},
    {'\f', "&#12;"},
    {'\r', "&#13;"},
    {' ', "&#32;"},
    {'"', "&#34;"},
    {'&', "&amp;"},
    {'\'', "&#39;"},
    {'+', "&#43;"},
    {'<', "&lt;"},
    {'=', "&#61;"},
    {'>', "&gt;"},
    // A parse error in the attribute value (unquoted) and before attribute
    // value states. Treated as a quoting character by IE.
    {'`', "&#96;"},
});

// Like htmlNospaceReplacementTable but without '&' to avoid over-encoding
// existing entities.
constexpr auto htmlNospaceNormReplacementTable = makeTable<128>({
    {0, "&#xfffd;"},
    {'\t', "&#9;"},
    {'\n', "&#10;"},
    {'\v', "&#11;"},
    {'\f', "&#12;"},
    {'\r', "&#13;"},
    {' ', "&#32;"},
    {'"', "&#34;"},
    {'\'', "&#39;"},
    {'+', "&#43;"},
    {'<', "&lt;"},
    {'=', "&#61;"},
    {'>', "&gt;"},
    // A parse error in the attribute value (unquoted) and before attribute
    // value states. Treated as a quoting character by IE.
    {'`', "&#96;"},
});

// s with the runes in replacementTable replaced; with badRunes false, the
// noncharacters (U+FDD0-U+FDEF, U+FFF0-U+FFFF) are escaped too.
std::string htmlReplacer(std::string_view s, table replacementTable, bool badRunes) {
    std::size_t written = 0;
    std::string b;
    for (std::size_t i = 0, w = 0; i < s.size(); i += w) {
        auto [r, rw] = utf8::DecodeRuneInString(s.substr(i));
        w = std::size_t(rw);
        if (std::size_t(r) < replacementTable.size()) {
            if (std::string_view repl = replacementTable[std::size_t(r)]; !repl.empty()) {
                if (written == 0) b.reserve(s.size());
                b += s.substr(written, i - written);
                b += repl;
                written = i + w;
            }
        } else if (badRunes) {
            // No-op.
            // IE does not allow these ranges in unquoted attrs.
        } else if ((0xfdd0 <= r && r <= 0xfdef) || (0xfff0 <= r && r <= 0xffff)) {
            if (written == 0) b.reserve(s.size());
            b += s.substr(written, i - written);
            b += std::format("&#x{:x};", r);
            written = i + w;
        }
    }
    if (written == 0) return std::string(s);
    b += s.substr(written);
    return b;
}

// Escapes for inclusion in unquoted attribute values.
std::string htmlNospaceEscaper(std::span<const Value> args) {
    auto [s, t] = stringify(args);
    if (s.empty()) return std::string(filterFailsafe);
    if (t == contentTypeHTML) return htmlReplacer(stripTags(s), htmlNospaceNormReplacementTable, false);
    return htmlReplacer(s, htmlNospaceReplacementTable, false);
}

// Escapes for inclusion in quoted attribute values.
std::string attrEscaper(std::span<const Value> args) {
    auto [s, t] = stringify(args);
    if (t == contentTypeHTML) return htmlReplacer(stripTags(s), htmlNormReplacementTable, true);
    return htmlReplacer(s, htmlReplacementTable, true);
}

// Escapes for inclusion in an RCDATA element body.
std::string rcdataEscaper(std::span<const Value> args) {
    auto [s, t] = stringify(args);
    if (t == contentTypeHTML) return htmlReplacer(s, htmlNormReplacementTable, true);
    return htmlReplacer(s, htmlReplacementTable, true);
}

// Accepts a string, and returns it if it's a valid HTML attribute name,
// in lower case, or ZgotmplZ.
std::string htmlNameFilter(std::span<const Value> args) {
    auto [s, t] = stringify(args);
    if (t == contentTypeHTMLAttr) return s;
    if (s.empty()) {
        // Avoid violation of structure preservation.
        // <input checked {{.K}}={{.V}}>.
        // Without this, if .K is empty then .V is the value of checked, but
        // otherwise .V is the value of the attribute named .K.
        return std::string(filterFailsafe);
    }
    for (char& c : s) {
        if ('A' <= c && c <= 'Z') c = char(c + 'a' - 'A');
    }
    // strings.ToLower lowers non-ASCII letters too; they're rejected below
    // anyway.
    if (attrType(s) != contentTypePlain) {
        // TODO: Split attr and element name part filters so we can allowlist
        // attributes.
        return std::string(filterFailsafe);
    }
    for (std::size_t i = 0; i < s.size();) {
        auto [r, w] = utf8::DecodeRuneInString(std::string_view(s).substr(i));
        if (!(('0' <= r && r <= '9') || ('a' <= r && r <= 'z'))) return std::string(filterFailsafe);
        i += std::size_t(w);
    }
    return s;
}

// Comments are dropped from the output; actions in them produce nothing.
std::string commentEscaper(std::span<const Value>) { return ""; }

}  // namespace

// Escapes for inclusion in HTML text.
std::string htmlEscaper(std::span<const Value> args) {
    auto [s, t] = stringify(args);
    if (t == contentTypeHTML) return s;
    return htmlReplacer(s, htmlReplacementTable, true);
}

// The text of html with its tags removed, for an HTML value in an
// attribute.
std::string stripTags(std::string_view html) {
    std::string b;
    std::string_view s = html;
    context c;
    std::size_t i = 0;
    bool allText = true;
    // Using the transition funcs helps us avoid mangling `<div title="1>2">`
    // or `I <3 Ponies!`.
    while (i != s.size()) {
        if (c.dl == delimNone) {
            state st = c.st;
            // Use RCDATA instead of parsing into JS or CSS styles.
            if (c.el != elementNone && !isInTag(st)) st = stateRCDATA;
            auto [d, nread] = transitionFunc[st](c, s.substr(i));
            std::size_t i1 = i + nread;
            if (c.st == stateText || c.st == stateRCDATA) {
                // Emit text up to the start of the tag or comment.
                std::size_t j = i1;
                if (d.st != c.st) {
                    for (std::size_t j1 = j; j1-- > i;) {
                        if (s[j1] == '<') {
                            j = j1;
                            break;
                        }
                    }
                }
                b += s.substr(i, j - i);
            } else {
                allText = false;
            }
            c = d;
            i = i1;
            continue;
        }
        static constexpr std::string_view delimEnds[] = {"", "\"", "'", " \t\n\f\r>"};
        std::size_t k = s.substr(i).find_first_of(delimEnds[c.dl]);
        if (k == s.npos) break;
        std::size_t i1 = i + k;
        if (c.dl != delimSpaceOrTagEnd) {
            // Consume any quote.
            i1++;
        }
        context d;
        d.st = stateTag;
        d.el = c.el;
        c = d;
        i = i1;
    }
    if (allText) return std::string(html);
    if (c.st == stateText || c.st == stateRCDATA) b += s.substr(i);
    return b;
}

// ---------------------------------------------------------------------------
// js.go

namespace {

template <std::size_t N>
constexpr std::array<std::string_view, N> makeRuneTable(std::initializer_list<std::pair<rune, std::string_view>> entries) {
    std::array<std::string_view, N> t{};
    for (auto [c, s] : entries) t[std::size_t(c)] = s;
    return t;
}

constexpr auto lowUnicodeReplacementTable = makeRuneTable<0x20>({
    {0, R"(\u0000)"}, {1, R"(\u0001)"}, {2, R"(\u0002)"}, {3, R"(\u0003)"}, {4, R"(\u0004)"}, {5, R"(\u0005)"},
    {6, R"(\u0006)"},
    {'\a', R"(\u0007)"},
    {'\b', R"(\u0008)"},
    {'\t', R"(\t)"},
    {'\n', R"(\n)"},
    {'\v', R"(\u000b)"},  // "\v" == "v" on IE 6.
    {'\f', R"(\f)"},
    {'\r', R"(\r)"},
    {0xe, R"(\u000e)"}, {0xf, R"(\u000f)"}, {0x10, R"(\u0010)"}, {0x11, R"(\u0011)"}, {0x12, R"(\u0012)"},
    {0x13, R"(\u0013)"}, {0x14, R"(\u0014)"}, {0x15, R"(\u0015)"}, {0x16, R"(\u0016)"}, {0x17, R"(\u0017)"},
    {0x18, R"(\u0018)"}, {0x19, R"(\u0019)"}, {0x1a, R"(\u001a)"}, {0x1b, R"(\u001b)"}, {0x1c, R"(\u001c)"},
    {0x1d, R"(\u001d)"}, {0x1e, R"(\u001e)"}, {0x1f, R"(\u001f)"},
});

constexpr auto jsStrReplacementTable = makeRuneTable<128>({
    {0, R"(\u0000)"},
    {'\t', R"(\t)"},
    {'\n', R"(\n)"},
    {'\v', R"(\u000b)"},  // "\v" == "v" on IE 6.
    {'\f', R"(\f)"},
    {'\r', R"(\r)"},
    // Encode HTML specials as hex so the output can be embedded in HTML
    // attributes without further encoding.
    {'"', R"(\u0022)"},
    {'`', R"(\u0060)"},
    {'&', R"(\u0026)"},
    {'\'', R"(\u0027)"},
    {'+', R"(\u002b)"},
    {'/', R"(\/)"},
    {'<', R"(\u003c)"},
    {'>', R"(\u003e)"},
    {'\\', R"(\\)"},
});

// Like jsStrReplacementTable, but also contains the special characters
// for JS template literals: $, { and }.
constexpr auto jsBqStrReplacementTable = makeRuneTable<128>({
    {0, R"(\u0000)"},
    {'\t', R"(\t)"},
    {'\n', R"(\n)"},
    {'\v', R"(\u000b)"},  // "\v" == "v" on IE 6.
    {'\f', R"(\f)"},
    {'\r', R"(\r)"},
    // Encode HTML specials as hex so the output can be embedded in HTML
    // attributes without further encoding.
    {'"', R"(\u0022)"},
    {'`', R"(\u0060)"},
    {'&', R"(\u0026)"},
    {'\'', R"(\u0027)"},
    {'+', R"(\u002b)"},
    {'/', R"(\/)"},
    {'<', R"(\u003c)"},
    {'>', R"(\u003e)"},
    {'\\', R"(\\)"},
    {'$', R"(\u0024)"},
    {'{', R"(\u007b)"},
    {'}', R"(\u007d)"},
});

// Like jsStrReplacementTable but does not overencode existing escapes
// since this table has no entry for `\`.
constexpr auto jsStrNormReplacementTable = makeRuneTable<128>({
    {0, R"(\u0000)"},
    {'\t', R"(\t)"},
    {'\n', R"(\n)"},
    {'\v', R"(\u000b)"},  // "\v" == "v" on IE 6.
    {'\f', R"(\f)"},
    {'\r', R"(\r)"},
    // Encode HTML specials as hex so the output can be embedded in HTML
    // attributes without further encoding.
    {'"', R"(\u0022)"},
    {'&', R"(\u0026)"},
    {'\'', R"(\u0027)"},
    {'`', R"(\u0060)"},
    {'+', R"(\u002b)"},
    {'/', R"(\/)"},
    {'<', R"(\u003c)"},
    {'>', R"(\u003e)"},
});

constexpr auto jsRegexpReplacementTable = makeRuneTable<128>({
    {0, R"(\u0000)"},
    {'\t', R"(\t)"},
    {'\n', R"(\n)"},
    {'\v', R"(\u000b)"},  // "\v" == "v" on IE 6.
    {'\f', R"(\f)"},
    {'\r', R"(\r)"},
    // Encode HTML specials as hex so the output can be embedded in HTML
    // attributes without further encoding.
    {'"', R"(\u0022)"},
    {'$', R"(\$)"},
    {'&', R"(\u0026)"},
    {'\'', R"(\u0027)"},
    {'(', R"(\()"},
    {')', R"(\))"},
    {'*', R"(\*)"},
    {'+', R"(\u002b)"},
    {'-', R"(\-)"},
    {'.', R"(\.)"},
    {'/', R"(\/)"},
    {'<', R"(\u003c)"},
    {'>', R"(\u003e)"},
    {'?', R"(\?)"},
    {'[', R"(\[)"},
    {'\\', R"(\\)"},
    {']', R"(\])"},
    {'^', R"(\^)"},
    {'{', R"(\{)"},
    {'|', R"(\|)"},
    {'}', R"(\})"},
});

// s with the runes in replacementTable replaced, and the low control
// characters and U+2028 and U+2029 escaped.
std::string replace(std::string_view s, table replacementTable) {
    std::string b;
    std::size_t written = 0;
    for (std::size_t i = 0, w = 0; i < s.size(); i += w) {
        auto [r, rw] = utf8::DecodeRuneInString(s.substr(i));
        w = std::size_t(rw);
        std::string_view repl;
        if (std::size_t(r) < lowUnicodeReplacementTable.size()) {
            repl = lowUnicodeReplacementTable[std::size_t(r)];
        } else if (std::size_t(r) < replacementTable.size() && !replacementTable[std::size_t(r)].empty()) {
            repl = replacementTable[std::size_t(r)];
        } else if (r == 0x2028) {
            repl = R"(\u2028)";
        } else if (r == 0x2029) {
            repl = R"(\u2029)";
        } else {
            continue;
        }
        if (written == 0) b.reserve(s.size());
        b += s.substr(written, i - written);
        b += repl;
        written = i + w;
    }
    if (written == 0) return std::string(s);
    b += s.substr(written);
    return b;
}

const ptr<regexp::Regexp>& scriptTagRe() {
    static const ptr<regexp::Regexp> re = regexp::MustCompile("(?i)<(/?)script");
    return re;
}

std::string replaceAll(std::string s, std::string_view from, std::string_view to) {
    for (std::size_t i = 0; (i = s.find(from, i)) != s.npos; i += to.size()) s.replace(i, from.size(), to);
    return s;
}

// JSON encoding of template values, as Go's json.Marshal of the dynamic
// values the escaper gets.
struct jsonEncoder {
    json::detail::encoder e;

    gopp::error unsupportedType(const Value& v) {
        return errors::New("json: unsupported type: " + std::string(v.TypeName()));
    }

    gopp::error encode(const Value& v, int depth) {
        if (depth > 1000) return errors::New("json: unsupported value: encountered a cycle via " + std::string(v.TypeName()));
        if (!v.IsValid()) {
            e.out += "null";
            return nullptr;
        }
        if (const auto* m = v.MethodByName("MarshalJSON"); m && m->in.empty()) {
            auto [r, err] = m->call(v.ptr(), v.owner(), {});
            if (!err) {
                if (const auto* b = r.As<slice<byte>>()) {
                    // Validated, then compacted with <, > and & escaped, as
                    // Go's encoder does with a marshaler's output.
                    std::string_view src = asStringView(*b);
                    err = json::detail::validate(src);
                    if (!err) {
                        json::detail::appendCompact(e.out, src, true);
                        return nullptr;
                    }
                } else {
                    err = errors::New("MarshalJSON didn't return bytes");
                }
            }
            return errors::New("json: error calling MarshalJSON for type " + std::string(v.TypeName()) + ": " +
                               err.Error().str());
        }
        switch (v.Kind()) {
            case Kind::Invalid:
                e.out += "null";
                return nullptr;
            case Kind::Bool:
                e.out += v.Bool() ? "true" : "false";
                return nullptr;
            case Kind::Int:
                e.writeInt(v.Int());
                return nullptr;
            case Kind::Uint:
                e.writeUint(v.Uint());
                return nullptr;
            case Kind::Float: {
                double f = v.Float();
                int bits = v.TypeName() == "float32" ? 32 : 64;
                if (std::isnan(f) || std::isinf(f)) {
                    return errors::New(std::string("json: unsupported value: ") +
                                       (std::isnan(f) ? "NaN" : f > 0 ? "+Inf" : "-Inf"));
                }
                e.writeFloat(f, bits);
                return nullptr;
            }
            case Kind::String:
                e.WriteString(v.Str());
                return nullptr;
            case Kind::Slice:
            case Kind::Array: {
                if (v.Kind() == Kind::Slice && v.IsNil()) {
                    e.out += "null";
                    return nullptr;
                }
                const auto* elem = v.Type()->elem ? v.Type()->elem() : nullptr;
                std::size_t n = v.Len();
                if (v.Kind() == Kind::Slice && elem && elem->kind == Kind::Uint && elem->name == "uint8") {
                    // []byte encodes as a base64 string.
                    std::vector<byte> bs(n);
                    for (std::size_t i = 0; i < n; i++) bs[i] = byte(v.Index(i).Uint());
                    e.out += '"';
                    e.out += base64::StdEncoding.EncodeToString(bs).view();
                    e.out += '"';
                    return nullptr;
                }
                e.out += '[';
                for (std::size_t i = 0; i < n; i++) {
                    if (i) e.out += ',';
                    if (auto err = encode(v.Index(i), depth + 1)) return err;
                }
                e.out += ']';
                return nullptr;
            }
            case Kind::Map: {
                // Keys are strings or integers, sorted as strings.
                std::vector<std::pair<std::string, Value>> entries;
                for (auto& [k, val] : v.MapEntries()) {
                    std::string key;
                    switch (k.Kind()) {
                        case Kind::String: key = k.Str().str(); break;
                        case Kind::Int: key = std::to_string(k.Int()); break;
                        case Kind::Uint: key = std::to_string(k.Uint()); break;
                        default: return unsupportedType(v);
                    }
                    entries.emplace_back(std::move(key), val);
                }
                std::sort(entries.begin(), entries.end(), [](auto& a, auto& b) { return a.first < b.first; });
                e.out += '{';
                for (std::size_t i = 0; i < entries.size(); i++) {
                    if (i) e.out += ',';
                    e.WriteString(entries[i].first);
                    e.out += ':';
                    if (auto err = encode(entries[i].second, depth + 1)) return err;
                }
                e.out += '}';
                return nullptr;
            }
            case Kind::Struct: {
                e.out += '{';
                bool first = true;
                for (const auto& f : v.Type()->fields) {
                    if (!first) e.out += ',';
                    first = false;
                    e.WriteString(f.name);
                    e.out += ':';
                    if (auto err = encode(f.get(v.ptr(), v.owner()), depth + 1)) return err;
                }
                e.out += '}';
                return nullptr;
            }
            case Kind::Pointer:
            case Kind::Interface:
                if (v.IsNil()) {
                    e.out += "null";
                    return nullptr;
                }
                return encode(v.Elem(), depth + 1);
            case Kind::Complex:
            case Kind::Func:
                return unsupportedType(v);
        }
        return unsupportedType(v);
    }
};

std::pair<std::string, gopp::error> marshal(const Value& v) {
    jsonEncoder je;
    if (auto err = je.encode(v, 0)) return {{}, err};
    return {std::move(je.e.out), nullptr};
}

// Escapes a value as a JavaScript expression: JSON, padded with spaces
// where it could run into adjacent identifiers.
std::string jsValEscaper(std::span<const Value> args) {
    Value a;
    if (args.size() == 1) {
        a = indirectToJSONMarshaler(args[0]);
        contentType t = a.IsValid() && a.Kind() == Kind::String ? contentOf(a) : contentTypePlain;
        if (t == contentTypeJS) return a.Str().str();
        if (t == contentTypeJSStr) {
            // TODO: normalize quotes.
            return "\"" + a.Str().str() + "\"";
        }
        if (a.IsValid() && hasMethod(a, "MarshalJSON")) {
            // Do not treat as a Stringer.
        } else if (a.IsValid() && hasMethod(a, "String")) {
            a = stringValue(ttmpl::Sprint(std::span(&a, 1)).str());
        }
    } else {
        std::vector<Value> as;
        for (const auto& arg : args) as.push_back(indirectToJSONMarshaler(arg));
        a = stringValue(ttmpl::Sprint(as).str());
    }
    // TODO: detect cycles before calling Marshal which loops infinitely on
    // cyclic data. This may be an unacceptable DoS risk.
    auto [b, err] = marshal(a);
    if (err) {
        // While the standard JSON marshaler does not include user
        // controlled information in the error message, if a type has a
        // MarshalJSON method, the content of the error message is not
        // guaranteed. Since we insert the error into the template, as part
        // of a comment, we attempt to prevent the error from either
        // terminating the comment, or the script block itself.
        //
        // In particular we:
        //   * replace "*/" comment end tokens with "* /", which does not
        //     terminate the comment
        //   * replace "</script" with "\x3C/script", and "<!--" with
        //     "\x3C!--", which prevents confusing script block termination
        //     semantics
        //
        // We also put a space before the comment so that if it is flush
        // against a division operator it is not turned into a line comment:
        //     x/{{y}}
        // turning into
        //     x//* error marshaling y:
        //          second line of error message */null
        std::string errStr = err.Error().str();
        errStr = scriptTagRe()->ReplaceAllString(string(errStr), R"(\x3C${1}script)").str();
        errStr = replaceAll(std::move(errStr), "*/", "* /");
        errStr = replaceAll(std::move(errStr), "<!--", R"(\x3C!--)");
        return std::format(" /* {} */null ", errStr);
    }

    // TODO: maybe post-process output to prevent it from containing
    // "<!--", "-->", "<![CDATA[", "]]>", or "</script"
    // in case custom marshalers produce output containing those.
    // Note: Do not use \x escaping to save bytes because it is not JSON
    // compatible and this escaper supports ld+json content-type.
    if (b.empty()) {
        // In, `x=y/{{.}}*z` a json.Marshaler that produces "" should not
        // cause the output `x=y/*z`.
        return " null ";
    }
    rune first = utf8::DecodeRuneInString(b).first;
    rune last = utf8::DecodeLastRuneInString(b).first;
    std::string buf;
    // Prevent IdentifierNames and NumericLiterals from running into
    // keywords: in, instanceof, typeof, void
    bool pad = isJSIdentPart(first) || isJSIdentPart(last);
    if (pad) buf += ' ';
    std::size_t written = 0;
    // Make sure that json.Marshal escapes codepoints U+2028 & U+2029 so it
    // falls within the subset of JSON which is valid JS.
    for (std::size_t i = 0; i < b.size();) {
        auto [r, n] = utf8::DecodeRuneInString(std::string_view(b).substr(i));
        std::string_view repl;
        if (r == 0x2028) {
            repl = R"(\u2028)";
        } else if (r == 0x2029) {
            repl = R"(\u2029)";
        }
        if (!repl.empty()) {
            buf += std::string_view(b).substr(written, i - written);
            buf += repl;
            written = i + std::size_t(n);
        }
        i += std::size_t(n);
    }
    if (!buf.empty()) {
        buf += std::string_view(b).substr(written);
        if (pad) buf += ' ';
        return buf;
    }
    return b;
}

// Escapes for inclusion in a JavaScript string literal.
std::string jsStrEscaper(std::span<const Value> args) {
    auto [s, t] = stringify(args);
    if (t == contentTypeJSStr) return replace(s, jsStrNormReplacementTable);
    return replace(s, jsStrReplacementTable);
}

// Escapes for inclusion in a JavaScript template literal.
std::string jsTmplLitEscaper(std::span<const Value> args) {
    auto [s, _] = stringify(args);
    return replace(s, jsBqStrReplacementTable);
}

// Escapes for inclusion in a JavaScript regexp literal: a literal match of
// the text.
std::string jsRegexpEscaper(std::span<const Value> args) {
    auto [s, _] = stringify(args);
    s = replace(s, jsRegexpReplacementTable);
    if (s.empty()) {
        // /{{.X}}/ should not produce a line comment when .X == "".
        return "(?:)";
    }
    return s;
}

// ---------------------------------------------------------------------------
// css.go

constexpr auto cssReplacementTable = makeTable<128>({
    {0, R"(\0)"},
    {'\t', R"(\9)"},
    {'\n', R"(\a)"},
    {'\f', R"(\c)"},
    {'\r', R"(\d)"},
    // Encode HTML specials as hex so the output can be embedded in HTML
    // attributes without further encoding.
    {'"', R"(\22)"},
    {'&', R"(\26)"},
    {'\'', R"(\27)"},
    {'(', R"(\28)"},
    {')', R"(\29)"},
    {'+', R"(\2b)"},
    {'/', R"(\2f)"},
    {':', R"(\3a)"},
    {';', R"(\3b)"},
    {'<', R"(\3c)"},
    {'>', R"(\3e)"},
    {'\\', R"(\\)"},
    {'{', R"(\7b)"},
    {'}', R"(\7d)"},
});

bool isCSSSpace(char b) {
    switch (b) {
        case '\t': case '\n': case '\f': case '\r': case ' ':
            return true;
        default:
            return false;
    }
}

// Escapes HTML and CSS special characters using \<hex>+ escapes.
std::string cssEscaper(std::span<const Value> args) {
    auto [s, _] = stringify(args);
    std::string b;
    std::size_t written = 0;
    for (std::size_t i = 0, w = 0; i < s.size(); i += w) {
        auto [r, rw] = utf8::DecodeRuneInString(std::string_view(s).substr(i));
        w = std::size_t(rw);
        std::string_view repl;
        if (std::size_t(r) < cssReplacementTable.size() && !cssReplacementTable[std::size_t(r)].empty()) {
            repl = cssReplacementTable[std::size_t(r)];
        } else {
            continue;
        }
        if (written == 0) b.reserve(s.size());
        b += std::string_view(s).substr(written, i - written);
        b += repl;
        written = i + w;
        if (repl != R"(\\)" && (written == s.size() || isHex(s[written]) || isCSSSpace(s[written]))) b += ' ';
    }
    if (written == 0) return s;
    b += std::string_view(s).substr(written);
    return b;
}

// Allows innocuous CSS values in the output including CSS quantities
// (10px or 25%), ID or class literals (#foo, .bar), keyword values
// (inherit, blue), and colors (#888). It filters out unsafe values, such
// as those that affect token boundaries, and anything that might execute
// scripts.
std::string cssValueFilter(std::span<const Value> args) {
    auto [s, t] = stringify(args);
    if (t == contentTypeCSS) return s;
    std::string b = decodeCSS(s);
    std::string id;
    for (std::size_t i = 0; i < b.size(); i++) {
        char c = b[i];
        switch (c) {
            case 0: case '"': case '\'': case '(': case ')': case '/': case ';': case '@': case '[': case '\\':
            case ']': case '`': case '{': case '}': case '<': case '>':
                return std::string(filterFailsafe);
            case '-':
                // Disallow <!-- or -->.
                // -- should not appear in valid identifiers.
                if (i != 0 && b[i - 1] == '-') return std::string(filterFailsafe);
                break;
            default:
                if (byte(c) < utf8::RuneSelf && isCSSNmchar(rune(c))) id += c;
        }
    }
    for (char& c : id) {
        if ('A' <= c && c <= 'Z') c = char(c + 'a' - 'A');
    }
    if (id.find("expression") != id.npos || id.find("mozbinding") != id.npos) return std::string(filterFailsafe);
    return b;
}

// ---------------------------------------------------------------------------
// url.go

// Whether s is a relative URL or one with a safe scheme: http, https or
// mailto.
bool isSafeURL(std::string_view s) {
    if (std::size_t colon = s.find(':'); colon != s.npos) {
        std::string_view protocol = s.substr(0, colon);
        if (protocol.find('/') == protocol.npos) {
            auto eqFold = [](std::string_view a, std::string_view b) {
                return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
                           return std::tolower(byte(x)) == std::tolower(byte(y));
                       });
            };
            if (!eqFold(protocol, "http") && !eqFold(protocol, "https") && !eqFold(protocol, "mailto")) return false;
        }
    }
    return true;
}

// Returns its input unless it contains an unsafe scheme, in which case it
// defangs the entire URL.
//
// Schemes that cause unintended side effects that are irreversible
// without user interaction are considered unsafe. For example, clicking
// on a "javascript:" link can immediately trigger JavaScript code
// execution.
//
// This filter conservatively assumes that all schemes other than the
// following are unsafe:
//   - http:   Navigates to a new website, and may open a new window or
//     tab. These side effects can be reversed by navigating back to the
//     previous website, or closing the window or tab. No irreversible
//     changes will take place without further user interaction with the
//     new website.
//   - https:  Same as http.
//   - mailto: Opens an email program and starts a new draft. This side
//     effect is not irreversible until the user explicitly clicks send;
//     it can be undone by closing the email program.
//
// To allow URLs containing other schemes to bypass this filter, developers
// must explicitly indicate that such a URL is expected and safe by
// encapsulating it in a template.URL value.
std::string urlFilter(std::span<const Value> args) {
    auto [s, t] = stringify(args);
    if (t == contentTypeURL) return s;
    if (!isSafeURL(s)) return "#" + std::string(filterFailsafe);
    return s;
}

// Appends s to b, %-encoding the bytes that need it: those that aren't
// unreserved characters (RFC 3986), and, unless norm, the reserved ones
// too; with norm, existing %-escapes are kept. Whether anything was
// encoded.
bool processURLOnto(std::string_view s, bool norm, std::string& b) {
    b.reserve(b.size() + s.size() + 16);
    std::size_t written = 0;
    // The byte loop below assumes that all URLs use UTF-8 as the
    // content-encoding. This is similar to the URI to IRI encoding scheme
    // defined in section 3.1 of RFC 3987, and behaves the same as the
    // EcmaScript builtin encodeURIComponent.
    // It should not cause any misencoding of URLs in pages with
    // Content-type: text/html;charset=UTF-8.
    for (std::size_t i = 0, n = s.size(); i < n; i++) {
        char c = s[i];
        switch (c) {
            // Single quote and parens are sub-delims in RFC 3986, but we
            // escape them so the output can be embedded in single quoted
            // attributes and unquoted CSS url(...) constructs. Single
            // quotes are reserved in URLs, but are only used in the
            // obsolete "mark" rule in an appendix in RFC 3986 so can be
            // safely encoded.
            case '!': case '#': case '$': case '&': case '*': case '+': case ',': case '/': case ':': case ';':
            case '=': case '?': case '@': case '[': case ']':
                if (norm) continue;
                break;
            // Unreserved according to RFC 3986 sec 2.3
            // "For consistency, percent-encoded octets in the ranges of
            // ALPHA (%41-%5A and %61-%7A), DIGIT (%30-%39), hyphen (%2D),
            // period (%2E), underscore (%5F), or tilde (%7E) should not be
            // created by URI producers
            case '-': case '.': case '_': case '~':
                continue;
            case '%':
                // When normalizing do not re-encode valid escapes.
                if (norm && i + 2 < s.size() && isHex(s[i + 1]) && isHex(s[i + 2])) continue;
                break;
            default:
                // Unreserved according to RFC 3986 sec 2.3
                if ('a' <= c && c <= 'z') continue;
                if ('A' <= c && c <= 'Z') continue;
                if ('0' <= c && c <= '9') continue;
        }
        b += s.substr(written, i - written);
        b += std::format("%{:02x}", byte(c));
        written = i + 1;
    }
    b += s.substr(written);
    return written != 0;
}

// Escapes a URL or part of one: normalizes it if norm (or it's a URL
// value), %-encodes the reserved characters otherwise.
std::string urlProcessor(bool norm, std::span<const Value> args) {
    auto [s, t] = stringify(args);
    if (t == contentTypeURL) norm = true;
    std::string b;
    if (processURLOnto(s, norm, b)) return b;
    return s;
}

// Produces an output that can be embedded in a URL query. The output can
// be embedded in an HTML attribute without further escaping.
std::string urlEscaper(std::span<const Value> args) { return urlProcessor(false, args); }

// Normalizes URL content so it can be embedded in a quote-delimited
// string or parenthesis delimited url(...). The normalizer does not
// encode all HTML specials. Specifically, it does not encode '&' so
// correct embedding in an HTML attribute requires escaping of '&' to
// '&amp;'.
std::string urlNormalizer(std::span<const Value> args) { return urlProcessor(true, args); }

// The bytes \t, \n, \f, \r, space and the ASCII letters and digits, as a
// bit set (Go's htmlSpaceAndASCIIAlnumBytes).
constexpr char htmlSpaceAndASCIIAlnumBytes[] = "\x00\x36\x00\x00\x01\x00\xff\x03\xfe\xff\xff\x07\xfe\xff\xff\x07";

// Whether c is an HTML space: \t, \n, \f, \r or space.
bool isHTMLSpace(char c) {
    auto u = byte(c);
    return u <= 0x20 && (byte(htmlSpaceAndASCIIAlnumBytes[u >> 3]) & (1 << (u & 0x7))) != 0;
}

bool isHTMLSpaceOrASCIIAlnum(char c) {
    auto u = byte(c);
    return u < 0x80 && (byte(htmlSpaceAndASCIIAlnumBytes[u >> 3]) & (1 << (u & 0x7))) != 0;
}

// Appends the srcset element s[left:right] to b: its URL normalized if
// it's safe and its metadata innocuous, #ZgotmplZ otherwise.
void filterSrcsetElement(std::string_view s, std::size_t left, std::size_t right, std::string& b) {
    std::size_t start = left;
    while (start < right && isHTMLSpace(s[start])) start++;
    std::size_t end = right;
    for (std::size_t i = start; i < right; i++) {
        if (isHTMLSpace(s[i])) {
            end = i;
            break;
        }
    }
    if (std::string_view url = s.substr(start, end - start); isSafeURL(url)) {
        // If image metadata is only spaces or alnums then we don't need to
        // URL normalize it.
        bool metadataOk = true;
        for (std::size_t i = end; i < right; i++) {
            if (!isHTMLSpaceOrASCIIAlnum(s[i])) {
                metadataOk = false;
                break;
            }
        }
        if (metadataOk) {
            b += s.substr(left, start - left);
            processURLOnto(url, true, b);
            b += s.substr(end, right - end);
            return;
        }
    }
    b += "#";
    b += filterFailsafe;
}

// Filters and normalizes srcset values, which are comma separated URLs
// followed by metadata.
std::string srcsetFilterAndEscaper(std::span<const Value> args) {
    auto [s, t] = stringify(args);
    switch (t) {
        case contentTypeSrcset:
            return s;
        case contentTypeURL: {
            // Normalizing gets rid of all HTML whitespace which separate
            // the image URL from its metadata.
            std::string b;
            if (processURLOnto(s, true, b)) s = b;
            // Additionally, commas separate one source from another.
            return replaceAll(std::move(s), ",", "%2c");
        }
        default:
            break;
    }

    std::string b;
    std::size_t written = 0;
    for (std::size_t i = 0; i < s.size(); i++) {
        if (s[i] == ',') {
            filterSrcsetElement(s, written, i, b);
            b += ",";
            written = i + 1;
        }
    }
    filterSrcsetElement(s, written, s.size(), b);
    return b;
}

ttmpl::Func escaperFunc(std::string (*f)(std::span<const Value>)) {
    return ttmpl::Func::Raw(1, true, [f](std::span<const Value> args) -> std::pair<Value, gopp::error> {
        return {stringValue(f(args)), nullptr};
    });
}

}  // namespace

const FuncMap& funcMap() {
    static const FuncMap m = {
        {"_html_template_attrescaper", escaperFunc(attrEscaper)},
        {"_html_template_commentescaper", escaperFunc(commentEscaper)},
        {"_html_template_cssescaper", escaperFunc(cssEscaper)},
        {"_html_template_cssvaluefilter", escaperFunc(cssValueFilter)},
        {"_html_template_htmlnamefilter", escaperFunc(htmlNameFilter)},
        {"_html_template_htmlescaper", escaperFunc(htmlEscaper)},
        {"_html_template_jsregexpescaper", escaperFunc(jsRegexpEscaper)},
        {"_html_template_jsstrescaper", escaperFunc(jsStrEscaper)},
        {"_html_template_jstmpllitescaper", escaperFunc(jsTmplLitEscaper)},
        {"_html_template_jsvalescaper", escaperFunc(jsValEscaper)},
        {"_html_template_nospaceescaper", escaperFunc(htmlNospaceEscaper)},
        {"_html_template_rcdataescaper", escaperFunc(rcdataEscaper)},
        {"_html_template_srcsetescaper", escaperFunc(srcsetFilterAndEscaper)},
        {"_html_template_urlescaper", escaperFunc(urlEscaper)},
        {"_html_template_urlfilter", escaperFunc(urlFilter)},
        {"_html_template_urlnormalizer", escaperFunc(urlNormalizer)},
        {"_eval_args_", escaperFunc(evalArgs)},
    };
    return m;
}

}  // namespace gopp::html::tmpl::detail
