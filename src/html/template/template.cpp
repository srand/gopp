// html/template's Template, ported from Go 1.27.3's html/template/template.go
// and error.go.
#include "html/template/internal.hpp"

#include <glob.h>

#include <format>
#include <fstream>
#include <sstream>

namespace gopp::html::tmpl {

using namespace detail;

// ---------------------------------------------------------------------------
// error.go

Error::Error(tmpl::ErrorCode code, ptr<parse::Node> node, string name, int line, string description)
    : ErrorCode(code), Node(std::move(node)), Name(std::move(name)), Line(line), Description(std::move(description)) {
    // The node's place, now: the tree it points into may go before the
    // error does.
    if (Node && Node->tree()) loc_ = Node->tree()->ErrorContext(*Node).first;
}

string Error::message() const {
    if (Node && !loc_.empty()) return std::format("html/template:{}: {}", loc_.view(), Description.view());
    if (Line != 0) return std::format("html/template:{}:{}: {}", Name.view(), Line, Description.view());
    if (!Name.empty()) return std::format("html/template:{}: {}", Name.view(), Description.view());
    return "html/template: " + Description;
}

// ---------------------------------------------------------------------------
// template.go

void nameSpace::join(Template* t) {
    // Templates are built on one fiber, as in Go: no reference can come or
    // go between reading the count and moving it.
    refs.fetch_add(t->object::use_count(), std::memory_order_relaxed);
    members.push_back(t);
    t->ns_ = this;
}

Template::Template(ptr<text::tmpl::Template> t, ptr<parse::Tree> tree) : Tree(std::move(tree)), text_(std::move(t)) {}

Template::~Template() = default;

void Template::addRef() const noexcept {
    if (ns_) ns_->refs.fetch_add(1, std::memory_order_relaxed);
    else object::addRef();
}

void Template::releaseRef() const noexcept {
    if (!ns_) {
        object::releaseRef();
        return;
    }
    if (ns_->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) delete ns_;
}

namespace {

ptr<Template> newIn(nameSpace* ns, ptr<text::tmpl::Template> text, ptr<parse::Tree> tree) {
    auto t = make_ptr<Template>(std::move(text), std::move(tree));
    ns->join(t.get());
    return t;
}

}  // namespace

ptr<Template> New(string name) {
    auto* ns = new nameSpace();
    auto t = newIn(ns, text::tmpl::New(name), nullptr);
    ns->set[name.str()] = t.get();
    return t;
}

ptr<Template> Template::New(string name) {
    ns_->mu.Lock();
    auto t = newLocked(std::move(name));
    ns_->mu.Unlock();
    return t;
}

// New with the name space locked.
ptr<Template> Template::newLocked(string name) {
    auto t = newIn(ns_, text_->New(name), nullptr);
    if (auto it = ns_->set.find(name.view()); it != ns_->set.end()) {
        // Go replaces the existing template with an empty one of its own;
        // here it is emptied.
        Template* existing = it->second;
        existing->escapeErr_ = nullptr;
        existing->text_ = text::tmpl::New(existing->Name());
        existing->Tree = nullptr;
    }
    ns_->set[name.str()] = t.get();
    return t;
}

string Template::Name() const { return text_->Name(); }

std::vector<ptr<Template>> Template::Templates() const {
    ns_->mu.Lock();
    std::vector<ptr<Template>> m;
    m.reserve(ns_->set.size());
    for (auto& [_, v] : ns_->set) m.emplace_back(v);
    ns_->mu.Unlock();
    return m;
}

Template* Template::Option(std::initializer_list<std::string_view> opts) {
    text_->Option(opts);
    return this;
}

// An error if the template can no longer be parsed: after Execute.
gopp::error Template::checkCanParse() const {
    ns_->mu.Lock();
    bool escaped = ns_->escaped;
    ns_->mu.Unlock();
    if (escaped) return errors::New("html/template: cannot Parse after Execute");
    return nullptr;
}

// Escapes all associated templates.
gopp::error Template::escape() {
    sync::Mutex& mu = ns_->mu;
    mu.Lock();
    ns_->escaped = true;
    gopp::error err;
    if (!escapeErr_) {
        if (!Tree) {
            err = errors::New("template: " + quote(Name()) + " is an incomplete or empty template");
        } else {
            err = escapeTemplate(this, text_->Tree->Root, Name());
        }
    } else if (escapeErr_ != escapeOK()) {
        err = escapeErr_;
    }
    mu.Unlock();
    return err;
}

gopp::error Template::Execute(io::Writer& w, const Value& data) {
    if (auto err = escape()) return err;
    return text_->Execute(w, data);
}

gopp::error Template::ExecuteTemplate(io::Writer& w, std::string_view name, const Value& data) {
    auto [t, err] = lookupAndEscapeTemplate(name);
    if (err) return err;
    return t->text_->Execute(w, data);
}

// Guarantees that the template with the given name is escaped, or returns
// an error if it cannot be. It returns the named template.
std::pair<ptr<Template>, gopp::error> Template::lookupAndEscapeTemplate(std::string_view name) {
    ns_->mu.Lock();
    ns_->escaped = true;
    auto unlock = [&](ptr<Template> t, gopp::error err) -> std::pair<ptr<Template>, gopp::error> {
        ns_->mu.Unlock();
        return {std::move(t), std::move(err)};
    };
    auto it = ns_->set.find(name);
    if (it == ns_->set.end()) return unlock(nullptr, errors::New("html/template: " + quote(name) + " is undefined"));
    ptr<Template> t(it->second);
    if (t->escapeErr_ && t->escapeErr_ != escapeOK()) return unlock(nullptr, t->escapeErr_);
    if (!t->text_->Tree || !t->text_->Tree->Root) {
        return unlock(nullptr, errors::New("html/template: " + quote(name) + " is an incomplete template"));
    }
    if (!text_->Lookup(name)) panic("html/template internal error: template escaping out of sync");
    gopp::error err;
    if (!t->escapeErr_) err = escapeTemplate(t.get(), t->text_->Tree->Root, name);
    return unlock(t, err);
}

string Template::DefinedTemplates() const { return text_->DefinedTemplates(); }

std::pair<ptr<Template>, gopp::error> Template::Parse(std::string_view text) {
    if (auto err = checkCanParse()) return {nullptr, err};

    auto [ret, err] = text_->Parse(text);
    if (err) return {nullptr, err};

    // In general, all the named templates might have changed underfoot.
    // Regardless, some new ones may have been defined. The
    // template.Template set has been updated; update ours.
    ns_->mu.Lock();
    for (const auto& v : ret->Templates()) {
        std::string name = v->Name().str();
        ptr<Template> t;
        if (auto it = ns_->set.find(name); it != ns_->set.end()) {
            t = ptr<Template>(it->second);
        } else {
            t = newLocked(string(name));
        }
        t->text_ = v;
        t->Tree = v->Tree;
    }
    ns_->mu.Unlock();
    return {ptr<Template>(this), nullptr};
}

std::pair<ptr<Template>, gopp::error> Template::AddParseTree(std::string_view name, ptr<parse::Tree> tree) {
    if (auto err = checkCanParse()) return {nullptr, err};

    ns_->mu.Lock();
    auto [text, err] = text_->AddParseTree(name, std::move(tree));
    if (err) {
        ns_->mu.Unlock();
        return {nullptr, err};
    }
    auto ret = newIn(ns_, text, text->Tree);
    ns_->set[std::string(name)] = ret.get();
    ns_->mu.Unlock();
    return {ret, nullptr};
}

std::pair<ptr<Template>, gopp::error> Template::Clone() const {
    ns_->mu.Lock();
    auto fail = [&] {
        ns_->mu.Unlock();
        return std::pair<ptr<Template>, gopp::error>{
            nullptr, errors::New("html/template: cannot Clone " + quote(Name()) + " after it has executed")};
    };
    if (escapeErr_) return fail();
    auto [textClone, err] = text_->Clone();
    if (err) {
        ns_->mu.Unlock();
        return {nullptr, err};
    }
    auto* ns = new nameSpace();
    auto ret = newIn(ns, textClone, textClone->Tree);
    ns->set[ret->Name().str()] = ret.get();
    for (const auto& x : textClone->Templates()) {
        std::string name = x->Name().str();
        auto it = ns_->set.find(name);
        if (it == ns_->set.end() || it->second->escapeErr_) return fail();
        x->Tree = x->Tree->Copy();
        ns->set[name] = newIn(ns, x, x->Tree).get();  // the name space keeps it
    }
    ns_->mu.Unlock();
    // Return the template associated with the name of this template.
    return {ptr<Template>(ns->set[ret->Name().str()]), nullptr};
}

Template* Template::Funcs(const FuncMap& funcs) {
    text_->Funcs(funcs);
    return this;
}

Template* Template::Delims(std::string_view left, std::string_view right) {
    text_->Delims(left, right);
    return this;
}

ptr<Template> Template::Lookup(std::string_view name) const {
    ns_->mu.Lock();
    auto it = ns_->set.find(name);
    ptr<Template> t = it == ns_->set.end() ? nullptr : ptr<Template>(it->second);
    ns_->mu.Unlock();
    return t;
}

ptr<Template> Must(std::pair<ptr<Template>, gopp::error> r) {
    if (r.second) panic(r.second.Error().str());
    return r.first;
}

namespace {

std::pair<std::string, gopp::error> readFile(std::string_view file) {
    std::ifstream in{std::string(file), std::ios::binary};
    if (!in) return {{}, errors::New("open " + string(file) + ": no such file or directory")};
    std::ostringstream ss;
    ss << in.rdbuf();
    return {ss.str(), nullptr};
}

std::string baseName(std::string_view path) {
    while (path.size() > 1 && path.back() == '/') path.remove_suffix(1);
    auto slash = path.rfind('/');
    return std::string(slash == std::string_view::npos ? path : path.substr(slash + 1));
}

// The helper for the method and function. If the argument template is
// nil, it is created from the first file.
std::pair<ptr<Template>, gopp::error> parseFiles(ptr<Template> t, std::span<const string> filenames) {
    if (filenames.empty()) {
        // Not really a problem, but be consistent.
        return {nullptr, errors::New("html/template: no files named in call to ParseFiles")};
    }
    for (const auto& filename : filenames) {
        auto [s, err] = readFile(filename);
        if (err) return {nullptr, err};
        std::string name = baseName(filename);
        // First template becomes return value if not already defined, and
        // we use that one for subsequent New calls to associate all the
        // templates together. Also, if this file has the same name as t,
        // this file becomes the contents of t, so
        //  t, err := New(name).Funcs(xxx).ParseFiles(name)
        // works. Otherwise we create a new template associated with t.
        ptr<Template> tmpl;
        if (!t) t = tmpl::New(string(name));
        if (name == t->Name().view()) {
            tmpl = t;
        } else {
            tmpl = t->New(string(name));
        }
        if (auto perr = tmpl->Parse(s).second) return {nullptr, perr};
    }
    return {t, nullptr};
}

std::pair<ptr<Template>, gopp::error> parseGlob(ptr<Template> t, std::string_view pattern) {
    glob_t g{};
    int rc = ::glob(std::string(pattern).c_str(), 0, nullptr, &g);
    std::vector<string> names;
    if (rc == 0) {
        for (std::size_t i = 0; i < g.gl_pathc; i++) names.emplace_back(g.gl_pathv[i]);
    }
    globfree(&g);
    if (rc != 0 && rc != GLOB_NOMATCH) return {nullptr, errors::New("syntax error in pattern")};
    if (names.empty()) {
        // %#q: backquoted where it can be.
        std::string q = pattern.find('`') == pattern.npos ? "`" + std::string(pattern) + "`" : quote(pattern);
        return {nullptr, errors::New("html/template: pattern matches no files: " + q)};
    }
    return parseFiles(std::move(t), names);
}

}  // namespace

std::pair<ptr<Template>, gopp::error> Template::ParseFiles(std::span<const string> filenames) {
    if (auto err = checkCanParse()) return {nullptr, err};
    return parseFiles(ptr<Template>(this), filenames);
}

std::pair<ptr<Template>, gopp::error> Template::ParseFiles(std::initializer_list<std::string_view> filenames) {
    std::vector<string> v(filenames.begin(), filenames.end());
    return ParseFiles(std::span<const string>(v));
}

std::pair<ptr<Template>, gopp::error> Template::ParseGlob(std::string_view pattern) {
    if (auto err = checkCanParse()) return {nullptr, err};
    return parseGlob(ptr<Template>(this), pattern);
}

std::pair<ptr<Template>, gopp::error> ParseFiles(std::initializer_list<std::string_view> filenames) {
    std::vector<string> v(filenames.begin(), filenames.end());
    return parseFiles(nullptr, v);
}

std::pair<ptr<Template>, gopp::error> ParseGlob(std::string_view pattern) { return parseGlob(nullptr, pattern); }

}  // namespace gopp::html::tmpl
