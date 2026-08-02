#pragma once
#include "utopia/AST/AST.hpp"
#include "utopia/AST/ASTVisitor.hpp"

namespace utopia {

class EffectAnalyzer : public ASTVisitor<EffectAnalyzer, void> {
public:
  bool writesMem = false;
  bool readsMem = false;
  bool freesMem = false;
  bool hasSync = false;
  bool potentiallyInfinite = false;

  void visit(const AssignNode *n);
  void visit(const FunctionCallNode *n);
  void visit(const DeleteExprNode *n);
  void visit(const NewExprNode *n);
  void visit(const ImplicitCastNode *n);
  void visit(const ForNode *n);
  void visit(const WhileNode *n);
  void visit(const SwitchNode *n);
  void visit(const CaseNode *n) {}
  void visit(const BreakNode *n) {}
  void visit(const ContinueNode *n) {}
  void visit(const UnaryOpNode *n);
  void visit(const ArraySubscriptNode *n);
  void visit(const MemberAccessNode *n);
  void visit(const VariableNode *n);
  void visit(const BlockNode *n);
  void visit(const ReturnNode *n);
  void visit(const IfNode *n);
  void visit(const CastNode *n);
  void visit(const BinaryOpNode *n);
  void visit(const VarDeclNode *n);
  void visit(const ArrayLiteralNode *n);
  void visit(const TypeLiteralNode *n);
  
  void visit(const NumberNode *) {}
  void visit(const BoolNode *) {}
  void visit(const CharNode *) {}
  void visit(const RuneNode *) {}
  void visit(const StringNode *) {}
  void visit(const NullNode *) {}
  void visit(const AnnotationDeclNode *) {}
  void visit(const TypedefDeclNode *) {}
  void visit(const AnnotationNode *) {}
  void visit(const ParamDeclNode *) {}
  void visit(const ModuleNode *) {}
  void visit(const UnionDeclNode *n);
  void visit(const StructDeclNode *n) {}
  void visit(const ClassDeclNode *n) {}
  void visit(const FunctionDeclNode *) {}
  void visit(const EnumDeclNode *) {}
  void visit(const EnumMemberNode *) {}
};

} // namespace utopia