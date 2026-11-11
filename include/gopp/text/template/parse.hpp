// The text/template/parse package: parse trees for text/template, ported
// from Go 1.27.3. Templates use it through text/template; it is public, as
// in Go, for tools that inspect or build trees.
//
//     auto [trees, err] = parse::Parse("t", "{{.Name}}", "", "", {&funcs});
//
// `template` is a C++ keyword, so the packages are gopp::text::template_
// and gopp::text::template_::parse.
#pragma once

#include <gopp/errors.hpp>
#include <gopp/ptr.hpp>
#include <gopp/string.hpp>

#include <complex>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace gopp::text::template_::parse {

// parse.Pos: a byte position in the original input.
using Pos = int;

// parse.NodeType
enum NodeType : int {
    NodeText,        // plain text
    NodeAction,      // a non-control action such as a field evaluation
    NodeBool,        // a boolean constant
    NodeChain,       // a sequence of field accesses
    NodeCommand,     // an element of a pipeline
    NodeDot,         // the cursor, dot
    nodeElse,        // an else action; not added to tree
    nodeEnd,         // an end action; not added to tree
    NodeField,       // a field or method name
    NodeIdentifier,  // an identifier; always a function name
    NodeIf,          // an if action
    NodeList,        // a list of Nodes
    NodeNil,         // an untyped nil constant
    NodeNumber,      // a numerical constant
    NodePipe,        // a pipeline of commands
    NodeRange,       // a range action
    NodeString,      // a string constant
    NodeTemplate,    // a template invocation action
    NodeVariable,    // a $ variable
    NodeWith,        // a with action
    NodeComment,     // a comment
    NodeBreak,       // a break action
    NodeContinue,    // a continue action
};

class Tree;

// parse.Node: an element of the parse tree.
class Node : public object {
public:
    parse::NodeType NodeType = NodeText;
    parse::Pos Pos = 0;

    parse::NodeType Type() const noexcept { return NodeType; }
    parse::Pos Position() const noexcept { return Pos; }
    // The node's text, as it would appear in a template.
    virtual string String() const;
    // A deep copy.
    virtual ptr<Node> Copy() const = 0;

    // Internals: the tree the node belongs to, and String's builder.
    Tree* tree() const noexcept { return tr; }
    virtual void write_to(std::string& sb) const = 0;
    Tree* tr = nullptr;
};

class VariableNode;
class CommandNode;

// parse.ListNode
class ListNode final : public Node {
public:
    std::vector<ptr<Node>> Nodes;  // the element nodes in lexical order
    ptr<ListNode> CopyList() const;
    ptr<Node> Copy() const override;
    void write_to(std::string& sb) const override;
};

// parse.TextNode
class TextNode final : public Node {
public:
    string Text;  // may span newlines
    string String() const override;
    ptr<Node> Copy() const override;
    void write_to(std::string& sb) const override;
};

// parse.CommentNode
class CommentNode final : public Node {
public:
    string Text;
    ptr<Node> Copy() const override;
    void write_to(std::string& sb) const override;
};

// parse.PipeNode: a pipeline with optional declaration.
class PipeNode final : public Node {
public:
    int Line = 0;                           // deprecated in Go; kept
    bool IsAssign = false;                  // the variables are being assigned, not declared
    std::vector<ptr<VariableNode>> Decl;    // variables in lexical order
    std::vector<ptr<CommandNode>> Cmds;     // the commands in lexical order
    ptr<PipeNode> CopyPipe() const;
    ptr<Node> Copy() const override;
    void write_to(std::string& sb) const override;
};

// parse.ActionNode: a non-control action such as a field evaluation.
class ActionNode final : public Node {
public:
    int Line = 0;
    ptr<PipeNode> Pipe;
    ptr<Node> Copy() const override;
    void write_to(std::string& sb) const override;
};

// parse.CommandNode: a command (a pipeline element).
class CommandNode final : public Node {
public:
    std::vector<ptr<Node>> Args;  // identifier, field, or constant
    ptr<Node> Copy() const override;
    void write_to(std::string& sb) const override;
};

// parse.IdentifierNode: a function name.
class IdentifierNode final : public Node {
public:
    string Ident;
    IdentifierNode* SetPos(parse::Pos pos) noexcept {
        Pos = pos;
        return this;
    }
    IdentifierNode* SetTree(Tree* t) noexcept {
        tr = t;
        return this;
    }
    ptr<Node> Copy() const override;
    void write_to(std::string& sb) const override;
};

// parse.NewIdentifier
ptr<IdentifierNode> NewIdentifier(string ident);

// parse.VariableNode: a $ variable with optional fields: $x.Field1.Field2.
class VariableNode final : public Node {
public:
    std::vector<string> Ident;  // variable name and fields in lexical order
    ptr<Node> Copy() const override;
    void write_to(std::string& sb) const override;
};

// parse.DotNode
class DotNode final : public Node {
public:
    ptr<Node> Copy() const override;
    void write_to(std::string& sb) const override;
};

// parse.NilNode
class NilNode final : public Node {
public:
    ptr<Node> Copy() const override;
    void write_to(std::string& sb) const override;
};

// parse.FieldNode: .Field1.Field2
class FieldNode final : public Node {
public:
    std::vector<string> Ident;  // the identifiers in lexical order
    ptr<Node> Copy() const override;
    void write_to(std::string& sb) const override;
};

// parse.ChainNode: a term followed by fields: (pipeline).Field1.Field2.
class ChainNode final : public Node {
public:
    ptr<parse::Node> Node;
    std::vector<string> Field;  // the identifiers in lexical order
    // Adds a field, with its leading dot.
    void Add(std::string_view field);
    ptr<parse::Node> Copy() const override;
    void write_to(std::string& sb) const override;
};

// parse.BoolNode
class BoolNode final : public Node {
public:
    bool True = false;
    ptr<Node> Copy() const override;
    void write_to(std::string& sb) const override;
};

// parse.NumberNode: a number in all the forms it can take.
class NumberNode final : public Node {
public:
    bool IsInt = false;
    bool IsUint = false;
    bool IsFloat = false;
    bool IsComplex = false;
    int64 Int64 = 0;
    uint64 Uint64 = 0;
    double Float64 = 0;
    std::complex<double> Complex128;
    string Text;  // the original textual representation
    ptr<Node> Copy() const override;
    void write_to(std::string& sb) const override;
    void simplify_complex();
};

// parse.StringNode
class StringNode final : public Node {
public:
    string Quoted;  // the original text, with quotes
    string Text;    // the string, after quote processing
    ptr<Node> Copy() const override;
    void write_to(std::string& sb) const override;
};

// An {{end}} or {{else}}; not added to trees.
class EndNode final : public Node {
public:
    int Line = 0;
    ptr<Node> Copy() const override;
    void write_to(std::string& sb) const override;
};

// parse.BranchNode: the common part of if, range and with.
class BranchNode : public Node {
public:
    int Line = 0;
    ptr<PipeNode> Pipe;          // the pipeline to be evaluated
    ptr<ListNode> List;          // what to execute if the value is non-empty
    ptr<ListNode> ElseList;      // what to execute if empty (nil if absent)
    ptr<Node> Copy() const override;
    void write_to(std::string& sb) const override;
};

class IfNode final : public BranchNode {};
class RangeNode final : public BranchNode {};
class WithNode final : public BranchNode {};

// parse.BreakNode
class BreakNode final : public Node {
public:
    int Line = 0;
    ptr<Node> Copy() const override;
    void write_to(std::string& sb) const override;
};

// parse.ContinueNode
class ContinueNode final : public Node {
public:
    int Line = 0;
    ptr<Node> Copy() const override;
    void write_to(std::string& sb) const override;
};

// parse.TemplateNode: {{template "name" pipeline}}
class TemplateNode final : public Node {
public:
    int Line = 0;
    string Name;         // the name of the template (unquoted)
    ptr<PipeNode> Pipe;  // the command to evaluate as dot (nil if none)
    ptr<Node> Copy() const override;
    void write_to(std::string& sb) const override;
};

// parse.Mode
using Mode = unsigned;
inline constexpr Mode ParseComments = 1;  // parse comments and add them to the tree
inline constexpr Mode SkipFuncCheck = 2;  // don't check that functions are defined

// The names of the functions a template may call (Go passes the FuncMaps).
using FuncNames = std::set<std::string, std::less<>>;

class lexer;

// A token from the lexer (lex.go's item); internal, but the parser keeps
// three for lookahead.
struct item {
    int typ = 0;  // itemType (src/text/template/parse/lex.hpp)
    parse::Pos pos = 0;
    string val;
    int line = 0;
};

// parse.Tree: the representation of a single parsed template.
class Tree final : public object {
public:
    string Name;       // the name of the template represented by the tree
    string ParseName;  // the top-level template's name during parsing, for errors
    ptr<ListNode> Root;
    parse::Mode Mode = 0;

    explicit Tree(string name) : Name(std::move(name)) {}
    ~Tree() override;

    // A copy of the tree; nodes are copied, parsing state is not.
    ptr<Tree> Copy() const;
    // Parses text into the tree, adding it and the templates it defines
    // to tree_set.
    std::pair<Tree*, gopp::error> Parse(std::string_view text, std::string_view left_delim, std::string_view right_delim,
                                        std::map<std::string, ptr<Tree>, std::less<>>& tree_set,
                                        std::vector<const FuncNames*> funcs = {});
    // "name:line:col" and the node's text, for error messages.
    std::pair<string, string> ErrorContext(const Node& n) const;

    // Internals.
    string text;
    string left_delim, right_delim;

private:
    friend struct tree_access;
    [[noreturn]] void errorf(const std::string& msg);
    [[noreturn]] void unexpected(const item& token, std::string_view context);
    item next();
    void backup();
    void backup2(const item& t1);
    void backup3(const item& t2, const item& t1);
    item peek();
    item next_non_space();
    item peek_non_space();
    item expect(int expected, std::string_view context);
    item expect_one_of(int expected1, int expected2, std::string_view context);
    void start_parse(std::vector<const FuncNames*> funcs, std::shared_ptr<lexer> lex,
                     std::map<std::string, ptr<Tree>, std::less<>>* tree_set);
    void stop_parse();
    void add();
    void parse();
    void parse_definition();
    std::pair<ptr<ListNode>, ptr<Node>> item_list();
    ptr<Node> text_or_action();
    ptr<Node> action();
    ptr<Node> break_control(parse::Pos pos, int line);
    ptr<Node> continue_control(parse::Pos pos, int line);
    ptr<PipeNode> pipeline(std::string_view context, int end);
    void check_pipeline(const PipeNode& pipe, std::string_view context);
    ptr<BranchNode> parse_control(std::string_view context, ptr<BranchNode> node);
    ptr<Node> if_control();
    ptr<Node> range_control();
    ptr<Node> with_control();
    ptr<Node> end_control();
    ptr<Node> else_control();
    ptr<Node> block_control();
    ptr<Node> template_control();
    string parse_template_name(const item& token, std::string_view context);
    ptr<CommandNode> command();
    ptr<Node> operand();
    ptr<Node> term();
    bool has_function(std::string_view name) const;
    ptr<Node> use_var(parse::Pos pos, std::string_view name);
    ptr<Node> new_number(parse::Pos pos, std::string_view text, int typ);
    template <typename N>
    ptr<N> make(parse::NodeType type, parse::Pos pos);

    std::vector<const FuncNames*> funcs_;
    std::shared_ptr<lexer> lex_;
    item token_[3];  // three-token lookahead
    int peek_count_ = 0;
    std::vector<string> vars_;
    std::map<std::string, ptr<Tree>, std::less<>>* tree_set_ = nullptr;
    int action_line_ = 0;
    int range_depth_ = 0;
    int stack_depth_ = 0;  // nested parenthesized pipelines
    int nest_depth_ = 0;   // nested control structures
};

// parse.New
ptr<Tree> New(string name);

// parse.Parse: the trees of the template named name and the templates it
// defines, by name.
std::pair<std::map<std::string, ptr<Tree>, std::less<>>, gopp::error> Parse(std::string_view name, std::string_view text,
                                                                             std::string_view left_delim,
                                                                             std::string_view right_delim,
                                                                             std::vector<const FuncNames*> funcs = {});

// parse.IsEmptyTree: whether n contains only whitespace or comments.
bool IsEmptyTree(const Node* n);

// Limits: Go allows parenthesized pipelines 10000 deep and doesn't limit
// nested control structures. Templates may be parsed on 256 KiB fiber
// stacks, so go++ allows 200 of each ("max expression depth exceeded",
// "max template nesting depth exceeded").

}  // namespace gopp::text::template_::parse
