// The compress/flate package, after Go 1.27.3's compress/flate API, on
// zlib's raw inflate and deflate (window bits -15).
#include <gopp/bufio.hpp>
#include <gopp/compress/flate.hpp>
#include <gopp/fmt.hpp>

#include <cstring>
#include <vector>

#include <zlib.h>

namespace gopp::flate {

string CorruptInputError::Error() const { return fmt::Sprint("flate: corrupt input before offset {}", Offset); }

namespace {

const error errWriterClosed = errors::New("flate: closed writer");

// The decompressor: inflate fed from the buffer of a bufio::Reader, which
// it consumes only as far as the stream goes.
class decompressor final : public io::ReadCloser, public Resetter {
public:
    decompressor(ptr<io::Reader> r, std::span<const byte> dict) {
        std::memset(&strm_, 0, sizeof strm_);
        if (int ret = inflateInit2(&strm_, -MAX_WBITS); ret != Z_OK) panic("flate: inflateInit2 failed");
        setReader(std::move(r));
        setDict(dict);
    }

    ~decompressor() override { inflateEnd(&strm_); }

    std::pair<isize, error> Read(std::span<byte> p) override {
        if (pending_) {
            // The end of the stream, or an error, after the data returned
            // last.
            pending_ = false;
            return {0, err_};
        }
        if (err_) return {0, err_};
        if (p.empty()) return {0, nullptr};
        for (;;) {
            auto [peek, perr] = br_->Peek(1);
            if (peek.empty()) {
                err_ = (perr == io::EOF_ || !perr) ? io::ErrUnexpectedEOF : perr;
                return {0, err_};
            }
            std::tie(peek, perr) = br_->Peek(br_->Buffered());
            strm_.next_in = const_cast<Bytef*>(peek.data());
            strm_.avail_in = uInt(peek.size());
            uInt room = uInt(std::min<std::size_t>(p.size(), 1u << 30));
            strm_.next_out = p.data();
            strm_.avail_out = room;
            int ret = inflate(&strm_, Z_NO_FLUSH);
            std::size_t consumed = peek.size() - strm_.avail_in;
            br_->Discard(isize(consumed));
            roffset_ += int64(consumed);
            isize n = isize(room - strm_.avail_out);
            switch (ret) {
            case Z_OK:
            case Z_BUF_ERROR:  // needs more input
                if (n > 0) return {n, nullptr};
                continue;
            case Z_STREAM_END:
                err_ = io::EOF_;
                break;
            case Z_DATA_ERROR:
                err_ = make_ptr<CorruptInputError>(roffset_);
                break;
            case Z_NEED_DICT:
                err_ = make_ptr<CorruptInputError>(roffset_);
                break;
            default:
                err_ = make_ptr<InternalError>(string(strm_.msg ? strm_.msg : "inflate failed"));
                break;
            }
            if (n > 0) {
                pending_ = true;
                return {n, nullptr};
            }
            return {0, err_};
        }
    }

    error Close() override {
        if (err_ == io::ErrUnexpectedEOF) return err_;  // if the stream was cut short
        return nullptr;
    }

    error Reset(ptr<io::Reader> r, std::span<const byte> dict) override {
        inflateReset(&strm_);
        setReader(std::move(r));
        setDict(dict);
        err_ = nullptr;
        pending_ = false;
        roffset_ = 0;
        return nullptr;
    }

private:
    void setReader(ptr<io::Reader> r) {
        if (auto br = dynamic_pointer_cast<bufio::Reader>(r)) {
            br_ = std::move(br);
        } else {
            br_ = make_ptr<bufio::Reader>(std::move(r));
        }
    }

    void setDict(std::span<const byte> dict) {
        // A raw inflate takes its dictionary before it starts.
        if (!dict.empty()) inflateSetDictionary(&strm_, dict.data(), uInt(dict.size()));
    }

    z_stream strm_;
    ptr<bufio::Reader> br_;
    error err_;
    bool pending_ = false;  // err_ is to be returned on the next Read
    int64 roffset_ = 0;
};

}  // namespace

ptr<io::ReadCloser> NewReader(ptr<io::Reader> r) { return make_ptr<decompressor>(std::move(r), std::span<const byte>{}); }

ptr<io::ReadCloser> NewReaderDict(ptr<io::Reader> r, std::span<const byte> dict) {
    return make_ptr<decompressor>(std::move(r), dict);
}

// ---------------------------------------------------------------------------
// Writer

struct Writer::impl {
    z_stream strm;
    ptr<io::Writer> w;
    std::vector<byte> dict;
    error err;
    bool closed = false;
    byte out[32 * 1024];

    // Runs deflate with flush over the input, writing what comes out.
    error run(std::span<const byte> in, int flush) {
        strm.next_in = const_cast<Bytef*>(in.data());
        strm.avail_in = uInt(in.size());
        for (;;) {
            strm.next_out = out;
            strm.avail_out = sizeof out;
            int ret = deflate(&strm, flush);
            if (ret == Z_STREAM_ERROR) return make_ptr<InternalError>(string("deflate failed"));
            std::size_t n = sizeof out - strm.avail_out;
            if (n > 0) {
                if (auto [_, werr] = w->Write(std::span<const byte>(out, n)); werr) return werr;
            }
            // Done when deflate didn't fill the output: all the input is
            // taken, and the flush complete.
            if (strm.avail_out != 0 && strm.avail_in == 0) return nullptr;
            if (ret == Z_STREAM_END) return nullptr;
        }
    }

    void setDict() {
        if (!dict.empty()) deflateSetDictionary(&strm, dict.data(), uInt(dict.size()));
    }
};

Writer::Writer(ptr<io::Writer> w, int level, std::span<const byte> dict) : impl_(std::make_unique<impl>()) {
    std::memset(&impl_->strm, 0, sizeof impl_->strm);
    impl_->w = std::move(w);
    impl_->dict.assign(dict.begin(), dict.end());
    int zlevel = level, strategy = Z_DEFAULT_STRATEGY;
    if (level == HuffmanOnly) {
        zlevel = Z_DEFAULT_COMPRESSION;
        strategy = Z_HUFFMAN_ONLY;
    } else if (level == DefaultCompression) {
        zlevel = Z_DEFAULT_COMPRESSION;
    }
    if (deflateInit2(&impl_->strm, zlevel, Z_DEFLATED, -MAX_WBITS, 8, strategy) != Z_OK) {
        panic("flate: deflateInit2 failed");
    }
    impl_->setDict();
}

Writer::~Writer() { deflateEnd(&impl_->strm); }

std::pair<isize, error> Writer::Write(std::span<const byte> p) {
    if (impl_->err) return {0, impl_->err};
    if (impl_->closed) return {0, errWriterClosed};
    if (auto err = impl_->run(p, Z_NO_FLUSH)) {
        impl_->err = err;
        return {0, err};
    }
    return {isize(p.size()), nullptr};
}

error Writer::Flush() {
    if (impl_->err) return impl_->err;
    if (impl_->closed) return nullptr;
    impl_->err = impl_->run({}, Z_SYNC_FLUSH);
    return impl_->err;
}

error Writer::Close() {
    if (impl_->err) return impl_->err;
    if (impl_->closed) return nullptr;
    impl_->closed = true;
    impl_->err = impl_->run({}, Z_FINISH);
    return impl_->err;
}

void Writer::Reset(ptr<io::Writer> w) {
    deflateReset(&impl_->strm);
    impl_->setDict();
    impl_->w = std::move(w);
    impl_->err = nullptr;
    impl_->closed = false;
}

std::pair<ptr<Writer>, error> NewWriter(ptr<io::Writer> w, int level) { return NewWriterDict(std::move(w), level, {}); }

std::pair<ptr<Writer>, error> NewWriterDict(ptr<io::Writer> w, int level, std::span<const byte> dict) {
    if (level < HuffmanOnly || level > BestCompression) {
        return {nullptr, fmt::Errorf("flate: invalid compression level {}: want value in range [-2, 9]", level)};
    }
    return {make_ptr<Writer>(std::move(w), level, dict), nullptr};
}

}  // namespace gopp::flate
