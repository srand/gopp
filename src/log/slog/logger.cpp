// slog's Logger and the default handler, which writes through log, after
// Go 1.27.3's log/slog/logger.go and handler.go.
#include "internal.hpp"

#include <gopp/log.hpp>

#include <mutex>

namespace gopp::slog {

namespace {

// The level of the bridge between log and slog (SetLogLoggerLevel).
const ptr<LevelVar>& logLoggerLevel() {
    static const ptr<LevelVar> v = make_ptr<LevelVar>();
    return v;
}

using output_func = std::function<error(const std::source_location&, std::string_view)>;

// Go's defaultHandler: "LEVEL msg k=v", written by log's standard logger.
struct default_handler final : Handler {
    default_handler(std::shared_ptr<const detail::common_handler> ch, output_func output) noexcept
        : ch(std::move(ch)), output(std::move(output)) {}

    bool Enabled(const ptr<context::Context>&, slog::Level l) override { return l >= logLoggerLevel()->Level(); }

    error Handle(const ptr<context::Context>&, const Record& r) override {
        std::string buf(r.Level.String().view());
        buf.push_back(' ');
        buf += r.Message.view();
        detail::handle_state state(ch.get(), &buf, " ");
        state.appendNonBuiltins(r);
        return output(r.PC, buf);
    }

    ptr<Handler> WithAttrs(std::span<const Attr> as) override {
        return make_ptr<default_handler>(ch->with_attrs(as), output);
    }
    ptr<Handler> WithGroup(std::string_view name) override {
        return make_ptr<default_handler>(ch->withGroup(name), output);
    }

    std::shared_ptr<const detail::common_handler> ch;
    output_func output;
};

// Go's handlerWriter: the Writer of a log::Logger that sends its lines to
// a slog Handler.
struct handler_writer final : log::detail::located_writer {
    handler_writer(ptr<Handler> h, leveler level, bool capture_pc) noexcept
        : h(std::move(h)), level(std::move(level)), capture_pc(capture_pc) {}

    std::pair<isize, error> Write(std::span<const byte> p) override { return WriteLocated(p, std::source_location()); }

    std::pair<isize, error> WriteLocated(std::span<const byte> p, const std::source_location& loc) override {
        slog::Level l = level.Level();
        const auto& ctx = context::Background();
        if (!h->Enabled(ctx, l)) return {0, nullptr};
        std::string_view s = asStringView(p);
        auto orig_len = static_cast<isize>(s.size());  // report that the whole buf was written
        if (s.ends_with('\n')) s.remove_suffix(1);
        Record r(time::Now(), l, string(s), capture_pc ? loc : std::source_location());
        return {orig_len, h->Handle(ctx, r)};
    }

    ptr<Handler> h;
    leveler level;
    bool capture_pc;
};

struct default_logger {
    sync::Mutex mu;
    ptr<Logger> logger;
};

default_logger& defaultState() {
    static default_logger d{{}, New(detail::newDefaultHandler([](const std::source_location& loc, std::string_view s) {
                                   return log::Default()->output(loc, s);
                               }))};
    return d;
}

}  // namespace

namespace detail {

ptr<Handler> newDefaultHandler(output_func output) {
    return make_ptr<default_handler>(std::make_shared<common_handler>(), std::move(output));
}

}  // namespace detail

Logger::Logger(ptr<slog::Handler> h) : handler_(std::move(h)) {
    if (!handler_) panic("nil Handler");
}

ptr<Logger> Logger::WithGroup(std::string_view name) const {
    if (name.empty()) return self();
    return make_ptr<Logger>(handler_->WithGroup(name));
}

bool Logger::Enabled(const ptr<context::Context>& ctx, slog::Level level) const {
    return handler_->Enabled(ctx ? ctx : background(), level);
}

void Logger::LogAttrs(const ptr<context::Context>& ctx, slog::Level level, message msg, std::span<const Attr> attrs) const {
    const ptr<context::Context>& c = ctx ? ctx : background();
    if (!handler_->Enabled(c, level)) return;
    Record r(time::Now(), level, string(msg.text), msg.loc);
    r.AddAttrs(attrs);
    (void)handler_->Handle(c, r);
}

const ptr<context::Context>& Logger::background() {
    static const ptr<context::Context> bg = context::Background();
    return bg;
}

ptr<Logger> New(ptr<Handler> h) { return make_ptr<Logger>(std::move(h)); }

ptr<Logger> Default() {
    auto& d = defaultState();
    std::lock_guard lock(d.mu);
    return d.logger;
}

void SetDefault(ptr<Logger> l) {
    auto& d = defaultState();
    {
        std::lock_guard lock(d.mu);
        d.logger = l;
    }
    // If the handler is the default one, log already writes to it, and
    // sending log's lines to it would loop.
    if (!dynamic_cast<default_handler*>(l->Handler().get())) {
        bool capture_pc = (log::Flags() & (log::Lshortfile | log::Llongfile)) != 0;
        log::SetOutput(make_ptr<handler_writer>(l->Handler(), leveler(logLoggerLevel()), capture_pc));
        log::SetFlags(0);  // just the message: no time or location
    }
}

Level SetLogLoggerLevel(Level level) {
    Level old = logLoggerLevel()->Level();
    logLoggerLevel()->Set(level);
    return old;
}

ptr<log::Logger> NewLogLogger(ptr<Handler> h, Level level) {
    return log::New(make_ptr<handler_writer>(std::move(h), leveler(level), true), "", 0);
}

}  // namespace gopp::slog
