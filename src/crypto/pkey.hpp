// Internal to go++'s crypto/rsa, crypto/ecdsa and crypto/ed25519: OpenSSL 3
// EVP_PKEYs, owned, and built from parameters with OSSL_PARAM_BLD.
//
//     pkey::ParamBuilder b;
//     b.add(OSSL_PKEY_PARAM_RSA_N, n);
//     auto [key, err] = pkey::fromParams("RSA", EVP_PKEY_PUBLIC_KEY, b, "crypto/rsa");
#pragma once

#include <gopp/base.hpp>
#include <gopp/crypto.hpp>
#include <gopp/errors.hpp>
#include <gopp/math/big.hpp>

#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace gopp::pkey {

// An EVP_PKEY, freed with the Key. Moves, doesn't copy.
class Key {
public:
    Key() = default;
    explicit Key(EVP_PKEY* p) : p_(p) {}
    Key(Key&& o) noexcept : p_(std::exchange(o.p_, nullptr)) {}
    Key& operator=(Key&& o) noexcept {
        std::swap(p_, o.p_);
        return *this;
    }
    ~Key() { EVP_PKEY_free(p_); }

    EVP_PKEY* get() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }

private:
    EVP_PKEY* p_ = nullptr;
};

struct ctxDeleter {
    void operator()(EVP_PKEY_CTX* c) const { EVP_PKEY_CTX_free(c); }
};
// An EVP_PKEY_CTX, freed with the pointer.
using Ctx = std::unique_ptr<EVP_PKEY_CTX, ctxDeleter>;

struct bnDeleter {
    void operator()(BIGNUM* b) const { BN_free(b); }
};
using BN = std::unique_ptr<BIGNUM, bnDeleter>;

// An error "<pkg>: <what> failed: <OpenSSL's reason>", draining OpenSSL's
// error queue.
inline error openSSLError(const char* pkg, const char* what) {
    std::string msg = std::string(pkg) + ": " + what + " failed";
    if (unsigned long e = ERR_get_error()) {
        char buf[256];
        ERR_error_string_n(e, buf, sizeof buf);
        msg += ": ";
        msg += buf;
    }
    ERR_clear_error();
    return errors::New(string(msg));
}

// big::Int to and from a BIGNUM.
inline const BIGNUM* bn(const big::Int& x) { return static_cast<const BIGNUM*>(x.bn()); }
inline big::Int toInt(const BIGNUM* b) {
    std::vector<unsigned char> buf(static_cast<size_t>(BN_num_bytes(b)));
    if (!buf.empty()) BN_bn2bin(b, buf.data());
    big::Int z;
    z.SetBytes(buf);
    return z;
}

// The big::Int parameter of key by name, such as OSSL_PKEY_PARAM_RSA_N.
inline std::pair<big::Int, bool> getInt(const EVP_PKEY* key, const char* name) {
    BIGNUM* b = nullptr;
    if (EVP_PKEY_get_bn_param(key, name, &b) != 1) return {big::Int(), false};
    BN owned(b);
    return {toInt(b), true};
}

// The name OpenSSL knows h by, or "" for an unset hash.
inline const char* mdName(crypto::Hash h) {
    switch (h) {
        case crypto::Hash::MD5: return "MD5";
        case crypto::Hash::SHA1: return "SHA1";
        case crypto::Hash::SHA224: return "SHA2-224";
        case crypto::Hash::SHA256: return "SHA2-256";
        case crypto::Hash::SHA384: return "SHA2-384";
        case crypto::Hash::SHA512: return "SHA2-512";
    }
    return "";
}

// Parameters of a key, for fromParams. The BIGNUMs and strings added must
// stay alive until fromParams (OSSL_PARAM_BLD copies at build time).
class ParamBuilder {
public:
    ParamBuilder() : bld_(OSSL_PARAM_BLD_new()) {}
    ParamBuilder(const ParamBuilder&) = delete;
    ParamBuilder& operator=(const ParamBuilder&) = delete;
    ~ParamBuilder() { OSSL_PARAM_BLD_free(bld_); }

    void add(const char* name, const BIGNUM* v) { ok_ = ok_ && bld_ && OSSL_PARAM_BLD_push_BN(bld_, name, v) == 1; }
    void add(const char* name, const big::Int& v) { add(name, bn(v)); }
    void addUtf8(const char* name, const char* v) {
        ok_ = ok_ && bld_ && OSSL_PARAM_BLD_push_utf8_string(bld_, name, v, 0) == 1;
    }
    void addOctets(const char* name, const void* v, size_t n) {
        ok_ = ok_ && bld_ && OSSL_PARAM_BLD_push_octet_string(bld_, name, v, n) == 1;
    }

    // The parameters, or nullptr if a push failed. Free with OSSL_PARAM_free.
    OSSL_PARAM* build() { return ok_ && bld_ ? OSSL_PARAM_BLD_to_param(bld_) : nullptr; }

private:
    OSSL_PARAM_BLD* bld_;
    bool ok_ = true;
};

// A key of keytype ("RSA", "EC", "ED25519") with the parameters, of which
// selection (EVP_PKEY_PUBLIC_KEY, EVP_PKEY_KEYPAIR) says which kind.
inline std::pair<Key, error> fromParams(const char* keytype, int selection, ParamBuilder& b, const char* pkg) {
    OSSL_PARAM* params = b.build();
    if (!params) return {Key(), openSSLError(pkg, "building key parameters")};
    Ctx ctx(EVP_PKEY_CTX_new_from_name(nullptr, keytype, nullptr));
    EVP_PKEY* p = nullptr;
    bool ok = ctx && EVP_PKEY_fromdata_init(ctx.get()) == 1 && EVP_PKEY_fromdata(ctx.get(), &p, selection, params) == 1;
    OSSL_PARAM_free(params);
    if (!ok) return {Key(), openSSLError(pkg, "importing the key")};
    return {Key(p), nullptr};
}

}  // namespace gopp::pkey
