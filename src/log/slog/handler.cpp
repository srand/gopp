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

constexpr char keyComponentSep = '.';
constexpr std::string_view hex = "0123456789abcdef";

// The ASCII characters that need no escaping in JSON strings, nor quoting
// in text (except ' ' and '=', and with '\\' allowed in text).
constexpr std::array<bool, 128> safeSet = [] {
    std::array<bool, 128> s{};
    for (int c = ' '; c <= 0x7f; c++) s[static_cast<std::size_t>(c)] = true;
    s['"'] = false;
    s['\\'] = false;
    return s;
}();

// Go's appendEscapedJSONString.
void appendEscapedJsonString(std::string& buf, std::string_view s) {
    std::size_t start = 0;
    for (std::size_t i = 0; i < s.size();) {
        auto b = static_cast<unsigned char>(s[i]);
        if (b < 0x80) {
            if (safeSet[b]) {
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
Value sourceGroup(const Source& s) {
    std::vector<Attr> as;
    if (!s.Function.empty()) as.push_back(slog::String("function", s.Function));
    if (!s.File.empty()) as.push_back(slog::String("file", s.File));
    if (s.Line != 0) as.push_back(slog::Int("line", s.Line));
    return GroupValue(as);
}

}  // namespace

// Go's needsQuoting: whether the text handler quotes s.
bool needsQuoting(std::string_view s) {
    if (s.empty()) return true;
    for (std::size_t i = 0; i < s.size();) {
        auto b = static_cast<unsigned char>(s[i]);
        if (b < 0x80) {
            if (b != '\\' && (b == ' ' || b == '=' || !safeSet[b])) return true;
            i++;
            continue;
        }
        auto [r, size] = utf8::DecodeRuneInString(s.substr(i));
        if (r == utf8::RuneError || conv::isSpace(r) || !conv::isPrint(r)) return true;
        i += static_cast<std::size_t>(size);
    }
    return false;
}

// Go's appendRFC3339Millis: RFC 3339 with exactly three fractional digits.
void appendRfc3339Millis(std::string& b, time::Time t) {
    constexpr std::size_t prefixLen = std::string_view("2006-01-02T15:04:05.000").size();
    // Truncate to milliseconds and add a tenth of one, so that Format
    // writes four digits, of which the fourth is dropped.
    t = t.Truncate(time::Millisecond).Add(time::Duration(time::Millisecond.ns / 10));
    std::string s(t.Format(time::RFC3339Nano).view());
    if (s.size() > prefixLen) s.erase(prefixLen, 1);
    b += s;
}

// ---------------------------------------------------------------------------
// commonHandler

std::shared_ptr<commonHandler> commonHandler::clone() const {
    auto h = std::make_shared<commonHandler>();
    h->json = json;
    h->opts = opts;
    h->preformattedAttrs = preformattedAttrs;
    h->groupPrefix = groupPrefix;
    h->groups = groups;
    h->nOpenGroups = nOpenGroups;
    h->w = w;
    h->mu = mu;  // shared among all clones of this handler
    return h;
}

bool commonHandler::enabled(Level l) const {
    Level minLevel = LevelInfo;
    if (opts.Level) minLevel = opts.Level.Level();
    return l >= minLevel;
}

std::shared_ptr<const commonHandler> commonHandler::withAttrs(std::span<const Attr> as) const {
    // Not a clone if there's nothing to add.
    bool allEmpty = std::all_of(as.begin(), as.end(), [](const Attr& a) {
        return a.Value.Kind() == KindGroup && a.Value.Group().empty();
    });
    if (allEmpty) return shared_from_this();
    auto h2 = clone();
    // The attributes are formatted onto preformattedAttrs.
    handleState state(h2.get(), &h2->preformattedAttrs, "");
    state.prefix = groupPrefix;
    if (!h2->preformattedAttrs.empty()) {
        state.sep = attrSep();
        if (h2->json && h2->preformattedAttrs.back() == '{') state.sep = "";
    }
    // The position before the groups, in case all attrs are empty.
    std::size_t pos = state.buf->size();
    state.openGroups();
    if (!state.appendAttrs(as)) {
        state.buf->resize(pos);
    } else {
        h2->groupPrefix = state.prefix;
        h2->nOpenGroups = h2->groups.size();
    }
    return h2;
}

std::shared_ptr<const commonHandler> commonHandler::withGroup(std::string_view name) const {
    auto h2 = clone();
    h2->groups.emplace_back(name);
    return h2;
}

error commonHandler::handle(const Record& r) const {
    std::string buf;
    handleState state(this, &buf, "");
    if (json) buf.push_back('{');
    // The built-in attributes go in no group, for ReplaceAttr.
    std::vector<string> stateGroups = std::move(state.groups);
    state.groups.clear();
    const auto& rep = opts.ReplaceAttr;
    if (!r.Time.IsZero()) {
        time::Time val = r.Time.Round(time::Duration(0));  // strip monotonic to match Attr behavior
        if (!rep) {
            state.appendKey(TimeKey);
            state.appendTime(val);
        } else {
            state.appendAttr(slog::Time(string::fromStatic(TimeKey), val));
        }
    }
    if (!rep) {
        state.appendKey(LevelKey);
        state.appendString(r.Level.String().view());
    } else {
        state.appendAttr(slog::Any(string::fromStatic(LevelKey), r.Level));
    }
    if (opts.AddSource) {
        state.appendAttr(slog::Any(string::fromStatic(SourceKey), r.Source().value_or(Source{})));
    }
    if (!rep) {
        state.appendKey(MessageKey);
        state.appendString(r.Message.view());
    } else {
        state.appendAttr(slog::String(string::fromStatic(MessageKey), r.Message));
    }
    state.groups = std::move(stateGroups);  // the groups for ReplaceAttr again
    state.appendNonBuiltins(r);
    buf.push_back('\n');

    std::lock_guard lock(*mu);
    return w->Write(std::span<const byte>(reinterpret_cast<const byte*>(buf.data()), buf.size())).second;
}

// ---------------------------------------------------------------------------
// handleState

handleState::handleState(const commonHandler* h_, std::string* buf_, std::string_view sep_)
    : h(h_), buf(buf_), sep(sep_) {
    if (h->opts.ReplaceAttr) {
        trackGroups = true;
        groups.assign(h->groups.begin(), h->groups.begin() + static_cast<std::ptrdiff_t>(h->nOpenGroups));
    }
}

void handleState::appendNonBuiltins(const Record& r) {
    // The attributes of WithAttrs.
    if (const std::string& pfa = h->preformattedAttrs; !pfa.empty()) {
        *buf += sep;
        *buf += pfa;
        sep = h->attrSep();
        if (h->json && pfa.back() == '{') sep = "";
    }
    // The record's attributes, in the groups of WithGroup not yet opened.
    std::size_t nOpenGroups = h->nOpenGroups;
    if (r.NumAttrs() > 0) {
        prefix += h->groupPrefix;
        // The position before the groups, in case all attrs are empty.
        std::size_t pos = buf->size();
        openGroups();
        nOpenGroups = h->groups.size();
        bool empty = true;
        r.Attrs([&](const Attr& a) {
            if (appendAttr(a)) empty = false;
            return true;
        });
        if (empty) {
            buf->resize(pos);
            nOpenGroups = h->nOpenGroups;
        }
    }
    if (h->json) {
        // Close the groups, and the top-level object.
        for (std::size_t i = 0; i < nOpenGroups; i++) buf->push_back('}');
        buf->push_back('}');
    }
}

void handleState::openGroups() {
    for (std::size_t i = h->nOpenGroups; i < h->groups.size(); i++) openGroup(h->groups[i]);
}

void handleState::openGroup(const string& name) {
    if (h->json) {
        appendKey(name.view());
        buf->push_back('{');
        sep = "";
    } else {
        prefix += name.view();
        prefix.push_back(keyComponentSep);
    }
    if (trackGroups) groups.push_back(name);
}

void handleState::closeGroup(const string& name) {
    if (h->json) buf->push_back('}');
    else prefix.resize(prefix.size() - name.size() - 1);
    sep = h->attrSep();
    if (trackGroups) groups.pop_back();
}

bool handleState::appendAttrs(std::span<const Attr> as) {
    bool nonEmpty = false;
    for (const Attr& a : as) {
        if (appendAttr(a)) nonEmpty = true;
    }
    return nonEmpty;
}

// Go's appendAttr: whether something was written.
bool handleState::appendAttr(Attr a) {
    a.Value = a.Value.Resolve();
    if (const auto& rep = h->opts.ReplaceAttr; rep && a.Value.Kind() != KindGroup) {
        a = rep(std::span<const string>(groups), std::move(a));
        // The new value may be a LogValuer too.
        a.Value = a.Value.Resolve();
    }
    // Elide empty Attrs.
    if (a.isEmpty()) return false;
    // A source is a group in JSON, file:line in text, and left out if empty.
    if (const Source* src = a.Value.As<Source>()) {
        if (*src == Source{}) return false;
        if (h->json) a.Value = sourceGroup(*src);
        else a.Value = StringValue(fmt::Sprint("{}:{}", src->File, src->Line));
    }
    if (a.Value.Kind() == KindGroup) {
        auto attrs = a.Value.Group();
        // Output only non-empty groups.
        if (!attrs.empty()) {
            // The position before the group, in case all its attrs are empty.
            std::size_t pos = buf->size();
            // Inline a group with an empty key.
            if (!a.Key.empty()) openGroup(a.Key);
            if (!appendAttrs(attrs)) {
                buf->resize(pos);
                return false;
            }
            if (!a.Key.empty()) closeGroup(a.Key);
        }
    } else {
        appendKey(a.Key.view());
        appendValue(a.Value);
    }
    return true;
}

void handleState::appendError(const error& err) { appendString("!ERROR:" + err.Error().str()); }

void handleState::appendKey(std::string_view key) {
    *buf += sep;
    if (!prefix.empty()) appendTwoStrings(prefix, key);
    else appendString(key);
    buf->push_back(h->json ? ':' : '=');
    sep = h->attrSep();
}

void handleState::appendTwoStrings(std::string_view x, std::string_view y) {
    if (h->json) {
        buf->push_back('"');
        appendEscapedJsonString(*buf, x);
        appendEscapedJsonString(*buf, y);
        buf->push_back('"');
    } else if (!needsQuoting(x) && !needsQuoting(y)) {
        *buf += x;
        *buf += y;
    } else {
        std::string s(x);
        s += y;
        *buf += conv::quote(s);
    }
}

void handleState::appendString(std::string_view s) {
    if (h->json) {
        buf->push_back('"');
        appendEscapedJsonString(*buf, s);
        buf->push_back('"');
    } else if (needsQuoting(s)) {
        *buf += conv::quote(s);
    } else {
        *buf += s;
    }
}

void handleState::appendValue(const Value& v) {
    error err = h->json ? appendJsonValue(v) : appendTextValue(v);
    if (err) appendError(err);
}

void handleState::appendTime(const time::Time& t) {
    if (h->json) {
        // Go's appendJSONTime.
        if (int y = t.Year(); y < 0 || y >= 10000) {
            appendError(errors::New("time.Time year outside of range [0,9999]"));
            return;
        }
        buf->push_back('"');
        *buf += t.Format(time::RFC3339Nano).view();
        buf->push_back('"');
    } else {
        appendRfc3339Millis(*buf, t);
    }
}

// Go's appendTextValue.
error handleState::appendTextValue(const Value& v) {
    switch (v.Kind()) {
    case KindString: appendString(v.String().view()); break;
    case KindTime: appendTime(v.Time()); break;
    case KindAny: {
        auto* p = std::get_if<ptr<const anyValue>>(&v.data_);
        if (!p) {
            appendString("<nil>");
            break;
        }
        std::string s;
        bool raw = false;
        if (error err = (*p)->text(s, raw)) return err;
        if (raw) *buf += s;
        else appendString(s);
        break;
    }
    default: v.append(*buf);
    }
    return nullptr;
}

// Go's appendJSONValue.
error handleState::appendJsonValue(const Value& v) {
    switch (v.Kind()) {
    case KindString: appendString(v.String().view()); break;
    case KindInt64: *buf += std::to_string(v.Int64()); break;
    case KindUint64: *buf += std::to_string(v.Uint64()); break;
    case KindFloat64: {
        // As json.Marshal writes it, or the error for NaN and infinities.
        gopp::json::detail::encoder e;
        e.writeFloat(v.Float64(), 64);
        if (e.err) return e.err;
        *buf += e.out;
        break;
    }
    case KindBool: *buf += v.Bool() ? "true" : "false"; break;
    case KindDuration: *buf += std::to_string(v.Duration().Nanoseconds()); break;
    case KindTime: appendTime(v.Time()); break;
    case KindAny: {
        auto* p = std::get_if<ptr<const anyValue>>(&v.data_);
        if (!p) {
            *buf += "null";
            break;
        }
        std::string s;
        if ((*p)->errorString(s)) {
            appendString(s);
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

std::shared_ptr<detail::commonHandler> newCommonHandler(bool json, ptr<io::Writer> w, const HandlerOptions& opts) {
    auto h = std::make_shared<detail::commonHandler>();
    h->json = json;
    h->w = std::move(w);
    h->opts = opts;
    h->mu = std::make_shared<sync::Mutex>();
    return h;
}

}  // namespace

ptr<TextHandler> NewTextHandler(ptr<io::Writer> w, const HandlerOptions& opts) {
    return make_ptr<TextHandler>(newCommonHandler(false, std::move(w), opts));
}

bool TextHandler::Enabled(const ptr<context::Context>&, slog::Level level) { return h_->enabled(level); }
error TextHandler::Handle(const ptr<context::Context>&, const Record& r) { return h_->handle(r); }
ptr<Handler> TextHandler::WithAttrs(std::span<const Attr> attrs) { return make_ptr<TextHandler>(h_->withAttrs(attrs)); }
ptr<Handler> TextHandler::WithGroup(std::string_view name) { return make_ptr<TextHandler>(h_->withGroup(name)); }

ptr<JSONHandler> NewJSONHandler(ptr<io::Writer> w, const HandlerOptions& opts) {
    return make_ptr<JSONHandler>(newCommonHandler(true, std::move(w), opts));
}

bool JSONHandler::Enabled(const ptr<context::Context>&, slog::Level level) { return h_->enabled(level); }
error JSONHandler::Handle(const ptr<context::Context>&, const Record& r) { return h_->handle(r); }
ptr<Handler> JSONHandler::WithAttrs(std::span<const Attr> attrs) { return make_ptr<JSONHandler>(h_->withAttrs(attrs)); }
ptr<Handler> JSONHandler::WithGroup(std::string_view name) { return make_ptr<JSONHandler>(h_->withGroup(name)); }

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

struct discardHandler final : Handler {
    bool Enabled(const ptr<context::Context>&, slog::Level) override { return false; }
    error Handle(const ptr<context::Context>&, const Record&) override { return nullptr; }
    ptr<Handler> WithAttrs(std::span<const Attr>) override { return ptr<Handler>(this); }
    ptr<Handler> WithGroup(std::string_view) override { return ptr<Handler>(this); }
};

}  // namespace

const ptr<Handler> DiscardHandler = make_ptr<discardHandler>();

}  // namespace gopp::slog
