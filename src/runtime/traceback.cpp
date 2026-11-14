// Tracebacks: what a crashing program prints, after Go's runtime/
// traceback.go, panic.go and signal_unix.go.
//
// A crash (gopp::panic, an exception that escapes a fiber or a thread,
// runtime::fatal, or a crash signal) prints a message and the stacks of
// the goroutines, then exits with status 2, or with GOTRACEBACK=crash dies
// of SIGABRT. The report takes two steps:
//
// 1. The crashing thread collects the return addresses of its own stack
//    with _Unwind_Backtrace, which walks any code with unwind tables:
//    libstdc++, libc, and the signal trampoline too.
// 2. It switches to a stack of its own, mapped at the crash, and prints.
//    libbacktrace (as GCC bundles it in libstdc++exp.a) turns addresses
//    into functions, files and lines, inlined calls included, and
//    __gcclibcxx_demangle_callback demangles the names. Neither allocates
//    with malloc, so a crash inside malloc still reports; the demangler
//    puts large arrays on the stack, hence the stack of its own.
//
// Other goroutines: a fiber that is switched out is walked by its frame
// pointers from where mcall left it, within its stack's bounds. A fiber
// running on another thread, and a thread the runtime didn't start, are
// sent SIGURG with a request: the handler collects its own stack for the
// reporting thread, and stays in the handler until the process exits.
#include "runtime.hpp"

#include <cxxabi.h>
#include <link.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>
#include <unwind.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string_view>
#include <typeinfo>

// libbacktrace as GCC 16 bundles it in libstdc++exp.a for std::stacktrace,
// with its names prefixed, and libstdc++'s demangler that hands its output
// to a callback instead of allocating it.
struct backtrace_state;
extern "C" {
typedef void (*goppBtErrorCb)(void* data, const char* msg, int errnum);
typedef int (*goppBtFullCb)(void* data, uintptr_t pc, const char* filename, int lineno, const char* function);
typedef void (*goppBtSyminfoCb)(void* data, uintptr_t pc, const char* symname, uintptr_t symval,
                                   uintptr_t symsize);
backtrace_state* __glibcxx_backtrace_create_state(const char* filename, int threaded, goppBtErrorCb, void* data);
int __glibcxx_backtrace_pcinfo(backtrace_state*, uintptr_t pc, goppBtFullCb, goppBtErrorCb, void* data);
void __glibcxx_backtrace_syminfo(backtrace_state*, uintptr_t addr, goppBtSyminfoCb, goppBtErrorCb, void* data);
int __gcclibcxx_demangle_callback(const char* mangled, void (*callback)(const char*, size_t, void*), void* opaque);

_Unwind_Reason_Code gopp_fiber_personality(int version, _Unwind_Action actions, _Unwind_Exception_Class cls,
                                           _Unwind_Exception* ue, _Unwind_Context* ctx);
}

namespace gopp::runtime {

namespace {

// ---------------------------------------------------------------------------
// Output to standard error, without stdio.

struct writer {
    char buf[4096] = {};
    size_t n = 0;

    void flush() noexcept {
        size_t off = 0;
        while (off < n) {
            ssize_t w = ::write(2, buf + off, n - off);
            if (w < 0) {
                if (errno == EINTR) continue;
                break;
            }
            off += static_cast<size_t>(w);
        }
        n = 0;
    }
    void put(const char* s, size_t len) noexcept {
        while (len > 0) {
            if (n == sizeof(buf)) flush();
            size_t k = std::min(len, sizeof(buf) - n);
            std::memcpy(buf + n, s, k);
            n += k;
            s += k;
            len -= k;
        }
    }
    void put(std::string_view s) noexcept { put(s.data(), s.size()); }
    void put(char c) noexcept { put(&c, 1); }
    void dec(uint64_t v) noexcept {
        char t[20];
        int i = sizeof(t);
        do t[--i] = static_cast<char>('0' + v % 10);
        while ((v /= 10) != 0);
        put(t + i, sizeof(t) - i);
    }
    void hex(uint64_t v) noexcept {
        char t[18];
        int i = sizeof(t);
        do t[--i] = "0123456789abcdef"[v & 15];
        while ((v >>= 4) != 0);
        t[--i] = 'x';
        t[--i] = '0';
        put(t + i, sizeof(t) - i);
    }
};

constinit writer out;

// ---------------------------------------------------------------------------
// GOTRACEBACK

struct tracebackSettings {
    int level = 1;  // 0: no stacks; 1: the program's frames; 2: the runtime's too
    bool all = false;
    bool crash = false;
};

tracebackSettings gotraceback() noexcept {
    tracebackSettings t;
    const char* e = std::getenv("GOTRACEBACK");
    std::string_view v = e ? e : "";
    if (v == "none" || v == "0") {
        t.level = 0;
    } else if (v == "all" || v == "1") {
        t.all = true;
    } else if (v == "system" || v == "2") {
        t.level = 2;
        t.all = true;
    } else if (v == "crash" || v == "wer") {
        t.level = 2;
        t.all = true;
        t.crash = true;
    }
    return t;
}

// ---------------------------------------------------------------------------
// Collecting stacks

constexpr int maxPcs = 256;  // collected per stack
constexpr int maxPrint = 100;  // frames printed per stack, as in Go

struct stackPcs {
    uintptr_t pc[maxPcs] = {};
    bool ret[maxPcs] = {};  // a return address: the call is at pc - 1
    int n = 0;
    bool more = false;  // frames beyond maxPcs

    void clear() noexcept {
        n = 0;
        more = false;
    }
    void push(uintptr_t p, bool r) noexcept {
        if (n == maxPcs) {
            more = true;
            return;
        }
        pc[n] = p;
        ret[n] = r;
        n++;
    }
};

enum class skipTo {
    none,
    pc,   // to the frame interrupted at this pc, in a signal handler
    cfa,  // to the frames whose canonical frame address is above this one
};

struct unwindState {
    stackPcs* s;
    skipTo kind;
    uintptr_t key;
    bool started;
};

_Unwind_Reason_Code unwindFrame(_Unwind_Context* ctx, void* arg) {
    auto* u = static_cast<unwindState*>(arg);
    int before = 0;
    uintptr_t ip = _Unwind_GetIPInfo(ctx, &before);
    if (ip == 0) return _URC_END_OF_STACK;
    if (!u->started) {
        switch (u->kind) {
        case skipTo::none: u->started = true; break;
        case skipTo::pc: u->started = ip == u->key; break;
        case skipTo::cfa: u->started = _Unwind_GetCFA(ctx) > u->key; break;
        }
        if (!u->started) return _URC_NO_REASON;
    }
    u->s->push(ip, !before);
    return u->s->more ? _URC_END_OF_STACK : _URC_NO_REASON;
}

// Collects the calling thread's stack.
void collectHere(stackPcs& s, skipTo kind, uintptr_t key) noexcept {
    s.clear();
    unwindState u{&s, kind, key, false};
    _Unwind_Backtrace(unwindFrame, &u);
}

uintptr_t loadWord(uintptr_t addr) noexcept {
    return __atomic_load_n(reinterpret_cast<const uintptr_t*>(addr), __ATOMIC_RELAXED);
}

// Walks the frame pointers from fp, within the stack [lo, hi). On x86-64
// and AArch64 alike a frame starts with the caller's frame pointer and the
// return address.
void collectFp(stackPcs& s, uintptr_t fp, uintptr_t lo, uintptr_t hi) noexcept {
    constexpr uintptr_t w = sizeof(uintptr_t);
    if (lo == 0 || hi < lo + 2 * w) return;
    while (fp >= lo && fp <= hi - 2 * w && fp % w == 0) {
        uintptr_t next = loadWord(fp);
        uintptr_t ret = loadWord(fp + w);
        if (ret == 0) break;
        s.push(ret, true);
        if (s.more || next <= fp) break;
        fp = next;
    }
}

uintptr_t ucPc(void* uc) noexcept {
    auto* u = static_cast<ucontext_t*>(uc);
#if defined(__x86_64__)
    return static_cast<uintptr_t>(u->uc_mcontext.gregs[REG_RIP]);
#elif defined(__aarch64__)
    return u->uc_mcontext.pc;
#else
#error "tracebacks: unsupported architecture"
#endif
}

uintptr_t ucFp(void* uc) noexcept {
    auto* u = static_cast<ucontext_t*>(uc);
#if defined(__x86_64__)
    return static_cast<uintptr_t>(u->uc_mcontext.gregs[REG_RBP]);
#elif defined(__aarch64__)
    return u->uc_mcontext.regs[29];
#endif
}

uintptr_t ucSp(void* uc) noexcept {
    auto* u = static_cast<ucontext_t*>(uc);
#if defined(__x86_64__)
    return static_cast<uintptr_t>(u->uc_mcontext.gregs[REG_RSP]);
#elif defined(__aarch64__)
    return u->uc_mcontext.sp;
#endif
}

// The calling fiber or pseudo-G, without creating one.
G* currentG() noexcept {
    M* m = getm();
    return m ? m->curg : getgIfAny();
}

// ---------------------------------------------------------------------------
// Symbols

void btError(void*, const char*, int) {}  // missing debug information: names come from the symbol table

constinit backtrace_state* btState = nullptr;

backtrace_state* bt() noexcept {
    if (!btState) btState = __glibcxx_backtrace_create_state(nullptr, 1, btError, nullptr);
    return btState;
}

constexpr int maxInline = 16;
constexpr size_t nameMax = 4096;

struct srcFrame {
    char func[nameMax];  // as in the debug information: mangled, or a plain name
    char file[1024];
    int line;
};

// What one pc maps to: the calls inlined there, innermost first, the
// function they are in last, and the symbol of that function.
struct pcInfo {
    srcFrame f[maxInline];
    int n;
    char sym[nameMax];
    uintptr_t symval;
    // The start of the function: the symbol's, or else from the unwind
    // tables, which strip leaves in place. 0 if neither knows.
    uintptr_t start;
};

constinit pcInfo info;

void copyName(char* dst, size_t cap, const char* src) noexcept {
    size_t k = src ? strnlen(src, cap - 1) : 0;
    if (k) std::memcpy(dst, src, k);
    dst[k] = '\0';
}

int pcinfoCb(void*, uintptr_t, const char* file, int line, const char* func) {
    if (info.n == maxInline) return 1;
    srcFrame& f = info.f[info.n++];
    copyName(f.func, sizeof(f.func), func);
    copyName(f.file, sizeof(f.file), file);
    f.line = line;
    return 0;
}

void syminfoCb(void*, uintptr_t, const char* name, uintptr_t val, uintptr_t) {
    copyName(info.sym, sizeof(info.sym), name);
    info.symval = val;
}

void lookup(uintptr_t pc) noexcept {
    info.n = 0;
    info.sym[0] = '\0';
    info.symval = 0;
    info.start = 0;
    if (bt()) {
        __glibcxx_backtrace_pcinfo(bt(), pc, pcinfoCb, btError, nullptr);
        __glibcxx_backtrace_syminfo(bt(), pc, syminfoCb, btError, nullptr);
    }
    info.start = info.symval ? info.symval
                             : reinterpret_cast<uintptr_t>(_Unwind_FindEnclosingFunction(reinterpret_cast<void*>(pc)));
}

// The object file a pc is in, and the address in it that addr2line takes:
// the pc less the load bias, which is 0 for a program that isn't PIE.
struct moduleInfo {
    uintptr_t pc;
    uintptr_t bias;
    const char* name;
    bool found;
};

int findModuleCb(dl_phdr_info* i, size_t, void* data) {
    auto* m = static_cast<moduleInfo*>(data);
    for (int k = 0; k < i->dlpi_phnum; k++) {
        const ElfW(Phdr)& ph = i->dlpi_phdr[k];
        if (ph.p_type != PT_LOAD) continue;
        uintptr_t lo = i->dlpi_addr + ph.p_vaddr;
        if (m->pc >= lo && m->pc - lo < ph.p_memsz) {
            m->bias = i->dlpi_addr;
            m->name = i->dlpi_name;
            m->found = true;
            return 1;
        }
    }
    return 0;
}

char exePath[1024];

// Writes " (/path/prog+0x4012ab)", as the sanitizers do, for a pc that has
// no source: what addr2line -e /path/prog 0x4012ab, run on a copy of the
// program that isn't stripped, needs. For a return address pc is the
// address before it, in the call, which maps to the call's line.
void putModule(uintptr_t pc) noexcept {
    moduleInfo m{pc, 0, nullptr, false};
    dl_iterate_phdr(findModuleCb, &m);
    if (!m.found) return;
    const char* name = m.name;
    if (!name || !name[0]) {  // the program itself
        if (!exePath[0]) {
            ssize_t k = ::readlink("/proc/self/exe", exePath, sizeof(exePath) - 1);
            exePath[k > 0 ? k : 0] = '\0';
        }
        name = exePath[0] ? exePath : "?";
    }
    out.put(" (");
    out.put(name);
    out.put('+');
    out.hex(pc - m.bias);
    out.put(')');
}

// The directory of the runtime's sources, from gentry's file, to hide
// their frames: inlined calls carry their plain names, not the namespace.
// Paths may be relative to where go++ was built, so a file there counts
// as the runtime's only within a function of go++.
char runtimeDir[1024];

void findRuntimeDir() noexcept {
    if (runtimeDir[0]) return;
    lookup(gentryPc());
    if (info.n == 0) return;
    const char* file = info.f[info.n - 1].file;
    const char* slash = std::strrchr(file, '/');
    if (!slash) return;
    copyName(runtimeDir, std::min<size_t>(sizeof(runtimeDir), slash - file + 2), file);
}

// Whether the mangled name m is of an entity in the namespace or class ns,
// given as mangled components such as "4gopp7runtime".
bool inNamespace(std::string_view m, std::string_view ns) noexcept {
    if (!m.starts_with("_Z")) return false;
    m.remove_prefix(2);
    while (m.starts_with('Z')) m.remove_prefix(1);  // a local entity: its enclosing function
    if (m.starts_with('N')) {
        m.remove_prefix(1);
        while (!m.empty() && std::string_view("rVKRO").find(m[0]) != std::string_view::npos) m.remove_prefix(1);
    }
    return m.starts_with(ns);
}

// Where the program starts and threads start, below main and the thread's
// function.
constexpr std::string_view startFrames[] = {
    "__libc_start_call_main", "__libc_start_main", "__libc_start_main_impl", "__libc_start_main_alias_2",
    "_start", "start_thread", "clone", "clone3", "__clone", "__clone3", "__clone_internal",
    "execute_native_thread_routine",
};

// The files of libstdc++'s call plumbing, between a std::thread,
// std::move_only_function or gopp::go and the function it calls.
constexpr std::string_view plumbingFiles[] = {
    "/bits/invoke.h", "/bits/funcwrap.h", "/bits/mofunc_impl.h", "/bits/cpyfunc_impl.h",
    "/bits/std_function.h", "/bits/std_thread.h", "/tuple", "/gopp/runtime.hpp",
};

// The functions of libstdc++'s call plumbing by their mangled names, for
// code without debug information.
constexpr std::string_view plumbingSyms[] = {
    "_ZSt8__invoke", "_ZSt13__invoke_impl", "_ZSt10__invoke_r", "_ZSt12__apply_impl", "_ZSt5apply",
    "_ZNSt10__polyfunc", "_ZNSt18move_only_function", "_ZNSt6thread11_State_impl", "_ZNSt6thread8_Invoker",
    "_ZNSt17_Function_handler", "_ZNKSt8function",
};

// The throw machinery, above the function that threw: by name, or by
// where the function starts when the symbols are stripped.
bool isThrowFrame(std::string_view sym, uintptr_t start) noexcept {
    if (start) {
        for (uintptr_t f : {reinterpret_cast<uintptr_t>(&abi::__cxa_throw), reinterpret_cast<uintptr_t>(&abi::__cxa_rethrow),
                            reinterpret_cast<uintptr_t>(&_Unwind_RaiseException),
                            reinterpret_cast<uintptr_t>(&_Unwind_Resume_or_Rethrow),
                            reinterpret_cast<uintptr_t>(&gopp_fiber_personality)}) {
            if (start == f) return true;
        }
    }
    return sym.starts_with("_Unwind_") || sym == "__cxa_throw" || sym == "__cxa_rethrow" ||
           sym == "__cxa_call_terminate" || sym == "__gxx_personality_v0" || sym == "gopp_fiber_personality" ||
           sym.starts_with("_ZSt17rethrow_exception") || sym == "_ZSt9terminatev" ||
           sym.starts_with("_ZN10__cxxabiv111__terminate");
}

// Go hides the runtime's frames unless GOTRACEBACK=system. func and file
// are one call's, and sym is the symbol of the function the call is in.
bool hidden(std::string_view func, std::string_view file, std::string_view sym, int level) noexcept {
    if (level >= 2) return false;
    if (inNamespace(func, "4gopp5panic")) return false;  // Go shows panic's frame
    if (inNamespace(func, "4gopp7runtime") || inNamespace(func, "5boost7context") || inNamespace(func, "4gopp2go"))
        return true;
    for (std::string_view s : startFrames)
        if (func == s) return true;
    for (std::string_view s : plumbingSyms)
        if (func.starts_with(s)) return true;
    if (!file.empty()) {
        if (runtimeDir[0] && file.starts_with(runtimeDir) && inNamespace(sym, "4gopp")) return true;
        if (file.find("/boost/context/") != std::string_view::npos) return true;
        for (std::string_view s : plumbingFiles)
            if (file.ends_with(s)) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Printing frames

struct demangled {
    char* p;
    size_t n;
    size_t cap;
    bool truncated;
};

void demangleCb(const char* s, size_t len, void* opaque) {
    auto* d = static_cast<demangled*>(opaque);
    if (d->n + len > d->cap) {
        len = d->cap - d->n;
        d->truncated = true;
    }
    std::memcpy(d->p + d->n, s, len);
    d->n += len;
}

char demangleBuf[16384];

// Writes a function's name, demangled, with its parameters elided Go's
// way, "ns::f(...)" or "ns::f()"; without them if !params.
void putFunc(const char* name, bool params) noexcept {
    std::string_view s = name;
    // Names this long take the demangler more stack than the report has.
    if (s.starts_with("_Z") && s.size() < 8000) {
        demangled d{demangleBuf, 0, sizeof(demangleBuf), false};
        if (__gcclibcxx_demangle_callback(name, demangleCb, &d) == 0 && !d.truncated) s = {d.p, d.n};
    }
    // GCC's clones, "f(int) [clone .isra.0]", are the function.
    for (size_t c; (c = s.rfind(" [clone ")) != std::string_view::npos && s.ends_with(']');) s = s.substr(0, c);
    // Trailing qualifiers, then the parameters.
    size_t end = s.size();
    for (bool again = true; again;) {
        again = false;
        for (std::string_view q : {" const", " volatile", " &&", " &"}) {
            if (s.substr(0, end).ends_with(q)) {
                end -= q.size();
                again = true;
            }
        }
    }
    size_t open = std::string_view::npos;
    if (end > 0 && s[end - 1] == ')') {
        int depth = 0;
        for (size_t i = end; i-- > 0;) {
            if (s[i] == ')') {
                depth++;
            } else if (s[i] == '(' && --depth == 0) {
                open = i;
                break;
            }
        }
        // "operator()" alone is a name without parameters.
        if (open != std::string_view::npos && open + 2 == end && s.substr(0, open).ends_with("operator"))
            open = std::string_view::npos;
    }
    if (open == std::string_view::npos) {
        out.put(s.substr(0, end));
        if (params) out.put("(...)");
    } else {
        out.put(s.substr(0, open));
        if (params) out.put(open + 2 == end ? "()" : "(...)");
    }
    if (params) out.put(s.substr(end));
}

// The function and source of call k of the pc looked up last.
struct call {
    const char* func;
    const char* file;
    int line;
    bool outer;  // the function the calls are inlined in: the pc's own frame
};

int ncalls() noexcept { return info.n == 0 ? 1 : info.n; }

call callAt(int k) noexcept {
    call c{"", "", 0, k == ncalls() - 1};
    if (info.n > 0) {
        c.func = info.f[k].func;
        c.file = info.f[k].file;
        c.line = info.f[k].line;
    }
    // The debug information may name a function without its namespace or
    // class: the symbol's name is complete.
    if (c.outer && std::string_view(info.sym).starts_with("_Z")) c.func = info.sym;
    if (!c.func[0] && c.outer) c.func = info.sym;
    return c;
}

// pc is the frame's, a return address or where it was interrupted;
// callPc the address looked up, in the call.
void putSource(const call& c, uintptr_t pc, uintptr_t callPc) noexcept {
    out.put('\t');
    if (c.file[0]) {
        out.put(c.file);
        out.put(':');
        out.dec(static_cast<uint64_t>(c.line));
    } else {
        out.put('?');
    }
    if (c.outer) {
        if (info.symval) {
            out.put(" +");
            out.hex(pc - info.symval);
        }
        // Without a source, at least the address.
        if (!c.file[0] || !info.symval) {
            out.put(" pc=");
            out.hex(pc);
            putModule(callPc);
        }
    }
    out.put('\n');
}

struct frameOpts {
    int level;
    bool fiber;       // stop at gentry
    bool skipThrow;  // skip the throw machinery at the top
};

// Prints the frames of s, Go's way:
//
//     ns::f(...)
//             /path/file.cpp:12 +0x1f
void printFrames(const stackPcs& s, const frameOpts& o) noexcept {
    auto lookupPc = [&](int i) { return s.ret[i] ? s.pc[i] - 1 : s.pc[i]; };
    int start = 0;
    if (o.skipThrow) {
        for (int i = 0; i < s.n && i < 48; i++) {
            lookup(lookupPc(i));
            if (isThrowFrame(info.sym, info.start)) start = i + 1;
        }
    }
    int printed = 0;
    for (int i = start; i < s.n; i++) {
        lookup(lookupPc(i));
        if (o.fiber && info.start == gentryPc()) return;
        // gentry's call to the fiber's function, hidden by its namespace
        // when there are symbols.
        if (o.fiber && o.level < 2 && info.start == reinterpret_cast<uintptr_t>(&runFiberFn)) continue;
        for (int k = 0; k < ncalls(); k++) {
            call c = callAt(k);
            if (hidden(c.func, c.file, info.sym, o.level)) continue;
            if (printed == maxPrint) {
                out.put("...additional frames elided...\n");
                return;
            }
            putFunc(c.func[0] ? c.func : "?", true);
            out.put('\n');
            putSource(c, s.pc[i], lookupPc(i));
            printed++;
        }
    }
    if (s.more) out.put("...additional frames elided...\n");
}

// The function with the go statement, below gopp::go, at any level.
void printCreatedBy(G* gp) noexcept {
    if (!gp->gopc) return;
    lookup(gp->gopc - 1);
    for (int k = 0; k < ncalls(); k++) {
        call c = callAt(k);
        if (hidden(c.func, c.file, info.sym, 1)) continue;
        out.put("created by ");
        putFunc(c.func[0] ? c.func : "?", false);
        if (gp->parentGoid) {
            out.put(" in goroutine ");
            out.dec(gp->parentGoid);
        }
        out.put('\n');
        putSource(c, gp->gopc, gp->gopc - 1);
        return;
    }
}

// ---------------------------------------------------------------------------
// Goroutine headers

std::string_view waitreasonString(waitreason w) noexcept {
    switch (w) {
    case waitreason::none: return "waiting";
    case waitreason::chanReceive: return "chan receive";
    case waitreason::chanSend: return "chan send";
    case waitreason::chanReceiveNil: return "chan receive (nil chan)";
    case waitreason::chanSendNil: return "chan send (nil chan)";
    case waitreason::select: return "select";
    case waitreason::selectNoCases: return "select (no cases)";
    case waitreason::sleep: return "sleep";
    case waitreason::ioWait: return "IO wait";
    case waitreason::semacquire: return "semacquire";
    case waitreason::syncMutexLock: return "sync.Mutex.Lock";
    case waitreason::syncRwmutexRlock: return "sync.RWMutex.RLock";
    case waitreason::syncRwmutexLock: return "sync.RWMutex.Lock";
    case waitreason::syncWaitgroupWait: return "sync.WaitGroup.Wait";
    case waitreason::syncCondWait: return "sync.Cond.Wait";
    }
    return "waiting";
}

std::string_view statusString(G* gp) noexcept {
    switch (gp->status.load(std::memory_order_relaxed)) {
    case gstatus::idle: return "idle";
    case gstatus::runnable: return "runnable";
    case gstatus::running: return "running";
    case gstatus::waiting: return waitreasonString(gp->wait);
    case gstatus::dead: return "dead";
    }
    return "unknown";
}

void putThreadName(pid_t tid) noexcept {
    out.put("thread ");
    out.dec(static_cast<uint64_t>(tid));
    if (isMainThread(tid)) out.put(" (main)");
}

void putHeader(G* gp, std::string_view status) noexcept {
    if (gp->isThread) {
        putThreadName(gp->tid);
    } else {
        out.put("goroutine ");
        out.dec(gp->goid);
    }
    out.put(" [");
    out.put(status);
    out.put("]:\n");
}

// ---------------------------------------------------------------------------
// The crashing thread

struct selfCtx {
    M* m;
    G* gp;  // the crashing fiber or pseudo-G; nullptr on an M's scheduler
    pid_t tid;
    bool skipThrow;
    stackPcs* pcs;
};

constinit stackPcs selfPcs;

selfCtx collectSelf(skipTo kind, uintptr_t key, void* uc) noexcept {
    selfCtx c{getm(), currentG(), gettid(), false, &selfPcs};
    collectHere(selfPcs, kind, key);
    if (selfPcs.n == 0 && uc && c.gp && !c.gp->isThread) {
        // The unwinder didn't get through the signal frame: walk the
        // fiber's frame pointers instead.
        selfPcs.push(ucPc(uc), false);
        collectFp(selfPcs, ucFp(uc), c.gp->stackLo, c.gp->stackHi);
    }
    return c;
}

void printSelf(const selfCtx& c, int level) noexcept {
    bool fiber = c.gp && !c.gp->isThread;
    if (c.gp) {
        putHeader(c.gp, "running");
    } else if (c.m) {
        out.put("runtime stack:\n");
        level = 2;  // it's all runtime
    } else {
        putThreadName(c.tid);
        out.put(" [running]:\n");
    }
    printFrames(*c.pcs, {level, fiber, c.skipThrow});
    if (fiber) printCreatedBy(c.gp);
}

// ---------------------------------------------------------------------------
// Other threads' stacks, on request

struct stackRequest {
    std::atomic<int> state{0};  // 0 none, 1 asked, 2 answering, 3 answered
    std::atomic<pid_t> tid{0};
    G* gp = nullptr;
    bool moved = false;  // the thread runs another goroutine now
    stackPcs pcs;
};

constinit stackRequest req;
// A thread took the request and didn't finish: it may still write.
constinit bool reqBroken = false;

void sleepMs(long ms) noexcept {
    struct timespec ts{0, ms * 1'000'000};
    ::nanosleep(&ts, nullptr);
}

// Asks thread tid, which runs gp, for its stack. Returns whether it answered.
bool requestStack(pid_t tid, G* gp) noexcept {
    if (reqBroken) return false;
    out.flush();
    req.gp = gp;
    req.moved = false;
    req.pcs.clear();
    req.tid.store(tid, std::memory_order_relaxed);
    req.state.store(1, std::memory_order_seq_cst);
    if (::syscall(SYS_tgkill, ::getpid(), tid, SIGURG) != 0) {
        req.state.store(0);
        return false;
    }
    for (int i = 0; i < 500 && req.state.load() == 1; i++) sleepMs(1);
    int asked = 1;
    if (req.state.compare_exchange_strong(asked, 0)) return false;  // no answer
    for (int i = 0; i < 5000 && req.state.load() != 3; i++) sleepMs(1);
    if (req.state.load() != 3) {
        reqBroken = true;
        return false;
    }
    req.state.store(0);
    return true;
}

// Answers a request for the calling thread's stack, if there is one.
bool answerRequest(skipTo kind, uintptr_t key, void* uc) noexcept {
    if (req.state.load(std::memory_order_acquire) != 1) return false;
    if (req.tid.load(std::memory_order_relaxed) != gettid()) return false;
    int asked = 1;
    if (!req.state.compare_exchange_strong(asked, 2)) return false;
    G* gp = currentG();
    if (gp != req.gp) {
        req.moved = true;
    } else {
        collectHere(req.pcs, kind, key);
        if (req.pcs.n == 0 && uc && gp && !gp->isThread) {
            req.pcs.push(ucPc(uc), false);
            collectFp(req.pcs, ucFp(uc), gp->stackLo, gp->stackHi);
        }
    }
    req.state.store(3, std::memory_order_release);
    return true;
}

constexpr std::string_view unavailable = "\tgoroutine running on other thread; stack unavailable\n";

constinit stackPcs otherPcs;

void printOther(const selfCtx& c, G* gp, int level) noexcept {
    gstatus st = gp->status.load(std::memory_order_relaxed);
    if (st == gstatus::dead || st == gstatus::idle) return;
    if (gp->isThread && gp->tid == 0) return;
    out.put('\n');
    putHeader(gp, statusString(gp));
    bool fiber = !gp->isThread;
    if (gp->isThread || st == gstatus::running) {
        pid_t tid = gp->tid;
        if (fiber) {
            M* m = __atomic_load_n(&gp->m, __ATOMIC_RELAXED);
            tid = m && m != c.m ? m->tid : 0;
        }
        if (tid == 0 || tid == c.tid || !requestStack(tid, gp) || req.moved) {
            out.put(unavailable);
        } else {
            printFrames(req.pcs, {level, fiber, false});
        }
    } else if (gp->schedFp) {
        otherPcs.clear();
        collectFp(otherPcs, gp->schedFp, gp->stackLo, gp->stackHi);
        printFrames(otherPcs, {level, true, false});
    }
    if (fiber) printCreatedBy(gp);
}

// main first, then goroutines by id, then other threads.
bool printOrder(G* a, G* b) noexcept {
    auto rank = [](G* g) { return g->isThread ? (isMainThread(g->tid) ? 0 : 2) : 1; };
    int ra = rank(a), rb = rank(b);
    if (ra != rb) return ra < rb;
    return a->isThread ? a->tid < b->tid : a->goid < b->goid;
}

void printOthers(const selfCtx& c, int level) noexcept {
    size_t n = 0;
    for (G* gp = allgs(); gp; gp = gp->alllink) n++;
    size_t bytes = (n * sizeof(G*) + 4095) & ~size_t{4095};
    void* mem = n ? ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) : MAP_FAILED;
    if (mem == MAP_FAILED) {
        for (G* gp = allgs(); gp; gp = gp->alllink)
            if (gp != c.gp) printOther(c, gp, level);
        return;
    }
    G** v = static_cast<G**>(mem);
    size_t k = 0;
    for (G* gp = allgs(); gp && k < n; gp = gp->alllink) v[k++] = gp;
    std::sort(v, v + k, printOrder);
    for (size_t i = 0; i < k; i++)
        if (v[i] != c.gp) printOther(c, v[i], level);
    ::munmap(mem, bytes);
}

void printGoroutines(const selfCtx& c, const tracebackSettings& t, bool all, int selfLevel) noexcept {
    if (t.level == 0) return;
    findRuntimeDir();
    out.put('\n');
    printSelf(c, selfLevel);
    if (all || t.all) printOthers(c, t.level);
}

// ---------------------------------------------------------------------------
// Crashing

constinit std::atomic<pid_t> crashing{0};

[[noreturn]] void die(const tracebackSettings& t) noexcept {
    out.flush();
    if (t.crash) {
        struct sigaction dfl{};
        dfl.sa_handler = SIG_DFL;
        sigemptyset(&dfl.sa_mask);
        ::sigaction(SIGABRT, &dfl, nullptr);
        sigset_t set;
        sigemptyset(&set);
        sigaddset(&set, SIGABRT);
        ::pthread_sigmask(SIG_UNBLOCK, &set, nullptr);
        ::syscall(SYS_tgkill, ::getpid(), gettid(), SIGABRT);
    }
    ::_exit(2);
}

// Takes the right to report. A thread that crashes while another reports
// waits for good, answering its requests for the waiting thread's stack.
void crashEnter(skipTo kind, uintptr_t key, void* uc) noexcept {
    pid_t me = gettid();
    pid_t none = 0;
    if (crashing.compare_exchange_strong(none, me)) return;
    if (none == me) {
        static constexpr std::string_view msg = "\nfatal error: unexpected signal during traceback\n";
        [[maybe_unused]] ssize_t r = ::write(2, msg.data(), msg.size());
        die(gotraceback());
    }
    for (;;) {
        answerRequest(kind, key, uc);
        sleepMs(1);
    }
}

// A stack for the report, so that it doesn't depend on what is left of
// the crashing one.
struct crashStack {
    void* base;
    std::size_t size;

    boost::context::stack_context allocate() noexcept {
        boost::context::stack_context s;
        s.size = size;
        s.sp = static_cast<char*>(base) + size;
        return s;
    }
    void deallocate(boost::context::stack_context&) noexcept {}
};

template <typename F>
[[noreturn]] void onCrashStack(F&& report) noexcept {
    constexpr std::size_t size = 1 << 20;
    void* p = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
    if (p != MAP_FAILED) {
        boost::context::fiber f(std::allocator_arg, crashStack{p, size},
                                [&report](boost::context::fiber&& c) {
                                    report();
                                    return std::move(c);
                                });
        std::move(f).resume();
    } else {
        report();
    }
    ::_exit(2);
}

std::string_view signalName(int sig) noexcept {
    switch (sig) {
    case SIGSEGV: return "SIGSEGV: segmentation violation";
    case SIGBUS: return "SIGBUS: bus error";
    case SIGFPE: return "SIGFPE: floating-point exception";
    case SIGILL: return "SIGILL: illegal instruction";
    case SIGTRAP: return "SIGTRAP: trace trap";
    case SIGABRT: return "SIGABRT: abort";
    case SIGSYS: return "SIGSYS: bad system call";
    case SIGQUIT: return "SIGQUIT: quit";
    }
    return "signal";
}

// "[signal SIGSEGV: segmentation violation code=0x1 addr=0x0 pc=0x4011d6]"
void putSignalLine(int sig, const siginfo_t* info, uintptr_t pc) noexcept {
    out.put("[signal ");
    out.put(signalName(sig));
    out.put(" code=");
    out.hex(static_cast<uint64_t>(static_cast<int64_t>(info->si_code)));
    out.put(" addr=");
    out.hex(reinterpret_cast<uintptr_t>(info->si_addr));
    out.put(" pc=");
    out.hex(pc);
    out.put("]\n");
}

void putReg(std::string_view name, uint64_t v) noexcept {
    out.put(name);
    for (size_t i = name.size(); i < 7; i++) out.put(' ');
    out.hex(v);
    out.put('\n');
}

void dumpregs(void* uc) noexcept {
    auto* u = static_cast<ucontext_t*>(uc);
#if defined(__x86_64__)
    const auto& g = u->uc_mcontext.gregs;
    static constexpr struct {
        std::string_view name;
        int reg;
    } regs[] = {
        {"rax", REG_RAX}, {"rbx", REG_RBX}, {"rcx", REG_RCX}, {"rdx", REG_RDX}, {"rdi", REG_RDI},
        {"rsi", REG_RSI}, {"rbp", REG_RBP}, {"rsp", REG_RSP}, {"r8", REG_R8},   {"r9", REG_R9},
        {"r10", REG_R10}, {"r11", REG_R11}, {"r12", REG_R12}, {"r13", REG_R13}, {"r14", REG_R14},
        {"r15", REG_R15}, {"rip", REG_RIP}, {"rflags", REG_EFL},
    };
    for (const auto& r : regs) putReg(r.name, static_cast<uint64_t>(g[r.reg]));
    uint64_t csgsfs = static_cast<uint64_t>(g[REG_CSGSFS]);
    putReg("cs", csgsfs & 0xffff);
    putReg("fs", (csgsfs >> 32) & 0xffff);
    putReg("gs", (csgsfs >> 16) & 0xffff);
#elif defined(__aarch64__)
    static constexpr std::string_view names[] = {
        "r0",  "r1",  "r2",  "r3",  "r4",  "r5",  "r6",  "r7",  "r8",  "r9",  "r10", "r11", "r12", "r13", "r14",
        "r15", "r16", "r17", "r18", "r19", "r20", "r21", "r22", "r23", "r24", "r25", "r26", "r27", "r28", "r29",
    };
    for (int i = 0; i < 30; i++) putReg(names[i], u->uc_mcontext.regs[i]);
    putReg("lr", u->uc_mcontext.regs[30]);
    putReg("sp", u->uc_mcontext.sp);
    putReg("pc", u->uc_mcontext.pc);
    putReg("fault", u->uc_mcontext.faultAddress);
#endif
}

// The message of the exception being handled: its type and what().
struct exceptionMessage {
    const std::type_info* type = nullptr;
    const char* what = nullptr;
};

exceptionMessage currentExceptionMessage() noexcept {
    exceptionMessage m;
    m.type = abi::__cxa_current_exception_type();
    try {
        throw;
    } catch (const std::exception& e) {
        m.what = e.what();
    } catch (...) {
    }
    return m;
}

[[noreturn]] void reportException() noexcept {
    crashEnter(skipTo::none, 0, nullptr);
    tracebackSettings t = gotraceback();
    exceptionMessage e = currentExceptionMessage();
    selfCtx c = collectSelf(skipTo::none, 0, nullptr);
    c.skipThrow = true;
    onCrashStack([&] {
        out.put("panic: ");
        if (e.type) {
            // A type's mangled name, unlike a function's, has no "_Z".
            demangled d{demangleBuf, 0, sizeof(demangleBuf), false};
            if (__gcclibcxx_demangle_callback(e.type->name(), demangleCb, &d) == 0 && !d.truncated)
                out.put(std::string_view(d.p, d.n));
            else
                out.put(e.type->name());
        } else {
            out.put("unknown exception");
        }
        if (e.what) {
            out.put(": ");
            out.put(e.what);
        }
        out.put('\n');
        printGoroutines(c, t, false, t.level);
        die(t);
    });
}

}  // namespace

// ---------------------------------------------------------------------------
// Entry points

[[noreturn]] void dopanic(std::string_view msg) noexcept {
    uintptr_t fp = reinterpret_cast<uintptr_t>(__builtin_frame_address(0));
    crashEnter(skipTo::cfa, fp, nullptr);
    tracebackSettings t = gotraceback();
    selfCtx c = collectSelf(skipTo::cfa, fp, nullptr);
    onCrashStack([&] {
        out.put("panic: ");
        out.put(msg);
        out.put('\n');
        printGoroutines(c, t, false, t.level);
        die(t);
    });
}

[[noreturn]] void dofatal(const char* msg) noexcept {
    uintptr_t fp = reinterpret_cast<uintptr_t>(__builtin_frame_address(0));
    crashEnter(skipTo::cfa, fp, nullptr);
    tracebackSettings t = gotraceback();
    selfCtx c = collectSelf(skipTo::cfa, fp, nullptr);
    onCrashStack([&] {
        out.put("fatal error: ");
        out.put(msg);
        out.put('\n');
        // The runtime's frames show for the thrower, as in Go's throw.
        printGoroutines(c, t, true, std::max(t.level, 2));
        die(t);
    });
}

[[noreturn]] void crashSignal(int sig, siginfo_t* info, void* uc) noexcept {
    uintptr_t pc = ucPc(uc);
    crashEnter(skipTo::pc, pc, uc);
    tracebackSettings t = gotraceback();
    selfCtx c = collectSelf(skipTo::pc, pc, uc);
    onCrashStack([&] {
        uintptr_t addr = reinterpret_cast<uintptr_t>(info->si_addr);
        bool fault = sig == SIGSEGV || sig == SIGBUS;
        bool all = true;
        bool regs = false;
        G* gp = c.gp;
        if (fault && gp && !gp->isThread && addr < gp->stackLo && addr + 65536 >= gp->stackLo) {
            out.put("runtime: goroutine stack exceeds ");
            out.dec(gp->stackHi - gp->stackLo);
            out.put("-byte limit\nruntime: sp=");
            out.hex(ucSp(uc));
            out.put(" stack=[");
            out.hex(gp->stackLo);
            out.put(", ");
            out.hex(gp->stackHi);
            out.put("]\nfatal error: stack overflow\n");
        } else if (fault && addr < 0x1000) {
            out.put("panic: runtime error: invalid memory address or nil pointer dereference\n");
            putSignalLine(sig, info, pc);
            all = false;
        } else if (fault) {
            out.put("unexpected fault address ");
            out.hex(addr);
            out.put("\nfatal error: fault\n");
            putSignalLine(sig, info, pc);
        } else if (sig == SIGFPE && info->si_code > 0) {
            out.put(info->si_code == FPE_INTDIV   ? "panic: runtime error: integer divide by zero\n"
                    : info->si_code == FPE_INTOVF ? "panic: runtime error: integer overflow\n"
                                                  : "panic: runtime error: floating point error\n");
            putSignalLine(sig, info, pc);
            all = false;
        } else {
            out.put(signalName(sig));
            out.put("\nPC=");
            out.hex(pc);
            if (c.m) {
                out.put(" m=");
                out.dec(static_cast<uint64_t>(c.m->id));
            }
            out.put(" sigcode=");
            out.dec(static_cast<uint64_t>(static_cast<int64_t>(info->si_code)));
            out.put('\n');
            regs = true;
        }
        printGoroutines(c, t, all, t.level);
        if (regs && t.level > 0) {
            out.put('\n');
            dumpregs(uc);
        }
        die(t);
    });
}

void tracebackSignal(void* uc) noexcept {
    if (req.state.load(std::memory_order_relaxed) != 1) return;
    if (!answerRequest(skipTo::pc, ucPc(uc), uc)) return;
    // The process exits once the report is done; this thread stays out of
    // its way. The handler blocks all signals, so pause doesn't return.
    for (;;) ::pause();
}

// An exception that reaches this frame in the unwinder's search phase has
// no handler in the fiber: gopp_fiber_personality reports it from there,
// before any destructor runs, with the stack as it was at the throw. A
// noexcept frame would not do: GCC unwinds through cleanups to it before
// calling std::terminate. The call isn't a tail call, so that this frame
// stays on the stack.
[[gnu::noinline]] void runFiberFn(G* gp) {
    asm volatile(".cfi_personality 0x1b, gopp_fiber_personality");
    gp->fn();
    asm volatile("");
}

[[noreturn]] void terminateHandler() noexcept {
    // __cxa_throw calls std::terminate without unwinding when no frame
    // handles the exception, so the stack is as it was at the throw.
    if (std::current_exception()) reportException();
    dofatal("std::terminate called without an active exception");
}

namespace {

constexpr std::size_t altstackSize = 64 * 1024;
constexpr std::size_t altstackGuard = 64 * 1024;  // a guard of the largest page size
thread_local void* tlsAltstack = nullptr;

}  // namespace

void ensureSigaltstack() noexcept {
    stack_t old{};
    if (::sigaltstack(nullptr, &old) == 0 && !(old.ss_flags & SS_DISABLE)) return;
    void* p = ::mmap(nullptr, altstackGuard + altstackSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return;
    ::mprotect(p, altstackGuard, PROT_NONE);
    stack_t ss{};
    ss.ss_sp = static_cast<char*>(p) + altstackGuard;
    ss.ss_size = altstackSize;
    if (::sigaltstack(&ss, nullptr) != 0) {
        ::munmap(p, altstackGuard + altstackSize);
        return;
    }
    tlsAltstack = p;
}

void releaseSigaltstack() noexcept {
    if (!tlsAltstack) return;
    stack_t ss{};
    ss.ss_flags = SS_DISABLE;
    ::sigaltstack(&ss, nullptr);
    ::munmap(tlsAltstack, altstackGuard + altstackSize);
    tlsAltstack = nullptr;
}

}  // namespace gopp::runtime

// Used: only run_fiber_fn's unwind information refers to it.
extern "C" [[gnu::used]] _Unwind_Reason_Code gopp_fiber_personality(int, _Unwind_Action actions, _Unwind_Exception_Class,
                                                      _Unwind_Exception* ue, _Unwind_Context*) {
    // Forced unwinds (thread cancellation) and the cleanup phase pass
    // through.
    if (!(actions & _UA_SEARCH_PHASE) || (actions & _UA_FORCE_UNWIND)) return _URC_CONTINUE_UNWIND;
    __cxxabiv1::__cxa_begin_catch(ue);
    gopp::runtime::reportException();
}
