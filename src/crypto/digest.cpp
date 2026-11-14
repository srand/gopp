// crypto/sha256, sha512, sha1 and md5 on OpenSSL's EVP digests.
#include <gopp/crypto/md5.hpp>
#include <gopp/crypto/sha1.hpp>
#include <gopp/crypto/sha256.hpp>
#include <gopp/crypto/sha512.hpp>

#include <openssl/evp.h>

#include <string>

namespace gopp {

namespace {

// An EVP_MD fetched once, by name.
struct digest_kind {
    const char* name;
    isize size;
    isize block_size;

    const EVP_MD* md() const {
        // Fetching is slow; each kind is fetched on first use and kept.
        const EVP_MD* m = EVP_MD_fetch(nullptr, name, nullptr);
        if (!m) panic(std::string("crypto: OpenSSL has no ") + name);
        return m;
    }
};

template <const digest_kind& K>
const EVP_MD* md_of() {
    static const EVP_MD* const m = K.md();
    return m;
}

[[noreturn]] void evp_failed(const char* what) { panic(std::string("crypto: ") + what + " failed"); }

// A running digest. Sum finalizes a copy of the context, so the digest
// goes on, as Go's do.
class evp_hash final : public hash::Hash {
public:
    evp_hash(const EVP_MD* md, isize size, isize block_size)
        : md_(md), ctx_(EVP_MD_CTX_new()), size_(size), block_size_(block_size) {
        if (!ctx_) evp_failed("EVP_MD_CTX_new");
        Reset();
    }
    ~evp_hash() override { EVP_MD_CTX_free(ctx_); }

    std::pair<isize, error> Write(std::span<const byte> p) override {
        if (!p.empty() && EVP_DigestUpdate(ctx_, p.data(), p.size()) != 1) evp_failed("EVP_DigestUpdate");
        return {static_cast<isize>(p.size()), nullptr};
    }

    bytes Sum(bytes b) const override {
        EVP_MD_CTX* c = EVP_MD_CTX_new();
        if (!c || EVP_MD_CTX_copy_ex(c, ctx_) != 1) evp_failed("EVP_MD_CTX_copy_ex");
        unsigned char out[EVP_MAX_MD_SIZE];
        unsigned int n = 0;
        int ok = EVP_DigestFinal_ex(c, out, &n);
        EVP_MD_CTX_free(c);
        if (ok != 1) evp_failed("EVP_DigestFinal_ex");
        return append_span(std::move(b), std::span<const byte>(out, n));
    }

    void Reset() override {
        if (EVP_DigestInit_ex(ctx_, md_, nullptr) != 1) evp_failed("EVP_DigestInit_ex");
    }

    isize Size() const override { return size_; }
    isize BlockSize() const override { return block_size_; }

private:
    const EVP_MD* md_;
    EVP_MD_CTX* ctx_;
    isize size_;
    isize block_size_;
};

template <const digest_kind& K>
ptr<hash::Hash> new_hash() {
    return make_ptr<evp_hash>(md_of<K>(), K.size, K.block_size);
}

template <const digest_kind& K>
std::array<byte, static_cast<std::size_t>(K.size)> sum(std::span<const byte> data) {
    std::array<byte, static_cast<std::size_t>(K.size)> out;
    unsigned int n = 0;
    if (EVP_Digest(data.data(), data.size(), out.data(), &n, md_of<K>(), nullptr) != 1) evp_failed("EVP_Digest");
    return out;
}

constexpr digest_kind sha224_kind{"SHA2-224", 28, 64};
constexpr digest_kind sha256_kind{"SHA2-256", 32, 64};
constexpr digest_kind sha384_kind{"SHA2-384", 48, 128};
constexpr digest_kind sha512_kind{"SHA2-512", 64, 128};
constexpr digest_kind sha512_224_kind{"SHA2-512/224", 28, 128};
constexpr digest_kind sha512_256_kind{"SHA2-512/256", 32, 128};
constexpr digest_kind sha1_kind{"SHA1", 20, 64};
constexpr digest_kind md5_kind{"MD5", 16, 64};

}  // namespace

namespace sha256 {
ptr<hash::Hash> New() { return new_hash<sha256_kind>(); }
ptr<hash::Hash> New224() { return new_hash<sha224_kind>(); }
std::array<byte, 32> Sum256(std::span<const byte> data) { return sum<sha256_kind>(data); }
std::array<byte, 28> Sum224(std::span<const byte> data) { return sum<sha224_kind>(data); }
}  // namespace sha256

namespace sha512 {
ptr<hash::Hash> New() { return new_hash<sha512_kind>(); }
ptr<hash::Hash> New384() { return new_hash<sha384_kind>(); }
ptr<hash::Hash> New512_224() { return new_hash<sha512_224_kind>(); }
ptr<hash::Hash> New512_256() { return new_hash<sha512_256_kind>(); }
std::array<byte, 64> Sum512(std::span<const byte> data) { return sum<sha512_kind>(data); }
std::array<byte, 48> Sum384(std::span<const byte> data) { return sum<sha384_kind>(data); }
std::array<byte, 28> Sum512_224(std::span<const byte> data) { return sum<sha512_224_kind>(data); }
std::array<byte, 32> Sum512_256(std::span<const byte> data) { return sum<sha512_256_kind>(data); }
}  // namespace sha512

namespace sha1 {
ptr<hash::Hash> New() { return new_hash<sha1_kind>(); }
std::array<byte, 20> Sum(std::span<const byte> data) { return sum<sha1_kind>(data); }
}  // namespace sha1

namespace md5 {
ptr<hash::Hash> New() { return new_hash<md5_kind>(); }
std::array<byte, 16> Sum(std::span<const byte> data) { return sum<md5_kind>(data); }
}  // namespace md5

}  // namespace gopp
