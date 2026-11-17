// The crypto package: identifiers of hash functions, and the interfaces of
// public and private keys that crypto/rsa, crypto/ecdsa and crypto/ed25519
// implement.
//
//     auto h = crypto::New(crypto::SHA256);   // Go: crypto.SHA256.New()
//     h->Write(as_bytes("hello"));
//     slice<byte> digest = h->Sum(slice<byte>());
//
// Go's type switch on a key is a dynamic_cast on a ptr<PublicKey> or
// ptr<PrivateKey> (dynamic_pointer_cast).
#pragma once

// The module interfaces (modules/) import these instead.
#ifndef GOPP_MODULE
#include <gopp/base.hpp>
#include <gopp/errors.hpp>
#include <gopp/hash.hpp>
#include <gopp/io.hpp>
#include <gopp/ptr.hpp>
#include <gopp/slice.hpp>
#include <gopp/string.hpp>

#include <functional>
#include <memory>
#include <span>
#include <utility>
#endif

struct evp_pkey_st;  // OpenSSL's EVP_PKEY

namespace gopp::crypto {

// crypto.Hash: a hash function, with Go's values.
enum class Hash : unsigned {
    MD5 = 2,
    SHA1 = 3,
    SHA224 = 4,
    SHA256 = 5,
    SHA384 = 6,
    SHA512 = 7,
};

// h.New(): a new hash.Hash computing h. Panics if h isn't one of the above.
ptr<hash::Hash> New(Hash h);
// h.Size(): the length in bytes of a digest of h. Panics for an unknown h.
isize Size(Hash h);
// h.Available(): whether h is one of the hash functions go++ has. All of
// the above are always linked in.
bool Available(Hash h);
// h.String(): "SHA-256", "MD5", ...
string String(Hash h);

// crypto.PublicKey. Go's is any; here it has the Equal method that every
// key type of the standard library has.
struct PublicKey : virtual object {
    virtual bool Equal(const PublicKey& x) const = 0;
};

// crypto.SignerOpts: the options of Signer.Sign.
struct SignerOpts {
    // The hash function that was used to make the digest.
    virtual Hash HashFunc() const = 0;
    virtual ~SignerOpts() = default;
};

// The SignerOpts that is only a hash function, which Go writes as the
// crypto.Hash itself: `key->Sign(rand, digest, crypto::HashOpts{crypto::Hash::SHA256})`.
struct HashOpts final : SignerOpts {
    Hash h{};
    HashOpts() = default;
    explicit HashOpts(Hash h) : h(h) {}
    Hash HashFunc() const override { return h; }
};

// crypto.Signer: an opaque private key that can sign.
struct Signer : virtual object {
    virtual ptr<PublicKey> Public() const = 0;
    // Signs digest with the private key, reading randomness from rand
    // where the scheme needs it.
    virtual std::pair<slice<byte>, error> Sign(io::Reader* rand, std::span<const byte> digest,
                                               const SignerOpts& opts) = 0;
};

// crypto.PrivateKey. Go's is any; here every private key is a Signer.
struct PrivateKey : Signer {};

namespace detail {

// The EVP_PKEY that a key of crypto/rsa or crypto/ecdsa builds from its
// fields on first use, once. Copying a key gives the copy a cache of its own.
// Internal.
class KeyCache {
public:
    KeyCache();
    KeyCache(const KeyCache&);
    KeyCache& operator=(const KeyCache&);
    ~KeyCache();

    // The cached key and error. The first call runs build, under a
    // sync::Once, which returns a new EVP_PKEY (owned by the cache from then
    // on) or an error; later calls, and concurrent ones, get its result.
    std::pair<evp_pkey_st*, error> Get(const std::function<std::pair<evp_pkey_st*, error>()>& build) const;

    struct State;

private:
    std::unique_ptr<State> s_;
};

}  // namespace detail

}  // namespace gopp::crypto
