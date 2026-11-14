// After spf13/cobra v1.10.2's default usage, help and version functions
// (command.go) and its template functions (cobra.go). Templates are
// executed over the Command by reflection, as in Go.
#include <gopp/cmd.hpp>
#include <gopp/fmt.hpp>
#include <gopp/strings.hpp>
#include <gopp/sync.hpp>

#include "cmd/internal.hpp"

namespace gopp::cmd {

namespace tmpl = text::tmpl;

namespace {

int64 asInt(const tmpl::Value& v) {
    switch (v.Kind()) {
    case tmpl::Kind::Array:
    case tmpl::Kind::Map:
    case tmpl::Kind::Slice: return static_cast<int64>(v.Len());
    case tmpl::Kind::Int: return v.Int();
    case tmpl::Kind::Uint: return static_cast<int64>(v.Uint());
    case tmpl::Kind::String: return strconv::ParseInt(v.Str(), 10, 64).first;
    default: return 0;
    }
}

struct templateFuncsT {
    sync::Mutex mu;
    tmpl::FuncMap funcs;
};

templateFuncsT& templateFuncs() {
    static auto* t = [] {
        auto* t = new templateFuncsT();
        t->funcs = {
            {"trim", [](string s) { return strings::TrimSpace(s); }},
            {"trimRightSpace", [](string s) { return detail::trimRightSpace(s); }},
            {"trimTrailingWhitespaces", [](string s) { return detail::trimRightSpace(s); }},
            // appendIfNotPresent appends s2 to s if it's not in it yet.
            {"appendIfNotPresent",
             [](string s, string stringToAppend) { return strings::Contains(s, stringToAppend) ? s : s + " " + stringToAppend; }},
            {"rpad", [](string s, int padding) { return detail::rpad(s, padding); }},
            // gt compares lengths of containers, integers, and strings as
            // integers.
            {"gt", tmpl::Func::Raw(2, false,
                                   [](std::span<const tmpl::Value> a) -> std::pair<tmpl::Value, error> {
                                       return {tmpl::Value::Of(asInt(a[0]) > asInt(a[1])), nullptr};
                                   })},
            // eq compares integers and strings; it replaces the builtin.
            {"eq", tmpl::Func::Raw(2, false, [](std::span<const tmpl::Value> a) -> std::pair<tmpl::Value, error> {
                 const tmpl::Value& x = a[0];
                 const tmpl::Value& y = a[1];
                 switch (x.Kind()) {
                 case tmpl::Kind::Array:
                 case tmpl::Kind::Map:
                 case tmpl::Kind::Slice: panic("Eq called on unsupported type");
                 case tmpl::Kind::Int: return {tmpl::Value::Of(x.Int() == y.Int()), nullptr};
                 case tmpl::Kind::String: return {tmpl::Value::Of(x.Str() == y.Str()), nullptr};
                 default: return {tmpl::Value::Of(false), nullptr};
                 }
             })},
        };
        return t;
    }();
    return *t;
}

}  // namespace

void AddTemplateFunc(std::string name, tmpl::Func f) {
    auto& t = templateFuncs();
    t.mu.Lock();
    t.funcs[std::move(name)] = std::move(f);
    t.mu.Unlock();
}

void AddTemplateFuncs(const tmpl::FuncMap& funcs) {
    auto& t = templateFuncs();
    t.mu.Lock();
    for (auto& [k, v] : funcs) t.funcs[k] = v;
    t.mu.Unlock();
}

namespace detail {

tmplFunc tmpl(string text) {
    return tmplFunc{text, [text](io::Writer& w, const Command& c) -> error {
                        auto t = tmpl::New("top");
                        {
                            auto& tf = templateFuncs();
                            tf.mu.Lock();
                            t->Funcs(tf.funcs);
                            tf.mu.Unlock();
                        }
                        auto parsed = tmpl::Must(t->Parse(text));
                        return parsed->Execute(w, c);
                    }};
}

// defaultUsageFunc is equivalent to executing defaultUsageTemplate. The
// two should be changed in sync.
error defaultUsageFunc(io::Writer& w, const Command& c) {
    std::string out = "Usage:";
    if (c.Runnable()) out += std::format("\n  {}", c.UseLine());
    if (c.HasAvailableSubCommands()) out += std::format("\n  {} [command]", c.CommandPath());
    if (!c.Aliases.empty()) {
        out += "\n\nAliases:\n";
        out += std::format("  {}", c.NameAndAliases());
    }
    if (c.HasExample()) {
        out += "\n\nExamples:\n";
        out += c.Example.view();
    }
    if (c.HasAvailableSubCommands()) {
        auto& cmds = c.Commands();
        auto line = [&](const Command& subcmd) { out += std::format("\n  {} {}", rpad(subcmd.Name().view(), subcmd.NamePadding()), subcmd.Short); };
        if (c.Groups().empty()) {
            out += "\n\nAvailable Commands:";
            for (auto& subcmd : cmds) {
                if (subcmd->IsAvailableCommand() || subcmd->Name() == "help") line(*subcmd);
            }
        } else {
            for (auto& group : c.Groups()) {
                out += std::format("\n\n{}", group.Title);
                for (auto& subcmd : cmds) {
                    if (subcmd->GroupID == group.ID && (subcmd->IsAvailableCommand() || subcmd->Name() == "help")) line(*subcmd);
                }
            }
            if (!c.AllChildCommandsHaveGroup()) {
                out += "\n\nAdditional Commands:";
                for (auto& subcmd : cmds) {
                    if (subcmd->GroupID.empty() && (subcmd->IsAvailableCommand() || subcmd->Name() == "help")) line(*subcmd);
                }
            }
        }
    }
    if (c.HasAvailableLocalFlags()) {
        out += "\n\nFlags:\n";
        out += trimRightSpace(c.LocalFlags()->FlagUsages()).view();
    }
    if (c.HasAvailableInheritedFlags()) {
        out += "\n\nGlobal Flags:\n";
        out += trimRightSpace(c.InheritedFlags()->FlagUsages()).view();
    }
    if (c.HasHelpSubCommands()) {
        out += "\n\nAdditional help topics:";
        for (auto& subcmd : c.Commands()) {
            if (subcmd->IsAdditionalHelpTopicCommand()) {
                out += std::format("\n  {} {}", rpad(subcmd->CommandPath().view(), subcmd->CommandPathPadding()), subcmd->Short);
            }
        }
    }
    if (c.HasAvailableSubCommands()) {
        out += std::format("\n\nUse \"{} [command] --help\" for more information about a command.", c.CommandPath());
    }
    out += "\n";
    io::WriteString(w, out);
    return nullptr;
}

// defaultHelpFunc is equivalent to executing defaultHelpTemplate. The two
// should be changed in sync.
error defaultHelpFunc(io::Writer& w, const Command& c) {
    string usage = c.Long;
    if (usage.empty()) usage = c.Short;
    usage = trimRightSpace(usage);
    if (!usage.empty()) io::WriteString(w, usage.str() + "\n\n");
    if (c.Runnable() || c.HasSubCommands()) io::WriteString(w, c.UsageString());
    return nullptr;
}

// defaultVersionFunc is equivalent to executing defaultVersionTemplate.
// The two should be changed in sync.
error defaultVersionFunc(io::Writer& w, const Command& c) {
    return fmt::Print(w, "{} version {}\n", c.DisplayName(), c.Version).second;
}

}  // namespace detail

}  // namespace gopp::cmd
