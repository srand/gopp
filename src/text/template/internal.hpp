// Internals shared by the text/template sources.
#pragma once

#include <gopp/sync.hpp>
#include <gopp/text/template.hpp>

#include <atomic>
#include <map>
#include <string>
#include <vector>

namespace gopp::text::tmpl::detail {

enum missingKeyAction { mapInvalid, mapZeroValue, mapError };

// What associated templates share (Go's common).
// A set of associated templates. It owns its templates and is freed when
// its reference count, shared by all of them, drops to zero.
struct common {
    std::atomic<unsigned long> refs{0};  // references to any of the templates
    std::vector<Template*> members;      // every template that joined, freed with the set
    std::map<std::string, Template*, std::less<>> tmpl;  // defined templates, by name
    sync::RWMutex muTmpl;
    missingKeyAction missingKey = mapInvalid;
    sync::RWMutex muFuncs;
    parse::FuncNames parseFuncs;  // names, for the parser
    FuncMap execFuncs;

    common() = default;
    common(const common&) = delete;
    common& operator=(const common&) = delete;
    ~common();
};

// Templates alive (template.cpp), for tests.
long liveTemplates() noexcept;

struct templateAccess {
    static common* of(const Template& t) { return t.common_; }
};

// The builtin functions.
const FuncMap& builtins();
const parse::FuncNames& builtinNames();

// Go's truth of a value.
std::pair<bool, bool> isTrue(const Value& v);
// Follows pointers and interfaces; whether it stopped at a nil.
std::pair<Value, bool> indirect(Value v);
Value indirectInterface(const Value& v);

// The deepest {{template}} recursion at execution. Go allows 100000;
// templates may run on 256 KiB fiber stacks.
inline constexpr int maxExecDepth = 200;

}  // namespace gopp::text::tmpl::detail
