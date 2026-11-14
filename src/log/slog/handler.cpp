// slog's handlers, after Go 1.27.3's log/slog/handler.go, text_handler.go,
// json_handler.go and multi_handler.go.
#include "internal.hpp"

#include <gopp/fmt.hpp>

#include <gopp/unicode/utf8.hpp>
#include "text/template/goconv.hpp"

#include <mutex>

namespace gopp::slog {

namespace conv = text::tmpl::detail;

namespace detail {

namespace {

constexpr char key_component_sep = '.';
constexpr std::string_view hex = "0123456789abcdef";

// The ASCII characters that need no escaping in JSON strings, nor quoting
// in text (except ' ' and '=', and with '\\' allowed in text).
constexpr std::array<bool, 128> safe_set = [] {
    std::array<bool, 128> s{};
    for (int c = ' '; c <= 0x7f; c++) s[static_cast<std::size_t>(c)] = true;
    s['"'] = false;
    s['\\'] = false;
    return s;
}();

// Go's appendEscapedJSONString.
void append_escaped_json_string(std::string& buf, std::string_view s) {
    std::size_t start = 0;
    for (std::size_t i = 0; i < s.size();) {
        auto b = static_cast<unsigned char>(s[i]);
        if (b < 0x80) {
            if (safe_set[b]) {
                i++;
                continue;
            }
            buf.append(s.substr(start, i - start));
            buf.push_back('\\');
            switch (b) {
            case '\\':
            case '"': buf.push_back(static_cast<char>(b)); break;
            case '\n': buf.push_back('n'); break;
            case '\r': buf.push_back('r'); break;
            case '\t': buf.push_back('t'); break;
            default:
                buf += "u00";
                buf.push_back(hex[b >> 4]);
                buf.push_back(hex[b & 0xF]);
            }
            i++;
            start = i;
            continue;
        }
        auto [c, size] = utf8::DecodeRuneInString(s.substr(i));
        if (c == utf8::RuneError && size == 1) {
            buf.append(s.substr(start, i - start));
            buf += "\\ufffd";
            i += static_cast<std::size_t>(size);
            start = i;
            continue;
        }
        // U+2028 and U+2029 are valid JSON but not valid JavaScript.
        if (c == 0x2028 || c == 0x2029) {
            buf.append(s.substr(start, i - start));
            buf += "\\u202";
            buf.push_back(hex[static_cast<std::size_t>(c & 0xF)]);
            i += static_cast<std::size_t>(size);
            start = i;
            continue;
        }
        i += static_cast<std::size_t>(size);
    }
    buf.append(s.substr(start));
}

// The JSON of a source: its non-empty fields.
Value source_group(const Source& s) {
    std::vector<Attr> as;
    if (!s.Function.empty()) as.push_back(slog::String("function", s.Function));
    if (!s.File.empty()) as.push_back(slog::String("file", s.File));
    if (s.Line != 0) as.push_back(slog::Int("line", s.Line));
    return GroupValue(as);
}

}  // namespace

// Go's needsQuoting: whether the text handler quotes s.
bool needs_quoting(std::string_view s) {
    if (s.empty()) return true;
    for (std::size_t i = 0; i < s.size();) {
        auto b = static_cast<unsigned char>(s[i]);
        if (b < 0x80) {
            if (b != '\\' && (b == ' ' || b == '=' || !safe_set[b])) return true;
            i++;
            continue;
        }
        auto [r, size] = utf8::DecodeRuneInString(s.substr(i));
        if (r == utf8::RuneError || conv::is_space(r) || !conv::is_print(r)) return true;
        i += static_cast<std::size_t>(size);
    }
    return false;
}

// Go's appendRFC3339Millis: RFC 3339 with exactly three fractional digits.
void append_rfc3339_millis(std::string& b, time::Time t) {
    constexpr std::size_t prefix_len = std::string_view("2006-01-02T15:04:05.000").size();
    // Truncate to milliseconds and add a tenth of one, so that Format
    // writes four digits, of which the fourth is dropped.
    t = t.Truncate(time::Millisecond).Add(time::Duration(time::Millisecond.ns / 10));
    std::string s(t.Format(time::RFC3339Nano).view());
    if (s.size() > prefix_len) s.erase(prefix_len, 1);
    b += s;
}

// ---------------------------------------------------------------------------
// common_handler

std::shared_ptr<common_handler> common_handler::clone() const {
    auto h = std::make_shared<common_handler>();
    h->json = json;
    h->opts = opts;
    h->preformatted_attrs = preformatted_attrs;
    h->group_prefix = group_prefix;
    h->groups = groups;
    h->n_open_groups = n_open_groups;
    h->w = w;
    h->mu = mu;  // shared among all clones of this handler
    return h;
}

bool common_handler::enabled(Level l) const {
    Level min_level = LevelInfo;
    if (opts.Level) min_level = opts.Level.Level();
    return l >= min_level;
}

std::shared_ptr<const common_handler> common_handler::with_attrs(std::span<const Attr> as) const {
    // Not a clone if there's nothing to add.
    bool all_empty = std::all_of(as.begin(), as.end(), [](const Attr& a) {
        return a.Value.Kind() == KindGroup && a.Value.Group().empty();
    });
    if (all_empty) return shared_from_this();
    auto h2 = clone();
    // The attributes are formatted onto preformatted_attrs.
    handle_state state(h2.get(), &h2->preformatted_attrs, "");
    state.prefix = group_prefix;
    if (!h2->preformatted_attrs.empty()) {
        state.sep = attr_sep();
        if (h2->json && h2->preformatted_attrs.back() == '{') state.sep = "";
    }
    // The position before the groups, in case all attrs are empty.
    std::size_t pos = state.buf->size();
    state.open_groups();
    if (!state.append_attrs(as)) {
        state.buf->resize(pos);
    } else {
        h2->group_prefix = state.prefix;
        h2->n_open_groups = h2->groups.size();
    }
    return h2;
}

std::shared_ptr<const common_handler> common_handler::with_group(std::string_view name) const {
    auto h2 = clone();
    h2->groups.emplace_back(name);
    return h2;
}

error common_handler::handle(const Record& r) const {
    std::string buf;
    handle_state state(this, &buf, "");
    if (json) buf.push_back('{');
    // The built-in attributes go in no group, for ReplaceAttr.
    std::vector<string> state_groups = std::move(state.groups);
    state.groups.clear();
    const auto& rep = opts.ReplaceAttr;
    if (!r.Time.IsZero()) {
        time::Time val = r.Time.Round(time::Duration(0));  // strip monotonic to match Attr behavior
        if (!rep) {
            state.append_key(TimeKey);
            state.append_time(val);
        } else {
            state.append_attr(slog::Time(string::from_static(TimeKey), val));
        }
    }
    if (!rep) {
        state.append_key(LevelKey);
        state.append_string(r.Level.String().view());
    } else {
        state.append_attr(slog::Any(string::from_static(LevelKey), r.Level));
    }
    if (opts.AddSource) {
        state.append_attr(slog::Any(string::from_static(SourceKey), r.Source().value_or(Source{})));
    }
    if (!rep) {
        state.append_key(MessageKey);
        state.append_string(r.Message.view());
    } else {
        state.append_attr(slog::String(string::from_static(MessageKey), r.Message));
    }
    state.groups = std::move(state_groups);  // the groups for ReplaceAttr again
    state.append_non_builtins(r);
    buf.push_back('\n');

    std::lock_guard lock(*mu);
    return w->Write(std::span<const byte>(reinterpret_cast<const byte*>(buf.data()), buf.size())).second;
}

// ---------------------------------------------------------------------------
// handle_state

handle_state::handle_state(const common_handler* h_, std::string* buf_, std::string_view sep_)
    : h(h_), buf(buf_), sep(sep_) {
    if (h->opts.ReplaceAttr) {
        track_groups = true;
        groups.assign(h->groups.begin(), h->groups.begin() + static_cast<std::ptrdiff_t>(h->n_open_groups));
    }
}

void handle_state::append_non_builtins(const Record& r) {
    // The attributes of WithAttrs.
    if (const std::string& pfa = h->preformatted_attrs; !pfa.empty()) {
        *buf += sep;
        *buf += pfa;
        sep = h->attr_sep();
        if (h->json && pfa.back() == '{') sep = "";
    }
    // The record's attributes, in the groups of WithGroup not yet opened.
    std::size_t n_open_groups = h->n_open_groups;
    if (r.NumAttrs() > 0) {
        prefix += h->group_prefix;
        // The position before the groups, in case all attrs are empty.
        std::size_t pos = buf->size();
        open_groups();
        n_open_groups = h->groups.size();
        bool empty = true;
        r.Attrs([&](const Attr& a) {
            if (append_attr(a)) empty = false;
            return true;
        });
        if (empty) {
            buf->resize(pos);
            n_open_groups = h->n_open_groups;
        }
    }
    if (h->json) {
        // Close the groups, and the top-level object.
        for (std::size_t i = 0; i < n_open_groups; i++) buf->push_back('}');
        buf->push_back('}');
    }
}

void handle_state::open_groups() {
    for (std::size_t i = h->n_open_groups; i < h->groups.size(); i++) open_group(h->groups[i]);
}

void handle_state::open_group(const string& name) {
    if (h->json) {
        append_key(name.view());
        buf->push_back('{');
        sep = "";
    } else {
        prefix += name.view();
        prefix.push_back(key_component_sep);
    }
    if (track_groups) groups.push_back(name);
}

void handle_state::close_group(const string& name) {
    if (h->json) buf->push_back('}');
    else prefix.resize(prefix.size() - name.size() - 1);
    sep = h->attr_sep();
    if (track_groups) groups.pop_back();
}

bool handle_state::append_attrs(std::span<const Attr> as) {
    bool non_empty = false;
    for (const Attr& a : as) {
        if (append_attr(a)) non_empty = true;
    }
    return non_empty;
}

// Go's appendAttr: whether something was written.
bool handle_state::append_attr(Attr a) {
    a.Value = a.Value.Resolve();
    if (const auto& rep = h->opts.ReplaceAttr; rep && a.Value.Kind() != KindGroup) {
        a = rep(std::span<const string>(groups), std::move(a));
        // The new value may be a LogValuer too.
        a.Value = a.Value.Resolve();
    }
    // Elide empty Attrs.
    if (a.is_empty()) return false;
    // A source is a group in JSON, file:line in text, and left out if empty.
    if (const Source* src = a.Value.As<Source>()) {
        if (*src == Source{}) return false;
        if (h->json) a.Value = source_group(*src);
        else a.Value = StringValue(fmt::Sprint("{}:{}", src->File, src->Line));
    }
    if (a.Value.Kind() == KindGroup) {
        auto attrs = a.Value.Group();
        // Output only non-empty groups.
        if (!attrs.empty()) {
            // The position before the group, in case all its attrs are empty.
            std::size_t pos = buf->size();
            // Inline a group with an empty key.
            if (!a.Key.empty()) open_group(a.Key);
            if (!append_attrs(attrs)) {
                buf->resize(pos);
                return false;
            }
            if (!a.Key.empty()) close_group(a.Key);
        }
    } else {
        append_key(a.Key.view());
        append_value(a.Value);
    }
    return true;
}

void handle_state::append_error(const error& err) { append_string("!ERROR:" + err.Error().str()); }

void handle_state::append_key(std::string_view key) {
    *buf += sep;
    if (!prefix.empty()) append_two_strings(prefix, key);
    else append_string(key);
    buf->push_back(h->json ? ':' : '=');
    sep = h->attr_sep();
}

void handle_state::append_two_strings(std::string_view x, std::string_view y) {
    if (h->json) {
        buf->push_back('"');
        append_escaped_json_string(*buf, x);
        append_escaped_json_string(*buf, y);
        buf->push_back('"');
    } else if (!needs_quoting(x) && !needs_quoting(y)) {
        *buf += x;
        *buf += y;
    } else {
        std::string s(x);
        s += y;
        *buf += conv::quote(s);
    }
}

void handle_state::append_string(std::string_view s) {
    if (h->json) {
        buf->push_back('"');
        append_escaped_json_string(*buf, s);
        buf->push_back('"');
    } else if (needs_quoting(s)) {
        *buf += conv::quote(s);
    } else {
        *buf += s;
    }
}

void handle_state::append_value(const Value& v) {
    error err = h->json ? append_json_value(v) : append_text_value(v);
    if (err) append_error(err);
}

void handle_state::append_time(const time::Time& t) {
    if (h->json) {
        // Go's appendJSONTime.
        if (int y = t.Year(); y < 0 || y >= 10000) {
            append_error(errors::New("time.Time year outside of range [0,9999]"));
            return;
        }
        buf->push_back('"');
        *buf += t.Format(time::RFC3339Nano).view();
        buf->push_back('"');
    } else {
        append_rfc3339_millis(*buf, t);
    }
}

// Go's appendTextValue.
error handle_state::append_text_value(const Value& v) {
    switch (v.Kind()) {
    case KindString: append_string(v.String().view()); break;
    case KindTime: append_time(v.Time()); break;
    case KindAny: {
        auto* p = std::get_if<ptr<const any_value>>(&v.data_);
        if (!p) {
            append_string("<nil>");
            break;
        }
        std::string s;
        bool raw = false;
        if (error err = (*p)->text(s, raw)) return err;
        if (raw) *buf += s;
        else append_string(s);
        break;
    }
    default: v.append(*buf);
    }
    return nullptr;
}

// Go's appendJSONValue.
error handle_state::append_json_value(const Value& v) {
    switch (v.Kind()) {
    case KindString: append_string(v.String().view()); break;
    case KindInt64: *buf += std::to_string(v.Int64()); break;
    case KindUint64: *buf += std::to_string(v.Uint64()); break;
    case KindFloat64: {
        // As json.Marshal writes it, or the error for NaN and infinities.
        gopp::json::detail::encoder e;
        e.write_float(v.Float64(), 64);
        if (e.err) return e.err;
        *buf += e.out;
        break;
    }
    case KindBool: *buf += v.Bool() ? "true" : "false"; break;
    case KindDuration: *buf += std::to_string(v.Duration().Nanoseconds()); break;
    case KindTime: append_time(v.Time()); break;
    case KindAny: {
        auto* p = std::get_if<ptr<const any_value>>(&v.data_);
        if (!p) {
            *buf += "null";
            break;
        }
        std::string s;
        if ((*p)->error_string(s)) {
            append_string(s);
            break;
        }
        return (*p)->json(*buf);
    }
    default: panic(fmt::Sprint("bad kind: {}", v.Kind()).view());
    }
    return nullptr;
}

}  // namespace detail

// ---------------------------------------------------------------------------
// TextHandler and JSONHandler

namespace {

std::shared_ptr<detail::common_handler> new_common_handler(bool json, ptr<io::Writer> w, const HandlerOptions& opts) {
    auto h = std::make_shared<detail::common_handler>();
    h->json = json;
    h->w = std::move(w);
    h->opts = opts;
    h->mu = std::make_shared<sync::Mutex>();
    return h;
}

}  // namespace

ptr<TextHandler> NewTextHandler(ptr<io::Writer> w, const HandlerOptions& opts) {
    return make_ptr<TextHandler>(new_common_handler(false, std::move(w), opts));
}

bool TextHandler::Enabled(const ptr<context::Context>&, slog::Level level) { return h_->enabled(level); }
error TextHandler::Handle(const ptr<context::Context>&, const Record& r) { return h_->handle(r); }
ptr<Handler> TextHandler::WithAttrs(std::span<const Attr> attrs) { return make_ptr<TextHandler>(h_->with_attrs(attrs)); }
ptr<Handler> TextHandler::WithGroup(std::string_view name) { return make_ptr<TextHandler>(h_->with_group(name)); }

ptr<JSONHandler> NewJSONHandler(ptr<io::Writer> w, const HandlerOptions& opts) {
    return make_ptr<JSONHandler>(new_common_handler(true, std::move(w), opts));
}

bool JSONHandler::Enabled(const ptr<context::Context>&, slog::Level level) { return h_->enabled(level); }
error JSONHandler::Handle(const ptr<context::Context>&, const Record& r) { return h_->handle(r); }
ptr<Handler> JSONHandler::WithAttrs(std::span<const Attr> attrs) { return make_ptr<JSONHandler>(h_->with_attrs(attrs)); }
ptr<Handler> JSONHandler::WithGroup(std::string_view name) { return make_ptr<JSONHandler>(h_->with_group(name)); }

// ---------------------------------------------------------------------------
// MultiHandler and DiscardHandler

ptr<MultiHandler> NewMultiHandler(std::span<const ptr<Handler>> handlers) {
    return make_ptr<MultiHandler>(std::vector<ptr<Handler>>(handlers.begin(), handlers.end()));
}

bool MultiHandler::Enabled(const ptr<context::Context>& ctx, slog::Level level) {
    for (auto& h : multi_) {
        if (h->Enabled(ctx, level)) return true;
    }
    return false;
}

error MultiHandler::Handle(const ptr<context::Context>& ctx, const Record& r) {
    std::vector<error> errs;
    for (auto& h : multi_) {
        if (h->Enabled(ctx, r.Level)) {
            if (error err = h->Handle(ctx, r)) errs.push_back(err);
        }
    }
    return errors::Join(errs);
}

ptr<Handler> MultiHandler::WithAttrs(std::span<const Attr> attrs) {
    std::vector<ptr<Handler>> hs;
    hs.reserve(multi_.size());
    for (auto& h : multi_) hs.push_back(h->WithAttrs(attrs));
    return make_ptr<MultiHandler>(std::move(hs));
}

ptr<Handler> MultiHandler::WithGroup(std::string_view name) {
    std::vector<ptr<Handler>> hs;
    hs.reserve(multi_.size());
    for (auto& h : multi_) hs.push_back(h->WithGroup(name));
    return make_ptr<MultiHandler>(std::move(hs));
}

namespace {

struct discard_handler final : Handler {
    bool Enabled(const ptr<context::Context>&, slog::Level) override { return false; }
    error Handle(const ptr<context::Context>&, const Record&) override { return nullptr; }
    ptr<Handler> WithAttrs(std::span<const Attr>) override { return ptr<Handler>(this); }
    ptr<Handler> WithGroup(std::string_view) override { return ptr<Handler>(this); }
};

}  // namespace

const ptr<Handler> DiscardHandler = make_ptr<discard_handler>();

}  // namespace gopp::slog
