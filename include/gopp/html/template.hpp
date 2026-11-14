// The html/template package: text/template that escapes its output for
// where it lands in HTML, so that it is safe against code injection;
// ported from Go 1.27.3.
//
//     namespace htmpl = gopp::html::tmpl;
//     auto t = htmpl::Must(htmpl::New("page")->Parse(
//         R"html(<a href="/search?q={{.}}" onclick="go({{.}})">{{.}}</a>)html"));
//     t->Execute(w, std::string("O'Reilly & <Sons>"));
//     // <a href="/search?q=O%27Reilly%20%26%20%3cSons%3e"
//     //    onclick="go(&#34;O&#39;Reilly \u0026 \u003cSons\u003e&#34;)">O&#39;Reilly &amp; &lt;Sons&gt;</a>
//
// The first Execute parses each action's context (HTML text, an attribute,
// a URL, JavaScript, CSS, ...) and adds the escaping functions it needs to
// the pipeline, as Go's does; templates can't be parsed further after
// that. Content known to be safe is passed as one of the content types
// (HTML, JS, URL, ...), whose text is used as it is where its type fits.
//
// The data model is text/template's (<gopp/text/template.hpp>). Values
// that JavaScript gets are JSON-encoded from that model: a struct's
// public fields by their names (json annotations don't apply), a type's
// MarshalJSON method if it has one, and slices of bytes as base64, as in
// Go. Go's `template` package is gopp::html::tmpl here, as `template` is a
// C++ keyword.
#pragma once

// The module interfaces (modules/) import these instead.
#ifndef GOPP_MODULE
#include <gopp/errors.hpp>
#include <gopp/io.hpp>
#include <gopp/ptr.hpp>
#include <gopp/string.hpp>
#include <gopp/sync.hpp>
#include <gopp/text/template.hpp>
#include <gopp/text/template/parse.hpp>

#include <initializer_list>
#include <map>
#include <memory>
#include <span>
#include <string_view>
#include <utility>
#include <vector>
#endif

namespace gopp::html::tmpl {

namespace parse = text::tmpl::parse;
using text::tmpl::FuncMap;
using text::tmpl::Value;

// The content types: strings known to be safe in a context, used as they
// are there (and escaped elsewhere). Each trusts its author: don't build
// them from untrusted input.

// template.CSS: a CSS3 stylesheet, rule, declaration or value.
struct CSS : text::tmpl::NamedString {
    using NamedString::NamedString;
};
// template.HTML: a document fragment of HTML.
struct HTML : text::tmpl::NamedString {
    using NamedString::NamedString;
};
// template.HTMLAttr: an attribute, such as ` dir="ltr"`.
struct HTMLAttr : text::tmpl::NamedString {
    using NamedString::NamedString;
};
// template.JS: a JavaScript expression, such as `(x + y * z())`.
struct JS : text::tmpl::NamedString {
    using NamedString::NamedString;
};
// template.JSStr: the content of a JavaScript string literal, without its
// quotes, such as `foo\nbar`.
struct JSStr : text::tmpl::NamedString {
    using NamedString::NamedString;
};
// template.URL: a URL or part of one, such as `http://example.com/x?y=1`;
// one with a scheme other than http, https or mailto is filtered unless it
// comes as a URL.
struct URL : text::tmpl::NamedString {
    using NamedString::NamedString;
};
// template.Srcset: a srcset attribute's value.
struct Srcset : text::tmpl::NamedString {
    using NamedString::NamedString;
};

// template.ErrorCode: the kind of an escaping error.
enum ErrorCode : int {
    OK,                    // no error
    ErrAmbigContext,       // an action is in an ambiguous context within a URL
    ErrBadHTML,            // malformed HTML: an unquoted attribute value with a quote, a bad attribute name...
    ErrBranchEnd,          // {{if}} branches end in different contexts
    ErrEndContext,         // a template ends in a non-text context
    ErrNoSuchTemplate,     // {{template}} of a template that doesn't exist
    ErrOutputContext,      // a template's output context can't be computed
    ErrPartialCharset,     // an unfinished JS regexp charset
    ErrPartialEscape,      // an unfinished escape sequence
    ErrRangeLoopReentry,   // a range loop body ends in a different context than it starts
    ErrSlashAmbig,         // '/' could start a division or a regexp
    ErrPredefinedEscaper,  // html or urlquery in a pipeline, where it isn't allowed
    ErrJSTemplate,         // no longer used
};

namespace detail {
// Implements Error() for an error type named Error, which can't declare it.
template <typename E>
struct namedError : errorInterface {
    string Error() const override { return static_cast<const E&>(*this).message(); }
};
}  // namespace detail

// template.Error: a failure to escape a template.
struct Error : detail::namedError<Error> {
    // The kind of error.
    tmpl::ErrorCode ErrorCode = OK;
    // The node that caused the problem, if known.
    ptr<parse::Node> Node;
    // The name of the template, and the line of the error in it, if known.
    string Name;
    int Line = 0;
    // A human-readable description of the problem.
    string Description;

    Error(tmpl::ErrorCode code, ptr<parse::Node> node, string name, int line, string description);
    // "html/template:name:line:col: description" and the like.
    string message() const;

private:
    string loc_;  // the node's place, "name:line:col"
};

class Template;

namespace detail {
struct nameSpace;
struct escaper;
gopp::error escapeTemplate(Template* tmpl, const ptr<parse::Node>& node, std::string_view name);
}  // namespace detail

// template.Template: a text/template whose output is escaped for HTML.
// Like text/template's, the templates associated with each other share one
// reference count, and must live in ptr<Template>.
class Template final : public object {
public:
    // The parse tree; nil until parsed.
    ptr<parse::Tree> Tree;

    ~Template() override;

    // Reference counting (intrusive_ptr calls these): a template counts
    // on its set.
    void addRef() const noexcept;
    void releaseRef() const noexcept;

    string Name() const;
    // A new template associated with this one, with its delimiters. A
    // template of the same name is replaced.
    ptr<Template> New(string name);
    // Parses text as the template's body; {{define}}d templates become
    // associated templates. An error after the first Execute.
    std::pair<ptr<Template>, gopp::error> Parse(std::string_view text);
    // Parses the named files: the first defines this template (or the one
    // named like it), the others are associated, by base name.
    std::pair<ptr<Template>, gopp::error> ParseFiles(std::initializer_list<std::string_view> filenames);
    std::pair<ptr<Template>, gopp::error> ParseFiles(std::span<const string> filenames);
    // Parses the files matching a glob(3) pattern.
    std::pair<ptr<Template>, gopp::error> ParseGlob(std::string_view pattern);
    // Associates tree with name.
    std::pair<ptr<Template>, gopp::error> AddParseTree(std::string_view name, ptr<parse::Tree> tree);

    // Escapes the template (on the first call), then applies it to data,
    // writing to w. Data is referenced.
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
    gopp::error ExecuteTemplate(io::Writer& w, std::string_view name, std::nullptr_t) {
        return ExecuteTemplate(w, name, Value());
    }

    // The associated template named name, or nil.
    ptr<Template> Lookup(std::string_view name) const;
    // All associated templates, including this one.
    std::vector<ptr<Template>> Templates() const;
    // "; defined templates are: "a", "b"", for error messages.
    string DefinedTemplates() const;
    // A copy of the template and its associated templates, before any has
    // executed, to add to.
    std::pair<ptr<Template>, gopp::error> Clone() const;

    // Sets the action delimiters ("" for the default {{ and }}).
    Template* Delims(std::string_view left, std::string_view right);
    // Adds functions; must happen before parsing templates that use them.
    Template* Funcs(const FuncMap& funcs);
    // Sets options, as text/template's Option.
    Template* Option(std::initializer_list<std::string_view> opts);
    Template* Option(std::string_view opt) { return Option({opt}); }

    // Internals.
    gopp::error escapeErr_;
    ptr<text::tmpl::Template> text_;
    detail::nameSpace* ns_ = nullptr;  // the set; owns this template
    Template(ptr<text::tmpl::Template> t, ptr<parse::Tree> tree);

private:
    friend ptr<Template> New(string name);
    friend struct detail::escaper;
    ptr<Template> newLocked(string name);
    gopp::error escape();
    std::pair<ptr<Template>, gopp::error> lookupAndEscapeTemplate(std::string_view name);
    gopp::error checkCanParse() const;
    friend gopp::error detail::escapeTemplate(Template* tmpl, const ptr<parse::Node>& node, std::string_view name);
};

// template.New
ptr<Template> New(string name);

// template.Must: panics if err is non-nil.
ptr<Template> Must(std::pair<ptr<Template>, gopp::error> r);

// template.ParseFiles, ParseGlob
std::pair<ptr<Template>, gopp::error> ParseFiles(std::initializer_list<std::string_view> filenames);
std::pair<ptr<Template>, gopp::error> ParseGlob(std::string_view pattern);

// template.IsTrue
using text::tmpl::IsTrue;

// The escapers of text/template's html, js and urlquery functions.
using text::tmpl::HTMLEscape;
using text::tmpl::HTMLEscaper;
using text::tmpl::HTMLEscapeString;
using text::tmpl::JSEscape;
using text::tmpl::JSEscaper;
using text::tmpl::JSEscapeString;
using text::tmpl::URLQueryEscaper;

}  // namespace gopp::html::tmpl
