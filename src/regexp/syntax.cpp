// The regexp/syntax package, ported from Go 1.27.3's regexp/syntax:
// regexp.go, parse.go, perl_groups.go, simplify.go, prog.go and compile.go.
#include <gopp/regexp/syntax.hpp>
#include <gopp/strings.hpp>
#include <gopp/unicode.hpp>
#include <gopp/unicode/utf8.hpp>

#include <algorithm>
#include <cstring>
#include <map>
#include <optional>
#include <unordered_map>

namespace gopp::syntax {

string Error::message() const { return "error parsing regexp: " + string(Code) + ": `" + Expr + "`"; }

string String(Op op) {
    static constexpr std::string_view names[] = {
        "",          "NoMatch", "EmptyMatch", "Literal",     "CharClass", "AnyCharNotNL", "AnyChar",
        "BeginLine", "EndLine", "BeginText",  "EndText",     "WordBoundary", "NoWordBoundary", "Capture",
        "Star",      "Plus",    "Quest",      "Repeat",      "Concat",    "Alternate",
    };
    if (op >= 1 && op <= OpAlternate) return string(names[op]);
    if (op == 128) return string::fromStatic("opPseudo");
    return "Op(" + strconv::Itoa(op) + ")";
}

string String(InstOp op) {
    static constexpr std::string_view names[] = {
        "InstAlt", "InstAltMatch", "InstCapture", "InstEmptyWidth", "InstMatch",        "InstFail",
        "InstNop", "InstRune",     "InstRune1",   "InstRuneAny",    "InstRuneAnyNotNL",
    };
    if (op <= InstRuneAnyNotNL) return string(names[op]);
    return {};
}

namespace {

constexpr rune minFold = 0x0041;
constexpr rune maxFold = 0x1e943;

constexpr Op opPseudo = Op(128);  // where pseudo-ops start
constexpr Op opLeftParen = Op(128);
constexpr Op opVerticalBar = Op(129);

void writeRune(std::string& b, rune r) {
    char buf[4];
    auto n = utf8::EncodeRune(std::span<byte>(reinterpret_cast<byte*>(buf), 4), r);
    b.append(buf, std::size_t(n));
}

bool inCharClass(rune r, const std::vector<rune>& cls) {
    std::size_t lo = 0, hi = cls.size() / 2;
    while (lo < hi) {
        std::size_t m = (lo + hi) / 2;
        if (r > cls[2 * m + 1]) {
            lo = m + 1;
        } else if (r < cls[2 * m]) {
            hi = m;
        } else {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Printing (regexp.go)

using printFlags = uint8;
constexpr printFlags flagI = 1 << 0;     // (?i:
constexpr printFlags flagM = 1 << 1;     // (?m:
constexpr printFlags flagS = 1 << 2;     // (?s:
constexpr printFlags flagOff = 1 << 3;   // )
constexpr printFlags flagPrec = 1 << 4;  // (?: )
constexpr int negShift = 5;              // flagI<<negShift is (?-i:

using flagMap = std::unordered_map<const Regexp*, printFlags>;

void addSpan(const Regexp* start, const Regexp* last, printFlags f, flagMap& flags) {
    flags[start] = f;
    flags[last] |= flagOff;  // maybe start==last
}

// calcFlags calculates the flags to print for re: those it must have, and
// those it can't.
std::pair<printFlags, printFlags> calcFlags(const Regexp* re, flagMap& flags) {
    switch (re->Op) {
    default:
        return {0, 0};
    case OpLiteral:
        // If literal is fold-sensitive, return (flagI, 0) or (0, flagI)
        // according to whether (?i) is active. If literal is not
        // fold-sensitive, return 0, 0.
        for (rune r : re->Rune) {
            if (minFold <= r && r <= maxFold && unicode::SimpleFold(r) != r) {
                if (re->Flags & FoldCase) return {flagI, 0};
                return {0, flagI};
            }
        }
        return {0, 0};
    case OpCharClass:
        // If literal is fold-sensitive, return 0, flagI - (?i) has been
        // compiled out. If literal is not fold-sensitive, return 0, 0.
        for (std::size_t i = 0; i < re->Rune.size(); i += 2) {
            rune lo = std::max(minFold, re->Rune[i]);
            rune hi = std::min(maxFold, re->Rune[i + 1]);
            for (rune r = lo; r <= hi; r++) {
                for (rune f = unicode::SimpleFold(r); f != r; f = unicode::SimpleFold(f)) {
                    if (!(lo <= f && f <= hi) && !inCharClass(f, re->Rune)) return {0, flagI};
                }
            }
        }
        return {0, 0};
    case OpAnyCharNotNL:  // (?-s).
        return {0, flagS};
    case OpAnyChar:  // (?s).
        return {flagS, 0};
    case OpBeginLine:
    case OpEndLine:  // (?m)^ (?m)$
        return {flagM, 0};
    case OpEndText:
        if (re->Flags & WasDollar) return {0, flagM};  // (?-m)$
        return {0, 0};
    case OpCapture:
    case OpStar:
    case OpPlus:
    case OpQuest:
    case OpRepeat:
        return calcFlags(re->Sub[0].get(), flags);
    case OpConcat:
    case OpAlternate: {
        // Gather the must and cant for each subexpression. When we find a
        // conflicting subexpression, insert the necessary flags around the
        // previously identified span and start over.
        printFlags must = 0, cant = 0, allCant = 0;
        std::size_t start = 0, last = 0;
        bool did = false;
        for (std::size_t i = 0; i < re->Sub.size(); i++) {
            auto [subMust, subCant] = calcFlags(re->Sub[i].get(), flags);
            if ((must & subCant) || (subMust & cant)) {
                if (must) addSpan(re->Sub[start].get(), re->Sub[last].get(), must, flags);
                must = 0;
                cant = 0;
                start = i;
                did = true;
            }
            must |= subMust;
            cant |= subCant;
            allCant |= subCant;
            if (subMust) last = i;
            if (must == 0 && start == i) start++;
        }
        if (!did) {
            // No conflicts: pass the accumulated must and cant upward.
            return {must, cant};
        }
        if (must) {
            // Conflicts found; need to finish final span.
            addSpan(re->Sub[start].get(), re->Sub[last].get(), must, flags);
        }
        return {0, allCant};
    }
    }
}

constexpr std::string_view meta = "\\.+*?()|[]{}^$";

void escape(std::string& b, rune r, bool force) {
    if (unicode::IsPrint(r)) {
        if ((r < 0x80 && meta.find(char(r)) != std::string_view::npos) || force) b += '\\';
        writeRune(b, r);
        return;
    }
    switch (r) {
    case '\a': b += "\\a"; break;
    case '\f': b += "\\f"; break;
    case '\n': b += "\\n"; break;
    case '\r': b += "\\r"; break;
    case '\t': b += "\\t"; break;
    case '\v': b += "\\v"; break;
    default:
        if (r < 0x100) {
            b += "\\x";
            std::string s(strconv::FormatInt(r, 16).view());
            if (s.size() == 1) b += '0';
            b += s;
            break;
        }
        b += "\\x{";
        b += strconv::FormatInt(r, 16).view();
        b += '}';
    }
}

// writeRegexp writes the Perl syntax for the regular expression re to b.
void writeRegexp(std::string& b, const Regexp* re, printFlags f, flagMap& flags) {
    if (auto it = flags.find(re); it != flags.end()) f |= it->second;
    if ((f & flagPrec) && (f & ~(flagOff | flagPrec)) && (f & flagOff)) {
        // flagPrec is redundant with other flags being added and terminated.
        f &= ~flagPrec;
    }
    if (f & ~(flagOff | flagPrec)) {
        b += "(?";
        if (f & flagI) b += 'i';
        if (f & flagM) b += 'm';
        if (f & flagS) b += 's';
        if (f & ((flagM | flagS) << negShift)) {
            b += '-';
            if (f & (flagM << negShift)) b += 'm';
            if (f & (flagS << negShift)) b += 's';
        }
        b += ':';
    }
    int closes = 0;
    if (f & flagOff) closes++;
    if (f & flagPrec) {
        b += "(?:";
        closes++;
    }

    switch (re->Op) {
    default:
        b += "<invalid op" + std::to_string(int(re->Op)) + ">";
        break;
    case OpNoMatch:
        b += "[^\\x00-\\x{10FFFF}]";
        break;
    case OpEmptyMatch:
        b += "(?:)";
        break;
    case OpLiteral:
        for (rune r : re->Rune) escape(b, r, false);
        break;
    case OpCharClass: {
        const auto& R = re->Rune;
        if (R.size() % 2 != 0) {
            b += "[invalid char class]";
            break;
        }
        b += '[';
        if (R.empty()) {
            b += "^\\x00-\\x{10FFFF}";
        } else if (R[0] == 0 && R.back() == unicode::MaxRune && R.size() > 2) {
            // Contains 0 and MaxRune. Probably a negated class. Print the
            // gaps.
            b += '^';
            for (std::size_t i = 1; i + 1 < R.size(); i += 2) {
                rune lo = R[i] + 1, hi = R[i + 1] - 1;
                escape(b, lo, lo == '-');
                if (lo != hi) {
                    if (hi != lo + 1) b += '-';
                    escape(b, hi, hi == '-');
                }
            }
        } else {
            for (std::size_t i = 0; i < R.size(); i += 2) {
                rune lo = R[i], hi = R[i + 1];
                escape(b, lo, lo == '-');
                if (lo != hi) {
                    if (hi != lo + 1) b += '-';
                    escape(b, hi, hi == '-');
                }
            }
        }
        b += ']';
        break;
    }
    case OpAnyCharNotNL:
    case OpAnyChar:
        b += '.';
        break;
    case OpBeginLine: b += '^'; break;
    case OpEndLine: b += '$'; break;
    case OpBeginText: b += "\\A"; break;
    case OpEndText:
        b += (re->Flags & WasDollar) ? "$" : "\\z";
        break;
    case OpWordBoundary: b += "\\b"; break;
    case OpNoWordBoundary: b += "\\B"; break;
    case OpCapture: {
        if (!re->Name.empty()) {
            b += "(?P<";
            b += re->Name.view();
            b += '>';
        } else {
            b += '(';
        }
        const Regexp* sub = re->Sub[0].get();
        if (sub->Op != OpEmptyMatch) {
            auto it = flags.find(sub);
            writeRegexp(b, sub, it == flags.end() ? 0 : it->second, flags);
        }
        b += ')';
        break;
    }
    case OpStar:
    case OpPlus:
    case OpQuest:
    case OpRepeat: {
        printFlags p = 0;
        const Regexp* sub = re->Sub[0].get();
        if (sub->Op > OpCapture || (sub->Op == OpLiteral && sub->Rune.size() > 1)) p = flagPrec;
        writeRegexp(b, sub, p, flags);
        switch (re->Op) {
        case OpStar: b += '*'; break;
        case OpPlus: b += '+'; break;
        case OpQuest: b += '?'; break;
        default:
            b += '{';
            b += std::to_string(re->Min);
            if (re->Max != re->Min) {
                b += ',';
                if (re->Max >= 0) b += std::to_string(re->Max);
            }
            b += '}';
        }
        if (re->Flags & NonGreedy) b += '?';
        break;
    }
    case OpConcat:
        for (const auto& sub : re->Sub) {
            writeRegexp(b, sub.get(), sub->Op == OpAlternate ? flagPrec : 0, flags);
        }
        break;
    case OpAlternate:
        for (std::size_t i = 0; i < re->Sub.size(); i++) {
            if (i > 0) b += '|';
            writeRegexp(b, re->Sub[i].get(), 0, flags);
        }
        break;
    }
    b.append(std::size_t(closes), ')');
}

}  // namespace

bool Regexp::Equal(const Regexp* y) const {
    const Regexp* x = this;
    if (!y) return false;
    if (x->Op != y->Op) return false;
    switch (x->Op) {
    case OpEndText:
        // The parse flags remember whether this is \z or \Z.
        if ((x->Flags & WasDollar) != (y->Flags & WasDollar)) return false;
        break;
    case OpLiteral:
    case OpCharClass:
        return (x->Flags & FoldCase) == (y->Flags & FoldCase) && x->Rune == y->Rune;
    case OpAlternate:
    case OpConcat:
        if (x->Sub.size() != y->Sub.size()) return false;
        for (std::size_t i = 0; i < x->Sub.size(); i++) {
            if (!x->Sub[i]->Equal(y->Sub[i].get())) return false;
        }
        return true;
    case OpStar:
    case OpPlus:
    case OpQuest:
        if ((x->Flags & NonGreedy) != (y->Flags & NonGreedy) || !x->Sub[0]->Equal(y->Sub[0].get())) return false;
        break;
    case OpRepeat:
        if ((x->Flags & NonGreedy) != (y->Flags & NonGreedy) || x->Min != y->Min || x->Max != y->Max ||
            !x->Sub[0]->Equal(y->Sub[0].get())) {
            return false;
        }
        break;
    case OpCapture:
        if (x->Cap != y->Cap || x->Name != y->Name || !x->Sub[0]->Equal(y->Sub[0].get())) return false;
        break;
    default:
        break;
    }
    return true;
}

string Regexp::String() const {
    std::string b;
    flagMap flags;
    auto [must, cant] = calcFlags(this, flags);
    must |= printFlags((cant & ~flagI) << negShift);
    if (must) must |= flagOff;
    writeRegexp(b, this, must, flags);
    return string(b);
}

int Regexp::MaxCap() const {
    int m = 0;
    if (Op == OpCapture) m = Cap;
    for (const auto& sub : Sub) m = std::max(m, sub->MaxCap());
    return m;
}

std::vector<string> Regexp::CapNames() const {
    std::vector<string> names(std::size_t(MaxCap() + 1));
    capNames(names);
    return names;
}

void Regexp::capNames(std::vector<string>& names) const {
    if (Op == OpCapture) names[std::size_t(Cap)] = Name;
    for (const auto& sub : Sub) sub->capNames(names);
}

// ---------------------------------------------------------------------------
// Simplify (simplify.go)

namespace {

ptr<Regexp> newRe(Op op, Flags flags = 0) {
    auto re = make_ptr<Regexp>(op);
    re->Flags = flags;
    return re;
}

// simplify1 implements Simplify for the unary OpStar, OpPlus, and OpQuest
// operators. It returns the simple regexp equivalent to
//
//	Regexp{Op: op, Flags: flags, Sub: {sub}}
//
// under the assumption that sub is already simple, and without first
// allocating that structure. If the regexp to be returned turns out to be
// equivalent to re, simplify1 returns re instead.
ptr<Regexp> simplify1(Op op, Flags flags, ptr<Regexp> sub, const ptr<Regexp>& re) {
    // Special case: repeat the empty string as much as you want, but it's
    // going to match only once.
    if (sub->Op == OpEmptyMatch) return sub;
    // The operators are idempotent if the flags match.
    if (op == sub->Op && (flags & NonGreedy) == (sub->Flags & NonGreedy)) return sub;
    if (re && re->Op == op && (re->Flags & NonGreedy) == (flags & NonGreedy) && sub == re->Sub[0]) return re;
    auto n = newRe(op, flags);
    n->Sub.push_back(std::move(sub));
    return n;
}

}  // namespace

ptr<Regexp> Regexp::Simplify() {
    ptr<Regexp> self(this);
    switch (Op) {
    case OpCapture:
    case OpConcat:
    case OpAlternate: {
        // Simplify children, building new Regexp if children change.
        ptr<Regexp> nre = self;
        for (std::size_t i = 0; i < Sub.size(); i++) {
            ptr<Regexp> sub = Sub[i];
            ptr<Regexp> nsub = sub->Simplify();
            if (nre == self && nsub != sub) {
                // Start a copy.
                nre = make_ptr<Regexp>(*this);
                nre->Rune.clear();
                nre->Sub.assign(Sub.begin(), Sub.begin() + isize(i));
            }
            if (nre != self) nre->Sub.push_back(nsub);
        }
        return nre;
    }
    case OpStar:
    case OpPlus:
    case OpQuest:
        return simplify1(Op, Flags, Sub[0]->Simplify(), self);
    case OpRepeat: {
        // Special special case: x{0} matches the empty string and doesn't
        // even need to consider x.
        if (Min == 0 && Max == 0) return newRe(OpEmptyMatch);
        // The fun begins.
        ptr<Regexp> sub = Sub[0]->Simplify();
        // x{n,} means at least n matches of x.
        if (Max == -1) {
            // Special case: x{0,} is x*.
            if (Min == 0) return simplify1(OpStar, Flags, sub, nullptr);
            // Special case: x{1,} is x+.
            if (Min == 1) return simplify1(OpPlus, Flags, sub, nullptr);
            // General case: x{4,} is xxxx+.
            auto nre = newRe(OpConcat);
            for (int i = 0; i < Min - 1; i++) nre->Sub.push_back(sub);
            nre->Sub.push_back(simplify1(OpPlus, Flags, sub, nullptr));
            return nre;
        }
        // Special case x{0} handled above.
        // Special case: x{1} is just x.
        if (Min == 1 && Max == 1) return sub;
        // General case: x{n,m} means n copies of x and m copies of x?. The
        // machine will do less work if we nest the final m copies, so that
        // x{2,5} = xx(x(x(x)?)?)?

        // Build leading prefix: xx.
        ptr<Regexp> prefix;
        if (Min > 0) {
            prefix = newRe(OpConcat);
            for (int i = 0; i < Min; i++) prefix->Sub.push_back(sub);
        }
        // Build and attach suffix: (x(x(x)?)?)?
        if (Max > Min) {
            ptr<Regexp> suffix = simplify1(OpQuest, Flags, sub, nullptr);
            for (int i = Min + 1; i < Max; i++) {
                auto nre2 = newRe(OpConcat);
                nre2->Sub = {sub, suffix};
                suffix = simplify1(OpQuest, Flags, nre2, nullptr);
            }
            if (!prefix) return suffix;
            prefix->Sub.push_back(suffix);
        }
        if (prefix) return prefix;
        // Some degenerate case like min > max or min < max < 0. Handle as
        // impossible match.
        return newRe(OpNoMatch);
    }
    default:
        return self;
    }
}

// ---------------------------------------------------------------------------
// Parsing (parse.go)

namespace {

// The limits of the parser, as Go's.
constexpr int maxHeight = 1000;
constexpr int64 instSize = 5 * 8;  // byte, 2 uint32, slice is 5 64-bit words
constexpr int64 maxSize = (128 << 20) / instSize;
constexpr int64 runeSize = 4;  // rune is int32
constexpr int64 maxRunes = (128 << 20) / runeSize;

// Thrown for ErrLarge and ErrNestingDepth, as Go panics with them.
struct limitError {
    ErrorCode code;
};

error makeError(ErrorCode code, std::string_view expr) { return make_ptr<Error>(code, string(expr)); }

struct charGroup {
    int sign = 0;
    std::span<const rune> cls;
};

constexpr rune code1[] = {/* \d */ 0x30, 0x39};
constexpr rune code2[] = {/* \s */ 0x9, 0xa, 0xc, 0xd, 0x20, 0x20};
constexpr rune code3[] = {/* \w */ 0x30, 0x39, 0x41, 0x5a, 0x5f, 0x5f, 0x61, 0x7a};
constexpr rune code4[] = {/* [:alnum:] */ 0x30, 0x39, 0x41, 0x5a, 0x61, 0x7a};
constexpr rune code5[] = {/* [:alpha:] */ 0x41, 0x5a, 0x61, 0x7a};
constexpr rune code6[] = {/* [:ascii:] */ 0x0, 0x7f};
constexpr rune code7[] = {/* [:blank:] */ 0x9, 0x9, 0x20, 0x20};
constexpr rune code8[] = {/* [:cntrl:] */ 0x0, 0x1f, 0x7f, 0x7f};
constexpr rune code9[] = {/* [:digit:] */ 0x30, 0x39};
constexpr rune code10[] = {/* [:graph:] */ 0x21, 0x7e};
constexpr rune code11[] = {/* [:lower:] */ 0x61, 0x7a};
constexpr rune code12[] = {/* [:print:] */ 0x20, 0x7e};
constexpr rune code13[] = {/* [:punct:] */ 0x21, 0x2f, 0x3a, 0x40, 0x5b, 0x60, 0x7b, 0x7e};
constexpr rune code14[] = {/* [:space:] */ 0x9, 0xd, 0x20, 0x20};
constexpr rune code15[] = {/* [:upper:] */ 0x41, 0x5a};
constexpr rune code16[] = {/* [:word:] */ 0x30, 0x39, 0x41, 0x5a, 0x5f, 0x5f, 0x61, 0x7a};
constexpr rune code17[] = {/* [:xdigit:] */ 0x30, 0x39, 0x41, 0x46, 0x61, 0x66};

charGroup perlGroup(std::string_view s) {
    if (s == "\\d") return {+1, code1};
    if (s == "\\D") return {-1, code1};
    if (s == "\\s") return {+1, code2};
    if (s == "\\S") return {-1, code2};
    if (s == "\\w") return {+1, code3};
    if (s == "\\W") return {-1, code3};
    return {};
}

charGroup posixGroup(std::string_view s) {
    struct entry {
        std::string_view name;
        std::span<const rune> cls;
    };
    static constexpr entry groups[] = {
        {"alnum", code4}, {"alpha", code5}, {"ascii", code6},  {"blank", code7},  {"cntrl", code8},  {"digit", code9},
        {"graph", code10}, {"lower", code11}, {"print", code12}, {"punct", code13}, {"space", code14}, {"upper", code15},
        {"word", code16},  {"xdigit", code17},
    };
    // "[:name:]" or "[:^name:]"
    if (s.size() < 5 || !s.starts_with("[:") || !s.ends_with(":]")) return {};
    std::string_view name = s.substr(2, s.size() - 4);
    int sign = +1;
    if (name.starts_with('^')) {
        sign = -1;
        name.remove_prefix(1);
    }
    for (const auto& g : groups) {
        if (g.name == name) return {sign, g.cls};
    }
    return {};
}

void sortRanges(std::vector<rune>& r) {
    std::size_t n = r.size() / 2;
    std::vector<std::pair<rune, rune>> v(n);
    for (std::size_t i = 0; i < n; i++) v[i] = {r[2 * i], r[2 * i + 1]};
    std::stable_sort(v.begin(), v.end(), [](const auto& a, const auto& b) {
        return a.first < b.first || (a.first == b.first && a.second > b.second);
    });
    for (std::size_t i = 0; i < n; i++) {
        r[2 * i] = v[i].first;
        r[2 * i + 1] = v[i].second;
    }
}

// cleanClass sorts the ranges (pairs of elements of r), merges them, and
// eliminates duplicates.
void cleanClass(std::vector<rune>& r) {
    // Sort by lo increasing, hi decreasing to break ties.
    sortRanges(r);
    if (r.size() < 2) return;
    // Merge abutting, overlapping.
    std::size_t w = 2;  // write index
    for (std::size_t i = 2; i < r.size(); i += 2) {
        rune lo = r[i], hi = r[i + 1];
        if (lo <= r[w - 1] + 1) {
            // merge with previous range
            if (hi > r[w - 1]) r[w - 1] = hi;
            continue;
        }
        // new disjoint range
        r[w] = lo;
        r[w + 1] = hi;
        w += 2;
    }
    r.resize(w);
}

// appendRange returns the result of appending the range lo-hi to the class
// r.
void appendRange(std::vector<rune>& r, rune lo, rune hi) {
    // Expand last range or next to last range if it overlaps or abuts.
    // Checking two ranges helps when appending case-folded alphabets, so
    // that one range can be expanding A-Z and the other a-z.
    std::size_t n = r.size();
    for (std::size_t i = 2; i <= 4; i += 2) {  // twice, using i=2, i=4
        if (n >= i) {
            rune rlo = r[n - i], rhi = r[n - i + 1];
            if (lo <= rhi + 1 && rlo <= hi + 1) {
                if (lo < rlo) r[n - i] = lo;
                if (hi > rhi) r[n - i + 1] = hi;
                return;
            }
        }
    }
    r.push_back(lo);
    r.push_back(hi);
}

// appendFoldedRange returns the result of appending the range lo-hi and its
// case folding-equivalent runes to the class r.
void appendFoldedRange(std::vector<rune>& r, rune lo, rune hi) {
    // Optimizations.
    if (lo <= minFold && hi >= maxFold) {
        // Range is full: folding can't add more.
        appendRange(r, lo, hi);
        return;
    }
    if (hi < minFold || lo > maxFold) {
        // Range is outside folding possibilities.
        appendRange(r, lo, hi);
        return;
    }
    if (lo < minFold) {
        // [lo, minFold-1] needs no folding.
        appendRange(r, lo, minFold - 1);
        lo = minFold;
    }
    if (hi > maxFold) {
        // [maxFold+1, hi] needs no folding.
        appendRange(r, maxFold + 1, hi);
        hi = maxFold;
    }
    // Brute force. Depend on appendRange to coalesce ranges on the fly.
    for (rune c = lo; c <= hi; c++) {
        appendRange(r, c, c);
        for (rune f = unicode::SimpleFold(c); f != c; f = unicode::SimpleFold(f)) appendRange(r, f, f);
    }
}

void appendLiteral(std::vector<rune>& r, rune x, Flags flags) {
    if (flags & FoldCase) {
        appendFoldedRange(r, x, x);
    } else {
        appendRange(r, x, x);
    }
}

void appendClass(std::vector<rune>& r, std::span<const rune> x) {
    for (std::size_t i = 0; i < x.size(); i += 2) appendRange(r, x[i], x[i + 1]);
}

void appendFoldedClass(std::vector<rune>& r, std::span<const rune> x) {
    for (std::size_t i = 0; i < x.size(); i += 2) appendFoldedRange(r, x[i], x[i + 1]);
}

void appendNegatedClass(std::vector<rune>& r, std::span<const rune> x) {
    rune nextLo = 0;
    for (std::size_t i = 0; i < x.size(); i += 2) {
        rune lo = x[i], hi = x[i + 1];
        if (nextLo <= lo - 1) appendRange(r, nextLo, lo - 1);
        nextLo = hi + 1;
    }
    if (nextLo <= unicode::MaxRune) appendRange(r, nextLo, unicode::MaxRune);
}

template <typename F>
void forEachTableRange(const unicode::RangeTable* x, F f) {
    for (const auto& xr : x->R16) f(rune(xr.Lo), rune(xr.Hi), rune(xr.Stride));
    for (const auto& xr : x->R32) f(rune(xr.Lo), rune(xr.Hi), rune(xr.Stride));
}

void appendTable(std::vector<rune>& r, const unicode::RangeTable* x) {
    forEachTableRange(x, [&](rune lo, rune hi, rune stride) {
        if (stride == 1) {
            appendRange(r, lo, hi);
            return;
        }
        for (rune c = lo; c <= hi; c += stride) appendRange(r, c, c);
    });
}

void appendNegatedTable(std::vector<rune>& r, const unicode::RangeTable* x) {
    rune nextLo = 0;  // lo end of next class to add
    forEachTableRange(x, [&](rune lo, rune hi, rune stride) {
        if (stride == 1) {
            if (nextLo <= lo - 1) appendRange(r, nextLo, lo - 1);
            nextLo = hi + 1;
            return;
        }
        for (rune c = lo; c <= hi; c += stride) {
            if (nextLo <= c - 1) appendRange(r, nextLo, c - 1);
            nextLo = c + 1;
        }
    });
    if (nextLo <= unicode::MaxRune) appendRange(r, nextLo, unicode::MaxRune);
}

// negateClass overwrites r and returns r's negation. It assumes the class r
// is already clean.
void negateClass(std::vector<rune>& r) {
    rune nextLo = 0;    // lo end of next class to add
    std::size_t w = 0;  // write index
    for (std::size_t i = 0; i < r.size(); i += 2) {
        rune lo = r[i], hi = r[i + 1];
        if (nextLo <= lo - 1) {
            r[w] = nextLo;
            r[w + 1] = lo - 1;
            w += 2;
        }
        nextLo = hi + 1;
    }
    r.resize(w);
    if (nextLo <= unicode::MaxRune) {
        r.push_back(nextLo);
        r.push_back(unicode::MaxRune);
    }
}

std::optional<error> checkUTF8(std::string_view s) {
    while (!s.empty()) {
        auto [r, size] = utf8::DecodeRuneInString(s);
        if (r == utf8::RuneError && size == 1) return makeError(ErrInvalidUTF8, s);
        s.remove_prefix(std::size_t(size));
    }
    return std::nullopt;
}

// nextRune: the first rune of s and the rest, or an error.
error nextRune(std::string_view s, rune& c, std::string_view& t) {
    auto [r, size] = utf8::DecodeRuneInString(s);
    if (r == utf8::RuneError && size == 1) return makeError(ErrInvalidUTF8, s);
    c = r;
    t = s.substr(std::size_t(size));
    return nullptr;
}

bool isalnum(rune c) { return ('0' <= c && c <= '9') || ('A' <= c && c <= 'Z') || ('a' <= c && c <= 'z'); }

rune unhex(rune c) {
    if ('0' <= c && c <= '9') return c - '0';
    if ('a' <= c && c <= 'f') return c - 'a' + 10;
    if ('A' <= c && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool isValidCaptureName(std::string_view name) {
    if (name.empty()) return false;
    while (!name.empty()) {
        auto [c, size] = utf8::DecodeRuneInString(name);
        if (c != '_' && !isalnum(c)) return false;
        name.remove_prefix(std::size_t(size));
    }
    return true;
}

bool isCharClass(const Regexp* re) {
    return (re->Op == OpLiteral && re->Rune.size() == 1) || re->Op == OpCharClass || re->Op == OpAnyCharNotNL ||
           re->Op == OpAnyChar;
}

// does re match r?
bool matchRune(const Regexp* re, rune r) {
    switch (re->Op) {
    case OpLiteral:
        return re->Rune.size() == 1 && re->Rune[0] == r;
    case OpCharClass:
        for (std::size_t i = 0; i < re->Rune.size(); i += 2) {
            if (re->Rune[i] <= r && r <= re->Rune[i + 1]) return true;
        }
        return false;
    case OpAnyCharNotNL:
        return r != '\n';
    case OpAnyChar:
        return true;
    default:
        return false;
    }
}

// mergeCharClass makes dst = dst|src. The caller must ensure that dst.Op >=
// src.Op, to reduce the amount of copying.
void mergeCharClass(Regexp* dst, const Regexp* src) {
    switch (dst->Op) {
    case OpAnyChar:
        // src doesn't add anything.
        break;
    case OpAnyCharNotNL:
        // src might add \n
        if (matchRune(src, '\n')) dst->Op = OpAnyChar;
        break;
    case OpCharClass:
        // src is simpler, so either literal or char class
        if (src->Op == OpLiteral) {
            appendLiteral(dst->Rune, src->Rune[0], src->Flags);
        } else {
            appendClass(dst->Rune, src->Rune);
        }
        break;
    case OpLiteral: {
        // both literal
        if (src->Rune[0] == dst->Rune[0] && src->Flags == dst->Flags) break;
        dst->Op = OpCharClass;
        rune d = dst->Rune[0];
        dst->Rune.clear();
        appendLiteral(dst->Rune, d, dst->Flags);
        appendLiteral(dst->Rune, src->Rune[0], src->Flags);
        break;
    }
    default:
        break;
    }
}

// cleanAlt cleans re for eventual inclusion in an alternation.
void cleanAlt(Regexp* re) {
    if (re->Op != OpCharClass) return;
    cleanClass(re->Rune);
    if (re->Rune.size() == 2 && re->Rune[0] == 0 && re->Rune[1] == unicode::MaxRune) {
        re->Rune.clear();
        re->Op = OpAnyChar;
        return;
    }
    if (re->Rune.size() == 4 && re->Rune[0] == 0 && re->Rune[1] == '\n' - 1 && re->Rune[2] == '\n' + 1 &&
        re->Rune[3] == unicode::MaxRune) {
        re->Rune.clear();
        re->Op = OpAnyCharNotNL;
        return;
    }
}

bool repeatIsValid(const Regexp* re, int n) {
    if (re->Op == OpRepeat) {
        int m = re->Max;
        if (m == 0) return true;
        if (m < 0) m = re->Min;
        if (m > n) return false;
        if (m > 0) n /= m;
    }
    for (const auto& sub : re->Sub) {
        if (!repeatIsValid(sub.get(), n)) return false;
    }
    return true;
}

rune minFoldRune(rune r) {
    if (r < minFold || r > maxFold) return r;
    rune m = r;
    rune r0 = r;
    for (r = unicode::SimpleFold(r); r != r0; r = unicode::SimpleFold(r)) m = std::min(m, r);
    return m;
}

// Tables for \p{Any}, \p{ASCII}.
constexpr unicode::Range16 anyR16[] = {{0, 0xFFFF, 1}};
constexpr unicode::Range32 anyR32[] = {{1 << 16, uint32(unicode::MaxRune), 1}};
constexpr unicode::RangeTable anyTable = {anyR16, anyR32};
constexpr unicode::Range16 asciiR16[] = {{0, 0x7F, 1}};
constexpr unicode::RangeTable asciiTable = {asciiR16, {}};
constexpr unicode::Range16 asciiFoldR16[] = {
    {0, 0x7F, 1},
    {0x017F, 0x017F, 1},  // Old English long s (ſ), folds to S/s.
    {0x212A, 0x212A, 1},  // Kelvin K, folds to K/k.
};
constexpr unicode::RangeTable asciiFoldTable = {asciiFoldR16, {}};

// canonicalName returns the canonical lookup string for name. The canonical
// name has a leading uppercase letter and then lowercase letters, and it
// omits all underscores, spaces, and hyphens. (We could have used all
// lowercase, but this way most package unicode map keys are already
// canonical.)
std::string canonicalName(std::string_view name) {
    std::string b;
    bool first = true;
    for (char c : name) {
        if (c == '_' || c == '-' || c == ' ') continue;
        if (first) {
            if ('a' <= c && c <= 'z') c -= 'a' - 'A';
            first = false;
        } else if ('A' <= c && c <= 'Z') {
            c += 'a' - 'A';
        }
        b += c;
    }
    return b;
}

struct aliasMaps {
    std::map<std::string, std::string_view, std::less<>> categories;
    std::map<std::string, std::string_view, std::less<>> scripts;
};

// Made on first use. Building them parks nothing, so a static is fine.
const aliasMaps& aliases() {
    static const aliasMaps* m = [] {
        auto* a = new aliasMaps();
        for (const auto& [name, actual] : unicode::CategoryAliases) a->categories[canonicalName(name)] = actual;
        for (const auto& [name, _] : unicode::Scripts) a->scripts[canonicalName(name)] = name;
        return a;
    }();
    return *m;
}

struct tableResult {
    const unicode::RangeTable* tab = nullptr;
    const unicode::RangeTable* fold = nullptr;
    int sign = 0;
};

// unicodeTable returns the unicode.RangeTable identified by name and the
// table of additional fold-equivalent code points. If sign < 0, the result
// should be inverted.
tableResult unicodeTable(std::string_view rawName) {
    std::string name = canonicalName(rawName);
    // Special case: "Any" means any.
    if (name == "Any") return {&anyTable, &anyTable, +1};
    // Special case: "Assigned" means assigned.
    if (name == "Assigned") return {unicode::Cn, unicode::Cn, -1};  // invert Cn (unassigned)
    // Special case: "ASCII" means ASCII.
    if (name == "Ascii") return {&asciiTable, &asciiFoldTable, +1};
    // Special case: "LC" is a category alias for "L&" but has no "L&" table.
    if (name == "Lc") return {unicode::Categories["LC"], unicode::FoldCategory["LC"], +1};
    if (auto t = unicode::Categories[name]) return {t, unicode::FoldCategory[name], +1};
    if (auto t = unicode::Scripts[name]) return {t, unicode::FoldScript[name], +1};
    // Look for aliases.
    const auto& a = aliases();
    if (auto it = a.categories.find(name); it != a.categories.end()) {
        return {unicode::Categories[it->second], unicode::FoldCategory[it->second], +1};
    }
    if (auto it = a.scripts.find(name); it != a.scripts.end()) {
        return {unicode::Scripts[it->second], unicode::FoldScript[it->second], +1};
    }
    return {};
}

// A parser is responsible for parsing a regular expression.
class parser {
public:
    Flags flags = 0;                 // parse mode flags
    std::vector<ptr<Regexp>> stack;  // stack of parsed expressions
    int numCap = 0;                  // number of capturing groups seen
    std::string_view wholeRegexp;
    std::vector<rune> tmpClass;  // temporary char class work space
    int64 numRegexp = 0;         // number of regexps allocated
    int64 numRunes = 0;          // number of runes in char classes
    int64 repeats = 0;           // product of all repetitions seen
    std::optional<std::unordered_map<const Regexp*, int>> height;  // regexp height, for height limit check
    std::optional<std::unordered_map<const Regexp*, int64>> size;  // regexp compiled size, for size limit check

    ptr<Regexp> newRegexp(Op op) {
        numRegexp++;
        return make_ptr<Regexp>(op);
    }

    void reuse(const ptr<Regexp>& re) {
        if (height) height->erase(re.get());
    }

    void checkLimits(const Regexp* re) {
        if (numRunes > maxRunes) throw limitError{ErrLarge};
        checkSize(re);
        checkHeight(re);
    }

    void checkSize(const Regexp* re) {
        if (!size) {
            // We haven't started tracking size yet. Do a relatively cheap
            // check to see if we need to start. Maintain the product of all
            // the repeats we've seen and don't track if the total number of
            // regexp nodes we've seen times the repeat product is in budget.
            if (repeats == 0) repeats = 1;
            if (re->Op == OpRepeat) {
                int n = re->Max;
                if (n == -1) n = re->Min;
                if (n <= 0) n = 1;
                if (int64(n) > maxSize / repeats) {
                    repeats = maxSize;
                } else {
                    repeats *= n;
                }
            }
            if (numRegexp < maxSize / repeats) return;
            // We need to start tracking size. Make the map and enter
            // everything we've seen so far.
            size.emplace();
            for (const auto& r : stack) checkSize(r.get());
        }
        if (calcSize(re, true) > maxSize) throw limitError{ErrLarge};
    }

    int64 calcSize(const Regexp* re, bool force) {
        if (!force) {
            if (auto it = size->find(re); it != size->end()) return it->second;
        }
        int64 sz = 0;
        switch (re->Op) {
        case OpLiteral:
            sz = int64(re->Rune.size());
            break;
        case OpCapture:
        case OpStar:
            // star can be 1+ or 2+; assume 2 pessimistically
            sz = 2 + calcSize(re->Sub[0].get(), false);
            break;
        case OpPlus:
        case OpQuest:
            sz = 1 + calcSize(re->Sub[0].get(), false);
            break;
        case OpConcat:
            for (const auto& sub : re->Sub) sz += calcSize(sub.get(), false);
            break;
        case OpAlternate:
            for (const auto& sub : re->Sub) sz += calcSize(sub.get(), false);
            if (re->Sub.size() > 1) sz += int64(re->Sub.size()) - 1;
            break;
        case OpRepeat: {
            int64 sub = calcSize(re->Sub[0].get(), false);
            if (re->Max == -1) {
                if (re->Min == 0) {
                    sz = 2 + sub;  // x*
                } else {
                    sz = 1 + int64(re->Min) * sub;  // xxx+
                }
                break;
            }
            // x{2,5} = xx(x(x(x)?)?)?
            sz = int64(re->Max) * sub + int64(re->Max - re->Min);
            break;
        }
        default:
            break;
        }
        sz = std::max<int64>(1, sz);
        (*size)[re] = sz;
        return sz;
    }

    void checkHeight(const Regexp* re) {
        if (numRegexp < maxHeight) return;
        if (!height) {
            height.emplace();
            for (const auto& r : stack) checkHeight(r.get());
        }
        if (calcHeight(re, true) > maxHeight) throw limitError{ErrNestingDepth};
    }

    int calcHeight(const Regexp* re, bool force) {
        if (!force) {
            if (auto it = height->find(re); it != height->end()) return it->second;
        }
        int h = 1;
        for (const auto& sub : re->Sub) h = std::max(h, 1 + calcHeight(sub.get(), false));
        (*height)[re] = h;
        return h;
    }

    // push pushes the regexp re onto the parse stack and returns the regexp.
    ptr<Regexp> push(ptr<Regexp> re) {
        numRunes += int64(re->Rune.size());
        auto& R = re->Rune;
        if (re->Op == OpCharClass && R.size() == 2 && R[0] == R[1]) {
            // Single rune.
            if (maybeConcat(R[0], flags & ~FoldCase)) return nullptr;
            re->Op = OpLiteral;
            R.resize(1);
            re->Flags = flags & ~FoldCase;
        } else if ((re->Op == OpCharClass && R.size() == 4 && R[0] == R[1] && R[2] == R[3] &&
                    unicode::SimpleFold(R[0]) == R[2] && unicode::SimpleFold(R[2]) == R[0]) ||
                   (re->Op == OpCharClass && R.size() == 2 && R[0] + 1 == R[1] && unicode::SimpleFold(R[0]) == R[1] &&
                    unicode::SimpleFold(R[1]) == R[0])) {
            // Case-insensitive rune like [Aa] or [Δδ].
            if (maybeConcat(R[0], flags | FoldCase)) return nullptr;
            // Rewrite as (case-insensitive) literal.
            re->Op = OpLiteral;
            R.resize(1);
            re->Flags = flags | FoldCase;
        } else {
            // Incremental concatenation.
            maybeConcat(-1, 0);
        }
        stack.push_back(re);
        checkLimits(re.get());
        return re;
    }

    // maybeConcat implements incremental concatenation of literal runes into
    // string nodes. Readers of the stack beware: the top-of-stack node may
    // contain only a single rune, which is not yet a full literal string.
    // If r >= 0 and there's a node left over, maybeConcat uses it to push r
    // with the given flags. maybeConcat reports whether r was pushed.
    bool maybeConcat(rune r, Flags fl) {
        std::size_t n = stack.size();
        if (n < 2) return false;
        Regexp* re1 = stack[n - 1].get();
        Regexp* re2 = stack[n - 2].get();
        if (re1->Op != OpLiteral || re2->Op != OpLiteral || (re1->Flags & FoldCase) != (re2->Flags & FoldCase)) {
            return false;
        }
        // Push re1 into re2.
        re2->Rune.insert(re2->Rune.end(), re1->Rune.begin(), re1->Rune.end());
        // Reuse re1 if possible.
        if (r >= 0) {
            re1->Rune = {r};
            re1->Flags = fl;
            return true;
        }
        reuse(stack.back());
        stack.pop_back();
        return false;  // did not push r
    }

    // literal pushes a literal regexp for the rune r on the stack.
    void literal(rune r) {
        auto re = newRegexp(OpLiteral);
        re->Flags = flags;
        if (flags & FoldCase) r = minFoldRune(r);
        re->Rune = {r};
        push(re);
    }

    // op pushes a regexp with the given op onto the stack and returns that
    // regexp.
    ptr<Regexp> op(Op o) {
        auto re = newRegexp(o);
        re->Flags = flags;
        return push(re);
    }

    // repeat replaces the top stack element with itself repeated according
    // to op, min, max. before is the regexp suffix starting at the
    // repetition operator. after is the regexp suffix following after the
    // repetition operator. repeat returns an updated 'after' and an error,
    // if any.
    error repeat(Op o, int min, int max, std::string_view before, std::string_view& after, std::string_view lastRepeat) {
        Flags fl = flags;
        if (flags & PerlX) {
            if (!after.empty() && after[0] == '?') {
                after.remove_prefix(1);
                fl ^= NonGreedy;
            }
            if (!lastRepeat.empty()) {
                // In Perl it is not allowed to stack repetition operators:
                //   a** is a syntax error, not a doubled star.
                // (and a++ means something else entirely, which we don't
                // support!)
                return makeError(ErrInvalidRepeatOp, lastRepeat.substr(0, lastRepeat.size() - after.size()));
            }
        }
        std::size_t n = stack.size();
        if (n == 0) return makeError(ErrMissingRepeatArgument, before.substr(0, before.size() - after.size()));
        ptr<Regexp> sub = stack[n - 1];
        if (sub->Op >= opPseudo) return makeError(ErrMissingRepeatArgument, before.substr(0, before.size() - after.size()));

        auto re = newRegexp(o);
        re->Min = min;
        re->Max = max;
        re->Flags = fl;
        re->Sub = {sub};
        stack[n - 1] = re;
        checkLimits(re.get());

        if (o == OpRepeat && (min >= 2 || max >= 2) && !repeatIsValid(re.get(), 1000)) {
            return makeError(ErrInvalidRepeatSize, before.substr(0, before.size() - after.size()));
        }
        return nullptr;
    }

    // concat replaces the top of the stack (above the topmost '|' or '(')
    // with its concatenation.
    ptr<Regexp> concat() {
        maybeConcat(-1, 0);
        // Scan down to find pseudo-operator | or (.
        std::size_t i = stack.size();
        while (i > 0 && stack[i - 1]->Op < opPseudo) i--;
        std::vector<ptr<Regexp>> subs(stack.begin() + isize(i), stack.end());
        stack.resize(i);
        // Empty concatenation is special case.
        if (subs.empty()) return push(newRegexp(OpEmptyMatch));
        return push(collapse(subs, OpConcat));
    }

    // alternate replaces the top of the stack (above the topmost '(') with
    // its alternation.
    ptr<Regexp> alternate() {
        // Scan down to find pseudo-operator (. There are no | above (.
        std::size_t i = stack.size();
        while (i > 0 && stack[i - 1]->Op < opPseudo) i--;
        std::vector<ptr<Regexp>> subs(stack.begin() + isize(i), stack.end());
        stack.resize(i);
        // Make sure top class is clean. All the others already are (see
        // swapVerticalBar).
        if (!subs.empty()) cleanAlt(subs.back().get());
        // Empty alternate is special case (shouldn't happen but easy to
        // handle).
        if (subs.empty()) return push(newRegexp(OpNoMatch));
        return push(collapse(subs, OpAlternate));
    }

    // collapse returns the result of applying op to sub. If sub contains op
    // nodes, they all get hoisted up so that there is never a concat of a
    // concat or an alternate of an alternate.
    ptr<Regexp> collapse(std::vector<ptr<Regexp>> subs, Op o) {
        if (subs.size() == 1) return subs[0];
        auto re = newRegexp(o);
        for (auto& sub : subs) {
            if (sub->Op == o) {
                re->Sub.insert(re->Sub.end(), sub->Sub.begin(), sub->Sub.end());
                reuse(sub);
            } else {
                re->Sub.push_back(sub);
            }
        }
        if (o == OpAlternate) {
            re->Sub = factor(std::move(re->Sub));
            if (re->Sub.size() == 1) {
                auto old = re;
                re = re->Sub[0];
                reuse(old);
            }
        }
        return re;
    }

    // factor factors common prefixes from the alternation list sub. It
    // returns a replacement list that reuses the same storage and frees
    // (passes to p.reuse) any removed *Regexps.
    //
    // For example,
    //
    //	ABC|ABD|AEF|BCX|BCY
    //
    // simplifies by literal prefix extraction to
    //
    //	A(B(C|D)|EF)|BC(X|Y)
    //
    // which simplifies by character class introduction to
    //
    //	A(B[CD]|EF)|BC[XY]
    std::vector<ptr<Regexp>> factor(std::vector<ptr<Regexp>> sub) {
        if (sub.size() < 2) return sub;

        // Round 1: Factor out common literal prefixes.
        std::vector<rune> str;
        Flags strflags = 0;
        std::size_t start = 0;
        std::vector<ptr<Regexp>> out;
        for (std::size_t i = 0; i <= sub.size(); i++) {
            // Invariant: the Regexps that were in sub[0:start] have been
            // used or marked for reuse, and the slice space has been reused
            // for out (len(out) <= start).
            //
            // Invariant: sub[start:i] consists of regexps that all begin
            // with str as modified by strflags.
            std::vector<rune> istr;
            Flags iflags = 0;
            if (i < sub.size()) {
                std::tie(istr, iflags) = leadingString(sub[i].get());
                if (iflags == strflags) {
                    std::size_t same = 0;
                    while (same < str.size() && same < istr.size() && str[same] == istr[same]) same++;
                    if (same > 0) {
                        // Matches at least one rune in current range. Keep
                        // going around.
                        str.resize(same);
                        continue;
                    }
                }
            }

            // Found end of a run with common leading literal string: sub[start:i]
            // all begin with str[:len(str)], but sub[i] does not even begin
            // with str[0].
            //
            // Factor out common string and append factored expression to
            // out.
            if (i == start) {
                // Nothing to do - run of length 0.
            } else if (i == start + 1) {
                // Just one: don't bother factoring.
                out.push_back(sub[start]);
            } else {
                // Construct factored form: prefix(suffix1|suffix2|...)
                auto prefix = newRegexp(OpLiteral);
                prefix->Flags = strflags;
                prefix->Rune = str;
                for (std::size_t j = start; j < i; j++) {
                    sub[j] = removeLeadingString(sub[j], str.size());
                    checkLimits(sub[j].get());
                }
                auto suffix = collapse(std::vector<ptr<Regexp>>(sub.begin() + isize(start), sub.begin() + isize(i)),
                                       OpAlternate);  // recurse
                auto re = newRegexp(OpConcat);
                re->Sub = {prefix, suffix};
                out.push_back(re);
            }

            // Prepare for next iteration.
            start = i;
            str = istr;
            strflags = iflags;
        }
        sub = std::move(out);

        // Round 2: Factor out common simple prefixes, just the first piece
        // of each concatenation. This will be good enough a lot of the
        // time.
        //
        // Complex subexpressions (e.g. involving quantifiers) are not safe
        // to factor because that collapses their distinct paths through the
        // automaton, which affects correctness in some cases.
        start = 0;
        out.clear();
        ptr<Regexp> first;
        for (std::size_t i = 0; i <= sub.size(); i++) {
            // Invariant: the Regexps that were in sub[0:start] have been
            // used or marked for reuse, and the slice space has been reused
            // for out (len(out) <= start).
            //
            // Invariant: sub[start:i] consists of regexps that all begin
            // with ifirst.
            ptr<Regexp> ifirst;
            if (i < sub.size()) {
                ifirst = leadingRegexp(sub[i].get());
                if (first && ifirst && first->Equal(ifirst.get()) &&
                    // first must be a character class OR a fixed repeat of
                    // a character class.
                    (isCharClass(first.get()) ||
                     (first->Op == OpRepeat && first->Min == first->Max && isCharClass(first->Sub[0].get())))) {
                    continue;
                }
            }

            // Found end of a run with common leading regexp: sub[start:i]
            // all begin with first, but sub[i] does not.
            //
            // Factor out common regexp and append factored expression to
            // out.
            if (i == start) {
                // Nothing to do - run of length 0.
            } else if (i == start + 1) {
                // Just one: don't bother factoring.
                out.push_back(sub[start]);
            } else {
                // Construct factored form: prefix(suffix1|suffix2|...)
                ptr<Regexp> prefix = first;
                for (std::size_t j = start; j < i; j++) {
                    bool reuse = j != start;  // prefix came from sub[start]
                    sub[j] = removeLeadingRegexp(sub[j], reuse);
                    checkLimits(sub[j].get());
                }
                auto suffix = collapse(std::vector<ptr<Regexp>>(sub.begin() + isize(start), sub.begin() + isize(i)),
                                       OpAlternate);  // recurse
                auto re = newRegexp(OpConcat);
                re->Sub = {prefix, suffix};
                out.push_back(re);
            }

            // Prepare for next iteration.
            start = i;
            first = ifirst;
        }
        sub = std::move(out);

        // Round 3: Collapse runs of single literals into character classes.
        start = 0;
        out.clear();
        for (std::size_t i = 0; i <= sub.size(); i++) {
            // Invariant: the Regexps that were in sub[0:start] have been
            // used or marked for reuse, and the slice space has been reused
            // for out (len(out) <= start).
            //
            // Invariant: sub[start:i] consists of regexps that are either
            // literal runes or character classes.
            if (i < sub.size() && isCharClass(sub[i].get())) continue;

            // sub[i] is not a char or char class; emit char class for
            // sub[start:i]...
            if (i == start) {
                // Nothing to do - run of length 0.
            } else if (i == start + 1) {
                out.push_back(sub[start]);
            } else {
                // Make new char class. Start with most complex regexp in
                // sub[start].
                std::size_t max = start;
                for (std::size_t j = start + 1; j < i; j++) {
                    if (sub[max]->Op < sub[j]->Op ||
                        (sub[max]->Op == sub[j]->Op && sub[max]->Rune.size() < sub[j]->Rune.size())) {
                        max = j;
                    }
                }
                std::swap(sub[start], sub[max]);
                // Copy the class, which may be shared (it may be a prefix
                // factored out above).
                auto merged = make_ptr<Regexp>(*sub[start]);
                sub[start] = merged;
                for (std::size_t j = start + 1; j < i; j++) {
                    mergeCharClass(sub[start].get(), sub[j].get());
                    reuse(sub[j]);
                }
                cleanAlt(sub[start].get());
                out.push_back(sub[start]);
            }

            // ... and then emit sub[i].
            if (i < sub.size()) out.push_back(sub[i]);
            start = i + 1;
        }
        sub = std::move(out);

        // Round 4: Collapse runs of empty matches into a single empty
        // match.
        out.clear();
        for (std::size_t i = 0; i < sub.size(); i++) {
            if (i + 1 < sub.size() && sub[i]->Op == OpEmptyMatch && sub[i + 1]->Op == OpEmptyMatch) continue;
            out.push_back(sub[i]);
        }
        return out;
    }

    // leadingString returns the leading literal string that re begins with.
    // The string refers to storage in re or its children.
    std::pair<std::vector<rune>, Flags> leadingString(const Regexp* re) {
        if (re->Op == OpConcat && !re->Sub.empty()) re = re->Sub[0].get();
        if (re->Op != OpLiteral) return {{}, 0};
        return {re->Rune, Flags(re->Flags & FoldCase)};
    }

    // removeLeadingString removes the first n leading runes from the
    // beginning of re. It returns the replacement for re.
    ptr<Regexp> removeLeadingString(ptr<Regexp> re, std::size_t n) {
        if (re->Op == OpConcat && !re->Sub.empty()) {
            // Removing a leading string in a concatenation might simplify
            // the concatenation.
            ptr<Regexp> sub = removeLeadingString(re->Sub[0], n);
            re->Sub[0] = sub;
            if (sub->Op == OpEmptyMatch) {
                reuse(sub);
                switch (re->Sub.size()) {
                case 0:
                case 1:
                    // Impossible but handle.
                    re->Op = OpEmptyMatch;
                    re->Sub.clear();
                    break;
                case 2: {
                    auto old = re;
                    re = re->Sub[1];
                    reuse(old);
                    break;
                }
                default:
                    re->Sub.erase(re->Sub.begin());
                    break;
                }
            }
            return re;
        }
        if (re->Op == OpLiteral) {
            re->Rune.erase(re->Rune.begin(), re->Rune.begin() + isize(std::min(n, re->Rune.size())));
            if (re->Rune.empty()) re->Op = OpEmptyMatch;
        }
        return re;
    }

    // leadingRegexp returns the leading regexp that re begins with. The
    // regexp refers to storage in re or its children.
    ptr<Regexp> leadingRegexp(const Regexp* re) {
        if (re->Op == OpEmptyMatch) return nullptr;
        if (re->Op == OpConcat && !re->Sub.empty()) {
            const ptr<Regexp>& sub = re->Sub[0];
            if (sub->Op == OpEmptyMatch) return nullptr;
            return sub;
        }
        return ptr<Regexp>(const_cast<Regexp*>(re));
    }

    // removeLeadingRegexp removes the leading regexp in re. It returns the
    // replacement for re. If reuse is true, it passes the removed regexp
    // (if no longer needed) to p.reuse.
    ptr<Regexp> removeLeadingRegexp(ptr<Regexp> re, bool reuseIt) {
        if (re->Op == OpConcat && !re->Sub.empty()) {
            if (reuseIt) reuse(re->Sub[0]);
            re->Sub.erase(re->Sub.begin());
            switch (re->Sub.size()) {
            case 0:
                re->Op = OpEmptyMatch;
                re->Sub.clear();
                break;
            case 1: {
                auto old = re;
                re = re->Sub[0];
                reuse(old);
                break;
            }
            default:
                break;
            }
            return re;
        }
        if (reuseIt) reuse(re);
        return newRegexp(OpEmptyMatch);
    }

    // parseRepeat parses {min} (max=min) or {min,} (max=-1) or {min,max}.
    // If s is not of that form, it returns ok == false. If s has the right
    // form but the values are too big, it returns min == -1, ok == true.
    bool parseRepeat(std::string_view s, int& min, int& max, std::string_view& rest) {
        if (s.empty() || s[0] != '{') return false;
        s.remove_prefix(1);
        if (!parseInt(s, min)) return false;
        if (s.empty()) return false;
        if (s[0] != ',') {
            max = min;
        } else {
            s.remove_prefix(1);
            if (s.empty()) return false;
            if (s[0] == '}') {
                max = -1;
            } else if (!parseInt(s, max)) {
                return false;
            } else if (max < 0) {
                // parseInt found too big a number
                min = -1;
            }
        }
        if (s.empty() || s[0] != '}') return false;
        rest = s.substr(1);
        return true;
    }

    // parsePerlFlags parses a Perl flag setting or non-capturing group or
    // both, like (?i) or (?: or (?i:. It removes the prefix from s and
    // updates the parse state. The caller must have ensured that s begins
    // with "(?".
    error parsePerlFlags(std::string_view s, std::string_view& rest) {
        std::string_view t = s;

        // Check for named captures, first introduced in Python's regexp
        // library. In general, a number of different names have been used
        // for this syntax, but support for (?P<name>expr) and (?<name>expr)
        // is now ubiquitous.
        bool startsWithP = t.size() > 4 && t[2] == 'P' && t[3] == '<';
        bool startsWithName = t.size() > 3 && t[2] == '<';
        if (startsWithP || startsWithName) {
            // position of expr start
            std::size_t exprStartPos = startsWithName ? 3 : 4;
            // Pull out name.
            auto end = t.find('>');
            if (end == std::string_view::npos) {
                if (auto err = checkUTF8(t)) return *err;
                return makeError(ErrInvalidNamedCapture, s);
            }
            std::string_view capture = t.substr(0, end + 1);              // "(?P<name>" or "(?<name>"
            std::string_view name = t.substr(exprStartPos, end - exprStartPos);  // "name"
            if (auto err = checkUTF8(name)) return *err;
            if (!isValidCaptureName(name)) return makeError(ErrInvalidNamedCapture, capture);
            // Like ordinary capture, but named.
            numCap++;
            auto re = op(opLeftParen);
            re->Cap = numCap;
            re->Name = string(name);
            rest = t.substr(end + 1);
            return nullptr;
        }

        // Non-capturing group. Might also twiddle Perl flags.
        rune c;
        t.remove_prefix(2);  // skip (?
        Flags fl = flags;
        int sign = +1;
        bool sawFlag = false;
        while (!t.empty()) {
            if (auto err = nextRune(t, c, t)) return err;
            switch (c) {
            default:
                goto bad;
            // Flags.
            case 'i':
                fl |= FoldCase;
                sawFlag = true;
                break;
            case 'm':
                fl &= ~OneLine;
                sawFlag = true;
                break;
            case 's':
                fl |= DotNL;
                sawFlag = true;
                break;
            case 'U':
                fl |= NonGreedy;
                sawFlag = true;
                break;
            // Switch to negation.
            case '-':
                if (sign < 0) goto bad;
                sign = -1;
                // Invert flags so that | above turn into &^ and vice versa.
                // We'll invert flags again before using it below.
                fl = Flags(~fl);
                sawFlag = false;
                break;
            // End of flags, starting group or not.
            case ':':
            case ')':
                if (sign < 0) {
                    if (!sawFlag) goto bad;
                    fl = Flags(~fl);
                }
                if (c == ':') {
                    // Open new group
                    op(opLeftParen);
                }
                flags = fl;
                rest = t;
                return nullptr;
            }
        }
    bad:
        return makeError(ErrInvalidPerlOp, s.substr(0, s.size() - t.size()));
    }

    // parseInt parses a decimal integer.
    bool parseInt(std::string_view& s, int& n) {
        n = 0;
        if (s.empty() || s[0] < '0' || '9' < s[0]) return false;
        // Disallow leading zeros.
        if (s.size() >= 2 && s[0] == '0' && '0' <= s[1] && s[1] <= '9') return false;
        std::string_view t = s;
        while (!s.empty() && '0' <= s[0] && s[0] <= '9') s.remove_prefix(1);
        t = t.substr(0, t.size() - s.size());
        for (char d : t) {
            // Avoid overflow.
            if (n >= 100000000) {
                n = -1;
                break;
            }
            n = n * 10 + int(d) - '0';
        }
        return true;
    }

    void parseVerticalBar() {
        concat();
        // The concatenation we just parsed is on top of the stack. If it
        // sits above an opVerticalBar, swap it below (things below an
        // opVerticalBar become an alternation). Otherwise, push a new
        // vertical bar.
        if (!swapVerticalBar()) op(opVerticalBar);
    }

    // If the top of the stack is an element followed by an opVerticalBar
    // swapVerticalBar swaps the two and returns true. Otherwise it returns
    // false.
    bool swapVerticalBar() {
        // If above and below vertical bar are literal or char class, can
        // merge into a single char class.
        std::size_t n = stack.size();
        if (n >= 3 && stack[n - 2]->Op == opVerticalBar && isCharClass(stack[n - 1].get()) &&
            isCharClass(stack[n - 3].get())) {
            ptr<Regexp> re1 = stack[n - 1];
            ptr<Regexp> re3 = stack[n - 3];
            // Make re3 the more complex of the two.
            if (re1->Op > re3->Op) {
                std::swap(re1, re3);
                stack[n - 3] = re3;
            }
            mergeCharClass(re3.get(), re1.get());
            reuse(re1);
            stack.pop_back();
            return true;
        }
        if (n >= 2) {
            ptr<Regexp> re1 = stack[n - 1];
            ptr<Regexp> re2 = stack[n - 2];
            if (re2->Op == opVerticalBar) {
                if (n >= 3) {
                    // Now out of reach. Clean opportunistically.
                    cleanAlt(stack[n - 3].get());
                }
                stack[n - 2] = re1;
                stack[n - 1] = re2;
                return true;
            }
        }
        return false;
    }

    // parseRightParen handles a ) in the input.
    error parseRightParen() {
        concat();
        if (swapVerticalBar()) {
            // pop vertical bar
            stack.pop_back();
        }
        alternate();

        std::size_t n = stack.size();
        if (n < 2) return makeError(ErrUnexpectedParen, wholeRegexp);
        ptr<Regexp> re1 = stack[n - 1];
        ptr<Regexp> re2 = stack[n - 2];
        stack.resize(n - 2);
        if (re2->Op != opLeftParen) return makeError(ErrUnexpectedParen, wholeRegexp);
        // Restore flags at time of paren.
        flags = re2->Flags;
        if (re2->Cap == 0) {
            // Just for grouping.
            push(re1);
        } else {
            re2->Op = OpCapture;
            re2->Sub = {re1};
            push(re2);
        }
        return nullptr;
    }

    // parseEscape parses an escape sequence at the beginning of s and
    // returns the rune.
    error parseEscape(std::string_view s, rune& r, std::string_view& rest) {
        std::string_view t = s.substr(1);
        if (t.empty()) return makeError(ErrTrailingBackslash, "");
        rune c;
        if (auto err = nextRune(t, c, t)) return err;

        switch (c) {
        default:
            if (c < utf8::RuneSelf && !isalnum(c)) {
                // Escaped non-word characters are always themselves. PCRE
                // is not quite so rigorous: it accepts things like \q, but
                // we don't. We once rejected \_, but too many programs and
                // people insist on using it, so allow \_.
                r = c;
                rest = t;
                return nullptr;
            }
            break;

        // Octal escapes.
        case '1': case '2': case '3': case '4': case '5': case '6': case '7':
            // Single non-zero digit is a backreference; not supported
            if (t.empty() || t[0] < '0' || t[0] > '7') break;
            [[fallthrough]];
        case '0':
            // Consume up to two more octal digits.
            r = c - '0';
            for (int i = 1; i < 3; i++) {
                if (t.empty() || t[0] < '0' || t[0] > '7') break;
                r = r * 8 + rune(t[0]) - '0';
                t.remove_prefix(1);
            }
            rest = t;
            return nullptr;

        // Hexadecimal escapes.
        case 'x': {
            if (t.empty()) break;
            if (auto err = nextRune(t, c, t)) return err;
            if (c == '{') {
                // Any number of digits in braces. Perl accepts any text at
                // all; it ignores all text after the first non-hex digit.
                // We require only hex digits, and at least one.
                int nhex = 0;
                r = 0;
                for (;;) {
                    if (t.empty()) goto invalid;
                    if (auto err = nextRune(t, c, t)) return err;
                    if (c == '}') break;
                    rune v = unhex(c);
                    if (v < 0) goto invalid;
                    r = r * 16 + v;
                    if (r > unicode::MaxRune) goto invalid;
                    nhex++;
                }
                if (nhex == 0) goto invalid;
                rest = t;
                return nullptr;
            }

            // Easy case: two hex digits.
            rune x = unhex(c);
            if (auto err = nextRune(t, c, t)) return err;
            rune y = unhex(c);
            if (x < 0 || y < 0) break;
            r = x * 16 + y;
            rest = t;
            return nullptr;
        }

        // C escapes. There is no case 'b', to avoid misparsing the Perl
        // word-boundary \b as the C backspace \b when in POSIX mode. In
        // Perl, /\b/ means word-boundary but /[\b]/ means backspace. We
        // don't support that. If you want a backspace, embed a literal
        // backspace character or use \x08.
        case 'a': r = '\a'; rest = t; return nullptr;
        case 'f': r = '\f'; rest = t; return nullptr;
        case 'n': r = '\n'; rest = t; return nullptr;
        case 'r': r = '\r'; rest = t; return nullptr;
        case 't': r = '\t'; rest = t; return nullptr;
        case 'v': r = '\v'; rest = t; return nullptr;
        }
    invalid:
        return makeError(ErrInvalidEscape, s.substr(0, s.size() - t.size()));
    }

    // parseClassChar parses a character class character at the beginning
    // of s and returns it.
    error parseClassChar(std::string_view s, std::string_view wholeClass, rune& r, std::string_view& rest) {
        if (s.empty()) return makeError(ErrMissingBracket, wholeClass);
        // Allow regular escape sequences even though many need not be
        // escaped in this context.
        if (s[0] == '\\') return parseEscape(s, r, rest);
        return nextRune(s, r, rest);
    }

    // parsePerlClassEscape parses a leading Perl character class escape like
    // \d from the beginning of s. If one is present, it appends the
    // characters to r and returns the new slice r and the remainder of the
    // string.
    bool parsePerlClassEscape(std::string_view s, std::vector<rune>& r, std::string_view& rest) {
        if (!(flags & PerlX) || s.size() < 2 || s[0] != '\\') return false;
        charGroup g = perlGroup(s.substr(0, 2));
        if (g.sign == 0) return false;
        appendGroup(r, g);
        rest = s.substr(2);
        return true;
    }

    // parseNamedClass parses a leading POSIX named character class like
    // [:alnum:] from the beginning of s. If one is present, it appends the
    // characters to r and returns the new slice r and the remainder of the
    // string.
    error parseNamedClass(std::string_view s, std::vector<rune>& r, std::string_view& rest, bool& found) {
        found = false;
        if (s.size() < 2 || s[0] != '[' || s[1] != ':') return nullptr;
        auto i = s.substr(2).find(":]");
        if (i == std::string_view::npos) return nullptr;
        i += 2;
        std::string_view name = s.substr(0, i + 2);
        s = s.substr(i + 2);
        charGroup g = posixGroup(name);
        if (g.sign == 0) return makeError(ErrInvalidCharRange, name);
        appendGroup(r, g);
        rest = s;
        found = true;
        return nullptr;
    }

    void appendGroup(std::vector<rune>& r, const charGroup& g) {
        if (!(flags & FoldCase)) {
            if (g.sign < 0) {
                appendNegatedClass(r, g.cls);
            } else {
                appendClass(r, g.cls);
            }
        } else {
            tmpClass.clear();
            appendFoldedClass(tmpClass, g.cls);
            cleanClass(tmpClass);
            if (g.sign < 0) {
                appendNegatedClass(r, tmpClass);
            } else {
                appendClass(r, tmpClass);
            }
        }
    }

    // parseUnicodeClass parses a leading Unicode character class like \p{Han}
    // from the beginning of s. If one is present, it appends the characters
    // to r and returns the new slice r and the remainder of the string.
    error parseUnicodeClass(std::string_view s, std::vector<rune>& r, std::string_view& rest, bool& found) {
        found = false;
        if (!(flags & UnicodeGroups) || s.size() < 2 || s[0] != '\\' || (s[1] != 'p' && s[1] != 'P')) return nullptr;

        // Committed to parse. Results:
        int sign = +1;
        if (s[1] == 'P') sign = -1;
        std::string_view t = s.substr(2);
        rune c;
        if (auto err = nextRune(t, c, t)) return err;
        std::string_view seq, name;
        if (c != '{') {
            // Single-letter name.
            seq = s.substr(0, s.size() - t.size());
            name = seq.substr(2);
        } else {
            // Name is in braces.
            auto end = s.find('}');
            if (end == std::string_view::npos) {
                if (auto err = checkUTF8(s)) return *err;
                return makeError(ErrInvalidCharRange, s);
            }
            seq = s.substr(0, end + 1);
            t = s.substr(end + 1);
            name = s.substr(3, end - 3);
            if (auto err = checkUTF8(name)) return *err;
        }

        // Group can have leading negation too.  \p{^Han} == \P{Han},
        // \P{^Han} == \p{Han}.
        if (!name.empty() && name[0] == '^') {
            sign = -sign;
            name.remove_prefix(1);
        }

        auto [tab, fold, tsign] = unicodeTable(name);
        if (!tab) return makeError(ErrInvalidCharRange, seq);
        if (tsign < 0) sign = -sign;

        if (!(flags & FoldCase) || !fold) {
            if (sign > 0) {
                appendTable(r, tab);
            } else {
                appendNegatedTable(r, tab);
            }
        } else {
            // Merge and clean tab and fold in a temporary buffer. This is
            // necessary for the negative case and just tidy for the
            // positive case.
            tmpClass.clear();
            appendTable(tmpClass, tab);
            appendTable(tmpClass, fold);
            cleanClass(tmpClass);
            if (sign > 0) {
                appendClass(r, tmpClass);
            } else {
                appendNegatedClass(r, tmpClass);
            }
        }
        rest = t;
        found = true;
        return nullptr;
    }

    // parseClass parses a character class at the beginning of s and pushes
    // it onto the parse stack.
    error parseClass(std::string_view s, std::string_view& rest) {
        std::string_view t = s.substr(1);  // chop [
        auto re = newRegexp(OpCharClass);
        re->Flags = flags;

        int sign = +1;
        if (!t.empty() && t[0] == '^') {
            sign = -1;
            t.remove_prefix(1);
            // If character class does not match \n, add it here, so that
            // negation later will do the right thing.
            if (!(flags & ClassNL)) {
                re->Rune.push_back('\n');
                re->Rune.push_back('\n');
            }
        }

        std::vector<rune>& cls = re->Rune;
        bool first = true;  // ] and - are okay as first char in class
        while (t.empty() || t[0] != ']' || first) {
            // POSIX: - is only okay unescaped as first or last in class.
            // Perl: - is okay anywhere.
            if (!t.empty() && t[0] == '-' && !(flags & PerlX) && !first && (t.size() == 1 || t[1] != ']')) {
                auto [_, size] = utf8::DecodeRuneInString(t.substr(1));
                return makeError(ErrInvalidCharRange, t.substr(0, 1 + std::size_t(size)));
            }
            first = false;

            // Look for POSIX [:alnum:] etc.
            if (t.size() > 2 && t[0] == '[' && t[1] == ':') {
                bool found;
                std::string_view nt;
                if (auto err = parseNamedClass(t, cls, nt, found)) return err;
                if (found) {
                    t = nt;
                    continue;
                }
            }

            // Look for Unicode character group like \p{Han}.
            {
                bool found;
                std::string_view nt;
                if (auto err = parseUnicodeClass(t, cls, nt, found)) return err;
                if (found) {
                    t = nt;
                    continue;
                }
            }

            // Look for Perl character class symbols (extension).
            {
                std::string_view nt;
                if (parsePerlClassEscape(t, cls, nt)) {
                    t = nt;
                    continue;
                }
            }

            // Single character or simple range.
            std::string_view rng = t;
            rune lo, hi;
            if (auto err = parseClassChar(t, s, lo, t)) return err;
            hi = lo;
            // [a-] means (a|-) so check for final ].
            if (t.size() >= 2 && t[0] == '-' && t[1] != ']') {
                t.remove_prefix(1);
                if (auto err = parseClassChar(t, s, hi, t)) return err;
                if (hi < lo) {
                    rng = rng.substr(0, rng.size() - t.size());
                    return makeError(ErrInvalidCharRange, rng);
                }
            }
            if (!(flags & FoldCase)) {
                appendRange(cls, lo, hi);
            } else {
                appendFoldedRange(cls, lo, hi);
            }
        }
        t.remove_prefix(1);  // chop ]

        // Use &re.Rune instead of &class to avoid allocation.
        cleanClass(re->Rune);
        if (sign < 0) negateClass(re->Rune);
        push(re);
        rest = t;
        return nullptr;
    }
};

ptr<Regexp> literalRegexp(std::string_view s, Flags flags) {
    auto re = make_ptr<Regexp>(OpLiteral);
    re->Flags = flags;
    while (!s.empty()) {
        auto [c, size] = utf8::DecodeRuneInString(s);
        re->Rune.push_back(c);
        s.remove_prefix(std::size_t(size));
    }
    return re;
}

std::pair<ptr<Regexp>, error> parse(std::string_view s, Flags flags) {
    if (flags & Literal) {
        // Trivial parser for literal string.
        if (auto err = checkUTF8(s)) return {nullptr, *err};
        return {literalRegexp(s, flags), nullptr};
    }

    // Otherwise, must do real work.
    parser p;
    rune c;
    Op op;
    std::string_view lastRepeat;
    p.flags = flags;
    p.wholeRegexp = s;
    std::string_view t = s;
    while (!t.empty()) {
        std::string_view repeat;
        switch (t[0]) {
        default:
            if (auto err = nextRune(t, c, t)) return {nullptr, err};
            p.literal(c);
            break;

        case '(':
            if ((p.flags & PerlX) && t.size() >= 2 && t[1] == '?') {
                // Flag changes and non-capturing groups.
                if (auto err = p.parsePerlFlags(t, t)) return {nullptr, err};
                break;
            }
            p.numCap++;
            p.op(opLeftParen)->Cap = p.numCap;
            t.remove_prefix(1);
            break;
        case '|':
            p.parseVerticalBar();
            t.remove_prefix(1);
            break;
        case ')':
            if (auto err = p.parseRightParen()) return {nullptr, err};
            t.remove_prefix(1);
            break;
        case '^':
            if (p.flags & OneLine) {
                p.op(OpBeginText);
            } else {
                p.op(OpBeginLine);
            }
            t.remove_prefix(1);
            break;
        case '$':
            if (p.flags & OneLine) {
                p.op(OpEndText)->Flags |= WasDollar;
            } else {
                p.op(OpEndLine);
            }
            t.remove_prefix(1);
            break;
        case '.':
            if (p.flags & DotNL) {
                p.op(OpAnyChar);
            } else {
                p.op(OpAnyCharNotNL);
            }
            t.remove_prefix(1);
            break;
        case '[':
            if (auto err = p.parseClass(t, t)) return {nullptr, err};
            break;
        case '*':
        case '+':
        case '?': {
            std::string_view before = t;
            switch (t[0]) {
            case '*': op = OpStar; break;
            case '+': op = OpPlus; break;
            default: op = OpQuest; break;
            }
            std::string_view after = t.substr(1);
            if (auto err = p.repeat(op, 0, 0, before, after, lastRepeat)) return {nullptr, err};
            repeat = before;
            t = after;
            break;
        }
        case '{': {
            op = OpRepeat;
            std::string_view before = t;
            int min = 0, max = 0;
            std::string_view after;
            if (!p.parseRepeat(t, min, max, after)) {
                // If the repeat cannot be parsed, { is a literal.
                p.literal('{');
                t.remove_prefix(1);
                break;
            }
            if (min < 0 || min > 1000 || max > 1000 || (max >= 0 && min > max)) {
                // Numbers were too big, or max is present and min > max.
                return {nullptr, makeError(ErrInvalidRepeatSize, before.substr(0, before.size() - after.size()))};
            }
            if (auto err = p.repeat(op, min, max, before, after, lastRepeat)) return {nullptr, err};
            repeat = before;
            t = after;
            break;
        }
        case '\\': {
            if ((p.flags & PerlX) && t.size() >= 2) {
                bool done = true;
                switch (t[1]) {
                case 'A':
                    p.op(OpBeginText);
                    t.remove_prefix(2);
                    break;
                case 'b':
                    p.op(OpWordBoundary);
                    t.remove_prefix(2);
                    break;
                case 'B':
                    p.op(OpNoWordBoundary);
                    t.remove_prefix(2);
                    break;
                case 'C':
                    // any byte; not supported
                    return {nullptr, makeError(ErrInvalidEscape, t.substr(0, 2))};
                case 'Q': {
                    // \Q ... \E: the ... is always literals
                    std::string_view lit = t.substr(2);
                    if (auto i = lit.find("\\E"); i != std::string_view::npos) {
                        t = lit.substr(i + 2);
                        lit = lit.substr(0, i);
                    } else {
                        t = {};
                    }
                    while (!lit.empty()) {
                        if (auto err = nextRune(lit, c, lit)) return {nullptr, err};
                        p.literal(c);
                    }
                    break;
                }
                case 'z':
                    p.op(OpEndText);
                    t.remove_prefix(2);
                    break;
                default:
                    done = false;
                }
                if (done) break;
            }

            auto re = p.newRegexp(OpCharClass);
            re->Flags = p.flags;

            // Look for Unicode character group like \p{Han}
            if (t.size() >= 2 && (t[1] == 'p' || t[1] == 'P')) {
                bool found;
                std::string_view rest;
                if (auto err = p.parseUnicodeClass(t, re->Rune, rest, found)) return {nullptr, err};
                if (found) {
                    t = rest;
                    p.push(re);
                    break;
                }
            }

            // Perl character class escape.
            {
                std::string_view rest;
                if (p.parsePerlClassEscape(t, re->Rune, rest)) {
                    t = rest;
                    p.push(re);
                    break;
                }
            }
            p.reuse(re);

            // Ordinary single-character escape.
            if (auto err = p.parseEscape(t, c, t)) return {nullptr, err};
            p.literal(c);
            break;
        }
        }
        lastRepeat = repeat;
    }

    p.concat();
    if (p.swapVerticalBar()) {
        // pop vertical bar
        p.stack.pop_back();
    }
    p.alternate();

    if (p.stack.size() != 1) return {nullptr, makeError(ErrMissingParen, s)};
    return {p.stack[0], nullptr};
}

}  // namespace

std::pair<ptr<Regexp>, error> Parse(std::string_view s, Flags flags) {
    try {
        return parse(s, flags);
    } catch (const limitError& e) {
        return {nullptr, makeError(e.code, s)};
    }
}

// ---------------------------------------------------------------------------
// Programs (prog.go)

EmptyOp EmptyOpContext(rune r1, rune r2) noexcept {
    EmptyOp op = EmptyNoWordBoundary;
    byte boundary = 0;
    if (IsWordChar(r1)) {
        boundary = 1;
    } else if (r1 == '\n') {
        op |= EmptyBeginLine;
    } else if (r1 < 0) {
        op |= EmptyBeginText | EmptyBeginLine;
    }
    if (IsWordChar(r2)) {
        boundary ^= 1;
    } else if (r2 == '\n') {
        op |= EmptyEndLine;
    } else if (r2 < 0) {
        op |= EmptyEndText | EmptyEndLine;
    }
    if (boundary != 0) {  // IsWordChar(r1) != IsWordChar(r2)
        op ^= (EmptyWordBoundary | EmptyNoWordBoundary);
    }
    return op;
}

namespace {

// The op of an instruction, with the rune variants as InstRune.
InstOp opOf(const Inst& i) {
    switch (i.Op) {
    case InstRune1:
    case InstRuneAny:
    case InstRuneAnyNotNL:
        return InstRune;
    default:
        return i.Op;
    }
}

std::string quoteToASCII(const std::vector<rune>& rs) {
    std::string s;
    for (rune r : rs) writeRune(s, r);
    std::string out = "\"";
    std::string_view v = s;
    while (!v.empty()) {
        auto [r, size] = utf8::DecodeRuneInString(v);
        if (r == utf8::RuneError && size == 1) {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\x%02x", unsigned(byte(v[0])));
            out += buf;
        } else if (r == '"' || r == '\\') {
            out += '\\';
            out += char(r);
        } else if (r >= 0x20 && r < 0x7f) {
            out += char(r);
        } else if (r == '\a') {
            out += "\\a";
        } else if (r == '\b') {
            out += "\\b";
        } else if (r == '\f') {
            out += "\\f";
        } else if (r == '\n') {
            out += "\\n";
        } else if (r == '\r') {
            out += "\\r";
        } else if (r == '\t') {
            out += "\\t";
        } else if (r == '\v') {
            out += "\\v";
        } else if (r < 0x20 || r == 0x7f) {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\x%02x", unsigned(r));
            out += buf;
        } else if (r < 0x10000) {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\u%04x", unsigned(r));
            out += buf;
        } else {
            char buf[12];
            std::snprintf(buf, sizeof buf, "\\U%08x", unsigned(r));
            out += buf;
        }
        v.remove_prefix(std::size_t(size));
    }
    out += '"';
    return out;
}

void dumpInst(std::string& b, const Inst& i) {
    auto u32 = [](uint32 v) { return std::to_string(v); };
    switch (i.Op) {
    case InstAlt: b += "alt -> " + u32(i.Out) + ", " + u32(i.Arg); break;
    case InstAltMatch: b += "altmatch -> " + u32(i.Out) + ", " + u32(i.Arg); break;
    case InstCapture: b += "cap " + u32(i.Arg) + " -> " + u32(i.Out); break;
    case InstEmptyWidth: b += "empty " + u32(i.Arg) + " -> " + u32(i.Out); break;
    case InstMatch: b += "match"; break;
    case InstFail: b += "fail"; break;
    case InstNop: b += "nop -> " + u32(i.Out); break;
    case InstRune:
        b += "rune " + quoteToASCII(i.Rune);
        if (Flags(i.Arg) & FoldCase) b += "/i";
        b += " -> " + u32(i.Out);
        break;
    case InstRune1: b += "rune1 " + quoteToASCII(i.Rune) + " -> " + u32(i.Out); break;
    case InstRuneAny: b += "any -> " + u32(i.Out); break;
    case InstRuneAnyNotNL: b += "anynotnl -> " + u32(i.Out); break;
    }
}

}  // namespace

string Inst::String() const {
    std::string b;
    dumpInst(b, *this);
    return string(b);
}

string Prog::String() const {
    std::string b;
    for (std::size_t j = 0; j < Inst.size(); j++) {
        std::string pc = std::to_string(j);
        if (pc.size() < 3) b.append(3 - pc.size(), ' ');
        if (int(j) == Start) pc += "*";
        b += pc + "\t";
        dumpInst(b, Inst[j]);
        b += "\n";
    }
    return string(b);
}

const Inst* Prog::skipNop(uint32 pc) const {
    const syntax::Inst* i = &Inst[pc];
    while (i->Op == InstNop || i->Op == InstCapture) i = &Inst[i->Out];
    return i;
}

std::pair<string, bool> Prog::Prefix() const {
    const syntax::Inst* i = skipNop(uint32(Start));

    // Avoid allocation of buffer if prefix is empty.
    if (opOf(*i) != InstRune || i->Rune.size() != 1) return {{}, i->Op == InstMatch};

    // Have prefix; gather characters.
    std::string buf;
    while (opOf(*i) == InstRune && i->Rune.size() == 1 && !(Flags(i->Arg) & FoldCase) && i->Rune[0] != utf8::RuneError) {
        writeRune(buf, i->Rune[0]);
        i = skipNop(i->Out);
    }
    return {string(buf), i->Op == InstMatch};
}

EmptyOp Prog::StartCond() const {
    EmptyOp flag = 0;
    uint32 pc = uint32(Start);
    const syntax::Inst* i = &Inst[pc];
    for (;;) {
        switch (i->Op) {
        case InstEmptyWidth:
            flag |= EmptyOp(i->Arg);
            break;
        case InstFail:
            return EmptyOp(~EmptyOp(0));
        case InstCapture:
        case InstNop:
            // skip
            break;
        default:
            return flag;
        }
        pc = i->Out;
        i = &Inst[pc];
    }
}

int Inst::MatchRunePos(rune r) const noexcept {
    const auto& R = Rune;
    switch (R.size()) {
    case 0:
        return -1;
    case 1: {
        // Special case: single-rune slice is from literal string, not char
        // class.
        rune r0 = R[0];
        if (r == r0) return 0;
        if (Flags(Arg) & FoldCase) {
            for (rune r1 = unicode::SimpleFold(r0); r1 != r0; r1 = unicode::SimpleFold(r1)) {
                if (r == r1) return 0;
            }
        }
        return -1;
    }
    case 2:
        if (r >= R[0] && r <= R[1]) return 0;
        return -1;
    case 4:
    case 6:
    case 8:
        // Linear search for a few pairs. Should handle ASCII well.
        for (std::size_t j = 0; j < R.size(); j += 2) {
            if (r < R[j]) return -1;
            if (r <= R[j + 1]) return int(j / 2);
        }
        return -1;
    default:
        break;
    }
    // Otherwise binary search.
    std::size_t lo = 0, hi = R.size() / 2;
    while (lo < hi) {
        std::size_t m = (lo + hi) / 2;
        if (rune c = R[2 * m]; c <= r) {
            if (r <= R[2 * m + 1]) return int(m);
            lo = m + 1;
        } else {
            hi = m;
        }
    }
    return -1;
}

bool Inst::MatchEmptyWidth(rune before, rune after) const {
    switch (EmptyOp(Arg)) {
    case EmptyBeginLine: return before == '\n' || before == -1;
    case EmptyEndLine: return after == '\n' || after == -1;
    case EmptyBeginText: return before == -1;
    case EmptyEndText: return after == -1;
    case EmptyWordBoundary: return IsWordChar(before) != IsWordChar(after);
    case EmptyNoWordBoundary: return IsWordChar(before) == IsWordChar(after);
    }
    panic("unknown empty width arg");
}

// ---------------------------------------------------------------------------
// Compiling (compile.go)

namespace {

// A patchList is a list of instruction pointers that need to be filled in
// (patched). Because the pointers haven't been filled in yet, we can reuse
// their storage to hold the list. It's kind of sleazy, but works well in
// practice. See https://swtch.com/~rsc/regexp/regexp1.html for inspiration.
//
// These aren't really pointers: they're integers, so we can reinterpret
// them this way without using package unsafe. A value l.head denotes
// p.inst[l.head>>1].Out (l.head&1==0) or .Arg (l.head&1==1). head == 0
// denotes the empty list, okay because we start every program with a fail
// instruction, so we'll never want to point at its output link.
struct patchList {
    uint32 head = 0, tail = 0;

    static patchList make(uint32 n) { return {n, n}; }

    void patch(Prog& p, uint32 val) const {
        uint32 h = head;
        while (h != 0) {
            Inst& i = p.Inst[h >> 1];
            if ((h & 1) == 0) {
                h = i.Out;
                i.Out = val;
            } else {
                h = i.Arg;
                i.Arg = val;
            }
        }
    }

    patchList append(Prog& p, patchList l2) const {
        if (head == 0) return l2;
        if (l2.head == 0) return *this;
        Inst& i = p.Inst[tail >> 1];
        if ((tail & 1) == 0) {
            i.Out = l2.head;
        } else {
            i.Arg = l2.head;
        }
        return {head, l2.tail};
    }
};

// A frag represents a compiled program fragment.
struct frag {
    uint32 i = 0;        // index of first instruction
    patchList out;       // where to record end instruction
    bool nullable = false;  // whether fragment can match empty string
};

class compiler {
public:
    ptr<Prog> p = make_ptr<Prog>();

    compiler() {
        p->NumCap = 2;  // implicit ( and ) for whole match $0
        inst(InstFail);
    }

    frag compile(const Regexp& re) {
        switch (re.Op) {
        case OpNoMatch:
            return fail();
        case OpEmptyMatch:
            return nop();
        case OpLiteral: {
            if (re.Rune.empty()) return nop();
            frag f;
            for (std::size_t j = 0; j < re.Rune.size(); j++) {
                frag f1 = rune({re.Rune[j]}, re.Flags);
                f = j == 0 ? f1 : cat(f, f1);
            }
            return f;
        }
        case OpCharClass:
            return rune(re.Rune, re.Flags);
        case OpAnyCharNotNL:
            return rune({0, '\n' - 1, '\n' + 1, unicode::MaxRune}, 0);
        case OpAnyChar:
            return rune({0, unicode::MaxRune}, 0);
        case OpBeginLine:
            return empty(EmptyBeginLine);
        case OpEndLine:
            return empty(EmptyEndLine);
        case OpBeginText:
            return empty(EmptyBeginText);
        case OpEndText:
            return empty(EmptyEndText);
        case OpWordBoundary:
            return empty(EmptyWordBoundary);
        case OpNoWordBoundary:
            return empty(EmptyNoWordBoundary);
        case OpCapture: {
            frag bra = cap(uint32(re.Cap << 1));
            frag sub = compile(*re.Sub[0]);
            frag ket = cap(uint32(re.Cap << 1 | 1));
            return cat(cat(bra, sub), ket);
        }
        case OpStar:
            return star(compile(*re.Sub[0]), re.Flags & NonGreedy);
        case OpPlus:
            return plus(compile(*re.Sub[0]), re.Flags & NonGreedy);
        case OpQuest:
            return quest(compile(*re.Sub[0]), re.Flags & NonGreedy);
        case OpConcat: {
            if (re.Sub.empty()) return nop();
            frag f;
            for (std::size_t i = 0; i < re.Sub.size(); i++) {
                f = i == 0 ? compile(*re.Sub[i]) : cat(f, compile(*re.Sub[i]));
            }
            return f;
        }
        case OpAlternate: {
            frag f;
            for (const auto& sub : re.Sub) f = alt(f, compile(*sub));
            return f;
        }
        default:
            panic("regexp: unhandled case in compile");
        }
    }

    frag inst(InstOp op) {
        frag f{uint32(p->Inst.size()), {}, true};
        p->Inst.push_back(Inst{op, 0, 0, {}});
        return f;
    }

    frag nop() {
        frag f = inst(InstNop);
        f.out = patchList::make(f.i << 1);
        return f;
    }

    frag fail() { return frag{}; }

    frag cap(uint32 arg) {
        frag f = inst(InstCapture);
        f.out = patchList::make(f.i << 1);
        p->Inst[f.i].Arg = arg;
        if (p->NumCap < int(arg) + 1) p->NumCap = int(arg) + 1;
        return f;
    }

    frag cat(frag f1, frag f2) {
        // concat of failure is failure
        if (f1.i == 0 || f2.i == 0) return frag{};
        // TODO: elide nop
        f1.out.patch(*p, f2.i);
        return frag{f1.i, f2.out, f1.nullable && f2.nullable};
    }

    frag alt(frag f1, frag f2) {
        // alt of failure is other
        if (f1.i == 0) return f2;
        if (f2.i == 0) return f1;
        frag f = inst(InstAlt);
        Inst& i = p->Inst[f.i];
        i.Out = f1.i;
        i.Arg = f2.i;
        f.out = f1.out.append(*p, f2.out);
        f.nullable = f1.nullable || f2.nullable;
        return f;
    }

    frag quest(frag f1, bool nongreedy) {
        frag f = inst(InstAlt);
        Inst& i = p->Inst[f.i];
        if (nongreedy) {
            i.Arg = f1.i;
            f.out = patchList::make(f.i << 1);
        } else {
            i.Out = f1.i;
            f.out = patchList::make(f.i << 1 | 1);
        }
        f.out = f.out.append(*p, f1.out);
        return f;
    }

    // loop returns the fragment for the main loop of a plus or star. For
    // plus, it can be used after changing the entry to f1.i. For star, it
    // can be used directly when f1 can't match an empty string. (When f1
    // can match an empty string, f1* must be implemented as (f1+)? to get
    // the priority match order correct.)
    frag loop(frag f1, bool nongreedy) {
        frag f = inst(InstAlt);
        Inst& i = p->Inst[f.i];
        if (nongreedy) {
            i.Arg = f1.i;
            f.out = patchList::make(f.i << 1);
        } else {
            i.Out = f1.i;
            f.out = patchList::make(f.i << 1 | 1);
        }
        f1.out.patch(*p, f.i);
        return f;
    }

    frag star(frag f1, bool nongreedy) {
        if (f1.nullable) {
            // Use (f1+)? to get priority match order correct. See golang.org/issue/46123.
            return quest(plus(f1, nongreedy), nongreedy);
        }
        return loop(f1, nongreedy);
    }

    frag plus(frag f1, bool nongreedy) { return frag{f1.i, loop(f1, nongreedy).out, f1.nullable}; }

    frag empty(EmptyOp op) {
        frag f = inst(InstEmptyWidth);
        p->Inst[f.i].Arg = uint32(op);
        f.out = patchList::make(f.i << 1);
        return f;
    }

    frag rune(std::vector<gopp::rune> r, Flags flags) {
        frag f = inst(InstRune);
        f.nullable = false;
        Inst& i = p->Inst[f.i];
        flags &= FoldCase;  // only relevant flag is FoldCase
        if (r.size() != 1 || unicode::SimpleFold(r[0]) == r[0]) {
            // and sometimes not even that
            flags &= ~FoldCase;
        }
        i.Arg = uint32(flags);
        f.out = patchList::make(f.i << 1);

        // Special cases for exec machine.
        if (!(flags & FoldCase) && (r.size() == 1 || (r.size() == 2 && r[0] == r[1]))) {
            i.Op = InstRune1;
        } else if (r.size() == 2 && r[0] == 0 && r[1] == unicode::MaxRune) {
            i.Op = InstRuneAny;
        } else if (r.size() == 4 && r[0] == 0 && r[1] == '\n' - 1 && r[2] == '\n' + 1 && r[3] == unicode::MaxRune) {
            i.Op = InstRuneAnyNotNL;
        }
        i.Rune = std::move(r);
        return f;
    }
};

}  // namespace

std::pair<ptr<Prog>, error> Compile(const Regexp& re) {
    compiler c;
    frag f = c.compile(re);
    f.out.patch(*c.p, c.inst(InstMatch).i);
    c.p->Start = int(f.i);
    return {c.p, nullptr};
}

}  // namespace gopp::syntax
