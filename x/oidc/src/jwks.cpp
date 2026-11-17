// Key sets, ported from coreos/go-oidc v3.21.0's jwks.go.
#include "oidc_internal.hpp"

#include <gopp/chan.hpp>
#include <gopp/crypto/ecdsa.hpp>
#include <gopp/crypto/ed25519.hpp>
#include <gopp/crypto/rsa.hpp>
#include <gopp/fmt.hpp>
#include <gopp/runtime.hpp>

#include <mutex>
#include <shared_mutex>

namespace gopp::oidc {

namespace internal {

const void* const parsedJWTKey = &parsedJWTKey;

ptr<RemoteKeySet> remoteKeySetAccess::newRemoteKeySet(ptr<context::Context> ctx, std::string_view jwksURL,
                                                      std::function<time::Time()> now) {
    ptr<RemoteKeySet> r(new RemoteKeySet());
    r->jwksURL_ = string(jwksURL);
    // For historical reasons, go-oidc uses contexts for configuration, not
    // just cancellation: only the client of ctx is used.
    r->client_ = getClient(ctx);
    r->now_ = now ? std::move(now) : [] { return time::Now(); };
    return r;
}

}  // namespace internal

namespace {

struct rawKeySet {
    [[= json::tag("keys")]] slice<json::RawMessage> Keys;
};

struct keyMetadata {
    [[= json::tag("alg")]] string Alg;
};

// jwkJSON.UnmarshalJSON: the keys of a JWK Set, without those of an
// algorithm that go-oidc doesn't support (such as ES256K) or of a key type
// that x/jose doesn't (such as Ed448).
std::pair<slice<jose::JSONWebKey>, error> unmarshalKeySet(std::span<const byte> data) {
    rawKeySet raw;
    if (error err = json::Unmarshal(data, &raw)) return {slice<jose::JSONWebKey>(), err};
    slice<jose::JSONWebKey> keys;
    for (const auto& key : raw.Keys) {
        keyMetadata metadata;
        if (error err = json::Unmarshal(key.data, &metadata)) return {slice<jose::JSONWebKey>(), err};
        // Algorithms are optional, but a key that advertises one that isn't
        // supported is skipped rather than failing the whole set.
        if (!metadata.Alg.empty() && !internal::supportedAlg(metadata.Alg.view())) continue;
        jose::JSONWebKey jwk;
        if (error err = jwk.UnmarshalJSON(key.data)) {
            // RFC 7517, section 5: implementations SHOULD ignore JWKs that
            // use "kty" values that are not understood by them.
            if (errors::Is(err, jose::ErrUnsupportedKeyType)) continue;
            return {slice<jose::JSONWebKey>(), err};
        }
        keys = append(keys, jwk);
    }
    return {keys, nullptr};
}

}  // namespace

// ---------------------------------------------------------------------------
// StaticKeySet

std::pair<slice<byte>, error> StaticKeySet::VerifySignature(ptr<context::Context> ctx, std::string_view jwt) {
    // Algorithms are already checked by Verifier, so this parse method
    // accepts any algorithm.
    auto [jws, err] = jose::ParseSigned(jwt, internal::allAlgs);
    if (err) return {slice<byte>(), fmt::Errorf("parsing jwt: {}", err)};
    for (const auto& pub : PublicKeys) {
        const auto* p = pub.get();
        if (!dynamic_cast<const rsa::PublicKey*>(p) && !dynamic_cast<const ecdsa::PublicKey*>(p) &&
            !dynamic_cast<const ed25519::PublicKey*>(p)) {
            return {slice<byte>(), errors::New("invalid public key type provided")};
        }
        auto [payload, err2] = jws->Verify(static_cast<const object&>(*p));
        if (!err2) return {payload, nullptr};
    }
    return {slice<byte>(), errors::New("no public keys able to verify jwt")};
}

// ---------------------------------------------------------------------------
// RemoteKeySet

// inflight: a fetch that several fibers wait for.
struct RemoteKeySet::inflight final : object {
    chan<empty> doneCh = make_chan<empty>();
    slice<jose::JSONWebKey> keys;
    error err;

    // done: records the result, once, and wakes the waiting fibers.
    void done(slice<jose::JSONWebKey> k, error e) {
        keys = std::move(k);
        err = std::move(e);
        doneCh.close();
    }
};

ptr<RemoteKeySet> NewRemoteKeySet(ptr<context::Context> ctx, std::string_view jwksURL) {
    return internal::remoteKeySetAccess::newRemoteKeySet(std::move(ctx), jwksURL);
}

std::pair<slice<byte>, error> RemoteKeySet::VerifySignature(ptr<context::Context> ctx, std::string_view jwt) {
    ptr<jose::JSONWebSignature> jws;
    if (ctx) jws = dynamic_pointer_cast<jose::JSONWebSignature>(ctx->Value(internal::parsedJWTKey));
    if (!jws) {
        // The algorithms are enforced by the verifier, which passes the
        // token it parsed in the context: this path is for direct calls.
        auto [parsed, err] = jose::ParseSigned(jwt, internal::allAlgs);
        if (err) return {slice<byte>(), fmt::Errorf("oidc: malformed jwt: {}", err)};
        jws = parsed;
    }
    return verify(ctx, *jws);
}

std::pair<slice<byte>, error> RemoteKeySet::verify(const ptr<context::Context>& ctx, const jose::JSONWebSignature& jws) {
    // We don't support JWTs signed with multiple signatures.
    string keyID;
    if (!jws.Signatures.empty()) keyID = jws.Signatures[0].Header.KeyID;

    auto tryKeys = [&](const slice<jose::JSONWebKey>& keys) -> std::optional<slice<byte>> {
        for (const auto& key : keys) {
            if (keyID.empty() || key.KeyID == keyID) {
                if (auto [payload, err] = jws.Verify(key); !err) return payload;
            }
        }
        return std::nullopt;
    };

    if (auto payload = tryKeys(keysFromCache())) return {*payload, nullptr};

    // If the kid doesn't match, check for new keys from the remote. This is
    // the strategy recommended by the spec:
    // https://openid.net/specs/openid-connect-core-1_0.html#RotateSigKeys
    auto [keys, err] = keysFromRemote(ctx);
    if (err) return {slice<byte>(), fmt::Errorf("fetching keys {}", fmt::W(err))};
    if (auto payload = tryKeys(keys)) return {*payload, nullptr};
    return {slice<byte>(), errors::New("failed to verify id token signature")};
}

slice<jose::JSONWebKey> RemoteKeySet::keysFromCache() {
    std::shared_lock lock(mu_);
    return cachedKeys_;
}

std::pair<slice<jose::JSONWebKey>, error> RemoteKeySet::keysFromRemote(const ptr<context::Context>& ctx) {
    ptr<inflight> inf;
    {
        std::lock_guard lock(mu_);
        // go++: not again within RemoteKeySetRefetchInterval of the last
        // fetch, so that tokens with made-up key IDs don't make a fetch
        // each. The keys (or error) of that fetch stand until then.
        if (!inflight_ && fetched_ && now_().Before(lastFetch_.Add(RemoteKeySetRefetchInterval))) {
            return {cachedKeys_, lastErr_};
        }
        if (!inflight_) {
            // This fiber has exclusive ownership over the current inflight
            // request, which it releases once done.
            inflight_ = make_ptr<inflight>();
            go([self = ptr<RemoteKeySet>(this), inf = inflight_] {
                auto [keys, err] = self->updateKeys();
                {
                    std::lock_guard lock(self->mu_);
                    if (!err) self->cachedKeys_ = keys;
                    self->inflight_ = nullptr;
                    self->fetched_ = true;
                    self->lastFetch_ = self->now_();
                    self->lastErr_ = err;
                }
                inf->done(keys, err);
            });
        }
        inf = inflight_;
    }

    error ctxErr;
    chan<empty> ctxDone = ctx ? ctx->Done() : chan<empty>();
    select(recv(ctxDone, [&] { ctxErr = ctx->Err(); }), recv(inf->doneCh, [] {}));
    if (ctxErr) return {slice<jose::JSONWebKey>(), ctxErr};
    return {inf->keys, inf->err};
}

std::pair<slice<jose::JSONWebKey>, error> RemoteKeySet::updateKeys() {
    auto [req, err] = http::NewRequest("GET", jwksURL_.view());
    if (err) return {slice<jose::JSONWebKey>(), fmt::Errorf("oidc: can't create request: {}", err)};
    req->Header.Set("Cache-Control", "no-cache");

    auto [resp, err2] = internal::doRequest(client_, context::Background(), req);
    if (err2) return {slice<jose::JSONWebKey>(), fmt::Errorf("oidc: get keys failed {}", fmt::W(err2))};
    auto [body, err3] = internal::readBody(*resp);
    if (err3) return {slice<jose::JSONWebKey>(), fmt::Errorf("unable to read response body: {}", err3)};
    if (resp->StatusCode != http::StatusOK) {
        return {slice<jose::JSONWebKey>(), fmt::Errorf("oidc: get keys failed: {} {}", resp->Status, asStringView(body))};
    }

    slice<jose::JSONWebKey> keys;
    error uerr = internal::unmarshalResp(*resp, body, [&] {
        auto [k, e] = unmarshalKeySet(body);
        keys = k;
        return e;
    });
    if (uerr) return {slice<jose::JSONWebKey>(), fmt::Errorf("oidc: failed to decode keys: {} {}", uerr, asStringView(body))};
    return {keys, nullptr};
}

}  // namespace gopp::oidc
