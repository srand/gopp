// Transport, ported from golang.org/x/oauth2 v0.36.0's transport.go.
#include "oauth2_internal.hpp"

namespace gopp::oauth2 {

std::pair<ptr<http::Response>, error> Transport::RoundTrip(http::Request& req) {
    bool reqBodyClosed = false;
    struct closeBody {
        http::Request& req;
        bool& closed;
        ~closeBody() {
            if (!closed && req.Body) req.Body->Close();
        }
    } guard{req, reqBodyClosed};

    if (!Source) return {nullptr, errors::New("oauth2: Transport's Source is nil")};
    auto [token, err] = Source->Token();
    if (err) return {nullptr, err};
    if (!token) return {nullptr, errors::New("oauth2: token source returned no token")};

    auto req2 = make_ptr<http::Request>(req);  // req.Clone: the header is copied
    token->SetAuthHeader(*req2);

    // The base transport takes over closing the body.
    reqBodyClosed = true;
    if (Base) return Base->RoundTrip(*req2);
    return http::DefaultTransport()->RoundTrip(*req2);
}

}  // namespace gopp::oauth2
