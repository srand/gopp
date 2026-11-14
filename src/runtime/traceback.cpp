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
typedef void (*gopp_bt_error_cb)(void* data, const char* msg, int errnum);
typedef int (*gopp_bt_full_cb)(void* data, uintptr_t pc, const char* filename, int lineno, const char* function);
typedef void (*gopp_bt_syminfo_cb)(void* data, uintptr_t pc, const char* symname, uintptr_t symval,
                                   uintptr_t symsize);
backtrace_state* __glibcxx_backtrace_create_state(const char* filename, int threaded, gopp_bt_error_cb, void* data);
int __glibcxx_backtrace_pcinfo(backtrace_state*, uintptr_t pc, gopp_bt_full_cb, gopp_bt_error_cb, void* data);
void __glibcxx_backtrace_syminfo(backtrace_state*, uintptr_t addr, gopp_bt_syminfo_cb, gopp_bt_error_cb, void* data);
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

struct traceback_settings {
    int level = 1;  // 0: no stacks; 1: the program's frames; 2: the runtime's too
    bool all = false;
    bool crash = false;
};

traceback_settings gotraceback() noexcept {
    traceback_settings t;
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

constexpr int max_pcs = 256;  // collected per stack
constexpr int max_print = 100;  // frames printed per stack, as in Go

struct stack_pcs {
    uintptr_t pc[max_pcs] = {};
    bool ret[max_pcs] = {};  // a return address: the call is at pc - 1
    int n = 0;
    bool more = false;  // frames beyond max_pcs

    void clear() noexcept {
        n = 0;
        more = false;
    }
    void push(uintptr_t p, bool r) noexcept {
        if (n == max_pcs) {
            more = true;
            return;
        }
        pc[n] = p;
        ret[n] = r;
        n++;
    }
};

enum class skip_to {
    none,
    pc,   // to the frame interrupted at this pc, in a signal handler
    cfa,  // to the frames whose canonical frame address is above this one
};

struct unwind_state {
    stack_pcs* s;
    skip_to kind;
    uintptr_t key;
    bool started;
};

_Unwind_Reason_Code unwind_frame(_Unwind_Context* ctx, void* arg) {
    auto* u = static_cast<unwind_state*>(arg);
    int before = 0;
    uintptr_t ip = _Unwind_GetIPInfo(ctx, &before);
    if (ip == 0) return _URC_END_OF_STACK;
    if (!u->started) {
        switch (u->kind) {
        case skip_to::none: u->started = true; break;
        case skip_to::pc: u->started = ip == u->key; break;
        case skip_to::cfa: u->started = _Unwind_GetCFA(ctx) > u->key; break;
        }
        if (!u->started) return _URC_NO_REASON;
    }
    u->s->push(ip, !before);
    return u->s->more ? _URC_END_OF_STACK : _URC_NO_REASON;
}

// Collects the calling thread's stack.
void collect_here(stack_pcs& s, skip_to kind, uintptr_t key) noexcept {
    s.clear();
    unwind_state u{&s, kind, key, false};
    _Unwind_Backtrace(unwind_frame, &u);
}

uintptr_t load_word(uintptr_t addr) noexcept {
    return __atomic_load_n(reinterpret_cast<const uintptr_t*>(addr), __ATOMIC_RELAXED);
}

// Walks the frame pointers from fp, within the stack [lo, hi). On x86-64
// and AArch64 alike a frame starts with the caller's frame pointer and the
// return address.
void collect_fp(stack_pcs& s, uintptr_t fp, uintptr_t lo, uintptr_t hi) noexcept {
    constexpr uintptr_t w = sizeof(uintptr_t);
    if (lo == 0 || hi < lo + 2 * w) return;
    while (fp >= lo && fp <= hi - 2 * w && fp % w == 0) {
        uintptr_t next = load_word(fp);
        uintptr_t ret = load_word(fp + w);
        if (ret == 0) break;
        s.push(ret, true);
        if (s.more || next <= fp) break;
        fp = next;
    }
}

uintptr_t uc_pc(void* uc) noexcept {
    auto* u = static_cast<ucontext_t*>(uc);
#if defined(__x86_64__)
    return static_cast<uintptr_t>(u->uc_mcontext.gregs[REG_RIP]);
#elif defined(__aarch64__)
    return u->uc_mcontext.pc;
#else
#error "tracebacks: unsupported architecture"
#endif
}

uintptr_t uc_fp(void* uc) noexcept {
    auto* u = static_cast<ucontext_t*>(uc);
#if defined(__x86_64__)
    return static_cast<uintptr_t>(u->uc_mcontext.gregs[REG_RBP]);
#elif defined(__aarch64__)
    return u->uc_mcontext.regs[29];
#endif
}

uintptr_t uc_sp(void* uc) noexcept {
    auto* u = static_cast<ucontext_t*>(uc);
#if defined(__x86_64__)
    return static_cast<uintptr_t>(u->uc_mcontext.gregs[REG_RSP]);
#elif defined(__aarch64__)
    return u->uc_mcontext.sp;
#endif
}

// The calling fiber or pseudo-G, without creating one.
G* current_g() noexcept {
    M* m = getm();
    return m ? m->curg : getg_if_any();
}

// ---------------------------------------------------------------------------
// Symbols

void bt_error(void*, const char*, int) {}  // missing debug information: names come from the symbol table

constinit backtrace_state* bt_state = nullptr;

backtrace_state* bt() noexcept {
    if (!bt_state) bt_state = __glibcxx_backtrace_create_state(nullptr, 1, bt_error, nullptr);
    return bt_state;
}

constexpr int max_inline = 16;
constexpr size_t name_max = 4096;

struct src_frame {
    char func[name_max];  // as in the debug information: mangled, or a plain name
    char file[1024];
    int line;
};

// What one pc maps to: the calls inlined there, innermost first, the
// function they are in last, and the symbol of that function.
struct pc_info {
    src_frame f[max_inline];
    int n;
    char sym[name_max];
    uintptr_t symval;
};

constinit pc_info info;

void copy_name(char* dst, size_t cap, const char* src) noexcept {
    size_t k = src ? strnlen(src, cap - 1) : 0;
    if (k) std::memcpy(dst, src, k);
    dst[k] = '\0';
}

int pcinfo_cb(void*, uintptr_t, const char* file, int line, const char* func) {
    if (info.n == max_inline) return 1;
    src_frame& f = info.f[info.n++];
    copy_name(f.func, sizeof(f.func), func);
    copy_name(f.file, sizeof(f.file), file);
    f.line = line;
    return 0;
}

void syminfo_cb(void*, uintptr_t, const char* name, uintptr_t val, uintptr_t) {
    copy_name(info.sym, sizeof(info.sym), name);
    info.symval = val;
}

void lookup(uintptr_t pc) noexcept {
    info.n = 0;
    info.sym[0] = '\0';
    info.symval = 0;
    if (!bt()) return;
    __glibcxx_backtrace_pcinfo(bt(), pc, pcinfo_cb, bt_error, nullptr);
    __glibcxx_backtrace_syminfo(bt(), pc, syminfo_cb, bt_error, nullptr);
}

// The directory of the runtime's sources, from gentry's file, to hide
// their frames: inlined calls carry their plain names, not the namespace.
// Paths may be relative to where go++ was built, so a file there counts
// as the runtime's only within a function of go++.
char runtime_dir[1024];

void find_runtime_dir() noexcept {
    if (runtime_dir[0]) return;
    lookup(gentry_pc());
    if (info.n == 0) return;
    const char* file = info.f[info.n - 1].file;
    const char* slash = std::strrchr(file, '/');
    if (!slash) return;
    copy_name(runtime_dir, std::min<size_t>(sizeof(runtime_dir), slash - file + 2), file);
}

// Whether the mangled name m is of an entity in the namespace or class ns,
// given as mangled components such as "4gopp7runtime".
bool in_namespace(std::string_view m, std::string_view ns) noexcept {
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
constexpr std::string_view start_frames[] = {
    "__libc_start_call_main", "__libc_start_main", "__libc_start_main_impl", "__libc_start_main_alias_2",
    "_start", "start_thread", "clone", "clone3", "__clone", "__clone3", "__clone_internal",
    "execute_native_thread_routine",
};

// The files of libstdc++'s call plumbing, between a std::thread,
// std::move_only_function or gopp::go and the function it calls.
constexpr std::string_view plumbing_files[] = {
    "/bits/invoke.h", "/bits/funcwrap.h", "/bits/mofunc_impl.h", "/bits/cpyfunc_impl.h",
    "/bits/std_function.h", "/bits/std_thread.h", "/tuple", "/gopp/runtime.hpp",
};

// The throw machinery, above the function that threw.
bool is_throw_frame(std::string_view sym) noexcept {
    return sym.starts_with("_Unwind_") || sym == "__cxa_throw" || sym == "__cxa_rethrow" ||
           sym == "__cxa_call_terminate" || sym == "__gxx_personality_v0" || sym == "gopp_fiber_personality" ||
           sym.starts_with("_ZSt17rethrow_exception") || sym == "_ZSt9terminatev" ||
           sym.starts_with("_ZN10__cxxabiv111__terminate");
}

// Go hides the runtime's frames unless GOTRACEBACK=system. func and file
// are one call's, and sym is the symbol of the function the call is in.
bool hidden(std::string_view func, std::string_view file, std::string_view sym, int level) noexcept {
    if (level >= 2) return false;
    if (func.empty() && file.empty()) return true;
    if (in_namespace(func, "4gopp5panic")) return false;  // Go shows panic's frame
    if (in_namespace(func, "4gopp7runtime") || in_namespace(func, "5boost7context") || in_namespace(func, "4gopp2go"))
        return true;
    for (std::string_view s : start_frames)
        if (func == s) return true;
    if (!file.empty()) {
        if (runtime_dir[0] && file.starts_with(runtime_dir) && in_namespace(sym, "4gopp")) return true;
        if (file.find("/boost/context/") != std::string_view::npos) return true;
        for (std::string_view s : plumbing_files)
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

void demangle_cb(const char* s, size_t len, void* opaque) {
    auto* d = static_cast<demangled*>(opaque);
    if (d->n + len > d->cap) {
        len = d->cap - d->n;
        d->truncated = true;
    }
    std::memcpy(d->p + d->n, s, len);
    d->n += len;
}

char demangle_buf[16384];

// Writes a function's name, demangled, with its parameters elided Go's
// way, "ns::f(...)" or "ns::f()"; without them if !params.
void put_func(const char* name, bool params) noexcept {
    std::string_view s = name;
    // Names this long take the demangler more stack than the report has.
    if (s.starts_with("_Z") && s.size() < 8000) {
        demangled d{demangle_buf, 0, sizeof(demangle_buf), false};
        if (__gcclibcxx_demangle_callback(name, demangle_cb, &d) == 0 && !d.truncated) s = {d.p, d.n};
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

call call_at(int k) noexcept {
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

void put_source(const call& c, uintptr_t pc) noexcept {
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
        } else {
            out.put(" pc=");
            out.hex(pc);
        }
    }
    out.put('\n');
}

struct frame_opts {
    int level;
    bool fiber;       // stop at gentry
    bool skip_throw;  // skip the throw machinery at the top
};

// Prints the frames of s, Go's way:
//
//     ns::f(...)
//             /path/file.cpp:12 +0x1f
void print_frames(const stack_pcs& s, const frame_opts& o) noexcept {
    auto lookup_pc = [&](int i) { return s.ret[i] ? s.pc[i] - 1 : s.pc[i]; };
    int start = 0;
    if (o.skip_throw) {
        for (int i = 0; i < s.n && i < 48; i++) {
            lookup(lookup_pc(i));
            if (is_throw_frame(info.sym)) start = i + 1;
        }
    }
    int printed = 0;
    for (int i = start; i < s.n; i++) {
        lookup(lookup_pc(i));
        if (o.fiber && info.symval == gentry_pc()) return;
        for (int k = 0; k < ncalls(); k++) {
            call c = call_at(k);
            if (hidden(c.func, c.file, info.sym, o.level)) continue;
            if (printed == max_print) {
                out.put("...additional frames elided...\n");
                return;
            }
            put_func(c.func[0] ? c.func : "?", true);
            out.put('\n');
            put_source(c, s.pc[i]);
            printed++;
        }
    }
    if (s.more) out.put("...additional frames elided...\n");
}

// The function with the go statement, below gopp::go, at any level.
void print_created_by(G* gp) noexcept {
    if (!gp->gopc) return;
    lookup(gp->gopc - 1);
    for (int k = 0; k < ncalls(); k++) {
        call c = call_at(k);
        if (hidden(c.func, c.file, info.sym, 1)) continue;
        out.put("created by ");
        put_func(c.func[0] ? c.func : "?", false);
        if (gp->parent_goid) {
            out.put(" in goroutine ");
            out.dec(gp->parent_goid);
        }
        out.put('\n');
        put_source(c, gp->gopc);
        return;
    }
}

// ---------------------------------------------------------------------------
// Goroutine headers

std::string_view waitreason_string(waitreason w) noexcept {
    switch (w) {
    case waitreason::none: return "waiting";
    case waitreason::chan_receive: return "chan receive";
    case waitreason::chan_send: return "chan send";
    case waitreason::chan_receive_nil: return "chan receive (nil chan)";
    case waitreason::chan_send_nil: return "chan send (nil chan)";
    case waitreason::select: return "select";
    case waitreason::select_no_cases: return "select (no cases)";
    case waitreason::sleep: return "sleep";
    case waitreason::io_wait: return "IO wait";
    case waitreason::semacquire: return "semacquire";
    case waitreason::sync_mutex_lock: return "sync.Mutex.Lock";
    case waitreason::sync_rwmutex_rlock: return "sync.RWMutex.RLock";
    case waitreason::sync_rwmutex_lock: return "sync.RWMutex.Lock";
    case waitreason::sync_waitgroup_wait: return "sync.WaitGroup.Wait";
    case waitreason::sync_cond_wait: return "sync.Cond.Wait";
    }
    return "waiting";
}

std::string_view status_string(G* gp) noexcept {
    switch (gp->status.load(std::memory_order_relaxed)) {
    case gstatus::idle: return "idle";
    case gstatus::runnable: return "runnable";
    case gstatus::running: return "running";
    case gstatus::waiting: return waitreason_string(gp->wait);
    case gstatus::dead: return "dead";
    }
    return "unknown";
}

void put_thread_name(pid_t tid) noexcept {
    out.put("thread ");
    out.dec(static_cast<uint64_t>(tid));
    if (is_main_thread(tid)) out.put(" (main)");
}

void put_header(G* gp, std::string_view status) noexcept {
    if (gp->is_thread) {
        put_thread_name(gp->tid);
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

struct self_ctx {
    M* m;
    G* gp;  // the crashing fiber or pseudo-G; nullptr on an M's scheduler
    pid_t tid;
    bool skip_throw;
    stack_pcs* pcs;
};

constinit stack_pcs self_pcs;

self_ctx collect_self(skip_to kind, uintptr_t key, void* uc) noexcept {
    self_ctx c{getm(), current_g(), gettid(), false, &self_pcs};
    collect_here(self_pcs, kind, key);
    if (self_pcs.n == 0 && uc && c.gp && !c.gp->is_thread) {
        // The unwinder didn't get through the signal frame: walk the
        // fiber's frame pointers instead.
        self_pcs.push(uc_pc(uc), false);
        collect_fp(self_pcs, uc_fp(uc), c.gp->stack_lo, c.gp->stack_hi);
    }
    return c;
}

void print_self(const self_ctx& c, int level) noexcept {
    bool fiber = c.gp && !c.gp->is_thread;
    if (c.gp) {
        put_header(c.gp, "running");
    } else if (c.m) {
        out.put("runtime stack:\n");
        level = 2;  // it's all runtime
    } else {
        put_thread_name(c.tid);
        out.put(" [running]:\n");
    }
    print_frames(*c.pcs, {level, fiber, c.skip_throw});
    if (fiber) print_created_by(c.gp);
}

// ---------------------------------------------------------------------------
// Other threads' stacks, on request

struct stack_request {
    std::atomic<int> state{0};  // 0 none, 1 asked, 2 answering, 3 answered
    std::atomic<pid_t> tid{0};
    G* gp = nullptr;
    bool moved = false;  // the thread runs another goroutine now
    stack_pcs pcs;
};

constinit stack_request req;
// A thread took the request and didn't finish: it may still write.
constinit bool req_broken = false;

void sleep_ms(long ms) noexcept {
    struct timespec ts{0, ms * 1'000'000};
    ::nanosleep(&ts, nullptr);
}

// Asks thread tid, which runs gp, for its stack. Returns whether it answered.
bool request_stack(pid_t tid, G* gp) noexcept {
    if (req_broken) return false;
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
    for (int i = 0; i < 500 && req.state.load() == 1; i++) sleep_ms(1);
    int asked = 1;
    if (req.state.compare_exchange_strong(asked, 0)) return false;  // no answer
    for (int i = 0; i < 5000 && req.state.load() != 3; i++) sleep_ms(1);
    if (req.state.load() != 3) {
        req_broken = true;
        return false;
    }
    req.state.store(0);
    return true;
}

// Answers a request for the calling thread's stack, if there is one.
bool answer_request(skip_to kind, uintptr_t key, void* uc) noexcept {
    if (req.state.load(std::memory_order_acquire) != 1) return false;
    if (req.tid.load(std::memory_order_relaxed) != gettid()) return false;
    int asked = 1;
    if (!req.state.compare_exchange_strong(asked, 2)) return false;
    G* gp = current_g();
    if (gp != req.gp) {
        req.moved = true;
    } else {
        collect_here(req.pcs, kind, key);
        if (req.pcs.n == 0 && uc && gp && !gp->is_thread) {
            req.pcs.push(uc_pc(uc), false);
            collect_fp(req.pcs, uc_fp(uc), gp->stack_lo, gp->stack_hi);
        }
    }
    req.state.store(3, std::memory_order_release);
    return true;
}

constexpr std::string_view unavailable = "\tgoroutine running on other thread; stack unavailable\n";

constinit stack_pcs other_pcs;

void print_other(const self_ctx& c, G* gp, int level) noexcept {
    gstatus st = gp->status.load(std::memory_order_relaxed);
    if (st == gstatus::dead || st == gstatus::idle) return;
    if (gp->is_thread && gp->tid == 0) return;
    out.put('\n');
    put_header(gp, status_string(gp));
    bool fiber = !gp->is_thread;
    if (gp->is_thread || st == gstatus::running) {
        pid_t tid = gp->tid;
        if (fiber) {
            M* m = __atomic_load_n(&gp->m, __ATOMIC_RELAXED);
            tid = m && m != c.m ? m->tid : 0;
        }
        if (tid == 0 || tid == c.tid || !request_stack(tid, gp) || req.moved) {
            out.put(unavailable);
        } else {
            print_frames(req.pcs, {level, fiber, false});
        }
    } else if (gp->sched_fp) {
        other_pcs.clear();
        collect_fp(other_pcs, gp->sched_fp, gp->stack_lo, gp->stack_hi);
        print_frames(other_pcs, {level, true, false});
    }
    if (fiber) print_created_by(gp);
}

// main first, then goroutines by id, then other threads.
bool print_order(G* a, G* b) noexcept {
    auto rank = [](G* g) { return g->is_thread ? (is_main_thread(g->tid) ? 0 : 2) : 1; };
    int ra = rank(a), rb = rank(b);
    if (ra != rb) return ra < rb;
    return a->is_thread ? a->tid < b->tid : a->goid < b->goid;
}

void print_others(const self_ctx& c, int level) noexcept {
    size_t n = 0;
    for (G* gp = allgs(); gp; gp = gp->alllink) n++;
    size_t bytes = (n * sizeof(G*) + 4095) & ~size_t{4095};
    void* mem = n ? ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) : MAP_FAILED;
    if (mem == MAP_FAILED) {
        for (G* gp = allgs(); gp; gp = gp->alllink)
            if (gp != c.gp) print_other(c, gp, level);
        return;
    }
    G** v = static_cast<G**>(mem);
    size_t k = 0;
    for (G* gp = allgs(); gp && k < n; gp = gp->alllink) v[k++] = gp;
    std::sort(v, v + k, print_order);
    for (size_t i = 0; i < k; i++)
        if (v[i] != c.gp) print_other(c, v[i], level);
    ::munmap(mem, bytes);
}

void print_goroutines(const self_ctx& c, const traceback_settings& t, bool all, int self_level) noexcept {
    if (t.level == 0) return;
    find_runtime_dir();
    out.put('\n');
    print_self(c, self_level);
    if (all || t.all) print_others(c, t.level);
}

// ---------------------------------------------------------------------------
// Crashing

constinit std::atomic<pid_t> crashing{0};

[[noreturn]] void die(const traceback_settings& t) noexcept {
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
void crash_enter(skip_to kind, uintptr_t key, void* uc) noexcept {
    pid_t me = gettid();
    pid_t none = 0;
    if (crashing.compare_exchange_strong(none, me)) return;
    if (none == me) {
        static constexpr std::string_view msg = "\nfatal error: unexpected signal during traceback\n";
        [[maybe_unused]] ssize_t r = ::write(2, msg.data(), msg.size());
        die(gotraceback());
    }
    for (;;) {
        answer_request(kind, key, uc);
        sleep_ms(1);
    }
}

// A stack for the report, so that it doesn't depend on what is left of
// the crashing one.
struct crash_stack {
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
[[noreturn]] void on_crash_stack(F&& report) noexcept {
    constexpr std::size_t size = 1 << 20;
    void* p = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
    if (p != MAP_FAILED) {
        boost::context::fiber f(std::allocator_arg, crash_stack{p, size},
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

std::string_view signal_name(int sig) noexcept {
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
void put_signal_line(int sig, const siginfo_t* info, uintptr_t pc) noexcept {
    out.put("[signal ");
    out.put(signal_name(sig));
    out.put(" code=");
    out.hex(static_cast<uint64_t>(static_cast<int64_t>(info->si_code)));
    out.put(" addr=");
    out.hex(reinterpret_cast<uintptr_t>(info->si_addr));
    out.put(" pc=");
    out.hex(pc);
    out.put("]\n");
}

void put_reg(std::string_view name, uint64_t v) noexcept {
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
    for (const auto& r : regs) put_reg(r.name, static_cast<uint64_t>(g[r.reg]));
    uint64_t csgsfs = static_cast<uint64_t>(g[REG_CSGSFS]);
    put_reg("cs", csgsfs & 0xffff);
    put_reg("fs", (csgsfs >> 32) & 0xffff);
    put_reg("gs", (csgsfs >> 16) & 0xffff);
#elif defined(__aarch64__)
    static constexpr std::string_view names[] = {
        "r0",  "r1",  "r2",  "r3",  "r4",  "r5",  "r6",  "r7",  "r8",  "r9",  "r10", "r11", "r12", "r13", "r14",
        "r15", "r16", "r17", "r18", "r19", "r20", "r21", "r22", "r23", "r24", "r25", "r26", "r27", "r28", "r29",
    };
    for (int i = 0; i < 30; i++) put_reg(names[i], u->uc_mcontext.regs[i]);
    put_reg("lr", u->uc_mcontext.regs[30]);
    put_reg("sp", u->uc_mcontext.sp);
    put_reg("pc", u->uc_mcontext.pc);
    put_reg("fault", u->uc_mcontext.fault_address);
#endif
}

// The message of the exception being handled: its type and what().
struct exception_message {
    const std::type_info* type = nullptr;
    const char* what = nullptr;
};

exception_message current_exception_message() noexcept {
    exception_message m;
    m.type = abi::__cxa_current_exception_type();
    try {
        throw;
    } catch (const std::exception& e) {
        m.what = e.what();
    } catch (...) {
    }
    return m;
}

[[noreturn]] void report_exception() noexcept {
    crash_enter(skip_to::none, 0, nullptr);
    traceback_settings t = gotraceback();
    exception_message e = current_exception_message();
    self_ctx c = collect_self(skip_to::none, 0, nullptr);
    c.skip_throw = true;
    on_crash_stack([&] {
        out.put("panic: ");
        if (e.type) {
            // A type's mangled name, unlike a function's, has no "_Z".
            demangled d{demangle_buf, 0, sizeof(demangle_buf), false};
            if (__gcclibcxx_demangle_callback(e.type->name(), demangle_cb, &d) == 0 && !d.truncated)
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
        print_goroutines(c, t, false, t.level);
        die(t);
    });
}

}  // namespace

// ---------------------------------------------------------------------------
// Entry points

[[noreturn]] void dopanic(std::string_view msg) noexcept {
    uintptr_t fp = reinterpret_cast<uintptr_t>(__builtin_frame_address(0));
    crash_enter(skip_to::cfa, fp, nullptr);
    traceback_settings t = gotraceback();
    self_ctx c = collect_self(skip_to::cfa, fp, nullptr);
    on_crash_stack([&] {
        out.put("panic: ");
        out.put(msg);
        out.put('\n');
        print_goroutines(c, t, false, t.level);
        die(t);
    });
}

[[noreturn]] void dofatal(const char* msg) noexcept {
    uintptr_t fp = reinterpret_cast<uintptr_t>(__builtin_frame_address(0));
    crash_enter(skip_to::cfa, fp, nullptr);
    traceback_settings t = gotraceback();
    self_ctx c = collect_self(skip_to::cfa, fp, nullptr);
    on_crash_stack([&] {
        out.put("fatal error: ");
        out.put(msg);
        out.put('\n');
        // The runtime's frames show for the thrower, as in Go's throw.
        print_goroutines(c, t, true, std::max(t.level, 2));
        die(t);
    });
}

[[noreturn]] void crash_signal(int sig, siginfo_t* info, void* uc) noexcept {
    uintptr_t pc = uc_pc(uc);
    crash_enter(skip_to::pc, pc, uc);
    traceback_settings t = gotraceback();
    self_ctx c = collect_self(skip_to::pc, pc, uc);
    on_crash_stack([&] {
        uintptr_t addr = reinterpret_cast<uintptr_t>(info->si_addr);
        bool fault = sig == SIGSEGV || sig == SIGBUS;
        bool all = true;
        bool regs = false;
        G* gp = c.gp;
        if (fault && gp && !gp->is_thread && addr < gp->stack_lo && addr + 65536 >= gp->stack_lo) {
            out.put("runtime: goroutine stack exceeds ");
            out.dec(gp->stack_hi - gp->stack_lo);
            out.put("-byte limit\nruntime: sp=");
            out.hex(uc_sp(uc));
            out.put(" stack=[");
            out.hex(gp->stack_lo);
            out.put(", ");
            out.hex(gp->stack_hi);
            out.put("]\nfatal error: stack overflow\n");
        } else if (fault && addr < 0x1000) {
            out.put("panic: runtime error: invalid memory address or nil pointer dereference\n");
            put_signal_line(sig, info, pc);
            all = false;
        } else if (fault) {
            out.put("unexpected fault address ");
            out.hex(addr);
            out.put("\nfatal error: fault\n");
            put_signal_line(sig, info, pc);
        } else if (sig == SIGFPE && info->si_code > 0) {
            out.put(info->si_code == FPE_INTDIV   ? "panic: runtime error: integer divide by zero\n"
                    : info->si_code == FPE_INTOVF ? "panic: runtime error: integer overflow\n"
                                                  : "panic: runtime error: floating point error\n");
            put_signal_line(sig, info, pc);
            all = false;
        } else {
            out.put(signal_name(sig));
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
        print_goroutines(c, t, all, t.level);
        if (regs && t.level > 0) {
            out.put('\n');
            dumpregs(uc);
        }
        die(t);
    });
}

void traceback_signal(void* uc) noexcept {
    if (req.state.load(std::memory_order_relaxed) != 1) return;
    if (!answer_request(skip_to::pc, uc_pc(uc), uc)) return;
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
[[gnu::noinline]] void run_fiber_fn(G* gp) {
    asm volatile(".cfi_personality 0x1b, gopp_fiber_personality");
    gp->fn();
    asm volatile("");
}

[[noreturn]] void terminate_handler() noexcept {
    // __cxa_throw calls std::terminate without unwinding when no frame
    // handles the exception, so the stack is as it was at the throw.
    if (std::current_exception()) report_exception();
    dofatal("std::terminate called without an active exception");
}

namespace {

constexpr std::size_t altstack_size = 64 * 1024;
constexpr std::size_t altstack_guard = 64 * 1024;  // a guard of the largest page size
thread_local void* tls_altstack = nullptr;

}  // namespace

void ensure_sigaltstack() noexcept {
    stack_t old{};
    if (::sigaltstack(nullptr, &old) == 0 && !(old.ss_flags & SS_DISABLE)) return;
    void* p = ::mmap(nullptr, altstack_guard + altstack_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return;
    ::mprotect(p, altstack_guard, PROT_NONE);
    stack_t ss{};
    ss.ss_sp = static_cast<char*>(p) + altstack_guard;
    ss.ss_size = altstack_size;
    if (::sigaltstack(&ss, nullptr) != 0) {
        ::munmap(p, altstack_guard + altstack_size);
        return;
    }
    tls_altstack = p;
}

void release_sigaltstack() noexcept {
    if (!tls_altstack) return;
    stack_t ss{};
    ss.ss_flags = SS_DISABLE;
    ::sigaltstack(&ss, nullptr);
    ::munmap(tls_altstack, altstack_guard + altstack_size);
    tls_altstack = nullptr;
}

}  // namespace gopp::runtime

// Used: only run_fiber_fn's unwind information refers to it.
extern "C" [[gnu::used]] _Unwind_Reason_Code gopp_fiber_personality(int, _Unwind_Action actions, _Unwind_Exception_Class,
                                                      _Unwind_Exception* ue, _Unwind_Context*) {
    // Forced unwinds (thread cancellation) and the cleanup phase pass
    // through.
    if (!(actions & _UA_SEARCH_PHASE) || (actions & _UA_FORCE_UNWIND)) return _URC_CONTINUE_UNWIND;
    __cxxabiv1::__cxa_begin_catch(ue);
    gopp::runtime::report_exception();
}
