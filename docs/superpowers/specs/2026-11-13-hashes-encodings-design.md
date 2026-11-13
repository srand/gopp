# hash, crypto hashes, hmac, rand, subtle, hex and base64

## Intent

Tokens, ETags, signed cookies, webhooks and content addressing all need
hashes, HMACs, random bytes and hex or base64 text. With go++ today, a
program has to call OpenSSL itself. This adds Go's packages for them.

## Goals

- `hash`: the `Hash` interface (an `io::Writer` with `Sum`, `Reset`, `Size`
  and `BlockSize`).
- `crypto/sha256` (`New`, `New224`, `Sum256`, `Sum224`), `crypto/sha512`
  (`New`, `New384`, `New512_224`, `New512_256` and their `Sum` functions),
  `crypto/sha1` and `crypto/md5`.
- `crypto/hmac`: `New` over any `hash::Hash` constructor, and `Equal`.
- `crypto/rand`: `Reader`, `Read` and `Text`.
- `crypto/subtle`: `ConstantTimeCompare`, `ConstantTimeSelect`,
  `ConstantTimeByteEq`, `ConstantTimeEq`, `ConstantTimeCopy`,
  `ConstantTimeLessOrEq` and `XORBytes`.
- `encoding/hex`: `Encode`, `Decode`, `EncodeToString`, `DecodeString`,
  `AppendEncode`, `AppendDecode`, `EncodedLen`, `DecodedLen`, `Dump`,
  `Dumper`, `NewEncoder`, `NewDecoder`, `InvalidByteError` and `ErrLength`.
- `encoding/base64`: `Encoding` (`NewEncoding`, `WithPadding`, `Strict`) and
  the four standard encodings. It has the same encode, decode and append
  functions as hex, plus `NewEncoder`, `NewDecoder` and `CorruptInputError`.
- Go's test vectors and tests, ported with their names.

## Non-goals

- `encoding.BinaryMarshaler` on hash state, the `Hash32` and `Hash64`
  interfaces, `crc32`, `fnv` and `crypto/sha3`.
- `rand.Int` and `rand.Prime`, which need `math/big`.

## Decided

- The digests run on OpenSSL's EVP, which is linked already for TLS. Each
  digest's `EVP_MD` is fetched once. `Sum` finalizes a copy of the context,
  so the running hash goes on, as in Go.
- HMAC is ported from Go's `hmac.go` rather than taken from OpenSSL's
  `HMAC`, so that it works over any `hash::Hash`.
- `crypto/rand` reads with `getrandom(2)`. As in Go 1.24, `Read` never
  returns an error: it fails fatally if the kernel can't supply randomness.
- `hex` and `base64` are ported from Go's sources.
  `base64::StdEncoding` and the others are `constexpr` objects. They are
  ready before any static initializer runs, and Go's
  `base64.StdEncoding.EncodeToString(b)` stays the same expression.
- The fixed-size results are `std::array<byte, N>`, as Go's `[N]byte`.
- The headers follow Go's paths: `<gopp/hash.hpp>`,
  `<gopp/crypto/sha256.hpp>` (namespace `gopp::sha256`) and so on, then
  `<gopp/encoding/hex.hpp>` and `<gopp/encoding/base64.hpp>`.
