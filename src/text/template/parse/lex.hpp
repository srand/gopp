// The template lexer, after Go's text/template/parse/lex.go. Like modern
// Go's, it runs synchronously: next_item() runs the state machine until it
// produces an item.
#pragma once

#include <gopp/text/template/parse.hpp>

#include <string>
#include <string_view>

namespace gopp::text::tmpl::parse {

enum itemType : int {
    itemError,         // error occurred; value is text of error
    itemBool,          // boolean constant
    itemChar,          // printable ASCII character; grab bag for comma etc.
    itemCharConstant,  // character constant
    itemComment,       // comment text
    itemComplex,       // complex constant (1+2i); imaginary is just a number
    itemAssign,        // equals ('=') introducing an assignment
    itemDeclare,       // colon-equals (':=') introducing a declaration
    itemEOF,
    itemField,       // alphanumeric identifier starting with '.'
    itemIdentifier,  // alphanumeric identifier not starting with '.'
    itemLeftDelim,   // left action delimiter
    itemLeftParen,   // '(' inside action
    itemNumber,      // simple number, including imaginary
    itemPipe,        // pipe symbol
    itemRawString,   // raw quoted string (includes quotes)
    itemRightDelim,  // right action delimiter
    itemRightParen,  // ')' inside action
    itemSpace,       // run of spaces separating arguments
    itemString,      // quoted string (includes quotes)
    itemText,        // plain text
    itemVariable,    // variable starting with '$', such as '$' or '$1' or '$hello'
    // Keywords appear after all the rest.
    itemKeyword,   // used only to delimit the keywords
    itemBlock,     // block keyword
    itemBreak,     // break keyword
    itemContinue,  // continue keyword
    itemDot,       // the cursor, spelled '.'
    itemDefine,    // define keyword
    itemElse,      // else keyword
    itemEnd,       // end keyword
    itemIf,        // if keyword
    itemNil,       // the untyped nil constant, easiest to treat as a keyword
    itemRange,     // range keyword
    itemTemplate,  // template keyword
    itemWith,      // with keyword
};

// item.String(), as Go's: "EOF", the error text, "<keyword>", or a quoted
// value (truncated past 10 bytes).
std::string to_string(const item& i);

// Internal knobs, which Go's tests change: the depth of parenthesized
// pipelines (maxStackDepth), and whether TextNode.String quotes
// (textFormat "%q").
extern int max_stack_depth;
extern bool text_format_q;

// tree.newNumber, for the tests: the number node for text, or its error.
std::pair<ptr<NumberNode>, gopp::error> newNumberForTest(std::string_view text, int typ);

inline constexpr std::string_view default_left_delim = "{{";
inline constexpr std::string_view default_right_delim = "}}";

struct lex_options {
    bool emit_comment = false;  // emit itemComment tokens
    bool break_ok = false;      // break keyword allowed
    bool continue_ok = false;   // continue keyword allowed
};

class lexer {
public:
    lexer(string name, string input, std::string_view left, std::string_view right);

    // The next item from the input.
    item nextItem();

    lex_options options;

private:
    using state_fn = bool (lexer::*)();  // false: an item is ready

    int32 next();
    int32 peek();
    void backup();
    item thisItem(itemType t);
    bool emit(itemType t);
    bool emitItem(item i);
    void ignore();
    bool accept(std::string_view valid);
    void acceptRun(std::string_view valid);
    bool errorf(std::string msg);
    bool atTerminator();
    std::pair<bool, bool> atRightDelim();
    bool scanNumber();

    // States: return false once an item is set; state_ holds the next.
    bool lexText();
    bool lexLeftDelim();
    bool lexComment();
    bool lexRightDelim();
    bool lex_inside_action();
    bool lexSpace();
    bool lexIdentifier();
    bool lexField();
    bool lexVariable();
    bool lexFieldOrVariable(itemType typ);
    bool lexChar();
    bool lexNumber();
    bool lexQuote();
    bool lexRawQuote();

    string name_;
    string input_;
    std::string left_delim_;
    std::string right_delim_;
    Pos pos_ = 0;
    Pos start_ = 0;
    bool at_eof_ = false;
    int paren_depth_ = 0;
    int line_ = 1;
    int start_line_ = 1;
    item item_;
    bool inside_action_ = false;
    state_fn state_ = nullptr;
};

}  // namespace gopp::text::tmpl::parse
