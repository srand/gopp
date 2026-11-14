// After Go's text/template/parse/node.go.
#include "../goconv.hpp"

#include "lex.hpp"

namespace gopp::text::tmpl::parse {

namespace {

template <typename N>
ptr<N> cloneBase(const Node& from) {
    auto n = make_ptr<N>();
    n->NodeType = from.NodeType;
    n->Pos = from.Pos;
    n->tr = from.tr;
    return n;
}

std::string_view left(const Node& n) { return n.tr ? n.tr->left_delim.view() : std::string_view("{{"); }
std::string_view right(const Node& n) { return n.tr ? n.tr->right_delim.view() : std::string_view("}}"); }

}  // namespace

string Node::String() const {
    std::string sb;
    write_to(sb);
    return string(sb);
}

// ListNode

ptr<ListNode> ListNode::CopyList() const {
    auto n = cloneBase<ListNode>(*this);
    for (auto& e : Nodes) n->Nodes.push_back(e->Copy());
    return n;
}

ptr<Node> ListNode::Copy() const { return CopyList(); }

void ListNode::write_to(std::string& sb) const {
    for (auto& n : Nodes) n->write_to(sb);
}

// TextNode: Go prints it with textFormat, "%s" (tests use "%q").

bool text_format_q = false;

string TextNode::String() const { return text_format_q ? string(detail::quote(Text)) : Text; }

ptr<Node> TextNode::Copy() const {
    auto n = cloneBase<TextNode>(*this);
    n->Text = Text;
    return n;
}

void TextNode::write_to(std::string& sb) const { sb += String().view(); }

// CommentNode

ptr<Node> CommentNode::Copy() const {
    auto n = cloneBase<CommentNode>(*this);
    n->Text = Text;
    return n;
}

void CommentNode::write_to(std::string& sb) const {
    sb += left(*this);
    sb += Text.view();
    sb += right(*this);
}

// PipeNode

ptr<PipeNode> PipeNode::CopyPipe() const {
    auto n = cloneBase<PipeNode>(*this);
    n->Line = Line;
    n->IsAssign = IsAssign;
    for (auto& d : Decl) n->Decl.push_back(dynamic_pointer_cast<VariableNode>(d->Copy()));
    for (auto& c : Cmds) n->Cmds.push_back(dynamic_pointer_cast<CommandNode>(c->Copy()));
    return n;
}

ptr<Node> PipeNode::Copy() const { return CopyPipe(); }

void PipeNode::write_to(std::string& sb) const {
    if (!Decl.empty()) {
        for (std::size_t i = 0; i < Decl.size(); i++) {
            if (i > 0) sb += ", ";
            Decl[i]->write_to(sb);
        }
        sb += IsAssign ? " = " : " := ";
    }
    for (std::size_t i = 0; i < Cmds.size(); i++) {
        if (i > 0) sb += " | ";
        Cmds[i]->write_to(sb);
    }
}

// ActionNode

ptr<Node> ActionNode::Copy() const {
    auto n = cloneBase<ActionNode>(*this);
    n->Line = Line;
    n->Pipe = Pipe ? Pipe->CopyPipe() : nullptr;
    return n;
}

void ActionNode::write_to(std::string& sb) const {
    sb += left(*this);
    Pipe->write_to(sb);
    sb += right(*this);
}

// CommandNode

ptr<Node> CommandNode::Copy() const {
    auto n = cloneBase<CommandNode>(*this);
    for (auto& a : Args) n->Args.push_back(a->Copy());
    return n;
}

void CommandNode::write_to(std::string& sb) const {
    for (std::size_t i = 0; i < Args.size(); i++) {
        if (i > 0) sb.push_back(' ');
        if (Args[i]->Type() == NodePipe) {
            sb.push_back('(');
            Args[i]->write_to(sb);
            sb.push_back(')');
            continue;
        }
        Args[i]->write_to(sb);
    }
}

// IdentifierNode

ptr<IdentifierNode> NewIdentifier(string ident) {
    auto n = make_ptr<IdentifierNode>();
    n->NodeType = NodeIdentifier;
    n->Ident = std::move(ident);
    return n;
}

ptr<Node> IdentifierNode::Copy() const {
    auto n = NewIdentifier(Ident);
    n->SetTree(tr)->SetPos(Pos);
    return n;
}

void IdentifierNode::write_to(std::string& sb) const { sb += Ident.view(); }

// VariableNode

ptr<Node> VariableNode::Copy() const {
    auto n = cloneBase<VariableNode>(*this);
    n->Ident = Ident;
    return n;
}

void VariableNode::write_to(std::string& sb) const {
    for (std::size_t i = 0; i < Ident.size(); i++) {
        if (i > 0) sb.push_back('.');
        sb += Ident[i].view();
    }
}

// DotNode, NilNode

ptr<Node> DotNode::Copy() const { return cloneBase<DotNode>(*this); }
void DotNode::write_to(std::string& sb) const { sb.push_back('.'); }

ptr<Node> NilNode::Copy() const { return cloneBase<NilNode>(*this); }
void NilNode::write_to(std::string& sb) const { sb += "nil"; }

// FieldNode

ptr<Node> FieldNode::Copy() const {
    auto n = cloneBase<FieldNode>(*this);
    n->Ident = Ident;
    return n;
}

void FieldNode::write_to(std::string& sb) const {
    for (auto& id : Ident) {
        sb.push_back('.');
        sb += id.view();
    }
}

// ChainNode

void ChainNode::Add(std::string_view field) {
    if (field.empty() || field[0] != '.') panic("no dot in field");
    field.remove_prefix(1);
    if (field.empty()) panic("empty field");
    Field.push_back(string(field));
}

ptr<parse::Node> ChainNode::Copy() const {
    auto n = cloneBase<ChainNode>(*this);
    n->Node = Node;
    n->Field = Field;
    return n;
}

void ChainNode::write_to(std::string& sb) const {
    if (Node->Type() == NodePipe) {
        sb.push_back('(');
        Node->write_to(sb);
        sb.push_back(')');
    } else {
        Node->write_to(sb);
    }
    for (auto& f : Field) {
        sb.push_back('.');
        sb += f.view();
    }
}

// BoolNode

ptr<Node> BoolNode::Copy() const {
    auto n = cloneBase<BoolNode>(*this);
    n->True = True;
    return n;
}

void BoolNode::write_to(std::string& sb) const { sb += True ? "true" : "false"; }

// NumberNode

ptr<Node> NumberNode::Copy() const {
    return make_ptr<NumberNode>(*this);  // object's copy starts a new count
}

void NumberNode::simplifyComplex() {
    IsFloat = Complex128.imag() == 0;
    if (IsFloat) {
        Float64 = Complex128.real();
        IsInt = static_cast<double>(static_cast<int64>(Float64)) == Float64;
        if (IsInt) Int64 = static_cast<int64>(Float64);
        IsUint = Float64 >= 0 && static_cast<double>(static_cast<uint64>(Float64)) == Float64;
        if (IsUint) Uint64 = static_cast<uint64>(Float64);
    }
}

void NumberNode::write_to(std::string& sb) const { sb += Text.view(); }

// StringNode

ptr<Node> StringNode::Copy() const {
    auto n = cloneBase<StringNode>(*this);
    n->Quoted = Quoted;
    n->Text = Text;
    return n;
}

void StringNode::write_to(std::string& sb) const { sb += Quoted.view(); }

// EndNode: {{end}} and {{else}}

ptr<Node> EndNode::Copy() const {
    auto n = cloneBase<EndNode>(*this);
    n->Line = Line;
    return n;
}

void EndNode::write_to(std::string& sb) const {
    sb += left(*this);
    sb += NodeType == nodeElse ? "else" : "end";
    sb += right(*this);
}

// BranchNode

ptr<Node> BranchNode::Copy() const {
    ptr<BranchNode> n;
    switch (NodeType) {
    case NodeIf: n = cloneBase<IfNode>(*this); break;
    case NodeRange: n = cloneBase<RangeNode>(*this); break;
    case NodeWith: n = cloneBase<WithNode>(*this); break;
    default: panic("unknown branch type");
    }
    n->Line = Line;
    n->Pipe = Pipe ? Pipe->CopyPipe() : nullptr;
    n->List = List ? List->CopyList() : nullptr;
    n->ElseList = ElseList ? ElseList->CopyList() : nullptr;
    return n;
}

void BranchNode::write_to(std::string& sb) const {
    std::string_view name;
    switch (NodeType) {
    case NodeIf: name = "if"; break;
    case NodeRange: name = "range"; break;
    case NodeWith: name = "with"; break;
    default: panic("unknown branch type");
    }
    sb += left(*this);
    sb += name;
    sb.push_back(' ');
    Pipe->write_to(sb);
    sb += right(*this);
    List->write_to(sb);
    if (ElseList) {
        sb += left(*this);
        sb += "else";
        sb += right(*this);
        ElseList->write_to(sb);
    }
    sb += left(*this);
    sb += "end";
    sb += right(*this);
}

// BreakNode, ContinueNode

ptr<Node> BreakNode::Copy() const {
    auto n = cloneBase<BreakNode>(*this);
    n->Line = Line;
    return n;
}

void BreakNode::write_to(std::string& sb) const {
    sb += left(*this);
    sb += "break";
    sb += right(*this);
}

ptr<Node> ContinueNode::Copy() const {
    auto n = cloneBase<ContinueNode>(*this);
    n->Line = Line;
    return n;
}

void ContinueNode::write_to(std::string& sb) const {
    sb += left(*this);
    sb += "continue";
    sb += right(*this);
}

// TemplateNode

ptr<Node> TemplateNode::Copy() const {
    auto n = cloneBase<TemplateNode>(*this);
    n->Line = Line;
    n->Name = Name;
    n->Pipe = Pipe ? Pipe->CopyPipe() : nullptr;
    return n;
}

void TemplateNode::write_to(std::string& sb) const {
    sb += left(*this);
    sb += "template ";
    sb += detail::quote(Name);
    if (Pipe) {
        sb.push_back(' ');
        Pipe->write_to(sb);
    }
    sb += right(*this);
}

}  // namespace gopp::text::tmpl::parse
