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
extern int maxStackDepth;
extern bool textFormatQ;

// tree.newNumber, for the tests: the number node for text, or its error.
std::pair<ptr<NumberNode>, gopp::error> newNumberForTest(std::string_view text, int typ);

inline constexpr std::string_view defaultLeftDelim = "{{";
inline constexpr std::string_view defaultRightDelim = "}}";

struct lexOptions {
    bool emitComment = false;  // emit itemComment tokens
    bool breakOk = false;      // break keyword allowed
    bool continueOk = false;   // continue keyword allowed
};

class lexer {
public:
    lexer(string name, string input, std::string_view left, std::string_view right);

    // The next item from the input.
    item nextItem();

    lexOptions options;

private:
    using stateFn = bool (lexer::*)();  // false: an item is ready

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
    bool lexInsideAction();
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
    std::string leftDelim_;
    std::string rightDelim_;
    Pos pos_ = 0;
    Pos start_ = 0;
    bool atEof_ = false;
    int parenDepth_ = 0;
    int line_ = 1;
    int startLine_ = 1;
    item item_;
    bool insideAction_ = false;
    stateFn state_ = nullptr;
};

}  // namespace gopp::text::tmpl::parse
