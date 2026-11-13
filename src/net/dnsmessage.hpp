// DNS messages, ported from golang.org/x/net/dns/dnsmessage (message.go) as
// vendored by Go 1.27.3, for the resolver. SVCB and HTTPS records (svcb.go)
// aren't ported: they parse as UnknownResource.
#pragma once

#include <gopp/errors.hpp>
#include <gopp/ptr.hpp>
#include <gopp/slice.hpp>
#include <gopp/string.hpp>

#include <array>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace gopp::net::dnsmessage {

// A type of DNS request and response.
enum Type : uint16 {
    TypeA = 1,
    TypeNS = 2,
    TypeCNAME = 5,
    TypeSOA = 6,
    TypePTR = 12,
    TypeMX = 15,
    TypeTXT = 16,
    TypeAAAA = 28,
    TypeSRV = 33,
    TypeOPT = 41,
    TypeSVCB = 64,
    TypeHTTPS = 65,
    // Question types.
    TypeWKS = 11,
    TypeHINFO = 13,
    TypeMINFO = 14,
    TypeAXFR = 252,
    TypeALL = 255,
};
string TypeString(Type t);

// A type of network.
enum Class : uint16 {
    ClassINET = 1,
    ClassCSNET = 2,
    ClassCHAOS = 3,
    ClassHESIOD = 4,
    ClassANY = 255,
};

using OpCode = uint16;

// A DNS response status code.
enum RCode : uint16 {
    RCodeSuccess = 0,
    RCodeFormatError = 1,
    RCodeServerFailure = 2,
    RCodeNameError = 3,
    RCodeNotImplemented = 4,
    RCodeRefused = 5,
};
string RCodeString(RCode r);

// ErrNotStarted: a section that isn't ready; ErrSectionDone: a section
// that's finished.
extern const error ErrNotStarted;
extern const error ErrSectionDone;

// A message header.
struct Header {
    uint16 ID = 0;
    bool Response = false;
    dnsmessage::OpCode OpCode = 0;
    bool Authoritative = false;
    bool Truncated = false;
    bool RecursionDesired = false;
    bool RecursionAvailable = false;
    bool AuthenticData = false;
    bool CheckingDisabled = false;
    dnsmessage::RCode RCode = RCodeSuccess;

    std::pair<uint16, uint16> pack() const;
};

// A non-encoded and non-escaped domain name: a sequence of labels each
// followed by a dot.
struct Name {
    std::array<byte, 255> Data{};
    uint8 Length = 0;
    std::string String() const { return std::string(reinterpret_cast<const char*>(Data.data()), Length); }
    friend bool operator==(const Name& a, const Name& b) {
        return a.Length == b.Length && std::equal(a.Data.begin(), a.Data.begin() + a.Length, b.Data.begin());
    }
};

using compression_map = std::map<std::string, uint16, std::less<>>;

// NewName: a Name of name, which must be at most 255 bytes.
std::pair<Name, error> NewName(std::string_view name);
Name MustNewName(std::string_view name);

// A DNS question.
struct Question {
    dnsmessage::Name Name;
    dnsmessage::Type Type = TypeA;
    dnsmessage::Class Class = ClassINET;
};

// The header of a resource record.
struct ResourceHeader {
    dnsmessage::Name Name;
    // Set by the Builder from the body.
    dnsmessage::Type Type = TypeA;
    dnsmessage::Class Class = ClassINET;
    uint32 TTL = 0;
    // The length of the data after the header; set by the Builder.
    uint16 Length = 0;

    // SetEDNS0 configures h for EDNS(0).
    error SetEDNS0(int udpPayloadLen, RCode extRCode, bool dnssecOK);
    bool DNSSECAllowed() const;
    RCode ExtendedRCode(RCode rcode) const;

    error pack(slice<byte>& msg, compression_map* compression, int compressionOff, int* lenOff) const;
    std::pair<int, error> unpack(const slice<byte>& msg, int off);
    error fixLen(slice<byte>& msg, int lenOff, int preLen);
};

// The body of a resource record.
struct ResourceBody : public virtual object {
    virtual Type realType() const = 0;
    virtual error pack(slice<byte>& msg, compression_map* compression, int compressionOff) const = 0;
};

struct CNAMEResource final : ResourceBody {
    Name CNAME;
    CNAMEResource() = default;
    explicit CNAMEResource(Name n) : CNAME(n) {}
    Type realType() const override { return TypeCNAME; }
    error pack(slice<byte>& msg, compression_map* c, int off) const override;
};

struct MXResource final : ResourceBody {
    uint16 Pref = 0;
    Name MX;
    MXResource() = default;
    MXResource(uint16 p, Name n) : Pref(p), MX(n) {}
    Type realType() const override { return TypeMX; }
    error pack(slice<byte>& msg, compression_map* c, int off) const override;
};

struct NSResource final : ResourceBody {
    Name NS;
    NSResource() = default;
    explicit NSResource(Name n) : NS(n) {}
    Type realType() const override { return TypeNS; }
    error pack(slice<byte>& msg, compression_map* c, int off) const override;
};

struct PTRResource final : ResourceBody {
    Name PTR;
    PTRResource() = default;
    explicit PTRResource(Name n) : PTR(n) {}
    Type realType() const override { return TypePTR; }
    error pack(slice<byte>& msg, compression_map* c, int off) const override;
};

struct SOAResource final : ResourceBody {
    Name NS;
    Name MBox;
    uint32 Serial = 0, Refresh = 0, Retry = 0, Expire = 0;
    // The minimum TTL of the zone (RFC 2308: negative caching).
    uint32 MinTTL = 0;
    Type realType() const override { return TypeSOA; }
    error pack(slice<byte>& msg, compression_map* c, int off) const override;
};

struct TXTResource final : ResourceBody {
    std::vector<std::string> TXT;
    TXTResource() = default;
    explicit TXTResource(std::vector<std::string> t) : TXT(std::move(t)) {}
    Type realType() const override { return TypeTXT; }
    error pack(slice<byte>& msg, compression_map* c, int off) const override;
};

struct SRVResource final : ResourceBody {
    uint16 Priority = 0, Weight = 0, Port = 0;
    Name Target;  // not compressed, as RFC 2782 says
    Type realType() const override { return TypeSRV; }
    error pack(slice<byte>& msg, compression_map* c, int off) const override;
};

struct AResource final : ResourceBody {
    std::array<byte, 4> A{};
    AResource() = default;
    explicit AResource(std::array<byte, 4> a) : A(a) {}
    Type realType() const override { return TypeA; }
    error pack(slice<byte>& msg, compression_map* c, int off) const override;
};

struct AAAAResource final : ResourceBody {
    std::array<byte, 16> AAAA{};
    AAAAResource() = default;
    explicit AAAAResource(std::array<byte, 16> a) : AAAA(a) {}
    Type realType() const override { return TypeAAAA; }
    error pack(slice<byte>& msg, compression_map* c, int off) const override;
};

// An option of an OPT pseudo-record.
struct Option {
    uint16 Code = 0;
    slice<byte> Data;
};

struct OPTResource final : ResourceBody {
    std::vector<Option> Options;
    Type realType() const override { return TypeOPT; }
    error pack(slice<byte>& msg, compression_map* c, int off) const override;
};

struct UnknownResource final : ResourceBody {
    dnsmessage::Type Type = dnsmessage::Type(0);
    slice<byte> Data;
    dnsmessage::Type realType() const override { return Type; }
    error pack(slice<byte>& msg, compression_map* c, int off) const override;
};

// A DNS resource record.
struct Resource {
    ResourceHeader Header;
    ptr<ResourceBody> Body;
    error pack(slice<byte>& msg, compression_map* compression, int compressionOff);
};

// A DNS message.
struct Message {
    dnsmessage::Header Header;
    std::vector<Question> Questions;
    std::vector<Resource> Answers;
    std::vector<Resource> Authorities;
    std::vector<Resource> Additionals;

    error Unpack(const slice<byte>& msg);
    std::pair<slice<byte>, error> Pack();
    std::pair<slice<byte>, error> AppendPack(slice<byte> b);
};

enum section : uint8 {
    sectionNotStarted,
    sectionHeader,
    sectionQuestions,
    sectionAnswers,
    sectionAuthorities,
    sectionAdditionals,
    sectionDone,
};

struct header {
    uint16 id = 0, bits = 0, questions = 0, answers = 0, authorities = 0, additionals = 0;
    uint16 count(section sec) const;
    slice<byte> pack(slice<byte> msg) const;
    std::pair<int, error> unpack(const slice<byte>& msg, int off);
    dnsmessage::Header header_() const;
};

// Parser parses a message incrementally, section by section.
class Parser {
public:
    std::pair<dnsmessage::Header, error> Start(slice<byte> msg);

    std::pair<dnsmessage::Question, error> Question();
    std::pair<std::vector<dnsmessage::Question>, error> AllQuestions();
    error SkipQuestion();
    error SkipAllQuestions();

    std::pair<dnsmessage::ResourceHeader, error> AnswerHeader();
    std::pair<dnsmessage::Resource, error> Answer();
    std::pair<std::vector<dnsmessage::Resource>, error> AllAnswers();
    error SkipAnswer();
    error SkipAllAnswers();

    std::pair<dnsmessage::ResourceHeader, error> AuthorityHeader();
    std::pair<dnsmessage::Resource, error> Authority();
    std::pair<std::vector<dnsmessage::Resource>, error> AllAuthorities();
    error SkipAuthority();
    error SkipAllAuthorities();

    std::pair<dnsmessage::ResourceHeader, error> AdditionalHeader();
    std::pair<dnsmessage::Resource, error> Additional();
    std::pair<std::vector<dnsmessage::Resource>, error> AllAdditionals();
    error SkipAdditional();
    error SkipAllAdditionals();

    // The body of the resource whose header was just read.
    std::pair<dnsmessage::CNAMEResource, error> CNAMEResource();
    std::pair<dnsmessage::MXResource, error> MXResource();
    std::pair<dnsmessage::NSResource, error> NSResource();
    std::pair<dnsmessage::PTRResource, error> PTRResource();
    std::pair<dnsmessage::SOAResource, error> SOAResource();
    std::pair<dnsmessage::TXTResource, error> TXTResource();
    std::pair<dnsmessage::SRVResource, error> SRVResource();
    std::pair<dnsmessage::AResource, error> AResource();
    std::pair<dnsmessage::AAAAResource, error> AAAAResource();
    std::pair<dnsmessage::OPTResource, error> OPTResource();
    std::pair<dnsmessage::UnknownResource, error> UnknownResource();

private:
    error checkAdvance(section sec);
    std::pair<dnsmessage::Resource, error> resource(section sec);
    std::pair<dnsmessage::ResourceHeader, error> resourceHeader(section sec);
    error skipResource(section sec);
    template <typename R, typename F>
    std::pair<R, error> body(dnsmessage::Type t, F unpack);

    slice<byte> msg_;
    header header_;
    section section_ = sectionNotStarted;
    int off_ = 0;
    int index_ = 0;
    bool resHeaderValid_ = false;
    int resHeaderOffset_ = 0;
    dnsmessage::Type resHeaderType_ = dnsmessage::Type(0);
    uint16 resHeaderLength_ = 0;
};

// Builder builds a message section by section.
class Builder {
public:
    Builder() = default;
    // NewBuilder: a Builder appending to buf (a new buffer if it's nil).
    Builder(slice<byte> buf, dnsmessage::Header h);
    void EnableCompression();
    error StartQuestions();
    error StartAnswers();
    error StartAuthorities();
    error StartAdditionals();
    error Question(const dnsmessage::Question& q);
    error CNAMEResource(dnsmessage::ResourceHeader h, const dnsmessage::CNAMEResource& r);
    error MXResource(dnsmessage::ResourceHeader h, const dnsmessage::MXResource& r);
    error NSResource(dnsmessage::ResourceHeader h, const dnsmessage::NSResource& r);
    error PTRResource(dnsmessage::ResourceHeader h, const dnsmessage::PTRResource& r);
    error SOAResource(dnsmessage::ResourceHeader h, const dnsmessage::SOAResource& r);
    error TXTResource(dnsmessage::ResourceHeader h, const dnsmessage::TXTResource& r);
    error SRVResource(dnsmessage::ResourceHeader h, const dnsmessage::SRVResource& r);
    error AResource(dnsmessage::ResourceHeader h, const dnsmessage::AResource& r);
    error AAAAResource(dnsmessage::ResourceHeader h, const dnsmessage::AAAAResource& r);
    error OPTResource(dnsmessage::ResourceHeader h, const dnsmessage::OPTResource& r);
    error UnknownResource(dnsmessage::ResourceHeader h, const dnsmessage::UnknownResource& r);
    std::pair<slice<byte>, error> Finish();

private:
    error startCheck(section s);
    error incrementSectionCount();
    error checkResourceSection();
    error addResource(dnsmessage::ResourceHeader h, const ResourceBody& r, std::string_view name);

    slice<byte> msg_;
    section section_ = sectionNotStarted;
    header header_;
    int start_ = 0;
    std::unique_ptr<compression_map> compression_;
};

inline Builder NewBuilder(slice<byte> buf, Header h) { return Builder(std::move(buf), h); }

}  // namespace gopp::net::dnsmessage
