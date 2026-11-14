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

void objectCreated(const object* o) noexcept {
    registry& r = reg();
    std::lock_guard l(r.mu);
    r.live.emplace(o, r.created++);
}

void objectDestroyed(const object* o) noexcept {
    registry& r = reg();
    std::lock_guard l(r.mu);
    r.live.erase(o);
}

}  // namespace detail

long objectsInUse() noexcept {
    registry& r = reg();
    std::lock_guard l(r.mu);
    return static_cast<long>(r.live.size());
}

unsigned long objectsCreated() noexcept {
    registry& r = reg();
    std::lock_guard l(r.mu);
    return r.created;
}

std::vector<std::string> liveObjectsSince(unsigned long since) {
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

std::span<const error> errorInterface::UnwrapList() const noexcept { return {}; }

namespace errors {

namespace detail {

const errorInterface* find(const errorInterface* e, bool (*match)(const errorInterface*, void*), void* ctx) noexcept {
    for (; e; e = e->Unwrap()) {
        if (match(e, ctx)) return e;
        for (const error& u : e->UnwrapList()) {
            if (auto* r = find(u.get(), match, ctx)) return r;
        }
    }
    return nullptr;
}

}  // namespace detail

namespace {

// Go's joinError.
struct joinError final : errorInterface {
    std::vector<error> errs;
    string Error() const override {
        std::string s;
        for (std::size_t i = 0; i < errs.size(); i++) {
            if (i > 0) s += '\n';
            s += errs[i].Error().view();
        }
        return string(s);
    }
    std::span<const error> UnwrapList() const noexcept override { return errs; }
};

struct errorString final : errorInterface {
    explicit errorString(string s) : s(std::move(s)) {}
    string Error() const override { return s; }
    string s;
};

}  // namespace

error New(string text) { return error(make_ptr<errorString>(std::move(text))); }

bool Is(const error& err, const error& target) noexcept {
    if (!err || !target) return err == target;
    const errorInterface* t = target.get();
    return detail::find(
               err.get(),
               [](const errorInterface* e, void* t) {
                   auto* target = static_cast<const errorInterface*>(t);
                   return e == target || e->Is(target);
               },
               const_cast<errorInterface*>(t)) != nullptr;
}

error Join(std::span<const error> errs) {
    auto e = make_ptr<joinError>();
    for (const error& err : errs) {
        if (err) e->errs.push_back(err);
    }
    if (e->errs.empty()) return nullptr;
    return error(std::move(e));
}

error Join(std::initializer_list<error> errs) { return Join(std::span<const error>(errs.begin(), errs.size())); }

error Unwrap(const error& err) noexcept {
    if (!err) return nullptr;
    const errorInterface* u = err->Unwrap();
    return u ? error(ptr<const errorInterface>(u)) : error();
}

}  // namespace errors

}  // namespace gopp
