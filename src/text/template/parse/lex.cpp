// After Go's text/template/parse/lex.go.
#include "lex.hpp"

#include "../goconv.hpp"

#include <algorithm>
#include <cstdio>
#include <map>

namespace gopp::text::tmpl::parse {

namespace {

constexpr int32 eof = -1;
constexpr std::string_view spaceChars = " \t\r\n";
constexpr char trimMarker = '-';
constexpr Pos trimMarkerLen = 2;  // marker plus space before or after
constexpr std::string_view leftComment = "/*";
constexpr std::string_view rightComment = "*/";

const std::map<std::string_view, itemType>& keys() {
    static const std::map<std::string_view, itemType> k = {
        {".", itemDot},       {"block", itemBlock}, {"break", itemBreak}, {"continue", itemContinue},
        {"define", itemDefine}, {"else", itemElse},   {"end", itemEnd},     {"if", itemIf},
        {"range", itemRange}, {"nil", itemNil},     {"template", itemTemplate}, {"with", itemWith},
    };
    return k;
}

bool isSpace(int32 r) { return r == ' ' || r == '\t' || r == '\r' || r == '\n'; }

bool isAlphaNumeric(int32 r) { return r == '_' || detail::isLetter(r) || detail::isDigit(r); }

bool hasLeftTrimMarker(std::string_view s) { return s.size() >= 2 && s[0] == trimMarker && isSpace(s[1]); }

bool hasRightTrimMarker(std::string_view s) { return s.size() >= 2 && isSpace(s[0]) && s[1] == trimMarker; }

Pos rightTrimLength(std::string_view s) {
    std::size_t n = s.size();
    while (n > 0 && spaceChars.find(s[n - 1]) != std::string_view::npos) n--;
    return static_cast<Pos>(s.size() - n);
}

Pos leftTrimLength(std::string_view s) {
    std::size_t n = 0;
    while (n < s.size() && spaceChars.find(s[n]) != std::string_view::npos) n++;
    return static_cast<Pos>(n);
}

int countNewlines(std::string_view s) { return static_cast<int>(std::count(s.begin(), s.end(), '\n')); }

bool containsRune(std::string_view valid, int32 r) {
    if (r < 0) return false;
    std::string enc;
    detail::appendRune(enc, r);
    return valid.find(enc) != std::string_view::npos;
}

// fmt's %#U: U+0078 'x'
std::string sharpU(int32 r) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "U+%04X", static_cast<unsigned>(r));
    std::string s = buf;
    if (detail::isPrint(r)) {
        s += " '";
        detail::appendRune(s, r);
        s += "'";
    }
    return s;
}

}  // namespace

std::string to_string(const item& i) {
    if (i.typ == itemEOF) return "EOF";
    if (i.typ == itemError) return i.val.str();
    if (i.typ > itemKeyword) return "<" + i.val.str() + ">";
    if (i.val.size() > 10) {
        // %.10q: the first 10 runes, quoted.
        std::size_t p = 0;
        for (int n = 0; n < 10 && p < i.val.size(); n++) p += static_cast<std::size_t>(detail::decodeRune(i.val, p).second);
        return detail::quote(i.val.view().substr(0, p)) + "...";
    }
    return detail::quote(i.val);
}

lexer::lexer(string name, string input, std::string_view left, std::string_view right)
    : name_(std::move(name)),
      input_(std::move(input)),
      leftDelim_(left.empty() ? defaultLeftDelim : left),
      rightDelim_(right.empty() ? defaultRightDelim : right) {}

int32 lexer::next() {
    if (static_cast<std::size_t>(pos_) >= input_.size()) {
        atEof_ = true;
        return eof;
    }
    auto [r, w] = detail::decodeRune(input_, static_cast<std::size_t>(pos_));
    pos_ += w;
    if (r == '\n') line_++;
    return r;
}

int32 lexer::peek() {
    int32 r = next();
    backup();
    return r;
}

void lexer::backup() {
    if (!atEof_ && pos_ > 0) {
        auto [r, w] = detail::decodeLastRune(input_, static_cast<std::size_t>(pos_));
        pos_ -= w;
        if (r == '\n') line_--;
    }
}

item lexer::thisItem(itemType t) {
    item i{t, start_, input_.slice(static_cast<std::size_t>(start_), static_cast<std::size_t>(pos_)), startLine_};
    start_ = pos_;
    startLine_ = line_;
    return i;
}

bool lexer::emit(itemType t) { return emitItem(thisItem(t)); }

bool lexer::emitItem(item i) {
    item_ = std::move(i);
    return false;
}

void lexer::ignore() {
    line_ += countNewlines(input_.view().substr(static_cast<std::size_t>(start_), static_cast<std::size_t>(pos_ - start_)));
    start_ = pos_;
    startLine_ = line_;
}

bool lexer::accept(std::string_view valid) {
    if (containsRune(valid, next())) return true;
    backup();
    return false;
}

void lexer::acceptRun(std::string_view valid) {
    while (containsRune(valid, next())) {
    }
    backup();
}

bool lexer::errorf(std::string msg) {
    item_ = item{itemError, start_, string(msg), startLine_};
    start_ = 0;
    pos_ = 0;
    input_ = {};
    return false;
}

item lexer::nextItem() {
    item_ = item{itemEOF, pos_, string::fromStatic("EOF"), startLine_};
    stateFn state = insideAction_ ? &lexer::lexInsideAction : &lexer::lexText;
    for (;;) {
        state_ = nullptr;
        if (!(this->*state)()) return item_;
        state = state_;
    }
}

bool lexer::lexText() {
    std::string_view in = input_;
    auto x = in.find(leftDelim_, static_cast<std::size_t>(pos_));
    if (x != std::string_view::npos) {
        x -= static_cast<std::size_t>(pos_);
        if (x > 0) {
            pos_ += static_cast<Pos>(x);
            // Do we trim any trailing space?
            Pos trimLength = 0;
            Pos delimEnd = pos_ + static_cast<Pos>(leftDelim_.size());
            if (hasLeftTrimMarker(in.substr(static_cast<std::size_t>(delimEnd)))) {
                trimLength = rightTrimLength(in.substr(static_cast<std::size_t>(start_), static_cast<std::size_t>(pos_ - start_)));
            }
            pos_ -= trimLength;
            line_ += countNewlines(in.substr(static_cast<std::size_t>(start_), static_cast<std::size_t>(pos_ - start_)));
            item i = thisItem(itemText);
            pos_ += trimLength;
            ignore();
            if (!i.val.empty()) return emitItem(std::move(i));
        }
        state_ = &lexer::lexLeftDelim;
        return true;
    }
    pos_ = static_cast<Pos>(in.size());
    // Correctly reached EOF.
    if (pos_ > start_) {
        line_ += countNewlines(in.substr(static_cast<std::size_t>(start_), static_cast<std::size_t>(pos_ - start_)));
        return emit(itemText);
    }
    return emit(itemEOF);
}

std::pair<bool, bool> lexer::atRightDelim() {
    std::string_view rest = input_.view().substr(static_cast<std::size_t>(pos_));
    if (hasRightTrimMarker(rest) && rest.substr(static_cast<std::size_t>(trimMarkerLen)).starts_with(rightDelim_)) {
        return {true, true};
    }
    if (rest.starts_with(rightDelim_)) return {true, false};
    return {false, false};
}

bool lexer::lexLeftDelim() {
    pos_ += static_cast<Pos>(leftDelim_.size());
    std::string_view rest = input_.view().substr(static_cast<std::size_t>(pos_));
    bool trimSpace = hasLeftTrimMarker(rest);
    Pos afterMarker = trimSpace ? trimMarkerLen : 0;
    if (rest.substr(static_cast<std::size_t>(afterMarker)).starts_with(leftComment)) {
        pos_ += afterMarker;
        ignore();
        state_ = &lexer::lexComment;
        return true;
    }
    item i = thisItem(itemLeftDelim);
    insideAction_ = true;
    pos_ += afterMarker;
    ignore();
    parenDepth_ = 0;
    return emitItem(std::move(i));
}

bool lexer::lexComment() {
    pos_ += static_cast<Pos>(leftComment.size());
    auto x = input_.view().find(rightComment, static_cast<std::size_t>(pos_));
    if (x == std::string_view::npos) return errorf("unclosed comment");
    pos_ = static_cast<Pos>(x + rightComment.size());
    auto [delim, trimSpace] = atRightDelim();
    if (!delim) return errorf("comment ends before closing delimiter");
    line_ += countNewlines(input_.view().substr(static_cast<std::size_t>(start_), static_cast<std::size_t>(pos_ - start_)));
    item i = thisItem(itemComment);
    if (trimSpace) pos_ += trimMarkerLen;
    pos_ += static_cast<Pos>(rightDelim_.size());
    if (trimSpace) pos_ += leftTrimLength(input_.view().substr(static_cast<std::size_t>(pos_)));
    ignore();
    if (options.emitComment) return emitItem(std::move(i));
    state_ = &lexer::lexText;
    return true;
}

bool lexer::lexRightDelim() {
    auto [delim, trimSpace] = atRightDelim();
    if (trimSpace) {
        pos_ += trimMarkerLen;
        ignore();
    }
    pos_ += static_cast<Pos>(rightDelim_.size());
    item i = thisItem(itemRightDelim);
    if (trimSpace) {
        pos_ += leftTrimLength(input_.view().substr(static_cast<std::size_t>(pos_)));
        ignore();
    }
    insideAction_ = false;
    return emitItem(std::move(i));
}

bool lexer::lexInsideAction() {
    // Either number, quoted string, or identifier. Spaces separate
    // arguments; runs of spaces turn into itemSpace. Pipe symbols separate
    // and are emitted.
    auto [delim, trim] = atRightDelim();
    if (delim) {
        if (parenDepth_ == 0) {
            state_ = &lexer::lexRightDelim;
            return true;
        }
        return errorf("unclosed left paren");
    }
    int32 r = next();
    if (r == eof) return errorf("unclosed action");
    if (isSpace(r)) {
        backup();  // put space back in case we have " -}}"
        state_ = &lexer::lexSpace;
        return true;
    }
    switch (r) {
    case '=': return emit(itemAssign);
    case ':':
        if (next() != '=') return errorf("expected :=");
        return emit(itemDeclare);
    case '|': return emit(itemPipe);
    case '"': state_ = &lexer::lexQuote; return true;
    case '`': state_ = &lexer::lexRawQuote; return true;
    case '$': state_ = &lexer::lexVariable; return true;
    case '\'': state_ = &lexer::lexChar; return true;
    case '.':
        // Special look-ahead for ".field" so we don't break l.backup().
        if (static_cast<std::size_t>(pos_) < input_.size()) {
            char c = input_[static_cast<std::size_t>(pos_)];
            if (c < '0' || '9' < c) {
                state_ = &lexer::lexField;
                return true;
            }
        }
        [[fallthrough]];  // '.' can start a number
    case '+': case '-': case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7': case '8': case '9':
        backup();
        state_ = &lexer::lexNumber;
        return true;
    case '(':
        parenDepth_++;
        return emit(itemLeftParen);
    case ')':
        parenDepth_--;
        if (parenDepth_ < 0) return errorf("unexpected right paren");
        return emit(itemRightParen);
    }
    if (isAlphaNumeric(r)) {
        backup();
        state_ = &lexer::lexIdentifier;
        return true;
    }
    if (r <= 0x7F && detail::isPrint(r)) return emit(itemChar);
    return errorf("unrecognized character in action: " + sharpU(r));
}

bool lexer::lexSpace() {
    int numSpaces = 0;
    for (;;) {
        int32 r = peek();
        if (!isSpace(r)) break;
        next();
        numSpaces++;
    }
    // Be careful about a trim-marked closing delimiter, which has a minus
    // after a space: we know there is a space, so check for the '-' that
    // might follow.
    std::string_view rest = input_.view().substr(static_cast<std::size_t>(pos_ - 1));
    if (hasRightTrimMarker(rest) && rest.substr(static_cast<std::size_t>(trimMarkerLen)).starts_with(rightDelim_)) {
        backup();  // before the space
        if (numSpaces == 1) {
            state_ = &lexer::lexRightDelim;  // on the delim, so go right to that
            return true;
        }
    }
    return emit(itemSpace);
}

bool lexer::lexIdentifier() {
    for (;;) {
        int32 r = next();
        if (isAlphaNumeric(r)) continue;
        backup();
        std::string_view word = input_.view().substr(static_cast<std::size_t>(start_), static_cast<std::size_t>(pos_ - start_));
        if (!atTerminator()) return errorf("bad character " + sharpU(r));
        auto it = keys().find(word);
        if (it != keys().end() && it->second > itemKeyword) {
            itemType k = it->second;
            if ((k == itemBreak && !options.breakOk) || (k == itemContinue && !options.continueOk)) return emit(itemIdentifier);
            return emit(k);
        }
        if (word[0] == '.') return emit(itemField);
        if (word == "true" || word == "false") return emit(itemBool);
        return emit(itemIdentifier);
    }
}

bool lexer::lexField() { return lexFieldOrVariable(itemField); }

bool lexer::lexVariable() {
    if (atTerminator()) return emit(itemVariable);  // nothing interesting follows -> "$"
    return lexFieldOrVariable(itemVariable);
}

bool lexer::lexFieldOrVariable(itemType typ) {
    if (atTerminator()) {  // nothing interesting follows -> "." or "$"
        if (typ == itemVariable) return emit(itemVariable);
        return emit(itemDot);
    }
    int32 r;
    for (;;) {
        r = next();
        if (!isAlphaNumeric(r)) {
            backup();
            break;
        }
    }
    if (!atTerminator()) return errorf("bad character " + sharpU(r));
    return emit(typ);
}

bool lexer::atTerminator() {
    int32 r = peek();
    if (isSpace(r)) return true;
    switch (r) {
    case eof: case '.': case ',': case '|': case ':': case ')': case '(':
        return true;
    }
    return input_.view().substr(static_cast<std::size_t>(pos_)).starts_with(rightDelim_);
}

bool lexer::lexChar() {
    for (;;) {
        int32 r = next();
        if (r == '\\') {
            int32 r2 = next();
            if (r2 != eof && r2 != '\n') continue;
            return errorf("unterminated character constant");
        }
        if (r == eof || r == '\n') return errorf("unterminated character constant");
        if (r == '\'') break;
    }
    return emit(itemCharConstant);
}

bool lexer::lexNumber() {
    if (!scanNumber()) {
        return errorf("bad number syntax: " +
                      detail::quote(input_.view().substr(static_cast<std::size_t>(start_), static_cast<std::size_t>(pos_ - start_))));
    }
    if (int32 sign = peek(); sign == '+' || sign == '-') {
        // Complex: 1+2i. No spaces, must end in 'i'.
        if (!scanNumber() || input_[static_cast<std::size_t>(pos_ - 1)] != 'i') {
            return errorf("bad number syntax: " +
                          detail::quote(input_.view().substr(static_cast<std::size_t>(start_), static_cast<std::size_t>(pos_ - start_))));
        }
        return emit(itemComplex);
    }
    return emit(itemNumber);
}

bool lexer::scanNumber() {
    // Optional leading sign.
    accept("+-");
    // Is it hex?
    std::string_view digits = "0123456789_";
    if (accept("0")) {
        // Note: Leading 0 does not mean octal in floats.
        if (accept("xX")) {
            digits = "0123456789abcdefABCDEF_";
        } else if (accept("oO")) {
            digits = "01234567_";
        } else if (accept("bB")) {
            digits = "01_";
        }
    }
    acceptRun(digits);
    if (accept(".")) acceptRun(digits);
    if (digits.size() == 10 + 1 && accept("eE")) {
        accept("+-");
        acceptRun("0123456789_");
    }
    if (digits.size() == 16 + 6 + 1 && accept("pP")) {
        accept("+-");
        acceptRun("0123456789_");
    }
    // Is it imaginary?
    accept("i");
    // Next thing mustn't be alphanumeric.
    if (isAlphaNumeric(peek())) {
        next();
        return false;
    }
    return true;
}

bool lexer::lexQuote() {
    for (;;) {
        int32 r = next();
        if (r == '\\') {
            int32 r2 = next();
            if (r2 != eof && r2 != '\n') continue;
            return errorf("unterminated quoted string");
        }
        if (r == eof || r == '\n') return errorf("unterminated quoted string");
        if (r == '"') break;
    }
    return emit(itemString);
}

bool lexer::lexRawQuote() {
    for (;;) {
        int32 r = next();
        if (r == eof) return errorf("unterminated raw quoted string");
        if (r == '`') break;
    }
    return emit(itemRawString);
}

}  // namespace gopp::text::tmpl::parse
