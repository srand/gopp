// tmpl::Value and the descriptors of the builtin types.
#include <gopp/text/template.hpp>

#include <mutex>
#include <set>

namespace gopp::text::tmpl {

namespace detail {

std::string_view intern(std::string s) {
    static std::mutex mu;
    static std::set<std::string, std::less<>>* names = new std::set<std::string, std::less<>>();
    std::lock_guard l(mu);
    return *names->insert(std::move(s)).first;
}

const type_ops* scalar_ops(Kind k, std::string_view name) {
    static std::mutex mu;
    static auto* m = new std::map<std::pair<int, std::string>, type_ops*>();
    std::lock_guard l(mu);
    auto key = std::make_pair(static_cast<int>(k), std::string(name));
    auto it = m->find(key);
    if (it != m->end()) return it->second;
    auto* o = new type_ops{};
    o->name = intern(std::string(name));
    o->kind = k;
    o->zero = [] { return Value(); };
    m->emplace(key, o);
    return o;
}

const type_ops* bool_ops() {
    static const type_ops* o = [] {
        auto* t = const_cast<type_ops*>(scalar_ops(Kind::Bool, "bool"));
        t->zero = [] { return Value::scalar(bool_ops(), false); };
        return t;
    }();
    return o;
}

const type_ops* int_ops() {
    static const type_ops* o = [] {
        auto* t = const_cast<type_ops*>(scalar_ops(Kind::Int, "int"));
        t->zero = [] { return Value::scalar_int(int_ops(), 0); };
        return t;
    }();
    return o;
}

const type_ops* string_ops() {
    static const type_ops* o = [] {
        auto* t = const_cast<type_ops*>(scalar_ops(Kind::String, "string"));
        t->zero = [] { return Value::scalar_string(string_ops(), string()); };
        return t;
    }();
    return o;
}

const type_ops* float64_ops() {
    static const type_ops* o = [] {
        auto* t = const_cast<type_ops*>(scalar_ops(Kind::Float, "float64"));
        t->zero = [] { return Value::scalar_float(float64_ops(), 0); };
        return t;
    }();
    return o;
}

const type_ops* complex_ops() { return scalar_ops(Kind::Complex, "complex128"); }

namespace {

// The dynamic value of a non-nil error: something with an Error method.
const method_info error_methods[] = {
    {"Error", {}, false,
     [](const void* obj, const owner_t&, std::span<const Value>) -> std::pair<Value, gopp::error> {
         return {Value::scalar_string(string_ops(), static_cast<const gopp::error*>(obj)->Error()), nullptr};
     }},
};

const type_ops* error_value_ops() {
    static const type_ops o = [] {
        type_ops t{};
        t.name = "*errors.errorString";
        t.kind = Kind::Struct;  // prints by its Error method
        t.methods = error_methods;
        return t;
    }();
    return &o;
}

Value json_dynamic(const json::Value& j, const owner_t& owner) {
    switch (j.type()) {
    case json::Value::kind::null: return Value();
    case json::Value::kind::boolean: return Value::scalar(bool_ops(), j.AsBool());
    case json::Value::kind::number: return Value::scalar_float(float64_ops(), j.AsNumber());
    case json::Value::kind::string: return Value::scalar_string(string_ops(), j.AsString());
    case json::Value::kind::array: return Value::Ref(j.AsArray(), owner);
    case json::Value::kind::object: return Value::Ref(j.AsObject(), owner);
    }
    return Value();
}

}  // namespace

const type_ops* error_ops() {
    static const type_ops o = [] {
        type_ops t{};
        t.name = "error";
        t.kind = Kind::Interface;
        t.is_nil = [](const void* p) { return !*static_cast<const gopp::error*>(p); };
        t.deref = [](const void* p, const owner_t& owner) -> Value {
            if (!*static_cast<const gopp::error*>(p)) return Value();
            return Value::object(error_value_ops(), p, owner);
        };
        t.zero = [] { return Value(); };
        return t;
    }();
    return &o;
}

const type_ops* json_ops() {
    static const type_ops o = [] {
        type_ops t{};
        t.name = "interface {}";
        t.kind = Kind::Interface;
        t.is_nil = [](const void* p) { return static_cast<const json::Value*>(p)->IsNull(); };
        t.deref = [](const void* p, const owner_t& owner) { return json_dynamic(*static_cast<const json::Value*>(p), owner); };
        t.zero = [] { return Value(); };
        return t;
    }();
    return &o;
}

const type_ops* any_ops() { return json_ops(); }

const type_ops* func_ops() {
    static const type_ops o = [] {
        type_ops t{};
        t.name = "func";
        t.kind = Kind::Func;
        t.is_nil = [](const void* p) { return !*static_cast<const Func*>(p); };
        t.func = [](const void* p) { return static_cast<const Func*>(p); };
        t.zero = [] { return Value(); };
        return t;
    }();
    return &o;
}

}  // namespace detail

// ---------------------------------------------------------------------------
// Value

Value Value::scalar(const detail::type_ops* t, bool b) {
    Value v;
    v.t_ = t;
    v.s_.b = b;
    return v;
}

Value Value::scalar_int(const detail::type_ops* t, int64 i) {
    Value v;
    v.t_ = t;
    v.s_.i = i;
    return v;
}

Value Value::scalar_uint(const detail::type_ops* t, uint64 u) {
    Value v;
    v.t_ = t;
    v.s_.u = u;
    return v;
}

Value Value::scalar_float(const detail::type_ops* t, double f, double im) {
    Value v;
    v.t_ = t;
    v.s_.f = f;
    v.im_ = im;
    return v;
}

Value Value::scalar_string(const detail::type_ops* t, string s) {
    Value v;
    v.t_ = t;
    v.str_ = std::move(s);
    return v;
}

Value Value::object(const detail::type_ops* t, const void* p, detail::owner_t owner) {
    Value v;
    v.t_ = t;
    v.p_ = p;
    v.owner_ = std::move(owner);
    return v;
}

Kind Value::Kind() const noexcept { return t_ ? t_->kind : tmpl::Kind::Invalid; }

std::string_view Value::TypeName() const noexcept { return t_ ? t_->name : std::string_view("<nil>"); }

std::size_t Value::Len() const {
    if (Kind() == tmpl::Kind::String) return str_.size();
    if (t_ && t_->len) return t_->len(p_);
    return 0;
}

Value Value::Index(std::size_t i) const {
    if (Kind() == tmpl::Kind::String) {
        return Value::scalar_uint(detail::scalar_ops(tmpl::Kind::Uint, "uint8"), static_cast<unsigned char>(str_[i]));
    }
    if (t_ && t_->index) return t_->index(p_, owner_, i);
    return Value();
}

Value Value::MapIndex(const Value& key) const {
    if (t_ && t_->map_index) return t_->map_index(p_, owner_, key);
    return Value();
}

std::vector<std::pair<Value, Value>> Value::MapEntries() const {
    std::vector<std::pair<Value, Value>> out;
    if (t_ && t_->entries) t_->entries(p_, owner_, out);
    return out;
}

std::optional<Value> Value::FieldByName(std::string_view name) const {
    if (!t_) return std::nullopt;
    for (auto& f : t_->fields) {
        if (f.name == name) return f.get(p_, owner_);
    }
    return std::nullopt;
}

const detail::method_info* Value::MethodByName(std::string_view name) const {
    if (!t_) return nullptr;
    for (auto& m : t_->methods) {
        if (m.name == name) return &m;
    }
    return nullptr;
}

bool Value::IsNil() const {
    if (!t_) return true;
    if (t_->is_nil) return t_->is_nil(p_);
    return false;
}

Value Value::Elem() const {
    if (t_ && t_->deref) return t_->deref(p_, owner_);
    return Value();
}

const Func* Value::AsFunc() const {
    if (t_ && t_->func) return t_->func(p_);
    return nullptr;
}

Func Func::Raw(int n_in, bool variadic, std::function<std::pair<Value, gopp::error>(std::span<const Value>)> fn) {
    Func f;
    f.in_.assign(static_cast<std::size_t>(n_in), nullptr);
    f.variadic_ = variadic;
    f.fn_ = std::move(fn);
    return f;
}

Func Func::Raw(std::vector<detail::ops_fn> in, bool variadic,
               std::function<std::pair<Value, gopp::error>(std::span<const Value>)> fn) {
    Func f;
    f.in_ = std::move(in);
    f.variadic_ = variadic;
    f.fn_ = std::move(fn);
    return f;
}

}  // namespace gopp::text::tmpl
