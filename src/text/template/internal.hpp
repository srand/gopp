// Internals shared by the text/template sources.
#pragma once

#include <gopp/sync.hpp>
#include <gopp/text/template.hpp>

#include <map>
#include <string>

namespace gopp::text::template_::detail {

enum missing_key_action { map_invalid, map_zero_value, map_error };

// What associated templates share (Go's common).
struct common {
    std::map<std::string, ptr<Template>, std::less<>> tmpl;  // defined templates, by name
    sync::RWMutex mu_tmpl;
    missing_key_action missing_key = map_invalid;
    sync::RWMutex mu_funcs;
    parse::FuncNames parse_funcs;  // names, for the parser
    FuncMap exec_funcs;
};

struct template_access {
    static common* of(const Template& t) { return t.common_.get(); }
};

// The builtin functions.
const FuncMap& builtins();
const parse::FuncNames& builtin_names();

// Go's truth of a value.
std::pair<bool, bool> is_true(const Value& v);
// Follows pointers and interfaces; whether it stopped at a nil.
std::pair<Value, bool> indirect(Value v);
Value indirect_interface(const Value& v);

// The deepest {{template}} recursion at execution. Go allows 100000;
// templates may run on 256 KiB fiber stacks.
inline constexpr int max_exec_depth = 200;

}  // namespace gopp::text::template_::detail
