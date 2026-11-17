// The EVP_PKEY cache of the keys of crypto/rsa and crypto/ecdsa.
#include <gopp/crypto.hpp>
#include <gopp/sync.hpp>

#include "crypto/pkey.hpp"

namespace gopp::crypto::detail {

struct KeyCache::State {
    sync::Once once;
    pkey::Key key;
    error err;
};

KeyCache::KeyCache() : s_(std::make_unique<State>()) {}
KeyCache::KeyCache(const KeyCache&) : s_(std::make_unique<State>()) {}
KeyCache& KeyCache::operator=(const KeyCache& o) {
    if (this != &o) s_ = std::make_unique<State>();
    return *this;
}
KeyCache::~KeyCache() = default;

std::pair<evp_pkey_st*, error> KeyCache::Get(const std::function<std::pair<evp_pkey_st*, error>()>& build) const {
    State& st = *s_;
    st.once.Do([&] {
        auto [k, err] = build();
        st.key = pkey::Key(k);
        st.err = err;
    });
    return {st.key.get(), st.err};
}

}  // namespace gopp::crypto::detail
