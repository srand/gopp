// Go's fmt printing (Sprint, Sprintln, Sprintf) on template values, after
// Go's fmt/print.go and format.go: what the print, println and printf
// functions and {{.}} output need.
#include "goconv.hpp"

#include <gopp/text/template.hpp>

#include <algorithm>
#include <cstdio>

namespace gopp::text::tmpl {

namespace {

constexpr int max_depth = 100;  // composites nested deeper print as "..."

struct printer {
    std::string buf;
    bool plus = false, minus = false, sharp = false, space = false, zero = false;
    bool plus_v = false, sharp_v = false;
    bool wid_present = false, prec_present = false;
    int wid = 0, prec = 0;

    void clearFlags() {
        plus = minus = sharp = space = zero = plus_v = sharp_v = false;
        wid_present = prec_present = false;
        wid = prec = 0;
    }

    // Pads s to the width.
    void pad(std::string_view s) {
        if (!wid_present || wid == 0) {
            buf += s;
            return;
        }
        int width = wid - static_cast<int>(detail::runeCount(s));
        if (width <= 0) {
            buf += s;
            return;
        }
        if (minus) {
            buf += s;
            buf.append(static_cast<std::size_t>(width), ' ');
        } else {
            buf.append(static_cast<std::size_t>(width), zero ? '0' : ' ');
            buf += s;
        }
    }

    void fmtBool(bool b) { pad(b ? "true" : "false"); }

    void fmtInteger(uint64 u, int base, bool is_signed, char verb) {
        bool negative = is_signed && static_cast<int64>(u) < 0;
        if (negative) u = 0 - u;
        std::string digits;
        const char* d = verb == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
        if (u == 0) digits = "0";
        while (u > 0) {
            digits.push_back(d[u % static_cast<uint64>(base)]);
            u /= static_cast<uint64>(base);
        }
        // Precision: minimum digits.
        int precision = prec_present ? prec : 1;
        if (prec_present && prec == 0 && digits == "0") digits.clear();
        while (static_cast<int>(digits.size()) < precision) digits.push_back('0');
        // Zero padding to the width.
        bool zero_pad = zero && wid_present && !minus && !prec_present;
        int width = wid;
        if (zero_pad) {
            int extra = (negative || plus || space) ? 1 : 0;
            if (sharp && base == 16) extra += 2;
            if (sharp && (base == 8 || base == 2)) extra += base == 8 ? 0 : 2;
            while (static_cast<int>(digits.size()) < width - extra) digits.push_back('0');
        }
        if (sharp) {
            if (base == 8 && digits.back() != '0') digits.push_back('0');
            if (base == 16) digits += verb == 'X' ? "X0" : "x0";
            if (base == 2) digits += "b0";
        }
        if (verb == 'O') digits += "o0";
        if (negative) digits.push_back('-');
        else if (plus) digits.push_back('+');
        else if (space) digits.push_back(' ');
        std::reverse(digits.begin(), digits.end());
        bool z = zero;
        zero = false;
        pad(digits);
        zero = z;
    }

    void fmtUnicode(uint64 u) {
        char b[32];
        std::snprintf(b, sizeof(b), "U+%04llX", static_cast<unsigned long long>(u));
        std::string s = b;
        if (sharp && u <= static_cast<uint64>(detail::max_rune) && detail::isPrint(static_cast<detail::rune>(u))) {
            s += " '";
            detail::appendRune(s, static_cast<detail::rune>(u));
            s += "'";
        }
        pad(s);
    }

    void fmtC(uint64 u) {
        std::string s;
        detail::appendRune(s, u > static_cast<uint64>(detail::max_rune) ? detail::rune_error : static_cast<detail::rune>(u));
        pad(s);
    }

    void fmtQc(uint64 u) {
        detail::rune r = u > static_cast<uint64>(detail::max_rune) ? detail::rune_error : static_cast<detail::rune>(u);
        pad(detail::quoteRune(r, plus));
    }

    void fmtFloat(double f, char verb, int default_prec) {
        int p = prec_present ? prec : default_prec;
        std::string s = detail::formatFloat(f, verb == 'v' ? 'g' : verb == 'F' ? 'f' : verb, p);
        if (s[0] != '-' && s[0] != '+') {
            if (plus) s = "+" + s;
            else if (space) s = " " + s;
        }
        if (s == "+Inf" && !plus) s = "Inf";
        if (sharp && verb != 'b') {
            // %#g keeps trailing zeros and the point; minimal support.
            if (s.find('.') == std::string::npos && s.find_first_of("eEIN") == std::string::npos) s += ".";
        }
        if (zero && wid_present && !minus && (s[0] == '-' || s[0] == '+' || s[0] == ' ') &&
            s.find_first_of("IN") == std::string::npos) {
            // Sign before the zeros.
            char sign = s[0];
            std::string rest = s.substr(1);
            int width = wid - 1;
            std::string padded = rest;
            while (static_cast<int>(detail::runeCount(padded)) < width) padded.insert(padded.begin(), '0');
            buf.push_back(sign);
            buf += padded;
            return;
        }
        if (s.find_first_of("IN") != std::string::npos) {
            bool z = zero;
            zero = false;
            pad(s);
            zero = z;
            return;
        }
        pad(s);
    }

    void fmtComplex(std::complex<double> c, char verb) {
        bool old_plus = plus;
        buf.push_back('(');
        fmtFloat(c.real(), verb, -1);
        plus = true;
        fmtFloat(c.imag(), verb, -1);
        buf += "i)";
        plus = old_plus;
    }

    std::string truncate(std::string_view s) {
        if (!prec_present) return std::string(s);
        std::size_t i = 0;
        for (int n = 0; n < prec && i < s.size(); n++) i += static_cast<std::size_t>(detail::decodeRune(s, i).second);
        return std::string(s.substr(0, i));
    }

    void fmtS(std::string_view s) { pad(truncate(s)); }

    void fmtSbx(std::string_view s, const char* digits) {
        std::string out;
        std::size_t length = s.size();
        if (prec_present && prec < static_cast<int>(length)) length = static_cast<std::size_t>(prec);
        for (std::size_t i = 0; i < length; i++) {
            if (space && i > 0) out.push_back(' ');
            if (sharp && (space || i == 0)) out += digits[10] == 'A' ? "0X" : "0x";
            unsigned char c = static_cast<unsigned char>(s[i]);
            out.push_back(digits[c >> 4]);
            out.push_back(digits[c & 0xF]);
        }
        pad(out);
    }

    void fmtQ(std::string_view s) {
        std::string t = truncate(s);
        if (sharp && t.find('`') == std::string::npos) {
            bool can_backquote = true;
            for (std::size_t i = 0; i < t.size();) {
                auto [r, w] = detail::decodeRune(t, i);
                if ((r == detail::rune_error && w == 1) || (r < ' ' && r != '\t') || r == 0x7F || r == 0xFEFF) {
                    can_backquote = false;
                    break;
                }
                i += static_cast<std::size_t>(w);
            }
            if (can_backquote) {
                pad("`" + t + "`");
                return;
            }
        }
        pad(detail::quote(t, plus));
    }

    void fmtString(std::string_view s, char verb) {
        switch (verb) {
        case 'v':
            if (sharp_v) fmtQ(s);
            else fmtS(s);
            break;
        case 's': fmtS(s); break;
        case 'x': fmtSbx(s, "0123456789abcdef"); break;
        case 'X': fmtSbx(s, "0123456789ABCDEF"); break;
        case 'q': fmtQ(s); break;
        default: badVerb(verb, nullptr); break;
        }
    }

    void badVerb(char verb, const Value* v) {
        buf += "%!";
        buf.push_back(verb);
        buf.push_back('(');
        if (v && v->IsValid()) {
            buf += v->TypeName();
            buf.push_back('=');
            printer p2;
            p2.print_value(*v, 'v', 0);
            buf += p2.buf;
        } else {
            buf += "<nil>";
        }
        buf.push_back(')');
    }

    // String() or Error(), for the verbs that take them.
    bool handleMethods(const Value& v, char verb) {
        if (sharp_v) return false;
        if (verb != 'v' && verb != 's' && verb != 'x' && verb != 'X' && verb != 'q') return false;
        Value recv = v;
        const detail::method_info* m = nullptr;
        for (int i = 0; i < 4 && recv.IsValid(); i++) {
            m = recv.MethodByName("Error");
            if (!m) m = recv.MethodByName("String");
            if (m && m->in.empty() && !m->variadic) break;
            m = nullptr;
            if (recv.Kind() != Kind::Pointer || recv.IsNil()) break;
            recv = recv.Elem();
        }
        if (!m) return false;
        auto [r, err] = m->call(recv.ptr(), recv.owner(), {});
        if (err) {
            buf += "%!";
            buf.push_back(verb);
            buf += "(PANIC=" + std::string(m->name) + " method: " + err.Error().str() + ")";
            return true;
        }
        if (r.Kind() != Kind::String) return false;
        fmtString(r.Str(), verb);
        return true;
    }

    void print_value(const Value& v0, char verb, int depth) {
        Value v = v0;
        if (depth > 0 && v.IsValid() && handleMethods(v, verb)) return;
        if (depth > max_depth) {
            buf += "...";
            return;
        }
        switch (v.Kind()) {
        case Kind::Invalid:
            if (depth == 0) buf += "<invalid reflect.Value>";
            else if (verb == 'v') buf += "<nil>";
            else badVerb(verb, nullptr);
            return;
        case Kind::Bool:
            if (verb == 't' || verb == 'v') fmtBool(v.Bool());
            else badVerb(verb, &v);
            return;
        case Kind::Int: printInt(static_cast<uint64>(v.Int()), true, verb, &v); return;
        case Kind::Uint: printInt(v.Uint(), false, verb, &v); return;
        case Kind::Float:
            switch (verb) {
            case 'v': fmtFloat(v.Float(), 'g', -1); return;
            case 'b': case 'g': case 'G': case 'x': case 'X': fmtFloat(v.Float(), verb, -1); return;
            case 'f': case 'e': case 'E': case 'F': fmtFloat(v.Float(), verb, 6); return;
            default: badVerb(verb, &v); return;
            }
        case Kind::Complex:
            switch (verb) {
            case 'v': case 'b': case 'g': case 'G': case 'x': case 'X': case 'f': case 'F': case 'e': case 'E':
                fmtComplex(v.Complex(), verb == 'v' ? 'g' : verb);
                return;
            default: badVerb(verb, &v); return;
            }
        case Kind::String:
            if (verb == 'v' || verb == 's' || verb == 'x' || verb == 'X' || verb == 'q') fmtString(v.Str(), verb);
            else badVerb(verb, &v);
            return;
        case Kind::Map: {
            if (sharp_v) buf += std::string(v.TypeName()) + "{";
            else buf += "map[";
            bool first = true;
            for (auto& [k, e] : v.MapEntries()) {
                if (!first) buf += sharp_v ? ", " : " ";
                first = false;
                print_value(k, verb, depth + 1);
                buf.push_back(':');
                print_value(e, verb, depth + 1);
            }
            buf += sharp_v ? "}" : "]";
            return;
        }
        case Kind::Struct: {
            if (sharp_v) buf += v.TypeName();
            buf.push_back('{');
            bool first = true;
            for (auto& f : v.Type()->fields) {
                if (!first) buf += sharp_v ? ", " : " ";
                first = false;
                if (plus_v || sharp_v) {
                    buf += f.name;
                    buf.push_back(':');
                }
                print_value(f.get(v.ptr(), v.owner()), verb, depth + 1);
            }
            buf.push_back('}');
            return;
        }
        case Kind::Interface: {
            Value e = v.Elem();
            if (!e.IsValid()) {
                if (sharp_v) buf += std::string(v.TypeName()) + "(nil)";
                else buf += "<nil>";
                return;
            }
            print_value(e, verb, depth + 1);
            return;
        }
        case Kind::Array:
        case Kind::Slice: {
            // []byte with %s, %q, %x, %X prints as a string.
            if ((verb == 's' || verb == 'q' || verb == 'x' || verb == 'X') && v.Type()->elem &&
                v.Type()->elem()->name == "uint8") {
                std::string bytes;
                for (std::size_t i = 0; i < v.Len(); i++) bytes.push_back(static_cast<char>(v.Index(i).Uint()));
                fmtString(bytes, verb);
                return;
            }
            if (sharp_v) {
                buf += std::string(v.TypeName());
                if (v.Kind() == Kind::Slice && v.IsNil()) {
                    buf += "(nil)";
                    return;
                }
                buf.push_back('{');
                for (std::size_t i = 0; i < v.Len(); i++) {
                    if (i > 0) buf += ", ";
                    print_value(v.Index(i), verb, depth + 1);
                }
                buf.push_back('}');
                return;
            }
            buf.push_back('[');
            for (std::size_t i = 0; i < v.Len(); i++) {
                if (i > 0) buf.push_back(' ');
                print_value(v.Index(i), verb, depth + 1);
            }
            buf.push_back(']');
            return;
        }
        case Kind::Pointer:
            // A pointer to a composite at the top prints as &{...}.
            if (depth == 0 && !v.IsNil()) {
                Value e = v.Elem();
                switch (e.Kind()) {
                case Kind::Array: case Kind::Slice: case Kind::Struct: case Kind::Map:
                    buf.push_back('&');
                    print_value(e, verb, depth + 1);
                    return;
                default:
                    break;
                }
            }
            fmtPointer(v, verb);
            return;
        case Kind::Func:
            fmtPointer(v, verb);
            return;
        }
    }

    void printInt(uint64 u, bool is_signed, char verb, const Value* v) {
        switch (verb) {
        case 'v':
            if (sharp_v && !is_signed) fmtInteger(u, 16, false, 'x');  // Go: %#v of uint is 0x...
            else fmtInteger(u, 10, is_signed, verb);
            break;
        case 'd': fmtInteger(u, 10, is_signed, verb); break;
        case 'b': fmtInteger(u, 2, is_signed, verb); break;
        case 'o': case 'O': fmtInteger(u, 8, is_signed, verb); break;
        case 'x': fmtInteger(u, 16, is_signed, verb); break;
        case 'X': fmtInteger(u, 16, is_signed, verb); break;
        case 'c': fmtC(u); break;
        case 'q': fmtQc(u); break;
        case 'U': fmtUnicode(u); break;
        default: badVerb(verb, v); break;
        }
    }

    void fmtPointer(const Value& v, char verb) {
        uintptr_t u = v.IsNil() ? 0 : reinterpret_cast<uintptr_t>(v.ptr());
        switch (verb) {
        case 'v':
            if (u == 0) {
                pad("<nil>");
            } else {
                char b[32];
                std::snprintf(b, sizeof(b), "0x%llx", static_cast<unsigned long long>(u));
                pad(b);
            }
            break;
        case 'p': {
            char b[32];
            std::snprintf(b, sizeof(b), "0x%llx", static_cast<unsigned long long>(u));
            pad(b);
            break;
        }
        case 'b': case 'o': case 'd': case 'x': case 'X': fmtInteger(u, verb == 'b' ? 2 : verb == 'o' ? 8 : verb == 'd' ? 10 : 16, false, verb); break;
        default: badVerb(verb, &v); break;
        }
    }

    void printArg(const Value& arg, char verb) {
        if (!arg.IsValid()) {
            switch (verb) {
            case 'T': case 'v': pad("<nil>"); break;
            default: badVerb(verb, nullptr); break;
            }
            return;
        }
        if (verb == 'T') {
            fmtS(arg.TypeName());
            return;
        }
        if (verb == 'p') {
            fmtPointer(arg, 'p');
            return;
        }
        // Interfaces print as what they hold.
        Value v = arg;
        if (v.Kind() == Kind::Interface) {
            v = v.Elem();
            if (!v.IsValid()) {
                if (verb == 'v' || verb == 's') pad("<nil>");
                else badVerb(verb, nullptr);
                return;
            }
        }
        if (handleMethods(v, verb)) return;
        print_value(v, verb, 0);
    }
};

}  // namespace

string Sprint(std::span<const Value> args) {
    printer p;
    bool prev_string = false;
    for (std::size_t i = 0; i < args.size(); i++) {
        bool is_string = args[i].IsValid() && args[i].Kind() == Kind::String;
        // Add a space between two non-string arguments.
        if (i > 0 && !is_string && !prev_string) p.buf.push_back(' ');
        p.printArg(args[i], 'v');
        prev_string = is_string;
    }
    return string(p.buf);
}

string Sprintln(std::span<const Value> args) {
    printer p;
    for (std::size_t i = 0; i < args.size(); i++) {
        if (i > 0) p.buf.push_back(' ');
        p.printArg(args[i], 'v');
    }
    p.buf.push_back('\n');
    return string(p.buf);
}

string Sprintf(std::string_view format, std::span<const Value> args) {
    printer p;
    std::size_t arg_num = 0;
    std::size_t end = format.size();
    for (std::size_t i = 0; i < end;) {
        std::size_t lasti = i;
        while (i < end && format[i] != '%') i++;
        if (i > lasti) p.buf.append(format.substr(lasti, i - lasti));
        if (i >= end) break;
        i++;  // skip %
        p.clearFlags();
        // Flags.
        for (; i < end; i++) {
            char c = format[i];
            if (c == '#') p.sharp = true;
            else if (c == '0') p.zero = true;
            else if (c == '+') p.plus = true;
            else if (c == '-') p.minus = true;
            else if (c == ' ') p.space = true;
            else break;
        }
        // Width.
        auto parse_num = [&](int& out) {
            bool any = false;
            out = 0;
            while (i < end && format[i] >= '0' && format[i] <= '9') {
                out = out * 10 + (format[i] - '0');
                i++;
                any = true;
            }
            return any;
        };
        if (i < end && format[i] == '*') {
            i++;
            if (arg_num < args.size() && (args[arg_num].Kind() == Kind::Int || args[arg_num].Kind() == Kind::Uint)) {
                p.wid = static_cast<int>(args[arg_num].Int());
                p.wid_present = true;
                if (p.wid < 0) {
                    p.minus = true;
                    p.wid = -p.wid;
                }
            } else {
                p.buf += "%!(BADWIDTH)";
            }
            arg_num++;
        } else {
            p.wid_present = parse_num(p.wid);
        }
        if (i < end && format[i] == '.') {
            i++;
            if (i < end && format[i] == '*') {
                i++;
                if (arg_num < args.size() && (args[arg_num].Kind() == Kind::Int || args[arg_num].Kind() == Kind::Uint)) {
                    p.prec = static_cast<int>(args[arg_num].Int());
                    p.prec_present = p.prec >= 0;
                } else {
                    p.buf += "%!(BADPREC)";
                }
                arg_num++;
            } else {
                parse_num(p.prec);
                p.prec_present = true;
            }
        }
        if (p.minus) p.zero = false;  // only allow zero padding to the left
        if (i >= end) {
            p.buf += "%!(NOVERB)";
            break;
        }
        auto [verb_rune, w] = detail::decodeRune(format, i);
        i += static_cast<std::size_t>(w);
        char verb = verb_rune < 0x80 ? static_cast<char>(verb_rune) : '?';
        if (verb == '%') {
            p.buf.push_back('%');
            continue;
        }
        if (arg_num >= args.size()) {
            p.buf += "%!";
            detail::appendRune(p.buf, verb_rune);
            p.buf += "(MISSING)";
            continue;
        }
        if (verb == 'v') {
            if (p.sharp) {
                p.sharp = false;
                p.sharp_v = true;
            }
            if (p.plus) {
                p.plus = false;
                p.plus_v = true;
            }
        }
        if (verb == 'w') verb = 'v';
        p.printArg(args[arg_num], verb);
        arg_num++;
    }
    if (arg_num < args.size()) {
        p.clearFlags();
        p.buf += "%!(EXTRA ";
        for (std::size_t k = arg_num; k < args.size(); k++) {
            if (k > arg_num) p.buf += ", ";
            if (!args[k].IsValid()) {
                p.buf += "<nil>";
            } else {
                p.buf += args[k].TypeName();
                p.buf.push_back('=');
                p.printArg(args[k], 'v');
            }
        }
        p.buf.push_back(')');
    }
    return string(p.buf);
}

}  // namespace gopp::text::tmpl
