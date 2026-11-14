// Ported from golang.org/x/net/dns/dnsmessage/message.go, as vendored by
// Go 1.27.3.
#include "dnsmessage.hpp"

#include <algorithm>
#include <cstring>

namespace gopp::net::dnsmessage {

namespace {

constexpr int packStartingCap = 512;
constexpr int uint16Len = 2;
constexpr int uint32Len = 4;
constexpr int headerLen = 6 * uint16Len;
constexpr int nonEncodedNameMax = 254;

constexpr uint16 headerBitQR = 1 << 15;  // query/response (response=1)
constexpr uint16 headerBitAA = 1 << 10;  // authoritative
constexpr uint16 headerBitTC = 1 << 9;   // truncated
constexpr uint16 headerBitRD = 1 << 8;   // recursion desired
constexpr uint16 headerBitRA = 1 << 7;   // recursion available
constexpr uint16 headerBitAD = 1 << 5;   // authentic data
constexpr uint16 headerBitCD = 1 << 4;   // checking disabled

const error errBaseLen = errors::New("insufficient data for base length type");
const error errCalcLen = errors::New("insufficient data for calculated length type");
const error errReserved = errors::New("segment prefix is reserved");
const error errTooManyPtr = errors::New("too many pointers (>10)");
const error errInvalidPtr = errors::New("invalid pointer");
const error errInvalidName = errors::New("invalid dns name");
const error errNilResouceBody = errors::New("nil resource body");
const error errResourceLen = errors::New("insufficient data for resource body length");
const error errSegTooLong = errors::New("segment length too long");
const error errNameTooLong = errors::New("name too long");
const error errZeroSegLen = errors::New("zero length segment");
const error errResTooLong = errors::New("resource length too long");
const error errTooManyQuestions = errors::New("too many Questions to pack (>65535)");
const error errTooManyAnswers = errors::New("too many Answers to pack (>65535)");
const error errTooManyAuthorities = errors::New("too many Authorities to pack (>65535)");
const error errTooManyAdditionals = errors::New("too many Additionals to pack (>65535)");
const error errNonCanonicalName = errors::New("name is not in canonical format (it must end with a .)");
const error errStringTooLong = errors::New("character string exceeds maximum length (255)");

// Go's nestedError: an error with the context of where it happened.
struct nestedError final : errorInterface {
    string s;
    error err;
    nestedError(string s_, error e) : s(std::move(s_)), err(std::move(e)) {}
    string Error() const override { return s + ": " + err.Error(); }
};

error nested(std::string_view s, error err) { return make_ptr<nestedError>(string(s), std::move(err)); }

const char* sectionName(section sec) {
    switch (sec) {
    case sectionHeader: return "header";
    case sectionQuestions: return "Question";
    case sectionAnswers: return "Answer";
    case sectionAuthorities: return "Authority";
    case sectionAdditionals: return "Additional";
    default: return "";
    }
}

int len(const slice<byte>& b) { return int(b.size()); }

slice<byte> packUint16(slice<byte> msg, uint16 field) { return append(std::move(msg), byte(field >> 8), byte(field)); }

slice<byte> packUint32(slice<byte> msg, uint32 field) {
    return append(std::move(msg), byte(field >> 24), byte(field >> 16), byte(field >> 8), byte(field));
}

slice<byte> packBytes(slice<byte> msg, std::span<const byte> field) { return appendSpan(std::move(msg), field); }

std::tuple<uint16, int, error> unpackUint16(const slice<byte>& msg, int off) {
    if (off + uint16Len > len(msg)) return {0, off, errBaseLen};
    return {uint16(msg[std::size_t(off)] << 8 | msg[std::size_t(off + 1)]), off + uint16Len, nullptr};
}

std::pair<int, error> skipUint16(const slice<byte>& msg, int off) {
    if (off + uint16Len > len(msg)) return {off, errBaseLen};
    return {off + uint16Len, nullptr};
}

std::tuple<uint32, int, error> unpackUint32(const slice<byte>& msg, int off) {
    if (off + uint32Len > len(msg)) return {0, off, errBaseLen};
    const byte* p = msg.data() + off;
    return {uint32(p[0]) << 24 | uint32(p[1]) << 16 | uint32(p[2]) << 8 | uint32(p[3]), off + uint32Len, nullptr};
}

std::pair<int, error> skipUint32(const slice<byte>& msg, int off) {
    if (off + uint32Len > len(msg)) return {off, errBaseLen};
    return {off + uint32Len, nullptr};
}

error packText(slice<byte>& msg, std::string_view field) {
    if (field.size() > 255) return errStringTooLong;
    msg = append(std::move(msg), byte(field.size()));
    msg = appendSpan(std::move(msg), as_bytes(field));
    return nullptr;
}

std::tuple<std::string, int, error> unpackText(const slice<byte>& msg, int off) {
    if (off >= len(msg)) return {{}, off, errBaseLen};
    int beginOff = off + 1;
    int endOff = beginOff + int(msg[std::size_t(off)]);
    if (endOff > len(msg)) return {{}, off, errCalcLen};
    return {std::string(reinterpret_cast<const char*>(msg.data() + beginOff), std::size_t(endOff - beginOff)), endOff,
            nullptr};
}

std::pair<int, error> unpackBytes(const slice<byte>& msg, int off, std::span<byte> field) {
    int newOff = off + int(field.size());
    if (newOff > len(msg)) return {off, errBaseLen};
    std::memcpy(field.data(), msg.data() + off, field.size());
    return {newOff, nullptr};
}

// Name.pack: the labels of n, compressed through compression if it's
// not nil.
error packName(const Name& n, slice<byte>& msg, compressionMap* compression, int compressionOff) {
    if (n.Length > nonEncodedNameMax) return errNameTooLong;
    // Add a trailing dot to canonicalize name.
    if (n.Length == 0 || n.Data[n.Length - 1] != '.') return errNonCanonicalName;
    // Allow root domain.
    if (n.Data[0] == '.' && n.Length == 1) {
        msg = append(std::move(msg), byte(0));
        return nullptr;
    }
    slice<byte> out = msg;
    std::string_view nameAsStr(reinterpret_cast<const char*>(n.Data.data()), n.Length);
    // Emit sequence of counted strings, chopping at dots.
    for (int i = 0, begin = 0; i < int(n.Length); i++) {
        // Check for the end of the segment.
        if (n.Data[std::size_t(i)] == '.') {
            // The two most significant bits have special meaning. It isn't
            // allowed for segments to be long enough to need them.
            if (i - begin >= 1 << 6) return errSegTooLong;
            // Segments must have a non-zero length.
            if (i - begin == 0) return errZeroSegLen;
            out = append(std::move(out), byte(i - begin));
            for (int j = begin; j < i; j++) out = append(std::move(out), n.Data[std::size_t(j)]);
            begin = i + 1;
            continue;
        }
        // We can only compress domain suffixes starting with a new segment.
        // A pointer is two bytes with the two most significant bits set to
        // 1 to indicate that it is a pointer.
        if ((i == 0 || n.Data[std::size_t(i - 1)] == '.') && compression) {
            if (auto it = compression->find(nameAsStr.substr(std::size_t(i))); it != compression->end()) {
                // Hit. Emit a pointer instead of the rest of the domain.
                msg = append(std::move(out), byte(it->second >> 8 | 0xC0), byte(it->second));
                return nullptr;
            }
            // Miss. Add the suffix to the compression table if the offset
            // can be stored in the available 14 bits.
            int newPtr = len(out) - compressionOff;
            if (newPtr <= int(uint16(~0) >> 2)) compression->emplace(std::string(nameAsStr.substr(std::size_t(i))), uint16(newPtr));
        }
    }
    msg = append(std::move(out), byte(0));
    return nullptr;
}

std::pair<int, error> unpackName(Name& n, const slice<byte>& msg, int off) {
    // currOff is the current working offset; newOff is the offset where
    // the next record should start; ptr counts pointers followed.
    int currOff = off, newOff = off, ptr = 0;
    std::size_t nlen = 0;
    for (;;) {
        if (currOff >= len(msg)) return {off, errBaseLen};
        int c = int(msg[std::size_t(currOff)]);
        currOff++;
        switch (c & 0xC0) {
        case 0x00: {  // string segment
            if (c == 0x00) goto done;  // a zero length signals the end of the name
            int endOff = currOff + c;
            if (endOff > len(msg)) return {off, errCalcLen};
            // Reject names containing dots.
            for (int i = currOff; i < endOff; i++) {
                if (msg[std::size_t(i)] == '.') return {off, errInvalidName};
            }
            if (int(nlen) + (endOff - currOff) >= nonEncodedNameMax) return {off, errNameTooLong};
            std::memcpy(n.Data.data() + nlen, msg.data() + currOff, std::size_t(endOff - currOff));
            nlen += std::size_t(endOff - currOff);
            n.Data[nlen++] = '.';
            currOff = endOff;
            break;
        }
        case 0xC0: {  // pointer
            if (currOff >= len(msg)) return {off, errInvalidPtr};
            int c1 = msg[std::size_t(currOff)];
            currOff++;
            if (ptr == 0) newOff = currOff;
            // Don't follow too many pointers, maybe there's a loop.
            if (++ptr > 10) return {off, errTooManyPtr};
            currOff = (c ^ 0xC0) << 8 | c1;
            break;
        }
        default:  // prefixes 0x80 and 0x40 are reserved
            return {off, errReserved};
        }
    }
done:
    if (nlen == 0) n.Data[nlen++] = '.';
    n.Length = uint8(nlen);
    if (ptr == 0) newOff = currOff;
    return {newOff, nullptr};
}

std::pair<int, error> skipName(const slice<byte>& msg, int off) {
    int newOff = off;
    for (;;) {
        if (newOff >= len(msg)) return {off, errBaseLen};
        int c = int(msg[std::size_t(newOff)]);
        newOff++;
        switch (c & 0xC0) {
        case 0x00:
            if (c == 0x00) return {newOff, nullptr};
            newOff += c;
            if (newOff > len(msg)) return {off, errCalcLen};
            break;
        case 0xC0:
            // Pointer to somewhere else in msg. Pointers are two bytes.
            return {newOff + 1, nullptr};
        default:
            return {off, errReserved};
        }
    }
}

error packQuestion(const Question& q, slice<byte>& msg, compressionMap* compression, int compressionOff) {
    slice<byte> out = msg;
    if (error err = packName(q.Name, out, compression, compressionOff)) return nested("Name", err);
    out = packUint16(std::move(out), q.Type);
    msg = packUint16(std::move(out), q.Class);
    return nullptr;
}

std::pair<int, error> skipResourceRec(const slice<byte>& msg, int off) {
    auto [newOff, err] = skipName(msg, off);
    if (err) return {off, nested("Name", err)};
    std::tie(newOff, err) = skipUint16(msg, newOff);
    if (err) return {off, nested("Type", err)};
    std::tie(newOff, err) = skipUint16(msg, newOff);
    if (err) return {off, nested("Class", err)};
    std::tie(newOff, err) = skipUint32(msg, newOff);
    if (err) return {off, nested("TTL", err)};
    auto [length, o, lerr] = unpackUint16(msg, newOff);
    if (lerr) return {off, nested("Length", lerr)};
    if (o + int(length) > len(msg)) return {off, errResourceLen};
    return {o + int(length), nullptr};
}

std::pair<CNAMEResource, error> unpackCNAMEResource(const slice<byte>& msg, int off) {
    Name cname;
    if (auto [o, err] = unpackName(cname, msg, off); err) return {{}, err};
    return {CNAMEResource(cname), nullptr};
}

std::pair<MXResource, error> unpackMXResource(const slice<byte>& msg, int off) {
    auto [pref, o, err] = unpackUint16(msg, off);
    if (err) return {{}, nested("Pref", err)};
    Name mx;
    if (auto [o2, err2] = unpackName(mx, msg, o); err2) return {{}, nested("MX", err2)};
    return {MXResource(pref, mx), nullptr};
}

std::pair<NSResource, error> unpackNSResource(const slice<byte>& msg, int off) {
    Name ns;
    if (auto [o, err] = unpackName(ns, msg, off); err) return {{}, err};
    return {NSResource(ns), nullptr};
}

std::pair<PTRResource, error> unpackPTRResource(const slice<byte>& msg, int off) {
    Name p;
    if (auto [o, err] = unpackName(p, msg, off); err) return {{}, err};
    return {PTRResource(p), nullptr};
}

std::pair<SOAResource, error> unpackSOAResource(const slice<byte>& msg, int off) {
    SOAResource r;
    error err;
    std::tie(off, err) = unpackName(r.NS, msg, off);
    if (err) return {{}, nested("NS", err)};
    std::tie(off, err) = unpackName(r.MBox, msg, off);
    if (err) return {{}, nested("MBox", err)};
    std::tie(r.Serial, off, err) = unpackUint32(msg, off);
    if (err) return {{}, nested("Serial", err)};
    std::tie(r.Refresh, off, err) = unpackUint32(msg, off);
    if (err) return {{}, nested("Refresh", err)};
    std::tie(r.Retry, off, err) = unpackUint32(msg, off);
    if (err) return {{}, nested("Retry", err)};
    std::tie(r.Expire, off, err) = unpackUint32(msg, off);
    if (err) return {{}, nested("Expire", err)};
    std::tie(r.MinTTL, off, err) = unpackUint32(msg, off);
    if (err) return {{}, nested("MinTTL", err)};
    return {r, nullptr};
}

std::pair<TXTResource, error> unpackTXTResource(const slice<byte>& msg, int off, uint16 length) {
    std::vector<std::string> txts;
    for (uint16 n = 0; n < length;) {
        auto [t, o, err] = unpackText(msg, off);
        if (err) return {{}, nested("text", err)};
        off = o;
        // Check if we got too many bytes.
        if (length - n < uint16(t.size()) + 1) return {{}, errCalcLen};
        n += uint16(t.size()) + 1;
        txts.push_back(std::move(t));
    }
    return {TXTResource(std::move(txts)), nullptr};
}

std::pair<SRVResource, error> unpackSRVResource(const slice<byte>& msg, int off) {
    SRVResource r;
    error err;
    std::tie(r.Priority, off, err) = unpackUint16(msg, off);
    if (err) return {{}, nested("Priority", err)};
    std::tie(r.Weight, off, err) = unpackUint16(msg, off);
    if (err) return {{}, nested("Weight", err)};
    std::tie(r.Port, off, err) = unpackUint16(msg, off);
    if (err) return {{}, nested("Port", err)};
    if (auto [o, e] = unpackName(r.Target, msg, off); e) return {{}, nested("Target", e)};
    return {r, nullptr};
}

std::pair<AResource, error> unpackAResource(const slice<byte>& msg, int off) {
    AResource r;
    if (auto [o, err] = unpackBytes(msg, off, r.A); err) return {{}, err};
    return {r, nullptr};
}

std::pair<AAAAResource, error> unpackAAAAResource(const slice<byte>& msg, int off) {
    AAAAResource r;
    if (auto [o, err] = unpackBytes(msg, off, r.AAAA); err) return {{}, err};
    return {r, nullptr};
}

std::pair<OPTResource, error> unpackOPTResource(const slice<byte>& msg, int off, uint16 length) {
    OPTResource r;
    for (int oldOff = off; off < oldOff + int(length);) {
        Option o;
        error err;
        std::tie(o.Code, off, err) = unpackUint16(msg, off);
        if (err) return {{}, nested("Code", err)};
        uint16 l;
        std::tie(l, off, err) = unpackUint16(msg, off);
        if (err) return {{}, nested("Data", err)};
        if (off + int(l) > len(msg)) return {{}, nested("Data", errCalcLen)};
        o.Data = slice<byte>::of(std::span<const byte>(msg.data() + off, l));
        off += int(l);
        r.Options.push_back(std::move(o));
    }
    return {r, nullptr};
}

std::pair<UnknownResource, error> unpackUnknownResource(Type recordType, const slice<byte>& msg, int off,
                                                        uint16 length) {
    UnknownResource r;
    r.Type = recordType;
    r.Data = slice<byte>::make(length);
    if (auto [o, err] = unpackBytes(msg, off, r.Data); err) return {{}, err};
    return {r, nullptr};
}

std::tuple<ptr<ResourceBody>, int, error> unpackResourceBody(const slice<byte>& msg, int off,
                                                             const ResourceHeader& hdr) {
    ptr<ResourceBody> r;
    error err;
    const char* name;
    auto take = [&](auto res) {
        err = res.second;
        r = make_ptr<std::decay_t<decltype(res.first)>>(std::move(res.first));
    };
    switch (hdr.Type) {
    case TypeA: take(unpackAResource(msg, off)), name = "A"; break;
    case TypeNS: take(unpackNSResource(msg, off)), name = "NS"; break;
    case TypeCNAME: take(unpackCNAMEResource(msg, off)), name = "CNAME"; break;
    case TypeSOA: take(unpackSOAResource(msg, off)), name = "SOA"; break;
    case TypePTR: take(unpackPTRResource(msg, off)), name = "PTR"; break;
    case TypeMX: take(unpackMXResource(msg, off)), name = "MX"; break;
    case TypeTXT: take(unpackTXTResource(msg, off, hdr.Length)), name = "TXT"; break;
    case TypeAAAA: take(unpackAAAAResource(msg, off)), name = "AAAA"; break;
    case TypeSRV: take(unpackSRVResource(msg, off)), name = "SRV"; break;
    case TypeOPT: take(unpackOPTResource(msg, off, hdr.Length)), name = "OPT"; break;
    default: take(unpackUnknownResource(hdr.Type, msg, off, hdr.Length)), name = "Unknown"; break;
    }
    if (err) return {nullptr, off, nested(std::string(name) + " record", err)};
    return {r, off + int(hdr.Length), nullptr};
}

}  // namespace

const error ErrNotStarted = errors::New("parsing/packing of this type isn't available yet");
const error ErrSectionDone = errors::New("parsing/packing of this section has completed");

string TypeString(Type t) {
    switch (t) {
    case TypeA: return string::fromStatic("TypeA");
    case TypeNS: return string::fromStatic("TypeNS");
    case TypeCNAME: return string::fromStatic("TypeCNAME");
    case TypeSOA: return string::fromStatic("TypeSOA");
    case TypePTR: return string::fromStatic("TypePTR");
    case TypeMX: return string::fromStatic("TypeMX");
    case TypeTXT: return string::fromStatic("TypeTXT");
    case TypeAAAA: return string::fromStatic("TypeAAAA");
    case TypeSRV: return string::fromStatic("TypeSRV");
    case TypeOPT: return string::fromStatic("TypeOPT");
    case TypeSVCB: return string::fromStatic("TypeSVCB");
    case TypeHTTPS: return string::fromStatic("TypeHTTPS");
    case TypeWKS: return string::fromStatic("TypeWKS");
    case TypeHINFO: return string::fromStatic("TypeHINFO");
    case TypeMINFO: return string::fromStatic("TypeMINFO");
    case TypeAXFR: return string::fromStatic("TypeAXFR");
    case TypeALL: return string::fromStatic("TypeALL");
    }
    return string(std::to_string(uint16(t)));
}

string RCodeString(RCode r) {
    switch (r) {
    case RCodeSuccess: return string::fromStatic("RCodeSuccess");
    case RCodeFormatError: return string::fromStatic("RCodeFormatError");
    case RCodeServerFailure: return string::fromStatic("RCodeServerFailure");
    case RCodeNameError: return string::fromStatic("RCodeNameError");
    case RCodeNotImplemented: return string::fromStatic("RCodeNotImplemented");
    case RCodeRefused: return string::fromStatic("RCodeRefused");
    }
    return string(std::to_string(uint16(r)));
}

std::pair<uint16, uint16> Header::pack() const {
    uint16 bits = uint16(OpCode << 11 | RCode);
    if (RecursionAvailable) bits |= headerBitRA;
    if (RecursionDesired) bits |= headerBitRD;
    if (Truncated) bits |= headerBitTC;
    if (Authoritative) bits |= headerBitAA;
    if (Response) bits |= headerBitQR;
    if (AuthenticData) bits |= headerBitAD;
    if (CheckingDisabled) bits |= headerBitCD;
    return {ID, bits};
}

std::pair<Name, error> NewName(std::string_view name) {
    Name n;
    if (name.size() > n.Data.size()) return {Name{}, errCalcLen};
    n.Length = uint8(name.size());
    std::memcpy(n.Data.data(), name.data(), name.size());
    return {n, nullptr};
}

Name MustNewName(std::string_view name) {
    auto [n, err] = NewName(name);
    if (err) panic("creating name: " + std::string(err.Error().view()));
    return n;
}

// ---------------------------------------------------------------------------
// ResourceHeader

error ResourceHeader::pack(slice<byte>& msg, compressionMap* compression, int compressionOff, int* lenOff) const {
    slice<byte> out = msg;
    if (error err = packName(Name, out, compression, compressionOff)) return nested("Name", err);
    out = packUint16(std::move(out), Type);
    out = packUint16(std::move(out), Class);
    out = packUint32(std::move(out), TTL);
    *lenOff = len(out);
    msg = packUint16(std::move(out), Length);
    return nullptr;
}

std::pair<int, error> ResourceHeader::unpack(const slice<byte>& msg, int off) {
    int newOff = off;
    error err;
    std::tie(newOff, err) = unpackName(Name, msg, newOff);
    if (err) return {off, nested("Name", err)};
    uint16 v;
    std::tie(v, newOff, err) = unpackUint16(msg, newOff);
    if (err) return {off, nested("Type", err)};
    Type = dnsmessage::Type(v);
    std::tie(v, newOff, err) = unpackUint16(msg, newOff);
    if (err) return {off, nested("Class", err)};
    Class = dnsmessage::Class(v);
    std::tie(TTL, newOff, err) = unpackUint32(msg, newOff);
    if (err) return {off, nested("TTL", err)};
    std::tie(Length, newOff, err) = unpackUint16(msg, newOff);
    if (err) return {off, nested("Length", err)};
    return {newOff, nullptr};
}

error ResourceHeader::fixLen(slice<byte>& msg, int lenOff, int preLen) {
    int conLen = len(msg) - preLen;
    if (conLen > int(uint16(~0))) return errResTooLong;
    // Fill in the length now that we know how long the content is.
    msg[std::size_t(lenOff)] = byte(conLen >> 8);
    msg[std::size_t(lenOff + 1)] = byte(conLen);
    Length = uint16(conLen);
    return nullptr;
}

namespace {
constexpr uint32 edns0Version = 0;
constexpr uint32 edns0DNSSECOK = 0x00008000;
constexpr uint32 ednsVersionMask = 0x00ff0000;
constexpr uint32 edns0DNSSECOKMask = 0x00ff8000;
}  // namespace

error ResourceHeader::SetEDNS0(int udpPayloadLen, RCode extRCode, bool dnssecOK) {
    Name = dnsmessage::Name{};
    Name.Data[0] = '.';  // RFC 6891 section 6.1.2
    Name.Length = 1;
    Type = TypeOPT;
    Class = dnsmessage::Class(udpPayloadLen);
    TTL = uint32(extRCode) >> 4 << 24;
    if (dnssecOK) TTL |= edns0DNSSECOK;
    return nullptr;
}

bool ResourceHeader::DNSSECAllowed() const { return (TTL & edns0DNSSECOKMask) == edns0DNSSECOK; }

RCode ResourceHeader::ExtendedRCode(RCode rcode) const {
    if ((TTL & ednsVersionMask) == edns0Version) return dnsmessage::RCode(TTL >> 24 << 4 | rcode);
    return rcode;
}

// ---------------------------------------------------------------------------
// Resource bodies

error CNAMEResource::pack(slice<byte>& msg, compressionMap* c, int off) const { return packName(CNAME, msg, c, off); }

error MXResource::pack(slice<byte>& msg, compressionMap* c, int off) const {
    slice<byte> out = packUint16(msg, Pref);
    if (error err = packName(MX, out, c, off)) return nested("MXResource.MX", err);
    msg = std::move(out);
    return nullptr;
}

error NSResource::pack(slice<byte>& msg, compressionMap* c, int off) const { return packName(NS, msg, c, off); }

error PTRResource::pack(slice<byte>& msg, compressionMap* c, int off) const { return packName(PTR, msg, c, off); }

error SOAResource::pack(slice<byte>& msg, compressionMap* c, int off) const {
    slice<byte> out = msg;
    if (error err = packName(NS, out, c, off)) return nested("SOAResource.NS", err);
    if (error err = packName(MBox, out, c, off)) return nested("SOAResource.MBox", err);
    out = packUint32(std::move(out), Serial);
    out = packUint32(std::move(out), Refresh);
    out = packUint32(std::move(out), Retry);
    out = packUint32(std::move(out), Expire);
    msg = packUint32(std::move(out), MinTTL);
    return nullptr;
}

error TXTResource::pack(slice<byte>& msg, compressionMap*, int) const {
    slice<byte> out = msg;
    for (auto& s : TXT) {
        if (error err = packText(out, s)) return err;
    }
    msg = std::move(out);
    return nullptr;
}

error SRVResource::pack(slice<byte>& msg, compressionMap*, int off) const {
    slice<byte> out = packUint16(msg, Priority);
    out = packUint16(std::move(out), Weight);
    out = packUint16(std::move(out), Port);
    if (error err = packName(Target, out, nullptr, off)) return nested("SRVResource.Target", err);
    msg = std::move(out);
    return nullptr;
}

error AResource::pack(slice<byte>& msg, compressionMap*, int) const {
    msg = packBytes(std::move(msg), A);
    return nullptr;
}

error AAAAResource::pack(slice<byte>& msg, compressionMap*, int) const {
    msg = packBytes(std::move(msg), AAAA);
    return nullptr;
}

error OPTResource::pack(slice<byte>& msg, compressionMap*, int) const {
    for (auto& opt : Options) {
        msg = packUint16(std::move(msg), opt.Code);
        msg = packUint16(std::move(msg), uint16(opt.Data.size()));
        msg = packBytes(std::move(msg), opt.Data);
    }
    return nullptr;
}

error UnknownResource::pack(slice<byte>& msg, compressionMap*, int) const {
    msg = packBytes(std::move(msg), Data);
    return nullptr;
}

error Resource::pack(slice<byte>& msg, compressionMap* compression, int compressionOff) {
    if (!Body) return errNilResouceBody;
    slice<byte> out = msg;
    Header.Type = Body->realType();
    int lenOff;
    if (error err = Header.pack(out, compression, compressionOff, &lenOff)) return nested("ResourceHeader", err);
    int preLen = len(out);
    if (error err = Body->pack(out, compression, compressionOff)) return nested("content", err);
    if (error err = Header.fixLen(out, lenOff, preLen)) return err;
    msg = std::move(out);
    return nullptr;
}

// ---------------------------------------------------------------------------
// header

uint16 header::count(section sec) const {
    switch (sec) {
    case sectionQuestions: return questions;
    case sectionAnswers: return answers;
    case sectionAuthorities: return authorities;
    case sectionAdditionals: return additionals;
    default: return 0;
    }
}

slice<byte> header::pack(slice<byte> msg) const {
    msg = packUint16(std::move(msg), id);
    msg = packUint16(std::move(msg), bits);
    msg = packUint16(std::move(msg), questions);
    msg = packUint16(std::move(msg), answers);
    msg = packUint16(std::move(msg), authorities);
    return packUint16(std::move(msg), additionals);
}

std::pair<int, error> header::unpack(const slice<byte>& msg, int off) {
    int newOff = off;
    error err;
    std::tie(id, newOff, err) = unpackUint16(msg, newOff);
    if (err) return {off, nested("id", err)};
    std::tie(bits, newOff, err) = unpackUint16(msg, newOff);
    if (err) return {off, nested("bits", err)};
    std::tie(questions, newOff, err) = unpackUint16(msg, newOff);
    if (err) return {off, nested("questions", err)};
    std::tie(answers, newOff, err) = unpackUint16(msg, newOff);
    if (err) return {off, nested("answers", err)};
    std::tie(authorities, newOff, err) = unpackUint16(msg, newOff);
    if (err) return {off, nested("authorities", err)};
    std::tie(additionals, newOff, err) = unpackUint16(msg, newOff);
    if (err) return {off, nested("additionals", err)};
    return {newOff, nullptr};
}

Header header::header_() const {
    Header h;
    h.ID = id;
    h.Response = (bits & headerBitQR) != 0;
    h.OpCode = OpCode((bits >> 11) & 0xF);
    h.Authoritative = (bits & headerBitAA) != 0;
    h.Truncated = (bits & headerBitTC) != 0;
    h.RecursionDesired = (bits & headerBitRD) != 0;
    h.RecursionAvailable = (bits & headerBitRA) != 0;
    h.AuthenticData = (bits & headerBitAD) != 0;
    h.CheckingDisabled = (bits & headerBitCD) != 0;
    h.RCode = dnsmessage::RCode(bits & 0xF);
    return h;
}

// ---------------------------------------------------------------------------
// Parser

std::pair<Header, error> Parser::Start(slice<byte> msg) {
    *this = Parser{};
    msg_ = std::move(msg);
    auto [off, err] = header_.unpack(msg_, 0);
    if (err) return {Header{}, nested("unpacking header", err)};
    off_ = off;
    section_ = sectionQuestions;
    return {header_.header_(), nullptr};
}

error Parser::checkAdvance(section sec) {
    if (section_ < sec) return ErrNotStarted;
    if (section_ > sec) return ErrSectionDone;
    resHeaderValid_ = false;
    if (index_ == int(header_.count(sec))) {
        index_ = 0;
        section_ = section(section_ + 1);
        return ErrSectionDone;
    }
    return nullptr;
}

std::pair<Resource, error> Parser::resource(section sec) {
    Resource r;
    error err;
    std::tie(r.Header, err) = resourceHeader(sec);
    if (err) return {r, err};
    resHeaderValid_ = false;
    std::tie(r.Body, off_, err) = unpackResourceBody(msg_, off_, r.Header);
    if (err) return {Resource{}, nested("unpacking " + std::string(sectionName(sec)), err)};
    index_++;
    return {r, nullptr};
}

std::pair<ResourceHeader, error> Parser::resourceHeader(section sec) {
    if (resHeaderValid_) off_ = resHeaderOffset_;
    if (error err = checkAdvance(sec)) return {ResourceHeader{}, err};
    ResourceHeader hdr;
    auto [off, err] = hdr.unpack(msg_, off_);
    if (err) return {ResourceHeader{}, err};
    resHeaderValid_ = true;
    resHeaderOffset_ = off_;
    resHeaderType_ = hdr.Type;
    resHeaderLength_ = hdr.Length;
    off_ = off;
    return {hdr, nullptr};
}

error Parser::skipResource(section sec) {
    if (resHeaderValid_ && section_ == sec) {
        int newOff = off_ + int(resHeaderLength_);
        if (newOff > len(msg_)) return errResourceLen;
        off_ = newOff;
        resHeaderValid_ = false;
        index_++;
        return nullptr;
    }
    if (error err = checkAdvance(sec)) return err;
    auto [off, err] = skipResourceRec(msg_, off_);
    if (err) return nested("skipping: " + std::string(sectionName(sec)), err);
    off_ = off;
    index_++;
    return nullptr;
}

std::pair<Question, error> Parser::Question() {
    if (error err = checkAdvance(sectionQuestions)) return {dnsmessage::Question{}, err};
    dnsmessage::Question q;
    auto [off, err] = unpackName(q.Name, msg_, off_);
    if (err) return {dnsmessage::Question{}, nested("unpacking Question.Name", err)};
    uint16 v;
    std::tie(v, off, err) = unpackUint16(msg_, off);
    if (err) return {dnsmessage::Question{}, nested("unpacking Question.Type", err)};
    q.Type = Type(v);
    std::tie(v, off, err) = unpackUint16(msg_, off);
    if (err) return {dnsmessage::Question{}, nested("unpacking Question.Class", err)};
    q.Class = Class(v);
    off_ = off;
    index_++;
    return {q, nullptr};
}

std::pair<std::vector<Question>, error> Parser::AllQuestions() {
    std::vector<dnsmessage::Question> qs;
    for (;;) {
        auto [q, err] = Question();
        if (err == ErrSectionDone) return {qs, nullptr};
        if (err) return {{}, err};
        qs.push_back(q);
    }
}

error Parser::SkipQuestion() {
    if (error err = checkAdvance(sectionQuestions)) return err;
    auto [off, err] = skipName(msg_, off_);
    if (err) return nested("skipping Question Name", err);
    std::tie(off, err) = skipUint16(msg_, off);
    if (err) return nested("skipping Question Type", err);
    std::tie(off, err) = skipUint16(msg_, off);
    if (err) return nested("skipping Question Class", err);
    off_ = off;
    index_++;
    return nullptr;
}

error Parser::SkipAllQuestions() {
    for (;;) {
        error err = SkipQuestion();
        if (err == ErrSectionDone) return nullptr;
        if (err) return err;
    }
}

#define GOPP_SECTION(Name, Plural, sec, cap)                                                      \
    std::pair<ResourceHeader, error> Parser::Name##Header() { return resourceHeader(sec); }        \
    std::pair<Resource, error> Parser::Name() { return resource(sec); }                            \
    std::pair<std::vector<Resource>, error> Parser::All##Plural() {                                \
        std::vector<dnsmessage::Resource> as;                                                      \
        as.reserve(std::min<std::size_t>(header_.count(sec), cap));                                \
        for (;;) {                                                                                 \
            auto [a, err] = Name();                                                                \
            if (err == ErrSectionDone) return {as, nullptr};                                       \
            if (err) return {{}, err};                                                             \
            as.push_back(std::move(a));                                                            \
        }                                                                                          \
    }                                                                                              \
    error Parser::Skip##Name() { return skipResource(sec); }                                       \
    error Parser::SkipAll##Plural() {                                                              \
        for (;;) {                                                                                 \
            error err = Skip##Name();                                                              \
            if (err == ErrSectionDone) return nullptr;                                             \
            if (err) return err;                                                                   \
        }                                                                                          \
    }

GOPP_SECTION(Answer, Answers, sectionAnswers, 20)
GOPP_SECTION(Authority, Authorities, sectionAuthorities, 10)
GOPP_SECTION(Additional, Additionals, sectionAdditionals, 10)
#undef GOPP_SECTION

template <typename R, typename F>
std::pair<R, error> Parser::body(dnsmessage::Type t, F unpack) {
    if (!resHeaderValid_ || resHeaderType_ != t) return {R{}, ErrNotStarted};
    auto [r, err] = unpack();
    if (err) return {R{}, err};
    off_ += int(resHeaderLength_);
    resHeaderValid_ = false;
    index_++;
    return {std::move(r), nullptr};
}

std::pair<CNAMEResource, error> Parser::CNAMEResource() {
    return body<dnsmessage::CNAMEResource>(TypeCNAME, [&] { return unpackCNAMEResource(msg_, off_); });
}
std::pair<MXResource, error> Parser::MXResource() {
    return body<dnsmessage::MXResource>(TypeMX, [&] { return unpackMXResource(msg_, off_); });
}
std::pair<NSResource, error> Parser::NSResource() {
    return body<dnsmessage::NSResource>(TypeNS, [&] { return unpackNSResource(msg_, off_); });
}
std::pair<PTRResource, error> Parser::PTRResource() {
    return body<dnsmessage::PTRResource>(TypePTR, [&] { return unpackPTRResource(msg_, off_); });
}
std::pair<SOAResource, error> Parser::SOAResource() {
    return body<dnsmessage::SOAResource>(TypeSOA, [&] { return unpackSOAResource(msg_, off_); });
}
std::pair<TXTResource, error> Parser::TXTResource() {
    return body<dnsmessage::TXTResource>(TypeTXT, [&] { return unpackTXTResource(msg_, off_, resHeaderLength_); });
}
std::pair<SRVResource, error> Parser::SRVResource() {
    return body<dnsmessage::SRVResource>(TypeSRV, [&] { return unpackSRVResource(msg_, off_); });
}
std::pair<AResource, error> Parser::AResource() {
    return body<dnsmessage::AResource>(TypeA, [&] { return unpackAResource(msg_, off_); });
}
std::pair<AAAAResource, error> Parser::AAAAResource() {
    return body<dnsmessage::AAAAResource>(TypeAAAA, [&] { return unpackAAAAResource(msg_, off_); });
}
std::pair<OPTResource, error> Parser::OPTResource() {
    return body<dnsmessage::OPTResource>(TypeOPT, [&] { return unpackOPTResource(msg_, off_, resHeaderLength_); });
}
std::pair<UnknownResource, error> Parser::UnknownResource() {
    if (!resHeaderValid_) return {dnsmessage::UnknownResource{}, ErrNotStarted};
    return body<dnsmessage::UnknownResource>(resHeaderType_, [&] {
        return unpackUnknownResource(resHeaderType_, msg_, off_, resHeaderLength_);
    });
}

// ---------------------------------------------------------------------------
// Message

error Message::Unpack(const slice<byte>& msg) {
    Parser p;
    error err;
    std::tie(Header, err) = p.Start(msg);
    if (err) return err;
    std::tie(Questions, err) = p.AllQuestions();
    if (err) return err;
    std::tie(Answers, err) = p.AllAnswers();
    if (err) return err;
    std::tie(Authorities, err) = p.AllAuthorities();
    if (err) return err;
    std::tie(Additionals, err) = p.AllAdditionals();
    return err;
}

std::pair<slice<byte>, error> Message::Pack() { return AppendPack(slice<byte>::make(0, packStartingCap)); }

std::pair<slice<byte>, error> Message::AppendPack(slice<byte> b) {
    constexpr std::size_t max = uint16(~0);
    if (Questions.size() > max) return {{}, errTooManyQuestions};
    if (Answers.size() > max) return {{}, errTooManyAnswers};
    if (Authorities.size() > max) return {{}, errTooManyAuthorities};
    if (Additionals.size() > max) return {{}, errTooManyAdditionals};
    header h;
    std::tie(h.id, h.bits) = Header.pack();
    h.questions = uint16(Questions.size());
    h.answers = uint16(Answers.size());
    h.authorities = uint16(Authorities.size());
    h.additionals = uint16(Additionals.size());
    int compressionOff = len(b);
    slice<byte> msg = h.pack(std::move(b));
    // RFC 1035 allows (but doesn't require) compression for packing.
    compressionMap compression;
    for (auto& q : Questions) {
        if (error err = packQuestion(q, msg, &compression, compressionOff)) return {{}, nested("packing Question", err)};
    }
    for (auto& a : Answers) {
        if (error err = a.pack(msg, &compression, compressionOff)) return {{}, nested("packing Answer", err)};
    }
    for (auto& a : Authorities) {
        if (error err = a.pack(msg, &compression, compressionOff)) return {{}, nested("packing Authority", err)};
    }
    for (auto& a : Additionals) {
        if (error err = a.pack(msg, &compression, compressionOff)) return {{}, nested("packing Additional", err)};
    }
    return {msg, nullptr};
}

// ---------------------------------------------------------------------------
// Builder

Builder::Builder(slice<byte> buf, dnsmessage::Header h) {
    if (buf.isNil()) buf = slice<byte>::make(0, packStartingCap);
    start_ = len(buf);
    std::tie(header_.id, header_.bits) = h.pack();
    byte hb[headerLen] = {};
    msg_ = appendSpan(std::move(buf), std::span<const byte>(hb));
    section_ = sectionHeader;
}

void Builder::EnableCompression() { compression_ = std::make_unique<compressionMap>(); }

error Builder::startCheck(section s) {
    if (section_ <= sectionNotStarted) return ErrNotStarted;
    if (section_ > s) return ErrSectionDone;
    return nullptr;
}

error Builder::StartQuestions() {
    if (error err = startCheck(sectionQuestions)) return err;
    section_ = sectionQuestions;
    return nullptr;
}

error Builder::StartAnswers() {
    if (error err = startCheck(sectionAnswers)) return err;
    section_ = sectionAnswers;
    return nullptr;
}

error Builder::StartAuthorities() {
    if (error err = startCheck(sectionAuthorities)) return err;
    section_ = sectionAuthorities;
    return nullptr;
}

error Builder::StartAdditionals() {
    if (error err = startCheck(sectionAdditionals)) return err;
    section_ = sectionAdditionals;
    return nullptr;
}

error Builder::incrementSectionCount() {
    uint16* count = nullptr;
    error err;
    switch (section_) {
    case sectionQuestions: count = &header_.questions, err = errTooManyQuestions; break;
    case sectionAnswers: count = &header_.answers, err = errTooManyAnswers; break;
    case sectionAuthorities: count = &header_.authorities, err = errTooManyAuthorities; break;
    case sectionAdditionals: count = &header_.additionals, err = errTooManyAdditionals; break;
    default: return nullptr;
    }
    if (*count == uint16(~0)) return err;
    (*count)++;
    return nullptr;
}

error Builder::Question(const dnsmessage::Question& q) {
    if (section_ < sectionQuestions) return ErrNotStarted;
    if (section_ > sectionQuestions) return ErrSectionDone;
    slice<byte> msg = msg_;
    if (error err = packQuestion(q, msg, compression_.get(), start_)) return err;
    if (error err = incrementSectionCount()) return err;
    msg_ = std::move(msg);
    return nullptr;
}

error Builder::checkResourceSection() {
    if (section_ < sectionAnswers) return ErrNotStarted;
    if (section_ > sectionAdditionals) return ErrSectionDone;
    return nullptr;
}

error Builder::addResource(dnsmessage::ResourceHeader h, const ResourceBody& r, std::string_view name) {
    if (error err = checkResourceSection()) return err;
    h.Type = r.realType();
    slice<byte> msg = msg_;
    int lenOff;
    if (error err = h.pack(msg, compression_.get(), start_, &lenOff)) return nested("ResourceHeader", err);
    int preLen = len(msg);
    if (error err = r.pack(msg, compression_.get(), start_)) return nested(std::string(name) + " body", err);
    if (error err = h.fixLen(msg, lenOff, preLen)) return err;
    if (error err = incrementSectionCount()) return err;
    msg_ = std::move(msg);
    return nullptr;
}

error Builder::CNAMEResource(dnsmessage::ResourceHeader h, const dnsmessage::CNAMEResource& r) {
    return addResource(h, r, "CNAMEResource");
}
error Builder::MXResource(dnsmessage::ResourceHeader h, const dnsmessage::MXResource& r) {
    return addResource(h, r, "MXResource");
}
error Builder::NSResource(dnsmessage::ResourceHeader h, const dnsmessage::NSResource& r) {
    return addResource(h, r, "NSResource");
}
error Builder::PTRResource(dnsmessage::ResourceHeader h, const dnsmessage::PTRResource& r) {
    return addResource(h, r, "PTRResource");
}
error Builder::SOAResource(dnsmessage::ResourceHeader h, const dnsmessage::SOAResource& r) {
    return addResource(h, r, "SOAResource");
}
error Builder::TXTResource(dnsmessage::ResourceHeader h, const dnsmessage::TXTResource& r) {
    return addResource(h, r, "TXTResource");
}
error Builder::SRVResource(dnsmessage::ResourceHeader h, const dnsmessage::SRVResource& r) {
    return addResource(h, r, "SRVResource");
}
error Builder::AResource(dnsmessage::ResourceHeader h, const dnsmessage::AResource& r) {
    return addResource(h, r, "AResource");
}
error Builder::AAAAResource(dnsmessage::ResourceHeader h, const dnsmessage::AAAAResource& r) {
    return addResource(h, r, "AAAAResource");
}
error Builder::OPTResource(dnsmessage::ResourceHeader h, const dnsmessage::OPTResource& r) {
    return addResource(h, r, "OPTResource");
}
error Builder::UnknownResource(dnsmessage::ResourceHeader h, const dnsmessage::UnknownResource& r) {
    return addResource(h, r, "UnknownResource");
}

std::pair<slice<byte>, error> Builder::Finish() {
    if (section_ < sectionHeader) return {{}, ErrNotStarted};
    section_ = sectionDone;
    // Space for the header was allocated in NewBuilder.
    slice<byte> h = header_.pack(slice<byte>::make(0, headerLen));
    std::copy(h.begin(), h.end(), msg_.begin() + start_);
    return {msg_, nullptr};
}

}  // namespace gopp::net::dnsmessage
