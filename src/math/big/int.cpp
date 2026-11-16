// math/big's Int over OpenSSL's BIGNUM.
#include <gopp/math/big.hpp>

#include <openssl/bn.h>

#include <algorithm>
#include <string>
#include <vector>

namespace gopp::big {

namespace {

static_assert(sizeof(BN_ULONG) == 8);

[[noreturn]] void bnFailed(const char* what) { panic(std::string("math/big: ") + what + " failed"); }

// What a moved-from Int is read as.
const BIGNUM* zero() {
    static const BIGNUM* const z = [] {
        BIGNUM* b = BN_new();
        if (!b) bnFailed("BN_new");
        return b;
    }();
    return z;
}

const char digits[] = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";

// The value of c as a digit in base, or -1.
int digitValue(char c, int base) {
    int d;
    if (c >= '0' && c <= '9') d = c - '0';
    else if (c >= 'a' && c <= 'z') d = c - 'a' + 10;
    else if (c >= 'A' && c <= 'Z') d = base <= 36 ? c - 'A' + 10 : c - 'A' + 36;
    else return -1;
    return d < base ? d : -1;
}

}  // namespace

Int::Int() : bn_(BN_new()) {
    if (!bn_) bnFailed("BN_new");
}

Int::Int(const Int& o) : object(o), bn_(BN_dup(static_cast<const BIGNUM*>(o.bn()))) {
    if (!bn_) bnFailed("BN_dup");
}

Int& Int::operator=(const Int& o) {
    if (this != &o && !BN_copy(mut(), static_cast<const BIGNUM*>(o.bn()))) bnFailed("BN_copy");
    return *this;
}

Int::~Int() { BN_free(bn_); }

bignum_st* Int::mut() {
    if (!bn_ && !(bn_ = BN_new())) bnFailed("BN_new");
    return bn_;
}

const void* Int::bn() const { return bn_ ? bn_ : zero(); }

Int& Int::SetInt64(int64 x) {
    uint64 u = x < 0 ? 0 - static_cast<uint64>(x) : static_cast<uint64>(x);
    BIGNUM* b = mut();
    if (!BN_set_word(b, u)) bnFailed("BN_set_word");
    BN_set_negative(b, x < 0);
    return *this;
}

Int& Int::SetBytes(std::span<const byte> buf) {
    if (!BN_bin2bn(buf.data(), static_cast<int>(buf.size()), mut())) bnFailed("BN_bin2bn");
    return *this;
}

std::pair<Int*, bool> Int::SetString(std::string_view s, int base) {
    if (base != 0 && (base < 2 || base > 62)) return {nullptr, false};
    bool neg = false;
    if (!s.empty() && (s[0] == '+' || s[0] == '-')) {
        neg = s[0] == '-';
        s.remove_prefix(1);
    }
    // Underscores are allowed only in base 0, and then only after a base
    // prefix or a digit, and before a digit.
    bool underscores = false, prefixed = false;
    if (base == 0) {
        underscores = true;
        base = 10;
        if (!s.empty() && s[0] == '0') {
            base = 8;  // the 0 is then the first digit
            if (s.size() >= 2) {
                switch (s[1]) {
                    case 'b': case 'B': base = 2; s.remove_prefix(2); prefixed = true; break;
                    case 'o': case 'O': base = 8; s.remove_prefix(2); prefixed = true; break;
                    case 'x': case 'X': base = 16; s.remove_prefix(2); prefixed = true; break;
                }
            }
        }
    }
    Int t;
    BIGNUM* b = t.mut();
    // What the last character was; an underscore may follow a digit or an
    // explicit base prefix.
    enum { start, prefix, digit, underscore } prev = start;
    if (prefixed) prev = prefix;
    int ndigits = 0;
    for (char c : s) {
        if (c == '_' && underscores) {
            if (prev != digit && prev != prefix) return {nullptr, false};
            prev = underscore;
            continue;
        }
        int d = digitValue(c, base);
        if (d < 0) return {nullptr, false};
        if (!BN_mul_word(b, static_cast<BN_ULONG>(base)) || !BN_add_word(b, static_cast<BN_ULONG>(d))) {
            bnFailed("BN_mul_word");
        }
        prev = digit;
        ndigits++;
    }
    if (ndigits == 0 || prev == underscore) return {nullptr, false};
    BN_set_negative(b, neg);
    std::swap(bn_, t.bn_);
    return {this, true};
}

slice<byte> Int::Bytes() const {
    const BIGNUM* b = static_cast<const BIGNUM*>(bn());
    auto s = slice<byte>::make(static_cast<size_t>(BN_num_bytes(b)));
    if (!s.empty()) BN_bn2bin(b, s.data());
    return s;
}

slice<byte> Int::FillBytes(std::span<byte> buf) const {
    const BIGNUM* b = static_cast<const BIGNUM*>(bn());
    if (static_cast<size_t>(BN_num_bytes(b)) > buf.size()) panic("math/big: buffer too small to fit value");
    if (!buf.empty() && BN_bn2binpad(b, buf.data(), static_cast<int>(buf.size())) < 0) bnFailed("BN_bn2binpad");
    return slice<byte>::of(buf);
}

int Int::Cmp(const Int& y) const {
    return BN_cmp(static_cast<const BIGNUM*>(bn()), static_cast<const BIGNUM*>(y.bn()));
}

int Int::Sign() const {
    const BIGNUM* b = static_cast<const BIGNUM*>(bn());
    return BN_is_zero(b) ? 0 : BN_is_negative(b) ? -1 : 1;
}

int Int::BitLen() const { return BN_num_bits(static_cast<const BIGNUM*>(bn())); }

bool Int::IsInt64() const {
    const BIGNUM* b = static_cast<const BIGNUM*>(bn());
    if (BN_num_bits(b) > 64) return false;
    uint64 w = BN_get_word(b);
    // As Go: the absolute value fits when its int64 is not negative, or
    // is -2^63 of a negative number.
    int64 v = static_cast<int64>(w);
    return v >= 0 || (BN_is_negative(b) && w == 0 - w);
}

int64 Int::Int64() const {
    const BIGNUM* b = static_cast<const BIGNUM*>(bn());
    unsigned char buf[8] = {};
    int n = BN_num_bytes(b);
    std::vector<unsigned char> all(static_cast<size_t>(n));
    if (n > 0) BN_bn2bin(b, all.data());
    for (int i = 0; i < 8 && i < n; i++) buf[7 - i] = all[static_cast<size_t>(n - 1 - i)];
    uint64 w = 0;
    for (unsigned char c : buf) w = w << 8 | c;
    return static_cast<int64>(BN_is_negative(b) ? 0 - w : w);
}

string Int::String() const { return Text(10); }

string Int::Text(int base) const {
    if (base < 2 || base > 62) panic("invalid base");
    const BIGNUM* b = static_cast<const BIGNUM*>(bn());
    if (BN_is_zero(b)) return "0";
    BIGNUM* t = BN_dup(b);
    if (!t) bnFailed("BN_dup");
    std::string out;
    while (!BN_is_zero(t)) {
        BN_ULONG r = BN_div_word(t, static_cast<BN_ULONG>(base));
        if (r == static_cast<BN_ULONG>(-1)) bnFailed("BN_div_word");
        out.push_back(digits[r]);
    }
    BN_free(t);
    if (BN_is_negative(b)) out.push_back('-');
    std::reverse(out.begin(), out.end());
    return out;
}

ptr<Int> NewInt(int64 x) {
    auto z = make_ptr<Int>();
    z->SetInt64(x);
    return z;
}

}  // namespace gopp::big
