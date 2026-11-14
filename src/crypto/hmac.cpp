// crypto/hmac, after Go 1.27.3's crypto/internal/fips140/hmac/hmac.go.
#include <gopp/crypto/hmac.hpp>
#include <gopp/crypto/subtle.hpp>

#include <vector>

namespace gopp::hmac {

namespace {

// Go's HMAC, without the marshaling of hash states.
class hmac_hash final : public hash::Hash {
public:
    hmac_hash(ptr<hash::Hash> outer, ptr<hash::Hash> inner, std::span<const byte> key)
        : outer_(std::move(outer)), inner_(std::move(inner)) {
        auto blocksize = static_cast<std::size_t>(inner_->BlockSize());
        ipad_.assign(blocksize, 0);
        opad_.assign(blocksize, 0);
        slice<byte> long_key;
        if (key.size() > blocksize) {
            // If key is too big, hash it.
            outer_->Write(key);
            long_key = outer_->Sum(slice<byte>());
            key = long_key;
        }
        std::copy(key.begin(), key.end(), ipad_.begin());
        std::copy(key.begin(), key.end(), opad_.begin());
        for (auto& b : ipad_) b ^= 0x36;
        for (auto& b : opad_) b ^= 0x5c;
        inner_->Write(ipad_);
    }

    std::pair<isize, error> Write(std::span<const byte> p) override { return inner_->Write(p); }

    slice<byte> Sum(slice<byte> in) const override {
        std::size_t orig_len = in.len();
        in = inner_->Sum(std::move(in));
        outer_->Reset();
        outer_->Write(opad_);
        outer_->Write(std::span<const byte>(in.data() + orig_len, in.len() - orig_len));
        return outer_->Sum(in.Sub(0, orig_len));
    }

    void Reset() override {
        inner_->Reset();
        inner_->Write(ipad_);
    }

    isize Size() const override { return outer_->Size(); }
    isize BlockSize() const override { return inner_->BlockSize(); }

private:
    std::vector<byte> opad_, ipad_;
    ptr<hash::Hash> outer_, inner_;
};

}  // namespace

ptr<hash::Hash> New(std::function<ptr<hash::Hash>()> h, std::span<const byte> key) {
    auto outer = h();
    auto inner = h();
    if (outer == inner) panic("crypto/hmac: hash generation function does not produce unique values");
    return make_ptr<hmac_hash>(std::move(outer), std::move(inner), key);
}

bool Equal(std::span<const byte> mac1, std::span<const byte> mac2) noexcept {
    return subtle::ConstantTimeCompare(mac1, mac2) == 1;
}

}  // namespace gopp::hmac
