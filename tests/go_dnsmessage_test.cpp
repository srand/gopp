// Ported from golang.org/x/net/dns/dnsmessage's message_test.go (v0.58.0,
// whose message.go is the one Go 1.27.3 vendors), with Go's test names.
//
// Left out: the tests of unexported helpers (printUint16 and friends, and
// the ones that build a Parser's fields by hand), the GoString and
// allocation tests. svcb_test.go's tests are here too (from v0.60.0, which
// adds TestSVCBUnpackOutOfBounds), through whole messages where Go's call the
// unexported pack and unpack functions. reflect.DeepEqual of messages is a
// comparison of their fields, with resource bodies compared by their packed
// form.
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

Message buildTestSVCBMsg() {
    auto svcb = make_ptr<SVCBResource>();
    svcb->Priority = 1;
    svcb->Target = MustNewName("svc.example.com.");
    svcb->Params = {SVCParam{SVCParamALPN, bytesOf({'h', '2'})}};
    auto https = make_ptr<HTTPSResource>();
    https->Priority = 2;
    https->Target = MustNewName("https.example.com.");
    https->Params = {SVCParam{SVCParamPort, bytesOf({0x01, 0xbb})}, SVCParam{SVCParamIPv4Hint, bytesOf({192, 0, 2, 1})}};
    Message m;
    m.Answers = {
        res(rh(MustNewName("foo.bar.example.com."), TypeSVCB), svcb),
        res(rh(MustNewName("foo.bar.example.com."), TypeHTTPS), https),
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
    EXPECT_EQ(p.SVCBResource().second, ErrNotStarted);
    EXPECT_EQ(p.HTTPSResource().second, ErrNotStarted);
    EXPECT_EQ(p.UnknownResource().second, ErrNotStarted);
}

TEST(GoDNSMessage, DNSPackUnpack) {
    Message small;
    small.Questions = {question(".", TypeAAAA)};
    std::vector<Message> wants = {small, largeTestMsg(), buildTestSVCBMsg()};
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
    std::vector<Message> wants = {small, largeTestMsg(), buildTestSVCBMsg()};
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
        EXPECT_EQ(b.SVCBResource(ResourceHeader{}, SVCBResource{}), want);
        EXPECT_EQ(b.HTTPSResource(ResourceHeader{}, HTTPSResource{}), want);
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

// ---------------------------------------------------------------------------
// svcb_test.go

namespace {

// A message whose one answer has the SVCB record body, as on the wire.
slice<byte> svcbMessage(const std::vector<byte>& body, Type t = TypeSVCB) {
    std::vector<byte> m = {0x00, 0x01, 0x84, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,  // header, 1 answer
                           0x00,                                                                  // name "."
                           byte(t >> 8), byte(t), 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
                           byte(body.size() >> 8), byte(body.size())};
    m.insert(m.end(), body.begin(), body.end());
    return slice<byte>::of(m);
}

std::string packBody(const ResourceBody& r) {
    slice<byte> out;
    if (error err = r.pack(out, nullptr, 0)) {
        ADD_FAILURE() << "pack() = " << err;
        return {};
    }
    return std::string(asStringView(out));
}

std::string paramsString(const std::vector<SVCParam>& ps) {
    std::string s;
    for (auto& p : ps) s += std::to_string(p.Key) + "=" + std::string(asStringView(p.Value)) + ";";
    return s;
}

}  // namespace

TEST(GoDNSMessage, SVCBParamsRoundTrip) {
    std::vector<SVCParam> params = {
        {SVCParamMandatory, bytesOf({0x00, 0x01, 0x00, 0x03, 0x00, 0x05})},
        {SVCParamALPN, bytesOf({0x02, 'h', '2', 0x02, 'h', '3'})},
        {SVCParamNoDefaultALPN, bytesOf({})},
        {SVCParamPort, bytesOf({0x1f, 0x90})},  // 8080
        {SVCParamIPv4Hint, bytesOf({192, 0, 2, 1, 198, 51, 100, 2})},
        {SVCParamECH, bytesOf({0x01, 0x02, 0x03, 0x04})},
        {SVCParamIPv6Hint, bytesOf({0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01})},
        {SVCParamDOHPath, slice<byte>::of(as_bytes("/dns-query{?dns}"))},
        {SVCParamOHTTP, bytesOf({0x00, 0x01, 0x02, 0x03})},
        {SVCParamTLSSupportedGroups, bytesOf({0x00, 0x1d, 0x00, 0x17})},
    };
    for (auto& p : params) {
        SVCBResource rr;
        rr.Priority = 1;
        rr.Target = MustNewName("svc.example.com.");
        rr.Params = {p};
        std::string packed = packBody(rr);
        Message got;
        ASSERT_FALSE(got.Unpack(svcbMessage(std::vector<byte>(packed.begin(), packed.end())))) << p.Key;
        auto* r = dynamic_cast<SVCBResource*>(got.Answers.at(0).Body.get());
        ASSERT_TRUE(r) << p.Key;
        EXPECT_EQ(r->Priority, 1);
        EXPECT_EQ(r->Target.String(), "svc.example.com.");
        EXPECT_EQ(paramsString(r->Params), paramsString(rr.Params));
    }
}

TEST(GoDNSMessage, SVCBParams) {
    SVCBResource rr;
    rr.Priority = 1;
    rr.Target = MustNewName("svc.example.com.");
    EXPECT_FALSE(rr.GetParam(SVCParamALPN).second) << "GetParam found non-existent param";
    rr.SetParam(SVCParamIPv4Hint, bytesOf({192, 0, 2, 1}));
    auto inALPN = bytesOf({0x02, 'h', '2', 0x02, 'h', '3'});
    rr.SetParam(SVCParamALPN, inALPN);

    // The params are sorted.
    std::vector<byte> expected = {
        0x00, 0x01,                                                                                     // priority
        0x03, 0x73, 0x76, 0x63, 0x07, 0x65, 0x78, 0x61, 0x6d, 0x70, 0x6c, 0x65, 0x03, 0x63, 0x6f, 0x6d, 0x00,  // target
        0x00, 0x01, 0x00, 0x06, 0x02, 'h', '2', 0x02, 'h', '3',                                          // key 1
        0x00, 0x04, 0x00, 0x04, 192, 0, 2, 1,                                                            // key 4
    };
    EXPECT_EQ(packBody(rr), std::string(expected.begin(), expected.end()));

    auto [outALPN, ok] = rr.GetParam(SVCParamALPN);
    EXPECT_TRUE(ok && asStringView(outALPN) == asStringView(inALPN)) << "GetParam failed to retrieve set param";
    EXPECT_TRUE(rr.DeleteParam(SVCParamALPN)) << "DeleteParam failed to remove existing param";
    EXPECT_FALSE(rr.GetParam(SVCParamALPN).second) << "GetParam found deleted param";
    ASSERT_EQ(rr.Params.size(), 1u);
    EXPECT_EQ(rr.Params[0].Key, SVCParamIPv4Hint) << "DeleteParam removed wrong param";
}

TEST(GoDNSMessage, SVCBWireFormat) {
    // RFC 9460's test vectors.
    auto testRecord = [](const std::vector<byte>& bytes, uint16 priority, std::string_view target, std::vector<SVCParam> params) {
        Message got;
        ASSERT_FALSE(got.Unpack(svcbMessage(bytes)));
        auto* r = dynamic_cast<SVCBResource*>(got.Answers.at(0).Body.get());
        ASSERT_TRUE(r);
        EXPECT_EQ(r->Priority, priority);
        EXPECT_EQ(r->Target.String(), target);
        EXPECT_EQ(paramsString(r->Params), paramsString(params));
        SVCBResource in;
        in.Priority = priority;
        in.Target = MustNewName(target);
        in.Params = params;
        EXPECT_EQ(packBody(in), std::string(bytes.begin(), bytes.end()));
    };
    const std::vector<byte> fooExampleCom = {0x03, 'f', 'o', 'o', 0x07, 'e', 'x', 'a', 'm', 'p', 'l', 'e', 0x03, 'c', 'o', 'm', 0x00};
    const std::vector<byte> fooExampleOrg = {0x03, 'f', 'o', 'o', 0x07, 'e', 'x', 'a', 'm', 'p', 'l', 'e', 0x03, 'o', 'r', 'g', 0x00};
    auto cat = [](std::initializer_list<std::vector<byte>> parts) {
        std::vector<byte> out;
        for (auto& p : parts) out.insert(out.end(), p.begin(), p.end());
        return out;
    };

    // Figure 2: AliasMode: example.com. HTTPS 0 foo.example.com.
    testRecord(cat({{0x00, 0x00}, fooExampleCom}), 0, "foo.example.com.", {});
    // Figure 3: TargetName Is ".": example.com. SVCB 1 .
    testRecord({0x00, 0x01, 0x00}, 1, ".", {});
    // Figure 4: Specifies a Port: example.com. SVCB 16 foo.example.com. port=53
    testRecord(cat({{0x00, 0x10}, fooExampleCom, {0x00, 0x03, 0x00, 0x02, 0x00, 0x35}}), 16, "foo.example.com.",
               {{SVCParamPort, bytesOf({0x00, 0x35})}});
    // Figure 5: A Generic Key and Unquoted Value: key667=hello
    testRecord(cat({{0x00, 0x01}, fooExampleCom, {0x02, 0x9b, 0x00, 0x05, 'h', 'e', 'l', 'l', 'o'}}), 1, "foo.example.com.",
               {{SVCParamKey(667), slice<byte>::of(as_bytes("hello"))}});
    // Figure 6: A Generic Key and Quoted Value with a Decimal Escape: key667="hello\210qoo"
    testRecord(cat({{0x00, 0x01}, fooExampleCom, {0x02, 0x9b, 0x00, 0x09, 'h', 'e', 'l', 'l', 'o', 0xd2, 'q', 'o', 'o'}}), 1,
               "foo.example.com.", {{SVCParamKey(667), slice<byte>::of(as_bytes("hello\xd2qoo"))}});
    // Figure 7: Two Quoted IPv6 Hints: ipv6hint="2001:db8::1,2001:db8::53:1"
    std::vector<byte> two = {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01,
                             0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x53, 0, 0x01};
    testRecord(cat({{0x00, 0x01}, fooExampleCom, {0x00, 0x06, 0x00, 0x20}, two}), 1, "foo.example.com.",
               {{SVCParamIPv6Hint, slice<byte>::of(two)}});
    // Figure 8: An IPv6 Hint Using the Embedded IPv4 Syntax: ipv6hint="2001:db8:122:344::192.0.2.33"
    std::vector<byte> embedded = {0x20, 0x01, 0x0d, 0xb8, 0x01, 0x22, 0x03, 0x44, 0, 0, 0, 0, 0xc0, 0x00, 0x02, 0x21};
    testRecord(cat({{0x00, 0x01, 0x07, 'e', 'x', 'a', 'm', 'p', 'l', 'e', 0x03, 'c', 'o', 'm', 0x00, 0x00, 0x06, 0x00, 0x10}, embedded}),
               1, "example.com.", {{SVCParamIPv6Hint, slice<byte>::of(embedded)}});
    // Figure 9: Key Order Is Arbitrary in Presentation Format but Sorted on the Wire
    testRecord(cat({{0x00, 0x10}, fooExampleOrg,
                    {0x00, 0x00, 0x00, 0x04, 0x00, 0x01, 0x00, 0x04,                          // mandatory=alpn,ipv4hint
                     0x00, 0x01, 0x00, 0x09, 0x02, 'h', '2', 0x05, 'h', '3', '-', '1', '9',  // alpn=h2,h3-19
                     0x00, 0x04, 0x00, 0x04, 0xc0, 0x00, 0x02, 0x01}}),                      // ipv4hint=192.0.2.1
               16, "foo.example.org.",
               {{SVCParamMandatory, bytesOf({0x00, 0x01, 0x00, 0x04})},
                {SVCParamALPN, bytesOf({0x02, 'h', '2', 0x05, 'h', '3', '-', '1', '9'})},
                {SVCParamIPv4Hint, bytesOf({0xc0, 0x00, 0x02, 0x01})}});
    // Figure 10: An "alpn" Value with an Escaped Comma and an Escaped Backslash
    testRecord(cat({{0x00, 0x10}, fooExampleOrg, {0x00, 0x01, 0x00, 0x0c, 0x08, 'f', '\\', 'o', 'o', ',', 'b', 'a', 'r', 0x02, 'h', '2'}}),
               16, "foo.example.org.", {{SVCParamALPN, bytesOf({0x08, 'f', '\\', 'o', 'o', ',', 'b', 'a', 'r', 0x02, 'h', '2'})}});
}

TEST(GoDNSMessage, SVCBPackErrors) {
    auto record = [](std::vector<SVCParam> params) {
        SVCBResource r;
        r.Target = MustNewName("example.com.");
        r.Params = std::move(params);
        return r;
    };
    std::pair<std::string, SVCBResource> tests[] = {
        {"long value", record({{SVCParamMandatory, slice<byte>::make(0x10000)}})},
        {"out-of-order keys", record({{SVCParamPort, slice<byte>::of(as_bytes("443"))}, {SVCParamALPN, slice<byte>::of(as_bytes("h2"))}})},
        {"duplicate keys", record({{SVCParamALPN, slice<byte>::of(as_bytes("h3"))}, {SVCParamALPN, slice<byte>::of(as_bytes("h2"))}})},
    };
    for (auto& [name, r] : tests) {
        ResourceHeader h;
        h.Name = MustNewName("example.com.");
        auto b = NewBuilder({}, Header{});
        b.StartQuestions();
        b.StartAnswers();
        EXPECT_TRUE(b.SVCBResource(h, r)) << name << ": b.SVCBResource() succeeded; want error";
        b = NewBuilder({}, Header{});
        b.StartQuestions();
        b.StartAnswers();
        EXPECT_TRUE(b.HTTPSResource(h, HTTPSResource(r))) << name << ": b.HTTPSResource() succeeded; want error";
    }
}

TEST(GoDNSMessage, SVCBUnpackOutOfBounds) {
    // An SVCB record whose header Length (65535) exceeds the message.
    std::vector<byte> msg = {
        0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,  // header
        0x00,                                                                  // name "."
        0x00, 0x40, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,                        // SVCB, INET, TTL
        0xff, 0xff,                                                            // length 65535 (spoofed)
        0x00, 0x01, 0x00,                                                      // priority, target
        0x00, 0x01, 0xff, 0xf8,                                                // a key, a length
    };
    Message m;
    EXPECT_TRUE(m.Unpack(slice<byte>::of(msg))) << "expected error parsing malformed message, got nil";
}
