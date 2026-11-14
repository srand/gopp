// Ported from golang.org/x/net/dns/dnsmessage's message_test.go (v0.58.0,
// whose message.go is the one Go 1.27.3 vendors), with Go's test names.
//
// Left out: the tests of unexported helpers (printUint16 and friends, and
// the ones that build a Parser's fields by hand), the GoString and
// allocation tests, and the SVCB/HTTPS cases (go++ parses those records as
// UnknownResource). reflect.DeepEqual of messages is a comparison of their
// fields, with resource bodies compared by their packed form.
#include "net/dnsmessage.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace gopp;
using namespace gopp::net::dnsmessage;

namespace {

constexpr Type privateUseType = Type(65362);

ResourceHeader mustEDNS0ResourceHeader(int l, RCode extrc, bool do_) {
    ResourceHeader h;
    h.Class = ClassINET;
    if (error err = h.SetEDNS0(l, extrc, do_)) panic(err.Error().view());
    return h;
}

ResourceHeader rh(const Name& name, Type t) {
    ResourceHeader h;
    h.Name = name;
    h.Type = t;
    h.Class = ClassINET;
    return h;
}

Resource res(ResourceHeader h, ptr<ResourceBody> body) { return Resource{h, std::move(body)}; }

std::string bodyBytes(const ptr<ResourceBody>& b) {
    if (!b) return "<nil>";
    slice<byte> msg;
    if (error err = b->pack(msg, nullptr, 0)) return "error: " + std::string(err.Error().view());
    return std::to_string(b->realType()) + ":" + std::string(asStringView(msg));
}

bool headerEqual(const ResourceHeader& a, const ResourceHeader& b) {
    return a.Name == b.Name && a.Type == b.Type && a.Class == b.Class && a.TTL == b.TTL && a.Length == b.Length;
}

bool resourcesEqual(const std::vector<Resource>& a, const std::vector<Resource>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); i++) {
        if (!headerEqual(a[i].Header, b[i].Header) || bodyBytes(a[i].Body) != bodyBytes(b[i].Body)) return false;
    }
    return true;
}

bool messageEqual(const Message& a, const Message& b) {
    auto [ida, bitsa] = a.Header.pack();
    auto [idb, bitsb] = b.Header.pack();
    if (ida != idb || bitsa != bitsb || a.Questions.size() != b.Questions.size()) return false;
    for (std::size_t i = 0; i < a.Questions.size(); i++) {
        if (!(a.Questions[i].Name == b.Questions[i].Name) || a.Questions[i].Type != b.Questions[i].Type ||
            a.Questions[i].Class != b.Questions[i].Class) {
            return false;
        }
    }
    return resourcesEqual(a.Answers, b.Answers) && resourcesEqual(a.Authorities, b.Authorities) &&
           resourcesEqual(a.Additionals, b.Additionals);
}

Question question(std::string_view name, Type t) {
    Question q;
    q.Name = MustNewName(name);
    q.Type = t;
    q.Class = ClassINET;
    return q;
}

slice<byte> bytesOf(std::initializer_list<byte> b) { return slice<byte>(b); }

Message smallTestMsg() {
    Name name = MustNewName("example.com.");
    Message m;
    m.Header.Response = true;
    m.Header.Authoritative = true;
    m.Questions = {question("example.com.", TypeA)};
    m.Answers = {res(rh(name, TypeA), make_ptr<AResource>(std::array<byte, 4>{127, 0, 0, 1}))};
    m.Authorities = {res(rh(name, TypeA), make_ptr<AResource>(std::array<byte, 4>{127, 0, 0, 1}))};
    m.Additionals = {res(rh(name, TypeA), make_ptr<AResource>(std::array<byte, 4>{127, 0, 0, 1}))};
    return m;
}

Message largeTestMsg() {
    Name name = MustNewName("foo.bar.example.com.");
    Message m;
    m.Header.Response = true;
    m.Header.Authoritative = true;
    m.Questions = {question("foo.bar.example.com.", TypeA)};
    auto soa = make_ptr<SOAResource>();
    soa->NS = MustNewName("ns1.example.com.");
    soa->MBox = MustNewName("mb.example.com.");
    soa->Serial = 1;
    soa->Refresh = 2;
    soa->Retry = 3;
    soa->Expire = 4;
    soa->MinTTL = 5;
    auto srv = make_ptr<SRVResource>();
    srv->Priority = 8;
    srv->Weight = 9;
    srv->Port = 11;
    srv->Target = MustNewName("srv.example.com.");
    auto unknown = make_ptr<UnknownResource>();
    unknown->Type = privateUseType;
    unknown->Data = bytesOf({42, 0, 43, 44});
    m.Answers = {
        res(rh(name, TypeA), make_ptr<AResource>(std::array<byte, 4>{127, 0, 0, 1})),
        res(rh(name, TypeA), make_ptr<AResource>(std::array<byte, 4>{127, 0, 0, 2})),
        res(rh(name, TypeAAAA),
            make_ptr<AAAAResource>(std::array<byte, 16>{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16})),
        res(rh(name, TypeCNAME), make_ptr<CNAMEResource>(MustNewName("alias.example.com."))),
        res(rh(name, TypeSOA), soa),
        res(rh(name, TypePTR), make_ptr<PTRResource>(MustNewName("ptr.example.com."))),
        res(rh(name, TypeMX), make_ptr<MXResource>(7, MustNewName("mx.example.com."))),
        res(rh(name, TypeSRV), srv),
        res(rh(name, privateUseType), unknown),
    };
    m.Authorities = {
        res(rh(name, TypeNS), make_ptr<NSResource>(MustNewName("ns1.example.com."))),
        res(rh(name, TypeNS), make_ptr<NSResource>(MustNewName("ns2.example.com."))),
    };
    auto opt = make_ptr<OPTResource>();
    opt->Options = {Option{10, bytesOf({0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef})}};  // RFC 7873
    m.Additionals = {
        res(rh(name, TypeTXT), make_ptr<TXTResource>(std::vector<std::string>{"So Long, and Thanks for All the Fish"})),
        res(rh(name, TypeTXT), make_ptr<TXTResource>(std::vector<std::string>{"Hamster Huey and the Gooey Kablooie"})),
        res(mustEDNS0ResourceHeader(4096, RCode(0xfe0 | RCodeSuccess), false), opt),
    };
    return m;
}

Message smallTestMsgWithUnknownResource() {
    Message m;
    ResourceHeader h = rh(MustNewName("."), privateUseType);
    h.TTL = 123;
    auto u = make_ptr<UnknownResource>();
    u->Type = privateUseType;
    u->Data = bytesOf({42, 42, 42, 42});
    m.Answers = {res(h, u)};
    return m;
}

}  // namespace

TEST(GoDNSMessage, NameString) {
    const std::string want = "foo";
    EXPECT_EQ(MustNewName(want).String(), want);
}

TEST(GoDNSMessage, Name) {
    for (std::string test : {"", ".", "google..com", "google.com", "google..com.", "google.com.", ".google.com.",
                             "www..google.com.", "www.google.com."}) {
        auto [n, err] = NewName(test);
        ASSERT_FALSE(err) << test;
        EXPECT_EQ(n.String(), test);
    }
}

TEST(GoDNSMessage, NamePackUnpack) {
    // Through a question of a message, the public way to pack a name.
    const std::string suffix = ".go.dev.";
    std::string longDNSPrefix;
    for (int i = 0; i < 20; i++) longDNSPrefix += "verylongdomainlabel.";
    struct {
        std::string in;
        bool ok;
    } tests[] = {
        {"", false},
        {".", true},
        {"google..com", false},
        {"google.com", false},
        {"google..com.", false},
        {"google.com.", true},
        {".google.com.", false},
        {"www..google.com.", false},
        {"www.google.com.", true},
        {longDNSPrefix.substr(0, 254 - suffix.size()) + suffix, true},   // 254B name, with ending dot
        {longDNSPrefix.substr(0, 255 - suffix.size()) + suffix, false},  // 255B name, with ending dot
    };
    for (auto& test : tests) {
        Message m;
        m.Questions = {question(test.in, TypeA)};
        auto [buf, err] = m.Pack();
        EXPECT_EQ(!err, test.ok) << test.in << ": " << err;
        if (err) continue;
        Message got;
        ASSERT_FALSE(got.Unpack(buf)) << test.in;
        ASSERT_EQ(got.Questions.size(), 1u);
        EXPECT_EQ(got.Questions[0].Name.String(), test.in);
    }
}

TEST(GoDNSMessage, ParserStart) {
    Parser p;
    for (int i = 0; i <= 1; i++) {
        auto [h, err] = p.Start(slice<byte>::make(0));
        ASSERT_TRUE(err);
        EXPECT_TRUE(err.Error().view().starts_with("unpacking header: ")) << err;
    }
}

TEST(GoDNSMessage, ResourceNotStarted) {
    Parser p;
    EXPECT_EQ(p.CNAMEResource().second, ErrNotStarted);
    EXPECT_EQ(p.MXResource().second, ErrNotStarted);
    EXPECT_EQ(p.NSResource().second, ErrNotStarted);
    EXPECT_EQ(p.PTRResource().second, ErrNotStarted);
    EXPECT_EQ(p.SOAResource().second, ErrNotStarted);
    EXPECT_EQ(p.TXTResource().second, ErrNotStarted);
    EXPECT_EQ(p.SRVResource().second, ErrNotStarted);
    EXPECT_EQ(p.AResource().second, ErrNotStarted);
    EXPECT_EQ(p.AAAAResource().second, ErrNotStarted);
    EXPECT_EQ(p.UnknownResource().second, ErrNotStarted);
}

TEST(GoDNSMessage, DNSPackUnpack) {
    Message small;
    small.Questions = {question(".", TypeAAAA)};
    std::vector<Message> wants = {small, largeTestMsg()};
    for (std::size_t i = 0; i < wants.size(); i++) {
        auto& want = wants[i];
        auto [b, err] = want.Pack();
        ASSERT_FALSE(err) << i << ": " << err;
        Message got;
        ASSERT_FALSE(got.Unpack(b)) << i;
        EXPECT_TRUE(messageEqual(got, want)) << i << ": Message.Pack/Unpack() roundtrip";
    }
}

TEST(GoDNSMessage, DNSAppendPackUnpack) {
    Message small;
    small.Questions = {question(".", TypeAAAA)};
    std::vector<Message> wants = {small, largeTestMsg()};
    for (std::size_t i = 0; i < wants.size(); i++) {
        auto& want = wants[i];
        auto [b, err] = want.AppendPack(slice<byte>::make(2, 514));
        ASSERT_FALSE(err) << i << ": " << err;
        Message got;
        ASSERT_FALSE(got.Unpack(b.Sub(2))) << i;
        EXPECT_TRUE(messageEqual(got, want)) << i << ": Message.AppendPack/Unpack() roundtrip";
    }
}

TEST(GoDNSMessage, SkipAll) {
    auto msg = largeTestMsg();
    auto [buf, err] = msg.Pack();
    ASSERT_FALSE(err);
    Parser p;
    ASSERT_FALSE(p.Start(buf).second);
    for (int i = 1; i <= 3; i++) EXPECT_FALSE(p.SkipAllQuestions()) << i;
    for (int i = 1; i <= 3; i++) EXPECT_FALSE(p.SkipAllAnswers()) << i;
    for (int i = 1; i <= 3; i++) EXPECT_FALSE(p.SkipAllAuthorities()) << i;
    for (int i = 1; i <= 3; i++) EXPECT_FALSE(p.SkipAllAdditionals()) << i;
}

TEST(GoDNSMessage, SkipEach) {
    auto msg = smallTestMsg();
    auto [buf, err] = msg.Pack();
    ASSERT_FALSE(err);
    Parser p;
    ASSERT_FALSE(p.Start(buf).second);
    EXPECT_FALSE(p.SkipQuestion());
    EXPECT_EQ(p.SkipQuestion(), ErrSectionDone);
    EXPECT_FALSE(p.SkipAnswer());
    EXPECT_EQ(p.SkipAnswer(), ErrSectionDone);
    EXPECT_FALSE(p.SkipAuthority());
    EXPECT_EQ(p.SkipAuthority(), ErrSectionDone);
    EXPECT_FALSE(p.SkipAdditional());
    EXPECT_EQ(p.SkipAdditional(), ErrSectionDone);
}

TEST(GoDNSMessage, SkipAfterRead) {
    auto msg = smallTestMsg();
    auto [buf, err] = msg.Pack();
    ASSERT_FALSE(err);
    Parser p;
    ASSERT_FALSE(p.Start(buf).second);
    EXPECT_FALSE(p.Question().second);
    EXPECT_EQ(p.SkipQuestion(), ErrSectionDone);
    EXPECT_FALSE(p.Answer().second);
    EXPECT_EQ(p.SkipAnswer(), ErrSectionDone);
    EXPECT_FALSE(p.Authority().second);
    EXPECT_EQ(p.SkipAuthority(), ErrSectionDone);
    EXPECT_FALSE(p.Additional().second);
    EXPECT_EQ(p.SkipAdditional(), ErrSectionDone);
}

TEST(GoDNSMessage, SkipNotStarted) {
    Parser p;
    EXPECT_EQ(p.SkipAllQuestions(), ErrNotStarted);
    EXPECT_EQ(p.SkipAllAnswers(), ErrNotStarted);
    EXPECT_EQ(p.SkipAllAuthorities(), ErrNotStarted);
    EXPECT_EQ(p.SkipAllAdditionals(), ErrNotStarted);
}

TEST(GoDNSMessage, TooManyRecords) {
    constexpr std::size_t recs = std::size_t(uint16(~0)) + 1;
    {
        Message m;
        m.Questions.resize(recs);
        EXPECT_EQ(m.Pack().second.Error().view(), "too many Questions to pack (>65535)");
    }
    {
        Message m;
        m.Answers.resize(recs);
        EXPECT_EQ(m.Pack().second.Error().view(), "too many Answers to pack (>65535)");
    }
    {
        Message m;
        m.Authorities.resize(recs);
        EXPECT_EQ(m.Pack().second.Error().view(), "too many Authorities to pack (>65535)");
    }
    {
        Message m;
        m.Additionals.resize(recs);
        EXPECT_EQ(m.Pack().second.Error().view(), "too many Additionals to pack (>65535)");
    }
}

TEST(GoDNSMessage, VeryLongTxt) {
    Message want;
    want.Answers = {res(rh(MustNewName("foo.bar.example.com."), TypeTXT),
                        make_ptr<TXTResource>(std::vector<std::string>{"", "", "foo bar", "", "www.example.com",
                                                                       "www.example.com.", std::string(255, '.')}))};
    auto [buf, err] = want.Pack();
    ASSERT_FALSE(err) << err;
    Message got;
    ASSERT_FALSE(got.Unpack(buf));
    EXPECT_TRUE(messageEqual(got, want));
}

TEST(GoDNSMessage, TooLongTxt) {
    TXTResource rb(std::vector<std::string>{std::string(256, '.')});
    slice<byte> msg;
    error err = rb.pack(msg, nullptr, 0);
    ASSERT_TRUE(err);
    EXPECT_EQ(err.Error().view(), "character string exceeds maximum length (255)");
}

TEST(GoDNSMessage, StartAppends) {
    auto buf = slice<byte>::make(2, 514);
    buf[0] = 4;
    buf[1] = 44;
    auto b = NewBuilder(buf, Header{});
    b.EnableCompression();
    auto [out, err] = b.Finish();
    ASSERT_FALSE(err);
    EXPECT_EQ(out.size(), 12u + 2);
    EXPECT_EQ(out[0], 4);
    EXPECT_EQ(out[1], 44);
}

TEST(GoDNSMessage, StartError) {
    Builder notStarted;  // sectionNotStarted
    EXPECT_EQ(notStarted.StartQuestions(), ErrNotStarted);
    EXPECT_EQ(notStarted.StartAnswers(), ErrNotStarted);
    EXPECT_EQ(notStarted.StartAuthorities(), ErrNotStarted);
    EXPECT_EQ(notStarted.StartAdditionals(), ErrNotStarted);
    auto done = NewBuilder({}, Header{});
    done.Finish();  // sectionDone
    EXPECT_EQ(done.StartQuestions(), ErrSectionDone);
    EXPECT_EQ(done.StartAnswers(), ErrSectionDone);
    EXPECT_EQ(done.StartAuthorities(), ErrSectionDone);
    EXPECT_EQ(done.StartAdditionals(), ErrSectionDone);
}

TEST(GoDNSMessage, BuilderResourceError) {
    auto check = [](Builder& b, const error& want) {
        EXPECT_EQ(b.AResource(ResourceHeader{}, AResource{}), want);
        EXPECT_EQ(b.AAAAResource(ResourceHeader{}, AAAAResource{}), want);
        EXPECT_EQ(b.CNAMEResource(ResourceHeader{}, CNAMEResource{}), want);
        EXPECT_EQ(b.MXResource(ResourceHeader{}, MXResource{}), want);
        EXPECT_EQ(b.NSResource(ResourceHeader{}, NSResource{}), want);
        EXPECT_EQ(b.OPTResource(ResourceHeader{}, OPTResource{}), want);
        EXPECT_EQ(b.PTRResource(ResourceHeader{}, PTRResource{}), want);
        EXPECT_EQ(b.SOAResource(ResourceHeader{}, SOAResource{}), want);
        EXPECT_EQ(b.SRVResource(ResourceHeader{}, SRVResource{}), want);
        EXPECT_EQ(b.TXTResource(ResourceHeader{}, TXTResource{}), want);
        EXPECT_EQ(b.UnknownResource(ResourceHeader{}, UnknownResource{}), want);
    };
    Builder notStarted;
    check(notStarted, ErrNotStarted);
    auto header = NewBuilder({}, Header{});  // sectionHeader
    check(header, ErrNotStarted);
    auto questions = NewBuilder({}, Header{});
    questions.StartQuestions();
    check(questions, ErrNotStarted);
    auto done = NewBuilder({}, Header{});
    done.Finish();
    check(done, ErrSectionDone);
}

TEST(GoDNSMessage, FinishError) {
    Builder b;
    EXPECT_EQ(b.Finish().second, ErrNotStarted);
}

TEST(GoDNSMessage, Builder) {
    auto msg = largeTestMsg();
    auto [want, err] = msg.Pack();
    ASSERT_FALSE(err) << err;
    auto b = NewBuilder({}, msg.Header);
    b.EnableCompression();
    ASSERT_FALSE(b.StartQuestions());
    for (auto& q : msg.Questions) ASSERT_FALSE(b.Question(q));
    ASSERT_FALSE(b.StartAnswers());
    for (auto& a : msg.Answers) {
        error e;
        switch (a.Header.Type) {
        case TypeA: e = b.AResource(a.Header, *static_cast<AResource*>(a.Body.get())); break;
        case TypeNS: e = b.NSResource(a.Header, *static_cast<NSResource*>(a.Body.get())); break;
        case TypeCNAME: e = b.CNAMEResource(a.Header, *static_cast<CNAMEResource*>(a.Body.get())); break;
        case TypeSOA: e = b.SOAResource(a.Header, *static_cast<SOAResource*>(a.Body.get())); break;
        case TypePTR: e = b.PTRResource(a.Header, *static_cast<PTRResource*>(a.Body.get())); break;
        case TypeMX: e = b.MXResource(a.Header, *static_cast<MXResource*>(a.Body.get())); break;
        case TypeTXT: e = b.TXTResource(a.Header, *static_cast<TXTResource*>(a.Body.get())); break;
        case TypeAAAA: e = b.AAAAResource(a.Header, *static_cast<AAAAResource*>(a.Body.get())); break;
        case TypeSRV: e = b.SRVResource(a.Header, *static_cast<SRVResource*>(a.Body.get())); break;
        default: e = b.UnknownResource(a.Header, *static_cast<UnknownResource*>(a.Body.get())); break;
        }
        ASSERT_FALSE(e) << e;
    }
    ASSERT_FALSE(b.StartAuthorities());
    for (auto& a : msg.Authorities) ASSERT_FALSE(b.NSResource(a.Header, *static_cast<NSResource*>(a.Body.get())));
    ASSERT_FALSE(b.StartAdditionals());
    for (auto& a : msg.Additionals) {
        if (auto* t = dynamic_cast<TXTResource*>(a.Body.get())) {
            ASSERT_FALSE(b.TXTResource(a.Header, *t));
        }
        if (auto* o = dynamic_cast<OPTResource*>(a.Body.get())) {
            ASSERT_FALSE(b.OPTResource(a.Header, *o));
        }
    }
    auto [got, ferr] = b.Finish();
    ASSERT_FALSE(ferr);
    EXPECT_EQ(asStringView(got), asStringView(want));
}

TEST(GoDNSMessage, ResourcePack) {
    {
        Message m;
        m.Questions = {question(".", TypeAAAA)};
        m.Answers = {Resource{ResourceHeader{}, nullptr}};
        EXPECT_EQ(m.Pack().second.Error().view(), "packing Answer: nil resource body");
    }
    {
        Message m;
        m.Questions = {question(".", TypeAAAA)};
        m.Authorities = {Resource{ResourceHeader{}, make_ptr<NSResource>()}};
        EXPECT_EQ(m.Pack().second.Error().view(),
                  "packing Authority: ResourceHeader: Name: name is not in canonical format (it must end with a .)");
    }
    {
        Message m;
        m.Questions = {question(".", TypeA)};
        m.Additionals = {Resource{ResourceHeader{}, nullptr}};
        EXPECT_EQ(m.Pack().second.Error().view(), "packing Additional: nil resource body");
    }
}

TEST(GoDNSMessage, OptionPackUnpack) {
    struct {
        std::string name;
        std::vector<byte> w;  // wire format of m.Additionals
        Message m;
        bool dnssecOK;
        RCode extRCode;
    } tests[3];
    tests[0].name = "without EDNS(0) options";
    tests[0].w = {0x00, 0x00, 0x29, 0x10, 0x00, 0xfe, 0x00, 0x80, 0x00, 0x00, 0x00};
    tests[0].m.Header.RCode = RCodeFormatError;
    tests[0].m.Questions = {question(".", TypeA)};
    tests[0].m.Additionals = {res(mustEDNS0ResourceHeader(4096, RCode(0xfe0 | RCodeFormatError), true),
                                  make_ptr<OPTResource>())};
    tests[0].dnssecOK = true;
    tests[0].extRCode = RCode(0xfe0 | RCodeFormatError);

    tests[1].name = "with EDNS(0) options";
    tests[1].w = {0x00, 0x00, 0x29, 0x10, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00, 0x0c, 0x00,
                  0x0c, 0x00, 0x02, 0x00, 0x00, 0x00, 0x0b, 0x00, 0x02, 0x12, 0x34};
    tests[1].m.Header.RCode = RCodeServerFailure;
    tests[1].m.Questions = {question(".", TypeAAAA)};
    auto opt1 = make_ptr<OPTResource>();
    opt1->Options = {Option{12, bytesOf({0x00, 0x00})}, Option{11, bytesOf({0x12, 0x34})}};
    tests[1].m.Additionals = {res(mustEDNS0ResourceHeader(4096, RCode(0xff0 | RCodeServerFailure), false), opt1)};
    tests[1].dnssecOK = false;
    tests[1].extRCode = RCode(0xff0 | RCodeServerFailure);

    tests[2].name = "with multiple OPT resources";
    tests[2].w = {0x00, 0x00, 0x29, 0x10, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00, 0x06, 0x00, 0x0b, 0x00, 0x02, 0x12, 0x34,
                  0x00, 0x00, 0x29, 0x10, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00, 0x06, 0x00, 0x0c, 0x00, 0x02, 0x00, 0x00};
    tests[2].m.Header.RCode = RCodeNameError;
    tests[2].m.Questions = {question(".", TypeAAAA)};
    auto opt2a = make_ptr<OPTResource>();
    opt2a->Options = {Option{11, bytesOf({0x12, 0x34})}};
    auto opt2b = make_ptr<OPTResource>();
    opt2b->Options = {Option{12, bytesOf({0x00, 0x00})}};
    tests[2].m.Additionals = {res(mustEDNS0ResourceHeader(4096, RCode(0xff0 | RCodeNameError), false), opt2a),
                              res(mustEDNS0ResourceHeader(4096, RCode(0xff0 | RCodeNameError), false), opt2b)};
    tests[2].dnssecOK = false;
    tests[2].extRCode = RCode(0xff0 | RCodeNameError);

    for (auto& tt : tests) {
        auto [w, err] = tt.m.Pack();
        ASSERT_FALSE(err) << tt.name << ": " << err;
        ASSERT_GE(w.size(), tt.w.size());
        std::string_view tail = asStringView(w).substr(w.size() - tt.w.size());
        EXPECT_EQ(tail, asStringView(tt.w)) << tt.name;
        Message m;
        ASSERT_FALSE(m.Unpack(w)) << tt.name;
        EXPECT_TRUE(resourcesEqual(m.Additionals, tt.m.Additionals)) << tt.name;
        EXPECT_EQ(m.Additionals[0].Header.DNSSECAllowed(), tt.dnssecOK) << tt.name;
        EXPECT_EQ(m.Additionals[0].Header.ExtendedRCode(m.Header.RCode), tt.extRCode) << tt.name;
    }
}

TEST(GoDNSMessage, UnknownPackUnpack) {
    auto msg = smallTestMsgWithUnknownResource();
    auto [packed, err] = msg.Pack();
    ASSERT_FALSE(err) << err;
    Message received;
    ASSERT_FALSE(received.Unpack(packed));
    ASSERT_EQ(received.Answers.size(), 1u);
    auto* u = dynamic_cast<UnknownResource*>(received.Answers[0].Body.get());
    ASSERT_TRUE(u) << "parsed something other than an UnknownResource";
    EXPECT_EQ(bodyBytes(received.Answers[0].Body), bodyBytes(msg.Answers[0].Body));
    EXPECT_EQ(u->Type, privateUseType);
}

TEST(GoDNSMessage, ParseUnknownResource) {
    auto msg = smallTestMsgWithUnknownResource();
    auto [packed, err] = msg.Pack();
    ASSERT_FALSE(err);
    Parser p;
    ASSERT_FALSE(p.Start(packed).second);
    ASSERT_FALSE(p.AllQuestions().second);
    auto [parsedHeader, herr] = p.AnswerHeader();
    ASSERT_FALSE(herr);
    EXPECT_TRUE(headerEqual(parsedHeader, msg.Answers[0].Header));
    auto [u, uerr] = p.UnknownResource();
    ASSERT_FALSE(uerr);
    EXPECT_EQ(bodyBytes(make_ptr<UnknownResource>(u)), bodyBytes(msg.Answers[0].Body));
    // The parser is left in a consistent state.
    EXPECT_EQ(p.AnswerHeader().second, ErrSectionDone);
    EXPECT_FALSE(p.AllAuthorities().second);
    EXPECT_FALSE(p.AllAdditionals().second);
}

TEST(GoDNSMessage, ParseResourceHeaderMultipleTimes) {
    Message msg;
    msg.Header.Response = true;
    msg.Header.Authoritative = true;
    msg.Answers = {res(rh(MustNewName("go.dev."), TypeA), make_ptr<AResource>(std::array<byte, 4>{127, 0, 0, 1}))};
    msg.Authorities = {res(rh(MustNewName("go.dev."), TypeA), make_ptr<AResource>(std::array<byte, 4>{127, 0, 0, 1}))};
    auto [raw, err] = msg.Pack();
    ASSERT_FALSE(err);
    Parser p;
    ASSERT_FALSE(p.Start(raw).second);
    ASSERT_FALSE(p.SkipAllQuestions());
    auto [hdr1, e1] = p.AnswerHeader();
    ASSERT_FALSE(e1);
    auto [hdr2, e2] = p.AnswerHeader();
    ASSERT_FALSE(e2);
    EXPECT_TRUE(headerEqual(hdr1, hdr2));
    ASSERT_FALSE(p.AResource().second);
    EXPECT_EQ(p.AnswerHeader().second, ErrSectionDone);
    auto [hdr3, e3] = p.AuthorityHeader();
    ASSERT_FALSE(e3);
    auto [hdr4, e4] = p.AuthorityHeader();
    ASSERT_FALSE(e4);
    EXPECT_TRUE(headerEqual(hdr3, hdr4));
    ASSERT_FALSE(p.AResource().second);
    EXPECT_EQ(p.AuthorityHeader().second, ErrSectionDone);
}

TEST(GoDNSMessage, ParseDifferentResourceHeadersWithoutParsingRData) {
    auto msg = smallTestMsg();
    auto [raw, err] = msg.Pack();
    ASSERT_FALSE(err);
    Parser p;
    ASSERT_FALSE(p.Start(raw).second);
    ASSERT_FALSE(p.SkipAllQuestions());
    ASSERT_FALSE(p.AnswerHeader().second);
    EXPECT_TRUE(p.AdditionalHeader().second) << "p.AdditionalHeader() unexpected success";
    EXPECT_TRUE(p.AuthorityHeader().second) << "p.AuthorityHeader() unexpected success";
}

TEST(GoDNSMessage, ParseWrongSection) {
    auto msg = smallTestMsg();
    auto [raw, err] = msg.Pack();
    ASSERT_FALSE(err);
    Parser p;
    ASSERT_FALSE(p.Start(raw).second);
    ASSERT_FALSE(p.SkipAllQuestions());
    ASSERT_FALSE(p.AnswerHeader().second);
    EXPECT_TRUE(p.AuthorityHeader().second);
    EXPECT_TRUE(p.SkipAuthority());
    EXPECT_TRUE(p.SkipAllAuthorities());
}

TEST(GoDNSMessage, BuilderNameCompressionWithNonZeroedName) {
    auto b = NewBuilder({}, Header{});
    b.EnableCompression();
    ASSERT_FALSE(b.StartQuestions());
    Name name = MustNewName("go.dev.");
    Question q;
    q.Name = name;
    q.Type = Type(0);
    q.Class = Class(0);
    ASSERT_FALSE(b.Question(q));
    q.Name.Data[q.Name.Length] = '1';
    ASSERT_FALSE(b.Question(q));
    auto [msg, err] = b.Finish();
    ASSERT_FALSE(err);
    std::vector<byte> expect = {
        0, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0,               // header
        2, 'g', 'o', 3, 'd', 'e', 'v', 0, 0, 0, 0, 0,  // question 1
        0xC0, 12, 0, 0, 0, 0,                            // question 2
    };
    EXPECT_EQ(asStringView(msg), asStringView(expect));
}

TEST(GoDNSMessage, BuilderCompressionInAppendMode) {
    std::size_t maxPtr = uint16(~0) >> 2;
    auto b = NewBuilder(slice<byte>::make(maxPtr, maxPtr + 512), Header{});
    b.EnableCompression();
    ASSERT_FALSE(b.StartQuestions());
    Question q;
    q.Name = MustNewName("go.dev.");
    q.Type = Type(0);
    q.Class = Class(0);
    ASSERT_FALSE(b.Question(q));
    ASSERT_FALSE(b.Question(q));
    auto [msg, err] = b.Finish();
    ASSERT_FALSE(err);
    std::vector<byte> expect = {
        0, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0,               // header
        2, 'g', 'o', 3, 'd', 'e', 'v', 0, 0, 0, 0, 0,  // question 1
        0xC0, 12, 0, 0, 0, 0,                            // question 2
    };
    EXPECT_EQ(asStringView(msg.Sub(maxPtr)), asStringView(expect));
}
