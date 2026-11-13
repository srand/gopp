// What slog's handlers share: Go's commonHandler and handleState, from
// log/slog/handler.go. Internal to log/slog.
#pragma once

#include <gopp/log/slog.hpp>
#include <gopp/sync.hpp>

#include <functional>
#include <memory>
#include <source_location>
#include <string>
#include <string_view>
#include <vector>

namespace gopp::slog::detail {

// The text and JSON handlers' state: options, the attributes and groups of
// WithAttrs and WithGroup, and the Writer. Immutable once made: WithAttrs
// and WithGroup make new ones, which share the Writer's mutex.
struct common_handler : std::enable_shared_from_this<common_handler> {
    bool json = false;  // JSON, or else text
    HandlerOptions opts;
    std::string preformatted_attrs;  // the attributes of WithAttrs, formatted
    // For text, the prefix of the groups opened in preformatted_attrs
    // ("a.b."); not used for JSON.
    std::string group_prefix;
    std::vector<string> groups;  // all the groups of WithGroup
    std::size_t n_open_groups = 0;  // how many of them preformatted_attrs opened
    std::shared_ptr<sync::Mutex> mu;
    ptr<io::Writer> w;

    bool enabled(Level l) const;
    std::shared_ptr<const common_handler> with_attrs(std::span<const Attr> as) const;
    std::shared_ptr<const common_handler> with_group(std::string_view name) const;
    error handle(const Record& r) const;
    std::string_view attr_sep() const noexcept { return json ? "," : " "; }

    std::shared_ptr<common_handler> clone() const;
};

// The state of formatting one record, or the attributes of WithAttrs.
struct handle_state {
    handle_state(const common_handler* h, std::string* buf, std::string_view sep);

    void append_non_builtins(const Record& r);
    void open_groups();
    void open_group(const string& name);
    void close_group(const string& name);
    bool append_attrs(std::span<const Attr> as);
    bool append_attr(Attr a);
    void append_error(const error& err);
    void append_key(std::string_view key);
    void append_two_strings(std::string_view x, std::string_view y);
    void append_string(std::string_view s);
    void append_value(const Value& v);
    void append_time(const time::Time& t);

    error append_text_value(const Value& v);
    error append_json_value(const Value& v);

    const common_handler* h;
    std::string* buf;
    std::string_view sep;  // the separator to write before the next key
    std::string prefix;    // for text: the key prefix of the open groups
    // The groups that are open, for ReplaceAttr; only kept if it's set.
    bool track_groups = false;
    std::vector<string> groups;
};

// Go's defaultHandler, writing with output; the default Logger's writes
// through log's standard logger.
ptr<Handler> new_default_handler(std::function<error(const std::source_location&, std::string_view)> output);

// Go's needsQuoting: whether the text handler quotes s.
bool needs_quoting(std::string_view s);

// Go's appendRFC3339Millis: RFC 3339 with exactly three fractional digits.
void append_rfc3339_millis(std::string& b, time::Time t);

}  // namespace gopp::slog::detail
