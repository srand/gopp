#include <gopp/errors.hpp>

#ifdef GOPP_DEBUG_LEAKS
#include <cstdio>
#endif

namespace gopp {

#ifdef GOPP_DEBUG_LEAKS
namespace {
std::atomic<long> objects{0};
}
namespace detail {
void increment_objects_in_use() noexcept { objects.fetch_add(1, std::memory_order_relaxed); }
void decrement_objects_in_use() noexcept { objects.fetch_sub(1, std::memory_order_relaxed); }
}  // namespace detail
long objects_in_use() noexcept { return objects.load(); }
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
