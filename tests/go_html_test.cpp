// Ported from Go 1.27.3's html/escape_test.go and entity_test.go, with
// Go's test names.
#include <gopp/html.hpp>
#include <gopp/unicode/utf8.hpp>

#include "html/entity.hpp"

#include <gtest/gtest.h>

#include <string>

using namespace gopp;

namespace {

struct unescapeTest {
    // A short description of the test case.
    const char* desc;
    // The HTML text.
    const char* html;
    // The unescaped text.
    const char* unescaped;
};

const unescapeTest unescapeTests[] = {
    // Handle no entities.
    {"copy", "A\ttext\nstring", "A\ttext\nstring"},
    // Handle simple named entities.
    {"simple", "&amp; &gt; &lt;", "& > <"},
    // Handle hitting the end of the string.
    {"stringEnd", "&amp &amp", "& &"},
    // Handle entities with two codepoints.
    {"multiCodepoint", "text &gesl; blah", "text ⋛︀ blah"},
    // Handle decimal numeric entities.
    {"decimalEntity", "Delta = &#916; ", "Delta = Δ "},
    // Handle hexadecimal numeric entities.
    {"hexadecimalEntity", "Lambda = &#x3bb; = &#X3Bb ", "Lambda = λ = λ "},
    // Handle numeric early termination.
    {"numericEnds", "&# &#x &#128;43 &copy = &#169f = &#xa9", "&# &#x €43 © = ©f = ©"},
    // Handle numeric ISO-8859-1 entity replacements.
    {"numericReplacements", "Footnote&#x87;", "Footnote‡"},
    // Handle single ampersand.
    {"copySingleAmpersand", "&", "&"},
    // Handle ampersand followed by non-entity.
    {"copyAmpersandNonEntity", "text &test", "text &test"},
    // Handle "&#".
    {"copyAmpersandHash", "text &#", "text &#"},
};

}  // namespace

TEST(GoHTML, Unescape) {
    for (const auto& tt : unescapeTests) {
        EXPECT_EQ(html::UnescapeString(tt.html), tt.unescaped) << "TestUnescape " << tt.desc;
    }
}

TEST(GoHTML, UnescapeEscape) {
    const char* ss[] = {
        R"()",
        R"(abc def)",
        R"(a & b)",
        R"(a&amp;b)",
        R"(a &amp b)",
        R"(&quot;)",
        R"(")",
        R"("<&>")",
        R"(&quot;&lt;&amp;&gt;&quot;)",
        R"(3&5==1 && 0<1, "0&lt;1", a+acute=&aacute;)",
        R"(The special characters are: <, >, &, ' and ")",
    };
    for (const char* s : ss) EXPECT_EQ(html::UnescapeString(html::EscapeString(s)), s);
}

TEST(GoHTML, EntityLength) {
    using namespace html::detail;
    ASSERT_GT(n_entities, 0u);
    ASSERT_GT(n_entities2, 0u);

    // We verify that the length of UTF-8 encoding of each value is <= 1 +
    // len(key). The +1 comes from the leading "&". This property implies
    // that the length of unescaped text is <= the length of escaped text.
    for (std::size_t i = 0; i < n_entities; i++) {
        const auto& [k, v] = entities[i];
        EXPECT_GE(1 + k.size(), std::size_t(utf8::RuneLen(v))) << "escaped entity &" << k << " is shorter than its UTF-8 encoding";
        if (k.size() > longestEntityWithoutSemicolon) {
            EXPECT_EQ(k.back(), ';') << "entity name " << k;
        }
        if (i > 0) {
            EXPECT_LT(entities[i - 1].name, k) << "entities out of order";
        }
    }
    for (std::size_t i = 0; i < n_entities2; i++) {
        const auto& e = entities2[i];
        EXPECT_GE(1 + e.name.size(), std::size_t(utf8::RuneLen(e.r[0]) + utf8::RuneLen(e.r[1])))
            << "escaped entity &" << e.name << " is shorter than its UTF-8 encoding";
        if (i > 0) {
            EXPECT_LT(entities2[i - 1].name, e.name) << "entities out of order";
        }
    }
}

TEST(HTML, EscapeString) {
    EXPECT_EQ(html::EscapeString(R"(<a href="x">'&'</a>)"), "&lt;a href=&#34;x&#34;&gt;&#39;&amp;&#39;&lt;/a&gt;");
    EXPECT_EQ(html::EscapeString("plain"), "plain");
    EXPECT_EQ(html::UnescapeString("caf&eacute; &lt;3 &#0; &#xD800; &#99999999999;"), "café <3 � � �");
}
