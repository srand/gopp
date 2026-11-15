// The flag package, ported from Go 1.27.3's flag/flag.go.
#include <gopp/flag.hpp>
#include <gopp/fmt.hpp>
#include <gopp/os.hpp>
#include <gopp/strings.hpp>

#include <cxxabi.h>

#include <cstdlib>
#include <exception>
#include <typeinfo>

namespace gopp::flag {

const gopp::error ErrHelp = errors::New("flag: help requested");

namespace {

// errParse and errRange: the errors of a value that doesn't parse, or is
// out of range for its type.
const gopp::error errParse = errors::New("parse error");
const gopp::error errRange = errors::New("value out of range");

// Go's numError: strconv's errors, as errParse and errRange.
gopp::error numError(const gopp::error& err) {
    auto* ne = dynamic_cast<const strconv::NumError*>(err.get());
    if (!ne) return err;
    if (ne->Err == strconv::ErrSyntax) return errParse;
    if (ne->Err == strconv::ErrRange) return errRange;
    return err;
}

// The values of the flags of each type: a variable of the caller's, or
// one of their own (for the plain forms).
template <typename T>
class valueOf : public Getter {
public:
    valueOf(T* p, T val) : p_(p ? p : &own_) { *p_ = std::move(val); }
    std::any Get() const override { return *p_; }
    T* ptr() noexcept { return p_; }

protected:
    T own_{};
    T* p_;
};

class boolValue final : public valueOf<bool>, public BoolFlag {
public:
    using valueOf::valueOf;
    gopp::error Set(gopp::string s) override {
        auto [v, err] = strconv::ParseBool(s);
        *p_ = v;
        return err ? errParse : nullptr;
    }
    gopp::string String() const override { return string::fromStatic(*p_ ? "true" : "false"); }
    bool IsBoolFlag() const override { return true; }
};

class intValue final : public valueOf<isize> {
public:
    using valueOf::valueOf;
    gopp::error Set(gopp::string s) override {
        auto [v, err] = strconv::ParseInt(s, 0, 64);
        *p_ = static_cast<isize>(v);
        return err ? numError(err) : nullptr;
    }
    gopp::string String() const override { return strconv::Itoa(*p_); }
};

class int64Value final : public valueOf<int64> {
public:
    using valueOf::valueOf;
    gopp::error Set(gopp::string s) override {
        auto [v, err] = strconv::ParseInt(s, 0, 64);
        *p_ = v;
        return err ? numError(err) : nullptr;
    }
    gopp::string String() const override { return strconv::Itoa(*p_); }
};

class uintValue final : public valueOf<std::size_t> {
public:
    using valueOf::valueOf;
    gopp::error Set(gopp::string s) override {
        auto [v, err] = strconv::ParseUint(s, 0, 64);
        *p_ = static_cast<std::size_t>(v);
        return err ? numError(err) : nullptr;
    }
    gopp::string String() const override { return strconv::FormatUint(*p_, 10); }
};

class uint64Value final : public valueOf<uint64> {
public:
    using valueOf::valueOf;
    gopp::error Set(gopp::string s) override {
        auto [v, err] = strconv::ParseUint(s, 0, 64);
        *p_ = v;
        return err ? numError(err) : nullptr;
    }
    gopp::string String() const override { return strconv::FormatUint(*p_, 10); }
};

class stringValue final : public valueOf<gopp::string> {
public:
    using valueOf::valueOf;
    gopp::error Set(gopp::string s) override {
        *p_ = std::move(s);
        return nullptr;
    }
    gopp::string String() const override { return *p_; }
};

class float64Value final : public valueOf<double> {
public:
    using valueOf::valueOf;
    gopp::error Set(gopp::string s) override {
        auto [v, err] = strconv::ParseFloat(s, 64);
        *p_ = v;
        return err ? numError(err) : nullptr;
    }
    gopp::string String() const override { return strconv::FormatFloat(*p_, 'g', -1, 64); }
};

class durationValue final : public valueOf<time::Duration> {
public:
    using valueOf::valueOf;
    gopp::error Set(gopp::string s) override {
        auto [v, err] = time::ParseDuration(s);
        *p_ = v;
        return err ? errParse : nullptr;
    }
    gopp::string String() const override { return p_->String(); }
};

class textValue final : public Getter {
public:
    textValue(std::function<gopp::string()> str, std::function<gopp::error(gopp::string)> set, std::function<std::any()> get)
        : str_(std::move(str)), set_(std::move(set)), get_(std::move(get)) {}
    gopp::error Set(gopp::string s) override { return set_(std::move(s)); }
    gopp::string String() const override { return str_(); }
    std::any Get() const override { return get_(); }

private:
    std::function<gopp::string()> str_;
    std::function<gopp::error(gopp::string)> set_;
    std::function<std::any()> get_;
};

class funcValue : public virtual Value {
public:
    explicit funcValue(std::function<gopp::error(gopp::string)> fn) : fn_(std::move(fn)) {}
    gopp::error Set(gopp::string s) override { return fn_(std::move(s)); }
    gopp::string String() const override { return {}; }

private:
    std::function<gopp::error(gopp::string)> fn_;
};

class boolFuncValue final : public funcValue, public BoolFlag {
public:
    using funcValue::funcValue;
    bool IsBoolFlag() const override { return true; }
};

// The zero Value's String of each type, for PrintDefaults.
std::function<gopp::string()> zeroOf(std::string_view s) {
    return [z = gopp::string(s)] { return z; };
}

// Whether value is the String of a zero Value of the flag's type; an
// error if making it threw, as Go's isZeroValue reports a panic.
std::pair<bool, gopp::error> isZeroValue(const Flag& flag, std::string_view value) {
    if (!flag.ZeroString) return {value.empty(), nullptr};
    std::string what;
    try {
        return {value == flag.ZeroString(), nullptr};
    } catch (const gopp::error& e) {
        what = e.Error().str();
    } catch (const std::exception& e) {
        what = e.what();
    }
    const Value& v = *flag.Value;
    int status = 0;
    char* demangled = abi::__cxa_demangle(typeid(v).name(), nullptr, nullptr, &status);
    std::string type = status == 0 && demangled ? demangled : typeid(v).name();
    std::free(demangled);
    return {false, errors::New("panic calling String method on zero " + type + " for flag " + flag.Name.str() + ": " + what)};
}

}  // namespace

namespace detail {

ptr<Getter> newTextValue(std::function<gopp::string()> str, std::function<gopp::error(gopp::string)> set,
                         std::function<std::any()> get) {
    return make_ptr<textValue>(std::move(str), std::move(set), std::move(get));
}

}  // namespace detail

// ---------------------------------------------------------------------------
// FlagSet

FlagSet::FlagSet(gopp::string name, flag::ErrorHandling errorHandling)
    : name_(std::move(name)), errorHandling_(errorHandling) {
    Usage = [this] { defaultUsage(); };
}

ptr<FlagSet> NewFlagSet(gopp::string name, ErrorHandling errorHandling) {
    return make_ptr<FlagSet>(std::move(name), errorHandling);
}

void FlagSet::Init(gopp::string name, flag::ErrorHandling errorHandling) {
    name_ = std::move(name);
    errorHandling_ = errorHandling;
}

io::Writer& FlagSet::Output() const {
    if (!output_) return os::Stderr;
    return *output_;
}

void FlagSet::VisitAll(const std::function<void(Flag*)>& fn) const {
    for (auto& [_, f] : formal_) fn(f.get());  // the map is sorted
}

void FlagSet::Visit(const std::function<void(Flag*)>& fn) const {
    for (auto& [_, f] : actual_) fn(f);
}

Flag* FlagSet::Lookup(std::string_view name) const {
    auto it = formal_.find(name);
    return it == formal_.end() ? nullptr : it->second.get();
}

gopp::error FlagSet::Set(std::string_view name, std::string_view value, std::source_location loc) {
    auto it = formal_.find(name);
    if (it == formal_.end()) {
        // Remembered, to panic if the flag is defined later.
        undef_.insert_or_assign(gopp::string(name), gopp::string(std::string(loc.file_name()) + ":" + std::to_string(loc.line())));
        return fmt::Errorf("no such flag -{}", name);
    }
    if (gopp::error err = it->second->Value->Set(gopp::string(value))) return err;
    actual_.insert_or_assign(it->first, it->second.get());
    return nullptr;
}

std::pair<gopp::string, gopp::string> UnquoteUsage(const Flag& flag) {
    // The first `quoted` word names the value.
    std::string_view usage = flag.Usage;
    if (auto i = usage.find('`'); i != usage.npos) {
        if (auto j = usage.find('`', i + 1); j != usage.npos) {
            gopp::string name(usage.substr(i + 1, j - i - 1));
            return {name, gopp::string(std::string(usage.substr(0, i)) + name.str() + std::string(usage.substr(j + 1)))};
        }
        // Only one back quote: the type's name.
    }
    std::string_view name = "value";
    Value* v = flag.Value.get();
    if (auto* bf = dynamic_cast<BoolFlag*>(v); bf && bf->IsBoolFlag()) name = "";
    else if (dynamic_cast<durationValue*>(v)) name = "duration";
    else if (dynamic_cast<float64Value*>(v)) name = "float";
    else if (dynamic_cast<intValue*>(v) || dynamic_cast<int64Value*>(v)) name = "int";
    else if (dynamic_cast<stringValue*>(v)) name = "string";
    else if (dynamic_cast<uintValue*>(v) || dynamic_cast<uint64Value*>(v)) name = "uint";
    return {gopp::string(name), flag.Usage};
}

void FlagSet::PrintDefaults() const {
    std::vector<gopp::error> isZeroValueErrs;
    VisitAll([&](Flag* flag) {
        std::string b = "  -" + flag->Name.str();  // two spaces before -
        auto [name, usage] = UnquoteUsage(*flag);
        if (!name.view().empty()) {
            b += " ";
            b += name.view();
        }
        // A one-letter boolean flag fits on its line; others start a new
        // one, indented by four spaces before the tab.
        if (b.size() <= 4) b += "\t";
        else b += "\n    \t";
        b += strings::ReplaceAll(usage, "\n", "\n    \t").view();
        if (auto [isZero, err] = isZeroValue(*flag, flag->DefValue); err) {
            isZeroValueErrs.push_back(err);
        } else if (!isZero) {
            if (dynamic_cast<stringValue*>(flag->Value.get())) b += " (default " + strconv::Quote(flag->DefValue).str() + ")";
            else b += " (default " + flag->DefValue.str() + ")";
        }
        io::WriteString(Output(), b + "\n");
    });
    if (!isZeroValueErrs.empty()) {
        io::WriteString(Output(), "\n");
        for (auto& err : isZeroValueErrs) io::WriteString(Output(), err.Error().str() + "\n");
    }
}

void FlagSet::defaultUsage() {
    if (name_.view().empty()) io::WriteString(Output(), "Usage:\n");
    else io::WriteString(Output(), "Usage of " + name_.str() + ":\n");
    PrintDefaults();
}

gopp::string FlagSet::Arg(isize i) const {
    if (i < 0 || i >= NArg()) return {};
    return args_[static_cast<std::size_t>(i)];
}

void FlagSet::BoolVar(bool* p, std::string_view name, bool value, std::string_view usage) {
    define(make_ptr<boolValue>(p, value), name, usage, zeroOf("false"));
}
bool* FlagSet::Bool(std::string_view name, bool value, std::string_view usage) {
    auto v = make_ptr<boolValue>(nullptr, value);
    define(v, name, usage, zeroOf("false"));
    return v->ptr();
}
void FlagSet::IntVar(isize* p, std::string_view name, isize value, std::string_view usage) {
    define(make_ptr<intValue>(p, value), name, usage, zeroOf("0"));
}
isize* FlagSet::Int(std::string_view name, isize value, std::string_view usage) {
    auto v = make_ptr<intValue>(nullptr, value);
    define(v, name, usage, zeroOf("0"));
    return v->ptr();
}
void FlagSet::Int64Var(int64* p, std::string_view name, int64 value, std::string_view usage) {
    define(make_ptr<int64Value>(p, value), name, usage, zeroOf("0"));
}
int64* FlagSet::Int64(std::string_view name, int64 value, std::string_view usage) {
    auto v = make_ptr<int64Value>(nullptr, value);
    define(v, name, usage, zeroOf("0"));
    return v->ptr();
}
void FlagSet::UintVar(std::size_t* p, std::string_view name, std::size_t value, std::string_view usage) {
    define(make_ptr<uintValue>(p, value), name, usage, zeroOf("0"));
}
std::size_t* FlagSet::Uint(std::string_view name, std::size_t value, std::string_view usage) {
    auto v = make_ptr<uintValue>(nullptr, value);
    define(v, name, usage, zeroOf("0"));
    return v->ptr();
}
void FlagSet::Uint64Var(uint64* p, std::string_view name, uint64 value, std::string_view usage) {
    define(make_ptr<uint64Value>(p, value), name, usage, zeroOf("0"));
}
uint64* FlagSet::Uint64(std::string_view name, uint64 value, std::string_view usage) {
    auto v = make_ptr<uint64Value>(nullptr, value);
    define(v, name, usage, zeroOf("0"));
    return v->ptr();
}
void FlagSet::StringVar(gopp::string* p, std::string_view name, gopp::string value, std::string_view usage) {
    define(make_ptr<stringValue>(p, std::move(value)), name, usage, zeroOf(""));
}
gopp::string* FlagSet::String(std::string_view name, gopp::string value, std::string_view usage) {
    auto v = make_ptr<stringValue>(nullptr, std::move(value));
    define(v, name, usage, zeroOf(""));
    return v->ptr();
}
void FlagSet::Float64Var(double* p, std::string_view name, double value, std::string_view usage) {
    define(make_ptr<float64Value>(p, value), name, usage, zeroOf("0"));
}
double* FlagSet::Float64(std::string_view name, double value, std::string_view usage) {
    auto v = make_ptr<float64Value>(nullptr, value);
    define(v, name, usage, zeroOf("0"));
    return v->ptr();
}
void FlagSet::DurationVar(time::Duration* p, std::string_view name, time::Duration value, std::string_view usage) {
    define(make_ptr<durationValue>(p, value), name, usage, zeroOf("0s"));
}
time::Duration* FlagSet::Duration(std::string_view name, time::Duration value, std::string_view usage) {
    auto v = make_ptr<durationValue>(nullptr, value);
    define(v, name, usage, zeroOf("0s"));
    return v->ptr();
}

void FlagSet::Func(std::string_view name, std::string_view usage, std::function<gopp::error(gopp::string)> fn) {
    define(make_ptr<funcValue>(std::move(fn)), name, usage, zeroOf(""));
}

void FlagSet::BoolFunc(std::string_view name, std::string_view usage, std::function<gopp::error(gopp::string)> fn) {
    define(make_ptr<boolFuncValue>(std::move(fn)), name, usage, zeroOf(""));
}

gopp::string FlagSet::sprintln(gopp::string msg) const {
    io::WriteString(Output(), msg.str() + "\n");
    return msg;
}

void FlagSet::define(ptr<flag::Value> value, std::string_view name, std::string_view usage, std::function<gopp::string()> zero) {
    if (name.starts_with("-")) panic(sprintln("flag " + strconv::Quote(name) + " begins with -").view());
    if (name.find('=') != name.npos) panic(sprintln("flag " + strconv::Quote(name) + " contains =").view());
    if (formal_.contains(name)) {
        gopp::string msg = name_.view().empty() ? "flag redefined: " + gopp::string(name)
                                                : name_ + " flag redefined: " + gopp::string(name);
        panic(sprintln(msg).view());  // happens only if flags are declared with identical names
    }
    if (auto it = undef_.find(name); it != undef_.end()) {
        panic(("flag " + std::string(name) + " set at " + it->second.str() + " before being defined").c_str());
    }
    auto f = std::make_unique<Flag>();
    f->Name = gopp::string(name);
    f->Usage = gopp::string(usage);
    f->DefValue = value->String();
    f->Value = std::move(value);
    f->ZeroString = std::move(zero);
    formal_.emplace(f->Name, std::move(f));
}

gopp::error FlagSet::failf(gopp::string msg) {
    sprintln(msg);
    usage();
    return errors::New(msg);
}

void FlagSet::usage() {
    if (Usage) Usage();
    else defaultUsage();
}

std::pair<bool, gopp::error> FlagSet::parseOne() {
    if (args_.empty()) return {false, nullptr};
    gopp::string s = args_[0];
    if (s.size() < 2 || s[0] != '-') return {false, nullptr};
    std::size_t numMinuses = 1;
    if (s[1] == '-') {
        numMinuses++;
        if (s.size() == 2) {  // "--" ends the flags
            args_.erase(args_.begin());
            return {false, nullptr};
        }
    }
    std::string_view name = s.view().substr(numMinuses);
    if (name.empty() || name[0] == '-' || name[0] == '=') return {false, failf("bad flag syntax: " + s)};

    // A flag.
    args_.erase(args_.begin());
    bool hasValue = false;
    gopp::string value;
    if (auto eq = name.find('=', 1); eq != name.npos) {  // = can't be first
        value = gopp::string(name.substr(eq + 1));
        hasValue = true;
        name = name.substr(0, eq);
    }

    auto it = formal_.find(name);
    if (it == formal_.end()) {
        if (name == "help" || name == "h") {  // a nice help message
            usage();
            return {false, ErrHelp};
        }
        return {false, failf("flag provided but not defined: -" + gopp::string(name))};
    }
    Flag* flag = it->second.get();
    if (auto* fv = dynamic_cast<BoolFlag*>(flag->Value.get()); fv && fv->IsBoolFlag()) {  // needs no argument
        if (hasValue) {
            if (gopp::error err = fv->Set(value)) {
                return {false, failf("invalid boolean value " + strconv::Quote(value) + " for -" + gopp::string(name) + ": " + err.Error())};
            }
        } else if (gopp::error err = fv->Set(string::fromStatic("true"))) {
            return {false, failf("invalid boolean flag " + gopp::string(name) + ": " + err.Error())};
        }
    } else {
        // It needs a value: the next argument.
        if (!hasValue && !args_.empty()) {
            hasValue = true;
            value = args_[0];
            args_.erase(args_.begin());
        }
        if (!hasValue) return {false, failf("flag needs an argument: -" + gopp::string(name))};
        if (gopp::error err = flag->Value->Set(value)) {
            return {false, failf("invalid value " + strconv::Quote(value) + " for flag -" + gopp::string(name) + ": " + err.Error())};
        }
    }
    actual_.insert_or_assign(it->first, flag);
    return {true, nullptr};
}

gopp::error FlagSet::Parse(std::span<const gopp::string> arguments) {
    parsed_ = true;
    args_.assign(arguments.begin(), arguments.end());
    for (;;) {
        auto [seen, err] = parseOne();
        if (seen) continue;
        if (!err) break;
        switch (errorHandling_) {
        case ContinueOnError: return err;
        case ExitOnError:
            if (err == ErrHelp) os::Exit(0);
            os::Exit(2);
        case PanicOnError: panic(err.Error().view());
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// CommandLine

void detail::defaultUsage() {
    const auto& a = os::Args();
    io::WriteString(CommandLine().Output(), "Usage of " + (a.empty() ? std::string() : a[0].str()) + ":\n");
    PrintDefaults();
}

std::function<void()> Usage = detail::defaultUsage;

namespace {

ptr<FlagSet> newCommandLine(ErrorHandling errorHandling) {
    const auto& a = os::Args();
    auto f = make_ptr<FlagSet>(a.empty() ? gopp::string() : a[0], errorHandling);
    f->Usage = [] { flag::Usage(); };
    return f;
}

ptr<FlagSet>& commandLine() {
    static ptr<FlagSet> fs = newCommandLine(ExitOnError);
    return fs;
}

}  // namespace

FlagSet& CommandLine() { return *commandLine(); }

void detail::resetForTesting(std::function<void()> usage) {
    struct discard final : io::Writer {
        std::pair<isize, gopp::error> Write(std::span<const byte> p) override { return {static_cast<isize>(p.size()), nullptr}; }
    };
    commandLine() = newCommandLine(ContinueOnError);
    commandLine()->SetOutput(make_ptr<discard>());
    flag::Usage = std::move(usage);
}

void VisitAll(const std::function<void(Flag*)>& fn) { CommandLine().VisitAll(fn); }
void Visit(const std::function<void(Flag*)>& fn) { CommandLine().Visit(fn); }
Flag* Lookup(std::string_view name) { return CommandLine().Lookup(name); }
gopp::error Set(std::string_view name, std::string_view value, std::source_location loc) {
    return CommandLine().Set(name, value, loc);
}
void PrintDefaults() { CommandLine().PrintDefaults(); }
isize NFlag() { return CommandLine().NFlag(); }
gopp::string Arg(isize i) { return CommandLine().Arg(i); }
isize NArg() { return CommandLine().NArg(); }
const std::vector<gopp::string>& Args() { return CommandLine().Args(); }

void BoolVar(bool* p, std::string_view name, bool value, std::string_view usage) { CommandLine().BoolVar(p, name, value, usage); }
bool* Bool(std::string_view name, bool value, std::string_view usage) { return CommandLine().Bool(name, value, usage); }
void IntVar(isize* p, std::string_view name, isize value, std::string_view usage) { CommandLine().IntVar(p, name, value, usage); }
isize* Int(std::string_view name, isize value, std::string_view usage) { return CommandLine().Int(name, value, usage); }
void Int64Var(int64* p, std::string_view name, int64 value, std::string_view usage) {
    CommandLine().Int64Var(p, name, value, usage);
}
int64* Int64(std::string_view name, int64 value, std::string_view usage) { return CommandLine().Int64(name, value, usage); }
void UintVar(std::size_t* p, std::string_view name, std::size_t value, std::string_view usage) {
    CommandLine().UintVar(p, name, value, usage);
}
std::size_t* Uint(std::string_view name, std::size_t value, std::string_view usage) {
    return CommandLine().Uint(name, value, usage);
}
void Uint64Var(uint64* p, std::string_view name, uint64 value, std::string_view usage) {
    CommandLine().Uint64Var(p, name, value, usage);
}
uint64* Uint64(std::string_view name, uint64 value, std::string_view usage) { return CommandLine().Uint64(name, value, usage); }
void StringVar(gopp::string* p, std::string_view name, gopp::string value, std::string_view usage) {
    CommandLine().StringVar(p, name, std::move(value), usage);
}
gopp::string* String(std::string_view name, gopp::string value, std::string_view usage) {
    return CommandLine().String(name, std::move(value), usage);
}
void Float64Var(double* p, std::string_view name, double value, std::string_view usage) {
    CommandLine().Float64Var(p, name, value, usage);
}
double* Float64(std::string_view name, double value, std::string_view usage) { return CommandLine().Float64(name, value, usage); }
void DurationVar(time::Duration* p, std::string_view name, time::Duration value, std::string_view usage) {
    CommandLine().DurationVar(p, name, value, usage);
}
time::Duration* Duration(std::string_view name, time::Duration value, std::string_view usage) {
    return CommandLine().Duration(name, value, usage);
}
void Func(std::string_view name, std::string_view usage, std::function<gopp::error(gopp::string)> fn) {
    CommandLine().Func(name, usage, std::move(fn));
}
void BoolFunc(std::string_view name, std::string_view usage, std::function<gopp::error(gopp::string)> fn) {
    CommandLine().BoolFunc(name, usage, std::move(fn));
}

void Parse() {
    const auto& a = os::Args();
    CommandLine().Parse(std::span<const gopp::string>(a).subspan(a.empty() ? 0 : 1));
}

void Parse(int argc, char** argv) {
    os::SetArgs(argc, argv);
    const auto& a = os::Args();
    FlagSet& fs = CommandLine();
    if (fs.Name().view().empty() && !a.empty()) fs.Init(a[0], fs.ErrorHandling());
    Parse();
}

bool Parsed() { return CommandLine().Parsed(); }

}  // namespace gopp::flag
