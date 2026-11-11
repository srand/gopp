// After Go's text/template/parse/parse.go. Go's parser reports errors by
// panicking and recovering in Parse; here a private exception does that.
#include "lex.hpp"

#include "../goconv.hpp"

#include <gopp/strings.hpp>

#include <algorithm>
#include <format>

namespace gopp::text::template_::parse {

namespace {

struct parse_error {
    gopp::error err;
};

bool is_rune_int(std::string_view s) { return !s.empty() && s[0] == '\''; }

constexpr int max_nest_depth = 200;

}  // namespace

int max_stack_depth = 200;

struct tree_access {
    static ptr<Node> new_number(Tree& t, std::string_view text, int typ) { return t.new_number(0, text, typ); }
};

std::pair<ptr<NumberNode>, gopp::error> new_number_for_test(std::string_view text, int typ) {
    auto t = New(string::from_static("test"));
    try {
        return {dynamic_pointer_cast<NumberNode>(tree_access::new_number(*t, text, typ)), nullptr};
    } catch (parse_error& e) {
        return {nullptr, e.err};
    }
}

Tree::~Tree() = default;

template <typename N>
ptr<N> Tree::make(parse::NodeType type, parse::Pos pos) {
    auto n = make_ptr<N>();
    n->NodeType = type;
    n->Pos = pos;
    n->tr = this;
    return n;
}

ptr<Tree> New(string name) { return make_ptr<Tree>(std::move(name)); }

ptr<Tree> Tree::Copy() const {
    auto t = make_ptr<Tree>(Name);
    t->ParseName = ParseName;
    t->Mode = Mode;
    t->text = text;
    t->left_delim = left_delim;
    t->right_delim = right_delim;
    if (Root) t->Root = Root->CopyList();
    return t;
}

std::pair<std::map<std::string, ptr<Tree>, std::less<>>, gopp::error> Parse(std::string_view name, std::string_view text,
                                                                             std::string_view left_delim,
                                                                             std::string_view right_delim,
                                                                             std::vector<const FuncNames*> funcs) {
    std::map<std::string, ptr<Tree>, std::less<>> tree_set;
    auto t = New(string(name));
    t->text = string(text);
    auto [tree, err] = t->Parse(text, left_delim, right_delim, tree_set, std::move(funcs));
    return {std::move(tree_set), err};
}

// ---------------------------------------------------------------------------
// Tokens

item Tree::next() {
    if (peek_count_ > 0) {
        peek_count_--;
    } else {
        token_[0] = lex_->next_item();
    }
    return token_[peek_count_];
}

void Tree::backup() { peek_count_++; }

void Tree::backup2(const item& t1) {
    token_[1] = t1;
    peek_count_ = 2;
}

void Tree::backup3(const item& t2, const item& t1) {
    token_[1] = t1;
    token_[2] = t2;
    peek_count_ = 3;
}

item Tree::peek() {
    if (peek_count_ > 0) return token_[peek_count_ - 1];
    peek_count_ = 1;
    token_[0] = lex_->next_item();
    return token_[0];
}

item Tree::next_non_space() {
    for (;;) {
        item token = next();
        if (token.typ != itemSpace) return token;
    }
}

item Tree::peek_non_space() {
    item token = next_non_space();
    backup();
    return token;
}

// ---------------------------------------------------------------------------
// Errors

std::pair<string, string> Tree::ErrorContext(const Node& n) const {
    int pos = n.Position();
    const Tree* tree = n.tree();
    if (!tree) tree = this;
    std::string_view text = tree->text.view().substr(0, std::min<std::size_t>(static_cast<std::size_t>(pos), tree->text.size()));
    auto nl = text.rfind('\n');
    int byte_num;
    if (nl == std::string_view::npos) {
        byte_num = pos;  // on first line
    } else {
        byte_num = pos - static_cast<int>(nl + 1);
    }
    int line_num = 1 + static_cast<int>(std::count(text.begin(), text.end(), '\n'));
    return {string(std::format("{}:{}:{}", tree->ParseName.view(), line_num, byte_num)), n.String()};
}

void Tree::errorf(const std::string& msg) {
    Root = nullptr;
    throw parse_error{errors::New(string(std::format("template: {}:{}: {}", ParseName.view(), token_[0].line, msg)))};
}

item Tree::expect(int expected, std::string_view context) {
    item token = next_non_space();
    if (token.typ != expected) unexpected(token, context);
    return token;
}

item Tree::expect_one_of(int expected1, int expected2, std::string_view context) {
    item token = next_non_space();
    if (token.typ != expected1 && token.typ != expected2) unexpected(token, context);
    return token;
}

void Tree::unexpected(const item& token, std::string_view context) {
    if (token.typ == itemError) {
        std::string extra;
        if (action_line_ != 0 && action_line_ != token.line) {
            extra = std::format(" in action started at {}:{}", ParseName.view(), action_line_);
            if (token.val.view().ends_with(" action")) extra = extra.substr(std::string_view(" in action").size());
        }
        errorf(to_string(token) + extra);
    }
    errorf("unexpected " + to_string(token) + " in " + std::string(context));
}

// ---------------------------------------------------------------------------
// Parsing

void Tree::start_parse(std::vector<const FuncNames*> funcs, std::shared_ptr<lexer> lex,
                       std::map<std::string, ptr<Tree>, std::less<>>* tree_set) {
    Root = nullptr;
    lex_ = std::move(lex);
    vars_ = {string::from_static("$")};
    funcs_ = std::move(funcs);
    tree_set_ = tree_set;
    stack_depth_ = 0;
    lex_->options = lex_options{
        .emit_comment = (Mode & ParseComments) != 0,
        .break_ok = !has_function("break"),
        .continue_ok = !has_function("continue"),
    };
}

void Tree::stop_parse() {
    lex_ = nullptr;
    vars_.clear();
    funcs_.clear();
    tree_set_ = nullptr;
}

std::pair<Tree*, gopp::error> Tree::Parse(std::string_view text_in, std::string_view left, std::string_view right,
                                          std::map<std::string, ptr<Tree>, std::less<>>& tree_set,
                                          std::vector<const FuncNames*> funcs) {
    ptr<Tree> keep(this);  // add() may hand the tree to tree_set only
    ParseName = Name;
    left_delim = left.empty() ? string(default_left_delim) : string(left);
    right_delim = right.empty() ? string(default_right_delim) : string(right);
    text = string(text_in);
    auto lex = std::make_shared<lexer>(Name, text, left_delim, right_delim);
    try {
        start_parse(std::move(funcs), lex, &tree_set);
        parse();
        add();
        stop_parse();
    } catch (parse_error& e) {
        stop_parse();
        return {nullptr, e.err};
    }
    return {this, nullptr};
}

void Tree::add() {
    auto it = tree_set_->find(Name.view());
    if (it == tree_set_->end() || IsEmptyTree(it->second->Root.get())) {
        (*tree_set_)[Name.str()] = ptr<Tree>(this);
        return;
    }
    if (!IsEmptyTree(Root.get())) errorf("template: multiple definition of template " + detail::quote(Name));
}

bool IsEmptyTree(const Node* n) {
    if (!n) return true;
    switch (n->Type()) {
    case NodeAction: return false;
    case NodeComment: return true;
    case NodeIf: return false;
    case NodeList:
        for (auto& node : static_cast<const ListNode*>(n)->Nodes) {
            if (!IsEmptyTree(node.get())) return false;
        }
        return true;
    case NodeRange: return false;
    case NodeTemplate: return false;
    case NodeText: {
        std::string_view t = static_cast<const TextNode*>(n)->Text;
        return t.find_first_not_of(" \t\r\n\v\f") == std::string_view::npos;
    }
    case NodeWith: return false;
    default: panic("unknown node: " + std::string(n->String().view()));
    }
}

void Tree::parse() {
    Root = make<ListNode>(NodeList, peek().pos);
    while (peek().typ != itemEOF) {
        if (peek().typ == itemLeftDelim) {
            item delim = next();
            if (next_non_space().typ == itemDefine) {
                auto new_t = New(string::from_static("definition"));  // name will be updated once we know it
                new_t->text = text;
                new_t->Mode = Mode;
                new_t->left_delim = left_delim;
                new_t->right_delim = right_delim;
                new_t->ParseName = ParseName;
                new_t->start_parse(funcs_, lex_, tree_set_);
                try {
                    new_t->parse_definition();
                } catch (parse_error&) {
                    token_[0] = new_t->token_[0];
                    throw;
                }
                continue;
            }
            backup2(delim);
        }
        ptr<Node> n = text_or_action();
        if (n->Type() == nodeEnd || n->Type() == nodeElse) errorf("unexpected " + n->String().str());
        Root->Nodes.push_back(n);
    }
}

void Tree::parse_definition() {
    constexpr std::string_view context = "define clause";
    item name = expect_one_of(itemString, itemRawString, context);
    auto [s, err] = detail::unquote(name.val);
    if (err) errorf(err.Error().str());
    Name = string(s);
    expect(itemRightDelim, context);
    auto [list, end] = item_list();
    Root = list;
    if (end->Type() != nodeEnd) errorf("unexpected " + end->String().str() + " in " + std::string(context));
    add();
    stop_parse();
}

std::pair<ptr<ListNode>, ptr<Node>> Tree::item_list() {
    if (nest_depth_ >= max_nest_depth) errorf("max template nesting depth exceeded");
    nest_depth_++;
    struct guard {
        int& d;
        ~guard() { d--; }
    } g{nest_depth_};
    auto list = make<ListNode>(NodeList, peek_non_space().pos);
    while (peek_non_space().typ != itemEOF) {
        ptr<Node> n = text_or_action();
        if (n->Type() == nodeEnd || n->Type() == nodeElse) return {list, n};
        list->Nodes.push_back(n);
    }
    errorf("unexpected EOF");
}

ptr<Node> Tree::text_or_action() {
    item token = next_non_space();
    switch (token.typ) {
    case itemText: {
        auto n = make<TextNode>(NodeText, token.pos);
        n->Text = token.val;
        return n;
    }
    case itemLeftDelim: {
        action_line_ = token.line;
        struct clear {
            int& l;
            ~clear() { l = 0; }
        } c{action_line_};
        return action();
    }
    case itemComment: {
        auto n = make<CommentNode>(NodeComment, token.pos);
        n->Text = token.val;
        return n;
    }
    default:
        unexpected(token, "input");
    }
}

ptr<Node> Tree::action() {
    item token = next_non_space();
    switch (token.typ) {
    case itemBlock: return block_control();
    case itemBreak: return break_control(token.pos, token.line);
    case itemContinue: return continue_control(token.pos, token.line);
    case itemElse: return else_control();
    case itemEnd: return end_control();
    case itemIf: return if_control();
    case itemRange: return range_control();
    case itemTemplate: return template_control();
    case itemWith: return with_control();
    }
    backup();
    item t = peek();
    // Do not pop variables; they persist until "end".
    auto n = make<ActionNode>(NodeAction, t.pos);
    n->Line = t.line;
    n->Pipe = pipeline("command", itemRightDelim);
    return n;
}

ptr<Node> Tree::break_control(parse::Pos pos, int line) {
    if (item token = next_non_space(); token.typ != itemRightDelim) unexpected(token, "{{break}}");
    if (range_depth_ == 0) errorf("{{break}} outside {{range}}");
    auto n = make<BreakNode>(NodeBreak, pos);
    n->Line = line;
    return n;
}

ptr<Node> Tree::continue_control(parse::Pos pos, int line) {
    if (item token = next_non_space(); token.typ != itemRightDelim) unexpected(token, "{{continue}}");
    if (range_depth_ == 0) errorf("{{continue}} outside {{range}}");
    auto n = make<ContinueNode>(NodeContinue, pos);
    n->Line = line;
    return n;
}

ptr<PipeNode> Tree::pipeline(std::string_view context, int end) {
    item token = peek_non_space();
    auto pipe = make<PipeNode>(NodePipe, token.pos);
    pipe->Line = token.line;
    auto new_variable = [&](const item& v) {
        auto n = make<VariableNode>(NodeVariable, v.pos);
        for (auto& part : strings::Split(v.val, ".")) n->Ident.push_back(part);
        return n;
    };
    // Are there declarations or assignments?
    for (;;) {
        item v = peek_non_space();
        if (v.typ != itemVariable) break;
        next();
        // Since space is a token, we need 3-token look-ahead here in the
        // worst case: in "$x foo" we need to read "foo" (as opposed to
        // ":=") to know that $x is an argument variable rather than a
        // declaration. So remember the token adjacent to the variable so
        // we can push it back if necessary.
        item token_after_variable = peek();
        item nx = peek_non_space();
        if (nx.typ == itemAssign || nx.typ == itemDeclare) {
            pipe->IsAssign = nx.typ == itemAssign;
            next_non_space();
            pipe->Decl.push_back(new_variable(v));
            vars_.push_back(v.val);
        } else if (nx.typ == itemChar && nx.val == ",") {
            next_non_space();
            pipe->Decl.push_back(new_variable(v));
            vars_.push_back(v.val);
            if (context == "range" && pipe->Decl.size() < 2) {
                int t = peek_non_space().typ;
                if (t == itemVariable || t == itemRightDelim || t == itemRightParen) {
                    continue;  // second initialized variable in a range pipeline
                }
                errorf("range can only initialize variables");
            }
            errorf("too many declarations in " + std::string(context));
        } else if (token_after_variable.typ == itemSpace) {
            backup3(v, token_after_variable);
        } else {
            backup2(v);
        }
        break;
    }
    for (;;) {
        item tk = next_non_space();
        if (tk.typ == end) {
            // At this point, the pipeline is complete.
            check_pipeline(*pipe, context);
            return pipe;
        }
        switch (tk.typ) {
        case itemBool: case itemCharConstant: case itemComplex: case itemDot: case itemField: case itemIdentifier:
        case itemNumber: case itemNil: case itemRawString: case itemString: case itemVariable: case itemLeftParen:
            backup();
            pipe->Cmds.push_back(command());
            break;
        default:
            unexpected(tk, context);
        }
    }
}

void Tree::check_pipeline(const PipeNode& pipe, std::string_view context) {
    // Reject empty pipelines.
    if (pipe.Cmds.empty()) errorf("missing value for " + std::string(context));
    // Only the first command of a pipeline can start with a non executable
    // operand.
    for (std::size_t i = 1; i < pipe.Cmds.size(); i++) {
        switch (pipe.Cmds[i]->Args[0]->Type()) {
        case NodeBool: case NodeDot: case NodeNil: case NodeNumber: case NodeString:
            // With A|B|C, pipeline stage 2 is B.
            errorf(std::format("non executable command in pipeline stage {}", i + 1));
        default:
            break;
        }
    }
}

ptr<BranchNode> Tree::parse_control(std::string_view context, ptr<BranchNode> node) {
    std::size_t nvars = vars_.size();
    struct pop {
        std::vector<string>& v;
        std::size_t n;
        ~pop() { v.resize(n); }
    } popper{vars_, nvars};
    auto pipe = pipeline(context, itemRightDelim);
    if (context == "range") range_depth_++;
    auto [list, nx] = item_list();
    if (context == "range") range_depth_--;
    ptr<ListNode> else_list;
    if (nx->Type() == nodeElse) {
        // Special case for "else if" and "else with". If the "else" is
        // followed immediately by an "if" or "with", the elseControl will
        // have left the "if" or "with" token pending. Treat
        //     {{if a}}_{{else if b}}_{{end}}
        //     {{with a}}_{{else with b}}_{{end}}
        // as
        //     {{if a}}_{{else}}{{if b}}_{{end}}{{end}}
        //     {{with a}}_{{else}}{{with b}}_{{end}}{{end}}.
        // To do this, parse the "if" or "with" as usual and stop at it
        // {{end}}; the subsequent{{end}} is assumed. This technique works
        // even for long if-else-if chains.
        if (context == "if" && peek().typ == itemIf) {
            next();  // consume the "if" token
            else_list = make<ListNode>(NodeList, nx->Position());
            else_list->Nodes.push_back(if_control());
        } else if (context == "with" && peek().typ == itemWith) {
            next();
            else_list = make<ListNode>(NodeList, nx->Position());
            else_list->Nodes.push_back(with_control());
        } else {
            auto [l, nx2] = item_list();
            else_list = l;
            if (nx2->Type() != nodeEnd) errorf("expected end; found " + nx2->String().str());
        }
    }
    node->Pos = pipe->Position();
    node->tr = this;
    node->Line = pipe->Line;
    node->Pipe = pipe;
    node->List = list;
    node->ElseList = else_list;
    return node;
}

ptr<Node> Tree::if_control() {
    auto n = make_ptr<IfNode>();
    n->NodeType = NodeIf;
    return parse_control("if", n);
}

ptr<Node> Tree::range_control() {
    auto n = make_ptr<RangeNode>();
    n->NodeType = NodeRange;
    return parse_control("range", n);
}

ptr<Node> Tree::with_control() {
    auto n = make_ptr<WithNode>();
    n->NodeType = NodeWith;
    return parse_control("with", n);
}

ptr<Node> Tree::end_control() { return make<EndNode>(nodeEnd, expect(itemRightDelim, "end").pos); }

ptr<Node> Tree::else_control() {
    item p = peek_non_space();
    // The "{{else if ... " and "{{else with ..." will be treated as
    // "{{else}}{{if ..." and "{{else}}{{with ...". So return the else
    // node here.
    if (p.typ == itemIf || p.typ == itemWith) {
        auto n = make<EndNode>(nodeElse, p.pos);
        n->Line = p.line;
        return n;
    }
    item token = expect(itemRightDelim, "else");
    auto n = make<EndNode>(nodeElse, token.pos);
    n->Line = token.line;
    return n;
}

ptr<Node> Tree::block_control() {
    constexpr std::string_view context = "block clause";
    item token = next_non_space();
    string name = parse_template_name(token, context);
    auto pipe = pipeline(context, itemRightDelim);

    auto block = New(name);  // name will be updated once we know it
    block->text = text;
    block->Mode = Mode;
    block->left_delim = left_delim;
    block->right_delim = right_delim;
    block->ParseName = ParseName;
    block->start_parse(funcs_, lex_, tree_set_);
    try {
        auto [list, end] = block->item_list();
        block->Root = list;
        if (end->Type() != nodeEnd) errorf("unexpected " + end->String().str() + " in " + std::string(context));
        block->add();
    } catch (parse_error&) {
        token_[0] = block->token_[0];
        throw;
    }
    block->stop_parse();

    auto n = make<TemplateNode>(NodeTemplate, token.pos);
    n->Line = token.line;
    n->Name = name;
    n->Pipe = pipe;
    return n;
}

ptr<Node> Tree::template_control() {
    constexpr std::string_view context = "template clause";
    item token = next_non_space();
    string name = parse_template_name(token, context);
    ptr<PipeNode> pipe;
    if (next_non_space().typ != itemRightDelim) {
        backup();
        // Do not pop variables; they persist until "end".
        pipe = pipeline(context, itemRightDelim);
    }
    auto n = make<TemplateNode>(NodeTemplate, token.pos);
    n->Line = token.line;
    n->Name = name;
    n->Pipe = pipe;
    return n;
}

string Tree::parse_template_name(const item& token, std::string_view context) {
    if (token.typ == itemString || token.typ == itemRawString) {
        auto [s, err] = detail::unquote(token.val);
        if (err) errorf(err.Error().str());
        return string(s);
    }
    unexpected(token, context);
}

ptr<CommandNode> Tree::command() {
    auto cmd = make<CommandNode>(NodeCommand, peek_non_space().pos);
    for (;;) {
        peek_non_space();  // skip leading spaces
        ptr<Node> op = operand();
        if (op) cmd->Args.push_back(op);
        item token = next();
        if (token.typ == itemSpace) continue;
        if (token.typ == itemRightDelim || token.typ == itemRightParen) {
            backup();
        } else if (token.typ != itemPipe) {
            unexpected(token, "operand");
        }
        break;
    }
    if (cmd->Args.empty()) errorf("empty command");
    return cmd;
}

ptr<Node> Tree::operand() {
    ptr<Node> node = term();
    if (!node) return nullptr;
    if (peek().typ == itemField) {
        auto chain = make<ChainNode>(NodeChain, peek().pos);
        chain->Node = node;
        while (peek().typ == itemField) chain->Add(next().val);
        // Compatibility with original API: If the term is of type
        // NodeField or NodeVariable, just put more fields on the original.
        // Otherwise, keep the Chain node. Obvious parsing errors involving
        // literal values are detected here. More complex error cases will
        // have to be handled at execution time.
        switch (node->Type()) {
        case NodeField: {
            auto f = make<FieldNode>(NodeField, chain->Position());
            for (auto& part : strings::Split(chain->String().slice(1), ".")) f->Ident.push_back(part);
            node = f;
            break;
        }
        case NodeVariable: {
            auto v = make<VariableNode>(NodeVariable, chain->Position());
            for (auto& part : strings::Split(chain->String(), ".")) v->Ident.push_back(part);
            node = v;
            break;
        }
        case NodeBool: case NodeString: case NodeNumber: case NodeNil: case NodeDot:
            errorf("unexpected . after term " + detail::quote(node->String()));
        default:
            node = chain;
        }
    }
    return node;
}

ptr<Node> Tree::term() {
    item token = next_non_space();
    switch (token.typ) {
    case itemIdentifier: {
        bool check_func = (Mode & SkipFuncCheck) == 0;
        if (check_func && !has_function(token.val)) errorf("function " + detail::quote(token.val) + " not defined");
        auto n = NewIdentifier(token.val);
        n->SetTree(this)->SetPos(token.pos);
        return n;
    }
    case itemDot: return make<DotNode>(NodeDot, token.pos);
    case itemNil: return make<NilNode>(NodeNil, token.pos);
    case itemVariable: return use_var(token.pos, token.val);
    case itemField: {
        auto f = make<FieldNode>(NodeField, token.pos);
        for (auto& part : strings::Split(token.val.slice(1), ".")) f->Ident.push_back(part);  // drop leading period
        return f;
    }
    case itemBool: {
        auto b = make<BoolNode>(NodeBool, token.pos);
        b->True = token.val == "true";
        return b;
    }
    case itemCharConstant: case itemComplex: case itemNumber:
        return new_number(token.pos, token.val, token.typ);
    case itemLeftParen: {
        if (stack_depth_ >= max_stack_depth) errorf("max expression depth exceeded");
        stack_depth_++;
        struct guard {
            int& d;
            ~guard() { d--; }
        } g{stack_depth_};
        return pipeline("parenthesized pipeline", itemRightParen);
    }
    case itemString: case itemRawString: {
        auto [s, err] = detail::unquote(token.val);
        if (err) errorf(err.Error().str());
        auto n = make<StringNode>(NodeString, token.pos);
        n->Quoted = token.val;
        n->Text = string(s);
        return n;
    }
    }
    backup();
    return nullptr;
}

ptr<Node> Tree::new_number(parse::Pos pos, std::string_view text_in, int typ) {
    auto n = make<NumberNode>(NodeNumber, pos);
    n->Text = string(text_in);
    std::string_view text = text_in;
    if (typ == itemCharConstant) {
        // Character constant: a rune, so an integer.
        auto [r, multibyte, tail, err] = detail::unquote_char(text.substr(1), text[0]);
        if (err) errorf(err.Error().str());
        if (tail != "'") errorf("malformed character constant: " + std::string(text));
        n->Int64 = r;
        n->IsInt = true;
        n->Uint64 = static_cast<uint64>(r);
        n->IsUint = true;
        n->Float64 = r;  // odd but those are the rules
        n->IsFloat = true;
        return n;
    }
    if (typ == itemComplex) {
        // A real and an imaginary part: "1+2i".
        auto split = text.find_first_of("+-", 1);
        while (split != std::string_view::npos && (text[split - 1] == 'e' || text[split - 1] == 'E' ||
                                                   text[split - 1] == 'p' || text[split - 1] == 'P')) {
            split = text.find_first_of("+-", split + 1);
        }
        if (split == std::string_view::npos) errorf("expected integer");
        auto [re, e1] = detail::parse_float(text.substr(0, split));
        auto [im, e2] = detail::parse_float(text.substr(split, text.size() - split - 1));
        if (e1 || e2) errorf("expected integer");
        n->IsComplex = true;
        n->Complex128 = {re, im};
        n->simplify_complex();
        return n;
    }
    // Imaginary constants can only be complex unless they are zero.
    if (!text.empty() && text.back() == 'i') {
        auto [f, err] = detail::parse_float(text.substr(0, text.size() - 1));
        if (!err) {
            n->IsComplex = true;
            n->Complex128 = {0, f};
            n->simplify_complex();
            return n;
        }
    }
    // Do integer test first so we get 0x123 etc.
    auto [u, uerr] = detail::parse_uint(text, 0, 64);  // will fail for -0; fixed below
    if (!uerr) {
        n->IsUint = true;
        n->Uint64 = u;
    }
    auto [i, ierr] = detail::parse_int(text, 0, 64);
    if (!ierr) {
        n->IsInt = true;
        n->Int64 = i;
        if (i == 0) {
            n->IsUint = true;  // in case of -0
            n->Uint64 = u;
        }
    }
    // If an integer extraction succeeded, promote the float.
    if (n->IsInt) {
        n->IsFloat = true;
        n->Float64 = static_cast<double>(n->Int64);
    } else if (n->IsUint) {
        n->IsFloat = true;
        n->Float64 = static_cast<double>(n->Uint64);
    } else {
        auto [f, ferr] = detail::parse_float(text);
        if (!ferr) {
            // If we parsed it as a float but it looks like an integer,
            // it's a huge number too large to fit in an int. Reject it.
            if (text.find_first_of(".eEpP") == std::string_view::npos) errorf("integer overflow: " + detail::quote(text));
            n->IsFloat = true;
            n->Float64 = f;
            // If a floating-point extraction succeeded, extract the int if
            // needed.
            if (!n->IsInt && f >= -9.2233720368547758e18 && f < 9.2233720368547758e18 &&
                static_cast<double>(static_cast<int64>(f)) == f) {
                n->IsInt = true;
                n->Int64 = static_cast<int64>(f);
            }
            if (!n->IsUint && f >= 0 && f < 1.8446744073709552e19 && static_cast<double>(static_cast<uint64>(f)) == f) {
                n->IsUint = true;
                n->Uint64 = static_cast<uint64>(f);
            }
        }
    }
    if (!n->IsInt && !n->IsUint && !n->IsFloat) errorf("illegal number syntax: " + detail::quote(text));
    (void)is_rune_int;
    return n;
}

bool Tree::has_function(std::string_view name) const {
    for (const FuncNames* f : funcs_) {
        if (f && f->contains(name)) return true;
    }
    return false;
}

ptr<Node> Tree::use_var(parse::Pos pos, std::string_view name) {
    auto v = make<VariableNode>(NodeVariable, pos);
    for (auto& part : strings::Split(string(name), ".")) v->Ident.push_back(part);
    for (auto& var_name : vars_) {
        if (var_name == v->Ident[0]) return v;
    }
    errorf("undefined variable " + detail::quote(v->Ident[0]));
}

}  // namespace gopp::text::template_::parse
