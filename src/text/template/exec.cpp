// The template executor, after Go's text/template/exec.go. Go's executor
// reports errors by panicking; here private exceptions do that, and break
// and continue too.
#include "goconv.hpp"
#include "internal.hpp"

#include <gopp/strings.hpp>

#include <exception>
#include <format>

namespace gopp::text::template_ {

using namespace detail;

namespace {

struct exec_panic {
    gopp::error err;
};
struct write_panic {
    gopp::error err;
};
struct walk_break {};

// An error message wrapping its cause, as Go's %w does.
struct wrap_error final : error_interface {
    string msg;
    gopp::error cause;
    wrap_error(string m, gopp::error c) : msg(std::move(m)), cause(std::move(c)) {}
    string Error() const override { return msg; }
    const error_interface* Unwrap() const noexcept override { return cause.get(); }
};
struct walk_continue {};

struct variable {
    string name;
    Value value;
};

// What a call needs to know of a function or method.
struct callee {
    std::function<const type_ops*(int)> in;
    int num_in = 0;
    bool variadic = false;
    std::function<std::pair<Value, gopp::error>(std::span<const Value>)> call;
};

callee of_func(const Func& f) {
    callee c;
    c.num_in = f.NumIn();
    c.variadic = f.IsVariadic();
    c.in = [&f](int i) { return f.In(i); };
    c.call = [&f](std::span<const Value> a) { return f.Call(a); };
    return c;
}

callee of_method(const method_info& m, const Value& recv) {
    callee c;
    c.num_in = static_cast<int>(m.in.size());
    c.variadic = m.variadic;
    c.in = [&m](int i) { return m.in[static_cast<std::size_t>(i)] ? m.in[static_cast<std::size_t>(i)]() : nullptr; };
    c.call = [&m, recv](std::span<const Value> a) { return m.call(recv.ptr(), recv.owner(), a); };
    return c;
}

bool can_be_nil(const type_ops* t) {
    if (!t) return true;
    switch (t->kind) {
    case Kind::Func: case Kind::Interface: case Kind::Map: case Kind::Pointer: case Kind::Slice:
        return true;
    default:
        return false;
    }
}

bool int_like(Kind k) { return k == Kind::Int || k == Kind::Uint; }

// Whether a value of type v may go where t is expected. go++ lets the
// scalar kinds of different sizes convert (Go wouldn't).
bool assignable(const type_ops* v, const type_ops* t) {
    if (!t || v == t) return true;
    if (v->name == t->name && v->kind == t->kind) return true;
    switch (t->kind) {
    case Kind::Int: case Kind::Uint: return int_like(v->kind);
    case Kind::Bool: case Kind::Float: case Kind::String: case Kind::Complex: return v->kind == t->kind;
    case Kind::Interface: return t->name == "interface {}";  // any takes anything
    default: return false;
    }
}

bool is_hex_int(std::string_view s) {
    return s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X') && s.find_first_of("pP") == std::string_view::npos;
}

bool is_rune_int(std::string_view s) { return !s.empty() && s[0] == '\''; }

std::string double_percent(std::string_view s) {
    std::string out;
    for (char c : s) {
        out.push_back(c);
        if (c == '%') out.push_back('%');
    }
    return out;
}

// fmt's %v of a value, for messages.
std::string fmt_v(const Value& v) {
    Value a[] = {v};
    return Sprint(a).str();
}

class state {
public:
    state(const Template* tmpl, io::Writer* wr) : tmpl_(tmpl), wr_(wr) {}

    const Template* tmpl_;
    io::Writer* wr_;
    const parse::Node* node_ = nullptr;
    std::vector<variable> vars_;
    int depth_ = 0;

    void push(const string& name, const Value& v) { vars_.push_back({name, v}); }
    std::size_t mark() const { return vars_.size(); }
    void pop(std::size_t m) { vars_.resize(m); }

    void set_var(const string& name, const Value& v) {
        for (std::size_t i = vars_.size(); i-- > 0;) {
            if (vars_[i].name == name) {
                vars_[i].value = v;
                return;
            }
        }
        errorf("undefined variable: " + name.str());
    }

    void set_top_var(std::size_t n, const Value& v) { vars_[vars_.size() - n].value = v; }

    Value var_value(const string& name) {
        for (std::size_t i = vars_.size(); i-- > 0;) {
            if (vars_[i].name == name) return vars_[i].value;
        }
        errorf("undefined variable: " + name.str());
    }

    void at(const parse::Node* n) { node_ = n; }

    [[noreturn]] void errorf(const std::string& msg, gopp::error cause = nullptr) {
        std::string name = double_percent(tmpl_->Name());
        std::string text;
        if (!node_ || !tmpl_->Tree) {
            text = "template: " + name + ": " + msg;
        } else {
            auto [location, context] = tmpl_->Tree->ErrorContext(*node_);
            text = "template: " + location.str() + ": executing " + detail::quote(name) + " at <" +
                   double_percent(context) + ">: " + msg;
        }
        // Go formats with the message's %% left doubled; undo it.
        std::string out;
        for (std::size_t i = 0; i < text.size(); i++) {
            out.push_back(text[i]);
            if (text[i] == '%' && i + 1 < text.size() && text[i + 1] == '%') i++;
        }
        gopp::error err = cause ? gopp::error(make_ptr<wrap_error>(string(out), cause)) : errors::New(string(out));
        throw exec_panic{make_ptr<ExecError>(tmpl_->Name(), err)};
    }

    // -------------------------------------------------------------------

    // walk recurses through every nested template, so it stays a small
    // dispatcher: the work is out of line, off the recursion's stack.
    void walk(const Value& dot, const parse::Node* node);
    [[gnu::noinline]] void walk_action(const Value& dot, const parse::ActionNode* n);
    [[gnu::noinline]] void walk_text(const parse::TextNode* n);
    [[gnu::noinline]] void walk_if_or_with(parse::NodeType typ, const Value& dot, const parse::BranchNode* n);
    [[gnu::noinline]] void walk_range(const Value& dot, const parse::BranchNode* r);
    [[gnu::noinline]] void walk_template(const Value& dot, const parse::TemplateNode* t);
    [[gnu::noinline]] void walk_unknown(const parse::Node* node);
    Value eval_pipeline(const Value& dot, const parse::PipeNode* pipe);
    void not_a_function(const std::vector<ptr<parse::Node>>& args, const std::optional<Value>& final);
    Value eval_command(const Value& dot, const parse::CommandNode* cmd, const std::optional<Value>& final);
    Value ideal_constant(const parse::NumberNode* c);
    Value eval_field_node(const Value& dot, const parse::FieldNode* f, const std::vector<ptr<parse::Node>>* args,
                          const std::optional<Value>& final);
    Value eval_chain_node(const Value& dot, const parse::ChainNode* c, const std::vector<ptr<parse::Node>>* args,
                          const std::optional<Value>& final);
    Value eval_variable_node(const Value& dot, const parse::VariableNode* v, const std::vector<ptr<parse::Node>>* args,
                             const std::optional<Value>& final);
    Value eval_field_chain(const Value& dot, Value receiver, const parse::Node* node, std::span<const string> ident,
                           const std::vector<ptr<parse::Node>>* args, const std::optional<Value>& final);
    Value eval_function(const Value& dot, const parse::IdentifierNode* node, const parse::Node* cmd,
                        const std::vector<ptr<parse::Node>>* args, const std::optional<Value>& final);
    Value eval_field(const Value& dot, const string& field_name, const parse::Node* node,
                     const std::vector<ptr<parse::Node>>* args, const std::optional<Value>& final, Value receiver);
    Value eval_call(const Value& dot, const callee& fun, bool is_builtin, const parse::Node* node, std::string_view name,
                    const std::vector<ptr<parse::Node>>* args, const std::optional<Value>& final);
    Value validate_type(const Value& value, const type_ops* typ);
    Value eval_arg(const Value& dot, const type_ops* typ, const parse::Node* n);
    Value eval_empty_interface(const Value& dot, const parse::Node* n);
    void print_value(const parse::Node* n, const Value& v);
};

void state::walk_action(const Value& dot, const parse::ActionNode* n) {
    // Don't pop variables: they persist until the end of the control
    // structure.
    Value val = eval_pipeline(dot, n->Pipe.get());
    if (n->Pipe->Decl.empty()) print_value(n, val);
}

void state::walk_text(const parse::TextNode* n) {
    const string& text = n->Text;
    if (auto [k, err] = wr_->Write(std::span<const byte>(reinterpret_cast<const byte*>(text.data()), text.size())); err) {
        throw write_panic{err};
    }
}

void state::walk_unknown(const parse::Node* node) { errorf("unknown node: " + node->String().str()); }

void state::walk(const Value& dot, const parse::Node* node) {
    at(node);
    switch (node->Type()) {
    case parse::NodeAction: walk_action(dot, static_cast<const parse::ActionNode*>(node)); break;
    case parse::NodeBreak: throw walk_break{};
    case parse::NodeComment: break;
    case parse::NodeContinue: throw walk_continue{};
    case parse::NodeIf: walk_if_or_with(parse::NodeIf, dot, static_cast<const parse::BranchNode*>(node)); break;
    case parse::NodeList:
        for (auto& n : static_cast<const parse::ListNode*>(node)->Nodes) walk(dot, n.get());
        break;
    case parse::NodeRange: walk_range(dot, static_cast<const parse::BranchNode*>(node)); break;
    case parse::NodeTemplate: walk_template(dot, static_cast<const parse::TemplateNode*>(node)); break;
    case parse::NodeText: walk_text(static_cast<const parse::TextNode*>(node)); break;
    case parse::NodeWith: walk_if_or_with(parse::NodeWith, dot, static_cast<const parse::BranchNode*>(node)); break;
    default: walk_unknown(node);
    }
}

void state::walk_if_or_with(parse::NodeType typ, const Value& dot, const parse::BranchNode* n) {
    std::size_t m = mark();
    struct popper {
        state& s;
        std::size_t m;
        ~popper() { s.pop(m); }
    } p{*this, m};
    Value val = eval_pipeline(dot, n->Pipe.get());
    auto [truth, ok] = is_true(indirect_interface(val));
    if (!ok) errorf("if/with can't use " + fmt_v(val));
    if (truth) {
        walk(typ == parse::NodeWith ? val : dot, n->List.get());
    } else if (n->ElseList) {
        walk(dot, n->ElseList.get());
    }
}

void state::walk_range(const Value& dot, const parse::BranchNode* r) {
    at(r);
    std::size_t m0 = mark();
    struct popper {
        state& s;
        std::size_t m;
        ~popper() { s.pop(m); }
    } p0{*this, m0};
    auto [val, is_nil] = indirect(eval_pipeline(dot, r->Pipe.get()));
    (void)is_nil;
    // mark top of stack before any variables in the body are pushed.
    std::size_t m = mark();
    const auto& decl = r->Pipe->Decl;
    auto one_iteration = [&](const Value& index, const Value& elem) {
        if (!decl.empty()) {
            if (r->Pipe->IsAssign) {
                // With two variables, index comes first; with one, element.
                if (decl.size() > 1) set_var(decl[0]->Ident[0], index);
                else set_var(decl[0]->Ident[0], elem);
            } else {
                // Set top var (lexically the second if there are two) to
                // the element.
                set_top_var(1, elem);
            }
        }
        if (decl.size() > 1) {
            if (r->Pipe->IsAssign) set_var(decl[1]->Ident[0], elem);
            else set_top_var(2, index);  // set next var (lexically the first if there are two) to the index
        }
        struct popper {
            state& s;
            std::size_t m;
            ~popper() { s.pop(m); }
        } p{*this, m};
        try {
            walk(elem, r->List.get());
        } catch (walk_continue&) {
        }
    };
    try {
        switch (val.Kind()) {
        case Kind::Int:
        case Kind::Uint: {
            if (decl.size() > 1) {
                errorf("can't use " + fmt_v(val) + " to iterate over more than one variable");
            }
            bool run = false;
            if (val.Kind() == Kind::Int) {
                for (int64 i = 0; i < val.Int(); i++) {
                    run = true;
                    one_iteration(Value(), Value::scalar_int(val.Type(), i));
                }
            } else {
                for (uint64 i = 0; i < val.Uint(); i++) {
                    run = true;
                    one_iteration(Value(), Value::scalar_uint(val.Type(), i));
                }
            }
            if (run) return;
            break;
        }
        case Kind::Array:
        case Kind::Slice: {
            if (val.Len() == 0) break;
            for (std::size_t i = 0; i < val.Len(); i++) {
                one_iteration(Value::scalar_int(int_ops(), static_cast<int64>(i)), val.Index(i));
            }
            return;
        }
        case Kind::Map: {
            if (val.Len() == 0) break;
            for (auto& [k, v] : val.MapEntries()) one_iteration(k, v);
            return;
        }
        case Kind::Invalid:
            break;  // an invalid value is likely a nil map, etc. and acts like an empty map
        default:
            errorf("range can't iterate over " + fmt_v(val));
        }
    } catch (walk_break&) {
        return;
    }
    if (r->ElseList) walk(dot, r->ElseList.get());
}

void state::walk_template(const Value& dot, const parse::TemplateNode* t) {
    at(t);
    ptr<Template> tmpl = tmpl_->Lookup(t->Name);
    if (!tmpl) errorf("template " + detail::quote(t->Name) + " not defined");
    if (depth_ >= max_exec_depth) errorf(std::format("exceeded maximum template depth ({})", max_exec_depth));
    // Variables declared by the pipeline persist.
    Value d = eval_pipeline(dot, t->Pipe.get());
    state ns(tmpl.get(), wr_);
    ns.depth_ = depth_ + 1;
    ns.vars_ = {{string::from_static("$"), d}};
    ns.walk(d, tmpl->Tree->Root.get());
}

// Eval functions evaluate pipelines, commands, and their elements and
// extract values from the data structure by examining fields, calling
// methods, and so on.

Value state::eval_pipeline(const Value& dot, const parse::PipeNode* pipe) {
    if (!pipe) return Value();
    at(pipe);
    std::optional<Value> value;  // nullopt: missing (no previous command)
    for (auto& cmd : pipe->Cmds) {
        value = eval_command(dot, cmd.get(), value);  // previous value is this one's final arg
        // If the object has type interface{}, dig down one level to the
        // thing inside.
        if (value->Kind() == Kind::Interface && value->Type()->name == "interface {}") value = value->Elem();
    }
    Value v = value.value_or(Value());
    for (auto& variable : pipe->Decl) {
        if (pipe->IsAssign) set_var(variable->Ident[0], v);
        else push(variable->Ident[0], v);
    }
    return v;
}

void state::not_a_function(const std::vector<ptr<parse::Node>>& args, const std::optional<Value>& final) {
    if (args.size() > 1 || final) errorf("can't give argument to non-function " + args[0]->String().str());
}

Value state::eval_command(const Value& dot, const parse::CommandNode* cmd, const std::optional<Value>& final) {
    const parse::Node* first_word = cmd->Args[0].get();
    switch (first_word->Type()) {
    case parse::NodeField:
        return eval_field_node(dot, static_cast<const parse::FieldNode*>(first_word), &cmd->Args, final);
    case parse::NodeChain:
        return eval_chain_node(dot, static_cast<const parse::ChainNode*>(first_word), &cmd->Args, final);
    case parse::NodeIdentifier:
        // Must be a function.
        return eval_function(dot, static_cast<const parse::IdentifierNode*>(first_word), cmd, &cmd->Args, final);
    case parse::NodePipe:
        // Parenthesized pipeline. The arguments are all inside the pipeline;
        // final must be absent.
        not_a_function(cmd->Args, final);
        return eval_pipeline(dot, static_cast<const parse::PipeNode*>(first_word));
    case parse::NodeVariable:
        return eval_variable_node(dot, static_cast<const parse::VariableNode*>(first_word), &cmd->Args, final);
    default:
        break;
    }
    at(first_word);
    not_a_function(cmd->Args, final);
    switch (first_word->Type()) {
    case parse::NodeBool: return Value::scalar(bool_ops(), static_cast<const parse::BoolNode*>(first_word)->True);
    case parse::NodeDot: return dot;
    case parse::NodeNil: errorf("nil is not a command");
    case parse::NodeNumber: return ideal_constant(static_cast<const parse::NumberNode*>(first_word));
    case parse::NodeString: return Value::scalar_string(string_ops(), static_cast<const parse::StringNode*>(first_word)->Text);
    default: break;
    }
    errorf("can't evaluate command " + detail::quote(first_word->String()));
}

// idealConstant is called to return the value of a number in a context
// where we don't know the type. In that case, the syntax of the number
// tells us its type, and we use Go rules to resolve. Note there is no
// such thing as a uint ideal constant in this situation - the value must
// be of int type.
Value state::ideal_constant(const parse::NumberNode* c) {
    at(c);
    if (c->IsComplex) return Value::scalar_float(complex_ops(), c->Complex128.real(), c->Complex128.imag());
    if (c->IsFloat && !is_hex_int(c->Text) && !is_rune_int(c->Text) && c->Text.view().find_first_of(".eEpP") != std::string_view::npos) {
        return Value::scalar_float(float64_ops(), c->Float64);
    }
    if (c->IsInt) return Value::scalar_int(int_ops(), c->Int64);
    if (c->IsUint) errorf(c->Text.str() + " overflows int");
    return Value();
}

Value state::eval_field_node(const Value& dot, const parse::FieldNode* field, const std::vector<ptr<parse::Node>>* args,
                             const std::optional<Value>& final) {
    at(field);
    return eval_field_chain(dot, dot, field, field->Ident, args, final);
}

Value state::eval_chain_node(const Value& dot, const parse::ChainNode* chain, const std::vector<ptr<parse::Node>>* args,
                             const std::optional<Value>& final) {
    at(chain);
    if (chain->Field.empty()) errorf("internal error: no fields in evalChainNode");
    if (chain->Node->Type() == parse::NodeNil) errorf("indirection through explicit nil in " + chain->String().str());
    // (pipe).Field1.Field2 has pipe as .Node, fields as .Field. Eval the
    // pipeline, then the fields.
    Value pipe = eval_arg(dot, nullptr, chain->Node.get());
    return eval_field_chain(dot, pipe, chain, chain->Field, args, final);
}

Value state::eval_variable_node(const Value& dot, const parse::VariableNode* variable,
                                const std::vector<ptr<parse::Node>>* args, const std::optional<Value>& final) {
    // $x.Field has $x as the first ident, Field as the second. Eval the
    // var, then the fields.
    at(variable);
    Value value = var_value(variable->Ident[0]);
    if (variable->Ident.size() == 1) {
        if (args) not_a_function(*args, final);
        else if (final) errorf("can't give argument to non-function " + variable->String().str());
        return value;
    }
    return eval_field_chain(dot, value, variable, std::span(variable->Ident).subspan(1), args, final);
}

// evalFieldChain evaluates .X.Y.Z possibly followed by arguments. dot is
// the environment in which to evaluate arguments, while receiver is the
// value being walked along the chain.
Value state::eval_field_chain(const Value& dot, Value receiver, const parse::Node* node, std::span<const string> ident,
                              const std::vector<ptr<parse::Node>>* args, const std::optional<Value>& final) {
    std::size_t n = ident.size();
    for (std::size_t i = 0; i + 1 < n; i++) receiver = eval_field(dot, ident[i], node, nullptr, std::nullopt, receiver);
    // Now if it's a method, it gets the arguments.
    return eval_field(dot, ident[n - 1], node, args, final, receiver);
}

Value state::eval_function(const Value& dot, const parse::IdentifierNode* node, const parse::Node* cmd,
                           const std::vector<ptr<parse::Node>>* args, const std::optional<Value>& final) {
    at(node);
    std::string_view name = node->Ident;
    const Func* function = nullptr;
    bool is_builtin = false;
    if (common* c = template_access::of(*tmpl_)) {
        c->mu_funcs.RLock();
        auto it = c->exec_funcs.find(name);
        if (it != c->exec_funcs.end()) function = &it->second;
        c->mu_funcs.RUnlock();
    }
    if (!function) {
        auto it = builtins().find(name);
        if (it != builtins().end()) {
            function = &it->second;
            is_builtin = true;
        }
    }
    if (!function) errorf(detail::quote(name) + " is not a defined function");
    return eval_call(dot, of_func(*function), is_builtin, cmd, name, args, final);
}

// evalField evaluates an expression like (.Field) or (.Field arg1 arg2).
// The 'final' argument represents the return value from the preceding
// value of the pipeline, if any.
Value state::eval_field(const Value& dot, const string& field_name, const parse::Node* node,
                        const std::vector<ptr<parse::Node>>* args, const std::optional<Value>& final, Value receiver) {
    if (!receiver.IsValid()) {
        if (template_access::of(*tmpl_)->missing_key == map_error) errorf("nil data; no entry for key " + detail::quote(field_name));
        return Value();
    }
    std::string typ(receiver.TypeName());
    auto [recv, is_nil] = indirect(receiver);
    if (recv.Kind() == Kind::Interface && is_nil) {
        // Calling a method on a nil interface can't work.
        errorf("nil pointer evaluating " + typ + "." + field_name.str());
    }
    // Unless it's an interface, need to get to a value of type *T to
    // guarantee we see all methods of both T and *T.
    const method_info* method = nullptr;
    Value method_recv = recv;
    if (!is_nil) method = recv.MethodByName(field_name);
    if (!method && receiver.IsValid() && receiver.Type() != recv.Type()) {
        method = receiver.MethodByName(field_name);
        method_recv = receiver;
    }
    if (method) return eval_call(dot, of_method(*method, method_recv), false, node, field_name, args, final);
    bool has_args = (args && args->size() > 1) || final;
    // It's not a method; must be a field of a struct or an element of a map.
    switch (recv.Kind()) {
    case Kind::Struct:
        if (auto field = recv.FieldByName(field_name)) {
            if (has_args) errorf(field_name.str() + " has arguments but cannot be invoked as function");
            return *field;
        }
        break;
    case Kind::Map: {
        // If it's a map, attempt to use the field name as a key.
        const type_ops* key = recv.Type()->key ? recv.Type()->key() : nullptr;
        if (key && key->kind == Kind::String) {
            if (has_args) errorf(field_name.str() + " is not a method but has arguments");
            Value result = recv.MapIndex(Value::scalar_string(string_ops(), field_name));
            if (!result.IsValid()) {
                switch (template_access::of(*tmpl_)->missing_key) {
                case map_invalid: break;  // just use the invalid value
                case map_zero_value: {
                    const type_ops* e = recv.Type()->elem ? recv.Type()->elem() : nullptr;
                    if (e && e->zero) result = e->zero();
                    break;
                }
                case map_error: errorf("map has no entry for key " + detail::quote(field_name));
                }
            }
            return result;
        }
        break;
    }
    case Kind::Pointer: {
        const type_ops* etyp = recv.Type()->elem ? recv.Type()->elem() : nullptr;
        if (etyp && etyp->kind == Kind::Struct) {
            bool found = false;
            for (auto& f : etyp->fields) found |= f.name == field_name.view();
            if (!found) break;
        }
        if (is_nil) errorf("nil pointer evaluating " + typ + "." + field_name.str());
        break;
    }
    default:
        break;
    }
    errorf("can't evaluate field " + field_name.str() + " in type " + typ);
}

// evalCall executes a function or method call. If it's a method, fun
// already has the receiver bound, so it looks just like a function call.
// The arg list, if non-nil, includes (in the manner of the shell), arg[0]
// as the function itself.
Value state::eval_call(const Value& dot, const callee& fun, bool is_builtin, const parse::Node* node, std::string_view name,
                       const std::vector<ptr<parse::Node>>* args_in, const std::optional<Value>& final) {
    std::span<const ptr<parse::Node>> args;
    if (args_in) args = std::span(*args_in).subspan(1);  // zeroth arg is function name/node; not passed to function
    int num_in = static_cast<int>(args.size());
    if (final) num_in++;
    int num_fixed = static_cast<int>(args.size());
    if (fun.variadic) {
        num_fixed = fun.num_in - 1;  // last arg is the variadic one
        if (num_in < num_fixed) {
            errorf(std::format("wrong number of args for {}: want at least {} got {}", name, fun.num_in - 1, args.size()));
        }
    } else if (num_in != fun.num_in) {
        errorf(std::format("wrong number of args for {}: want {} got {}", name, fun.num_in, num_in));
    }

    // Special case for builtin and/or, which short-circuit.
    if (is_builtin && (name == "and" || name == "or")) {
        Value v;
        for (auto& arg : args) {
            v = eval_arg(dot, nullptr, arg.get());
            if (is_true(indirect_interface(v)).first == (name == "or")) return v;
        }
        if (final) v = *final;
        return v;
    }

    // Build the arg list.
    std::vector<Value> argv(static_cast<std::size_t>(num_in));
    // Args must be evaluated. Fixed args first.
    int i = 0;
    for (; i < num_fixed && i < static_cast<int>(args.size()); i++) {
        argv[static_cast<std::size_t>(i)] = eval_arg(dot, fun.in(i), args[static_cast<std::size_t>(i)].get());
    }
    // Now the ... args.
    if (fun.variadic) {
        const type_ops* arg_type = fun.in(fun.num_in - 1);  // the element type
        for (; i < static_cast<int>(args.size()); i++) {
            argv[static_cast<std::size_t>(i)] = eval_arg(dot, arg_type, args[static_cast<std::size_t>(i)].get());
        }
    }
    // Add final value if necessary.
    if (final) {
        const type_ops* t = fun.in(fun.num_in - 1);
        if (fun.variadic && num_in - 1 < num_fixed) t = fun.in(num_in - 1);
        argv[static_cast<std::size_t>(i)] = validate_type(*final, t);
    }

    // Special case for the "call" builtin: pass the name of the callee
    // along for error messages.
    if (is_builtin && name == "call") {
        string callee_name = args.empty() ? (final ? string(fmt_v(*final)) : string()) : args[0]->String();
        argv.insert(argv.begin(), Value::scalar_string(string_ops(), callee_name));
    }

    std::pair<Value, gopp::error> r;
    try {
        r = fun.call(argv);
    } catch (exec_panic&) {
        throw;
    } catch (const std::exception& e) {
        r = {Value(), errors::New(string(e.what()))};
    }
    if (r.second) {
        at(node);
        errorf(std::format("error calling {}: {}", name, r.second.Error().view()), r.second);
    }
    return r.first;
}

// validateType guarantees that the value is valid and assignable to the
// type.
Value state::validate_type(const Value& value, const type_ops* typ) {
    if (!value.IsValid()) {
        if (!typ) return Value();  // an untyped nil interface{}; accept as a proper nil value
        if (can_be_nil(typ)) return Value();
        errorf("invalid value; expected " + std::string(typ->name));
    }
    if (typ && !assignable(value.Type(), typ)) {
        if (value.Kind() == Kind::Interface && !value.IsNil()) {
            Value e = value.Elem();
            if (e.IsValid() && assignable(e.Type(), typ)) return e;
        }
        // Does one dereference or indirection work? We could do more, as
        // we do with method receivers, but that gets messy and method
        // receivers are much more constrained, so it makes more sense
        // there than here. Besides, one is almost always all you need.
        if (value.Kind() == Kind::Pointer && value.Type()->elem && assignable(value.Type()->elem(), typ)) {
            Value e = value.Elem();
            if (!e.IsValid() || value.IsNil()) errorf("dereference of nil pointer of type " + std::string(typ->name));
            return e;
        }
        if (typ->kind == Kind::Pointer && typ->elem && typ->elem() == value.Type()) return value;
        errorf("wrong type for value; expected " + std::string(typ->name) + "; got " + std::string(value.TypeName()));
    }
    return value;
}

Value state::eval_arg(const Value& dot, const type_ops* typ, const parse::Node* n) {
    at(n);
    switch (n->Type()) {
    case parse::NodeDot: return validate_type(dot, typ);
    case parse::NodeNil:
        if (can_be_nil(typ)) return Value();
        errorf("cannot assign nil to " + std::string(typ->name));
    case parse::NodeField: {
        std::vector<ptr<parse::Node>> self{ptr<parse::Node>(const_cast<parse::Node*>(n))};
        return validate_type(eval_field_node(dot, static_cast<const parse::FieldNode*>(n), &self, std::nullopt), typ);
    }
    case parse::NodeVariable:
        return validate_type(eval_variable_node(dot, static_cast<const parse::VariableNode*>(n), nullptr, std::nullopt), typ);
    case parse::NodePipe: return validate_type(eval_pipeline(dot, static_cast<const parse::PipeNode*>(n)), typ);
    case parse::NodeIdentifier:
        return validate_type(eval_function(dot, static_cast<const parse::IdentifierNode*>(n), n, nullptr, std::nullopt), typ);
    case parse::NodeChain:
        return validate_type(eval_chain_node(dot, static_cast<const parse::ChainNode*>(n), nullptr, std::nullopt), typ);
    default: break;
    }
    if (!typ || typ->kind == Kind::Interface) return eval_empty_interface(dot, n);
    switch (typ->kind) {
    case Kind::Bool:
        if (n->Type() == parse::NodeBool) return Value::scalar(typ, static_cast<const parse::BoolNode*>(n)->True);
        errorf("expected bool; found " + n->String().str());
    case Kind::Complex: {
        auto* num = n->Type() == parse::NodeNumber ? static_cast<const parse::NumberNode*>(n) : nullptr;
        if (num && num->IsComplex) return Value::scalar_float(typ, num->Complex128.real(), num->Complex128.imag());
        errorf("expected complex; found " + n->String().str());
    }
    case Kind::Float: {
        auto* num = n->Type() == parse::NodeNumber ? static_cast<const parse::NumberNode*>(n) : nullptr;
        if (num && num->IsFloat) return Value::scalar_float(typ, num->Float64);
        errorf("expected float; found " + n->String().str());
    }
    case Kind::Int: {
        auto* num = n->Type() == parse::NodeNumber ? static_cast<const parse::NumberNode*>(n) : nullptr;
        if (num && num->IsInt) return Value::scalar_int(typ, num->Int64);
        errorf("expected integer; found " + n->String().str());
    }
    case Kind::String:
        if (n->Type() == parse::NodeString) return Value::scalar_string(typ, static_cast<const parse::StringNode*>(n)->Text);
        errorf("expected string; found " + n->String().str());
    case Kind::Uint: {
        auto* num = n->Type() == parse::NodeNumber ? static_cast<const parse::NumberNode*>(n) : nullptr;
        if (num && num->IsUint) return Value::scalar_uint(typ, num->Uint64);
        errorf("expected unsigned integer; found " + n->String().str());
    }
    default: break;
    }
    errorf("can't handle " + n->String().str() + " for arg of type " + std::string(typ->name));
}

Value state::eval_empty_interface(const Value& dot, const parse::Node* n) {
    at(n);
    switch (n->Type()) {
    case parse::NodeBool: return Value::scalar(bool_ops(), static_cast<const parse::BoolNode*>(n)->True);
    case parse::NodeDot: return dot;
    case parse::NodeField: return eval_field_node(dot, static_cast<const parse::FieldNode*>(n), nullptr, std::nullopt);
    case parse::NodeIdentifier:
        return eval_function(dot, static_cast<const parse::IdentifierNode*>(n), n, nullptr, std::nullopt);
    case parse::NodeNil: errorf("evalEmptyInterface: nil (can't happen)");
    case parse::NodeNumber: return ideal_constant(static_cast<const parse::NumberNode*>(n));
    case parse::NodeString: return Value::scalar_string(string_ops(), static_cast<const parse::StringNode*>(n)->Text);
    case parse::NodeVariable:
        return eval_variable_node(dot, static_cast<const parse::VariableNode*>(n), nullptr, std::nullopt);
    case parse::NodePipe: return eval_pipeline(dot, static_cast<const parse::PipeNode*>(n));
    default: break;
    }
    errorf("can't handle assignment of " + n->String().str() + " to empty interface argument");
}

// printValue writes the textual representation of the value to the
// output of the template.
void state::print_value(const parse::Node* n, const Value& v0) {
    at(n);
    Value v = v0;
    if (v.Kind() == Kind::Pointer) v = indirect(v).first;  // fmt.Fprint handles nil
    std::string out;
    if (!v.IsValid()) {
        out = "<no value>";
    } else {
        if (v.Kind() == Kind::Func && !v.MethodByName("String") && !v.MethodByName("Error")) {
            errorf("can't print " + n->String().str() + " of type " + std::string(v.TypeName()));
        }
        Value a[] = {v};
        out = Sprint(a).str();
    }
    if (auto [k, err] = io::WriteString(*wr_, out); err) throw write_panic{err};
}

}  // namespace

namespace detail {

std::pair<bool, bool> is_true(const Value& val) {
    if (!val.IsValid()) return {false, true};  // something like var x interface{}, never set; it's a form of nil
    switch (val.Kind()) {
    case Kind::Array: case Kind::Map: case Kind::Slice: case Kind::String: return {val.Len() > 0, true};
    case Kind::Bool: return {val.Bool(), true};
    case Kind::Complex: return {val.Complex() != std::complex<double>(0, 0), true};
    case Kind::Func: case Kind::Pointer: case Kind::Interface: return {!val.IsNil(), true};
    case Kind::Int: return {val.Int() != 0, true};
    case Kind::Float: return {val.Float() != 0, true};
    case Kind::Uint: return {val.Uint() != 0, true};
    case Kind::Struct: return {true, true};  // struct values are always true
    default: return {false, false};
    }
}

std::pair<Value, bool> indirect(Value v) {
    for (int i = 0; i < 64 && (v.Kind() == Kind::Pointer || v.Kind() == Kind::Interface); i++) {
        if (v.IsNil()) return {v, true};
        v = v.Elem();
    }
    return {v, false};
}

Value indirect_interface(const Value& v) {
    if (v.Kind() != Kind::Interface) return v;
    if (v.IsNil()) return Value();
    return v.Elem();
}

}  // namespace detail

std::pair<bool, bool> IsTrue(const Value& v) { return is_true(v); }

gopp::error Template::Execute(io::Writer& w, const Value& data) {
    state s(this, &w);
    s.vars_ = {{string::from_static("$"), data}};
    try {
        if (!Tree || !Tree->Root) s.errorf(detail::quote(Name()) + " is an incomplete or empty template");
        s.walk(data, Tree->Root.get());
    } catch (exec_panic& e) {
        return e.err;
    } catch (write_panic& e) {
        return e.err;  // strip the wrapper
    }
    return nullptr;
}

gopp::error Template::ExecuteTemplate(io::Writer& w, std::string_view name, const Value& data) {
    ptr<Template> tmpl = Lookup(name);
    if (!tmpl) {
        return errors::New("template: no template " + string(detail::quote(name)) + " associated with template " +
                           string(detail::quote(Name())));
    }
    return tmpl->Execute(w, data);
}

}  // namespace gopp::text::template_
