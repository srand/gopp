// crypto/elliptic: the NIST curves' parameters, read from OpenSSL's groups.
#include <gopp/crypto/elliptic.hpp>
#include <gopp/sync.hpp>

#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/obj_mac.h>

#include <string>
#include <vector>

namespace gopp::elliptic {

namespace {

[[noreturn]] void ecFailed(const char* what) { panic(std::string("crypto/elliptic: ") + what + " failed"); }

big::Int toInt(const BIGNUM* b) {
    std::vector<unsigned char> buf(static_cast<size_t>(BN_num_bytes(b)));
    if (!buf.empty()) BN_bn2bin(b, buf.data());
    big::Int z;
    z.SetBytes(buf);
    return z;
}

CurveParams load(int nid, const char* name, int bitSize) {
    EC_GROUP* g = EC_GROUP_new_by_curve_name(nid);
    BN_CTX* ctx = BN_CTX_new();
    BIGNUM *p = BN_new(), *a = BN_new(), *b = BN_new(), *x = BN_new(), *y = BN_new();
    if (!g || !ctx || !p || !a || !b || !x || !y) ecFailed("EC_GROUP_new_by_curve_name");
    if (!EC_GROUP_get_curve(g, p, a, b, ctx)) ecFailed("EC_GROUP_get_curve");
    const EC_POINT* gen = EC_GROUP_get0_generator(g);
    if (!gen || !EC_POINT_get_affine_coordinates(g, gen, x, y, ctx)) ecFailed("EC_POINT_get_affine_coordinates");
    const BIGNUM* n = EC_GROUP_get0_order(g);
    if (!n) ecFailed("EC_GROUP_get0_order");
    CurveParams cp;
    cp.P = toInt(p);
    cp.N = toInt(n);
    cp.B = toInt(b);
    cp.Gx = toInt(x);
    cp.Gy = toInt(y);
    cp.BitSize = bitSize;
    cp.Name = name;
    BN_free(p);
    BN_free(a);
    BN_free(b);
    BN_free(x);
    BN_free(y);
    BN_CTX_free(ctx);
    EC_GROUP_free(g);
    return cp;
}

class nistCurve final : public Curve {
public:
    explicit nistCurve(CurveParams p) : params_(std::move(p)) {}
    const CurveParams& Params() const override { return params_; }

private:
    CurveParams params_;
};

// A curve made on first use, under a sync::Once and not by a static's
// initializer: a fiber must not park inside one. Never freed.
struct lazyCurve {
    sync::Once once;
    ptr<Curve> curve;
};

ptr<Curve> curveOnce(int nid, const char* name, int bitSize, lazyCurve* l) {
    l->once.Do([&] { l->curve = make_ptr<nistCurve>(load(nid, name, bitSize)); });
    return l->curve;
}

}  // namespace

ptr<Curve> P224() {
    static auto* l = new lazyCurve;
    return curveOnce(NID_secp224r1, "P-224", 224, l);
}

ptr<Curve> P256() {
    static auto* l = new lazyCurve;
    return curveOnce(NID_X9_62_prime256v1, "P-256", 256, l);
}

ptr<Curve> P384() {
    static auto* l = new lazyCurve;
    return curveOnce(NID_secp384r1, "P-384", 384, l);
}

ptr<Curve> P521() {
    static auto* l = new lazyCurve;
    return curveOnce(NID_secp521r1, "P-521", 521, l);
}

}  // namespace gopp::elliptic
