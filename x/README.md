# x/: ports of Go libraries outside the standard library

Each directory is a Jolt library of its own, so a program links only what
it uses. Each keeps the upstream package's names and semantics; the header
of each library says where go++ differs. The tests are ported from
upstream's, as `tests/go_x_<name>_test.cpp`.

| Directory | Library | Header | Ported from | License |
|---|---|---|---|---|
| `jose` | `gopp/x/jose` | `<gopp/jose.hpp>`, `<gopp/jose/jwt.hpp>` | [go-jose/go-jose](https://github.com/go-jose/go-jose) v4.1.7: JSON Web Keys, compact JWS and JWT | Apache 2.0 ([jose/LICENSE](jose/LICENSE)) |
| `oauth2` | `gopp/x/oauth2` | `<gopp/oauth2.hpp>` | [golang.org/x/oauth2](https://pkg.go.dev/golang.org/x/oauth2) v0.36.0: the OAuth 2.0 client, PKCE and the device flow | BSD-3-Clause ([oauth2/LICENSE](oauth2/LICENSE)) |
| `oidc` | `gopp/x/oidc` | `<gopp/oidc.hpp>`, `<gopp/oidc/oidctest.hpp>` | [coreos/go-oidc](https://github.com/coreos/go-oidc) v3.21.0: OpenID Connect discovery, ID token verification, UserInfo, and a fake provider for tests | Apache 2.0 ([oidc/LICENSE](oidc/LICENSE)) |

`gopp/x/oidc` requires `gopp/x/jose` and `gopp/x/oauth2`; build it with
`jolt build gopp/x/oidc`.
