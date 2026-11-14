// Internals shared by the html/template sources: contexts (context.go),
// transitions (transition.go), the escaper (escape.go) and the escaping
// functions.
#pragma once

#include <gopp/html/template.hpp>

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace gopp::html::tmpl::detail {

namespace ttmpl = gopp::text::tmpl;

// ---------------------------------------------------------------------------
// content.go

// The kind of a value's content.
enum contentType : uint8 {
    contentTypePlain,
    contentTypeCSS,
    contentTypeHTML,
    contentTypeHTMLAttr,
    contentTypeJS,
    contentTypeJSStr,
    contentTypeURL,
    contentTypeSrcset,
    // contentTypeUnsafe is used in attr.go for values that affect how
    // embedded content and network messages are formed, vetted, or
    // interpreted; or which credentials network messages carry.
    contentTypeUnsafe,
};

// The arguments as a string, and the type of their content: a single
// content-typed argument keeps its type.
std::pair<std::string, contentType> stringify(std::span<const Value> args);

// ---------------------------------------------------------------------------
// context.go

// The state of the HTML parser: what the escaper knows about where
// output goes.
enum state : uint8 {
    stateText,            // parsed character data
    stateTag,             // before an HTML attribute or the end of a tag
    stateAttrName,        // inside an attribute name
    stateAfterName,       // after an attribute name, before an equals sign
    stateBeforeValue,     // after the equals sign, before the value
    stateHTMLCmt,         // inside an <!-- HTML comment -->
    stateRCDATA,          // inside an RCDATA element (<textarea> or <title>)
    stateAttr,            // inside an HTML attribute whose content is text
    stateURL,             // inside an HTML attribute whose content is a URL
    stateSrcset,          // inside an HTML srcset attribute
    stateJS,              // inside an event handler or script element
    stateJSDqStr,         // inside a JavaScript double quoted string
    stateJSSqStr,         // inside a JavaScript single quoted string
    stateJSTmplLit,       // inside a JavaScript back quoted string
    stateJSRegexp,        // inside a JavaScript regexp literal
    stateJSBlockCmt,      // inside a JavaScript /* block comment */
    stateJSLineCmt,       // inside a JavaScript // line comment
    stateJSHTMLOpenCmt,   // inside a JavaScript <!-- HTML-like comment
    stateJSHTMLCloseCmt,  // inside a JavaScript --> HTML-like comment
    stateCSS,             // inside a <style> element or style attribute
    stateCSSDqStr,        // inside a CSS double quoted string
    stateCSSSqStr,        // inside a CSS single quoted string
    stateCSSDqURL,        // inside a CSS double quoted url("...")
    stateCSSSqURL,        // inside a CSS single quoted url('...')
    stateCSSURL,          // inside a CSS unquoted url(...)
    stateCSSBlockCmt,     // inside a CSS /* block comment */
    stateCSSLineCmt,      // inside a CSS // line comment
    stateError,           // an infectious error state outside any valid HTML/CSS/JS construct
    stateMetaContent,     // inside a <meta> content attribute
    stateMetaContentURL,  // inside a "url=" in a <meta> content attribute
    stateDead,            // after a {{break}} or {{continue}}: nothing is output
};

// The delimiter that ends an attribute value.
enum delim : uint8 {
    delimNone,           // not in an attribute
    delimDoubleQuote,    // "
    delimSingleQuote,    // '
    delimSpaceOrTagEnd,  // unquoted: a space or >
};

// The part of a URL an action is in.
enum urlPart : uint8 {
    urlPartNone,         // not in a URL, or before its scheme
    urlPartPreQuery,     // in the scheme, authority or path
    urlPartQueryOrFrag,  // in the query or fragment
    urlPartUnknown,      // could be either (after a branch)
};

// What a '/' means in JavaScript at this point.
enum jsCtx : uint8 {
    jsCtxRegexp,   // a regexp literal
    jsCtxDivOp,    // a division operator
    jsCtxUnknown,  // could be either
};

// The element whose content is being parsed, for the special ones.
enum element : uint8 {
    elementNone,
    elementScript,    // JavaScript
    elementStyle,     // CSS
    elementTextarea,  // RCDATA
    elementTitle,     // RCDATA
    elementMeta,      // <meta>, for its content attribute
};

// The kind of attribute being parsed.
enum attr : uint8 {
    attrNone,
    attrScript,       // an event handler
    attrScriptType,   // a <script type>
    attrStyle,        // a style attribute
    attrURL,          // a URL
    attrSrcset,       // a srcset
    attrMetaContent,  // a <meta content>
};

std::string_view stateName(state s);
std::string_view delimName(delim d);
std::string_view urlPartName(urlPart u);
std::string_view jsCtxName(jsCtx j);
std::string_view elementName(element e);
std::string_view attrName(attr a);

// The state of the HTML parser at a point in a template: which escaping
// an action there needs.
struct context {
    state st = stateText;
    delim dl = delimNone;
    urlPart up = urlPartNone;
    jsCtx js = jsCtxRegexp;
    // The depth of the braces in the JS template literal substitutions
    // (${...}) the parser is in, innermost last; Go tells nil from empty.
    std::vector<int> jsBraceDepth;
    bool hasJSBraceDepth = false;
    attr at = attrNone;
    element el = elementNone;
    ptr<parse::Node> n;  // for range break/continue
    ptr<Error> err;

    std::string String() const;
    bool eq(const context& d) const;
    // The name of the template's variant for a start in this context.
    std::string mangle(std::string_view templateName) const;
};

bool isComment(state s);
bool isInTag(state s);
bool isInScriptLiteral(state s);

// Go's errorf.
ptr<Error> errorf(ErrorCode k, ptr<parse::Node> node, int line, std::string description);

// %q of b, Go's.
std::string quote(std::string_view b);

// ---------------------------------------------------------------------------
// transition.go

using transitionFn = std::pair<context, std::size_t> (*)(context, std::string_view);
extern const transitionFn transitionFunc[];

std::pair<context, std::size_t> tSpecialTagEnd(context c, std::string_view s);
std::pair<context, std::size_t> tURL(context c, std::string_view s);
std::size_t eatWhiteSpace(std::string_view s, std::size_t i);
// The index of the end tag of tag in s (case-insensitive), or npos.
std::size_t indexTagEnd(std::string_view s, std::string_view tag);

// attr.go: the content type of an attribute's value.
contentType attrType(std::string_view name);

// css.go
bool endsWithCSSKeyword(std::string_view b, std::string_view kw);
bool isCSSNmchar(rune r);
std::string decodeCSS(std::string_view s);
rune hexDecode(std::string_view s);
// c with a single optional CSS space at its start skipped.
std::string_view skipCSSSpace(std::string_view c);
bool isHex(char c);

// js.go
jsCtx nextJSCtx(std::string_view s, jsCtx preceding);
bool isJSIdentPart(rune r);
bool isJSType(std::string_view mimeType);

// ---------------------------------------------------------------------------
// The escaping functions, as the template functions the escaper adds.

extern const std::string_view filterFailsafe;  // "ZgotmplZ"
const FuncMap& funcMap();
std::string stripTags(std::string_view html);
std::string htmlEscaper(std::span<const Value> args);

// ---------------------------------------------------------------------------
// template.go and escape.go

// What associated html templates share (Go's nameSpace). It owns its
// templates and is freed when the reference count they share drops to
// zero.
struct nameSpace {
    std::atomic<unsigned long> refs{0};
    std::vector<Template*> members;  // every template that joined, freed with the set
    sync::Mutex mu;
    std::map<std::string, Template*, std::less<>> set;
    bool escaped = false;
    std::unique_ptr<escaper> esc;

    nameSpace();
    ~nameSpace();
    nameSpace(const nameSpace&) = delete;
    nameSpace& operator=(const nameSpace&) = delete;
    // Makes t, so far on its own, a member: its references move here.
    void join(Template* t);
};

// Escapes the template named name, whose body is node, in tmpl's name
// space (locked).
gopp::error escapeTemplate(Template* tmpl, const ptr<parse::Node>& node, std::string_view name);

// The marker of a template that escaped correctly.
const gopp::error& escapeOK();

// For tests: the context after n's text, starting in c, as an escaper of
// its own computes it.
context escapeTextNode(context c, parse::TextNode* n);
// Makes the pipeline end with the commands in s, merging them with a
// predefined escaper at its end.
void ensurePipelineContains(parse::PipeNode* p, std::vector<std::string> s);
// Whether funcMap()[b](funcMap()[a](x)) == funcMap()[a](x) for all x.
bool redundantFunc(std::string_view a, std::string_view b);

}  // namespace gopp::html::tmpl::detail
