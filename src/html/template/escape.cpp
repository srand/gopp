// html/template's escaper, ported from Go 1.27.3's html/template/escape.go:
// it walks the parse trees, computing the context of each action, and
// rewrites the pipelines to end in the escaping functions those contexts
// need.
#include "html/template/internal.hpp"

#include <gopp/html.hpp>
#include <gopp/regexp.hpp>
#include <gopp/unicode/utf8.hpp>

#include <set>

namespace gopp::html::tmpl::detail {

// What the escaper knows about a range loop it is in (Go's rangeContext).
struct rangeInfo {
    std::shared_ptr<rangeInfo> outer;  // outer loop
    std::vector<context> breaks;       // context at each break action
    std::vector<context> continues;    // context at each continue action
};

// escaper collects type inferences about templates and changes needed to
// make templates injection safe.
struct escaper {
    // ns is the nameSpace that this escaper is associated with.
    nameSpace* ns;
    // output[templateName] is the output context for a templateName that
    // has been mangled to include its input context.
    std::map<std::string, context, std::less<>> output;
    // derived[c.mangle(name)] maps to a template derived from the template
    // named name templateName for the start context c.
    std::map<std::string, ptr<ttmpl::Template>, std::less<>> derived;
    // called[templateName] is a set of called mangled template names.
    std::set<std::string, std::less<>> called;
    // xxxNodeEdits are the accumulated edits to apply during commit. Such
    // edits are not applied immediately in case a template set executes a
    // given template in different escaping contexts.
    std::map<parse::ActionNode*, std::vector<std::string>> actionNodeEdits;
    std::map<parse::TemplateNode*, std::string> templateNodeEdits;
    std::map<parse::TextNode*, std::string> textNodeEdits;
    // rangeContext holds context about the current range loop.
    std::shared_ptr<rangeInfo> rangeContext;

    explicit escaper(nameSpace* n) : ns(n) {}

    context escape(context c, parse::Node* n);
    context escapeAction(context c, parse::ActionNode* n);
    context escapeBranch(context c, parse::BranchNode* n, std::string_view nodeName);
    context escapeList(context c, parse::ListNode* n);
    template <typename F>
    std::pair<context, bool> escapeListConditionally(context c, parse::ListNode* n, F&& filter);
    context escapeTemplate(context c, parse::TemplateNode* n);
    std::pair<context, std::string> escapeTree(context c, const ptr<parse::Node>& node, std::string_view name,
                                               int line);
    context computeOutCtx(context c, const ptr<ttmpl::Template>& t);
    std::pair<context, bool> escapeTemplateBody(const context& c, const ptr<ttmpl::Template>& t);
    context escapeText(context c, parse::TextNode* n);
    void editActionNode(parse::ActionNode* n, std::vector<std::string> cmds);
    void editTemplateNode(parse::TemplateNode* n, std::string callee);
    void editTextNode(parse::TextNode* n, std::string text);
    void commit();
    ptr<ttmpl::Template> templateNamed(std::string_view name);
    Template* arbitraryTemplate();
};

namespace {

const std::set<std::string_view>& predefinedEscapers() {
    static const std::set<std::string_view> m = {"html", "urlquery"};
    return m;
}

// The escapers that are equivalent to the predefined ones: they may be
// used in their place.
std::string_view normalizeEscFn(std::string_view e) {
    if (e == "_html_template_attrescaper" || e == "_html_template_htmlescaper" || e == "_html_template_rcdataescaper") {
        return "html";
    }
    if (e == "_html_template_urlescaper" || e == "_html_template_urlnormalizer") return "urlquery";
    return e;
}

// Whether the two escapers are equivalent.
bool escFnsEq(std::string_view a, std::string_view b) { return normalizeEscFn(a) == normalizeEscFn(b); }

}  // namespace

// redundantFuncs[a][b] implies that funcMap[b](funcMap[a](x)) ==
// funcMap[a](x) for all x.
bool redundantFunc(std::string_view a, std::string_view b) {
    if (a == "_html_template_commentescaper") {
        return b == "_html_template_attrescaper" || b == "_html_template_htmlescaper";
    }
    if (a == "_html_template_cssescaper" || a == "_html_template_jsregexpescaper" ||
        a == "_html_template_jsstrescaper" || a == "_html_template_jstmpllitescaper") {
        return b == "_html_template_attrescaper";
    }
    if (a == "_html_template_urlescaper") return b == "_html_template_urlnormalizer";
    return false;
}

namespace {

const parse::IdentifierNode* asIdentifier(const ptr<parse::Node>& n) {
    return n && n->NodeType == parse::NodeIdentifier ? static_cast<const parse::IdentifierNode*>(n.get()) : nullptr;
}

// Appends a command to the end of a command list unless it's redundant
// with the last command.
void appendCmd(std::vector<ptr<parse::CommandNode>>& cmds, ptr<parse::CommandNode> cmd) {
    if (!cmds.empty()) {
        const auto* last = asIdentifier(cmds.back()->Args[0]);
        const auto* next = asIdentifier(cmd->Args[0]);
        if (last && next && redundantFunc(last->Ident, next->Ident)) return;
    }
    cmds.push_back(std::move(cmd));
}

ptr<parse::IdentifierNode> newIdent(std::string_view identifier, parse::Pos pos) {
    auto id = parse::NewIdentifier(string(identifier));
    id->SetTree(nullptr)->SetPos(pos);  // TODO: SetTree.
    return id;
}

// A command consisting of a single identifier.
ptr<parse::CommandNode> newIdentCmd(std::string_view identifier, parse::Pos pos) {
    auto cmd = make_ptr<parse::CommandNode>();
    cmd->NodeType = parse::NodeCommand;
    cmd->Args.push_back(newIdent(identifier, pos));
    return cmd;
}

}  // namespace

// Ensures that the pipeline ends with the commands with the identifiers in
// s in order. If the pipeline ends with a predefined escaper (i.e. "html"
// or "urlquery"), merge it with the identifiers in s.
void ensurePipelineContains(parse::PipeNode* p, std::vector<std::string> s) {
    // This method rewrites the pipeline p by appending escaping functions
    // in s. The predefined escapers "html" and "urlquery" are treated as
    // special: a predefined escaper at the end of the pipeline merges into
    // the escaper list s.
    if (s.empty()) {
        // Do not rewrite pipeline if we have no escapers to insert.
        return;
    }
    // Rewrite the pipeline, creating the escapers in s at the end of the
    // pipeline.
    std::size_t pipelineLen = p->Cmds.size();
    if (pipelineLen > 0) {
        auto lastCmd = p->Cmds[pipelineLen - 1];
        if (const auto* idNode = asIdentifier(lastCmd->Args[0])) {
            std::string esc(idNode->Ident.view());
            if (predefinedEscapers().contains(esc)) {
                // Pipeline ends with a predefined escaper.
                if (p->Cmds.size() == 1 && lastCmd->Args.size() > 1) {
                    // Special case: pipeline is of the form {{esc arg1
                    // arg2 ... argN}}, where esc is the predefined escaper,
                    // and arg1...argN are its arguments. Convert this into
                    // the equivalent form {{_eval_args_ arg1 arg2 ... argN
                    // | esc}}, so that esc can be easily merged with the
                    // escapers in s.
                    lastCmd->Args[0] = newIdent("_eval_args_", lastCmd->Args[0]->Position());
                    appendCmd(p->Cmds, newIdentCmd(esc, p->Position()));
                    pipelineLen++;
                }
                // If any of the commands in s that we are about to insert
                // is equivalent to the predefined escaper, use the
                // predefined escaper instead.
                bool dup = false;
                for (auto& escaper : s) {
                    if (escFnsEq(esc, escaper)) {
                        escaper = esc;
                        dup = true;
                    }
                }
                if (dup) {
                    // The predefined escaper will already be inserted along
                    // with the escapers in s, so do not copy it to the
                    // rewritten pipeline.
                    pipelineLen--;
                }
            }
        }
    }
    // Rewrite the pipeline, creating the escapers in s at the end of the
    // pipeline.
    std::vector<ptr<parse::CommandNode>> newCmds(p->Cmds.begin(), p->Cmds.begin() + std::ptrdiff_t(pipelineLen));
    std::set<std::string, std::less<>> insertedIdents;
    for (const auto& cmd : newCmds) {
        if (const auto* idNode = asIdentifier(cmd->Args[0])) insertedIdents.emplace(normalizeEscFn(idNode->Ident));
    }
    for (const auto& name : s) {
        if (!insertedIdents.contains(normalizeEscFn(name))) {
            // When two templates share an underlying parse tree via the use
            // of AddParseTree and one template is executed after the
            // other, this check ensures that escapers that were already
            // inserted into the pipeline on the first escaping pass do not
            // get inserted again.
            appendCmd(newCmds, newIdentCmd(name, p->Position()));
        }
    }
    p->Cmds = std::move(newCmds);
}

namespace {

constexpr state attrStartStates[] = {
    stateAttr,         // attrNone
    stateJS,           // attrScript
    stateAttr,         // attrScriptType
    stateCSS,          // attrStyle
    stateURL,          // attrURL
    stateSrcset,       // attrSrcset
    stateMetaContent,  // attrMetaContent
};

// Returns the context that would result from following empty string
// transitions from the input context. For example, parsing:
//
//	`<a href=`
//
// will end in context{stateBeforeValue, attrURL}, but parsing one extra
// rune:
//
//	`<a href=x`
//
// will end in context{stateURL, delimSpaceOrTagEnd, ...}.
// There are two transitions that happen when the 'x' is seen:
// (1) Transition from a before-value state to a start-of-value state
// without consuming any character.
// (2) Consume 'x' and transition past the first value character.
// In this case, nudging produces the context after (1) happens.
context nudge(context c) {
    switch (c.st) {
        case stateTag:
            // In `<foo {{.}}`, the action should emit an attribute.
            c.st = stateAttrName;
            break;
        case stateBeforeValue:
            // In `<foo bar={{.}}`, the action is an undelimited value.
            c.st = attrStartStates[c.at];
            c.dl = delimSpaceOrTagEnd;
            c.at = attrNone;
            break;
        case stateAfterName:
            // In `<foo bar {{.}}`, the action is an attribute name.
            c.st = stateAttrName;
            c.at = attrNone;
            break;
        default:
            break;
    }
    return c;
}

// The context after a {{break}} or {{continue}}.
context deadContext() {
    context c;
    c.st = stateDead;
    return c;
}

context errorContext(ptr<Error> err) {
    context c;
    c.st = stateError;
    c.err = std::move(err);
    return c;
}

// Produces a context after which all following text means the same thing
// as it would after a or b: the context of a branch's end.
context join(const context& a, const context& b, const ptr<parse::Node>& node, std::string_view nodeName) {
    if (a.st == stateError) return a;
    if (b.st == stateError) return b;
    if (a.st == stateDead) return b;
    if (b.st == stateDead) return a;
    if (a.eq(b)) return a;

    context c = a;
    c.up = b.up;
    if (c.eq(b)) {
        // The contexts differ only by urlPart.
        c.up = urlPartUnknown;
        return c;
    }

    c = a;
    c.js = b.js;
    if (c.eq(b)) {
        // The contexts differ only by jsCtx.
        c.js = jsCtxUnknown;
        return c;
    }

    // Allow a nudged context to join with an unnudged one. This means that
    //   <p title={{if .C}}{{.}}{{end}}
    // ends in an unquoted value state even though the else branch ends in
    // stateBeforeValue.
    context cn = nudge(a), dn = nudge(b);
    if (!(cn.eq(a) && dn.eq(b))) {
        if (context e = join(cn, dn, node, nodeName); e.st != stateError) return e;
    }

    return errorContext(errorf(ErrBranchEnd, node, 0,
                               "{{" + std::string(nodeName) + "}} branches end in different contexts: " + a.String() +
                                   ", " + b.String()));
}

int nodeLine(const parse::Node* n) {
    if (n->NodeType == parse::NodeBreak) return static_cast<const parse::BreakNode*>(n)->Line;
    if (n->NodeType == parse::NodeContinue) return static_cast<const parse::ContinueNode*>(n)->Line;
    return 0;
}

// Joins the contexts at the breaks and continues of a range loop into c0.
context joinRange(context c0, const rangeInfo& rc) {
    // Merge contexts at break and continue statements into overall body
    // context. In Go 1.10, any break or continue in a range loop could
    // produce an escape context different from the body's.
    for (const auto& c : rc.breaks) {
        c0 = join(c0, c, c.n, "range");
        if (c0.st == stateError) {
            c0.err->Line = nodeLine(c.n.get());
            c0.err->Description = "at range loop break: " + c0.err->Description;
            return c0;
        }
    }
    for (const auto& c : rc.continues) {
        c0 = join(c0, c, c.n, "range");
        if (c0.st == stateError) {
            c0.err->Line = nodeLine(c.n.get());
            c0.err->Description = "at range loop continue: " + c0.err->Description;
            return c0;
        }
    }
    return c0;
}

// The text that ends each kind of attribute value.
constexpr std::string_view delimEnds[] = {
    "",           // delimNone
    "\"",         // delimDoubleQuote
    "'",          // delimSingleQuote
    // Determined empirically by running the below in various browsers.
    // var div = document.createElement("DIV");
    // for (var i = 0; i < 0x10000; ++i) {
    //   div.innerHTML = "<span title=x" + String.fromCharCode(i) + "-bar>";
    //   if (div.getElementsByTagName("SPAN")[0].title.indexOf("bar") < 0)
    //     document.write("<p>U+" + i.toString(16));
    // }
    " \t\n\f\r>",  // delimSpaceOrTagEnd
};

// specialScriptTagRE matches tags that need escaping in script literals.
const ptr<regexp::Regexp>& specialScriptTagRE() {
    static const ptr<regexp::Regexp> re = regexp::MustCompile("(?i)<(script|/script|!--)");
    return re;
}

constexpr std::string_view specialScriptTagReplacement = "\\x3C$1";

bool containsSpecialScriptTag(std::string_view s) { return specialScriptTagRE()->MatchString(s); }

std::string escapeSpecialScriptTags(std::string_view s) {
    return specialScriptTagRE()->ReplaceAllString(string(s), specialScriptTagReplacement).str();
}

bool hasDoctypePrefix(std::string_view s) {
    constexpr std::string_view doctype = "<!DOCTYPE";
    if (s.size() < doctype.size()) return false;
    for (std::size_t i = 0; i < doctype.size(); i++) {
        char c = s[i];
        if ('a' <= c && c <= 'z') c = char(c - 'a' + 'A');
        if (c != doctype[i]) return false;
    }
    return true;
}

bool containsLineTerminator(std::string_view s) {
    return s.find_first_of("\n\r") != s.npos || s.find("\u2028") != s.npos || s.find("\u2029") != s.npos;
}

// Starting in context c, the context after s, and the number of bytes of
// s consumed: up to the end of an attribute value, or of an element's raw
// text.
std::pair<context, std::size_t> contextAfterText(context c, std::string_view s) {
    if (c.dl == delimNone) {
        auto [c1, i] = tSpecialTagEnd(c, s);
        if (i == 0) {
            // A special end tag (`</script>`) has been seen and all content
            // preceding it has been consumed.
            return {c1, 0};
        }
        // Consider all content up to any end tag.
        return transitionFunc[c.st](c, s.substr(0, i));
    }

    // We are at the beginning of an attribute value.

    std::size_t i = s.find_first_of(delimEnds[c.dl]);
    if (i == s.npos) i = s.size();
    if (c.dl == delimSpaceOrTagEnd) {
        // https://www.w3.org/TR/html5/syntax.html#attribute-value-(unquoted)-state
        // lists the runes below as error characters.
        // Error out because HTML parsers may differ on whether
        // "<a id= onclick=f("     ends inside id's or onclick's value,
        // "<a class=`foo "        ends inside a value,
        // "<a style=font:'Arial'" needs open-quote fixup.
        // IE treats '`' as a quotation character.
        if (std::size_t j = s.substr(0, i).find_first_of("\"'<=`"); j != s.npos) {
            return {errorContext(errorf(ErrBadHTML, nullptr, 0,
                                        quote(s.substr(j, 1)) + " in unquoted attr: " + quote(s.substr(0, i)))),
                    s.size()};
        }
    }
    if (i == s.size()) {
        // Remain inside the attribute.
        // Decode the value so non-HTML rules can easily handle
        //     <button onclick="alert(&quot;Hi!&quot;)">
        // without having to entity decode token boundaries.
        std::string u = html::UnescapeString(string(s)).str();
        std::string_view uv = u;
        while (!uv.empty()) {
            auto [c1, i1] = transitionFunc[c.st](c, uv);
            c = std::move(c1);
            uv = uv.substr(i1);
        }
        return {c, s.size()};
    }

    element el = c.el;

    // If this is a non-JS "type" attribute inside "script" tag, do not
    // treat the contents as JS.
    if (c.st == stateAttr && c.el == elementScript && c.at == attrScriptType && !isJSType(s.substr(0, i))) {
        el = elementNone;
    }

    if (c.dl != delimSpaceOrTagEnd) {
        // Consume any quote.
        i++;
    }
    // On exiting an attribute, we discard all state information except the
    // state and element.
    context d;
    d.st = stateTag;
    d.el = el;
    return {d, i};
}

}  // namespace

// Escapes a template node.
context escaper::escape(context c, parse::Node* n) {
    switch (n->NodeType) {
        case parse::NodeAction:
            return escapeAction(c, static_cast<parse::ActionNode*>(n));
        case parse::NodeBreak:
            c.n = ptr<parse::Node>(n);
            rangeContext->breaks.push_back(c);
            return deadContext();
        case parse::NodeComment:
            return c;
        case parse::NodeContinue:
            c.n = ptr<parse::Node>(n);
            rangeContext->continues.push_back(c);
            return deadContext();
        case parse::NodeIf:
            return escapeBranch(c, static_cast<parse::BranchNode*>(n), "if");
        case parse::NodeList:
            return escapeList(c, static_cast<parse::ListNode*>(n));
        case parse::NodeRange:
            return escapeBranch(c, static_cast<parse::BranchNode*>(n), "range");
        case parse::NodeTemplate:
            return escapeTemplate(c, static_cast<parse::TemplateNode*>(n));
        case parse::NodeText:
            return escapeText(c, static_cast<parse::TextNode*>(n));
        case parse::NodeWith:
            return escapeBranch(c, static_cast<parse::BranchNode*>(n), "with");
        default:
            break;
    }
    panic("escaping " + n->String().str() + " is unimplemented");
}

// Escapes an action template node.
context escaper::escapeAction(context c, parse::ActionNode* n) {
    if (!n->Pipe->Decl.empty()) {
        // A local variable assignment, not an interpolation.
        return c;
    }
    c = nudge(c);
    // Check for disallowed use of predefined escapers in the pipeline.
    for (std::size_t pos = 0; pos < n->Pipe->Cmds.size(); pos++) {
        const auto* node = asIdentifier(n->Pipe->Cmds[pos]->Args[0]);
        if (!node) {
            // A predefined escaper "esc" will never be found as an
            // identifier in a Chain or Field node, since:
            // - "esc.x ..." is invalid, since predefined escapers return
            //   strings, and strings do not have methods, keys or fields.
            // - "... .esc" is invalid, since predefined escapers are global
            //   functions, not methods or fields of any types.
            // Therefore, it is safe to ignore these two node types.
            continue;
        }
        std::string_view ident = node->Ident;
        if (predefinedEscapers().contains(ident)) {
            if (pos < n->Pipe->Cmds.size() - 1 ||
                (c.st == stateAttr && c.dl == delimSpaceOrTagEnd && ident == "html")) {
                return errorContext(errorf(ErrPredefinedEscaper, ptr<parse::Node>(n), n->Line,
                                           "predefined escaper " + quote(ident) + " disallowed in template"));
            }
        }
    }
    std::vector<std::string> s;
    s.reserve(3);
    switch (c.st) {
        case stateError:
            return c;
        case stateURL: case stateCSSDqStr: case stateCSSSqStr: case stateCSSDqURL: case stateCSSSqURL:
        case stateCSSURL:
            switch (c.up) {
                case urlPartNone:
                    s.emplace_back("_html_template_urlfilter");
                    [[fallthrough]];
                case urlPartPreQuery:
                    switch (c.st) {
                        case stateCSSDqStr: case stateCSSSqStr:
                            s.emplace_back("_html_template_cssescaper");
                            break;
                        default:
                            s.emplace_back("_html_template_urlnormalizer");
                    }
                    break;
                case urlPartQueryOrFrag:
                    s.emplace_back("_html_template_urlescaper");
                    break;
                case urlPartUnknown:
                    return errorContext(errorf(ErrAmbigContext, ptr<parse::Node>(n), n->Line,
                                               n->String().str() + " appears in an ambiguous context within a URL"));
                default:
                    panic(std::string(urlPartName(c.up)));
            }
            break;
        case stateMetaContent:
            // Handled below in delim check.
            break;
        case stateMetaContentURL:
            // Go's GODEBUG htmlmetacontenturlescape=0 turns this off; go++
            // always filters.
            s.emplace_back("_html_template_urlfilter");
            break;
        case stateJS:
            s.emplace_back("_html_template_jsvalescaper");
            // A slash after a value starts a div operator.
            c.js = jsCtxDivOp;
            break;
        case stateJSDqStr: case stateJSSqStr:
            s.emplace_back("_html_template_jsstrescaper");
            break;
        case stateJSTmplLit:
            s.emplace_back("_html_template_jstmpllitescaper");
            break;
        case stateJSRegexp:
            s.emplace_back("_html_template_jsregexpescaper");
            break;
        case stateCSS:
            s.emplace_back("_html_template_cssvaluefilter");
            break;
        case stateText:
            s.emplace_back("_html_template_htmlescaper");
            break;
        case stateRCDATA:
            s.emplace_back("_html_template_rcdataescaper");
            break;
        case stateAttr:
            // Handled below in delim check.
            break;
        case stateAttrName: case stateTag:
            c.st = stateAttrName;
            s.emplace_back("_html_template_htmlnamefilter");
            break;
        case stateSrcset:
            s.emplace_back("_html_template_srcsetescaper");
            break;
        default:
            if (isComment(c.st)) {
                s.emplace_back("_html_template_commentescaper");
            } else {
                panic("unexpected state " + std::string(stateName(c.st)));
            }
    }
    switch (c.dl) {
        case delimNone:
            // No extra-escaping needed for raw text content.
            break;
        case delimSpaceOrTagEnd:
            s.emplace_back("_html_template_nospaceescaper");
            break;
        default:
            s.emplace_back("_html_template_attrescaper");
    }
    editActionNode(n, std::move(s));
    return c;
}

// Escapes a branch template node: "if", "range" and "with".
context escaper::escapeBranch(context c, parse::BranchNode* n, std::string_view nodeName) {
    ptr<parse::Node> node(n);
    if (nodeName == "range") rangeContext = std::make_shared<rangeInfo>(rangeInfo{rangeContext, {}, {}});
    context c0 = escapeList(c, n->List.get());
    if (nodeName == "range") {
        if (c0.st != stateError) c0 = joinRange(c0, *rangeContext);
        rangeContext = rangeContext->outer;
        if (c0.st == stateError) return c0;

        // The "true" branch of a "range" node can execute multiple times.
        // We check that executing n.List once results in the same context
        // as executing n.List twice.
        rangeContext = std::make_shared<rangeInfo>(rangeInfo{rangeContext, {}, {}});
        auto [c1, _] = escapeListConditionally(c0, n->List.get(), nullptr);
        c0 = join(c0, c1, node, nodeName);
        if (c0.st == stateError) {
            rangeContext = rangeContext->outer;
            // Make clear that this is a problem on loop re-entry since
            // developers tend to overlook that branch when debugging
            // templates.
            c0.err->Line = n->Line;
            c0.err->Description = "on range loop re-entry: " + c0.err->Description;
            return c0;
        }
        c0 = joinRange(c0, *rangeContext);
        rangeContext = rangeContext->outer;
        if (c0.st == stateError) return c0;
    }
    context c1 = escapeList(c, n->ElseList.get());
    return join(c0, c1, node, nodeName);
}

// Escapes a list template node.
context escaper::escapeList(context c, parse::ListNode* n) {
    if (!n) return c;
    for (const auto& m : n->Nodes) {
        c = escape(c, m.get());
        if (c.st == stateDead) break;
    }
    return c;
}

// Escapes a list node but only preserves edits and inferences in e if the
// inferences and output context satisfy filter. It returns the best guess
// at an output context, and the result of the filter which is the same as
// whether e was updated.
template <typename F>
std::pair<context, bool> escaper::escapeListConditionally(context c, parse::ListNode* n, F&& filter) {
    escaper e1(ns);
    e1.rangeContext = rangeContext;
    // Make type inferences available to f.
    for (const auto& [k, v] : output) e1.output.insert_or_assign(k, v);
    c = e1.escapeList(c, n);
    bool ok = false;
    if constexpr (!std::is_same_v<std::remove_cvref_t<F>, std::nullptr_t>) ok = filter(e1, c);
    if (ok) {
        // Copy inferences and edits from e1 back into e.
        for (auto& [k, v] : e1.output) output.insert_or_assign(k, v);
        for (auto& [k, v] : e1.derived) derived.insert_or_assign(k, v);
        for (auto& k : e1.called) called.insert(k);
        for (auto& [k, v] : e1.actionNodeEdits) editActionNode(k, v);
        for (auto& [k, v] : e1.templateNodeEdits) editTemplateNode(k, v);
        for (auto& [k, v] : e1.textNodeEdits) editTextNode(k, v);
    }
    return {c, ok};
}

// Escapes a {{template}} call node.
context escaper::escapeTemplate(context c, parse::TemplateNode* n) {
    auto [c1, name] = escapeTree(c, ptr<parse::Node>(n), n->Name, n->Line);
    if (name != n->Name.view()) editTemplateNode(n, name);
    return c1;
}

// Escapes the named template starting in the given context as necessary
// and returns its output context.
std::pair<context, std::string> escaper::escapeTree(context c, const ptr<parse::Node>& node, std::string_view name,
                                                    int line) {
    // Mangle the template name with the input context to produce a
    // reliable identifier.
    std::string dname = c.mangle(name);
    called.insert(dname);
    if (auto it = output.find(dname); it != output.end()) {
        // Already escaped.
        return {it->second, dname};
    }
    auto t = templateNamed(name);
    if (!t) {
        // Two cases: The template exists but is empty, or has never been
        // mentioned at all. Distinguish the cases in the error messages.
        if (ns->set.contains(name)) {
            return {errorContext(errorf(ErrNoSuchTemplate, node, line,
                                        quote(name) + " is an incomplete or empty template")),
                    dname};
        }
        return {errorContext(errorf(ErrNoSuchTemplate, node, line, "no such template " + quote(name))), dname};
    }
    if (dname != name) {
        // Use any template derived during an earlier call to escapeTemplate
        // with different top level templates, or clone if necessary.
        auto dt = templateNamed(dname);
        if (!dt) {
            dt = ttmpl::New(string(dname));
            auto tree = make_ptr<parse::Tree>(string(dname));
            tree->Root = t->Tree->Root->CopyList();
            dt->Tree = tree;
            derived[dname] = dt;
        }
        t = dt;
    }
    return {computeOutCtx(c, t), dname};
}

// Takes a template and its start context and computes the output context
// while storing any inferences in e.
context escaper::computeOutCtx(context c, const ptr<ttmpl::Template>& t) {
    // Propagate context over the body.
    auto [c1, ok] = escapeTemplateBody(c, t);
    if (!ok) {
        // Look for a fixed point by assuming c1 as the output context.
        if (auto [c2, ok2] = escapeTemplateBody(c1, t); ok2) {
            c1 = c2;
            ok = true;
        }
        // Use c1 as the error context if neither assumption worked.
    }
    if (!ok && c1.st != stateError) {
        return errorContext(errorf(ErrOutputContext, t->Tree->Root, 0,
                                   "cannot compute output context for template " + t->Name().str()));
    }
    return c1;
}

// Escapes the given template assuming the given output context, and
// returns the best guess at the output context and whether the assumption
// was correct.
std::pair<context, bool> escaper::escapeTemplateBody(const context& c, const ptr<ttmpl::Template>& t) {
    std::string name = t->Name().str();
    auto filter = [&](escaper& e1, const context& c1) {
        if (c1.st == stateError) {
            // Do not update the input escaper, e.
            return false;
        }
        if (!e1.called.contains(name)) {
            // If t is not recursively called, then c1 is an accurate output
            // context.
            return true;
        }
        // c1 is accurate if it matches our assumed output context.
        return c.eq(c1);
    };
    // We need to assume an output context so that recursive template calls
    // take the fast path out of escapeTree instead of infinitely recurring.
    // Naively assuming that the input context is the same as the output
    // works >90% of the time.
    output.insert_or_assign(name, c);
    return escapeListConditionally(c, t->Tree->Root.get(), filter);
}

// Escapes a text template node.
context escaper::escapeText(context c, parse::TextNode* n) {
    std::string_view s = n->Text;
    std::size_t written = 0, i = 0;
    std::string b;
    while (i != s.size()) {
        auto [c1, nread] = contextAfterText(c, s.substr(i));
        std::size_t i1 = i + nread;
        if (c.st == stateText || c.st == stateRCDATA) {
            std::size_t end = i1;
            if (c1.st != c.st) {
                for (std::size_t j = end; j-- > i;) {
                    if (s[j] == '<') {
                        end = j;
                        break;
                    }
                }
            }
            for (std::size_t j = i; j < end; j++) {
                if (s[j] == '<' && !hasDoctypePrefix(s.substr(j))) {
                    b += s.substr(written, j - written);
                    b += "&lt;";
                    written = j + 1;
                }
            }
        } else if (isComment(c.st) && c.dl == delimNone) {
            switch (c.st) {
                case stateJSBlockCmt:
                    // https://es5.github.io/#x7.4:
                    // "Comments behave like white space and are
                    // discarded except that, if a MultiLineComment
                    // contains a line terminator character, then
                    // the entire comment is considered to be a
                    // LineTerminator for purposes of parsing by
                    // the syntactic grammar."
                    if (containsLineTerminator(s.substr(written, i1 - written))) {
                        b += '\n';
                    } else {
                        b += ' ';
                    }
                    break;
                case stateCSSBlockCmt:
                    b += ' ';
                    break;
                default:
                    break;
            }
            written = i1;
        }
        if (c.st != c1.st && isComment(c1.st) && c1.dl == delimNone) {
            // Preserve the portion between written and the comment start.
            std::size_t cs = i1 - 2;
            if (c1.st == stateHTMLCmt || c1.st == stateJSHTMLOpenCmt) {
                // "<!--" instead of "/*" or "//"
                cs -= 2;
            } else if (c1.st == stateJSHTMLCloseCmt) {
                // "-->" instead of "/*" or "//"
                cs -= 1;
            }
            b += s.substr(written, cs - written);
            written = i1;
        }
        if (isInScriptLiteral(c.st) && containsSpecialScriptTag(s.substr(i, i1 - i))) {
            b += s.substr(written, i - written);
            b += escapeSpecialScriptTags(s.substr(i, i1 - i));
            written = i1;
        }
        if (i == i1 && c.st == c1.st) {
            panic("infinite loop from " + c.String() + " to " + c1.String() + " on " + quote(s.substr(0, i)) + ".." +
                  quote(s.substr(i)));
        }
        c = std::move(c1);
        i = i1;
    }

    if (written != 0 && c.st != stateError) {
        if (!isComment(c.st) || c.dl != delimNone) b += s.substr(written);
        editTextNode(n, std::move(b));
    }
    return c;
}

// Records a change to an action pipeline for later commit.
void escaper::editActionNode(parse::ActionNode* n, std::vector<std::string> cmds) {
    if (actionNodeEdits.contains(n)) panic("node " + n->String().str() + " shared between templates");
    actionNodeEdits[n] = std::move(cmds);
}

// Records a change to a {{template}} callee for later commit.
void escaper::editTemplateNode(parse::TemplateNode* n, std::string callee) {
    if (templateNodeEdits.contains(n)) panic("node " + n->String().str() + " shared between templates");
    templateNodeEdits[n] = std::move(callee);
}

// Records a change to a text node for later commit.
void escaper::editTextNode(parse::TextNode* n, std::string text) {
    if (textNodeEdits.contains(n)) panic("node " + n->String().str() + " shared between templates");
    textNodeEdits[n] = std::move(text);
}

// Applies changes to actions and template calls needed to contextually
// autoescape content and adds any derived templates to the set.
void escaper::commit() {
    for (const auto& [name, _] : output) {
        if (auto t = templateNamed(name)) t->Funcs(funcMap());
    }
    // Any template from the name space associated with this escaper can be
    // used to add derived templates to the underlying text/template name
    // space.
    Template* tmpl = arbitraryTemplate();
    for (const auto& [name, t] : derived) {
        if (auto [_, err] = tmpl->text_->AddParseTree(name, t->Tree); err) panic("error adding derived template");
    }
    for (auto& [n, s] : actionNodeEdits) ensurePipelineContains(n->Pipe.get(), s);
    for (auto& [n, name] : templateNodeEdits) n->Name = string(name);
    for (auto& [n, s] : textNodeEdits) n->Text = string(s);
    // Reset state that is specific to this commit so that the same changes
    // are not re-applied to the template on subsequent calls to commit.
    called.clear();
    actionNodeEdits.clear();
    templateNodeEdits.clear();
    textNodeEdits.clear();
}

// The named template given a mangled template name.
ptr<ttmpl::Template> escaper::templateNamed(std::string_view name) {
    // Any template from the name space associated with this escaper can be
    // used to look up templates in the underlying text/template name space.
    auto t = arbitraryTemplate()->text_->Lookup(name);
    if (!t) {
        if (auto it = derived.find(name); it != derived.end()) t = it->second;
    }
    return t;
}

// An arbitrary template from the name space associated with e and panics
// if no templates are found.
Template* escaper::arbitraryTemplate() {
    if (!ns->set.empty()) return ns->set.begin()->second;
    panic("no templates in name space");
}

// ---------------------------------------------------------------------------

nameSpace::nameSpace() : esc(std::make_unique<escaper>(this)) {}

nameSpace::~nameSpace() {
    for (Template* t : members) delete t;
}

context escapeTextNode(context c, parse::TextNode* n) {
    escaper e(nullptr);
    return e.escapeText(std::move(c), n);
}

const gopp::error& escapeOK() {
    static const gopp::error err = errors::New("template escaped correctly");
    return err;
}

// Rewrites the named template, which must be associated with t, to
// guarantee that the output of any of the named templates is properly
// escaped. If no error is returned, then the named templates have been
// modified. Otherwise the named templates have been rendered unusable.
gopp::error escapeTemplate(Template* tmpl, const ptr<parse::Node>& node, std::string_view name) {
    escaper& esc = *tmpl->ns_->esc;
    auto [c, _] = esc.escapeTree(context{}, node, name, 0);
    gopp::error err;
    if (c.err) {
        c.err->Name = string(name);
        err = c.err;
    } else if (c.st != stateText) {
        err = make_ptr<Error>(ErrEndContext, nullptr, string(name), 0, string("ends in a non-text context: " + c.String()));
    }
    if (err) {
        // Prevent execution of unsafe templates.
        if (auto it = tmpl->ns_->set.find(name); it != tmpl->ns_->set.end()) {
            Template* t = it->second;
            t->escapeErr_ = err;
            t->text_->Tree = nullptr;
            t->Tree = nullptr;
        }
        return err;
    }
    esc.commit();
    if (auto it = tmpl->ns_->set.find(name); it != tmpl->ns_->set.end()) {
        Template* t = it->second;
        t->escapeErr_ = escapeOK();
        t->Tree = t->text_->Tree;
    }
    return nullptr;
}

}  // namespace gopp::html::tmpl::detail
