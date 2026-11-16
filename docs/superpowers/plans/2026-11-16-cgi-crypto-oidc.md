# CGI, public-key crypto, JOSE, OAuth2 and OIDC: Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add what a web service needs to run official tools over CGI and to
sign users in with OpenID Connect:
- `net/http/cgi`
- `crypto/rsa`, `crypto/ecdsa` and `crypto/ed25519` (with `crypto`,
  `crypto/elliptic` and a minimal `math/big`)
- ports of go-jose v4, golang.org/x/oauth2 and coreos/go-oidc v3

**Architecture:** Each package is a port of the Go code it is named after,
with Go's names, signatures and tests:
- `net/http/cgi` and the crypto packages belong to Go's standard library,
  so they go into the `gopp` library. The crypto runs on OpenSSL's EVP,
  which go++ already links for TLS.
- The third-party ports live under `x/<name>/`, laid out like
  `drivers/sqlite3/`. Each is a Jolt library of its own, so programs that
  don't use them don't link them.

**Tech Stack:** C++26 (GCC 16, `-freflection`), OpenSSL 3 EVP, GoogleTest,
Jolt.

**Spec:** `/home/rbrt/git/git++/docs/superpowers/specs/2026-11-16-gitpp-design.md`,
the section "go++ additions". Task 1 writes go++'s own spec for these
packages.

**Sources to port** (all on this machine):
- `net/http/cgi`: `/usr/lib/go-1.26/src/net/http/cgi/`
- crypto tests: `/usr/lib/go-1.26/src/crypto/{rsa,ecdsa,ed25519}/`
- go-jose: `~/go/pkg/mod/github.com/go-jose/go-jose/v4@v4.1.7/`
- oauth2: `~/go/pkg/mod/golang.org/x/oauth2@v0.36.0/`
- go-oidc: `~/go/pkg/mod/github.com/coreos/go-oidc/v3@v3.21.0/oidc/`

## Global Constraints

- **Branch and worktree.** Work on branch `auth` in a git worktree at
  `/home/rbrt/git/go++/.worktrees/auth`, created from `master`. The main
  checkout has uncommitted work of the user's, which must not be touched
  or committed.
- **Commits.** Use straight history and the repository's commit subject
  style (`net/http/cgi: …`, `crypto/rsa: …`, `x/oidc: …`).
- **API style.** Keep Go's names and semantics, as the rest of go++ does:
  - namespaces such as `gopp::cgi`, `gopp::rsa` and `gopp::oidc`
  - `std::pair<T, error>` for Go's `(T, error)`, `ptr<T>`, `slice<T>`,
    `string`
  - every header starts with a comment block saying what it is, with a
    short example
- **Headers.** Public headers keep their `#include`s in one block inside
  `#ifndef GOPP_MODULE`. After adding a header to `include/gopp/`, run
  `python3 modules/generate.py`. The `x/` libraries get no modules, as
  with `drivers/sqlite3`.
- **Tests** are ported from the named Go test files and keep Go's test
  names (`TEST(GoCGI, CGIBasicGet)`). Each test file starts with a comment
  that names its source, its Go version, and what was left out and why,
  as `tests/go_crypto_test.cpp` does.
- **Licenses.** Copy each third-party license into its directory:
  - `x/jose/LICENSE` (Apache-2.0)
  - `x/oauth2/LICENSE` (BSD-3-Clause)
  - `x/oidc/LICENSE` (Apache-2.0)
- **Commands:**
  - everything: `jolt build test`
  - filtered: `jolt build "test:filter=GoCGI.*"`
  - leak check: `jolt build "test:leaks=true,repeat=2,filter=GoCGI.*"`
- **Crypto.** Everything runs on OpenSSL 3 EVP (`EVP_PKEY`,
  `EVP_PKEY_fromdata`, `EVP_DigestSign*`/`EVP_DigestVerify*`). Nothing
  outside OpenSSL is used, and there are no hand-written primitives.
  - Where Go takes a `random io.Reader` for signing or key generation, go++
    takes it and ignores it, as Go 1.24+ does for most keys. OpenSSL's DRBG
    is used.
- **Warnings.** The build is `-Wall -Wextra -pedantic -Werror`. Keep it
  clean.

## Review Focus

The inputs and conditions below are most likely to bite users of these
packages. Each line names the task whose tests must pin it:

1. **A CGI response larger than memory streams.** `cgi::Handler` must copy
   the child's stdout to the `ResponseWriter` as it arrives. It must not
   read the whole body first: git++ streams multi-gigabyte packs through
   it. The request body must stream into the child's stdin the same way.
   (Task 1: `CGIStreamsLargeBody`.)
2. **A JWT that names an algorithm the key doesn't fit** (`alg: none`,
   `HS256` against an RSA JWK, `RS256` against an EC key) must fail to
   verify. It must never verify with whatever happens to match.
   (Task 6: `GoJWS.UnexpectedAlgorithmCheckedBeforeHeaders`, plus the
   added `GoJWS.RejectsAlgNone`.)
3. **An unknown key ID refetches JWKS once, rate-limited.** It must not
   refetch on every request: a flood of tokens with garbage `kid`s must
   not turn into a flood of requests to the provider. (Task 8:
   `GoOIDC.Rotation`, plus the added `GoOIDC.UnknownKidDoesNotRefetchPerRequest`.)
4. **Tokens are checked for expiry with the clock and allowed skew**,
   including a token whose `exp` is a float or a very large number.
   (Task 6: `GoJWT.NumericDateOutOfRange` and `GoJWT.IntegerAndFloatsNormalize`.)
5. **The child process ends when the client disconnects mid-response.**
   The handler must kill and reap it, with no zombie and no stuck fiber.
   (Task 1: the added `CGIClientDisconnectKillsChild`.)

---

### Task 1: `net/http/cgi` (the host side)

**Files:**
- Create: `docs/superpowers/specs/2026-11-16-cgi-crypto-oidc-design.md`.
  This is go++'s short spec for all of these packages, in the format of
  `2026-11-13-sqlite-driver-design.md`, with a section per package that
  summarizes the git++ design's "go++ additions" and the decisions in this
  plan.
- Create: `include/gopp/net/http/cgi.hpp`
- Create: `src/net/http/cgi/host.cpp`
- Create: `tests/go_net_http_cgi_test.cpp`
- Modify: `modules/` (regenerated)

**Interfaces:**
- Produces (namespace `gopp::cgi`):
  ```cpp
  struct Handler final : http::Handler {
      string Path;                  // the CGI executable
      string Root;                  // the URL prefix the handler is at; "" means "/"
      string Dir;                   // working directory; "" means Path's directory
      slice<string> Env;            // extra "key=value"
      slice<string> InheritEnv;     // names of this process's variables to pass on
      ptr<log::Logger> Logger;      // nil: log::Default()
      slice<string> Args;           // extra arguments
      ptr<io::Writer> Stderr;       // nil: os::Stderr
      ptr<http::Handler> PathLocationHandler;  // for local redirects; nil: http::DefaultServeMux
      void ServeHTTP(http::ResponseWriter& w, http::Request& r) override;
  };
  ```
- Produces a test-only hook: none. The tests run the test program itself as
  the CGI child, through `go_helper::registerHelper` (`tests/go_helper.hpp`).

- [ ] **Step 1: Create the worktree, branch `auth`, and the go++ spec**

  ```bash
  cd /home/rbrt/git/go++ && git worktree add .worktrees/auth -b auth master
  ```

  Add `.worktrees/` to `.gitignore` if it isn't there, in its own commit.
  This plan was written into the main checkout as an untracked file. Move
  it into the worktree at the same path (`mv`, not copy, so the main
  checkout is left as the user had it). Write the spec file and commit
  both (`docs: CGI, crypto, JOSE, OAuth2 and OIDC design and plan`).

- [ ] **Step 2: Port `host_test.go` into `tests/go_net_http_cgi_test.cpp`**

  - Go runs `testdata/test.cgi`, a Perl script. Replace it with a helper
    command registered as `"cgi-test"`, which does what test.cgi does:
    - prints the headers
    - prints `test=Hello CGI` and each `param-*` from the query and form
    - prints `env-*` for each variable
    - prints `cwd=`
    - and on `?loc=`, `?bigresponse=1`, `?writestderr=1` and `?exit=` does
      what test.cgi does
  - Port these tests from `host_test.go`: CGIBasicGet, CGIEnvIPv6,
    CGIBasicGetAbsPath, PathInfo, PathInfoDirRoot, DupHeaders,
    DropProxyHeader, PathInfoNoRoot, CGIBasicPost, CGIPostChunked,
    Redirect, InternalRedirect, CopyError, Dir, EnvOverride,
    HandlerStderr and RemoveLeadingDuplicates.
  - Add `CGIStreamsLargeBody`. The helper writes 256 MiB in 64 KiB writes
    on `?stream=268435456`. The test reads the response through an
    `httptest::Server` and checks the byte count. It also checks the
    test's peak RSS growth from `getrusage(RUSAGE_SELF).ru_maxrss` before
    and after, which must be below 64 MiB.
  - Add `CGIClientDisconnectKillsChild`. The helper writes its PID, then
    writes forever. The client reads 1 MiB, then closes the body. Within 5
    s, `kill(pid, 0)` must return `ESRCH`.

- [ ] **Step 3: Run the tests and see them fail to compile**

  Run: `jolt build "test:filter=GoCGI.*"`
  Expected: a compile error, because `gopp/net/http/cgi.hpp` doesn't exist.

- [ ] **Step 4: Implement `cgi::Handler` in `src/net/http/cgi/host.cpp`**

  Port `host.go` line by line:
  - the environment: `SERVER_SOFTWARE=go`, `HTTP_*` headers, `PATH_INFO`,
    `REMOTE_ADDR`/`REMOTE_HOST`/`REMOTE_PORT`, `HTTPS`, `CONTENT_LENGTH`
    and `CONTENT_TYPE`
  - `removeLeadingDuplicates`
  - parsing the headers with `textproto`/`bufio`
  - the `Status:` and `Location:` handling, and the internal redirect

  Run the child with `exec::Cmd`:
  - `Stdin` is `r.Body` when `ContentLength != 0`.
  - Use `StdoutPipe()`.
  - After the headers, copy with `io::Copy` straight to `w`, so that the
    body streams. Call `Flusher::Flush` after each read when the
    `ResponseWriter` is a Flusher. git's sideband progress depends on this.
  - On a copy error, kill the process and `Wait`, as Go does.

- [ ] **Step 5: Run the tests and check that they pass, with no leaks**

  Run: `jolt build "test:filter=GoCGI.*"`, then
  `jolt build "test:leaks=true,repeat=2,filter=GoCGI.*"`
  Expected: PASS in both.

- [ ] **Step 6: Commit**

  ```bash
  python3 modules/generate.py
  git add include/gopp/net/http/cgi.hpp src/net/http/cgi tests/go_net_http_cgi_test.cpp modules
  git commit -m "net/http/cgi: the host side of CGI, ported from Go"
  ```

---

### Task 2: `crypto`, `math/big` (minimal) and `crypto/elliptic`

**Files:**
- Create: `include/gopp/crypto.hpp` and `src/crypto/crypto.cpp`
- Create: `include/gopp/math/big.hpp` and `src/math/big/int.cpp`
- Create: `include/gopp/crypto/elliptic.hpp` and `src/crypto/elliptic.cpp`
- Create: `tests/go_crypto_pubkey_test.cpp`. Tasks 3 and 4 add their tests
  to this file.
- Create: `tests/go_math_big_test.cpp`

**Interfaces:**
- Produces in `gopp::crypto`:
  ```cpp
  enum class Hash : uint { MD5 = 2, SHA1 = 3, SHA224 = 4, SHA256 = 5, SHA384 = 6, SHA512 = 7 };  // Go's values
  ptr<hash::Hash> New(Hash h);          // Go: h.New()
  isize Size(Hash h);
  bool Available(Hash h);
  string String(Hash h);                // "SHA-256"
  struct PublicKey : virtual object { virtual bool Equal(const PublicKey& x) const = 0; };
  struct SignerOpts { virtual Hash HashFunc() const = 0; virtual ~SignerOpts() = default; };
  struct Signer : virtual object {
      virtual ptr<PublicKey> Public() const = 0;
      virtual std::pair<slice<byte>, error> Sign(io::Reader* rand, std::span<const byte> digest, const SignerOpts& opts) = 0;
  };
  struct PrivateKey : Signer {};        // Go: any; here every private key is a Signer
  ```
  Go types a type switch on a key. Here that is a `dynamic_cast` on
  `ptr<PublicKey>` and `ptr<PrivateKey>`.
- Produces in `gopp::big` (`<gopp/math/big.hpp>`). `Int` is a value type
  over an OpenSSL `BIGNUM*`, with deep copies and moves:
  ```cpp
  class Int {
  public:
      Int();  Int(const Int&);  Int& operator=(const Int&);  Int(Int&&) noexcept;  ~Int();
      Int& SetInt64(int64 x);  Int& SetBytes(std::span<const byte> buf);   // big-endian, unsigned
      std::pair<Int*, bool> SetString(std::string_view s, int base);
      slice<byte> Bytes() const;  slice<byte> FillBytes(std::span<byte> buf) const;
      int Cmp(const Int& y) const;  int Sign() const;  int BitLen() const;
      int64 Int64() const;  bool IsInt64() const;  string String() const;  string Text(int base) const;
      const void* bn() const;  // the BIGNUM, for go++'s crypto
  };
  ptr<Int> NewInt(int64 x);
  ```
- Produces in `gopp::elliptic`:
  ```cpp
  struct CurveParams { big::Int P, N, B, Gx, Gy; int BitSize; string Name; };
  struct Curve : virtual object { virtual const CurveParams& Params() const = 0; };
  ptr<Curve> P224(); ptr<Curve> P256(); ptr<Curve> P384(); ptr<Curve> P521();  // singletons
  ```
  The curves carry their parameters only. Point arithmetic is OpenSSL's,
  inside ecdsa, and Go has deprecated it anyway.

- [ ] **Step 1: Write the failing tests**

  `GoBig.*`:
  - Port `TestBytes`, `TestSetBytes`, `TestFillBytes`, `TestBitLen`,
    `TestCmp` and `TestInt64` from `/usr/lib/go-1.26/src/math/big/int_test.go`,
    keeping only the cases that use the methods above.
  - Port `TestSetString`, for bases 0, 2, 10 and 16.

  `GoCrypto.*`:
  - `HashSizes` checks each `Hash`'s `Size` against
    `New(h)->Size()`, and `String`.
  - `CurveParams` checks each curve's `Name` and `BitSize` against Go's
    values, and that P-256's `N` in hex is
    `ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551`.

- [ ] **Step 2: Run them and check that they fail**

  Run: `jolt build "test:filter=GoBig.*:GoCrypto.*"`
  Expected: compile errors for the missing headers.

- [ ] **Step 3: Implement the three headers and their sources**

  `big::Int` wraps `BN_*`. The curves are filled from
  `EC_GROUP_new_by_curve_name`/`EC_GROUP_get_curve` and kept in
  `sync::Once`, never in a static initializer, because a fiber must not
  park inside one.

- [ ] **Step 4: Run them and check that they pass**

  Run: `jolt build "test:filter=GoBig.*:GoCrypto.*"`
  Expected: PASS.

- [ ] **Step 5: Commit**

  `crypto, math/big, crypto/elliptic: hash identifiers, key interfaces, Int over BIGNUM, curve parameters`

---

### Task 3: `crypto/rsa`

**Files:**
- Create: `include/gopp/crypto/rsa.hpp`
- Create: `src/crypto/rsa.cpp`, which also holds a small internal
  `src/crypto/pkey.hpp` helper (`EVP_PKEY` ownership, building a key from
  parameters with `OSSL_PARAM_BLD`) that Task 4 reuses
- Modify: `tests/go_crypto_pubkey_test.cpp`

**Interfaces:**
- Consumes: `crypto::Hash`, `crypto::PublicKey`/`PrivateKey`/`SignerOpts`
  and `big::Int` (Task 2).
- Produces (namespace `gopp::rsa`):
  ```cpp
  struct PublicKey final : crypto::PublicKey { big::Int N; int E = 0; int Size() const; bool Equal(const crypto::PublicKey&) const override; };
  struct PrivateKey final : crypto::PrivateKey {
      rsa::PublicKey PublicKey;
      ptr<crypto::PublicKey> Public() const override;
      std::pair<slice<byte>, error> Sign(io::Reader* rand, std::span<const byte> digest, const crypto::SignerOpts& opts) override;  // PSS if opts is a PSSOptions
      error Validate() const;
      // the EVP_PKEY, private
  };
  struct PSSOptions final : crypto::SignerOpts { int SaltLength = 0; crypto::Hash Hash{}; crypto::Hash HashFunc() const override; };
  inline constexpr int PSSSaltLengthAuto = 0, PSSSaltLengthEqualsHash = -1;
  std::pair<ptr<PrivateKey>, error> GenerateKey(io::Reader* random, int bits);
  std::pair<slice<byte>, error> SignPKCS1v15(io::Reader* random, const PrivateKey& priv, crypto::Hash hash, std::span<const byte> hashed);
  error VerifyPKCS1v15(const PublicKey& pub, crypto::Hash hash, std::span<const byte> hashed, std::span<const byte> sig);
  std::pair<slice<byte>, error> SignPSS(io::Reader* rand, const PrivateKey& priv, crypto::Hash hash, std::span<const byte> digest, const PSSOptions* opts);
  error VerifyPSS(const PublicKey& pub, crypto::Hash hash, std::span<const byte> digest, std::span<const byte> sig, const PSSOptions* opts);
  extern const error ErrVerification;   // "crypto/rsa: verification error"
  ```
  A `crypto::SignerOpts` that is just a hash is `crypto::HashOpts{h}`. Add
  `struct HashOpts final : SignerOpts { Hash h; … }` to `crypto.hpp` in
  this task.

- [ ] **Step 1: Write the failing tests**, named `GoRSA.*`:
  - From `pkcs1v15_test.go`:
    - `SignPKCS1v15` uses `signPKCS1v15Tests` with Go's 512-bit test key
      `rsaPrivateKey`. Build it from Go's N, E, D, P and Q. That needs a
      test-only `rsa::detail::fromComponents`, declared in the internal
      header.
    - `VerifyPKCS1v15` uses the same vectors.
    - `UnpaddedSignature`
    - `ShortSessionKey` is left out, because it is about decryption.
  - From `pss_test.go`: `PSSSigning`, and `PSSNilOpts`.
  - `GoRSA.KeyGeneration`, from `rsa_test.go`, generates 2048 bits, then
    checks `Validate()`, that a signature round-trips, and that it fails
    after one bit of the signature is flipped.
  - The test file's header comment says why encryption tests are left
    out: encryption is not ported.

- [ ] **Step 2: Run them and check that they fail**

  Run: `jolt build "test:filter=GoRSA.*"`
  Expected: compile errors.

- [ ] **Step 3: Implement `rsa.cpp`**

  `PublicKey` builds its `EVP_PKEY` on demand from N and E and caches it,
  behind a `sync::Once` in a member. Signing and verifying use
  `EVP_PKEY_sign`/`EVP_PKEY_verify` over the given digest, with
  `RSA_PKCS1_PADDING`/`RSA_PKCS1_PSS_PADDING` and the signature digest
  set. Salt length `Auto` maps to `RSA_PSS_SALTLEN_AUTO` when verifying
  and to `RSA_PSS_SALTLEN_MAX` when signing, which is what Go does.

- [ ] **Step 4: Run them and check that they pass**, with the leak check.

  Run: `jolt build "test:filter=GoRSA.*"` and `jolt build "test:leaks=true,repeat=2,filter=GoRSA.*"`
  Expected: PASS.

- [ ] **Step 5: Commit**

  `crypto/rsa: PKCS #1 v1.5 and PSS signatures on OpenSSL`

---

### Task 4: `crypto/ecdsa` and `crypto/ed25519`

**Files:**
- Create: `include/gopp/crypto/ecdsa.hpp` and `src/crypto/ecdsa.cpp`
- Create: `include/gopp/crypto/ed25519.hpp` and `src/crypto/ed25519.cpp`
- Modify: `tests/go_crypto_pubkey_test.cpp`

**Interfaces:**
- Consumes: Task 2's interfaces and Task 3's `src/crypto/pkey.hpp`.
- Produces (`gopp::ecdsa`):
  ```cpp
  struct PublicKey final : crypto::PublicKey { ptr<elliptic::Curve> Curve; big::Int X, Y; bool Equal(const crypto::PublicKey&) const override; };
  struct PrivateKey final : crypto::PrivateKey { ecdsa::PublicKey PublicKey; big::Int D; /* Public, Sign (ASN.1) */ };
  std::pair<ptr<PrivateKey>, error> GenerateKey(ptr<elliptic::Curve> c, io::Reader* rand);
  std::tuple<big::Int, big::Int, error> Sign(io::Reader* rand, const PrivateKey& priv, std::span<const byte> hash);
  bool Verify(const PublicKey& pub, std::span<const byte> hash, const big::Int& r, const big::Int& s);
  std::pair<slice<byte>, error> SignASN1(io::Reader* rand, const PrivateKey& priv, std::span<const byte> hash);
  bool VerifyASN1(const PublicKey& pub, std::span<const byte> hash, std::span<const byte> sig);
  ```
- Produces (`gopp::ed25519`):
  ```cpp
  inline constexpr isize PublicKeySize = 32, PrivateKeySize = 64, SignatureSize = 64, SeedSize = 32;
  struct PublicKey final : crypto::PublicKey { slice<byte> key; bool Equal(const crypto::PublicKey&) const override; };   // Go: []byte
  struct PrivateKey final : crypto::PrivateKey { slice<byte> key; slice<byte> Seed() const; /* Public, Sign with Hash 0 */ };
  std::tuple<ptr<PublicKey>, ptr<PrivateKey>, error> GenerateKey(io::Reader* rand);
  ptr<PrivateKey> NewKeyFromSeed(std::span<const byte> seed);
  slice<byte> Sign(const PrivateKey& priv, std::span<const byte> message);
  bool Verify(const PublicKey& pub, std::span<const byte> message, std::span<const byte> sig);
  ```

- [ ] **Step 1: Write the failing tests**
  - `GoECDSA.*`, from `ecdsa_test.go`:
    - `SignAndVerify` and `SignAndVerifyASN1`, for P-256, P-384 and P-521
    - `NonceSafety` and `INDCCA`, which check that two signatures of the
      same hash differ
    - `ZeroHashSignature`
    - `RandomPoint` is left out (deprecated `elliptic` arithmetic). So is
      `TestVectors`, whose testdata is gzipped SigVer.rsp. Instead, add
      `GoECDSA.RFC6979Vector`, which verifies the RFC 6979 A.2.5 P-256 /
      SHA-256 signature of "sample" with its public key.
  - `GoEd25519.*`, from `ed25519_test.go`:
    - `SignVerify`
    - `CryptoSigner`
    - `Equal`
    - `MalleabilityRejected`, from `TestMalleability`
    - `Golden`, using the first 8 vectors of Go's
      `testdata/sign.input.gz`, written into the test as hex. Name them
      in the comment.

- [ ] **Step 2: Run them and check that they fail**

  Run: `jolt build "test:filter=GoECDSA.*:GoEd25519.*"`
  Expected: compile errors.

- [ ] **Step 3: Implement both packages**
  - ECDSA builds its keys from X/Y (and D) with `OSSL_PKEY_PARAM_PUB_KEY`
    as an uncompressed point. `Sign` converts OpenSSL's DER signature to
    r and s through `ECDSA_SIG`.
  - Ed25519 uses `EVP_PKEY_ED25519` raw keys
    (`EVP_PKEY_new_raw_public_key`/`private_key`) and one-shot
    `EVP_DigestSign`/`EVP_DigestVerify` with a NULL digest.

- [ ] **Step 4: Run them and check that they pass**, with the leak check.

- [ ] **Step 5: Commit**

  `crypto/ecdsa, crypto/ed25519: signatures on OpenSSL`

---

### Task 5: `x/jose`, JSON Web Keys

**Files:**
- Create: `x/jose/LICENSE`
- Create: `x/jose/include/gopp/jose.hpp` and `x/jose/src/jwk.cpp`
- Create: `x/jose/src/internal.hpp`, with the base64url and
  big-endian-integer helpers that go-jose's `encoding.go` has
- Modify: `build.jolt`:
  - add `class GoppJose(GoppProject, cxx.Library)` with `name =
    "gopp/x/jose"`, `requires = ["gopp"]`, laid out and published like
    `GoppSQLite3`
  - add it to `Tests.requires` and its `src` to `Tests.incpaths`
- Create: `tests/go_x_jose_jwk_test.cpp`

**Interfaces:**
- Consumes: `rsa::PublicKey`, `ecdsa::PublicKey`, `ed25519::PublicKey` and
  their private keys (Tasks 3–4), `big::Int`, and go++'s `json::Value` /
  `json::RawMessage`.
- Produces (`gopp::jose`):
  ```cpp
  enum class SignatureAlgorithm;  // as strings: inline constexpr std::string_view RS256 = "RS256", RS384, RS512, PS256, PS384, PS512, ES256, ES384, ES512, EdDSA, HS256, HS384, HS512
  struct JSONWebKey {
      ptr<object> Key;               // a ptr<rsa::PublicKey>, ecdsa::PublicKey, ed25519::PublicKey or one of the private keys
      string KeyID, Algorithm, Use;
      std::pair<slice<byte>, error> MarshalJSON() const;
      error UnmarshalJSON(std::span<const byte> data);   // Go's rules: rejects bad sizes and unknown kty
      bool IsPublic() const;  JSONWebKey Public() const;  bool Valid() const;
      std::pair<slice<byte>, error> Thumbprint(crypto::Hash h) const;
  };
  struct JSONWebKeySet { slice<JSONWebKey> Keys; slice<JSONWebKey> Key(std::string_view kid) const; };
  ```
  Symmetric (`oct`) keys and X.509 (`x5c`, `x5t`, `x5u`) are not ported.
  Unmarshaling a JWK that uses them returns go-jose's errors for
  unsupported keys. A set skips such keys, as go-oidc's `jwkJSON` does.

- [ ] **Step 1: Port the tests** from `jwk_test.go`:
  - RoundtripEcPublic, RoundtripEcPrivate, RoundtripRsaPrivate
  - MarshalUnmarshal, MarshalUnmarshalInvalid
  - WebKeyVectorsInvalid, WebKeyVectorsValid
  - Ed25519Serialization, Ed25519RejectsMalformedPublicJWK
  - Thumbprint
  - MarshalUnmarshalJWKSet, JWKSetKey
  - JWKIsPublic, JWKValid, JWKBufferSizeCheck
  - JWKPaddingX, JWKPaddingY, JWKPaddingPrivateX, JWKPaddingPrivateY,
    JWKPaddingPrivateD
  - JWKUnsupported

  Leave out the X.509 and symmetric tests, and say so in the header
  comment.

- [ ] **Step 2: Run them and check that they fail**

  Run: `jolt build "test:filter=GoJWK.*"`
  Expected: compile errors.

- [ ] **Step 3: Implement `jwk.cpp`**

  Port `jwk.go`'s `rawJSONWebKey` and its `ecPublicKey`/`rsaPublicKey`/
  `edPublicKey` and private conversions, and `Thumbprint`.

- [ ] **Step 4: Run them and check that they pass**, with the leak check.

- [ ] **Step 5: Commit**

  `x/jose: JSON Web Keys, ported from go-jose v4.1.7`

---

### Task 6: `x/jose`, JWS and JWT

**Files:**
- Create: `x/jose/src/jws.cpp` and `x/jose/src/signing.cpp`
- Create: `x/jose/include/gopp/jose/jwt.hpp` and `x/jose/src/jwt.cpp`
- Create: `tests/go_x_jose_jws_test.cpp` and `tests/go_x_jose_jwt_test.cpp`

**Interfaces:**
- Consumes: Task 5.
- Produces (`gopp::jose`):
  ```cpp
  struct Header { string KeyID; ptr<JSONWebKey> JSONWebKey; string Algorithm, Nonce; std::map<string, json::Value> ExtraHeaders; };
  struct Signature { Header Protected, Unprotected, Header; slice<byte> signature; };
  class JSONWebSignature {
  public:
      slice<Signature> Signatures;
      std::pair<slice<byte>, error> Verify(const object& verificationKey) const;   // a public key, a JSONWebKey or a JSONWebKeySet
      std::pair<slice<byte>, error> Verify(const JSONWebKey& k) const;
      slice<byte> UnsafePayloadWithoutVerification() const;
      std::pair<string, error> CompactSerialize() const;
  };
  std::pair<ptr<JSONWebSignature>, error> ParseSigned(std::string_view s, std::span<const std::string_view> allowedAlgs);         // compact only
  std::pair<ptr<JSONWebSignature>, error> ParseSignedCompact(std::string_view s, std::span<const std::string_view> allowedAlgs);
  struct SigningKey { std::string_view Algorithm; ptr<object> Key; };    // a private key or a JSONWebKey holding one
  struct SignerOptions { std::map<string, json::Value> ExtraHeaders; bool EmbedJWK = false; SignerOptions& WithType(std::string_view t); SignerOptions& WithHeader(std::string_view k, json::Value v); };
  struct Signer : virtual object { virtual std::pair<ptr<JSONWebSignature>, error> Sign(std::span<const byte> payload) = 0; };
  std::pair<ptr<Signer>, error> NewSigner(const SigningKey& sig, const SignerOptions* opts);
  extern const error ErrCryptoFailure, ErrUnsupportedAlgorithm, ErrUnsupportedKeyType, ErrNotSupported, ErrUnprotectedNonce;
  ```
- Produces (`gopp::jose::jwt`):
  ```cpp
  struct NumericDate { int64 seconds; time::Time Time() const; /* JSON: accepts integers and floats, rejects out of range */ };
  ptr<NumericDate> NewNumericDate(time::Time t);
  struct Audience : slice<string> { bool Contains(std::string_view v) const; /* JSON: a string or a list */ };
  struct Claims { string Issuer, Subject; Audience Audience; ptr<NumericDate> Expiry, NotBefore, IssuedAt; string ID; };  // json tags iss, sub, aud, exp, nbf, iat, jti, all omitempty
  struct Expected { string Issuer, Subject, ID; jwt::Audience AnyAudience; time::Time Time; };
  inline constexpr time::Duration DefaultLeeway = time::Minute;
  error Claims::Validate(const Expected& e) const;   error Claims::ValidateWithLeeway(const Expected& e, time::Duration leeway) const;
  class JSONWebToken { public: slice<jose::Header> Headers;
      template <typename... T> error Claims(const object& key, T*... out) const;
      template <typename... T> error UnsafeClaimsWithoutVerification(T*... out) const; };
  std::pair<ptr<JSONWebToken>, error> ParseSigned(std::string_view s, std::span<const std::string_view> allowedAlgs);
  // Builder: Signed(signer).Claims(c)...Serialize()
  class Builder { public: template <typename T> Builder Claims(const T& c) const; std::pair<string, error> Serialize() const; };
  Builder Signed(ptr<jose::Signer> sig);
  extern const error ErrInvalidIssuer, ErrInvalidSubject, ErrInvalidAudience, ErrInvalidID, ErrNotValidYet, ErrExpired, ErrIssuedInTheFuture, ErrInvalidClaims;
  ```
  `ParseSigned` checks the algorithm in the protected header against
  `allowedAlgs` before anything else, as go-jose 4 does. Only the compact
  serialization is parsed. A JSON serialization returns `ErrNotSupported`,
  which is a documented narrowing, because OIDC tokens are always compact.
  HMAC algorithms are rejected with `ErrUnsupportedAlgorithm`, because
  there are no symmetric keys.

- [ ] **Step 1: Port the tests**
  - From `jws_test.go`:
    - ParseEmptySigned, CompactParseJWS
    - RejectUnprotectedJWSNonce
    - HeaderFieldsCompact
    - ErrorMissingPayloadJWS
    - ErrorUnexpectedSignatureAlgorithmInProtected,
      UnexpectedAlgorithmCheckedBeforeHeaders
    - NullHeaderValue, RejectNullProtectedHeader
    - EmbeddedMalformedEd25519JWK
    - SampleNimbusJWSMessagesRSA and SampleNimbusJWSMessagesEC, compact
      messages only
  - From `signing_test.go`:
    - RoundtripsJWS, RoundtripsJWSCorruptSignature
    - JWSInvalidKey, JWSNilVerificationKey
    - InvalidSignerAlg, InvalidJWS
    - SignerKid, EmbedJwk
    - SignerOptionsEd, SignerOptions, SignerExtraHeaderInclusion
  - From `asymmetric_test.go`: Ed25519, InvalidAlgorithmsRSA,
    InvalidAlgorithmsEC, InvalidECPublicKey, InvalidAlgorithmEC.
  - From `jwt/`:
    - FieldsMatch, ExpiryAndNotBefore, IssuedInFuture,
      OptionalDateClaims
    - EncodeClaims, EncodeClaimsWithSingleAudience, DecodeClaims
    - NumericDate, NumericDateOutOfRange, OutOfRangeNbfNotBeforeCheck,
      EncodeClaimsTimeValues, IntegerAndFloatsNormalize
    - BuilderCustomClaimsNonPointer, BuilderCustomClaimsPointer,
      BuilderMergeClaims, BuilderHeadersSigner
    - GetClaimsWithoutVerification
    - DecodeTokenWithJWKS, DecodeTokenWithMismatchedJWKSKID, DecodeToken
    - TamperedJWT
  - Add `GoJWS.RejectsAlgNone`. Parsing a token whose header is
    `{"alg":"none"}`, with every algorithm allowed, returns an error.
  - Add `GoJWS.RejectsHS256WithRSAKey`. An HS256 token "signed" with the
    RSA public key's bytes as the HMAC key fails `Verify` against that
    `rsa::PublicKey`.
  - Leave out the JSON serialization, detached payload, b64 and
    encryption tests, and say so in the comments.

- [ ] **Step 2: Run them and check that they fail**

  Run: `jolt build "test:filter=GoJWS.*:GoJWT.*"`

- [ ] **Step 3: Implement**

  Port `jws.go` (compact parsing, `computeAuthData`), `signing.go`
  (`NewSigner`, `Verify`, choosing the key from a JWKS by `kid`),
  `asymmetric.go` (the RSA/ECDSA/EdDSA signers and verifiers, with ECDSA
  r‖s fixed-width encoding), and `jwt/{claims,validation,jwt,builder}.go`.

- [ ] **Step 4: Run them and check that they pass**, with the leak check.

- [ ] **Step 5: Commit**

  `x/jose: JWS and JWT, ported from go-jose v4.1.7`

---

### Task 7: `x/oauth2`

**Files:**
- Create: `x/oauth2/LICENSE`
- Create: `x/oauth2/include/gopp/oauth2.hpp`
- Create: `x/oauth2/src/{oauth2,token,transport,deviceauth,pkce,internal}.cpp`
- Modify: `build.jolt`, adding `gopp/x/oauth2` (requires `gopp`) and
  wiring it into `Tests`
- Create: `tests/go_x_oauth2_test.cpp`

**Interfaces:**
- Produces (`gopp::oauth2`):
  ```cpp
  enum class AuthStyle { AutoDetect = 0, InParams = 1, InHeader = 2 };
  struct Endpoint { string AuthURL, DeviceAuthURL, TokenURL; AuthStyle AuthStyle = AuthStyle::AutoDetect; };
  struct Token : object {
      string AccessToken, TokenType, RefreshToken; time::Time Expiry; int64 ExpiresIn = 0;
      bool Valid() const; string Type() const; void SetAuthHeader(http::Request& r) const;
      json::Value Extra(std::string_view key) const;      // e.g. Extra("id_token")
      ptr<Token> WithExtra(json::Value raw) const;
  };
  struct TokenSource : virtual object { virtual std::pair<ptr<Token>, error> Token() = 0; };
  struct AuthCodeOption { /* a key and value */ };
  AuthCodeOption SetAuthURLParam(std::string_view key, std::string_view value);
  extern const AuthCodeOption AccessTypeOnline, AccessTypeOffline, ApprovalForce;
  string GenerateVerifier();
  AuthCodeOption S256ChallengeOption(std::string_view verifier);
  AuthCodeOption VerifierOption(std::string_view verifier);
  string S256ChallengeFromVerifier(std::string_view verifier);
  struct DeviceAuthResponse { string DeviceCode, UserCode, VerificationURI, VerificationURIComplete; time::Time Expiry; int64 Interval = 0; };  // Go's JSON names
  struct Config {
      string ClientID, ClientSecret; oauth2::Endpoint Endpoint; string RedirectURL; slice<string> Scopes;
      string AuthCodeURL(std::string_view state, std::span<const AuthCodeOption> opts = {}) const;
      std::pair<ptr<Token>, error> Exchange(ptr<context::Context> ctx, std::string_view code, std::span<const AuthCodeOption> opts = {}) const;
      std::pair<ptr<Token>, error> PasswordCredentialsToken(ptr<context::Context> ctx, std::string_view username, std::string_view password) const;
      ptr<oauth2::TokenSource> TokenSource(ptr<context::Context> ctx, ptr<Token> t) const;   // refreshes, reusing a valid token
      ptr<http::Client> Client(ptr<context::Context> ctx, ptr<Token> t) const;
      std::pair<ptr<DeviceAuthResponse>, error> DeviceAuth(ptr<context::Context> ctx, std::span<const AuthCodeOption> opts = {}) const;
      std::pair<ptr<Token>, error> DeviceAccessToken(ptr<context::Context> ctx, const DeviceAuthResponse& da, std::span<const AuthCodeOption> opts = {}) const;  // polls at Interval, honors slow_down
  };
  ptr<TokenSource> StaticTokenSource(ptr<Token> t);
  ptr<TokenSource> ReuseTokenSource(ptr<Token> t, ptr<TokenSource> src);
  ptr<http::Client> NewClient(ptr<context::Context> ctx, ptr<TokenSource> src);
  struct Transport final : http::RoundTripper { ptr<TokenSource> Source; ptr<http::RoundTripper> Base; /* RoundTrip */ };
  struct RetrieveError : errorInterface { ptr<http::Response> Response; slice<byte> Body; string ErrorCode, ErrorDescription, ErrorURI; string Error() const override; };
  extern const void* const HTTPClient;   // context key; the value is a ptr to an object holding an http::Client
  ```
  `HTTPClient` is a context key, as in Go. Its value is a
  `ptr<oauth2::ContextClient>` (`struct ContextClient : object {
  ptr<http::Client> Client; }`), because go++'s context values are
  `ptr<object>`. Define `ContextClient` in the header.

- [ ] **Step 1: Port the tests**
  - From `oauth2_test.go`: every test listed by
    `grep '^func Test' oauth2_test.go`, which are AuthCodeURL,
    AuthCodeURL_CustomParam, AuthCodeURL_Optional, URLUnsafeClientConfig,
    ExchangeRequest and its _CustomParam, _JSONResponse, _JSONResponse_Expiry,
    _BadResponse, _BadResponseType and _NonBasicAuth variants,
    ExtraValueRetrieval, PasswordCredentialsTokenRequest,
    TokenRefreshRequest, FetchWithNoRefreshToken, TokenRetrieveError,
    TokenRetrieveError200, RefreshToken_RefreshTokenReplacement,
    RefreshToken_RefreshTokenPreservation and ConfigClientWithToken.
  - From `token_test.go`: TokenExtra, TokenExpiry and TokenTypeMethod.
  - From `deviceauth_test.go`: DeviceAuthResponseMarshalJson,
    DeviceAuthResponseUnmarshalJson and DeviceAuthTokenRetrieveError.
  - Add `GoOAuth2.DeviceAccessTokenPolls`. An `httptest` server answers
    `authorization_pending` twice, then `slow_down`, then a token. The test
    checks that the result is the token, and that the requests are spaced
    at least 1 s and then at least 6 s apart, with `Interval=1`.
  - Add `GoOAuth2.PKCERoundTrip`. `S256ChallengeFromVerifier(v)` equals
    the RFC 7636 appendix B value for the RFC's verifier.
  - Add `GoOAuth2.TransportSetsBearer`.

- [ ] **Step 2: Run them and check that they fail**

  Run: `jolt build "test:filter=GoOAuth2.*"`

- [ ] **Step 3: Implement**

  Port `oauth2.go`, `token.go`, `transport.go`, `deviceauth.go`,
  `pkce.go` and `internal/{token,oauth2,transport}.go`.
  - Leave out the `endpoints` and provider subpackages, `jws`, `jwt`,
    `google`, `clientcredentials` and `authhandler`.
  - The internal auth-style cache for `AutoDetect` is per `Config`. It
    sits behind `sync::Mutex`, as in Go.

- [ ] **Step 4: Run them and check that they pass**, with the leak check.

- [ ] **Step 5: Commit**

  `x/oauth2: the OAuth 2.0 client, device flow and PKCE, ported from golang.org/x/oauth2 v0.36.0`

---

### Task 8: `x/oidc` and `oidctest`

**Files:**
- Create: `x/oidc/LICENSE`
- Create: `x/oidc/include/gopp/oidc.hpp` and
  `x/oidc/include/gopp/oidc/oidctest.hpp`
- Create: `x/oidc/src/{oidc,verify,jwks,oidctest}.cpp`
- Modify: `build.jolt`, adding `gopp/x/oidc` (requires `gopp`,
  `gopp/x/jose` and `gopp/x/oauth2`) and wiring it into `Tests`
- Create: `tests/go_x_oidc_test.cpp`
- Modify: `x/README.md`, or create it if missing, listing the three
  libraries, their upstream versions and their licenses. Add one line
  about `x/` to the top-level `README.md`.

**Interfaces:**
- Consumes: `jose::ParseSigned`, `jose::JSONWebKey(Set)` and
  `jose::JSONWebSignature::Verify` (Tasks 5–6), and `oauth2::Endpoint`,
  `oauth2::TokenSource` and `oauth2::HTTPClient` (Task 7).
- Produces (`gopp::oidc`):
  ```cpp
  inline constexpr std::string_view ScopeOpenID = "openid", ScopeOfflineAccess = "offline_access";
  inline constexpr std::string_view RS256 = "RS256", RS384…, PS256…, ES256…, EdDSA = "EdDSA";
  ptr<context::Context> ClientContext(ptr<context::Context> ctx, ptr<http::Client> client);
  ptr<context::Context> InsecureIssuerURLContext(ptr<context::Context> ctx, std::string_view issuerURL);
  struct KeySet : virtual object { virtual std::pair<slice<byte>, error> VerifySignature(ptr<context::Context> ctx, std::string_view jwt) = 0; };
  struct StaticKeySet final : KeySet { slice<ptr<crypto::PublicKey>> PublicKeys; /* VerifySignature */ };
  class RemoteKeySet final : public KeySet { /* VerifySignature: cached keys; on an unknown kid refetch once, coalescing concurrent refetches (Go's inflight) */ };
  ptr<RemoteKeySet> NewRemoteKeySet(ptr<context::Context> ctx, std::string_view jwksURL);
  struct Config { string ClientID; slice<string> SupportedSigningAlgs; bool SkipClientIDCheck = false, SkipExpiryCheck = false, SkipIssuerCheck = false; std::function<time::Time()> Now; bool InsecureSkipSignatureCheck = false; };
  class IDTokenVerifier : public object { public: std::pair<ptr<IDToken>, error> Verify(ptr<context::Context> ctx, std::string_view rawIDToken) const; };
  ptr<IDTokenVerifier> NewVerifier(std::string_view issuerURL, ptr<KeySet> keySet, const Config& config);
  struct ProviderConfig { string IssuerURL, AuthURL, TokenURL, DeviceAuthURL, UserInfoURL, JWKSURL; slice<string> Algorithms; ptr<Provider> NewProvider(ptr<context::Context> ctx) const; };   // JSON names as in Go
  class Provider : public object { public:
      oauth2::Endpoint Endpoint() const; string UserInfoEndpoint() const;
      ptr<IDTokenVerifier> Verifier(const Config& config) const; ptr<IDTokenVerifier> VerifierContext(ptr<context::Context> ctx, const Config& config) const;
      template <typename T> error Claims(T* v) const;   // the discovery document
      std::pair<ptr<UserInfo>, error> UserInfo(ptr<context::Context> ctx, ptr<oauth2::TokenSource> ts) const; };
  std::pair<ptr<Provider>, error> NewProvider(ptr<context::Context> ctx, std::string_view issuer);
  struct IDToken : object { string Issuer; slice<string> Audience; string Subject; time::Time Expiry, IssuedAt; string Nonce, AccessTokenHash;
      template <typename T> error Claims(T* v) const;  error Claims(json::Value* v) const;  error VerifyAccessToken(std::string_view accessToken) const; };
  struct UserInfo : object { string Subject, Profile, Email; bool EmailVerified = false; template <typename T> error Claims(T* v) const; };
  oauth2::AuthCodeOption Nonce(std::string_view nonce);
  struct TokenExpiredError : errorInterface { time::Time Expiry; string Error() const override; };
  struct IssuerMismatchError : errorInterface { string Expected, Actual; string Error() const override; };
  ```
- Produces (`gopp::oidctest`):
  ```cpp
  struct PublicKey { ptr<crypto::PublicKey> PublicKey; string KeyID, Algorithm; };
  class Server final : public http::Handler { public: slice<oidctest::PublicKey> PublicKeys; std::map<string, string> Algorithms;
      void SetIssuer(std::string_view issuerURL); void ServeHTTP(http::ResponseWriter& w, http::Request& r) override; };
  string SignIDToken(const crypto::PrivateKey& priv, std::string_view keyID, std::string_view alg, std::string_view claims);
  ```
  git++ will extend this server in its own tests with a token and device
  endpoint. The class only has to serve discovery and keys, as in Go.

- [ ] **Step 1: Port the tests**
  - From `verify_test.go`:
    - Verify, VerifyAudience, VerifySigningAlg
    - AccessTokenHash
    - DistClaimResolver is left out: distributed claims are not ported,
      and the header comment says so.
  - From `jwks_test.go`:
    - RSAVerify, ECDSAVerify, EdDSAVerify
    - MultipleKeysVerify, MismatchedKeyID
    - KeyVerifyContextCanceled
    - Rotation
    - UpdateKeysSendsCacheControlNoCache
    - JWKParseWithIgnoredKeys, JWKParseWithUnsupportedKeys
  - From `oidc_test.go`:
    - AccessTokenVerification
    - NewProvider, IssuerMismatchError, ProviderConfigJSON
    - GetClient
    - UserInfoEndpoint
    - VerifierAlg
    - CanceledContext
  - From `oidctest_test.go`: Server.
  - Add `GoOIDC.UnknownKidDoesNotRefetchPerRequest`. The test verifies 50
    tokens with random unknown `kid`s, concurrently from 8 fibers, against
    a counting JWKS server. The server must see at most 2 key fetches.
    Port go-oidc's behavior, then add the minimum the test needs: after a
    refetch, don't refetch again for 10 s on unknown kids. Document that
    in the header and in the go++ spec as a go++ addition.

- [ ] **Step 2: Run them and check that they fail**

  Run: `jolt build "test:filter=GoOIDC.*:GoOIDCTest.*"`

- [ ] **Step 3: Implement**

  Port `oidc.go` (discovery, the issuer check, `doRequest` with the
  context client), `verify.go` (`Verify` in Go's order: parse, issuer,
  audience, expiry, signature algorithm, signature) and `jwks.go`
  (`RemoteKeySet`, with `inflight` built from a channel). Leave out
  `logout.go` and distributed claims.

- [ ] **Step 4: Run them and check that they pass**, then the whole suite
  with the leak check.

  Run: `jolt build "test:filter=GoOIDC.*:GoOIDCTest.*"`, then `jolt build test`, then `jolt build "test:leaks=true,repeat=2"`
  Expected: PASS everywhere.

- [ ] **Step 5: Commit**

  `x/oidc: OpenID Connect discovery and ID token verification, ported from coreos/go-oidc v3.21.0`

- [ ] **Step 6: Hand the branch over**

  Use superpowers:finishing-a-development-branch. The user decides whether
  and how `auth` reaches `master`. git++ (plan
  `/home/rbrt/git/git++/docs/superpowers/plans/2026-11-16-gitpp.md`)
  builds against `/home/rbrt/git/go++`, so these packages must be in that
  checkout before git++'s Task 2 starts.
