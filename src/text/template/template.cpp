// After Go's text/template/template.go, option.go and helper.go.
#include "goconv.hpp"
#include "internal.hpp"

#include <glob.h>

#include <fstream>
#include <sstream>

namespace gopp::text::tmpl {

using namespace detail;

namespace {

std::atomic<long> n_live{0};  // templates alive

}  // namespace

// Templates alive, for tests.
long detail::liveTemplates() noexcept { return n_live.load(); }

Template::Template(string name) : name_(std::move(name)) { n_live.fetch_add(1, std::memory_order_relaxed); }

Template::~Template() { n_live.fetch_sub(1, std::memory_order_relaxed); }

common::~common() {
    for (Template* t : members) delete t;
}

void Template::addRef() const noexcept {
    if (common_) common_->refs.fetch_add(1, std::memory_order_relaxed);
    else object::addRef();
}

void Template::releaseRef() const noexcept {
    if (!common_) {
        object::releaseRef();
        return;
    }
    if (common_->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) delete common_;
}

void Template::join(common* c) {
    if (common_) {
        if (common_ != c) panic("template: internal error: a template joins a second set");
        return;
    }
    // Templates are built on one fiber, as in Go: no reference can come or
    // go between reading the count and moving it.
    c->refs.fetch_add(object::use_count(), std::memory_order_relaxed);
    c->members.push_back(this);
    common_ = c;
}

ptr<Template> New(string name) {
    auto t = make_ptr<Template>(std::move(name));
    t->init();
    return t;
}

void Template::init() {
    if (!common_) join(new common());
}

ptr<Template> Template::New(string name) {
    init();
    auto nt = make_ptr<Template>(std::move(name));
    nt->join(common_);
    nt->left_delim_ = left_delim_;
    nt->right_delim_ = right_delim_;
    return nt;
}

ptr<Template> Template::copy(common* c) const {
    auto nt = make_ptr<Template>(name_);
    nt->Tree = Tree;
    if (c) nt->join(c);
    nt->left_delim_ = left_delim_;
    nt->right_delim_ = right_delim_;
    return nt;
}

std::pair<ptr<Template>, gopp::error> Template::Clone() const {
    auto nt = copy(nullptr);
    nt->init();
    if (!common_) return {nt, nullptr};
    nt->common_->missing_key = common_->missing_key;
    common_->mu_tmpl.RLock();
    for (auto& [k, v] : common_->tmpl) {
        if (k == name_.view()) {
            nt->common_->tmpl[k] = nt.get();
            continue;
        }
        // The associated templates share nt's common structure.
        nt->common_->tmpl[k] = v->copy(nt->common_).get();  // the set keeps it
    }
    common_->mu_tmpl.RUnlock();
    common_->mu_funcs.RLock();
    nt->common_->parse_funcs = common_->parse_funcs;
    nt->common_->exec_funcs = common_->exec_funcs;
    common_->mu_funcs.RUnlock();
    return {nt, nullptr};
}

std::pair<ptr<Template>, gopp::error> Template::AddParseTree(std::string_view name, ptr<parse::Tree> tree) {
    init();
    common_->mu_tmpl.Lock();
    ptr<Template> nt(this);
    if (name != name_.view()) nt = New(string(name));
    // Even if nt == t, we need to install it in the common.tmpl map.
    if (associate(nt, tree) || !nt->Tree) nt->Tree = tree;
    common_->mu_tmpl.Unlock();
    return {nt, nullptr};
}

std::vector<ptr<Template>> Template::Templates() const {
    std::vector<ptr<Template>> m;
    if (!common_) return m;
    common_->mu_tmpl.RLock();
    for (auto& [k, v] : common_->tmpl) m.push_back(ptr<Template>(v));
    common_->mu_tmpl.RUnlock();
    return m;
}

Template* Template::Delims(std::string_view left, std::string_view right) {
    init();
    left_delim_ = string(left);
    right_delim_ = string(right);
    return this;
}

namespace {

bool goodName(std::string_view name) {
    if (name.empty()) return false;
    for (std::size_t i = 0; i < name.size();) {
        auto [r, w] = detail::decodeRune(name, i);
        if (r == '_') {
        } else if (i == 0 && !detail::isLetter(r)) {
            return false;
        } else if (!detail::isLetter(r) && !detail::is_digit(r)) {
            return false;
        }
        i += static_cast<std::size_t>(w);
    }
    return true;
}

}  // namespace

Template* Template::Funcs(const FuncMap& funcs) {
    init();
    common_->mu_funcs.Lock();
    for (auto& [name, fn] : funcs) {
        if (!goodName(name)) {
            common_->mu_funcs.Unlock();
            panic("function name " + detail::quote(name) + " is not a valid identifier");
        }
        common_->exec_funcs[name] = fn;
        common_->parse_funcs.insert(name);
    }
    common_->mu_funcs.Unlock();
    return this;
}

ptr<Template> Template::Lookup(std::string_view name) const {
    if (!common_) return nullptr;
    common_->mu_tmpl.RLock();
    auto it = common_->tmpl.find(name);
    ptr<Template> t = it == common_->tmpl.end() ? nullptr : ptr<Template>(it->second);
    common_->mu_tmpl.RUnlock();
    return t;
}

std::pair<ptr<Template>, gopp::error> Template::Parse(std::string_view text) {
    init();
    common_->mu_funcs.RLock();
    parse::FuncNames funcs = common_->parse_funcs;
    common_->mu_funcs.RUnlock();
    auto [trees, err] = parse::Parse(name_, text, left_delim_, right_delim_, {&funcs, &builtinNames()});
    if (err) return {nullptr, err};
    // Add the newly parsed trees, including the one for t, into our common
    // structure.
    for (auto& [name, tree] : trees) {
        if (auto [t, aerr] = AddParseTree(name, tree); aerr) return {nullptr, aerr};
    }
    return {ptr<Template>(this), nullptr};
}

// associate installs the new template into the group of templates
// associated with t. The two are already known to share the common
// structure. The boolean return value reports whether to store this tree
// as t.Tree.
bool Template::associate(const ptr<Template>& nt, const ptr<parse::Tree>& tree) {
    if (nt->common_ != common_) panic("internal error: associate not common");
    auto it = common_->tmpl.find(nt->name_.view());
    if (it != common_->tmpl.end() && parse::IsEmptyTree(tree->Root.get()) && it->second->Tree) {
        // If a template by that name exists, don't replace it with an empty
        // template.
        return false;
    }
    common_->tmpl[nt->name_.str()] = nt.get();
    return true;
}

string Template::DefinedTemplates() const {
    if (!common_) return {};
    std::string b;
    common_->mu_tmpl.RLock();
    for (auto& [name, tmpl] : common_->tmpl) {
        if (!tmpl->Tree || !tmpl->Tree->Root) continue;
        b += b.empty() ? "; defined templates are: " : ", ";
        b += detail::quote(name);
    }
    common_->mu_tmpl.RUnlock();
    return string(b);
}

// option.go

Template* Template::Option(std::initializer_list<std::string_view> opts) {
    init();
    for (std::string_view opt : opts) {
        if (opt.empty()) panic("empty option string");
        // key=value
        auto eq = opt.find('=');
        if (eq != std::string_view::npos && opt.substr(0, eq) == "missingkey") {
            std::string_view value = opt.substr(eq + 1);
            if (value == "invalid" || value == "default") {
                common_->missing_key = map_invalid;
                continue;
            }
            if (value == "zero") {
                common_->missing_key = map_zero_value;
                continue;
            }
            if (value == "error") {
                common_->missing_key = map_error;
                continue;
            }
        }
        panic("unrecognized option: " + std::string(opt));
    }
    return this;
}

// helper.go

ptr<Template> Must(std::pair<ptr<Template>, gopp::error> r) {
    if (r.second) panic(r.second.Error().str());
    return r.first;
}

namespace {

std::pair<std::string, gopp::error> read_file(std::string_view file) {
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

std::pair<ptr<Template>, gopp::error> parseFiles(ptr<Template> t, std::span<const string> filenames) {
    if (filenames.empty()) {
        // Not really a problem, but be consistent.
        return {nullptr, errors::New("template: no files named in call to ParseFiles")};
    }
    for (auto& filename : filenames) {
        auto [s, err] = read_file(filename);
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
        if (name == t->Name().view()) tmpl = t;
        else tmpl = t->New(string(name));
        if (auto [r, perr] = tmpl->Parse(s); perr) return {nullptr, perr};
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
    if (names.empty()) return {nullptr, errors::New("template: pattern matches no files: " + string(detail::quote(pattern)))};
    return parseFiles(std::move(t), names);
}

}  // namespace

std::pair<ptr<Template>, gopp::error> Template::ParseFiles(std::span<const string> filenames) {
    init();
    return parseFiles(ptr<Template>(this), filenames);
}

std::pair<ptr<Template>, gopp::error> Template::ParseFiles(std::initializer_list<std::string_view> filenames) {
    std::vector<string> v(filenames.begin(), filenames.end());
    return ParseFiles(std::span<const string>(v));
}

std::pair<ptr<Template>, gopp::error> Template::ParseGlob(std::string_view pattern) {
    init();
    return parseGlob(ptr<Template>(this), pattern);
}

std::pair<ptr<Template>, gopp::error> ParseFiles(std::initializer_list<std::string_view> filenames) {
    std::vector<string> v(filenames.begin(), filenames.end());
    return parseFiles(nullptr, v);
}

std::pair<ptr<Template>, gopp::error> ParseGlob(std::string_view pattern) { return parseGlob(nullptr, pattern); }

}  // namespace gopp::text::tmpl
