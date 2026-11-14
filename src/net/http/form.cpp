// Forms: Request.ParseForm, ParseMultipartForm, FormValue, FormFile and
// MultipartReader, and MaxBytesReader, after Go 1.27.3's net/http/request.go.
#include "internal.hpp"

#include <gopp/mime.hpp>
#include <gopp/net/http.hpp>

#include <limits>

namespace gopp::http {

const gopp::error ErrNotMultipart = errors::New("request Content-Type isn't multipart/form-data");
const gopp::error ErrMissingBoundary = errors::New("no multipart boundary param in Content-Type");
const gopp::error ErrMissingFile = errors::New("http: no such file");

namespace {

// The memory ParseMultipartForm gets from FormValue and FormFile.
constexpr int64 defaultMaxMemory = 32 << 20;  // 32 MB

class maxBytesReader final : public io::ReadCloser {
public:
    maxBytesReader(ResponseWriter* w, ptr<io::ReadCloser> r, int64 n) : w_(w), r_(std::move(r)), i_(n), n_(n) {}

    std::pair<isize, gopp::error> Read(std::span<byte> p) override {
        if (err_) return {0, err_};
        if (p.empty()) return {0, nullptr};
        // If they asked for a 32KB byte read but only 5 bytes are remaining,
        // no need to read 32KB. 6 bytes will answer the question of the
        // whether we hit the limit or go past it. 0 < len(p) < 2^63.
        if (int64(p.size()) - 1 > n_) p = p.first(std::size_t(n_ + 1));
        auto [n, err] = r_->Read(p);
        if (n <= n_) {
            n_ -= n;
            err_ = err;
            return {n, err};
        }
        n = isize(n_);
        n_ = 0;
        // The server code and client code both use maxBytesReader. This
        // "requestTooLarge" check is only used by the server code. To prevent
        // binaries which only using the HTTP Client code (such as cmd/go) from
        // also linking in the HTTP server, don't use a static type assertion
        // to the server "*response" type. Check this interface instead.
        if (auto* res = dynamic_cast<internal::requestTooLarger*>(w_)) res->requestTooLarge();
        err_ = make_ptr<MaxBytesError>(i_);
        return {n, err_};
    }

    gopp::error Close() override { return r_->Close(); }

private:
    ResponseWriter* w_;
    ptr<io::ReadCloser> r_;  // underlying reader
    int64 i_;                // max bytes initially, for MaxBytesError
    int64 n_;                // max bytes remaining
    gopp::error err_;        // sticky error
};

void copyValues(url::Values& dst, const url::Values& src) {
    for (const auto& [k, v] : src) dst.Add(k, v);
}

std::pair<url::Values, gopp::error> parsePostForm(Request& r) {
    if (!r.Body) return {{}, errors::New("missing form body")};
    string ct = r.Header.Get("Content-Type");
    // RFC 7231, section 3.1.1.5 - empty type MAY be treated as
    // application/octet-stream.
    if (ct.empty()) ct = string::fromStatic("application/octet-stream");
    auto [mt, _, err] = mime::ParseMediaType(ct);
    url::Values vs;
    if (mt == "application/x-www-form-urlencoded") {
        ptr<io::Reader> reader = r.Body;
        int64 maxFormSize = std::numeric_limits<int64>::max();
        if (!dynamic_pointer_cast<maxBytesReader>(r.Body)) {
            maxFormSize = int64(10 << 20);  // 10 MB is a lot of text.
            reader = io::LimitReader(r.Body, maxFormSize + 1);
        }
        auto [b, e] = io::ReadAll(*reader);
        if (e) {
            if (!err) err = e;
            return {std::move(vs), err};
        }
        if (int64(b.len()) > maxFormSize) return {{}, errors::New("http: POST too large")};
        auto [v, perr] = url::ParseQuery(asStringView(b));
        vs = std::move(v);
        if (!err) err = perr;
    }
    // multipart/form-data is handled by ParseMultipartForm.
    return {std::move(vs), err};
}

}  // namespace

ptr<io::ReadCloser> MaxBytesReader(ResponseWriter* w, ptr<io::ReadCloser> r, int64 n) {
    if (n < 0) n = 0;  // treat negative limits as equivalent to 0
    return make_ptr<maxBytesReader>(w, std::move(r), n);
}

gopp::error Request::ParseForm() {
    gopp::error err;
    // Go's nil checks: a Form or PostForm set by the caller counts as
    // parsed.
    if (PostForm.size() > 0) postFormParsed_ = true;
    if (Form.size() > 0) formParsed_ = true;
    if (!postFormParsed_) {
        postFormParsed_ = true;
        if (Method == "POST" || Method == "PUT" || Method == "PATCH") std::tie(PostForm, err) = parsePostForm(*this);
    }
    if (!formParsed_) {
        formParsed_ = true;
        url::Values form;
        copyValues(form, PostForm);
        auto [newValues, e] = url::ParseQuery(URL.RawQuery);
        if (!err) err = e;
        copyValues(form, newValues);
        Form = std::move(form);
    }
    return err;
}

gopp::error Request::ParseMultipartForm(int64 maxMemory) {
    if (multipartByReader_) return errors::New("http: multipart handled by MultipartReader");
    gopp::error parseFormErr;
    if (!formParsed_ && Form.size() == 0) {
        // Let errors in ParseForm fall through, and just return it at the
        // end.
        parseFormErr = ParseForm();
    }
    if (MultipartForm) return nullptr;

    auto [mr, err] = multipartReader(false);
    if (err) return err;

    auto [f, ferr] = mr->ReadForm(maxMemory);
    if (ferr) return ferr;

    postFormParsed_ = true;
    for (const auto& [k, vs] : f->Value) {
        for (const auto& v : vs) {
            Form.Add(k, v);
            // r.PostForm should also be populated. See Go's issue 9305.
            PostForm.Add(k, v);
        }
    }
    MultipartForm = std::move(f);
    return parseFormErr;
}

string Request::FormValue(std::string_view key) {
    if (!formParsed_ && Form.size() == 0) ParseMultipartForm(defaultMaxMemory);
    return Form.Get(key);
}

string Request::PostFormValue(std::string_view key) {
    if (!postFormParsed_ && PostForm.size() == 0) ParseMultipartForm(defaultMaxMemory);
    return PostForm.Get(key);
}

std::tuple<ptr<multipart::File>, ptr<multipart::FileHeader>, gopp::error> Request::FormFile(std::string_view key) {
    if (multipartByReader_) return {nullptr, nullptr, errors::New("http: multipart handled by MultipartReader")};
    if (!MultipartForm) {
        if (auto err = ParseMultipartForm(defaultMaxMemory)) return {nullptr, nullptr, err};
    }
    if (MultipartForm) {
        if (auto it = MultipartForm->File.find(key); it != MultipartForm->File.end() && !it->second.empty()) {
            auto fh = it->second.front();
            auto [f, err] = fh->Open();
            return {std::move(f), std::move(fh), err};
        }
    }
    return {nullptr, nullptr, ErrMissingFile};
}

std::pair<ptr<multipart::Reader>, gopp::error> Request::MultipartReader() {
    if (multipartByReader_) return {nullptr, errors::New("http: MultipartReader called twice")};
    if (MultipartForm) return {nullptr, errors::New("http: multipart handled by ParseMultipartForm")};
    multipartByReader_ = true;
    return multipartReader(true);
}

std::pair<ptr<multipart::Reader>, gopp::error> Request::multipartReader(bool allowMixed) {
    string v = Header.Get("Content-Type");
    if (v.empty()) return {nullptr, ErrNotMultipart};
    if (!Body) return {nullptr, errors::New("missing form body")};
    auto [d, params, err] = mime::ParseMediaType(v);
    if (err || !(d == "multipart/form-data" || (allowMixed && d == "multipart/mixed"))) return {nullptr, ErrNotMultipart};
    auto it = params.find(string::fromStatic("boundary"));
    if (it == params.end()) return {nullptr, ErrMissingBoundary};
    return {multipart::NewReader(Body, it->second), nullptr};
}

}  // namespace gopp::http
