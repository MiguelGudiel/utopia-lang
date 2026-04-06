#pragma once

#include "utopia/AST/AST.hpp"
namespace utopia {

// Forward declarations for the vtable mapping
class ThisNode;
class StructDeclNode;
class MemberAccessNode;
class BlockNode;
class NullLiteralNode;
class IfNode;
class WhileNode;
class ForNode;
class BreakNode;
class ContinueNode;
class NullAssertNode;
class LogicalNotNode;
class NumberNode;
class FloatNode;
class BoolNode;
class StringNode;
class VariableNode;
class AddressOfNode;
class DerefNode;
class NewNode;
class DeleteNode;
class MoveNode;
class BinaryOpNode;
class CallNode;
class AssignNode;
class VarDeclNode;
class ReturnNode;
class FunctionNode;
class ProgramNode;

class ASTVisitor {
public:
  virtual ~ASTVisitor() = default;

  virtual void visit(ThisNode *node) = 0;
  virtual void visit(StructDeclNode *node) = 0;
  virtual void visit(ExtensionNode *node) = 0;
  virtual void visit(MemberAccessNode *node) = 0;
  virtual void visit(BlockNode *node) = 0;
  virtual void visit(NullLiteralNode *node) = 0;
  virtual void visit(IfNode *node) = 0;
  virtual void visit(WhileNode *node) = 0;
  virtual void visit(ForNode *node) = 0;
  virtual void visit(BreakNode *node) = 0;
  virtual void visit(ContinueNode *node) = 0;
  virtual void visit(NullAssertNode *node) = 0;
  virtual void visit(LogicalNotNode *node) = 0;
  virtual void visit(NumberNode *node) = 0;
  virtual void visit(FloatNode *node) = 0;
  virtual void visit(BoolNode *node) = 0;
  virtual void visit(StringNode *node) = 0;
  virtual void visit(UnaryMinusNode *node) = 0;
  virtual void visit(SubscriptNode *node) = 0;
  virtual void visit(VariableNode *node) = 0;
  virtual void visit(AddressOfNode *node) = 0;
  virtual void visit(DerefNode *node) = 0;
  virtual void visit(NewNode *node) = 0;
  virtual void visit(DeleteNode *node) = 0;
  virtual void visit(MoveNode *node) = 0;
  virtual void visit(BinaryOpNode *node) = 0;
  virtual void visit(CallNode *node) = 0;
  virtual void visit(AssignNode *node) = 0;
  virtual void visit(VarDeclNode *node) = 0;
  virtual void visit(ReturnNode *node) = 0;
  virtual void visit(FunctionNode *node) = 0;
  virtual void visit(ProgramNode *node) = 0;
};

} // namespace utopia