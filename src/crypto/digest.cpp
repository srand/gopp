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
struct digestKind {
    const char* name;
    isize size;
    isize blockSize;

    const EVP_MD* md() const {
        // Fetching is slow; each kind is fetched on first use and kept.
        const EVP_MD* m = EVP_MD_fetch(nullptr, name, nullptr);
        if (!m) panic(std::string("crypto: OpenSSL has no ") + name);
        return m;
    }
};

template <const digestKind& K>
const EVP_MD* mdOf() {
    static const EVP_MD* const m = K.md();
    return m;
}

[[noreturn]] void evpFailed(const char* what) { panic(std::string("crypto: ") + what + " failed"); }

// A running digest. Sum finalizes a copy of the context, so the digest
// goes on, as Go's do.
class evpHash final : public hash::Hash {
public:
    evpHash(const EVP_MD* md, isize size, isize blockSize)
        : md_(md), ctx_(EVP_MD_CTX_new()), size_(size), blockSize_(blockSize) {
        if (!ctx_) evpFailed("EVP_MD_CTX_new");
        Reset();
    }
    ~evpHash() override { EVP_MD_CTX_free(ctx_); }

    std::pair<isize, error> Write(std::span<const byte> p) override {
        if (!p.empty() && EVP_DigestUpdate(ctx_, p.data(), p.size()) != 1) evpFailed("EVP_DigestUpdate");
        return {static_cast<isize>(p.size()), nullptr};
    }

    slice<byte> Sum(slice<byte> b) const override {
        EVP_MD_CTX* c = EVP_MD_CTX_new();
        if (!c || EVP_MD_CTX_copy_ex(c, ctx_) != 1) evpFailed("EVP_MD_CTX_copy_ex");
        unsigned char out[EVP_MAX_MD_SIZE];
        unsigned int n = 0;
        int ok = EVP_DigestFinal_ex(c, out, &n);
        EVP_MD_CTX_free(c);
        if (ok != 1) evpFailed("EVP_DigestFinal_ex");
        return appendSpan(std::move(b), std::span<const byte>(out, n));
    }

    void Reset() override {
        if (EVP_DigestInit_ex(ctx_, md_, nullptr) != 1) evpFailed("EVP_DigestInit_ex");
    }

    isize Size() const override { return size_; }
    isize BlockSize() const override { return blockSize_; }

private:
    const EVP_MD* md_;
    EVP_MD_CTX* ctx_;
    isize size_;
    isize blockSize_;
};

template <const digestKind& K>
ptr<hash::Hash> newHash() {
    return make_ptr<evpHash>(mdOf<K>(), K.size, K.blockSize);
}

template <const digestKind& K>
std::array<byte, static_cast<std::size_t>(K.size)> sum(std::span<const byte> data) {
    std::array<byte, static_cast<std::size_t>(K.size)> out;
    unsigned int n = 0;
    if (EVP_Digest(data.data(), data.size(), out.data(), &n, mdOf<K>(), nullptr) != 1) evpFailed("EVP_Digest");
    return out;
}

constexpr digestKind sha224Kind{"SHA2-224", 28, 64};
constexpr digestKind sha256Kind{"SHA2-256", 32, 64};
constexpr digestKind sha384Kind{"SHA2-384", 48, 128};
constexpr digestKind sha512Kind{"SHA2-512", 64, 128};
constexpr digestKind sha512224Kind{"SHA2-512/224", 28, 128};
constexpr digestKind sha512256Kind{"SHA2-512/256", 32, 128};
constexpr digestKind sha1Kind{"SHA1", 20, 64};
constexpr digestKind md5Kind{"MD5", 16, 64};

}  // namespace

namespace sha256 {
ptr<hash::Hash> New() { return newHash<sha256Kind>(); }
ptr<hash::Hash> New224() { return newHash<sha224Kind>(); }
std::array<byte, 32> Sum256(std::span<const byte> data) { return sum<sha256Kind>(data); }
std::array<byte, 28> Sum224(std::span<const byte> data) { return sum<sha224Kind>(data); }
}  // namespace sha256

namespace sha512 {
ptr<hash::Hash> New() { return newHash<sha512Kind>(); }
ptr<hash::Hash> New384() { return newHash<sha384Kind>(); }
ptr<hash::Hash> New512_224() { return newHash<sha512224Kind>(); }
ptr<hash::Hash> New512_256() { return newHash<sha512256Kind>(); }
std::array<byte, 64> Sum512(std::span<const byte> data) { return sum<sha512Kind>(data); }
std::array<byte, 48> Sum384(std::span<const byte> data) { return sum<sha384Kind>(data); }
std::array<byte, 28> Sum512_224(std::span<const byte> data) { return sum<sha512224Kind>(data); }
std::array<byte, 32> Sum512_256(std::span<const byte> data) { return sum<sha512256Kind>(data); }
}  // namespace sha512

namespace sha1 {
ptr<hash::Hash> New() { return newHash<sha1Kind>(); }
std::array<byte, 20> Sum(std::span<const byte> data) { return sum<sha1Kind>(data); }
}  // namespace sha1

namespace md5 {
ptr<hash::Hash> New() { return newHash<md5Kind>(); }
std::array<byte, 16> Sum(std::span<const byte> data) { return sum<md5Kind>(data); }
}  // namespace md5

}  // namespace gopp
