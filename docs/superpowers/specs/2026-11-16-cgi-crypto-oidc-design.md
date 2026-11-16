# CGI, public-key crypto, JOSE, OAuth2 and OIDC

## Intent

git++, a review server built on go++, runs `git http-backend` through CGI
and signs users in with OpenID Connect. go++ has none of the pieces. This
spec adds them as ports of the Go code they are named after, each usable on
its own, so that C++ services get the same building blocks that Go ones
have.

The packages, in the order they are built:

1. `net/http/cgi` (host side)
2. `crypto`, `math/big` (minimal), `crypto/elliptic`, `crypto/rsa`,
   `crypto/ecdsa` and `crypto/ed25519`
3. `x/jose`, a subset of go-jose v4
4. `x/oauth2`, golang.org/x/oauth2
5. `x/oidc`, coreos/go-oidc v3, with a fake provider for tests

## Goals

- Go's names, signatures and semantics: `cgi::Handler` is configured with
  the same fields as Go's, `rsa::SignPSS` takes what Go's takes, and Go code
  ports line by line.
- Tests ported from the Go sources, keeping their names, with a comment at
  the top of each file saying what was left out and why.
- The standard-library packages go into the `gopp` library. The
  third-party ports live under `x/<name>/`, laid out like `drivers/sqlite3`,
  each a Jolt library of its own, so a program that doesn't use OIDC
  doesn't link it.

## Non-goals (could come later)

- `cgi::Serve` and `cgi::Request`, the child side of CGI.
- Other key types and formats: DSA, X25519, PKCS#8/x509 parsing, JWE, and
  JWS with HMAC beyond what OIDC needs.
- OAuth2 providers' sub-packages (`oauth2/google`, ...) and JWT-bearer
  flows.
- A `math/big` beyond what the keys need.
- C++ modules for the `x/` libraries.

## Anti-goals

- **Buffering.** A CGI response, or request, that is larger than memory must
  still stream.
- **Verifying with whatever matches.** A token whose `alg` doesn't fit its
  key must fail, never fall back.
- **Hand-written cryptographic primitives.** Everything runs on OpenSSL.

## Approaches

- **Port the Go packages (chosen).** The Go sources and tests are on hand,
  and Go's behavior is the specification.
- *Wrap C libraries* (libcgi-like helpers, a JOSE C library). They have
  other APIs and other edge cases, and the tests wouldn't carry over.
  Rejected.

## Decided

### net/http/cgi

- Namespace `gopp::cgi`, header `<gopp/net/http/cgi.hpp>`, source
  `src/net/http/cgi/host.cpp`, in the `gopp` library.
- `cgi::Handler` is Go's `cgi.Handler`: `Path`, `Root`, `Dir`, `Env`,
  `InheritEnv`, `Logger`, `Args`, `Stderr` and `PathLocationHandler`. Its
  environment, `removeLeadingDuplicates`, header parsing (`Status:`,
  `Location:`, the internal redirect) and error responses are ported from
  `host.go`.
- It runs the child with `os::exec::Cmd`. The request body is the child's
  stdin when `ContentLength != 0`, copied by `Cmd`'s own fiber. The child's
  stdout is read through `StdoutPipe()` and copied straight to the
  `ResponseWriter`, which is flushed after each read when it is a `Flusher`
  (git's sideband progress depends on that).
- When copying fails, the handler kills the child and waits for it, so no
  process or fiber is left behind when the client goes away.
- `PathLocationHandler` that is nil means `http::DefaultServeMux()` here.
- Chunked request bodies get 400, as in Go: CGI forbids them.
- Tests: Go's `host_test.go`, with the test program running itself as the
  CGI child through `go_helper` (in place of Go's Perl script), and two
  added: `CGIStreamsLargeBody` and `CGIClientDisconnectKillsChild`.

### Crypto

- `crypto/rsa` (PKCS#1 v1.5 and PSS signing and verification, key
  generation), `crypto/ecdsa` (P-256, P-384, P-521; ASN.1 signatures) and
  `crypto/ed25519`, on OpenSSL's EVP (`EVP_PKEY`, `EVP_DigestSign*`,
  `EVP_DigestVerify*`). Where Go takes a `random io.Reader`, go++ takes it
  and ignores it, as Go 1.24 does for most keys.
- A minimal `math/big` (`Int`: SetBytes, Bytes, Cmp, Int64, BitLen and a few
  more) so that `rsa::PublicKey::N` and `ecdsa::PublicKey::X, Y` keep Go's
  shapes.

### x/jose, x/oauth2, x/oidc

- Namespaces `gopp::jose`, `gopp::oauth2` and `gopp::oidc`; the libraries
  `gopp/x/jose`, `gopp/x/oauth2` and `gopp/x/oidc`.
- jose: `JSONWebKey` and `JSONWebKeySet` for RSA, EC and OKP keys,
  `ParseSigned` with a list of allowed algorithms and `Verify`, and
  `jwt::ParseSigned`, `Claims` and `Claims::Validate`.
- oauth2: `Config` with `AuthCodeURL`, `Exchange` and `TokenSource`, PKCE,
  and the device flow.
- oidc: `NewProvider`, `RemoteKeySet` (which fetches the key set again once
  for an unknown key ID, rate-limited), `IDTokenVerifier::Verify`,
  `IDToken::Claims`, `UserInfo`, and an `oidctest` fake provider.

## Left to the builder

- Whether `rsa::PublicKey` holds a `big::Int` or bytes behind Go's
  signatures: choose the smaller design that keeps call sites Go-shaped.
- Which parts of go-jose's and go-oidc's tests apply to the subset.
