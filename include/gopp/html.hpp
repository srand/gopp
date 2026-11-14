// The html package: escaping and unescaping HTML text, after Go 1.27.3.
//
//     html::EscapeString("<a href=\"x\">");      // &lt;a href=&#34;x&#34;&gt;
//     html::UnescapeString("caf&eacute; &lt;3");  // café <3
#pragma once

// The module interfaces (modules/) import these instead.
#ifndef GOPP_MODULE
#include <gopp/string.hpp>

#include <string_view>
#endif

namespace gopp::html {

// html.EscapeString: s with <, >, &, ' and " escaped (as &lt;, &gt;,
// &amp;, &#39; and &#34;). UnescapeString(EscapeString(s)) == s, but not
// always the other way around.
string EscapeString(const string& s);

// html.UnescapeString: s with entities such as &lt;, &eacute; and &#233;
// unescaped: all HTML5 named entities, and numeric ones (with the
// replacements browsers make for 0x80-0x9F and invalid code points).
string UnescapeString(const string& s);

}  // namespace gopp::html
