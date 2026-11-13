// The mime package: MIME types by file extension, and media types.
//
//     mime::TypeByExtension(".css");          // "text/css; charset=utf-8"
//     auto [mt, params, err] = mime::ParseMediaType("text/html; charset=utf-8");
//     mime::FormatMediaType("text/plain", {{"charset", "utf-8"}});
//
// The table of types starts with Go's built-in types, and adds those of the
// system: the shared MIME-info database (/usr/share/mime/globs2) if there
// is one, else the mime.types files (/etc/mime.types and the like).
#pragma once

// The module interfaces (modules/) import these instead.
#ifndef GOPP_MODULE
#include <gopp/errors.hpp>
#include <gopp/string.hpp>

#include <map>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>
#endif

namespace gopp::mime {

// The parameters of a media type, by lowercase name.
using Params = std::map<string, string>;

// mime.TypeByExtension: the MIME type of the file extension ext, which
// starts with a dot (".html"), or "" if it's unknown. Text types have a
// charset parameter ("text/html; charset=utf-8"). The lookup is first case
// sensitive, then case insensitive.
string TypeByExtension(std::string_view ext);

// mime.ExtensionsByType: the extensions known for the MIME type typ,
// sorted; empty if there are none.
std::pair<std::vector<string>, error> ExtensionsByType(std::string_view typ);

// mime.AddExtensionType: sets the MIME type of the extension ext, which
// must start with a dot. A text type gets a charset of utf-8 unless it has
// one.
error AddExtensionType(std::string_view ext, std::string_view typ);

// mime.ErrInvalidMediaParameter: ParseMediaType found the media type but
// not its parameters, and returned it without them.
extern const error ErrInvalidMediaParameter;

// mime.ParseMediaType: parses a media type value (Content-Type) or a
// disposition (Content-Disposition), as RFC 1521 and RFC 2183 have them,
// into the type, lowercase and trimmed, and its parameters, with RFC 2231
// continuations and encodings decoded. On ErrInvalidMediaParameter the
// type is returned without parameters.
std::tuple<string, Params, error> ParseMediaType(std::string_view v);

// mime.FormatMediaType: the media type t with the parameters, as RFC 2045
// and RFC 2616 have it, with parameter values quoted or RFC 2231 encoded
// as needed; "" if t or a parameter name isn't a valid token.
string FormatMediaType(std::string_view t, const Params& param);

}  // namespace gopp::mime
