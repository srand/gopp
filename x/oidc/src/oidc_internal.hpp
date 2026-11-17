// x/oidc's internals: what go-oidc's files share, and what its in-package
// tests reach.
#pragma once

#include <gopp/oidc.hpp>

#include <array>
#include <functional>
#include <optional>
#include <span>
#include <string_view>

namespace gopp::oidc::internal {

// allAlgs: the algorithms go-oidc supports.
inline constexpr std::array<std::string_view, 10> allAlgs = {RS256, RS384, RS512, ES256, ES384,
                                                             ES512, PS256, PS384, PS512, EdDSA};
bool supportedAlg(std::string_view alg);

// getClient: the http::Client of ctx (ClientContext), if it has one.
std::optional<http::Client> getClient(const ptr<context::Context>& ctx);

// doRequest: sends req with the client of ctx (or the default client), as
// a request of ctx.
std::pair<ptr<http::Response>, error> doRequest(const ptr<context::Context>& ctx, ptr<http::Request> req);
// doRequest with a client given (std::nullopt: the default client).
std::pair<ptr<http::Response>, error> doRequest(const std::optional<http::Client>& client,
                                                const ptr<context::Context>& ctx, ptr<http::Request> req);

// readBody: the body of r, up to MaxResponseSize; an error if it is longer.
std::pair<slice<byte>, error> readBody(http::Response& r);

// unmarshalResp: the body as JSON into v, with an error that says what the
// Content-Type was if it isn't JSON.
error unmarshalResp(const http::Response& r, std::span<const byte> body, const std::function<error()>& unmarshal);

// parsedJWTKey: the context key under which Verify passes the parsed token
// (a jose::JSONWebSignature) to a RemoteKeySet, so that it isn't parsed
// twice.
extern const void* const parsedJWTKey;

// For the tests: the private parts of IDToken, Provider and RemoteKeySet.
struct idTokenAccess {
    static string& sigAlgorithm(IDToken& t) { return t.sigAlgorithm_; }
};

struct providerAccess {
    static const string& issuer(const Provider& p) { return p.issuer_; }
    static const slice<string>& algorithms(const Provider& p) { return p.algorithms_; }
};

struct remoteKeySetAccess {
    // newRemoteKeySet, with the clock of the refetch interval.
    static ptr<RemoteKeySet> newRemoteKeySet(ptr<context::Context> ctx, std::string_view jwksURL,
                                             std::function<time::Time()> now = nullptr);
    // rks.verify(ctx, jws)
    static std::pair<slice<byte>, error> verify(RemoteKeySet& rks, const ptr<context::Context>& ctx,
                                                const jose::JSONWebSignature& jws) {
        return rks.verify(ctx, jws);
    }
};

}  // namespace gopp::oidc::internal
