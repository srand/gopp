// The compress/gzip and compress/zlib packages, ported from Go 1.27.3's
// compress/gzip/gunzip.go and gzip.go, and compress/zlib/reader.go and
// writer.go.
#include <gopp/compress/gzip.hpp>
#include <gopp/compress/zlib.hpp>
#include <gopp/fmt.hpp>
#include <gopp/hash/adler32.hpp>
#include <gopp/hash/crc32.hpp>
#include <gopp/unicode/utf8.hpp>

#include <cstring>

namespace gopp {

namespace {

uint16 le16(const byte* b) { return uint16(b[0] | b[1] << 8); }
uint32 le32(const byte* b) { return uint32(b[0]) | uint32(b[1]) << 8 | uint32(b[2]) << 16 | uint32(b[3]) << 24; }
void putLE32(byte* b, uint32 v) {
    b[0] = byte(v), b[1] = byte(v >> 8), b[2] = byte(v >> 16), b[3] = byte(v >> 24);
}
uint32 be32(const byte* b) { return uint32(b[0]) << 24 | uint32(b[1]) << 16 | uint32(b[2]) << 8 | uint32(b[3]); }
void putBE32(byte* b, uint32 v) {
    b[0] = byte(v >> 24), b[1] = byte(v >> 16), b[2] = byte(v >> 8), b[3] = byte(v);
}

error noEOF(error err) { return err == io::EOF_ ? io::ErrUnexpectedEOF : err; }

ptr<bufio::Reader> asBufio(ptr<io::Reader> r) {
    if (auto br = dynamic_pointer_cast<bufio::Reader>(r)) return br;
    return make_ptr<bufio::Reader>(std::move(r));
}

}  // namespace

namespace gzip {

namespace {

constexpr byte gzipID1 = 0x1f;
constexpr byte gzipID2 = 0x8b;
constexpr byte gzipDeflate = 8;
constexpr byte flagText = 1 << 0;
constexpr byte flagHdrCrc = 1 << 1;
constexpr byte flagExtra = 1 << 2;
constexpr byte flagName = 1 << 3;
constexpr byte flagComment = 1 << 4;

}  // namespace

const error ErrChecksum = errors::New("gzip: invalid checksum");
const error ErrHeader = errors::New("gzip: invalid header");

// ---------------------------------------------------------------------------
// Reader

Reader::~Reader() = default;

std::pair<ptr<Reader>, error> NewReader(ptr<io::Reader> r) {
    auto z = make_ptr<Reader>();
    if (auto err = z->Reset(std::move(r))) return {nullptr, err};
    return {z, nullptr};
}

error Reader::Reset(ptr<io::Reader> r) {
    static_cast<Header&>(*this) = Header{};
    seeker_ = dynamic_pointer_cast<bufio::Reader>(r) ? nullptr : dynamic_pointer_cast<io::Seeker>(r);
    r_ = asBufio(std::move(r));
    digest_ = size_ = 0;
    err_ = nullptr;
    multistream_ = true;
    std::tie(static_cast<Header&>(*this), err_) = readHeader();
    if (err_ == io::EOF_) giveBack();
    return err_;
}

// Seeks the source back over what the bufio::Reader read ahead of the end.
void Reader::giveBack() {
    if (!seeker_) return;
    if (auto n = isize(r_->Buffered()); n > 0) {
        seeker_->Seek(-n, io::SeekCurrent);
        r_->Discard(n);
    }
}

// readString reads a NUL-terminated string from the reader. The gzip
// specification says that the strings are ISO 8859-1 (Latin-1); they're
// returned as UTF-8.
std::pair<string, error> Reader::readString() {
    bool needConv = false;
    for (std::size_t i = 0;; i++) {
        if (i >= sizeof buf_) return {{}, ErrHeader};
        auto [c, err] = r_->ReadByte();
        if (err) return {{}, err};
        buf_[i] = c;
        if (c > 0x7f) needConv = true;
        if (c == 0) {
            // Digest covers the NUL terminator.
            digest_ = crc32::Update(digest_, crc32::IEEETable, std::span<const byte>(buf_, i + 1));
            // Strings are ISO 8859-1, Latin-1 (RFC 1952, section 2.3.1).
            if (needConv) {
                std::string s;
                for (std::size_t j = 0; j < i; j++) {
                    char tmp[4];
                    auto n = utf8::EncodeRune(std::span<byte>(reinterpret_cast<byte*>(tmp), 4), rune(buf_[j]));
                    s.append(tmp, std::size_t(n));
                }
                return {string(s), nullptr};
            }
            return {String(std::span<const byte>(buf_, i)), nullptr};
        }
    }
}

// readHeader reads the GZIP header according to section 2.3.1. This method
// does not set z.err.
std::pair<Header, error> Reader::readHeader() {
    Header hdr;
    if (auto [_, err] = io::ReadFull(*r_, std::span<byte>(buf_, 10)); err) {
        // RFC 1952, section 2.2, says the following:
        //	A gzip file consists of a series of "members" (compressed data
        //	sets).
        //
        // Other than this, the specification does not clarify whether a
        // "series" is defined as "one or more" or "zero or more". To err on
        // the side of caution, Go interprets this to mean "zero or more".
        // Thus, it is okay to return io.EOF here.
        return {hdr, err};
    }
    if (buf_[0] != gzipID1 || buf_[1] != gzipID2 || buf_[2] != gzipDeflate) return {hdr, ErrHeader};
    byte flg = buf_[3];
    if (int64 t = int64(le32(buf_ + 4)); t > 0) {
        // Section 2.3.1, the zero value for MTIME means that the modified
        // time is not set.
        hdr.ModTime = time::Unix(t, 0);
    }
    // z.buf[8] is XFL and is currently ignored.
    hdr.OS = buf_[9];
    digest_ = crc32::ChecksumIEEE(std::span<const byte>(buf_, 10));

    if (flg & flagExtra) {
        if (auto [_, err] = io::ReadFull(*r_, std::span<byte>(buf_, 2)); err) return {hdr, noEOF(err)};
        digest_ = crc32::Update(digest_, crc32::IEEETable, std::span<const byte>(buf_, 2));
        auto data = slice<byte>::make(le16(buf_));
        if (auto [_, err] = io::ReadFull(*r_, data); err) return {hdr, noEOF(err)};
        digest_ = crc32::Update(digest_, crc32::IEEETable, data);
        hdr.Extra = std::move(data);
    }

    if (flg & flagName) {
        auto [s, err] = readString();
        if (err) return {hdr, noEOF(err)};
        hdr.Name = std::move(s);
    }

    if (flg & flagComment) {
        auto [s, err] = readString();
        if (err) return {hdr, noEOF(err)};
        hdr.Comment = std::move(s);
    }

    if (flg & flagHdrCrc) {
        if (auto [_, err] = io::ReadFull(*r_, std::span<byte>(buf_, 2)); err) return {hdr, noEOF(err)};
        if (le16(buf_) != uint16(digest_)) return {hdr, ErrHeader};
    }

    digest_ = 0;
    if (!decompressor_) {
        decompressor_ = flate::NewReader(r_);
    } else {
        dynamic_pointer_cast<flate::Resetter>(decompressor_)->Reset(r_, {});
    }
    return {hdr, nullptr};
}

std::pair<isize, error> Reader::Read(std::span<byte> p) {
    if (err_) return {0, err_};
    isize n = 0;
    while (n == 0) {
        std::tie(n, err_) = decompressor_->Read(p);
        digest_ = crc32::Update(digest_, crc32::IEEETable, p.first(std::size_t(n)));
        size_ += uint32(n);
        if (err_ != io::EOF_) {
            // In the normal case we return here.
            return {n, err_};
        }

        // Finished file; check checksum and size.
        if (auto [_, err] = io::ReadFull(*r_, std::span<byte>(buf_, 8)); err) {
            err_ = noEOF(err);
            return {n, err_};
        }
        uint32 digest = le32(buf_);
        uint32 size = le32(buf_ + 4);
        if (digest != digest_ || size != size_) {
            err_ = ErrChecksum;
            return {n, err_};
        }
        digest_ = size_ = 0;

        // File is ok; check if there is another.
        if (!multistream_) {
            giveBack();
            return {n, io::EOF_};
        }
        err_ = nullptr;  // remove io.EOF

        auto [hdr, herr] = readHeader();
        err_ = herr;
        if (err_ == io::EOF_) giveBack();
        if (err_) return {n, err_};
        static_cast<Header&>(*this) = std::move(hdr);
    }
    return {n, nullptr};
}

error Reader::Close() { return decompressor_ ? decompressor_->Close() : nullptr; }

// ---------------------------------------------------------------------------
// Writer

ptr<Writer> NewWriter(ptr<io::Writer> w) { return NewWriterLevel(std::move(w), DefaultCompression).first; }

std::pair<ptr<Writer>, error> NewWriterLevel(ptr<io::Writer> w, int level) {
    if (level < HuffmanOnly || level > BestCompression) {
        return {nullptr, fmt::Errorf("gzip: invalid compression level: {}", level)};
    }
    return {make_ptr<Writer>(std::move(w), level), nullptr};
}

Writer::Writer(ptr<io::Writer> w, int level) : w_(std::move(w)), level_(level) {}

Writer::~Writer() = default;

void Writer::Reset(ptr<io::Writer> w) {
    if (compressor_) compressor_->Reset(w);
    static_cast<Header&>(*this) = Header{};
    w_ = std::move(w);
    wroteHeader_ = closed_ = false;
    digest_ = size_ = 0;
    err_ = nullptr;
}

// writeBytes writes a length-prefixed byte slice to z.w.
error Writer::writeBytes(std::span<const byte> b) {
    if (b.size() > 0xffff) return errors::New("gzip.Write: Extra data is too large");
    byte l[2] = {byte(b.size()), byte(b.size() >> 8)};
    if (auto [_, err] = w_->Write(l); err) return err;
    return w_->Write(b).second;
}

// writeString writes a UTF-8 string s in GZIP's format to z.w. GZIP (RFC
// 1952) specifies that strings are NUL-terminated ISO 8859-1 (Latin-1).
error Writer::writeString(const string& s) {
    // GZIP stores Latin-1 strings; error if non-Latin-1; convert if
    // non-ASCII.
    std::string b;
    for (auto [i, r] = std::pair<std::size_t, rune>{0, 0}; i < s.size();) {
        auto [rr, size] = utf8::DecodeRuneInString(s.view().substr(i));
        r = rr;
        if (r == 0 || r > 0xff) return errors::New("gzip.Write: non-Latin-1 header string");
        b.push_back(char(r));
        i += std::size_t(size);
    }
    b.push_back('\0');
    return io::WriteString(*w_, b).second;
}

std::pair<isize, error> Writer::Write(std::span<const byte> p) {
    if (err_) return {0, err_};
    // Write the GZIP header lazily.
    if (!wroteHeader_) {
        wroteHeader_ = true;
        std::memset(buf_, 0, sizeof buf_);
        buf_[0] = gzipID1;
        buf_[1] = gzipID2;
        buf_[2] = gzipDeflate;
        if (!Extra.isNil()) buf_[3] |= 0x04;
        if (!Name.empty()) buf_[3] |= 0x08;
        if (!Comment.empty()) buf_[3] |= 0x10;
        if (ModTime.After(time::Unix(0, 0))) {
            // Section 2.3.1, the zero value for MTIME means that the
            // modified time is not set.
            putLE32(buf_ + 4, uint32(ModTime.Unix()));
        }
        if (level_ == BestCompression) {
            buf_[8] = 2;
        } else if (level_ == BestSpeed) {
            buf_[8] = 4;
        }
        buf_[9] = OS;
        if (auto [_, err] = w_->Write(std::span<const byte>(buf_, 10)); err) {
            err_ = err;
            return {0, err_};
        }
        if (!Extra.isNil()) {
            if ((err_ = writeBytes(Extra))) return {0, err_};
        }
        if (!Name.empty()) {
            if ((err_ = writeString(Name))) return {0, err_};
        }
        if (!Comment.empty()) {
            if ((err_ = writeString(Comment))) return {0, err_};
        }
        if (!compressor_) compressor_ = flate::NewWriter(w_, level_).first;
    }
    size_ += uint32(p.size());
    digest_ = crc32::Update(digest_, crc32::IEEETable, p);
    auto [n, err] = compressor_->Write(p);
    err_ = err;
    return {n, err_};
}

error Writer::Flush() {
    if (err_) return err_;
    if (closed_) return nullptr;
    if (!wroteHeader_) {
        Write({});
        if (err_) return err_;
    }
    err_ = compressor_->Flush();
    return err_;
}

error Writer::Close() {
    if (err_) return err_;
    if (closed_) return nullptr;
    closed_ = true;
    if (!wroteHeader_) {
        Write({});
        if (err_) return err_;
    }
    if ((err_ = compressor_->Close())) return err_;
    putLE32(buf_, digest_);
    putLE32(buf_ + 4, size_);
    err_ = w_->Write(std::span<const byte>(buf_, 8)).second;
    return err_;
}

}  // namespace gzip

namespace zlib {

namespace {

constexpr byte zlibDeflate = 8;
constexpr byte zlibMaxWindow = 7;

class reader final : public io::ReadCloser, public Resetter {
public:
    std::pair<isize, error> Read(std::span<byte> p) override {
        if (err_) return {0, err_};
        auto [n, err] = decompressor_->Read(p);
        err_ = err;
        digest_->Write(p.first(std::size_t(n)));
        if (err_ != io::EOF_) {
            // In the normal case we return here.
            return {n, err_};
        }
        // Finished file; check checksum.
        if (auto [_, rerr] = io::ReadFull(*r_, std::span<byte>(scratch_, 4)); rerr) {
            err_ = noEOF(rerr);
            return {n, err_};
        }
        // ZLIB (RFC 1950) is big-endian, unlike GZIP (RFC 1952).
        if (be32(scratch_) != digest_->Sum32()) {
            err_ = ErrChecksum;
            return {n, err_};
        }
        // Seek a source the bufio::Reader read ahead of back to the end of
        // the stream, as gzip::Reader does.
        if (seeker_) {
            if (auto b = isize(r_->Buffered()); b > 0) {
                seeker_->Seek(-b, io::SeekCurrent);
                r_->Discard(b);
            }
        }
        return {n, io::EOF_};
    }

    // Calling Close does not close the wrapped io.Reader originally passed
    // to NewReader. In order for the ZLIB checksum to be verified, the
    // reader must be fully consumed until the io.EOF.
    error Close() override {
        if (err_ && err_ != io::EOF_) return err_;
        err_ = decompressor_->Close();
        return err_;
    }

    error Reset(ptr<io::Reader> r, std::span<const byte> dict) override {
        seeker_ = dynamic_pointer_cast<bufio::Reader>(r) ? nullptr : dynamic_pointer_cast<io::Seeker>(r);
        r_ = asBufio(std::move(r));
        err_ = nullptr;

        // Read the header (RFC 1950 section 2.2.).
        if (auto [_, err] = io::ReadFull(*r_, std::span<byte>(scratch_, 2)); err) {
            err_ = noEOF(err);
            return err_;
        }
        uint16 h = uint16(scratch_[0] << 8 | scratch_[1]);
        if ((scratch_[0] & 0x0f) != zlibDeflate || (scratch_[0] >> 4) > zlibMaxWindow || h % 31 != 0) {
            err_ = ErrHeader;
            return err_;
        }
        bool haveDict = scratch_[1] & 0x20;
        if (haveDict) {
            if (auto [_, err] = io::ReadFull(*r_, std::span<byte>(scratch_, 4)); err) {
                err_ = noEOF(err);
                return err_;
            }
            if (be32(scratch_) != adler32::Checksum(dict)) {
                err_ = ErrDictionary;
                return err_;
            }
        }

        if (!decompressor_) {
            decompressor_ = haveDict ? flate::NewReaderDict(r_, dict) : flate::NewReader(r_);
        } else {
            dynamic_pointer_cast<flate::Resetter>(decompressor_)->Reset(r_, haveDict ? dict : std::span<const byte>{});
        }
        digest_ = adler32::New();
        return nullptr;
    }

private:
    ptr<bufio::Reader> r_;
    ptr<io::Seeker> seeker_;
    ptr<io::ReadCloser> decompressor_;
    ptr<hash::Hash32> digest_;
    error err_;
    byte scratch_[4];
};

}  // namespace

const error ErrChecksum = errors::New("zlib: invalid checksum");
const error ErrDictionary = errors::New("zlib: invalid dictionary");
const error ErrHeader = errors::New("zlib: invalid header");

std::pair<ptr<io::ReadCloser>, error> NewReader(ptr<io::Reader> r) { return NewReaderDict(std::move(r), {}); }

std::pair<ptr<io::ReadCloser>, error> NewReaderDict(ptr<io::Reader> r, std::span<const byte> dict) {
    auto z = make_ptr<reader>();
    if (auto err = z->Reset(std::move(r), dict)) return {nullptr, err};
    return {z, nullptr};
}

ptr<Writer> NewWriter(ptr<io::Writer> w) { return NewWriterLevelDict(std::move(w), DefaultCompression, slice<byte>{}).first; }

std::pair<ptr<Writer>, error> NewWriterLevel(ptr<io::Writer> w, int level) {
    return NewWriterLevelDict(std::move(w), level, slice<byte>{});
}

std::pair<ptr<Writer>, error> NewWriterLevelDict(ptr<io::Writer> w, int level, slice<byte> dict) {
    if (level < HuffmanOnly || level > BestCompression) {
        return {nullptr, fmt::Errorf("zlib: invalid compression level: {}", level)};
    }
    return {make_ptr<Writer>(std::move(w), level, std::move(dict)), nullptr};
}

Writer::Writer(ptr<io::Writer> w, int level, slice<byte> dict) : w_(std::move(w)), level_(level), dict_(std::move(dict)) {}

Writer::~Writer() = default;

void Writer::Reset(ptr<io::Writer> w) {
    w_ = std::move(w);
    // w was created with a valid level and dictionary.
    if (compressor_) compressor_->Reset(w_);
    if (digest_) digest_->Reset();
    err_ = nullptr;
    std::memset(scratch_, 0, sizeof scratch_);
    wroteHeader_ = false;
}

// writeHeader writes the ZLIB header.
error Writer::writeHeader() {
    wroteHeader_ = true;
    // ZLIB has a two-byte header (as documented in RFC 1950). The first
    // four bits is the CINFO (compression info), which is 7 for the default
    // deflate window size. The next four bits is the CM (compression
    // method), which is 8 for deflate.
    scratch_[0] = 0x78;
    // The next two bits is the FLEVEL (compression level). The four values
    // are: 0=fastest, 1=fast, 2=default, 3=best. The next bit, FDICT, is set
    // if a dictionary is given. The final five FCHECK bits form a
    // mod-31 checksum.
    switch (level_) {
    case -2: case 0: case 1: scratch_[1] = 0 << 6; break;
    case 2: case 3: case 4: case 5: scratch_[1] = 1 << 6; break;
    case 6: case -1: scratch_[1] = 2 << 6; break;
    case 7: case 8: case 9: scratch_[1] = 3 << 6; break;
    default: panic("unreachable");
    }
    if (!dict_.isNil()) scratch_[1] |= 1 << 5;
    scratch_[1] += byte(31 - uint16(scratch_[0] << 8 | scratch_[1]) % 31);
    if (auto [_, err] = w_->Write(std::span<const byte>(scratch_, 2)); err) return err;
    if (!dict_.isNil()) {
        // The next four bytes are the Adler-32 checksum of the dictionary.
        putBE32(scratch_, adler32::Checksum(dict_));
        if (auto [_, err] = w_->Write(std::span<const byte>(scratch_, 4)); err) return err;
    }
    if (!compressor_) {
        // Initialize deflater unless the Writer is being reused after a
        // Reset call.
        auto [c, err] = flate::NewWriterDict(w_, level_, dict_);
        if (err) return err;
        compressor_ = std::move(c);
        digest_ = adler32::New();
    }
    return nullptr;
}

std::pair<isize, error> Writer::Write(std::span<const byte> p) {
    if (!wroteHeader_) err_ = writeHeader();
    if (err_) return {0, err_};
    if (p.empty()) return {0, nullptr};
    auto [n, err] = compressor_->Write(p);
    if (err) {
        err_ = err;
        return {n, err};
    }
    digest_->Write(p);
    return {n, nullptr};
}

error Writer::Flush() {
    if (!wroteHeader_) err_ = writeHeader();
    if (err_) return err_;
    err_ = compressor_->Flush();
    return err_;
}

error Writer::Close() {
    if (!wroteHeader_) err_ = writeHeader();
    if (err_) return err_;
    if ((err_ = compressor_->Close())) return err_;
    putBE32(scratch_, digest_->Sum32());
    err_ = w_->Write(std::span<const byte>(scratch_, 4)).second;
    return err_;
}

}  // namespace zlib

}  // namespace gopp
