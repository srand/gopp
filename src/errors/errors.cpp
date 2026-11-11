#include <gopp/errors.hpp>

#ifdef GOPP_DEBUG_LEAKS
#include <cxxabi.h>

#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <typeinfo>
#include <unordered_map>
#endif

namespace gopp {

#ifdef GOPP_DEBUG_LEAKS
namespace {

// Live objects by address, with their creation number. Never freed, so that
// objects destroyed after static destruction began still find it. A plain
// mutex: held only briefly, never across a park.
struct registry {
    std::mutex mu;
    std::unordered_map<const object*, unsigned long> live;
    unsigned long created = 0;
};

registry& reg() {
    static registry* r = new registry;
    return *r;
}

std::string demangle(const char* name) {
    int status = 0;
    char* s = abi::__cxa_demangle(name, nullptr, nullptr, &status);
    std::string out = status == 0 && s ? s : name;
    std::free(s);
    return out;
}

}  // namespace

namespace detail {

void object_created(const object* o) noexcept {
    registry& r = reg();
    std::lock_guard l(r.mu);
    r.live.emplace(o, r.created++);
}

void object_destroyed(const object* o) noexcept {
    registry& r = reg();
    std::lock_guard l(r.mu);
    r.live.erase(o);
}

}  // namespace detail

long objects_in_use() noexcept {
    registry& r = reg();
    std::lock_guard l(r.mu);
    return static_cast<long>(r.live.size());
}

unsigned long objects_created() noexcept {
    registry& r = reg();
    std::lock_guard l(r.mu);
    return r.created;
}

std::vector<std::string> live_objects_since(unsigned long since) {
    std::map<std::string, int> types;
    {
        registry& r = reg();
        std::lock_guard l(r.mu);
        for (auto& [o, n] : r.live) {
            if (n >= since) types[demangle(typeid(*o).name())]++;
        }
    }
    std::vector<std::string> out;
    for (auto& [t, n] : types) out.push_back(t + (n > 1 ? " x" + std::to_string(n) : ""));
    return out;
}
#endif

namespace errors {

namespace {

struct error_string final : error_interface {
    explicit error_string(string s) : s(std::move(s)) {}
    string Error() const override { return s; }
    string s;
};

}  // namespace

error New(string text) { return error(make_ptr<error_string>(std::move(text))); }

bool Is(const error& err, const error& target) noexcept {
    if (!err || !target) return err == target;
    for (const error_interface* e = err.get(); e; e = e->Unwrap()) {
        if (e == target.get()) return true;
    }
    return false;
}

error Unwrap(const error& err) noexcept {
    if (!err) return nullptr;
    const error_interface* u = err->Unwrap();
    return u ? error(ptr<const error_interface>(u)) : error();
}

}  // namespace errors

}  // namespace gopp
