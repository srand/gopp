// The regexp package, ported from Go 1.27.3's regexp: regexp.go, exec.go,
// backtrack.go and onepass.go.
#include <gopp/regexp.hpp>
#include <gopp/strings.hpp>
#include <gopp/sync.hpp>
#include <gopp/unicode.hpp>
#include <gopp/unicode/utf8.hpp>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <optional>

namespace gopp::regexp {

using syntax::EmptyOp;
using syntax::Inst;
using syntax::InstOp;

namespace {

constexpr rune endOfText = -1;

// lazyFlag is a lazily-evaluated syntax.EmptyOp, for checking zero-width
// flags like ^ $ \A \z \B \b. It records the pair of relevant runes and
// does not determine the implied flags until absolutely necessary (most of
// the time, that means never).
using lazyFlag = uint64;

lazyFlag newLazyFlag(rune r1, rune r2) { return lazyFlag(uint64(uint32(r1)) << 32 | uint64(uint32(r2))); }

bool flagMatch(lazyFlag f, EmptyOp op) {
    if (op == 0) return true;
    rune r1 = rune(int32(uint32(f >> 32)));
    if (op & syntax::EmptyBeginLine) {
        if (r1 != '\n' && r1 >= 0) return false;
        op &= ~syntax::EmptyBeginLine;
    }
    if (op & syntax::EmptyBeginText) {
        if (r1 >= 0) return false;
        op &= ~syntax::EmptyBeginText;
    }
    if (op == 0) return true;
    rune r2 = rune(int32(uint32(f)));
    if (op & syntax::EmptyEndLine) {
        if (r2 != '\n' && r2 >= 0) return false;
        op &= ~syntax::EmptyEndLine;
    }
    if (op & syntax::EmptyEndText) {
        if (r2 >= 0) return false;
        op &= ~syntax::EmptyEndText;
    }
    if (op == 0) return true;
    if (syntax::IsWordChar(r1) != syntax::IsWordChar(r2)) {
        op &= ~syntax::EmptyWordBoundary;
    } else {
        op &= ~syntax::EmptyNoWordBoundary;
    }
    return op == 0;
}

// input abstracts different representations of the input text. It
// provides one-character lookahead.
struct input {
    virtual ~input() = default;
    virtual std::pair<rune, int> step(int pos) = 0;  // advance one rune
    virtual bool canCheckPrefix() const = 0;         // can we look ahead without losing info?
    virtual bool hasPrefix(std::string_view prefix) const = 0;
    virtual int index(std::string_view prefix, int pos) const = 0;
    virtual lazyFlag context(int pos) const = 0;
};

// inputString scans a string (or the bytes of a slice).
struct inputString final : input {
    std::string_view str;
    explicit inputString(std::string_view s) : str(s) {}

    std::pair<rune, int> step(int pos) override {
        if (std::size_t(pos) < str.size()) {
            if (byte c = byte(str[std::size_t(pos)]); c < utf8::RuneSelf) return {rune(c), 1};
            return utf8::DecodeRuneInString(str.substr(std::size_t(pos)));
        }
        return {endOfText, 0};
    }
    bool canCheckPrefix() const override { return true; }
    bool hasPrefix(std::string_view prefix) const override { return str.starts_with(prefix); }
    int index(std::string_view prefix, int pos) const override {
        auto i = str.substr(std::size_t(pos)).find(prefix);
        return i == std::string_view::npos ? -1 : int(i);
    }
    lazyFlag context(int pos) const override {
        rune r1 = endOfText, r2 = endOfText;
        // 0 < pos && pos <= len(i.str)
        if (unsigned(pos - 1) < str.size()) r1 = utf8::DecodeLastRuneInString(str.substr(0, std::size_t(pos))).first;
        // 0 <= pos && pos < len(i.str)
        if (unsigned(pos) < str.size()) r2 = utf8::DecodeRuneInString(str.substr(std::size_t(pos))).first;
        return newLazyFlag(r1, r2);
    }
};

// inputReader scans a RuneReader.
struct inputReader final : input {
    io::RuneReader* r;
    bool atEOT = false;
    int pos = 0;
    explicit inputReader(io::RuneReader* rr) : r(rr) {}

    std::pair<rune, int> step(int p) override {
        if (!atEOT && p != pos) return {endOfText, 0};
        auto [c, w, err] = r->ReadRune();
        if (err) {
            atEOT = true;
            return {endOfText, 0};
        }
        pos += w;
        return {c, w};
    }
    bool canCheckPrefix() const override { return false; }
    bool hasPrefix(std::string_view) const override { return false; }
    int index(std::string_view, int) const override { return -1; }
    lazyFlag context(int) const override { return 0; }  // not used
};

InstOp iop(const Inst& i) {
    switch (i.Op) {
    case syntax::InstRune1:
    case syntax::InstRuneAny:
    case syntax::InstRuneAnyNotNL:
        return syntax::InstRune;
    default:
        return i.Op;
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// One-pass programs (onepass.go)

namespace detail {

// onePassInst is a single instruction in a one-pass regular expression
// program. It is the same as syntax.Inst except for the new 'Next' field.
struct onePassInst : syntax::Inst {
    std::vector<uint32> Next;
};

// onePassProg is a compiled one-pass regular expression program. It is
// the same as syntax.Prog except for the use of onePassInst.
struct onePassProg {
    std::vector<onePassInst> Inst;
    int Start = 0;   // index of start instruction
    int NumCap = 0;  // number of InstCapture insts in re
};

}  // namespace detail

namespace {

using detail::onePassInst;
using detail::onePassProg;

// onePassPrefix returns a literal string that all matches for the regexp
// must start with. Complete is true if the prefix is the entire match. Pc
// is the index of the last rune instruction in the string. The
// onePassPrefix skips over the mandatory EmptyBeginText.
std::tuple<string, bool, uint32> onePassPrefix(const syntax::Prog& p) {
    const Inst* i = &p.Inst[std::size_t(p.Start)];
    if (i->Op != syntax::InstEmptyWidth || !(EmptyOp(i->Arg) & syntax::EmptyBeginText)) {
        return {{}, i->Op == syntax::InstMatch, uint32(p.Start)};
    }
    uint32 pc = i->Out;
    i = &p.Inst[pc];
    while (i->Op == syntax::InstNop) {
        pc = i->Out;
        i = &p.Inst[pc];
    }
    // Avoid allocation of buffer if prefix is empty.
    if (iop(*i) != syntax::InstRune || i->Rune.size() != 1) return {{}, i->Op == syntax::InstMatch, uint32(p.Start)};

    // Have prefix; gather characters.
    std::string buf;
    while (iop(*i) == syntax::InstRune && i->Rune.size() == 1 && !(syntax::Flags(i->Arg) & syntax::FoldCase) &&
           i->Rune[0] != utf8::RuneError) {
        char tmp[4];
        auto n = utf8::EncodeRune(std::span<byte>(reinterpret_cast<byte*>(tmp), 4), i->Rune[0]);
        buf.append(tmp, std::size_t(n));
        pc = i->Out;
        i = &p.Inst[i->Out];
    }
    bool complete = false;
    if (i->Op == syntax::InstEmptyWidth && (EmptyOp(i->Arg) & syntax::EmptyEndText) &&
        p.Inst[i->Out].Op == syntax::InstMatch) {
        complete = true;
    }
    return {string(buf), complete, pc};
}

// onePassNext selects the next actionable state of the prog, based on the
// input character. It should only be called when i.Op == InstAlt or
// InstAltMatch, and is used to determine the next state to enter.
uint32 onePassNext(const onePassInst& i, rune r) {
    int next = i.MatchRunePos(r);
    if (next >= 0) return i.Next[std::size_t(next)];
    if (i.Op == syntax::InstAltMatch) return i.Out;
    return 0;
}

// Sparse Array implementation is used as a queueOnePass.
struct queueOnePass {
    std::vector<uint32> sparse, dense;
    uint32 size = 0, nextIndex = 0;

    explicit queueOnePass(std::size_t n) : sparse(n), dense(n) {}
    bool empty() const { return nextIndex >= size; }
    uint32 next() { return dense[nextIndex++]; }
    void clear() { size = nextIndex = 0; }
    bool contains(uint32 u) const {
        if (u >= sparse.size()) return false;
        return sparse[u] < size && dense[sparse[u]] == u;
    }
    void insert(uint32 u) {
        if (!contains(u)) insertNew(u);
    }
    void insertNew(uint32 u) {
        if (u >= sparse.size()) return;
        sparse[u] = size;
        dense[size] = u;
        size++;
    }
};

constexpr uint32 mergeFailed = 0xffffffff;

// mergeRuneSets merges two non-intersecting runesets, and returns the
// merged result, and a NextIp array. The idea is that if a rune matches the
// OnePassRunes at index i, NextIp[i/2] is the target. If the input sets
// intersect, an empty runeset and a NextIp array with the single element
// mergeFailed is returned. The code assumes that both inputs contain
// ordered and non-intersecting rune pairs.
std::pair<std::vector<rune>, std::vector<uint32>> mergeRuneSets(const std::vector<rune>& leftRunes,
                                                                const std::vector<rune>& rightRunes, uint32 leftPC,
                                                                uint32 rightPC) {
    std::size_t leftLen = leftRunes.size(), rightLen = rightRunes.size();
    if ((leftLen & 1) || (rightLen & 1)) panic("mergeRuneSets odd length []rune");
    std::size_t lx = 0, rx = 0;
    std::vector<rune> merged;
    std::vector<uint32> next;
    isize ix = -1;
    auto extend = [&](std::size_t& newLow, const std::vector<rune>& newArray, uint32 pc) {
        if (ix > 0 && newArray[newLow] <= merged[std::size_t(ix)]) return false;
        merged.push_back(newArray[newLow]);
        merged.push_back(newArray[newLow + 1]);
        newLow += 2;
        ix += 2;
        next.push_back(pc);
        return true;
    };
    while (lx < leftLen || rx < rightLen) {
        bool ok;
        if (rx >= rightLen) {
            ok = extend(lx, leftRunes, leftPC);
        } else if (lx >= leftLen) {
            ok = extend(rx, rightRunes, rightPC);
        } else if (rightRunes[rx] < leftRunes[lx]) {
            ok = extend(rx, rightRunes, rightPC);
        } else {
            ok = extend(lx, leftRunes, leftPC);
        }
        if (!ok) return {{}, {mergeFailed}};
    }
    return {std::move(merged), std::move(next)};
}

// cleanupOnePass drops working memory, and restores certain shortcut
// instructions.
void cleanupOnePass(onePassProg& prog, const syntax::Prog& original) {
    for (std::size_t ix = 0; ix < original.Inst.size(); ix++) {
        const Inst& instOriginal = original.Inst[ix];
        switch (instOriginal.Op) {
        case syntax::InstAlt:
        case syntax::InstAltMatch:
        case syntax::InstRune:
            break;
        case syntax::InstCapture:
        case syntax::InstEmptyWidth:
        case syntax::InstNop:
        case syntax::InstMatch:
        case syntax::InstFail:
            prog.Inst[ix].Next.clear();
            break;
        case syntax::InstRune1:
        case syntax::InstRuneAny:
        case syntax::InstRuneAnyNotNL:
            prog.Inst[ix] = onePassInst{instOriginal, {}};
            break;
        }
    }
}

// onePassCopy creates a copy of the original Prog, as we'll be modifying it.
std::unique_ptr<onePassProg> onePassCopy(const syntax::Prog& prog) {
    auto p = std::make_unique<onePassProg>();
    p->Start = prog.Start;
    p->NumCap = prog.NumCap;
    p->Inst.reserve(prog.Inst.size());
    for (const auto& inst : prog.Inst) p->Inst.push_back(onePassInst{inst, {}});

    // rewrites one or more common Prog constructs that enable some otherwise
    // non-onepass Progs to be onepass. A:BD (for example) means an InstAlt
    // at pc A, which has Out and Arg values B and D.
    auto isAlt = [](const Inst& i) { return i.Op == syntax::InstAlt || i.Op == syntax::InstAltMatch; };
    for (std::size_t pc = 0; pc < p->Inst.size(); pc++) {
        if (!isAlt(p->Inst[pc])) continue;
        // A:Bx + B:Ay
        uint32* p_A_Other = &p->Inst[pc].Out;
        uint32* p_A_Alt = &p->Inst[pc].Arg;
        // make sure a target is another Alt
        onePassInst instAlt = p->Inst[*p_A_Alt];
        if (!isAlt(instAlt)) {
            std::swap(p_A_Alt, p_A_Other);
            instAlt = p->Inst[*p_A_Alt];
            if (!isAlt(instAlt)) continue;
        }
        onePassInst instOther = p->Inst[*p_A_Other];
        // Analyzing both legs pointing to Alts is for another day
        if (isAlt(instOther)) continue;
        // simple empty transition loop
        // A:BC + B:DA => A:BC + B:DC
        uint32* p_B_Alt = &p->Inst[*p_A_Alt].Out;
        uint32* p_B_Other = &p->Inst[*p_A_Alt].Arg;
        bool patch = false;
        if (instAlt.Out == uint32(pc)) {
            patch = true;
        } else if (instAlt.Arg == uint32(pc)) {
            patch = true;
            std::swap(p_B_Alt, p_B_Other);
        }
        if (patch) *p_B_Alt = *p_A_Other;
        // empty transition to common target
        // A:BC + B:DC => A:DC + B:DC
        if (*p_A_Other == *p_B_Alt) *p_A_Alt = *p_B_Other;
    }
    return p;
}

constexpr rune anyRuneNotNL[] = {0, '\n' - 1, '\n' + 1, unicode::MaxRune};
constexpr rune anyRune[] = {0, unicode::MaxRune};

// makeOnePass creates a onepass Prog, if possible. It is possible if at any
// alt, the match engine can always tell which branch to take. The routine
// may modify p if it is turned into a onepass Prog. If it isn't possible
// for this to be a onepass Prog, the Prog nullptr is returned.
// makeOnePass is recursive to the size of the Prog.
std::unique_ptr<onePassProg> makeOnePass(std::unique_ptr<onePassProg> p) {
    // If the machine is very long, it's not worth the time to check if we
    // can use one pass.
    if (p->Inst.size() >= 1000) return nullptr;

    queueOnePass instQueue(p->Inst.size()), visitQueue(p->Inst.size());
    std::vector<std::vector<rune>> onePassRunes(p->Inst.size());
    std::vector<bool> m(p->Inst.size());

    auto fillNext = [](onePassInst& inst, std::size_t nruns) { inst.Next.assign(nruns / 2 + 1, inst.Out); };

    // check that each path through the program can be matched in one pass
    std::function<bool(uint32)> check = [&](uint32 pc) -> bool {
        bool ok = true;
        onePassInst& inst = p->Inst[pc];
        if (visitQueue.contains(pc)) return ok;
        visitQueue.insert(pc);
        switch (inst.Op) {
        case syntax::InstAlt:
        case syntax::InstAltMatch: {
            ok = check(inst.Out) && check(inst.Arg);
            // check no-input paths to InstMatch
            bool matchOut = m[inst.Out];
            bool matchArg = m[inst.Arg];
            if (matchOut && matchArg) {
                ok = false;
                break;
            }
            // Match on empty goes in inst.Out
            if (matchArg) {
                std::swap(inst.Out, inst.Arg);
                std::swap(matchOut, matchArg);
            }
            if (matchOut) {
                m[pc] = true;
                inst.Op = syntax::InstAltMatch;
            }
            // build a dispatch operator from the two legs
            std::tie(onePassRunes[pc], inst.Next) =
                mergeRuneSets(onePassRunes[inst.Out], onePassRunes[inst.Arg], inst.Out, inst.Arg);
            if (!inst.Next.empty() && inst.Next[0] == mergeFailed) {
                ok = false;
                break;
            }
            break;
        }
        case syntax::InstCapture:
        case syntax::InstNop:
        case syntax::InstEmptyWidth:
            ok = check(inst.Out);
            m[pc] = m[inst.Out];
            onePassRunes[pc] = onePassRunes[inst.Out];
            fillNext(inst, onePassRunes[pc].size());
            break;
        case syntax::InstMatch:
        case syntax::InstFail:
            m[pc] = inst.Op == syntax::InstMatch;
            break;
        case syntax::InstRune: {
            m[pc] = false;
            if (!inst.Next.empty()) break;
            instQueue.insert(inst.Out);
            if (inst.Rune.empty()) {
                onePassRunes[pc].clear();
                inst.Next = {inst.Out};
                break;
            }
            std::vector<rune> runes;
            if (inst.Rune.size() == 1 && (syntax::Flags(inst.Arg) & syntax::FoldCase)) {
                rune r0 = inst.Rune[0];
                runes = {r0, r0};
                for (rune r1 = unicode::SimpleFold(r0); r1 != r0; r1 = unicode::SimpleFold(r1)) {
                    runes.push_back(r1);
                    runes.push_back(r1);
                }
                std::sort(runes.begin(), runes.end());
            } else {
                runes = inst.Rune;
            }
            onePassRunes[pc] = runes;
            fillNext(inst, runes.size());
            inst.Op = syntax::InstRune;
            break;
        }
        case syntax::InstRune1: {
            m[pc] = false;
            if (!inst.Next.empty()) break;
            instQueue.insert(inst.Out);
            std::vector<rune> runes;
            // expand case-folded runes
            if (syntax::Flags(inst.Arg) & syntax::FoldCase) {
                rune r0 = inst.Rune[0];
                runes = {r0, r0};
                for (rune r1 = unicode::SimpleFold(r0); r1 != r0; r1 = unicode::SimpleFold(r1)) {
                    runes.push_back(r1);
                    runes.push_back(r1);
                }
                std::sort(runes.begin(), runes.end());
            } else {
                runes = {inst.Rune[0], inst.Rune[0]};
            }
            onePassRunes[pc] = runes;
            fillNext(inst, runes.size());
            inst.Op = syntax::InstRune;
            break;
        }
        case syntax::InstRuneAny:
            m[pc] = false;
            if (!inst.Next.empty()) break;
            instQueue.insert(inst.Out);
            onePassRunes[pc].assign(std::begin(anyRune), std::end(anyRune));
            inst.Next = {inst.Out};
            break;
        case syntax::InstRuneAnyNotNL:
            m[pc] = false;
            if (!inst.Next.empty()) break;
            instQueue.insert(inst.Out);
            onePassRunes[pc].assign(std::begin(anyRuneNotNL), std::end(anyRuneNotNL));
            fillNext(inst, onePassRunes[pc].size());
            break;
        }
        return ok;
    };

    instQueue.clear();
    instQueue.insert(uint32(p->Start));
    while (!instQueue.empty()) {
        visitQueue.clear();
        uint32 pc = instQueue.next();
        if (!check(pc)) return nullptr;
    }
    for (std::size_t i = 0; i < p->Inst.size(); i++) p->Inst[i].Rune = onePassRunes[i];
    return p;
}

// compileOnePass returns a new *syntax.Prog suitable for onePass execution
// if the original Prog can be recharacterized as a one-pass regexp program,
// or syntax.nil if the Prog cannot be converted. For a one pass prog, the
// fundamental condition that must be true is: at any InstAlt, there must be
// no ambiguity about what branch to take.
std::unique_ptr<onePassProg> compileOnePass(const syntax::Prog& prog) {
    if (prog.Start == 0) return nullptr;
    // onepass regexp is anchored
    const Inst& start = prog.Inst[std::size_t(prog.Start)];
    if (start.Op != syntax::InstEmptyWidth ||
        (EmptyOp(start.Arg) & syntax::EmptyBeginText) != syntax::EmptyBeginText) {
        return nullptr;
    }
    bool hasAlt = false;
    for (const auto& inst : prog.Inst) {
        if (inst.Op == syntax::InstAlt || inst.Op == syntax::InstAltMatch) {
            hasAlt = true;
            break;
        }
    }
    // If we have alternates, every instruction leading to InstMatch must be
    // EmptyEndText. Also, any match on empty text must be $.
    for (const auto& inst : prog.Inst) {
        InstOp opOut = prog.Inst[inst.Out].Op;
        switch (inst.Op) {
        default:
            if (opOut == syntax::InstMatch && hasAlt) return nullptr;
            break;
        case syntax::InstAlt:
        case syntax::InstAltMatch:
            if (opOut == syntax::InstMatch || prog.Inst[inst.Arg].Op == syntax::InstMatch) return nullptr;
            break;
        case syntax::InstEmptyWidth:
            if (opOut == syntax::InstMatch) {
                if ((EmptyOp(inst.Arg) & syntax::EmptyEndText) == syntax::EmptyEndText) continue;
                return nullptr;
            }
            break;
        }
    }
    // Creates a slightly optimized copy of the original Prog that cleans up
    // some Prog idioms that block valid onepass programs.
    auto p = makeOnePass(onePassCopy(prog));
    if (p) cleanupOnePass(*p, prog);
    return p;
}

}  // namespace

// ---------------------------------------------------------------------------
// Machines (exec.go, backtrack.go)

namespace {

// A thread is the state of a single path through the machine: an
// instruction and a corresponding capture array.
struct thread {
    const Inst* inst = nullptr;
    std::vector<int> cap;
};

// An entry is an entry on a queue. It holds both the instruction pc and
// the actual thread. Some queue entries are just place holders so that
// the machine knows it has considered that pc. Such entries have t ==
// nil.
struct entry {
    uint32 pc = 0;
    thread* t = nullptr;
};

// A queue is a 'sparse array' holding pending threads of execution. See
// https://research.swtch.com/2008/03/using-uninitialized-memory-for-fun-and.html
struct queue {
    std::vector<uint32> sparse;
    std::vector<entry> dense;
    std::size_t len = 0;  // the entries of dense in use
};

// A job is an entry on the backtracker's job stack. It holds the
// instruction pc and the position in the input.
struct job {
    uint32 pc;
    bool arg;
    int pos;
};

constexpr int visitedBits = 32;
constexpr std::size_t maxBacktrackProg = 500;          // len(prog.Inst) <= max
constexpr std::size_t maxBacktrackVector = 256 * 1024;  // bit vector size <= max (bits)

}  // namespace

namespace detail {

// A machine holds all the state during an NFA simulation for p.
struct machine {
    const syntax::Prog* p = nullptr;  // compiled program
    queue q0, q1;                     // two queues for runq, nextq
    std::vector<std::unique_ptr<thread>> threads;
    std::vector<thread*> pool;  // pool of available threads
    bool matched = false;       // whether a match was found
    std::vector<int> matchcap;  // capture information for the match
    bool longest = false;
    EmptyOp cond = 0;
    std::string_view prefix;
    rune prefixRune = 0;

    void init(int ncap) {
        for (auto& t : threads) t->cap.resize(std::size_t(ncap));
        matchcap.resize(std::size_t(ncap));
    }

    // alloc allocates a new thread with the given instruction. It uses the
    // free pool if possible.
    thread* alloc(const Inst* i) {
        thread* t;
        if (!pool.empty()) {
            t = pool.back();
            pool.pop_back();
        } else {
            threads.push_back(std::make_unique<thread>());
            t = threads.back().get();
            t->cap.resize(matchcap.size());
        }
        t->inst = i;
        return t;
    }

    // match runs the machine over the input starting at pos. It reports
    // whether a match was found. If so, m.matchcap holds the submatch
    // information.
    bool match(input& i, int pos) {
        EmptyOp startCond = cond;
        if (startCond == EmptyOp(~EmptyOp(0))) return false;  // impossible
        matched = false;
        std::fill(matchcap.begin(), matchcap.end(), -1);
        queue* runq = &q0;
        queue* nextq = &q1;
        rune r = endOfText, r1 = endOfText;
        int width = 0, width1 = 0;
        std::tie(r, width) = i.step(pos);
        if (r != endOfText) std::tie(r1, width1) = i.step(pos + width);
        lazyFlag flag;
        if (pos == 0) {
            flag = newLazyFlag(-1, r);
        } else {
            flag = i.context(pos);
        }
        for (;;) {
            if (runq->len == 0) {
                if ((startCond & syntax::EmptyBeginText) && pos != 0) {
                    // Anchored match, past beginning of text.
                    break;
                }
                if (matched) {
                    // Have match; finished exploring alternatives.
                    break;
                }
                if (!prefix.empty() && r1 != prefixRune && i.canCheckPrefix()) {
                    // Match requires literal prefix; fast search for it.
                    int advance = i.index(prefix, pos);
                    if (advance < 0) break;
                    pos += advance;
                    std::tie(r, width) = i.step(pos);
                    std::tie(r1, width1) = i.step(pos + width);
                }
            }
            if (!matched) {
                if (!matchcap.empty()) matchcap[0] = pos;
                add(*runq, uint32(p->Start), pos, matchcap, flag, nullptr);
            }
            flag = newLazyFlag(r, r1);
            step(*runq, *nextq, pos, pos + width, r, flag);
            if (width == 0) break;
            if (matchcap.empty() && matched) {
                // Found a match and not paying attention to where it is,
                // so any match will do.
                break;
            }
            pos += width;
            r = r1;
            width = width1;
            if (r != endOfText) std::tie(r1, width1) = i.step(pos + width);
            std::swap(runq, nextq);
        }
        clear(*nextq);
        return matched;
    }

    // clear frees all threads on the thread queue.
    void clear(queue& q) {
        for (std::size_t k = 0; k < q.len; k++) {
            if (q.dense[k].t) pool.push_back(q.dense[k].t);
        }
        q.len = 0;
    }

    // step executes one step of the machine, running each of the threads on
    // runq and appending new threads to nextq. The step processes the rune
    // c (which may be endOfText), which starts at position pos and ends at
    // nextPos. nextCond gives the setting for the empty-width flags after
    // c.
    void step(queue& runq, queue& nextq, int pos, int nextPos, rune c, lazyFlag nextCond) {
        for (std::size_t j = 0; j < runq.len; j++) {
            entry& d = runq.dense[j];
            thread* t = d.t;
            if (!t) continue;
            if (longest && matched && !t->cap.empty() && matchcap[0] < t->cap[0]) {
                pool.push_back(t);
                continue;
            }
            const Inst* i = t->inst;
            bool add = false;
            switch (i->Op) {
            default:
                panic("bad inst");
            case syntax::InstMatch:
                if (!t->cap.empty() && (!longest || !matched || matchcap[1] < pos)) {
                    t->cap[1] = pos;
                    std::copy(t->cap.begin(), t->cap.end(), matchcap.begin());
                }
                if (!longest) {
                    // First-match mode: cut off all lower-priority threads.
                    for (std::size_t k = j + 1; k < runq.len; k++) {
                        if (runq.dense[k].t) pool.push_back(runq.dense[k].t);
                    }
                    runq.len = 0;
                }
                matched = true;
                break;
            case syntax::InstRune:
                add = i->MatchRune(c);
                break;
            case syntax::InstRune1:
                add = c == i->Rune[0];
                break;
            case syntax::InstRuneAny:
                add = true;
                break;
            case syntax::InstRuneAnyNotNL:
                add = c != '\n';
                break;
            }
            if (add) t = this->add(nextq, i->Out, nextPos, t->cap, nextCond, t);
            if (t) pool.push_back(t);
        }
        runq.len = 0;
    }

    // add adds an entry to q for pc, unless the q already has such an entry.
    // It also recursively adds an entry for all instructions reachable from
    // pc by following empty-width conditions satisfied by cond. pos gives
    // the current position in the input.
    thread* add(queue& q, uint32 pc, int pos, std::vector<int>& cap, lazyFlag cond, thread* t) {
        for (;;) {
            if (pc == 0) return t;
            if (uint32 j = q.sparse[pc]; j < q.len && q.dense[j].pc == pc) return t;

            std::size_t j = q.len++;
            entry& d = q.dense[j];
            d.t = nullptr;
            d.pc = pc;
            q.sparse[pc] = uint32(j);

            const Inst* i = &p->Inst[pc];
            switch (i->Op) {
            default:
                panic("unhandled");
            case syntax::InstFail:
                // nothing
                return t;
            case syntax::InstAlt:
            case syntax::InstAltMatch:
                t = add(q, i->Out, pos, cap, cond, t);
                pc = i->Arg;
                continue;
            case syntax::InstEmptyWidth:
                if (flagMatch(cond, EmptyOp(i->Arg))) {
                    pc = i->Out;
                    continue;
                }
                return t;
            case syntax::InstNop:
                pc = i->Out;
                continue;
            case syntax::InstCapture:
                if (i->Arg < cap.size()) {
                    int opos = cap[i->Arg];
                    cap[i->Arg] = pos;
                    add(q, i->Out, pos, cap, cond, nullptr);
                    cap[i->Arg] = opos;
                    return t;
                }
                pc = i->Out;
                continue;
            case syntax::InstMatch:
            case syntax::InstRune:
            case syntax::InstRune1:
            case syntax::InstRuneAny:
            case syntax::InstRuneAnyNotNL:
                if (!t) {
                    t = alloc(i);
                } else {
                    t->inst = i;
                }
                if (!cap.empty() && t->cap.data() != cap.data()) std::copy(cap.begin(), cap.end(), t->cap.begin());
                q.dense[j].t = t;
                return nullptr;
            }
        }
    }
};

// bitState holds state for the backtracker.
struct bitState {
    int end = 0;
    std::vector<int> cap;
    std::vector<int> matchcap;
    std::vector<job> jobs;
    std::vector<uint32> visited;

    // reset resets the state of the backtracker. end is the end position in
    // the input. ncap is the number of captures.
    void reset(const syntax::Prog& prog, int e, int ncap) {
        end = e;
        jobs.clear();
        std::size_t visitedSize = (prog.Inst.size() * std::size_t(e + 1) + visitedBits - 1) / visitedBits;
        visited.assign(visitedSize, 0);
        cap.assign(std::size_t(ncap), -1);
        matchcap.assign(std::size_t(ncap), -1);
    }

    // shouldVisit reports whether the combination of (pc, pos) has not been
    // visited yet.
    bool shouldVisit(uint32 pc, int pos) {
        std::size_t n = std::size_t(pc) * std::size_t(end + 1) + std::size_t(pos);
        uint32 bit = uint32(1) << (n & (visitedBits - 1));
        if (visited[n / visitedBits] & bit) return false;
        visited[n / visitedBits] |= bit;
        return true;
    }

    // push pushes (pc, pos, arg) onto the job stack if it should be visited.
    void push(const syntax::Prog& prog, uint32 pc, int pos, bool arg) {
        // Only check shouldVisit when arg is false. When arg is true, we
        // are continuing a previous visit.
        if (prog.Inst[pc].Op != syntax::InstFail && (arg || shouldVisit(pc, pos))) jobs.push_back(job{pc, arg, pos});
    }
};

}  // namespace detail

struct Regexp::impl {
    ptr<syntax::Prog> prog;                       // compiled program
    std::unique_ptr<detail::onePassProg> onepass;  // onepass program or nil
    int maxBitStateLen = 0;
    std::string prefix;      // required prefix in unanchored matches
    rune prefixRune = 0;     // first rune in prefix
    uint32 prefixEnd = 0;    // pc for last rune in prefix
    int matchcap = 0;        // size of recorded match lengths
    EmptyOp cond = 0;        // empty-width conditions required at start of match
    int minInputLen = 0;     // minimum length of the input in bytes

    // Pools of machines, as Go's sync.Pools.
    sync::Mutex mu;
    std::vector<std::unique_ptr<detail::machine>> machines;
    std::vector<std::unique_ptr<detail::bitState>> bitStates;

    std::unique_ptr<detail::machine> getMachine() {
        {
            std::lock_guard l(mu);
            if (!machines.empty()) {
                auto m = std::move(machines.back());
                machines.pop_back();
                return m;
            }
        }
        auto m = std::make_unique<detail::machine>();
        m->p = prog.get();
        std::size_t n = prog->Inst.size();
        m->q0.sparse.resize(n);
        m->q0.dense.resize(n);
        m->q1.sparse.resize(n);
        m->q1.dense.resize(n);
        m->matchcap.resize(std::size_t(matchcap));
        return m;
    }
    void putMachine(std::unique_ptr<detail::machine> m) {
        std::lock_guard l(mu);
        if (machines.size() < 16) machines.push_back(std::move(m));
    }
    std::unique_ptr<detail::bitState> getBitState() {
        std::lock_guard l(mu);
        if (!bitStates.empty()) {
            auto b = std::move(bitStates.back());
            bitStates.pop_back();
            return b;
        }
        return std::make_unique<detail::bitState>();
    }
    void putBitState(std::unique_ptr<detail::bitState> b) {
        std::lock_guard l(mu);
        if (bitStates.size() < 16) bitStates.push_back(std::move(b));
    }
};

Regexp::~Regexp() = default;

namespace {

// minInputLen walks the regexp to find the minimum length of any matchable
// input.
int minInputLen(const syntax::Regexp& re) {
    switch (re.Op) {
    default:
        return 0;
    case syntax::OpAnyChar:
    case syntax::OpAnyCharNotNL:
    case syntax::OpCharClass:
        return 1;
    case syntax::OpLiteral: {
        int l = 0;
        for (rune r : re.Rune) l += r == utf8::RuneError ? 1 : utf8::RuneLen(r);
        return l;
    }
    case syntax::OpCapture:
    case syntax::OpPlus:
        return minInputLen(*re.Sub[0]);
    case syntax::OpRepeat:
        return re.Min * minInputLen(*re.Sub[0]);
    case syntax::OpConcat: {
        int l = 0;
        for (const auto& sub : re.Sub) l += minInputLen(*sub);
        return l;
    }
    case syntax::OpAlternate: {
        int l = minInputLen(*re.Sub[0]);
        for (std::size_t k = 1; k < re.Sub.size(); k++) l = std::min(l, minInputLen(*re.Sub[k]));
        return l;
    }
    }
}

// maxBitStateLen returns the maximum length of a string to search with
// the backtracker using prog.
int maxBitStateLen(const syntax::Prog& prog) {
    if (prog.Inst.size() > maxBacktrackProg) return 0;
    return int(maxBacktrackVector / prog.Inst.size());
}

}  // namespace

std::pair<ptr<Regexp>, error> compile(std::string_view expr, syntax::Flags mode, bool longest) {
    auto [re, err] = syntax::Parse(expr, mode);
    if (err) return {nullptr, err};
    int maxCap = re->MaxCap();
    auto capNames = re->CapNames();

    re = re->Simplify();
    auto [prog, cerr] = syntax::Compile(*re);
    if (cerr) return {nullptr, cerr};
    auto impl = std::make_shared<Regexp::impl>();
    impl->prog = prog;
    impl->matchcap = std::max(prog->NumCap, 2);
    impl->onepass = compileOnePass(*prog);
    impl->cond = prog->StartCond();
    impl->minInputLen = minInputLen(*re);

    auto r = ptr<Regexp>(new Regexp());
    r->expr_ = string(expr);
    r->numSubexp_ = maxCap;
    r->subexpNames_ = std::move(capNames);
    r->longest_ = longest;
    if (!impl->onepass) {
        std::tie(r->prefix_, r->prefixComplete_) = prog->Prefix();
        impl->maxBitStateLen = maxBitStateLen(*prog);
    } else {
        std::tie(r->prefix_, r->prefixComplete_, impl->prefixEnd) = onePassPrefix(*prog);
    }
    if (!r->prefix_.empty()) {
        impl->prefix = r->prefix_.str();
        impl->prefixRune = utf8::DecodeRuneInString(impl->prefix).first;
    }
    r->impl_ = std::move(impl);
    return {r, nullptr};
}

std::pair<ptr<Regexp>, error> Compile(std::string_view expr) { return compile(expr, syntax::Perl, false); }

std::pair<ptr<Regexp>, error> CompilePOSIX(std::string_view expr) { return compile(expr, syntax::POSIX, true); }

namespace {

// quote: Go's quote of MustCompile's message, with backquotes when it can.
std::string quote(std::string_view s) {
    bool canBackquote = true;
    for (std::string_view t = s; !t.empty();) {
        auto [r, size] = utf8::DecodeRuneInString(t);
        t.remove_prefix(std::size_t(size));
        if ((r == utf8::RuneError && size == 1) || r == '`' || r == 0xFEFF || (r < ' ' && r != '\t') || r == 0x7f) {
            canBackquote = false;
            break;
        }
    }
    if (canBackquote) return "`" + std::string(s) + "`";
    return strconv::Quote(s).str();
}

}  // namespace

ptr<Regexp> MustCompile(std::string_view expr) {
    auto [re, err] = Compile(expr);
    if (err) panic("regexp: Compile(" + quote(expr) + "): " + err.Error().str());
    return re;
}

ptr<Regexp> MustCompilePOSIX(std::string_view expr) {
    auto [re, err] = CompilePOSIX(expr);
    if (err) panic("regexp: CompilePOSIX(" + quote(expr) + "): " + err.Error().str());
    return re;
}

int Regexp::SubexpIndex(std::string_view name) const {
    if (!name.empty()) {
        for (std::size_t i = 0; i < subexpNames_.size(); i++) {
            if (subexpNames_[i] == name) return int(i);
        }
    }
    return -1;
}

// ---------------------------------------------------------------------------
// Running the machines

namespace {

// doOnePass implements r.doExecute using the one-pass execution engine.
std::optional<std::vector<int>> doOnePass(const Regexp::impl& re, input& i, int pos, int ncap) {
    EmptyOp startCond = re.cond;
    if (startCond == EmptyOp(~EmptyOp(0))) return std::nullopt;  // impossible

    std::vector<int> matchcap(std::size_t(ncap), -1);
    bool matched = false;
    rune r = endOfText, r1 = endOfText;
    int width = 0, width1 = 0;
    std::tie(r, width) = i.step(pos);
    if (r != endOfText) std::tie(r1, width1) = i.step(pos + width);
    lazyFlag flag;
    if (pos == 0) {
        flag = newLazyFlag(-1, r);
    } else {
        flag = i.context(pos);
    }
    const auto& prog = *re.onepass;
    std::size_t pc = std::size_t(prog.Start);
    const onePassInst* inst = &prog.Inst[pc];
    // If there is a simple literal prefix, skip over it.
    if (pos == 0 && flagMatch(flag, EmptyOp(inst->Arg)) && !re.prefix.empty() && i.canCheckPrefix()) {
        // Match requires literal prefix; fast search for it.
        if (!i.hasPrefix(re.prefix)) goto Return;
        pos += int(re.prefix.size());
        std::tie(r, width) = i.step(pos);
        std::tie(r1, width1) = i.step(pos + width);
        flag = i.context(pos);
        pc = re.prefixEnd;
    }
    for (;;) {
        inst = &prog.Inst[pc];
        pc = inst->Out;
        switch (inst->Op) {
        default:
            panic("bad inst");
        case syntax::InstMatch:
            matched = true;
            if (!matchcap.empty()) {
                matchcap[0] = 0;
                matchcap[1] = pos;
            }
            goto Return;
        case syntax::InstRune:
            if (!inst->MatchRune(r)) goto Return;
            break;
        case syntax::InstRune1:
            // Special case: single rune.
            if (r != inst->Rune[0]) goto Return;
            break;
        case syntax::InstRuneAny:
            // Nothing
            break;
        case syntax::InstRuneAnyNotNL:
            if (r == '\n') goto Return;
            break;
        // peek at the input rune to see which branch of the Alt to take
        case syntax::InstAlt:
        case syntax::InstAltMatch:
            pc = onePassNext(*inst, r);
            continue;
        case syntax::InstFail:
            goto Return;
        case syntax::InstNop:
            continue;
        case syntax::InstEmptyWidth:
            if (!flagMatch(flag, EmptyOp(inst->Arg))) goto Return;
            continue;
        case syntax::InstCapture:
            if (inst->Arg < matchcap.size()) matchcap[inst->Arg] = pos;
            continue;
        }
        if (width == 0) break;
        flag = newLazyFlag(r, r1);
        pos += width;
        r = r1;
        width = width1;
        if (r != endOfText) std::tie(r1, width1) = i.step(pos + width);
    }

Return:
    if (!matched) return std::nullopt;
    return matchcap;
}

// tryBacktrack runs a backtracking search starting at pos.
bool tryBacktrack(const Regexp::impl& re, bool longest, detail::bitState& b, input& i, uint32 pc0, int pos0) {
    const syntax::Prog& prog = *re.prog;
    b.push(prog, pc0, pos0, false);
    while (!b.jobs.empty()) {
        job jb = b.jobs.back();
        b.jobs.pop_back();
        uint32 pc = jb.pc;
        int pos = jb.pos;
        bool arg = jb.arg;

        // Optimization: rather than push and pop, code that is going to
        // Push and continue the loop simply updates ip, p, and arg and
        // jumps to CheckAndLoop. We have to do the ShouldVisit check that
        // Push would have, but we avoid the stack manipulation.
        goto Skip;
    CheckAndLoop:
        if (!b.shouldVisit(pc, pos)) continue;
    Skip:
        {
            const Inst& inst = prog.Inst[pc];
            switch (inst.Op) {
            default:
                panic("bad inst");
            case syntax::InstFail:
                panic("unexpected InstFail");
            case syntax::InstAlt:
                // Cannot just
                //   b.push(inst.Out, pos, false)
                //   b.push(inst.Arg, pos, false)
                // If during the processing of inst.Out, we encounter
                // inst.Arg via another path, we want to process it then.
                // Pushing it here will inhibit that. Instead, re-push
                // inst with arg==true as a reminder to push inst.Arg out
                // later.
                if (arg) {
                    // Finished inst.Out; try inst.Arg.
                    arg = false;
                    pc = inst.Arg;
                    goto CheckAndLoop;
                }
                b.push(prog, pc, pos, true);
                pc = inst.Out;
                goto CheckAndLoop;
            case syntax::InstAltMatch:
                // One opcode consumes runes; the other leads to match.
                switch (prog.Inst[inst.Out].Op) {
                case syntax::InstRune:
                case syntax::InstRune1:
                case syntax::InstRuneAny:
                case syntax::InstRuneAnyNotNL:
                    // inst.Arg is the match.
                    b.push(prog, inst.Arg, pos, false);
                    pc = inst.Arg;
                    pos = b.end;
                    goto CheckAndLoop;
                default:
                    break;
                }
                // inst.Out is the match - non-greedy
                b.push(prog, inst.Out, b.end, false);
                pc = inst.Out;
                goto CheckAndLoop;
            case syntax::InstRune: {
                auto [r, width] = i.step(pos);
                if (!inst.MatchRune(r)) continue;
                pos += width;
                pc = inst.Out;
                goto CheckAndLoop;
            }
            case syntax::InstRune1: {
                auto [r, width] = i.step(pos);
                if (r != inst.Rune[0]) continue;
                pos += width;
                pc = inst.Out;
                goto CheckAndLoop;
            }
            case syntax::InstRuneAnyNotNL: {
                auto [r, width] = i.step(pos);
                if (r == '\n' || r == endOfText) continue;
                pos += width;
                pc = inst.Out;
                goto CheckAndLoop;
            }
            case syntax::InstRuneAny: {
                auto [r, width] = i.step(pos);
                if (r == endOfText) continue;
                pos += width;
                pc = inst.Out;
                goto CheckAndLoop;
            }
            case syntax::InstCapture:
                if (arg) {
                    // Finished inst.Out; restore the old value.
                    b.cap[inst.Arg] = pos;
                    continue;
                }
                if (inst.Arg < b.cap.size()) {
                    // Capture pos to register, but save old value.
                    b.push(prog, pc, b.cap[inst.Arg], true);  // come back when we're done.
                    b.cap[inst.Arg] = pos;
                }
                pc = inst.Out;
                goto CheckAndLoop;
            case syntax::InstEmptyWidth: {
                lazyFlag flag = i.context(pos);
                if (!flagMatch(flag, EmptyOp(inst.Arg))) continue;
                pc = inst.Out;
                goto CheckAndLoop;
            }
            case syntax::InstNop:
                pc = inst.Out;
                goto CheckAndLoop;
            case syntax::InstMatch: {
                // We found a match. If the caller doesn't care where the
                // match is, no point going further.
                if (b.cap.empty()) return true;
                // Record best match so far. Only need to check end point,
                // because this entire call is only considering one start
                // position.
                if (b.cap.size() > 1) b.cap[1] = pos;
                if (int old = b.matchcap[1]; old == -1 || (longest && pos > 0 && pos > old)) {
                    std::copy(b.cap.begin(), b.cap.end(), b.matchcap.begin());
                }
                // If going for first match, we're done.
                if (!longest) return true;
                // If we used the entire text, no longer match is possible.
                if (pos == b.end) return true;
                // Otherwise, continue on in hope of a longer match.
                continue;
            }
            }
        }
    }
    return longest && b.matchcap.size() > 1 && b.matchcap[1] >= 0;
}

// backtrack runs a backtracking search of prog on the input starting at
// pos.
std::optional<std::vector<int>> backtrack(Regexp::impl& re, bool longest, std::string_view s, int pos, int ncap) {
    EmptyOp startCond = re.cond;
    if (startCond == EmptyOp(~EmptyOp(0))) return std::nullopt;  // impossible
    if ((startCond & syntax::EmptyBeginText) && pos != 0) {
        // Anchored match, past beginning of text.
        return std::nullopt;
    }

    auto b = re.getBitState();
    inputString i(s);
    int end = int(s.size());
    b->reset(*re.prog, end, ncap);

    std::optional<std::vector<int>> result;
    // Anchored search must start at the beginning of the input
    if (startCond & syntax::EmptyBeginText) {
        if (!b->cap.empty()) b->cap[0] = pos;
        if (tryBacktrack(re, longest, *b, i, uint32(re.prog->Start), pos)) result = b->matchcap;
    } else {
        // Unanchored search, starting from each possible text position.
        // Notice that we have to try the empty string at the end of the
        // text, so the loop condition is pos <= end, not pos < end. This
        // looks like it's quadratic in the length of the text, but we are
        // avoiding revisiting (pc, pos) pairs, so the running time is
        // linear in the length of the input.
        int width = -1;
        for (; pos <= end && width != 0; pos += width) {
            if (!re.prefix.empty()) {
                // Match requires literal prefix; fast search for it.
                int advance = i.index(re.prefix, pos);
                if (advance < 0) break;
                pos += advance;
            }
            if (!b->cap.empty()) b->cap[0] = pos;
            if (tryBacktrack(re, longest, *b, i, uint32(re.prog->Start), pos)) {
                result = b->matchcap;
                break;
            }
            width = i.step(pos).second;
        }
    }
    re.putBitState(std::move(b));
    return result;
}

}  // namespace

namespace {

// find: the matches of re in r or s at pos, as Go's doExecute: nullopt for
// no match.
std::optional<std::vector<int>> findOpt(Regexp::impl& re, bool longest, io::RuneReader* r, std::string_view s, int pos,
                                        int ncap) {
    if (!r && int(s.size()) < re.minInputLen) return std::nullopt;
    if (re.onepass) {
        if (r) {
            inputReader in(r);
            return doOnePass(re, in, pos, ncap);
        }
        inputString in(s);
        return doOnePass(re, in, pos, ncap);
    }
    if (!r && int(s.size()) < re.maxBitStateLen) return backtrack(re, longest, s, pos, ncap);

    auto m = re.getMachine();
    m->longest = longest;
    m->cond = re.cond;
    m->prefix = re.prefix;
    m->prefixRune = re.prefixRune;
    m->init(ncap);
    bool ok;
    if (r) {
        inputReader in(r);
        ok = m->match(in, pos);
    } else {
        inputString in(s);
        ok = m->match(in, pos);
    }
    std::optional<std::vector<int>> result;
    if (ok) result = m->matchcap;
    re.putMachine(std::move(m));
    return result;
}

std::string_view view(const slice<byte>& b) { return asStringView(b.span()); }

}  // namespace

std::vector<int> Regexp::pad(std::vector<int> a) const {
    std::size_t n = std::size_t(1 + numSubexp_) * 2;
    if (a.size() < n) a.resize(n, -1);
    return a;
}

bool Regexp::Match(std::span<const byte> b) const { return findOpt(*impl_, longest_, nullptr, asStringView(b), 0, 0).has_value(); }
bool Regexp::MatchString(std::string_view s) const { return findOpt(*impl_, longest_, nullptr, s, 0, 0).has_value(); }
bool Regexp::MatchReader(io::RuneReader& r) const { return findOpt(*impl_, longest_, &r, {}, 0, 0).has_value(); }

std::pair<bool, error> Match(std::string_view pattern, std::span<const byte> b) {
    auto [re, err] = Compile(pattern);
    if (err) return {false, err};
    return {re->Match(b), nullptr};
}

std::pair<bool, error> MatchString(std::string_view pattern, std::string_view s) {
    auto [re, err] = Compile(pattern);
    if (err) return {false, err};
    return {re->MatchString(s), nullptr};
}

std::pair<bool, error> MatchReader(std::string_view pattern, io::RuneReader& r) {
    auto [re, err] = Compile(pattern);
    if (err) return {false, err};
    return {re->MatchReader(r), nullptr};
}

slice<byte> Regexp::Find(const slice<byte>& b) const {
    auto a = findOpt(*impl_, longest_, nullptr, view(b), 0, 2);
    if (!a) return {};
    return b.Sub(std::size_t((*a)[0]), std::size_t((*a)[1]), std::size_t((*a)[1]));
}

string Regexp::FindString(const string& s) const {
    auto a = findOpt(*impl_, longest_, nullptr, s, 0, 2);
    if (!a) return {};
    return s.slice(std::size_t((*a)[0]), std::size_t((*a)[1]));
}

std::vector<int> Regexp::FindIndex(std::span<const byte> b) const {
    auto a = findOpt(*impl_, longest_, nullptr, asStringView(b), 0, 2);
    if (!a) return {};
    a->resize(2);
    return *a;
}

std::vector<int> Regexp::FindStringIndex(std::string_view s) const {
    auto a = findOpt(*impl_, longest_, nullptr, s, 0, 2);
    if (!a) return {};
    a->resize(2);
    return *a;
}

std::vector<int> Regexp::FindReaderIndex(io::RuneReader& r) const {
    auto a = findOpt(*impl_, longest_, &r, {}, 0, 2);
    if (!a) return {};
    a->resize(2);
    return *a;
}

std::vector<slice<byte>> Regexp::FindSubmatch(const slice<byte>& b) const {
    auto m = findOpt(*impl_, longest_, nullptr, view(b), 0, impl_->prog->NumCap);
    if (!m) return {};
    std::vector<slice<byte>> sub(std::size_t(1 + numSubexp_));
    for (std::size_t i = 0; i < sub.size(); i++) {
        if (2 * i < m->size() && (*m)[2 * i] >= 0) {
            sub[i] = b.Sub(std::size_t((*m)[2 * i]), std::size_t((*m)[2 * i + 1]), std::size_t((*m)[2 * i + 1]));
        }
    }
    return sub;
}

std::vector<string> Regexp::FindStringSubmatch(const string& s) const {
    auto a = findOpt(*impl_, longest_, nullptr, s, 0, impl_->prog->NumCap);
    if (!a) return {};
    std::vector<string> ret(std::size_t(1 + numSubexp_));
    for (std::size_t i = 0; i < ret.size(); i++) {
        if (2 * i < a->size() && (*a)[2 * i] >= 0) ret[i] = s.slice(std::size_t((*a)[2 * i]), std::size_t((*a)[2 * i + 1]));
    }
    return ret;
}

std::vector<int> Regexp::FindSubmatchIndex(std::span<const byte> b) const {
    auto a = findOpt(*impl_, longest_, nullptr, asStringView(b), 0, impl_->prog->NumCap);
    if (!a) return {};
    return pad(std::move(*a));
}

std::vector<int> Regexp::FindStringSubmatchIndex(std::string_view s) const {
    auto a = findOpt(*impl_, longest_, nullptr, s, 0, impl_->prog->NumCap);
    if (!a) return {};
    return pad(std::move(*a));
}

std::vector<int> Regexp::FindReaderSubmatchIndex(io::RuneReader& r) const {
    auto a = findOpt(*impl_, longest_, &r, {}, 0, impl_->prog->NumCap);
    if (!a) return {};
    return pad(std::move(*a));
}

// matches calls yield with the successive matches of s, at most max of
// them (all if max < 0), with ncap capture positions each, until yield
// returns false.
template <typename F>
void Regexp::matches(std::string_view s, int max, int ncap, F&& yield) const {
    if (max == 0) return;
    int end = int(s.size());
    for (int pos = 0, prevMatchEnd = -1; pos <= end;) {
        auto m = findOpt(*impl_, longest_, nullptr, s, pos, ncap);
        if (!m || m->empty()) break;
        bool accept = true;
        if ((*m)[1] == pos) {
            // We've found an empty match.
            if ((*m)[0] == prevMatchEnd) {
                // We don't allow an empty match right after a previous
                // match, so ignore it.
                accept = false;
            }
            // Move to next matching position.
            int width = inputString(s).step(pos).second;
            if (width > 0) {
                pos += width;
            } else {
                pos = end + 1;
            }
        } else {
            pos = (*m)[1];
        }
        prevMatchEnd = (*m)[1];
        if (accept) {
            if (!yield(pad(std::move(*m)))) return;
            if (max > 0 && --max == 0) return;
        }
    }
}

std::vector<slice<byte>> Regexp::FindAll(const slice<byte>& b, int n) const {
    std::vector<slice<byte>> out;
    matches(view(b), n, 2, [&](const std::vector<int>& m) {
        out.push_back(b.Sub(std::size_t(m[0]), std::size_t(m[1]), std::size_t(m[1])));
        return true;
    });
    return out;
}

std::vector<string> Regexp::FindAllString(const string& s, int n) const {
    std::vector<string> out;
    matches(s, n, 2, [&](const std::vector<int>& m) {
        out.push_back(s.slice(std::size_t(m[0]), std::size_t(m[1])));
        return true;
    });
    return out;
}

std::vector<std::vector<int>> Regexp::FindAllIndex(std::span<const byte> b, int n) const {
    std::vector<std::vector<int>> out;
    matches(asStringView(b), n, 2, [&](const std::vector<int>& m) {
        out.push_back({m[0], m[1]});
        return true;
    });
    return out;
}

std::vector<std::vector<int>> Regexp::FindAllStringIndex(std::string_view s, int n) const {
    std::vector<std::vector<int>> out;
    matches(s, n, 2, [&](const std::vector<int>& m) {
        out.push_back({m[0], m[1]});
        return true;
    });
    return out;
}

std::vector<std::vector<slice<byte>>> Regexp::FindAllSubmatch(const slice<byte>& b, int n) const {
    std::vector<std::vector<slice<byte>>> out;
    matches(view(b), n, impl_->prog->NumCap, [&](const std::vector<int>& m) {
        std::vector<slice<byte>> sub(m.size() / 2);
        for (std::size_t i = 0; i < sub.size(); i++) {
            if (m[2 * i] >= 0) sub[i] = b.Sub(std::size_t(m[2 * i]), std::size_t(m[2 * i + 1]), std::size_t(m[2 * i + 1]));
        }
        out.push_back(std::move(sub));
        return true;
    });
    return out;
}

std::vector<std::vector<string>> Regexp::FindAllStringSubmatch(const string& s, int n) const {
    std::vector<std::vector<string>> out;
    matches(s, n, impl_->prog->NumCap, [&](const std::vector<int>& m) {
        std::vector<string> sub(m.size() / 2);
        for (std::size_t i = 0; i < sub.size(); i++) {
            if (m[2 * i] >= 0) sub[i] = s.slice(std::size_t(m[2 * i]), std::size_t(m[2 * i + 1]));
        }
        out.push_back(std::move(sub));
        return true;
    });
    return out;
}

std::vector<std::vector<int>> Regexp::FindAllSubmatchIndex(std::span<const byte> b, int n) const {
    std::vector<std::vector<int>> out;
    matches(asStringView(b), n, impl_->prog->NumCap, [&](const std::vector<int>& m) {
        out.push_back(m);
        return true;
    });
    return out;
}

std::vector<std::vector<int>> Regexp::FindAllStringSubmatchIndex(std::string_view s, int n) const {
    std::vector<std::vector<int>> out;
    matches(s, n, impl_->prog->NumCap, [&](const std::vector<int>& m) {
        out.push_back(m);
        return true;
    });
    return out;
}

// ---------------------------------------------------------------------------
// Replacing and expanding

std::string Regexp::replaceAll(std::string_view src, int nmatch,
                               const std::function<void(std::string&, const std::vector<int>&)>& repl) const {
    int lastMatchEnd = 0;  // end position of the most recent match
    int searchPos = 0;     // position where we next look for a match
    std::string buf;
    int endPos = int(src.size());
    if (nmatch > impl_->prog->NumCap) nmatch = impl_->prog->NumCap;

    while (searchPos <= endPos) {
        auto a = findOpt(*impl_, longest_, nullptr, src, searchPos, nmatch);
        if (!a || a->empty()) break;  // no more matches

        // Copy the unmatched characters before this match.
        buf.append(src.substr(std::size_t(lastMatchEnd), std::size_t((*a)[0] - lastMatchEnd)));

        // Now insert a copy of the replacement string, but not for a match
        // of the empty string immediately after another match. (Otherwise,
        // we get double replacement for patterns that match both empty and
        // nonempty strings.)
        if ((*a)[1] > lastMatchEnd || (*a)[0] == 0) repl(buf, *a);
        lastMatchEnd = (*a)[1];

        // Advance past this match; always advance at least one character.
        int width = inputString(src).step(searchPos).second;
        if (searchPos + width > (*a)[1]) {
            searchPos += width;
        } else if (searchPos + 1 > (*a)[1]) {
            // This clause is only needed at the end of the input string.
            // In that case, DecodeRuneInString returns width=0.
            searchPos++;
        } else {
            searchPos = (*a)[1];
        }
    }

    // Copy the unmatched characters after the last match.
    buf.append(src.substr(std::size_t(lastMatchEnd)));
    return buf;
}

string Regexp::ReplaceAllString(const string& src, std::string_view repl) const {
    int n = 2;
    if (repl.find('$') != std::string_view::npos) n = 2 * (numSubexp_ + 1);
    return string(replaceAll(src, n, [&](std::string& dst, const std::vector<int>& match) { expand(dst, repl, src, match); }));
}

string Regexp::ReplaceAllLiteralString(const string& src, std::string_view repl) const {
    return string(replaceAll(src, 2, [&](std::string& dst, const std::vector<int>&) { dst.append(repl); }));
}

string Regexp::ReplaceAllStringFunc(const string& src, const std::function<string(const string&)>& repl) const {
    return string(replaceAll(src, 2, [&](std::string& dst, const std::vector<int>& match) {
        dst.append(repl(src.slice(std::size_t(match[0]), std::size_t(match[1]))).view());
    }));
}

slice<byte> Regexp::ReplaceAll(const slice<byte>& src, std::span<const byte> repl) const {
    std::string_view r = asStringView(repl);
    int n = 2;
    if (r.find('$') != std::string_view::npos) n = 2 * (numSubexp_ + 1);
    return toBytes(replaceAll(view(src), n, [&](std::string& dst, const std::vector<int>& match) { expand(dst, r, view(src), match); }));
}

slice<byte> Regexp::ReplaceAllLiteral(const slice<byte>& src, std::span<const byte> repl) const {
    return toBytes(replaceAll(view(src), 2, [&](std::string& dst, const std::vector<int>&) { dst.append(asStringView(repl)); }));
}

slice<byte> Regexp::ReplaceAllFunc(const slice<byte>& src, const std::function<slice<byte>(const slice<byte>&)>& repl) const {
    return toBytes(replaceAll(view(src), 2, [&](std::string& dst, const std::vector<int>& match) {
        dst.append(view(repl(src.Sub(std::size_t(match[0]), std::size_t(match[1]), std::size_t(match[1])))));
    }));
}

namespace {

// extract returns the name from a leading "name" or "{name}" in str. (The
// $ has already been removed by the caller.) If it is a number, extract
// returns num set to that number; otherwise num = -1.
bool extract(std::string_view str, std::string_view& name, int& num, std::string_view& rest) {
    if (str.empty()) return false;
    bool brace = false;
    if (str[0] == '{') {
        brace = true;
        str.remove_prefix(1);
    }
    std::size_t i = 0;
    while (i < str.size()) {
        auto [r, size] = utf8::DecodeRuneInString(str.substr(i));
        if (!unicode::IsLetter(r) && !unicode::IsDigit(r) && r != '_') break;
        i += std::size_t(size);
    }
    if (i == 0) {
        // empty name is not okay
        return false;
    }
    name = str.substr(0, i);
    if (brace) {
        if (i >= str.size() || str[i] != '}') {
            // missing closing brace
            return false;
        }
        i++;
    }

    // Parse number.
    num = 0;
    for (char c : name) {
        if (c < '0' || '9' < c || num >= 100000000) {
            num = -1;
            break;
        }
        num = num * 10 + int(c) - '0';
    }
    // Disallow leading zeros.
    if (name[0] == '0' && name.size() > 1) num = -1;

    rest = str.substr(i);
    return true;
}

}  // namespace

void Regexp::expand(std::string& dst, std::string_view tmpl, std::string_view src, const std::vector<int>& match) const {
    while (!tmpl.empty()) {
        auto d = tmpl.find('$');
        if (d == std::string_view::npos) break;
        dst.append(tmpl.substr(0, d));
        tmpl = tmpl.substr(d + 1);
        if (!tmpl.empty() && tmpl[0] == '$') {
            // Treat $$ as $.
            dst += '$';
            tmpl.remove_prefix(1);
            continue;
        }
        std::string_view name, rest;
        int num = 0;
        if (!extract(tmpl, name, num, rest)) {
            // Malformed; treat $ as raw text.
            dst += '$';
            continue;
        }
        tmpl = rest;
        auto group = [&](std::size_t i) {
            dst.append(src.substr(std::size_t(match[2 * i]), std::size_t(match[2 * i + 1] - match[2 * i])));
        };
        if (num >= 0) {
            if (std::size_t(2 * num + 1) < match.size() && match[std::size_t(2 * num)] >= 0) group(std::size_t(num));
        } else {
            for (std::size_t i = 0; i < subexpNames_.size(); i++) {
                if (name == subexpNames_[i] && 2 * i + 1 < match.size() && match[2 * i] >= 0) {
                    group(i);
                    break;
                }
            }
        }
    }
    dst.append(tmpl);
}

slice<byte> Regexp::Expand(slice<byte> dst, std::span<const byte> tmpl, std::span<const byte> src,
                           const std::vector<int>& match) const {
    std::string out;
    expand(out, asStringView(tmpl), asStringView(src), match);
    return appendSpan(std::move(dst), as_bytes(out));
}

slice<byte> Regexp::ExpandString(slice<byte> dst, std::string_view tmpl, std::string_view src,
                                 const std::vector<int>& match) const {
    std::string out;
    expand(out, tmpl, src, match);
    return appendSpan(std::move(dst), as_bytes(out));
}

std::vector<string> Regexp::Split(const string& s, int n) const {
    if (n == 0) return {};
    if (!expr_.empty() && s.empty()) return {string()};
    auto ms = FindAllStringIndex(s, n);
    std::vector<string> strs;
    strs.reserve(ms.size());
    int beg = 0, end = 0;
    for (const auto& match : ms) {
        if (n > 0 && int(strs.size()) >= n - 1) break;
        end = match[0];
        if (match[1] != 0) strs.push_back(s.slice(std::size_t(beg), std::size_t(end)));
        beg = match[1];
    }
    if (end != int(s.size())) strs.push_back(s.slice(std::size_t(beg)));
    return strs;
}

std::pair<slice<byte>, error> Regexp::MarshalText() const { return {toBytes(expr_), nullptr}; }

string QuoteMeta(const string& s) {
    constexpr std::string_view special = "\\.+*?()|[]{}^$";
    std::size_t i = 0;
    for (; i < s.size(); i++) {
        if (special.find(s[i]) != std::string_view::npos) break;
    }
    // No meta characters found, so return original string.
    if (i >= s.size()) return s;
    std::string b(s.view().substr(0, i));
    for (; i < s.size(); i++) {
        if (special.find(s[i]) != std::string_view::npos) b += '\\';
        b += s[i];
    }
    return string(b);
}

}  // namespace gopp::regexp
