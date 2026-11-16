// The crypto/elliptic package, the part that go++'s crypto needs: the
// parameters of the NIST curves.
//
//     auto c = elliptic::P256();
//     fmt::Println("{} has {} bits", c->Params().Name, c->Params().BitSize);
//
// Unlike Go's, the curves carry their parameters only. Go has deprecated
// their point arithmetic, and crypto/ecdsa uses OpenSSL's.
#pragma once

// The module interfaces (modules/) import these instead.
#ifndef GOPP_MODULE
#include <gopp/base.hpp>
#include <gopp/math/big.hpp>
#include <gopp/ptr.hpp>
#include <gopp/string.hpp>
#endif

namespace gopp::elliptic {

// elliptic.CurveParams: the parameters of a short Weierstrass curve
// y^2 = x^3 - 3x + b.
struct CurveParams {
    big::Int P;     // the order of the underlying field
    big::Int N;     // the order of the base point
    big::Int B;     // the constant of the curve equation
    big::Int Gx, Gy;  // (x, y) of the base point
    int BitSize = 0;  // the size of the underlying field
    string Name;      // the canonical name of the curve
};

// elliptic.Curve.
struct Curve : virtual object {
    // The parameters of the curve.
    virtual const CurveParams& Params() const = 0;
};

// elliptic.P224, P256, P384 and P521: the NIST curves, as shared
// singletons.
ptr<Curve> P224();
ptr<Curve> P256();
ptr<Curve> P384();
ptr<Curve> P521();

}  // namespace gopp::elliptic
