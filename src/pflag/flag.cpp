// After spf13/pflag v1.0.10's flag.go and errors.go.
#include <gopp/fmt.hpp>
#include <gopp/os.hpp>
#include <gopp/pflag.hpp>
#include <gopp/strings.hpp>

#include <algorithm>
#include <string>

namespace gopp::pflag {

const gopp::error ErrHelp = errors::New("pflag: help requested");

namespace {

// %q of a byte or rune: 'c'.
std::string quoteChar(char c) {
    switch (c) {
    case '\'': return "'\\''";
    case '\\': return "'\\\\'";
    case '\n': return "'\\n'";
    case '\t': return "'\\t'";
    default: break;
    }
    if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) {
        static constexpr char hexd[] = "0123456789abcdef";
        return std::string("'\\x") + hexd[(c >> 4) & 0xf] + hexd[c & 0xf] + "'";
    }
    return std::string("'") + c + "'";
}

std::vector<Flag*> sortFlags(const std::unordered_map<NormalizedName, ptr<Flag>>& flags) {
    std::vector<std::pair<std::string_view, Flag*>> list;
    list.reserve(flags.size());
    for (auto& [k, f] : flags) list.emplace_back(k.view(), f.get());
    std::sort(list.begin(), list.end(), [](auto& a, auto& b) { return a.first < b.first; });
    std::vector<Flag*> result;
    result.reserve(list.size());
    for (auto& e : list) result.push_back(e.second);
    return result;
}

// Whether the flag's default is its type's zero value, which usage
// messages don't show.
bool defaultIsZeroValue(const Flag* f) {
    std::string_view t = f->Value->Type().view();
    std::string_view d = f->DefValue.view();
    if (t == "bool" || t == "boolfunc") return d == "false" || d.empty();
    if (t == "duration") return d == "0" || d == "0s";
    if (t == "int" || t == "int8" || t == "int32" || t == "int64" || t == "uint" || t == "uint8" || t == "uint16" ||
        t == "uint32" || t == "uint64" || t == "count" || t == "float32" || t == "float64") {
        return d == "0";
    }
    if (t == "string") return d.empty();
    if (t == "ip" || t == "ipMask" || t == "ipNet") return d == "<nil>";
    if (t == "intSlice" || t == "stringSlice" || t == "stringArray") return d == "[]";
    return d == "false" || d == "<nil>" || d.empty() || d == "0";
}

// wrapN splits s at the last whitespace before i (and slop more bytes), or
// at a newline before that.
std::pair<std::string, std::string> wrapN(std::size_t i, std::size_t slop, const std::string& s) {
    if (i + slop > s.size()) return {s, ""};
    std::string_view head = std::string_view(s).substr(0, i);
    auto w = head.find_last_of(" \t\n");
    if (w == std::string_view::npos || w == 0) return {s, ""};
    auto nl = head.rfind('\n');
    if (nl != std::string_view::npos && nl > 0 && nl < w) return {s.substr(0, nl), s.substr(nl + 1)};
    return {s.substr(0, w), s.substr(w + 1)};
}

std::string replaceNewlines(const std::string& s, const std::string& with) {
    std::string out;
    for (char c : s) {
        if (c == '\n') out += with;
        else out += c;
    }
    return out;
}

// wrap wraps s to w columns, indenting continuation lines by i.
std::string wrap(std::size_t i, std::size_t w, std::string s) {
    if (w == 0) return replaceNewlines(s, "\n" + std::string(i, ' '));
    // The width left for the usage, and the start of the next line.
    long wrapw = static_cast<long>(w) - static_cast<long>(i);
    std::string r, l;
    // Not enough room: start on a new line, indented by 16.
    if (wrapw < 24) {
        i = 16;
        wrapw = static_cast<long>(w) - static_cast<long>(i);
        r += "\n" + std::string(i, ' ');
    }
    // Still not enough room: don't wrap.
    if (wrapw < 24) return replaceNewlines(s, r);
    // Try to avoid short orphan words on the last line.
    std::size_t slop = 5;
    wrapw -= static_cast<long>(slop);
    std::tie(l, s) = wrapN(static_cast<std::size_t>(wrapw), slop, s);
    r += replaceNewlines(l, "\n" + std::string(i, ' '));
    while (!s.empty()) {
        std::string t;
        std::tie(t, s) = wrapN(static_cast<std::size_t>(wrapw), slop, s);
        r += "\n" + std::string(i, ' ') + replaceNewlines(t, "\n" + std::string(i, ' '));
    }
    return r;
}

std::span<const gopp::string> stripUnknownFlagValue(std::span<const gopp::string> args) {
    if (args.empty()) return args;
    // --unknown --next-flag ...
    const gopp::string& first = args[0];
    if (!first.empty() && first[0] == '-') return args;
    // --unknown arg ... (args may be the flag's value)
    if (args.size() > 1) return args.subspan(1);
    return {};
}

bool isGotestShorthandFlag(std::string_view flag) { return flag.starts_with("test."); }

}  // namespace

// ---------------------------------------------------------------------------
// FlagSet

FlagSet::FlagSet(gopp::string name, ErrorHandling errorHandling) : name_(std::move(name)), errorHandling_(errorHandling) {}

FlagSet::~FlagSet() = default;

ptr<FlagSet> NewFlagSet(gopp::string name, ErrorHandling errorHandling) {
    return make_ptr<FlagSet>(std::move(name), errorHandling);
}

void FlagSet::Init(gopp::string name, ErrorHandling errorHandling) {
    name_ = std::move(name);
    errorHandling_ = errorHandling;
    argsLenAtDash_ = -1;
}

void FlagSet::SetNormalizeFunc(NormalizeFunc n) {
    normalizeNameFunc_ = std::move(n);
    sortedFormal_.clear();
    std::vector<std::pair<NormalizedName, ptr<Flag>>> renamed;
    for (auto& [fname, flag] : formal_) {
        NormalizedName nname = normalizeFlagName(flag->Name.view());
        if (fname == nname) continue;
        renamed.emplace_back(fname, flag);
    }
    for (auto& [fname, flag] : renamed) {
        NormalizedName nname = normalizeFlagName(flag->Name.view());
        flag->Name = nname;
        formal_.erase(fname);
        formal_[nname] = flag;
        if (auto it = actual_.find(fname); it != actual_.end()) {
            actual_.erase(it);
            actual_[nname] = flag;
        }
    }
}

FlagSet::NormalizeFunc FlagSet::GetNormalizeFunc() const {
    if (normalizeNameFunc_) return normalizeNameFunc_;
    return [](FlagSet*, std::string_view name) { return NormalizedName(name); };
}

NormalizedName FlagSet::normalizeFlagName(std::string_view name) const {
    if (normalizeNameFunc_) return normalizeNameFunc_(const_cast<FlagSet*>(this), name);
    return NormalizedName(name);
}

io::Writer& FlagSet::Output() const {
    if (!output_) return os::Stderr;
    return *output_;
}

void FlagSet::SetOutput(ptr<io::Writer> output) { output_ = std::move(output); }

void FlagSet::VisitAll(const std::function<void(Flag*)>& fn) const {
    if (formal_.empty()) return;
    std::vector<Flag*> flags;
    if (SortFlags) {
        if (formal_.size() != sortedFormal_.size()) sortedFormal_ = sortFlags(formal_);
        flags = sortedFormal_;
    } else {
        flags = orderedFormal_;
    }
    for (Flag* f : flags) fn(f);
}

bool FlagSet::HasFlags() const noexcept { return !formal_.empty(); }

bool FlagSet::HasAvailableFlags() const noexcept {
    for (auto& [_, f] : formal_) {
        if (!f->Hidden) return true;
    }
    return false;
}

void FlagSet::Visit(const std::function<void(Flag*)>& fn) const {
    if (actual_.empty()) return;
    std::vector<Flag*> flags;
    if (SortFlags) {
        if (actual_.size() != sortedActual_.size()) sortedActual_ = sortFlags(actual_);
        flags = sortedActual_;
    } else {
        flags = orderedActual_;
    }
    for (Flag* f : flags) fn(f);
}

Flag* FlagSet::Lookup(std::string_view name) const {
    auto it = formal_.find(normalizeFlagName(name));
    return it == formal_.end() ? nullptr : it->second.get();
}

Flag* FlagSet::ShorthandLookup(std::string_view name) const {
    if (name.empty()) return nullptr;
    if (name.size() > 1) {
        auto msg = fmt::Sprint("can not look up shorthand which is more than one ASCII character: {}", strconv::Quote(name));
        io::WriteString(Output(), msg);
        panic(msg.view());
    }
    auto it = shorthands_.find(name[0]);
    return it == shorthands_.end() ? nullptr : it->second;
}

gopp::error FlagSet::notDefined(std::string_view name) {
    return make_ptr<NotExistError>(gopp::string(name), gopp::string(), NotExistError::flagNotDefined);
}

gopp::error FlagSet::wrongType(std::string_view want, const gopp::string& have) {
    return fmt::Errorf("trying to get {} value of flag of type {}", want, have);
}

gopp::error FlagSet::MarkDeprecated(std::string_view name, std::string_view usageMessage) {
    Flag* flag = Lookup(name);
    if (!flag) return make_ptr<NotExistError>(gopp::string(name), gopp::string(), NotExistError::flagNotExist);
    if (usageMessage.empty()) return fmt::Errorf("deprecated message for flag {} must be set", strconv::Quote(name));
    flag->Deprecated = gopp::string(usageMessage);
    flag->Hidden = true;
    return nullptr;
}

gopp::error FlagSet::MarkShorthandDeprecated(std::string_view name, std::string_view usageMessage) {
    Flag* flag = Lookup(name);
    if (!flag) return make_ptr<NotExistError>(gopp::string(name), gopp::string(), NotExistError::flagNotExist);
    if (usageMessage.empty()) return fmt::Errorf("deprecated message for flag {} must be set", strconv::Quote(name));
    flag->ShorthandDeprecated = gopp::string(usageMessage);
    return nullptr;
}

gopp::error FlagSet::MarkHidden(std::string_view name) {
    Flag* flag = Lookup(name);
    if (!flag) return make_ptr<NotExistError>(gopp::string(name), gopp::string(), NotExistError::flagNotExist);
    flag->Hidden = true;
    return nullptr;
}

gopp::error FlagSet::Set(std::string_view name, gopp::string value) {
    NormalizedName normalName = normalizeFlagName(name);
    auto it = formal_.find(normalName);
    if (it == formal_.end()) {
        return make_ptr<NotExistError>(gopp::string(name), gopp::string(), NotExistError::flagNoSuchFlag);
    }
    ptr<Flag> flag = it->second;
    if (auto err = flag->Value->Set(value)) return make_ptr<InvalidValueError>(flag, value, err);
    if (!flag->Changed) {
        actual_[normalName] = flag;
        orderedActual_.push_back(flag.get());
        flag->Changed = true;
    }
    if (!flag->Deprecated.empty()) {
        fmt::Print(Output(), "Flag --{} has been deprecated, {}\n", flag->Name, flag->Deprecated);
    }
    return nullptr;
}

gopp::error FlagSet::SetAnnotation(std::string_view name, std::string_view key, std::vector<gopp::string> values) {
    auto it = formal_.find(normalizeFlagName(name));
    if (it == formal_.end()) {
        return make_ptr<NotExistError>(gopp::string(name), gopp::string(), NotExistError::flagNoSuchFlag);
    }
    it->second->Annotations[gopp::string(key)] = std::move(values);
    return nullptr;
}

bool FlagSet::Changed(std::string_view name) const {
    Flag* flag = Lookup(name);
    return flag && flag->Changed;
}

void FlagSet::PrintDefaults() const { io::WriteString(Output(), FlagUsagesWrapped(0)); }

std::pair<gopp::string, gopp::string> UnquoteUsage(const Flag* flag) {
    // Look for a back-quoted name, but avoid the strings package.
    std::string_view usage = flag->Usage.view();
    for (std::size_t i = 0; i < usage.size(); i++) {
        if (usage[i] == '`') {
            for (std::size_t j = i + 1; j < usage.size(); j++) {
                if (usage[j] == '`') {
                    gopp::string name(usage.substr(i + 1, j - i - 1));
                    gopp::string u = gopp::string(std::string(usage.substr(0, i)) + name.str() + std::string(usage.substr(j + 1)));
                    return {name, u};
                }
            }
            break;  // only one back quote; use the type's name
        }
    }
    gopp::string name = flag->Value->Type();
    std::string_view n = name.view();
    if (n == "bool" || n == "boolfunc") name = gopp::string();
    else if (n == "func") name = "value"_s;
    else if (n == "float64") name = "float"_s;
    else if (n == "int64") name = "int"_s;
    else if (n == "uint64") name = "uint"_s;
    else if (n == "stringSlice") name = "strings"_s;
    else if (n == "intSlice") name = "ints"_s;
    else if (n == "uintSlice") name = "uints"_s;
    else if (n == "boolSlice") name = "bools"_s;
    return {name, flag->Usage};
}

gopp::string FlagSet::FlagUsagesWrapped(int cols) const {
    std::vector<std::string> lines;
    std::size_t maxlen = 0;
    VisitAll([&](Flag* flag) {
        if (flag->Hidden) return;
        std::string line;
        if (!flag->Shorthand.empty() && flag->ShorthandDeprecated.empty()) {
            line = std::format("  -{}, --{}", flag->Shorthand, flag->Name);
        } else {
            line = std::format("      --{}", flag->Name);
        }
        auto [varname, usage] = UnquoteUsage(flag);
        if (!varname.empty()) line += " " + varname.str();
        if (!flag->NoOptDefVal.empty()) {
            std::string_view t = flag->Value->Type().view();
            if (t == "string") {
                line += std::format("[=\"{}\"]", flag->NoOptDefVal);
            } else if (t == "bool" || t == "boolfunc") {
                if (flag->NoOptDefVal != "true") line += std::format("[={}]", flag->NoOptDefVal);
            } else if (t == "count") {
                if (flag->NoOptDefVal != "+1") line += std::format("[={}]", flag->NoOptDefVal);
            } else {
                line += std::format("[={}]", flag->NoOptDefVal);
            }
        }
        // This special character is replaced with spacing once the correct
        // alignment is calculated.
        line += '\x00';
        maxlen = std::max(maxlen, line.size());
        line += usage.view();
        if (!defaultIsZeroValue(flag)) {
            if (flag->Value->Type() == "string") line += std::format(" (default {})", strconv::Quote(flag->DefValue));
            else line += std::format(" (default {})", flag->DefValue);
        }
        if (!flag->Deprecated.empty()) line += std::format(" (DEPRECATED: {})", flag->Deprecated);
        lines.push_back(std::move(line));
    });
    std::string buf;
    for (auto& line : lines) {
        auto sidx = line.find('\x00');
        std::string spacing(maxlen - sidx, ' ');
        // fmt.Fprintln puts spaces between its operands.
        buf += line.substr(0, sidx) + " " + spacing + " " + wrap(maxlen + 2, static_cast<std::size_t>(std::max(cols, 0)), line.substr(sidx + 1)) +
               "\n";
    }
    return gopp::string(buf);
}

gopp::string FlagSet::Arg(isize i) const {
    if (i < 0 || i >= static_cast<isize>(args_.size())) return {};
    return args_[static_cast<std::size_t>(i)];
}

void FlagSet::Var(ptr<pflag::Value> value, std::string_view name, std::string_view usage) {
    VarP(std::move(value), name, {}, usage);
}

void FlagSet::VarP(ptr<pflag::Value> value, std::string_view name, std::string_view shorthand, std::string_view usage) {
    VarPF(std::move(value), name, shorthand, usage);
}

Flag* FlagSet::VarPF(ptr<pflag::Value> value, std::string_view name, std::string_view shorthand, std::string_view usage) {
    // Remember the default value as a string; it won't change.
    auto flag = make_ptr<Flag>();
    flag->Name = gopp::string(name);
    flag->Shorthand = gopp::string(shorthand);
    flag->Usage = gopp::string(usage);
    flag->DefValue = value->String();
    flag->Value = std::move(value);
    AddFlag(flag.get());
    return flag.get();
}

void FlagSet::AddFlag(Flag* flag) {
    ptr<Flag> keep(flag);
    NormalizedName normalizedFlagName = normalizeFlagName(flag->Name.view());
    if (formal_.contains(normalizedFlagName)) {
        auto msg = fmt::Sprint("{} flag redefined: {}", name_, flag->Name);
        fmt::Println(Output(), "{}", msg);
        panic(msg.view());  // happens only if flags are declared with identical names
    }
    flag->Name = normalizedFlagName;
    formal_[normalizedFlagName] = keep;
    orderedFormal_.push_back(flag);

    if (flag->Shorthand.empty()) return;
    if (flag->Shorthand.size() > 1) {
        auto msg = fmt::Sprint("{} shorthand is more than one ASCII character", strconv::Quote(flag->Shorthand));
        io::WriteString(Output(), msg);
        panic(msg.view());
    }
    char c = flag->Shorthand[0];
    if (auto it = shorthands_.find(c); it != shorthands_.end()) {
        auto msg = fmt::Sprint("unable to redefine {} shorthand in {} flagset: it's already used for {} flag", quoteChar(c),
                               strconv::Quote(name_), strconv::Quote(it->second->Name));
        io::WriteString(Output(), msg);
        panic(msg.view());
    }
    shorthands_[c] = flag;
}

void FlagSet::AddFlagSet(const FlagSet* newSet) {
    if (!newSet) return;
    newSet->VisitAll([&](Flag* flag) {
        if (!Lookup(flag->Name.view())) AddFlag(flag);
    });
}

gopp::error FlagSet::fail(gopp::error err) {
    if (errorHandling_ != ContinueOnError) usage();
    return err;
}

// The usage message of CommandLine.
std::function<void()> Usage = [] {
    fmt::Print(os::Stderr, "Usage of {}:\n", os::Args().empty() ? gopp::string() : os::Args()[0]);
    PrintDefaults();
};

void FlagSet::usage() {
    if (this == &CommandLine()) {
        if (pflag::Usage) pflag::Usage();
    } else if (!Usage) {
        fmt::Print(Output(), "Usage of {}:\n", name_);
        PrintDefaults();
    } else {
        Usage();
    }
}

gopp::error FlagSet::parseLongArg(const gopp::string& s, std::span<const gopp::string>& args,
                                  const std::function<gopp::error(Flag*, gopp::string)>& fn) {
    gopp::string name = s.slice(2);
    if (name.empty() || name[0] == '-' || name[0] == '=') return fail(make_ptr<InvalidSyntaxError>(s));

    gopp::string value;
    bool hasValue = false;
    if (auto eq = name.find('='); eq != gopp::string::npos) {
        value = name.slice(eq + 1);
        name = name.slice(0, eq);
        hasValue = true;
    }
    auto it = formal_.find(normalizeFlagName(name.view()));
    if (it == formal_.end()) {
        if (name == "help") {  // special case for nice help message
            usage();
            return ErrHelp;
        }
        if (ParseErrorsWhitelist.UnknownFlags || ParseErrorsAllowlist.UnknownFlags) {
            // --unknown=unknownval arg ...: we do not want to lose arg in
            // this case.
            if (hasValue) return nullptr;
            args = stripUnknownFlagValue(args);
            return nullptr;
        }
        return fail(make_ptr<NotExistError>(name, gopp::string(), NotExistError::flagUnknownFlag));
    }
    ptr<Flag> flag = it->second;
    if (hasValue) {
        // '--flag=arg'
    } else if (!flag->NoOptDefVal.empty()) {
        // '--flag' (arg was optional)
        value = flag->NoOptDefVal;
    } else if (!args.empty()) {
        // '--flag arg'
        value = args[0];
        args = args.subspan(1);
    } else {
        // '--flag' (arg was required)
        return fail(make_ptr<ValueRequiredError>(flag, name, gopp::string()));
    }
    auto err = fn(flag.get(), value);
    if (err) fail(err);
    return err;
}

gopp::error FlagSet::parseSingleShortArg(gopp::string& shorthands, std::span<const gopp::string>& args,
                                         const std::function<gopp::error(Flag*, gopp::string)>& fn) {
    if (isGotestShorthandFlag(shorthands.view())) {
        shorthands = gopp::string();
        return nullptr;
    }
    gopp::string all = shorthands;
    shorthands = all.slice(1);
    char c = all[0];

    auto it = shorthands_.find(c);
    if (it == shorthands_.end()) {
        if (c == 'h') {  // special case for nice help message
            usage();
            return ErrHelp;
        }
        if (ParseErrorsWhitelist.UnknownFlags || ParseErrorsAllowlist.UnknownFlags) {
            // '-f=arg arg ...': we do not want to lose arg in this case.
            if (all.size() > 2 && all[1] == '=') {
                shorthands = gopp::string();
                return nullptr;
            }
            args = stripUnknownFlagValue(args);
            return nullptr;
        }
        return fail(make_ptr<NotExistError>(gopp::string(std::string_view(&c, 1)), all, NotExistError::flagUnknownShorthandFlag));
    }
    ptr<Flag> flag(it->second);

    gopp::string value;
    if (all.size() > 2 && all[1] == '=') {
        // '-f=arg'
        value = all.slice(2);
        shorthands = gopp::string();
    } else if (!flag->NoOptDefVal.empty()) {
        // '-f' (arg was optional)
        value = flag->NoOptDefVal;
    } else if (all.size() > 1) {
        // '-farg'
        value = all.slice(1);
        shorthands = gopp::string();
    } else if (!args.empty()) {
        // '-f arg'
        value = args[0];
        args = args.subspan(1);
    } else {
        // '-f' (arg was required)
        return fail(make_ptr<ValueRequiredError>(flag, gopp::string(std::string_view(&c, 1)), all));
    }

    if (!flag->ShorthandDeprecated.empty()) {
        fmt::Print(Output(), "Flag shorthand -{} has been deprecated, {}\n", flag->Shorthand, flag->ShorthandDeprecated);
    }
    auto err = fn(flag.get(), value);
    if (err) fail(err);
    return err;
}

gopp::error FlagSet::parseShortArg(const gopp::string& s, std::span<const gopp::string>& args,
                                   const std::function<gopp::error(Flag*, gopp::string)>& fn) {
    gopp::string shorthands = s.slice(1);
    // As in pflag, each shorthand starts from the arguments after s: the
    // last one that takes a value from them decides what's left.
    std::span<const gopp::string> orig = args;
    while (!shorthands.empty()) {
        std::span<const gopp::string> a = orig;
        auto err = parseSingleShortArg(shorthands, a, fn);
        args = a;
        if (err) return err;
    }
    return nullptr;
}

gopp::error FlagSet::parseArgs(std::span<const gopp::string> args, const std::function<gopp::error(Flag*, gopp::string)>& fn) {
    while (!args.empty()) {
        const gopp::string s = args[0];
        args = args.subspan(1);
        if (s.empty() || s[0] != '-' || s.size() == 1) {
            if (!interspersed_) {
                args_.push_back(s);
                args_.insert(args_.end(), args.begin(), args.end());
                return nullptr;
            }
            args_.push_back(s);
            continue;
        }
        gopp::error err;
        if (s[1] == '-') {
            if (s.size() == 2) {  // "--" terminates the flags
                argsLenAtDash_ = static_cast<isize>(args_.size());
                args_.insert(args_.end(), args.begin(), args.end());
                break;
            }
            err = parseLongArg(s, args, fn);
        } else {
            err = parseShortArg(s, args, fn);
        }
        if (err) return err;
    }
    return nullptr;
}

gopp::error FlagSet::handleParseError(gopp::error err) {
    if (!err) return nullptr;
    switch (errorHandling_) {
    case ContinueOnError: return err;
    case ExitOnError:
        if (err == ErrHelp) os::Exit(0);
        fmt::Println(Output(), "{}", err);
        os::Exit(2);
    case PanicOnError: panic(err.Error().view());
    }
    return err;
}

gopp::error FlagSet::Parse(std::span<const gopp::string> arguments) {
    parsed_ = true;
    args_.clear();
    args_.reserve(arguments.size());
    if (arguments.empty()) return nullptr;
    auto set = [this](Flag* flag, gopp::string value) { return Set(flag->Name.view(), std::move(value)); };
    return handleParseError(parseArgs(arguments, set));
}

gopp::error FlagSet::ParseAll(std::span<const gopp::string> arguments, const std::function<gopp::error(Flag*, gopp::string)>& fn) {
    parsed_ = true;
    args_.clear();
    args_.reserve(arguments.size());
    return handleParseError(parseArgs(arguments, fn));
}

// ---------------------------------------------------------------------------
// CommandLine

FlagSet& CommandLine() {
    // Never freed, so that it outlives static destructors that use it.
    static FlagSet* cl = [] {
        auto* f = new FlagSet(os::Args().empty() ? gopp::string() : os::Args()[0], ExitOnError);
        f->addRef();
        return f;
    }();
    return *cl;
}

void VisitAll(const std::function<void(Flag*)>& fn) { CommandLine().VisitAll(fn); }
void Visit(const std::function<void(Flag*)>& fn) { CommandLine().Visit(fn); }
Flag* Lookup(std::string_view name) { return CommandLine().Lookup(name); }
Flag* ShorthandLookup(std::string_view name) { return CommandLine().ShorthandLookup(name); }
gopp::error Set(std::string_view name, gopp::string value) { return CommandLine().Set(name, std::move(value)); }
void PrintDefaults() { CommandLine().PrintDefaults(); }
isize NFlag() { return CommandLine().NFlag(); }
gopp::string Arg(isize i) { return CommandLine().Arg(i); }
isize NArg() { return CommandLine().NArg(); }
const std::vector<gopp::string>& Args() { return CommandLine().Args(); }
void Var(ptr<pflag::Value> value, std::string_view name, std::string_view usage) { CommandLine().VarP(std::move(value), name, {}, usage); }
void VarP(ptr<pflag::Value> value, std::string_view name, std::string_view shorthand, std::string_view usage) {
    CommandLine().VarP(std::move(value), name, shorthand, usage);
}

void Parse() {
    // Ignore errors; CommandLine is set for ExitOnError.
    std::span<const gopp::string> args = os::Args();
    CommandLine().Parse(args.empty() ? args : args.subspan(1));
}

void ParseAll(const std::function<gopp::error(Flag*, gopp::string)>& fn) {
    std::span<const gopp::string> args = os::Args();
    CommandLine().ParseAll(args.empty() ? args : args.subspan(1), fn);
}

void SetInterspersed(bool interspersed) { CommandLine().SetInterspersed(interspersed); }
bool Parsed() { return CommandLine().Parsed(); }

// ---------------------------------------------------------------------------
// Errors

gopp::string NotExistError::Error() const {
    switch (messageType_) {
    case flagNotExist: return fmt::Sprint("flag {} does not exist", strconv::Quote(name_));
    case flagNotDefined: return fmt::Sprint("flag accessed but not defined: {}", name_);
    case flagNoSuchFlag: return fmt::Sprint("no such flag -{}", name_);
    case flagUnknownFlag: return fmt::Sprint("unknown flag: --{}", name_);
    case flagUnknownShorthandFlag:
        return fmt::Sprint("unknown shorthand flag: {} in -{}", quoteChar(name_.empty() ? '\0' : name_[0]), specifiedShorthands_);
    }
    panic("unknown flagNotExistErrorMessageType");
}

gopp::string ValueRequiredError::Error() const {
    if (!specifiedShorthands_.empty()) {
        return fmt::Sprint("flag needs an argument: {} in -{}", quoteChar(specifiedName_.empty() ? '\0' : specifiedName_[0]),
                           specifiedShorthands_);
    }
    return fmt::Sprint("flag needs an argument: --{}", specifiedName_);
}

gopp::string InvalidValueError::Error() const {
    std::string flagName;
    if (!flag_->Shorthand.empty() && flag_->ShorthandDeprecated.empty()) {
        flagName = std::format("-{}, --{}", flag_->Shorthand, flag_->Name);
    } else {
        flagName = std::format("--{}", flag_->Name);
    }
    return fmt::Sprint("invalid argument {} for {} flag: {}", strconv::Quote(value_), strconv::Quote(flagName), cause_);
}

gopp::string InvalidSyntaxError::Error() const { return fmt::Sprint("bad flag syntax: {}", specifiedFlag_); }

}  // namespace gopp::pflag
