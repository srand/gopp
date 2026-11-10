// After Go's flag/flag.Go.
#include <gopp/flag.hpp>
#include <gopp/fmt.hpp>
#include <gopp/os.hpp>
#include <gopp/strings.hpp>

#include <algorithm>

namespace gopp::flag {

const gopp::error ErrHelp = errors::New("flag: help requested");

void FlagSet::define(flag_def f) {
    for (auto& g : flags_) {
        if (g.name == f.name) panic(fmt::Sprint("{} flag redefined: {}", name_, f.name).view());
    }
    flags_.push_back(std::move(f));
}

void FlagSet::StringVar(gopp::string* p, std::string_view name, gopp::string value, std::string_view usage) {
    *p = value;
    define({gopp::string(name), gopp::string(usage), value, "string", false, [p](std::string_view v) -> gopp::error {
                *p = gopp::string(v);
                return nullptr;
            }});
}

void FlagSet::IntVar(isize* p, std::string_view name, isize value, std::string_view usage) {
    *p = value;
    define({gopp::string(name), gopp::string(usage), strconv::Itoa(value), "int", false, [p](std::string_view v) -> gopp::error {
                auto [n, err] = strconv::ParseInt(v, 0, 64);
                if (err) return errors::New("parse error");
                *p = static_cast<isize>(n);
                return nullptr;
            }});
}

void FlagSet::Int64Var(int64* p, std::string_view name, int64 value, std::string_view usage) {
    *p = value;
    define({gopp::string(name), gopp::string(usage), strconv::Itoa(value), "int", false, [p](std::string_view v) -> gopp::error {
                auto [n, err] = strconv::ParseInt(v, 0, 64);
                if (err) return errors::New("parse error");
                *p = n;
                return nullptr;
            }});
}

void FlagSet::BoolVar(bool* p, std::string_view name, bool value, std::string_view usage) {
    *p = value;
    define({gopp::string(name), gopp::string(usage), gopp::string(value ? "true" : "false"), "", true,
            [p](std::string_view v) -> gopp::error {
                auto [b, err] = strconv::ParseBool(v);
                if (err) return errors::New("parse error");
                *p = b;
                return nullptr;
            }});
}

void FlagSet::DurationVar(time::Duration* p, std::string_view name, time::Duration value, std::string_view usage) {
    *p = value;
    define({gopp::string(name), gopp::string(usage), value.String(), "duration", false, [p](std::string_view v) -> gopp::error {
                auto [d, err] = time::ParseDuration(v);
                if (err) return errors::New("parse error");
                *p = d;
                return nullptr;
            }});
}

gopp::error FlagSet::fail(gopp::string msg) {
    fmt::Println(os::Stderr, "{}", msg);
    PrintDefaults();
    return errors::New(msg);
}

gopp::error FlagSet::Parse(std::span<const gopp::string> arguments) {
    parsed_ = true;
    args_.clear();
    std::size_t i = 0;
    for (; i < arguments.size(); i++) {
        std::string_view s = arguments[i];
        if (s.size() < 2 || s[0] != '-') break;
        int dashes = 1;
        if (s[1] == '-') {
            dashes = 2;
            if (s.size() == 2) {  // "--" ends the flags
                i++;
                break;
            }
        }
        std::string_view name = s.substr(static_cast<std::size_t>(dashes));
        if (name.empty() || name[0] == '-' || name[0] == '=') return fail(fmt::Sprint("bad flag syntax: {}", s));

        bool has_value = false;
        std::string_view value;
        if (auto eq = name.find('='); eq != name.npos) {
            value = name.substr(eq + 1);
            name = name.substr(0, eq);
            has_value = true;
        }

        flag_def* f = nullptr;
        for (auto& g : flags_) {
            if (g.name == name) f = &g;
        }
        if (!f) {
            if (name == "help" || name == "h") {
                PrintDefaults();
                return ErrHelp;
            }
            return fail(fmt::Sprint("flag provided but not defined: -{}", name));
        }
        if (f->IsBool) {
            if (gopp::error err = f->set(has_value ? value : "true")) {
                return fail(fmt::Sprint("invalid boolean value {} for -{}: {}", strconv::Quote(value), name, err));
            }
            continue;
        }
        if (!has_value) {
            if (i + 1 >= arguments.size()) return fail(fmt::Sprint("flag needs an argument: -{}", name));
            value = arguments[++i];
        }
        if (gopp::error err = f->set(value)) {
            return fail(fmt::Sprint("invalid value {} for flag -{}: {}", strconv::Quote(value), name, err));
        }
    }
    for (; i < arguments.size(); i++) args_.push_back(arguments[i]);
    return nullptr;
}

void FlagSet::PrintDefaults() const {
    if (name_.empty()) fmt::Println(os::Stderr, "Usage:");
    else fmt::Println(os::Stderr, "Usage of {}:", name_);
    std::vector<const flag_def*> sorted;
    for (auto& f : flags_) sorted.push_back(&f);
    std::sort(sorted.begin(), sorted.end(), [](auto* a, auto* b) { return a->name < b->name; });
    for (auto* f : sorted) {
        std::string line = "  -" + f->name.str();
        if (*f->type) line += std::string(" ") + f->type;
        line += "\n    \t" + f->usage.str();
        bool zero = f->DefValue.empty() || f->DefValue == "0" || f->DefValue == "false" || f->DefValue == "0s";
        if (!zero) {
            bool quoted = std::string_view(f->type) == "string";
            line += " (default " + (quoted ? std::string(strconv::Quote(f->DefValue).view()) : f->DefValue.str()) + ")";
        }
        fmt::Println(os::Stderr, "{}", line);
    }
}

FlagSet& CommandLine() {
    static FlagSet* fs = new FlagSet(gopp::string());
    return *fs;
}

void StringVar(gopp::string* p, std::string_view name, gopp::string value, std::string_view usage) {
    CommandLine().StringVar(p, name, std::move(value), usage);
}
void IntVar(isize* p, std::string_view name, isize value, std::string_view usage) { CommandLine().IntVar(p, name, value, usage); }
void Int64Var(int64* p, std::string_view name, int64 value, std::string_view usage) {
    CommandLine().Int64Var(p, name, value, usage);
}
void BoolVar(bool* p, std::string_view name, bool value, std::string_view usage) { CommandLine().BoolVar(p, name, value, usage); }
void DurationVar(time::Duration* p, std::string_view name, time::Duration value, std::string_view usage) {
    CommandLine().DurationVar(p, name, value, usage);
}

void Parse(int argc, char** argv) {
    os::SetArgs(argc, argv);
    const auto& a = os::Args();
    FlagSet& fs = CommandLine();
    if (fs.Name().empty() && !a.empty()) fs.Init(a[0]);
    gopp::error err = fs.Parse(std::span<const gopp::string>(a).subspan(a.empty() ? 0 : 1));
    if (err == ErrHelp) os::Exit(0);
    if (err) os::Exit(2);
}

const std::vector<gopp::string>& Args() { return CommandLine().Args(); }

}  // namespace gopp::flag
