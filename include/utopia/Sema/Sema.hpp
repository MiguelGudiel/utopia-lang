#pragma once
#include "utopia/AST/ASTVisitor.hpp"
#include "utopia/Sema/SemaContext.hpp"
#include <memory>
#include <unordered_set>
#include <vector>

namespace utopia {

/*
 * Strips indirection to evaluate core type compatibility.
 * References are transparently followed to their underlying pointee types.
 */
static bool canImplicitlyCast(const Type *from, const Type *to) {
  if (!from || !to)
    return false;

  if (from == to)
    return true;

  const Type *baseFrom =
      from->isReferenceType()
          ? static_cast<const ReferenceType *>(from)->getPointeeType()
          : from;
  const Type *baseTo =
      to->isReferenceType()
          ? static_cast<const ReferenceType *>(to)->getPointeeType()
          : to;

  if (baseFrom == baseTo)
    return true;

  if (baseFrom->getUnqualifiedType() == baseTo->getUnqualifiedType()) {
    if (!baseFrom->isConstQualified() || baseTo->isConstQualified())
      return true;
  }

  if (baseFrom->isPointerType() && baseTo->isPointerType()) {
    const Type *fromPointee =
        static_cast<const PointerType *>(baseFrom)->getPointeeType();
    const Type *toPointee =
        static_cast<const PointerType *>(baseTo)->getPointeeType();

    if (toPointee->isVoid())
      return true;

    return fromPointee->getUnqualifiedType() == toPointee->getUnqualifiedType();
  }

  return baseFrom->isNumeric() && baseTo->isNumeric();
}

class SemaPass {
public:
  virtual ~SemaPass() = default;
  virtual bool run(const ModuleNode *module, SemaContext &ctx) = 0;
  virtual const char *getName() const = 0;
};

class DeclCollectorPass : public SemaPass,
                          public ASTVisitor<DeclCollectorPass, void> {
  SemaContext *ctx = nullptr;
  std::unordered_set<const ModuleNode *> visitedModules;

public:
  bool run(const ModuleNode *module, SemaContext &context) override;
  const char *getName() const override { return "DeclarationCollector"; }

  void visit(const ModuleNode *node);
  void visit(const FunctionDeclNode *node);
  void visit(const VarDeclNode *node);
  void visit(const StructDeclNode *node);
  void visit(const ClassDeclNode *node);
  void visit(const AnnotationDeclNode *node);

  void visit(const AnnotationNode *node) {}
  void visit(const NumberNode *) {}
  void visit(const BoolNode *) {}
  void visit(const CharNode *) {}
  void visit(const RuneNode *) {}
  void visit(const StringNode *) {}
  void visit(const VariableNode *) {}
  void visit(const UnaryOpNode *) {}
  void visit(const BinaryOpNode *) {}
  void visit(const AssignNode *) {}
  void visit(const BlockNode *) {}
  void visit(const FunctionCallNode *) {}
  void visit(const IfNode *node);
  void visit(const ForNode *node);
  void visit(const WhileNode *node);
  void visit(const ReturnNode *) {}
  void visit(const CastNode *) {}
  void visit(const ParamDeclNode *) {}
  void visit(const MemberAccessNode *) {}
};

class TypeCheckPass : public SemaPass,
                      public ASTVisitor<TypeCheckPass, SemaResult> {
  SemaContext *ctx = nullptr;
  std::unordered_set<const ModuleNode *> visitedModules;

public:
  bool run(const ModuleNode *module, SemaContext &context) override;
  const char *getName() const override { return "TypeChecker"; }

  SemaResult visit(const NumberNode *node);
  SemaResult visit(const BoolNode *node);
  SemaResult visit(const CharNode *node);
  SemaResult visit(const RuneNode *node);
  SemaResult visit(const StringNode *node);
  SemaResult visit(const VariableNode *node);
  SemaResult visit(const UnaryOpNode *node);
  SemaResult visit(const BinaryOpNode *node);
  SemaResult visit(const VarDeclNode *node);
  SemaResult visit(const AssignNode *node);
  SemaResult visit(const BlockNode *node);
  SemaResult visit(const FunctionDeclNode *node);
  SemaResult visit(const FunctionCallNode *node);
  SemaResult visit(const IfNode *node);
  SemaResult visit(const ForNode *node);
  SemaResult visit(const WhileNode *node);
  SemaResult visit(const ReturnNode *node);
  SemaResult visit(const CastNode *node);
  SemaResult visit(const ParamDeclNode *node);
  SemaResult visit(const ModuleNode *node);
  SemaResult visit(const StructDeclNode *node);
  SemaResult visit(const ClassDeclNode *node);
  SemaResult visit(const MemberAccessNode *node);
  SemaResult visit(const AnnotationDeclNode *node);
  SemaResult visit(const AnnotationNode *node);
};

class SemaPipeline {
  std::vector<std::unique_ptr<SemaPass>> passes;

public:
  SemaPipeline();
  bool run(const ModuleNode *module, SemaContext &ctx);
};

} // namespace utopia