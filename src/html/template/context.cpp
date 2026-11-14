// html/template's contexts and their transitions, ported from Go 1.27.3's
// html/template/context.go, transition.go, attr.go, and the parts of
// css.go and js.go that the transitions use.
#include "html/template/internal.hpp"

#include <gopp/unicode/utf8.hpp>

#include "text/template/goconv.hpp"

#include <algorithm>
#include <format>
#include <unordered_map>

namespace gopp::html::tmpl::detail {

// ---------------------------------------------------------------------------
// context.go

namespace {

constexpr std::string_view stateNames[] = {
    "stateText",        "stateTag",           "stateAttrName",       "stateAfterName",  "stateBeforeValue",
    "stateHTMLCmt",     "stateRCDATA",        "stateAttr",           "stateURL",        "stateSrcset",
    "stateJS",          "stateJSDqStr",       "stateJSSqStr",        "stateJSTmplLit",  "stateJSRegexp",
    "stateJSBlockCmt",  "stateJSLineCmt",     "stateJSHTMLOpenCmt",  "stateJSHTMLCloseCmt",
    "stateCSS",         "stateCSSDqStr",      "stateCSSSqStr",       "stateCSSDqURL",   "stateCSSSqURL",
    "stateCSSURL",      "stateCSSBlockCmt",   "stateCSSLineCmt",     "stateError",      "stateMetaContent",
    "stateMetaContentURL", "stateDead",
};
constexpr std::string_view delimNames[] = {"delimNone", "delimDoubleQuote", "delimSingleQuote", "delimSpaceOrTagEnd"};
constexpr std::string_view urlPartNames[] = {"urlPartNone", "urlPartPreQuery", "urlPartQueryOrFrag", "urlPartUnknown"};
constexpr std::string_view jsCtxNames[] = {"jsCtxRegexp", "jsCtxDivOp", "jsCtxUnknown"};
constexpr std::string_view elementNames[] = {"elementNone",     "elementScript", "elementStyle",
                                             "elementTextarea", "elementTitle",  "elementMeta"};
constexpr std::string_view attrNames[] = {"attrNone", "attrScript", "attrScriptType", "attrStyle",
                                          "attrURL",  "attrSrcset", "attrMetaContent"};

template <std::size_t N>
std::string_view nameOf(const std::string_view (&names)[N], unsigned i, std::string_view type) {
    if (i < N) return names[i];
    static thread_local std::string s;
    s = std::format("{}({})", type, i);
    return s;
}

std::string braceDepth(const std::vector<int>& v) {
    std::string s = "[";
    for (std::size_t i = 0; i < v.size(); i++) {
        if (i) s += ' ';
        s += std::to_string(v[i]);
    }
    return s + "]";
}

}  // namespace

std::string_view stateName(state s) { return nameOf(stateNames, s, "state"); }
std::string_view delimName(delim d) { return nameOf(delimNames, d, "delim"); }
std::string_view urlPartName(urlPart u) { return nameOf(urlPartNames, u, "urlPart"); }
std::string_view jsCtxName(jsCtx j) { return nameOf(jsCtxNames, j, "jsCtx"); }
std::string_view elementName(element e) { return nameOf(elementNames, e, "element"); }
std::string_view attrName(attr a) { return nameOf(attrNames, a, "attr"); }

std::string context::String() const {
    return std::format("{{{} {} {} {} {} {} {} {}}}", stateName(st), delimName(dl), urlPartName(up), jsCtxName(js),
                       braceDepth(jsBraceDepth), attrName(at), elementName(el),
                       err ? err->message().view() : std::string_view("<nil>"));
}

bool context::eq(const context& d) const {
    return st == d.st && dl == d.dl && up == d.up && js == d.js && jsBraceDepth == d.jsBraceDepth && at == d.at &&
           el == d.el && err == d.err;
}

std::string context::mangle(std::string_view templateName) const {
    // The mangled name for the default context is the input templateName.
    if (st == stateText) return std::string(templateName);
    // The mangled name is the input templateName followed by the context
    // in some readable form, as in Go.
    std::string s = std::string(templateName) + "$htmltemplate_" + std::string(stateName(st));
    if (dl != delimNone) s += "_" + std::string(delimName(dl));
    if (up != urlPartNone) s += "_" + std::string(urlPartName(up));
    if (js != jsCtxRegexp) s += "_" + std::string(jsCtxName(js));
    if (hasJSBraceDepth) s += "_jsBraceDepth(" + braceDepth(jsBraceDepth) + ")";
    if (at != attrNone) s += "_" + std::string(attrName(at));
    if (el != elementNone) s += "_" + std::string(elementName(el));
    return s;
}

bool isComment(state s) {
    switch (s) {
        case stateHTMLCmt: case stateJSBlockCmt: case stateJSLineCmt: case stateJSHTMLOpenCmt:
        case stateJSHTMLCloseCmt: case stateCSSBlockCmt: case stateCSSLineCmt:
            return true;
        default:
            return false;
    }
}

bool isInTag(state s) {
    switch (s) {
        case stateTag: case stateAttrName: case stateAfterName: case stateBeforeValue: case stateAttr:
            return true;
        default:
            return false;
    }
}

bool isInScriptLiteral(state s) {
    // Ignore the comment states (stateJSBlockCmt, stateJSLineCmt,
    // stateJSHTMLOpenCmt, stateJSHTMLCloseCmt) because their content is
    // already omitted from the output.
    switch (s) {
        case stateJSDqStr: case stateJSSqStr: case stateJSTmplLit: case stateJSRegexp:
            return true;
        default:
            return false;
    }
}

ptr<Error> errorf(ErrorCode k, ptr<parse::Node> node, int line, std::string description) {
    return make_ptr<Error>(k, std::move(node), string(), line, string(description));
}

std::string quote(std::string_view b) { return text::tmpl::detail::quote(b); }

namespace {

// %.32q: the first 32 runes of b, quoted.
std::string quote32(std::string_view b) {
    std::size_t i = 0;
    for (int n = 0; n < 32 && i < b.size(); n++) i += std::size_t(utf8::DecodeRuneInString(b.substr(i)).second);
    return quote(b.substr(0, i));
}

context errContext(ErrorCode k, std::string description) {
    context c;
    c.st = stateError;
    c.err = errorf(k, nullptr, 0, std::move(description));
    return c;
}

context errContext(ptr<Error> err) {
    context c;
    c.st = stateError;
    c.err = std::move(err);
    return c;
}

bool equalFold(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); i++) {
        char x = a[i], y = b[i];
        if ('A' <= x && x <= 'Z') x = char(x + 'a' - 'A');
        if ('A' <= y && y <= 'Z') y = char(y + 'a' - 'A');
        if (x != y) return false;
    }
    return true;
}

std::string toLower(std::string_view s) {
    std::string r(s);
    for (char& c : r) {
        if ('A' <= c && c <= 'Z') c = char(c + 'a' - 'A');
    }
    return r;
}

std::size_t indexAny(std::string_view s, std::string_view chars) {
    // bytes.IndexAny: chars may hold multi-byte runes, as jsWhitespace and
    // the line terminators do.
    for (std::size_t i = 0; i < s.size();) {
        auto [r, w] = utf8::DecodeRuneInString(s.substr(i));
        for (std::size_t j = 0; j < chars.size();) {
            auto [c, cw] = utf8::DecodeRuneInString(chars.substr(j));
            if (r == c && (r != utf8::RuneError || w == cw)) return i;
            j += std::size_t(cw);
        }
        i += std::size_t(w);
    }
    return std::string_view::npos;
}

}  // namespace

// ---------------------------------------------------------------------------
// transition.go

namespace {

using result = std::pair<context, std::size_t>;

constexpr std::string_view commentStart = "<!--";
constexpr std::string_view commentEnd = "-->";

std::pair<std::size_t, ptr<Error>> eatAttrName(std::string_view s, std::size_t i);
std::pair<std::size_t, element> eatTagName(std::string_view s, std::size_t i);

// The context after s in HTML text.
result tText(context c, std::string_view s) {
    std::size_t k = 0;
    for (;;) {
        std::size_t i = s.find('<', k);
        if (i == s.npos || i + 1 == s.size()) {
            return {c, s.size()};
        } else if (i + 4 <= s.size() && s.substr(i, 4) == commentStart) {
            context d;
            d.st = stateHTMLCmt;
            return {d, i + 4};
        }
        i++;
        bool end = false;
        if (s[i] == '/') {
            if (i + 1 == s.size()) return {c, s.size()};
            end = true;
            i++;
        }
        auto [j, e] = eatTagName(s, i);
        if (j != i) {
            if (end) e = elementNone;
            // We've found an HTML tag.
            context d;
            d.st = stateTag;
            d.el = e;
            return {d, j};
        }
        k = j;
    }
}

constexpr state elementContentType[] = {
    stateText,    // elementNone
    stateJS,      // elementScript
    stateCSS,     // elementStyle
    stateRCDATA,  // elementTextarea
    stateRCDATA,  // elementTitle
    stateText,    // elementMeta
};

// The context after s in an HTML tag.
result tTag(context c, std::string_view s) {
    // Find the attribute name.
    std::size_t i = eatWhiteSpace(s, 0);
    if (i == s.size()) return {c, s.size()};
    if (s[i] == '>') {
        // Treat <meta> specially, because it doesn't have an end tag, and
        // we want to transition into the correct state/element for it.
        context d;
        if (c.el == elementMeta) return {d, i + 1};
        d.st = elementContentType[c.el];
        d.el = c.el;
        return {d, i + 1};
    }
    auto [j, err] = eatAttrName(s, i);
    if (err) return {errContext(err), s.size()};
    state st = stateTag;
    attr a = attrNone;
    if (i == j) {
        return {errContext(ErrBadHTML,
                           "expected space, attr name, or end of tag, but got " + quote(s.substr(i))),
                s.size()};
    }

    std::string name = toLower(s.substr(i, j - i));
    if (c.el == elementScript && name == "type") {
        a = attrScriptType;
    } else if (c.el == elementMeta && name == "content") {
        a = attrMetaContent;
    } else {
        switch (attrType(name)) {
            case contentTypeURL: a = attrURL; break;
            case contentTypeCSS: a = attrStyle; break;
            case contentTypeJS: a = attrScript; break;
            case contentTypeSrcset: a = attrSrcset; break;
            default: break;
        }
    }

    if (j == s.size()) {
        st = stateAttrName;
    } else {
        st = stateAfterName;
    }
    context d;
    d.st = st;
    d.el = c.el;
    d.at = a;
    return {d, j};
}

// The context after s in an attribute name.
result tAttrName(context c, std::string_view s) {
    auto [i, err] = eatAttrName(s, 0);
    if (err) {
        return {errContext(err), s.size()};
    } else if (i != s.size()) {
        c.st = stateAfterName;
    }
    return {c, i};
}

// The context after s after an attribute name.
result tAfterName(context c, std::string_view s) {
    // Look for the start of the value.
    std::size_t i = eatWhiteSpace(s, 0);
    if (i == s.size()) {
        return {c, s.size()};
    } else if (s[i] != '=') {
        // Occurs due to tag ending '>', and valueless attribute.
        c.st = stateTag;
        return {c, i};
    }
    c.st = stateBeforeValue;
    // Consume the "=".
    return {c, i + 1};
}

constexpr state attrStartStates[] = {
    stateAttr,         // attrNone
    stateJS,           // attrScript
    stateAttr,         // attrScriptType
    stateCSS,          // attrStyle
    stateURL,          // attrURL
    stateSrcset,       // attrSrcset
    stateMetaContent,  // attrMetaContent
};

// The context after s before an attribute value.
result tBeforeValue(context c, std::string_view s) {
    std::size_t i = eatWhiteSpace(s, 0);
    if (i == s.size()) return {c, s.size()};
    // Find the attribute delimiter.
    delim d = delimSpaceOrTagEnd;
    switch (s[i]) {
        case '\'': d = delimSingleQuote; i++; break;
        case '"': d = delimDoubleQuote; i++; break;
        default: break;
    }
    c.st = attrStartStates[c.at];
    c.dl = d;
    return {c, i};
}

// The context after s in an HTML comment.
result tHTMLCmt(context c, std::string_view s) {
    if (std::size_t i = s.find(commentEnd); i != s.npos) return {context{}, i + 3};
    return {c, s.size()};
}

// The tag names whose end tags end their raw text content.
constexpr std::string_view specialTagEndMarkers[] = {
    "",          // elementNone
    "script",    // elementScript
    "style",     // elementStyle
    "textarea",  // elementTextarea
    "title",     // elementTitle
    "",          // elementMeta
};

constexpr std::string_view specialTagEndPrefix = "</";
constexpr std::string_view tagEndSeparators = "> \t\n\f/";

}  // namespace

// The index of the end tag of tag in s (case-insensitive), or npos.
std::size_t indexTagEnd(std::string_view s, std::string_view tag) {
    std::size_t res = 0;
    std::size_t plen = specialTagEndPrefix.size();
    while (!s.empty()) {
        // Try to find the tag end prefix first.
        std::size_t i = s.find(specialTagEndPrefix);
        if (i == s.npos) return i;
        s = s.substr(i + plen);
        // Try to match the actual tag if there is still space for it.
        if (tag.size() <= s.size() && equalFold(tag, s.substr(0, tag.size()))) {
            s = s.substr(tag.size());
            // Check the tag is followed by a proper separator.
            if (!s.empty() && tagEndSeparators.find(s[0]) != tagEndSeparators.npos) return res + i;
            res += tag.size();
        }
        res += i + plen;
    }
    return std::string_view::npos;
}

namespace {

// The context after s in an attribute value.
result tAttr(context c, std::string_view s) { return {c, s.size()}; }

// The context after s in JavaScript.
result tJS(context c, std::string_view s) {
    std::size_t i = s.find_first_of("\"`'/{}<-#");
    if (i == s.npos) {
        // Entire input is non string, comment, regexp tokens.
        c.js = nextJSCtx(s, c.js);
        return {c, s.size()};
    }
    c.js = nextJSCtx(s.substr(0, i), c.js);
    switch (s[i]) {
        case '"': c.st = stateJSDqStr; c.js = jsCtxRegexp; break;
        case '\'': c.st = stateJSSqStr; c.js = jsCtxRegexp; break;
        case '`': c.st = stateJSTmplLit; c.js = jsCtxRegexp; break;
        case '/':
            if (i + 1 < s.size() && s[i + 1] == '/') {
                c.st = stateJSLineCmt;
                i++;
            } else if (i + 1 < s.size() && s[i + 1] == '*') {
                c.st = stateJSBlockCmt;
                i++;
            } else if (c.js == jsCtxRegexp) {
                c.st = stateJSRegexp;
            } else if (c.js == jsCtxDivOp) {
                c.js = jsCtxRegexp;
            } else {
                return {errContext(ErrSlashAmbig, "'/' could start a division or regexp: " + quote32(s.substr(i))),
                        s.size()};
            }
            break;
        // ECMAScript supports HTML style comments for legacy reasons, see
        // Appendix B.1.1 "HTML-like Comments". The handling of these
        // comments is somewhat confusing. Multi-line comments are not
        // supported, i.e. anything on lines between the opening and
        // closing tokens is not considered a comment, but anything
        // following the opening or closing token, on the same line, is
        // ignored. As such we simply treat any line prefixed with "<!--" or
        // "-->" as if it were actually prefixed with "//" and move on.
        case '<':
            if (i + 3 < s.size() && s.substr(i, 4) == commentStart) {
                c.st = stateJSHTMLOpenCmt;
                i += 3;
            }
            break;
        case '-':
            if (i + 2 < s.size() && s.substr(i, 3) == commentEnd) {
                c.st = stateJSHTMLCloseCmt;
                i += 2;
            }
            break;
        // ECMAScript also supports "hashbang" comment lines, see Section
        // 12.5.
        case '#':
            if (i + 1 < s.size() && s[i + 1] == '!') {
                c.st = stateJSLineCmt;
                i++;
                break;
            }
            // A private class member: #name; what follows is like after
            // an identifier.
            while (i + 1 < s.size() && isJSIdentPart(rune(byte(s[i + 1])))) i++;
            c.js = jsCtxDivOp;
            break;
        case '{':
            // We only care about tracking brace depth if we are inside of a
            // template literal.
            if (c.jsBraceDepth.empty()) {
                c.js = nextJSCtx(s.substr(i, 1), c.js);
                return {c, i + 1};
            }
            c.jsBraceDepth.back()++;
            c.js = nextJSCtx(s.substr(i, 1), c.js);
            break;
        case '}':
            if (c.jsBraceDepth.empty()) {
                c.js = nextJSCtx(s.substr(i, 1), c.js);
                return {c, i + 1};
            }
            // There are no cases where a brace can be escaped in the JS
            // context (that are not syntax errors), it seems. Because of
            // this we can just count "\}" as "}" and move on, the script is
            // already broken as fully fledged parsers will just fail
            // anyway.
            c.jsBraceDepth.back()--;
            if (c.jsBraceDepth.back() >= 0) {
                c.js = nextJSCtx(s.substr(i, 1), c.js);
                return {c, i + 1};
            }
            c.jsBraceDepth.pop_back();
            c.st = stateJSTmplLit;
            break;
        default:
            panic("unreachable");
    }
    return {c, i + 1};
}

// The context after s in a JS template literal.
result tJSTmpl(context c, std::string_view s) {
    std::size_t k = 0;
    for (;;) {
        std::size_t i = s.find_first_of("`\\$", k);
        if (i == s.npos) break;
        switch (s[i]) {
            case '\\':
                i++;
                if (i == s.size()) {
                    return {errContext(ErrPartialEscape, "unfinished escape sequence in JS string: " + quote(s)),
                            s.size()};
                }
                break;
            case '$':
                if (s.size() >= i + 2 && s[i + 1] == '{') {
                    c.jsBraceDepth.push_back(0);
                    c.hasJSBraceDepth = true;
                    c.st = stateJS;
                    c.js = jsCtxRegexp;
                    return {c, i + 2};
                }
                break;
            case '`':
                // End the JS template literal.
                c.st = stateJS;
                return {c, i + 1};
        }
        k = i + 1;
    }
    return {c, s.size()};
}

// The context after s in a JS string or regexp literal: the delimiter
// depends on the state.
result tJSDelimited(context c, std::string_view s) {
    std::string_view specials = "\\\"";
    switch (c.st) {
        case stateJSSqStr: specials = "\\'"; break;
        case stateJSRegexp: specials = "\\/[]"; break;
        default: break;
    }

    std::size_t k = 0;
    bool inCharset = false;
    for (;;) {
        std::size_t i = s.find_first_of(specials, k);
        if (i == s.npos) break;
        switch (s[i]) {
            case '\\':
                i++;
                if (i == s.size()) {
                    return {errContext(ErrPartialEscape, "unfinished escape sequence in JS string: " + quote(s)),
                            s.size()};
                }
                break;
            case '[':
                inCharset = true;
                break;
            case ']':
                inCharset = false;
                break;
            case '/':
                // If "</script" appears in a regex literal, the '/' should
                // not close the regex literal, and it will later be escaped
                // to "\x3C/script" in escapeText.
                if (i > 0 && i + 7 <= s.size() && equalFold(s.substr(i - 1, 8), "</script")) {
                    i++;
                } else if (!inCharset) {
                    c.st = stateJS;
                    c.js = jsCtxDivOp;
                    return {c, i + 1};
                }
                break;
            default:
                // End delimiter
                if (!inCharset) {
                    c.st = stateJS;
                    c.js = jsCtxDivOp;
                    return {c, i + 1};
                }
        }
        k = i + 1;
    }

    if (inCharset) {
        // This can be fixed by making context richer if interpolation into
        // charsets is desired.
        return {errContext(ErrPartialCharset, "unfinished JS regexp charset: " + quote(s)), s.size()};
    }

    return {c, s.size()};
}

// The context after s in a JS or CSS block comment.
result tBlockCmt(context c, std::string_view s) {
    std::size_t i = s.find("*/");
    if (i == s.npos) return {c, s.size()};
    switch (c.st) {
        case stateJSBlockCmt: c.st = stateJS; break;
        case stateCSSBlockCmt: c.st = stateCSS; break;
        default: panic(std::string(stateName(c.st)));
    }
    return {c, i + 2};
}

// The context after s in a JS or CSS line comment.
result tLineCmt(context c, std::string_view s) {
    std::string_view lineTerminators;
    state endState;
    switch (c.st) {
        case stateJSLineCmt: case stateJSHTMLOpenCmt: case stateJSHTMLCloseCmt:
            lineTerminators = "\n\r\u2028\u2029";
            endState = stateJS;
            break;
        case stateCSSLineCmt:
            lineTerminators = "\n\f\r";
            endState = stateCSS;
            // Line comments are not part of any published CSS standard but
            // are supported by the 4 major browsers.
            break;
        default:
            panic(std::string(stateName(c.st)));
    }

    std::size_t i = indexAny(s, lineTerminators);
    if (i == s.npos) return {c, s.size()};
    c.st = endState;
    // Per section 7.4 of EcmaScript 5 : https://es5.github.io/#x7.4
    // "However, the LineTerminator at the end of the line is not
    // considered to be part of the single-line comment; it is recognized
    // separately by the lexical grammar and becomes part of the stream of
    // input elements for the syntactic grammar."
    return {c, i};
}

// The context after s in CSS.
result tCSS(context c, std::string_view s) {
    // CSS quoted strings are almost never used except for:
    // (1) URLs as in background: "/foo.png"
    // (2) Multiword font-names as in font-family: "Times New Roman"
    // (3) List separators in content values as in inline-lists:
    //    <style>
    //    ul.inlineList { list-style: none; padding:0 }
    //    ul.inlineList > li { display: inline }
    //    ul.inlineList > li:before { content: ", " }
    //    ul.inlineList > li:first-child:before { content: "" }
    //    </style>
    // (4) Attribute value selectors as in a[href="http://example.com/"]
    //
    // We conservatively treat all strings as URLs, but make some
    // allowances to avoid confusion.
    //
    // In (1), our conservative assumption is justified.
    // In (2), valid font names do not contain ':', '?', or '#', so our
    // conservative assumption is fine since we will never transition past
    // urlPartPreQuery.
    // In (3), our protocol heuristic should not be tripped, and there
    // should not be non-space content after a '?' or '#', so as long as
    // we only %-encode RFC 3986 reserved characters we are ok.
    // In (4), we should URL escape for URL attributes, and for others we
    // have the attribute name available if our conservative assumption
    // proves problematic for real code.
    std::size_t k = 0;
    for (;;) {
        std::size_t i = s.find_first_of("(\"'/", k);
        if (i == s.npos) return {c, s.size()};
        switch (s[i]) {
            case '(': {
                // Look for url to the left.
                std::string_view p = s.substr(0, i);
                p = p.substr(0, p.find_last_not_of("\t\n\f\r ") + 1);  // npos + 1 is 0
                if (endsWithCSSKeyword(p, "url")) {
                    std::string_view rest = s.substr(i + 1);
                    std::size_t lead = rest.find_first_not_of("\t\n\f\r ");
                    std::size_t j = lead == rest.npos ? s.size() : i + 1 + lead;
                    if (j != s.size() && s[j] == '"') {
                        c.st = stateCSSDqURL;
                        j++;
                    } else if (j != s.size() && s[j] == '\'') {
                        c.st = stateCSSSqURL;
                        j++;
                    } else {
                        c.st = stateCSSURL;
                    }
                    return {c, j};
                }
                break;
            }
            case '/':
                if (i + 1 < s.size()) {
                    switch (s[i + 1]) {
                        case '/': c.st = stateCSSLineCmt; return {c, i + 2};
                        case '*': c.st = stateCSSBlockCmt; return {c, i + 2};
                    }
                }
                break;
            case '"':
                c.st = stateCSSDqStr;
                return {c, i + 1};
            case '\'':
                c.st = stateCSSSqStr;
                return {c, i + 1};
        }
        k = i + 1;
    }
}

// The context after s in a CSS string or url().
result tCSSStr(context c, std::string_view s) {
    std::string_view endAndEsc;
    switch (c.st) {
        case stateCSSDqStr: case stateCSSDqURL: endAndEsc = "\\\""; break;
        case stateCSSSqStr: case stateCSSSqURL: endAndEsc = "\\'"; break;
        case stateCSSURL:
            // Unquoted URLs end with a newline or close parenthesis. The
            // below includes the wc (whitespace character) and nl.
            endAndEsc = "\\\t\n\f\r )";
            break;
        default:
            panic(std::string(stateName(c.st)));
    }

    std::size_t k = 0;
    for (;;) {
        std::size_t i = s.find_first_of(endAndEsc, k);
        if (i == s.npos) {
            auto [c1, nread] = tURL(c, decodeCSS(s.substr(k)));
            return {c1, k + nread};
        }
        if (s[i] == '\\') {
            i++;
            if (i == s.size()) {
                return {errContext(ErrPartialEscape, "unfinished escape sequence in CSS string: " + quote(s)),
                        s.size()};
            }
        } else {
            c.st = stateCSS;
            return {c, i + 1};
        }
        c = tURL(c, decodeCSS(s.substr(0, i + 1))).first;
        k = i + 1;
    }
}

// The context after s in an error state.
result tError(context c, std::string_view s) { return {c, s.size()}; }

// The context after s in a <meta> content attribute: a url= there starts a
// URL.
result tMetaContent(context c, std::string_view s) {
    for (std::size_t i = 0; i < s.size(); i++) {
        if (i + 3 + 1 <= s.size() && equalFold(s.substr(i, 3), "url")) {
            if (std::size_t j = eatWhiteSpace(s, i + 3); j < s.size() && s[j] == '=') {
                c.st = stateMetaContentURL;
                return {c, j + 1};
            }
        }
    }
    return {c, s.size()};
}

// The context after s in a URL in a <meta> content attribute; ; ends it.
result tMetaContentURL(context c, std::string_view s) {
    for (std::size_t i = 0; i < s.size(); i++) {
        if (s[i] == ';') {
            c.st = stateMetaContent;
            return {c, i + 1};
        }
    }
    return {c, s.size()};
}

// The end of the attribute name at s[i:], or an error.
std::pair<std::size_t, ptr<Error>> eatAttrName(std::string_view s, std::size_t i) {
    for (std::size_t j = i; j < s.size(); j++) {
        switch (s[j]) {
            case ' ': case '\t': case '\n': case '\f': case '\r': case '=': case '>':
                return {j, nullptr};
            case '\'': case '"': case '<':
                // These result in a parse warning in HTML5 and are
                // indicative of serious problems if seen in an attr name in
                // a template.
                return {std::string_view::npos,
                        errorf(ErrBadHTML, nullptr, 0,
                               quote(s.substr(j, 1)) + " in attribute name: " + quote32(s))};
            default:
                // No-op.
                break;
        }
    }
    return {s.size(), nullptr};
}

bool asciiAlpha(char c) { return ('A' <= c && c <= 'Z') || ('a' <= c && c <= 'z'); }

bool asciiAlphaNum(char c) { return asciiAlpha(c) || ('0' <= c && c <= '9'); }

// The end of the tag name at s[i:], and the element it names.
std::pair<std::size_t, element> eatTagName(std::string_view s, std::size_t i) {
    if (i == s.size() || !asciiAlpha(s[i])) return {i, elementNone};
    std::size_t j = i + 1;
    while (j < s.size()) {
        char x = s[j];
        if (asciiAlphaNum(x)) {
            j++;
            continue;
        }
        // Allow "x-y" or "x:y" but not "x-", "-y", or "x--y".
        if ((x == ':' || x == '-') && j + 1 < s.size() && asciiAlphaNum(s[j + 1])) {
            j += 2;
            continue;
        }
        break;
    }
    std::string name = toLower(s.substr(i, j - i));
    element e = elementNone;
    if (name == "script") e = elementScript;
    else if (name == "style") e = elementStyle;
    else if (name == "textarea") e = elementTextarea;
    else if (name == "title") e = elementTitle;
    else if (name == "meta") e = elementMeta;
    return {j, e};
}

}  // namespace

// The context after s in an RCDATA element or one with raw text: its end
// tag ends it.
std::pair<context, std::size_t> tSpecialTagEnd(context c, std::string_view s) {
    if (c.el != elementNone) {
        // script end tags ("</script") within script literals are ignored,
        // so that we can properly escape them.
        if (c.el == elementScript && (isInScriptLiteral(c.st) || isComment(c.st))) return {c, s.size()};
        if (std::size_t i = indexTagEnd(s, specialTagEndMarkers[c.el]); i != s.npos) return {context{}, i};
    }
    return {c, s.size()};
}

// The context after s in a URL.
std::pair<context, std::size_t> tURL(context c, std::string_view s) {
    if (s.find_first_of("#?") != s.npos) {
        c.up = urlPartQueryOrFrag;
    } else if (s.size() != eatWhiteSpace(s, 0) && c.up == urlPartNone) {
        // HTML5 uses "Valid URL potentially surrounded by spaces" for attrs
        // like href.
        c.up = urlPartPreQuery;
    }
    return {c, s.size()};
}

std::size_t eatWhiteSpace(std::string_view s, std::size_t i) {
    for (std::size_t j = i; j < s.size(); j++) {
        switch (s[j]) {
            case ' ': case '\t': case '\n': case '\f': case '\r':
                // No-op.
                break;
            default:
                return j;
        }
    }
    return s.size();
}

const transitionFn transitionFunc[] = {
    tText,            // stateText
    tTag,             // stateTag
    tAttrName,        // stateAttrName
    tAfterName,       // stateAfterName
    tBeforeValue,     // stateBeforeValue
    tHTMLCmt,         // stateHTMLCmt
    tSpecialTagEnd,   // stateRCDATA
    tAttr,            // stateAttr
    tURL,             // stateURL
    tURL,             // stateSrcset
    tJS,              // stateJS
    tJSDelimited,     // stateJSDqStr
    tJSDelimited,     // stateJSSqStr
    tJSTmpl,          // stateJSTmplLit
    tJSDelimited,     // stateJSRegexp
    tBlockCmt,        // stateJSBlockCmt
    tLineCmt,         // stateJSLineCmt
    tLineCmt,         // stateJSHTMLOpenCmt
    tLineCmt,         // stateJSHTMLCloseCmt
    tCSS,             // stateCSS
    tCSSStr,          // stateCSSDqStr
    tCSSStr,          // stateCSSSqStr
    tCSSStr,          // stateCSSDqURL
    tCSSStr,          // stateCSSSqURL
    tCSSStr,          // stateCSSURL
    tBlockCmt,        // stateCSSBlockCmt
    tLineCmt,         // stateCSSLineCmt
    tError,           // stateError
    tMetaContent,     // stateMetaContent
    tMetaContentURL,  // stateMetaContentURL
    nullptr,          // stateDead
};

// ---------------------------------------------------------------------------
// attr.go

namespace {

// The types of the values of the HTML attributes whose values aren't
// plain text (and of some that are, as documentation).
const std::unordered_map<std::string_view, contentType>& attrTypeMap() {
    static const std::unordered_map<std::string_view, contentType> m = {
        {"accept", contentTypePlain},
        {"accept-charset", contentTypeUnsafe},
        {"action", contentTypeURL},
        {"alt", contentTypePlain},
        {"archive", contentTypeURL},
        {"async", contentTypeUnsafe},
        {"autocomplete", contentTypePlain},
        {"autofocus", contentTypePlain},
        {"autoplay", contentTypePlain},
        {"background", contentTypeURL},
        {"border", contentTypePlain},
        {"checked", contentTypePlain},
        {"cite", contentTypeURL},
        {"challenge", contentTypeUnsafe},
        {"charset", contentTypeUnsafe},
        {"class", contentTypePlain},
        {"classid", contentTypeURL},
        {"codebase", contentTypeURL},
        {"cols", contentTypePlain},
        {"colspan", contentTypePlain},
        {"content", contentTypeUnsafe},
        {"contenteditable", contentTypePlain},
        {"contextmenu", contentTypePlain},
        {"controls", contentTypePlain},
        {"coords", contentTypePlain},
        {"crossorigin", contentTypeUnsafe},
        {"data", contentTypeURL},
        {"datetime", contentTypePlain},
        {"default", contentTypePlain},
        {"defer", contentTypeUnsafe},
        {"dir", contentTypePlain},
        {"dirname", contentTypePlain},
        {"disabled", contentTypePlain},
        {"draggable", contentTypePlain},
        {"dropzone", contentTypePlain},
        {"enctype", contentTypeUnsafe},
        {"for", contentTypePlain},
        {"form", contentTypeUnsafe},
        {"formaction", contentTypeURL},
        {"formenctype", contentTypeUnsafe},
        {"formmethod", contentTypeUnsafe},
        {"formnovalidate", contentTypeUnsafe},
        {"formtarget", contentTypePlain},
        {"headers", contentTypePlain},
        {"height", contentTypePlain},
        {"hidden", contentTypePlain},
        {"high", contentTypePlain},
        {"href", contentTypeURL},
        {"hreflang", contentTypePlain},
        {"http-equiv", contentTypeUnsafe},
        {"icon", contentTypeURL},
        {"id", contentTypePlain},
        {"ismap", contentTypePlain},
        {"keytype", contentTypeUnsafe},
        {"kind", contentTypePlain},
        {"label", contentTypePlain},
        {"lang", contentTypePlain},
        {"language", contentTypeUnsafe},
        {"list", contentTypePlain},
        {"longdesc", contentTypeURL},
        {"loop", contentTypePlain},
        {"low", contentTypePlain},
        {"manifest", contentTypeURL},
        {"max", contentTypePlain},
        {"maxlength", contentTypePlain},
        {"media", contentTypePlain},
        {"mediagroup", contentTypePlain},
        {"method", contentTypeUnsafe},
        {"min", contentTypePlain},
        {"multiple", contentTypePlain},
        {"name", contentTypePlain},
        {"novalidate", contentTypeUnsafe},
        // Skip handler names from
        // https://www.w3.org/TR/html5/webappapis.html#event-handlers-on-elements,-document-objects,-and-window-objects
        // since we have special handling in attrType.
        {"open", contentTypePlain},
        {"optimum", contentTypePlain},
        {"pattern", contentTypeUnsafe},
        {"placeholder", contentTypePlain},
        {"poster", contentTypeURL},
        {"profile", contentTypeURL},
        {"preload", contentTypePlain},
        {"pubdate", contentTypePlain},
        {"radiogroup", contentTypePlain},
        {"readonly", contentTypePlain},
        {"rel", contentTypeUnsafe},
        {"required", contentTypePlain},
        {"reversed", contentTypePlain},
        {"rows", contentTypePlain},
        {"rowspan", contentTypePlain},
        {"sandbox", contentTypeUnsafe},
        {"spellcheck", contentTypePlain},
        {"scope", contentTypePlain},
        {"scoped", contentTypePlain},
        {"seamless", contentTypePlain},
        {"selected", contentTypePlain},
        {"shape", contentTypePlain},
        {"size", contentTypePlain},
        {"sizes", contentTypePlain},
        {"span", contentTypePlain},
        {"src", contentTypeURL},
        {"srcdoc", contentTypeHTML},
        {"srclang", contentTypePlain},
        {"srcset", contentTypeSrcset},
        {"start", contentTypePlain},
        {"step", contentTypePlain},
        {"style", contentTypeCSS},
        {"tabindex", contentTypePlain},
        {"target", contentTypePlain},
        {"title", contentTypePlain},
        {"type", contentTypeUnsafe},
        {"usemap", contentTypeURL},
        {"value", contentTypeUnsafe},
        {"width", contentTypePlain},
        {"wrap", contentTypePlain},
        {"xmlns", contentTypeURL},
    };
    return m;
}

}  // namespace

// The type of the value of the attribute named name, which must be lower
// case.
contentType attrType(std::string_view name) {
    if (name.starts_with("data-")) {
        // Strip data- so that custom attribute heuristics below are widely
        // applied. Treat data-action as URL below.
        name = name.substr(5);
    } else if (std::size_t colon = name.find(':'); colon != name.npos) {
        if (name.substr(0, colon) == "xmlns") return contentTypeURL;
        // Treat svg:href and xlink:href as href below.
        name = name.substr(colon + 1);
    }
    if (auto it = attrTypeMap().find(name); it != attrTypeMap().end()) return it->second;
    // Treat partial event handler names as script.
    if (name.starts_with("on")) return contentTypeJS;

    // Heuristics to prevent "javascript:..." injection in custom data
    // attributes and custom attributes like g:tweetUrl.
    // https://www.w3.org/TR/html5/dom.html#embedding-custom-non-visible-data-with-the-data-*-attributes
    // "Custom data attributes are intended to store custom data private to
    //  the page or application, for which there are no more appropriate
    //  attributes or elements."
    // Developers seem to store URL content in data URLs that start or end
    // with "URI" or "URL".
    if (name.find("src") != name.npos || name.find("uri") != name.npos || name.find("url") != name.npos) {
        return contentTypeURL;
    }
    return contentTypePlain;
}

// ---------------------------------------------------------------------------
// css.go: what the transitions need

// Whether b ends with an ident that case-insensitively matches the lower
// case kw.
bool endsWithCSSKeyword(std::string_view b, std::string_view kw) {
    if (b.size() < kw.size()) return false;
    std::size_t i = b.size() - kw.size();
    if (i != 0) {
        rune r = utf8::DecodeLastRuneInString(b.substr(0, i)).first;
        if (isCSSNmchar(r)) return false;
    }
    // Many CSS keywords, such as "!important" can have characters encoded,
    // but the URI production does not allow that according to
    // https://www.w3.org/TR/css3-syntax/#TOK-URI
    // This does not attempt to recognize encoded keywords. For example,
    // given "\75\72\6c" and "url" this return false.
    return toLower(b.substr(i)) == kw;
}

// Whether rune is allowed anywhere in a CSS identifier.
bool isCSSNmchar(rune r) {
    // Based on the CSS3 nmchar production but ignores multi-rune escape
    // sequences.
    // https://www.w3.org/TR/css3-syntax/#SUBTOK-nmchar
    return ('a' <= r && r <= 'z') || ('A' <= r && r <= 'Z') || ('0' <= r && r <= '9') || r == '-' || r == '_' ||
           // Non-ASCII cases below.
           (0x80 <= r && r <= 0xd7ff) || (0xe000 <= r && r <= 0xfffd) || (0x10000 <= r && r <= 0x10ffff);
}

bool isHex(char c) { return ('0' <= c && c <= '9') || ('a' <= c && c <= 'f') || ('A' <= c && c <= 'F'); }

rune hexDecode(std::string_view s) {
    rune n = 0;
    for (char c : s) {
        n <<= 4;
        if ('0' <= c && c <= '9') n |= rune(c - '0');
        else if ('a' <= c && c <= 'f') n |= rune(c - 'a') + 10;
        else if ('A' <= c && c <= 'F') n |= rune(c - 'A') + 10;
        else panic("Bad hex digit in " + quote(s));
    }
    return n;
}

// c with a single optional CSS space at its start skipped.
std::string_view skipCSSSpace(std::string_view c) {
    if (c.empty()) return c;
    // wc ::= #x9 | #xA | #xC | #xD | #x20
    switch (c[0]) {
        case '\t': case '\n': case '\f': case ' ':
            return c.substr(1);
        case '\r':
            // This differs from CSS3's wc production because it contains a
            // probable spec error whereby wc contains all the single byte
            // sequences in nl (newline) but not CRLF.
            if (c.size() >= 2 && c[1] == '\n') return c.substr(2);
            return c.substr(1);
    }
    return c;
}

// The text that the CSS escapes in s stand for.
std::string decodeCSS(std::string_view s) {
    if (s.find('\\') == s.npos) return std::string(s);
    // The UTF-8 sequence for a codepoint is never longer than 1 + the
    // number hex digits need to represent that codepoint, so len(s) is an
    // upper bound on the output length.
    std::string b;
    b.reserve(s.size());
    while (!s.empty()) {
        std::size_t i = s.find('\\');
        if (i == s.npos) i = s.size();
        b += s.substr(0, i);
        s = s.substr(i);
        if (s.size() < 2) break;
        // https://www.w3.org/TR/css3-syntax/#SUBTOK-escape
        // escape ::= unicode | '\' [#x20-#x7E#x80-#xD7FF#xE000-#xFFFD#x10000-#x10FFFF]
        if (isHex(s[1])) {
            // https://www.w3.org/TR/css3-syntax/#SUBTOK-unicode
            //   unicode ::= '\' [0-9a-fA-F]{1,6} wc?
            std::size_t j = 2;
            while (j < s.size() && j < 7 && isHex(s[j])) j++;
            rune r = hexDecode(s.substr(1, j - 1));
            if (r > utf8::MaxRune) {
                r /= 16;
                j--;
            }
            byte buf[utf8::UTFMax];
            int n = utf8::EncodeRune(buf, r);
            b.append(reinterpret_cast<const char*>(buf), std::size_t(n));
            s = skipCSSSpace(s.substr(j));
        } else {
            // "\\" decodes to "\" and "\"" to "\"".
            auto n = std::size_t(utf8::DecodeRuneInString(s.substr(1)).second);
            b += s.substr(1, n);
            s = s.substr(1 + n);
        }
    }
    return b;
}

// ---------------------------------------------------------------------------
// js.go: what the transitions need

namespace {

// The JavaScript white space characters: ECMAScript's WhiteSpace and
// LineTerminator.
constexpr std::string_view jsWhitespace =
    "\f\n\r\t\v \u00a0\u1680\u2000\u2001\u2002\u2003\u2004\u2005\u2006\u2007\u2008\u2009\u200a\u2028\u2029"
    "\u202f\u205f\u3000\ufeff";

std::string_view trimRightJSWhitespace(std::string_view s) {
    while (!s.empty()) {
        auto [r, w] = utf8::DecodeLastRuneInString(s);
        if (r == utf8::RuneError && w == 1) break;
        if (indexAny(s.substr(s.size() - std::size_t(w)), jsWhitespace) == std::string_view::npos) break;
        s = s.substr(0, s.size() - std::size_t(w));
    }
    return s;
}

// The keywords after which a '/' starts a regexp.
bool regexpPrecederKeyword(std::string_view s) {
    static constexpr std::string_view kws[] = {"break",   "case",       "continue", "delete", "do",
                                               "else",    "finally",    "in",       "instanceof",
                                               "return",  "throw",      "try",      "typeof", "void",
                                               "yield"};
    return std::find(std::begin(kws), std::end(kws), s) != std::end(kws);
}

}  // namespace

// The JS context a '/' after s has: whether it would start a regexp or be
// a division operator.
jsCtx nextJSCtx(std::string_view s, jsCtx preceding) {
    // Trim all JS whitespace characters.
    s = trimRightJSWhitespace(s);
    if (s.empty()) return preceding;

    // All cases below are in the single-byte UTF-8 group.
    char c = s.back();
    std::size_t n = s.size();
    switch (c) {
        case '+': case '-': {
            // ++ and -- are not regexp preceders, but + and - are whether
            // they are used as infix or prefix operators.
            std::size_t start = n - 1;
            // Count the number of adjacent dashes or pluses.
            while (start > 0 && s[start - 1] == c) start--;
            if (((n - start) & 1) == 1) {
                // Reached for trailing minus signs since "---" is the same
                // as "-- -".
                return jsCtxRegexp;
            }
            return jsCtxDivOp;
        }
        case '.':
            // Handle "42."
            if (n != 1 && '0' <= s[n - 2] && s[n - 2] <= '9') return jsCtxDivOp;
            return jsCtxRegexp;
        // Suffixes for all punctuators from section 7.7 of the language
        // spec that only end binary operators not handled above.
        case ',': case '<': case '>': case '=': case '*': case '%': case '&': case '|': case '^': case '?':
            return jsCtxRegexp;
        // Suffixes for all punctuators from section 7.7 of the language
        // spec that are prefix operators not handled above.
        case '!': case '~':
            return jsCtxRegexp;
        // Matches all the punctuators from section 7.7 of the language spec
        // that are open brackets not handled above.
        case '(': case '[':
            return jsCtxRegexp;
        // Matches all the punctuators from section 7.7 of the language spec
        // that precede expression starts.
        case ':': case ';': case '{':
            return jsCtxRegexp;
        // CAVEAT: the close punctuators ('}', ']', ')') precede div ops and
        // are handled in the default except for '}' which can precede a
        // division op as in
        //    ({ valueOf: function () { return 42 } } / 2
        // which is valid, but, in practice, developers don't divide object
        // literals, so our heuristic works well for code like
        //    function () { ... }  /foo/.test(x) && sideEffect();
        // The ')' punctuator can precede a regular expression as in
        //     if (b) /foo/.test(x) && ...
        // but this is much less likely than
        //     (a + b) / c
        case '}':
            return jsCtxRegexp;
        default: {
            // Look for an IdentifierName and see if it is a keyword that
            // can precede a regular expression.
            std::size_t j = n;
            while (j > 0 && isJSIdentPart(rune(byte(s[j - 1])))) j--;
            if (regexpPrecederKeyword(s.substr(j))) {
                // Exclude property accesses like "x.return".
                std::string_view before = trimRightJSWhitespace(s.substr(0, j));
                if (!before.ends_with(".")) return jsCtxRegexp;
            }
        }
    }
    // Otherwise is a punctuator not listed above, or a string which
    // precedes a div op, or an identifier which precedes a div op.
    return jsCtxDivOp;
}

// Whether r can appear in an identifier part. It is conservative and
// ignores non-ASCII letters.
bool isJSIdentPart(rune r) {
    return r == '$' || ('0' <= r && r <= '9') || ('A' <= r && r <= 'Z') || r == '_' || ('a' <= r && r <= 'z');
}

// Whether mimeType is a JavaScript one (or JSON, which is JavaScript
// here), so that a <script> of that type holds JavaScript.
bool isJSType(std::string_view mimeType) {
    // per
    //   https://www.w3.org/TR/html5/scripting-1.html#attr-script-type
    //   https://tools.ietf.org/html/rfc7231#section-3.1.1
    //   https://tools.ietf.org/html/rfc4329#section-3
    //   https://www.ietf.org/rfc/rfc4627.txt
    // discard parameters
    mimeType = mimeType.substr(0, mimeType.find(';'));
    std::string m = toLower(mimeType);
    std::string_view v = m;
    constexpr std::string_view space = " \t\n\v\f\r";
    v.remove_prefix(std::min(v.find_first_not_of(space), v.size()));
    v = v.substr(0, v.find_last_not_of(space) + 1);
    static constexpr std::string_view types[] = {
        "",
        "application/ecmascript",
        "application/javascript",
        "application/json",
        "application/ld+json",
        "application/x-ecmascript",
        "application/x-javascript",
        "module",
        "text/ecmascript",
        "text/javascript",
        "text/javascript1.0",
        "text/javascript1.1",
        "text/javascript1.2",
        "text/javascript1.3",
        "text/javascript1.4",
        "text/javascript1.5",
        "text/jscript",
        "text/livescript",
        "text/x-ecmascript",
        "text/x-javascript",
    };
    return std::find(std::begin(types), std::end(types), v) != std::end(types);
}

}  // namespace gopp::html::tmpl::detail
