// After Go's text/template/parse/lex.go.
#include "lex.hpp"

#include "../goconv.hpp"

#include <algorithm>
#include <cstdio>
#include <map>

namespace gopp::text::tmpl::parse {

namespace {

constexpr int32 eof = -1;
constexpr std::string_view space_chars = " \t\r\n";
constexpr char trim_marker = '-';
constexpr Pos trim_marker_len = 2;  // marker plus space before or after
constexpr std::string_view left_comment = "/*";
constexpr std::string_view right_comment = "*/";

const std::map<std::string_view, itemType>& keys() {
    static const std::map<std::string_view, itemType> k = {
        {".", itemDot},       {"block", itemBlock}, {"break", itemBreak}, {"continue", itemContinue},
        {"define", itemDefine}, {"else", itemElse},   {"end", itemEnd},     {"if", itemIf},
        {"range", itemRange}, {"nil", itemNil},     {"template", itemTemplate}, {"with", itemWith},
    };
    return k;
}

bool is_space(int32 r) { return r == ' ' || r == '\t' || r == '\r' || r == '\n'; }

bool is_alpha_numeric(int32 r) { return r == '_' || detail::is_letter(r) || detail::is_digit(r); }

bool has_left_trim_marker(std::string_view s) { return s.size() >= 2 && s[0] == trim_marker && is_space(s[1]); }

bool has_right_trim_marker(std::string_view s) { return s.size() >= 2 && is_space(s[0]) && s[1] == trim_marker; }

Pos right_trim_length(std::string_view s) {
    std::size_t n = s.size();
    while (n > 0 && space_chars.find(s[n - 1]) != std::string_view::npos) n--;
    return static_cast<Pos>(s.size() - n);
}

Pos left_trim_length(std::string_view s) {
    std::size_t n = 0;
    while (n < s.size() && space_chars.find(s[n]) != std::string_view::npos) n++;
    return static_cast<Pos>(n);
}

int count_newlines(std::string_view s) { return static_cast<int>(std::count(s.begin(), s.end(), '\n')); }

bool contains_rune(std::string_view valid, int32 r) {
    if (r < 0) return false;
    std::string enc;
    detail::append_rune(enc, r);
    return valid.find(enc) != std::string_view::npos;
}

// fmt's %#U: U+0078 'x'
std::string sharp_u(int32 r) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "U+%04X", static_cast<unsigned>(r));
    std::string s = buf;
    if (detail::is_print(r)) {
        s += " '";
        detail::append_rune(s, r);
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
        for (int n = 0; n < 10 && p < i.val.size(); n++) p += static_cast<std::size_t>(detail::decode_rune(i.val, p).second);
        return detail::quote(i.val.view().substr(0, p)) + "...";
    }
    return detail::quote(i.val);
}

lexer::lexer(string name, string input, std::string_view left, std::string_view right)
    : name_(std::move(name)),
      input_(std::move(input)),
      left_delim_(left.empty() ? default_left_delim : left),
      right_delim_(right.empty() ? default_right_delim : right) {}

int32 lexer::next() {
    if (static_cast<std::size_t>(pos_) >= input_.size()) {
        at_eof_ = true;
        return eof;
    }
    auto [r, w] = detail::decode_rune(input_, static_cast<std::size_t>(pos_));
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
    if (!at_eof_ && pos_ > 0) {
        auto [r, w] = detail::decode_last_rune(input_, static_cast<std::size_t>(pos_));
        pos_ -= w;
        if (r == '\n') line_--;
    }
}

item lexer::this_item(itemType t) {
    item i{t, start_, input_.slice(static_cast<std::size_t>(start_), static_cast<std::size_t>(pos_)), start_line_};
    start_ = pos_;
    start_line_ = line_;
    return i;
}

bool lexer::emit(itemType t) { return emit_item(this_item(t)); }

bool lexer::emit_item(item i) {
    item_ = std::move(i);
    return false;
}

void lexer::ignore() {
    line_ += count_newlines(input_.view().substr(static_cast<std::size_t>(start_), static_cast<std::size_t>(pos_ - start_)));
    start_ = pos_;
    start_line_ = line_;
}

bool lexer::accept(std::string_view valid) {
    if (contains_rune(valid, next())) return true;
    backup();
    return false;
}

void lexer::accept_run(std::string_view valid) {
    while (contains_rune(valid, next())) {
    }
    backup();
}

bool lexer::errorf(std::string msg) {
    item_ = item{itemError, start_, string(msg), start_line_};
    start_ = 0;
    pos_ = 0;
    input_ = {};
    return false;
}

item lexer::next_item() {
    item_ = item{itemEOF, pos_, string::from_static("EOF"), start_line_};
    state_fn state = inside_action_ ? &lexer::lex_inside_action : &lexer::lex_text;
    for (;;) {
        state_ = nullptr;
        if (!(this->*state)()) return item_;
        state = state_;
    }
}

bool lexer::lex_text() {
    std::string_view in = input_;
    auto x = in.find(left_delim_, static_cast<std::size_t>(pos_));
    if (x != std::string_view::npos) {
        x -= static_cast<std::size_t>(pos_);
        if (x > 0) {
            pos_ += static_cast<Pos>(x);
            // Do we trim any trailing space?
            Pos trim_length = 0;
            Pos delim_end = pos_ + static_cast<Pos>(left_delim_.size());
            if (has_left_trim_marker(in.substr(static_cast<std::size_t>(delim_end)))) {
                trim_length = right_trim_length(in.substr(static_cast<std::size_t>(start_), static_cast<std::size_t>(pos_ - start_)));
            }
            pos_ -= trim_length;
            line_ += count_newlines(in.substr(static_cast<std::size_t>(start_), static_cast<std::size_t>(pos_ - start_)));
            item i = this_item(itemText);
            pos_ += trim_length;
            ignore();
            if (!i.val.empty()) return emit_item(std::move(i));
        }
        state_ = &lexer::lex_left_delim;
        return true;
    }
    pos_ = static_cast<Pos>(in.size());
    // Correctly reached EOF.
    if (pos_ > start_) {
        line_ += count_newlines(in.substr(static_cast<std::size_t>(start_), static_cast<std::size_t>(pos_ - start_)));
        return emit(itemText);
    }
    return emit(itemEOF);
}

std::pair<bool, bool> lexer::at_right_delim() {
    std::string_view rest = input_.view().substr(static_cast<std::size_t>(pos_));
    if (has_right_trim_marker(rest) && rest.substr(static_cast<std::size_t>(trim_marker_len)).starts_with(right_delim_)) {
        return {true, true};
    }
    if (rest.starts_with(right_delim_)) return {true, false};
    return {false, false};
}

bool lexer::lex_left_delim() {
    pos_ += static_cast<Pos>(left_delim_.size());
    std::string_view rest = input_.view().substr(static_cast<std::size_t>(pos_));
    bool trim_space = has_left_trim_marker(rest);
    Pos after_marker = trim_space ? trim_marker_len : 0;
    if (rest.substr(static_cast<std::size_t>(after_marker)).starts_with(left_comment)) {
        pos_ += after_marker;
        ignore();
        state_ = &lexer::lex_comment;
        return true;
    }
    item i = this_item(itemLeftDelim);
    inside_action_ = true;
    pos_ += after_marker;
    ignore();
    paren_depth_ = 0;
    return emit_item(std::move(i));
}

bool lexer::lex_comment() {
    pos_ += static_cast<Pos>(left_comment.size());
    auto x = input_.view().find(right_comment, static_cast<std::size_t>(pos_));
    if (x == std::string_view::npos) return errorf("unclosed comment");
    pos_ = static_cast<Pos>(x + right_comment.size());
    auto [delim, trim_space] = at_right_delim();
    if (!delim) return errorf("comment ends before closing delimiter");
    line_ += count_newlines(input_.view().substr(static_cast<std::size_t>(start_), static_cast<std::size_t>(pos_ - start_)));
    item i = this_item(itemComment);
    if (trim_space) pos_ += trim_marker_len;
    pos_ += static_cast<Pos>(right_delim_.size());
    if (trim_space) pos_ += left_trim_length(input_.view().substr(static_cast<std::size_t>(pos_)));
    ignore();
    if (options.emit_comment) return emit_item(std::move(i));
    state_ = &lexer::lex_text;
    return true;
}

bool lexer::lex_right_delim() {
    auto [delim, trim_space] = at_right_delim();
    if (trim_space) {
        pos_ += trim_marker_len;
        ignore();
    }
    pos_ += static_cast<Pos>(right_delim_.size());
    item i = this_item(itemRightDelim);
    if (trim_space) {
        pos_ += left_trim_length(input_.view().substr(static_cast<std::size_t>(pos_)));
        ignore();
    }
    inside_action_ = false;
    return emit_item(std::move(i));
}

bool lexer::lex_inside_action() {
    // Either number, quoted string, or identifier. Spaces separate
    // arguments; runs of spaces turn into itemSpace. Pipe symbols separate
    // and are emitted.
    auto [delim, trim] = at_right_delim();
    if (delim) {
        if (paren_depth_ == 0) {
            state_ = &lexer::lex_right_delim;
            return true;
        }
        return errorf("unclosed left paren");
    }
    int32 r = next();
    if (r == eof) return errorf("unclosed action");
    if (is_space(r)) {
        backup();  // put space back in case we have " -}}"
        state_ = &lexer::lex_space;
        return true;
    }
    switch (r) {
    case '=': return emit(itemAssign);
    case ':':
        if (next() != '=') return errorf("expected :=");
        return emit(itemDeclare);
    case '|': return emit(itemPipe);
    case '"': state_ = &lexer::lex_quote; return true;
    case '`': state_ = &lexer::lex_raw_quote; return true;
    case '$': state_ = &lexer::lex_variable; return true;
    case '\'': state_ = &lexer::lex_char; return true;
    case '.':
        // Special look-ahead for ".field" so we don't break l.backup().
        if (static_cast<std::size_t>(pos_) < input_.size()) {
            char c = input_[static_cast<std::size_t>(pos_)];
            if (c < '0' || '9' < c) {
                state_ = &lexer::lex_field;
                return true;
            }
        }
        [[fallthrough]];  // '.' can start a number
    case '+': case '-': case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7': case '8': case '9':
        backup();
        state_ = &lexer::lex_number;
        return true;
    case '(':
        paren_depth_++;
        return emit(itemLeftParen);
    case ')':
        paren_depth_--;
        if (paren_depth_ < 0) return errorf("unexpected right paren");
        return emit(itemRightParen);
    }
    if (is_alpha_numeric(r)) {
        backup();
        state_ = &lexer::lex_identifier;
        return true;
    }
    if (r <= 0x7F && detail::is_print(r)) return emit(itemChar);
    return errorf("unrecognized character in action: " + sharp_u(r));
}

bool lexer::lex_space() {
    int num_spaces = 0;
    for (;;) {
        int32 r = peek();
        if (!is_space(r)) break;
        next();
        num_spaces++;
    }
    // Be careful about a trim-marked closing delimiter, which has a minus
    // after a space: we know there is a space, so check for the '-' that
    // might follow.
    std::string_view rest = input_.view().substr(static_cast<std::size_t>(pos_ - 1));
    if (has_right_trim_marker(rest) && rest.substr(static_cast<std::size_t>(trim_marker_len)).starts_with(right_delim_)) {
        backup();  // before the space
        if (num_spaces == 1) {
            state_ = &lexer::lex_right_delim;  // on the delim, so go right to that
            return true;
        }
    }
    return emit(itemSpace);
}

bool lexer::lex_identifier() {
    for (;;) {
        int32 r = next();
        if (is_alpha_numeric(r)) continue;
        backup();
        std::string_view word = input_.view().substr(static_cast<std::size_t>(start_), static_cast<std::size_t>(pos_ - start_));
        if (!at_terminator()) return errorf("bad character " + sharp_u(r));
        auto it = keys().find(word);
        if (it != keys().end() && it->second > itemKeyword) {
            itemType k = it->second;
            if ((k == itemBreak && !options.break_ok) || (k == itemContinue && !options.continue_ok)) return emit(itemIdentifier);
            return emit(k);
        }
        if (word[0] == '.') return emit(itemField);
        if (word == "true" || word == "false") return emit(itemBool);
        return emit(itemIdentifier);
    }
}

bool lexer::lex_field() { return lex_field_or_variable(itemField); }

bool lexer::lex_variable() {
    if (at_terminator()) return emit(itemVariable);  // nothing interesting follows -> "$"
    return lex_field_or_variable(itemVariable);
}

bool lexer::lex_field_or_variable(itemType typ) {
    if (at_terminator()) {  // nothing interesting follows -> "." or "$"
        if (typ == itemVariable) return emit(itemVariable);
        return emit(itemDot);
    }
    int32 r;
    for (;;) {
        r = next();
        if (!is_alpha_numeric(r)) {
            backup();
            break;
        }
    }
    if (!at_terminator()) return errorf("bad character " + sharp_u(r));
    return emit(typ);
}

bool lexer::at_terminator() {
    int32 r = peek();
    if (is_space(r)) return true;
    switch (r) {
    case eof: case '.': case ',': case '|': case ':': case ')': case '(':
        return true;
    }
    return input_.view().substr(static_cast<std::size_t>(pos_)).starts_with(right_delim_);
}

bool lexer::lex_char() {
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

bool lexer::lex_number() {
    if (!scan_number()) {
        return errorf("bad number syntax: " +
                      detail::quote(input_.view().substr(static_cast<std::size_t>(start_), static_cast<std::size_t>(pos_ - start_))));
    }
    if (int32 sign = peek(); sign == '+' || sign == '-') {
        // Complex: 1+2i. No spaces, must end in 'i'.
        if (!scan_number() || input_[static_cast<std::size_t>(pos_ - 1)] != 'i') {
            return errorf("bad number syntax: " +
                          detail::quote(input_.view().substr(static_cast<std::size_t>(start_), static_cast<std::size_t>(pos_ - start_))));
        }
        return emit(itemComplex);
    }
    return emit(itemNumber);
}

bool lexer::scan_number() {
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
    accept_run(digits);
    if (accept(".")) accept_run(digits);
    if (digits.size() == 10 + 1 && accept("eE")) {
        accept("+-");
        accept_run("0123456789_");
    }
    if (digits.size() == 16 + 6 + 1 && accept("pP")) {
        accept("+-");
        accept_run("0123456789_");
    }
    // Is it imaginary?
    accept("i");
    // Next thing mustn't be alphanumeric.
    if (is_alpha_numeric(peek())) {
        next();
        return false;
    }
    return true;
}

bool lexer::lex_quote() {
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

bool lexer::lex_raw_quote() {
    for (;;) {
        int32 r = next();
        if (r == eof) return errorf("unterminated raw quoted string");
        if (r == '`') break;
    }
    return emit(itemRawString);
}

}  // namespace gopp::text::tmpl::parse
