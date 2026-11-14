// The mime package's internals, for its tests (after Go's export_test.go
// and the package-level variables its tests set).
#pragma once

#include <gopp/mime.hpp>

#include <functional>
#include <string_view>
#include <utility>
#include <vector>

namespace gopp::mime::internal {

using typeMap = std::map<string, string>;

// Go's builtinTypesLower.
const typeMap& builtinTypesLower();

// Replaces the tables (setMimeTypes).
void setMimeTypes(const typeMap& lowerExt, const typeMap& mixExt);
// Sets the type of an extension (setExtensionType).
error setExtensionType(std::string_view extension, std::string_view mimeType);

// Loads a shared MIME-info globs2 file, or a mime.types file.
error loadMimeGlobsFile(std::string_view filename);
void loadMimeFile(std::string_view filename);

// Makes the next lookup initialize the tables again, with fn instead of
// the built-in and system types if fn isn't empty (setMimeInit).
void setMimeInit(std::function<void()> fn);
// Initializes the tables if they aren't (once.Do(initMime)).
void ensureInit();

// The globs2 files and mime.types files that initialization reads; returns
// the earlier lists.
std::pair<std::vector<string>, std::vector<string>> setSystemFiles(std::vector<string> globs,
                                                                     std::vector<string> typeFiles);

// mediatype.go's helpers, for TestConsumeToken and friends.
std::pair<std::string_view, std::string_view> consumeToken(std::string_view v);
std::pair<string, std::string_view> consumeValue(std::string_view v);
std::tuple<string, string, std::string_view> consumeMediaParam(std::string_view v);

}  // namespace gopp::mime::internal
