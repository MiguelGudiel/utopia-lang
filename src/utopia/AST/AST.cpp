// File: src/utopia/AST/AST.cpp
#include "utopia/AST/AST.hpp"
#include "utopia/AST/ASTVisitor.hpp"

namespace utopia {

// visitor->visit(this);
// Bypasses RTTI overhead by allowing the compiler to implicitly
// resolve the vtable pointer of the calling object. Do not touch.

void ThisNode::accept(ASTVisitor *visitor) { visitor->visit(this); }
void StructDeclNode::accept(ASTVisitor *visitor) { visitor->visit(this); }
void MemberAccessNode::accept(ASTVisitor *visitor) { visitor->visit(this); }
void BlockNode::accept(ASTVisitor *visitor) { visitor->visit(this); }
void NullLiteralNode::accept(ASTVisitor *visitor) { visitor->visit(this); }
void IfNode::accept(ASTVisitor *visitor) { visitor->visit(this); }
void WhileNode::accept(ASTVisitor *visitor) { visitor->visit(this); }
void ForNode::accept(ASTVisitor *visitor) { visitor->visit(this); }
void BreakNode::accept(ASTVisitor *visitor) { visitor->visit(this); }
void ContinueNode::accept(ASTVisitor *visitor) { visitor->visit(this); }
void NullAssertNode::accept(ASTVisitor *visitor) { visitor->visit(this); }
void NumberNode::accept(ASTVisitor *visitor) { visitor->visit(this); }
void FloatNode::accept(ASTVisitor *visitor) { visitor->visit(this); }
void BoolNode::accept(ASTVisitor *visitor) { visitor->visit(this); }
void StringNode::accept(ASTVisitor *visitor) { visitor->visit(this); }
void UnaryMinusNode::accept(ASTVisitor *visitor) { visitor->visit(this); }
void SubscriptNode::accept(ASTVisitor *visitor) { visitor->visit(this); }
void VariableNode::accept(ASTVisitor *visitor) { visitor->visit(this); }
void AddressOfNode::accept(ASTVisitor *visitor) { visitor->visit(this); }
void DerefNode::accept(ASTVisitor *visitor) { visitor->visit(this); }
void NewNode::accept(ASTVisitor *visitor) { visitor->visit(this); }
void DeleteNode::accept(ASTVisitor *visitor) { visitor->visit(this); }
void BinaryOpNode::accept(ASTVisitor *visitor) { visitor->visit(this); }
void CallNode::accept(ASTVisitor *visitor) { visitor->visit(this); }
void AssignNode::accept(ASTVisitor *visitor) { visitor->visit(this); }
void VarDeclNode::accept(ASTVisitor *visitor) { visitor->visit(this); }
void ReturnNode::accept(ASTVisitor *visitor) { visitor->visit(this); }
void FunctionNode::accept(ASTVisitor *visitor) { visitor->visit(this); }
void ProgramNode::accept(ASTVisitor *visitor) { visitor->visit(this); }

} // namespace utopia