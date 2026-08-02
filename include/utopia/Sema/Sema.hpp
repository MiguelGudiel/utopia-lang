#pragma once
#include "utopia/AST/ASTVisitor.hpp"
#include "utopia/Common/Types.hpp"
#include "utopia/Sema/SemaContext.hpp"
#include <memory>
#include <unordered_set>
#include <vector>

namespace utopia {

/*
 * Strips indirection to evaluate core type compatibility, now safely stripping
 * both LValue and RValue references.
 */
bool canImplicitlyCast(const Type *from, const Type *to,
                              bool allowUserDefined = true);

class SemaPass {
public:
  virtual ~SemaPass() = default;
  virtual bool run(const ModuleNode *module, SemaContext &ctx) = 0;
  virtual const char *getName() const = 0;
};

class DeclCollectorPass : public SemaPass,
                          public ASTVisitor<DeclCollectorPass, void> {
  std::unordered_set<const ModuleNode *> visitedModules;

public:
  SemaContext *ctx = nullptr;
  bool run(const ModuleNode *module, SemaContext &context) override;
  const char *getName() const override { return "DeclarationCollector"; }

  void visit(const ModuleNode *node);
  void visit(const FunctionDeclNode *node);
  void visit(const VarDeclNode *node);
  void visit(const UnionDeclNode *node);
  void visit(const StructDeclNode *node);
  void visit(const ClassDeclNode *node);
  void visit(const TypedefDeclNode *node);
  void visit(const AnnotationDeclNode *node);
  void visit(const EnumDeclNode *node);
  void visit(const EnumMemberNode *node) {}

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
  void visit(const SwitchNode *node);
  void visit(const CaseNode *node) {}
  void visit(const BreakNode *node) {}
  void visit(const ContinueNode *node) {}
  void visit(const ReturnNode *) {}
  void visit(const CastNode *) {}
  void visit(const ParamDeclNode *) {}
  void visit(const MemberAccessNode *) {}
  void visit(const ArraySubscriptNode *) {}
  void visit(const ArrayLiteralNode *) {}
  void visit(const NewExprNode *) {}
  void visit(const DeleteExprNode *) {}
  void visit(const TypeLiteralNode *) {}
  void visit(const NullNode *) {}
  void visit(const ImplicitCastNode *node) {}
};

class TypeCheckPass : public SemaPass,
                      public ASTVisitor<TypeCheckPass, SemaResult> {
  SemaContext *ctx = nullptr;
  std::unordered_set<const ModuleNode *> visitedModules;

private:
  const FunctionDeclNode *
  resolveOverloadedOperator(const Type *lhsType, std::string_view opName,
                            const std::vector<ExprNode *> &args);

public:
  bool run(const ModuleNode *module, SemaContext &context) override;
  const char *getName() const override { return "TypeChecker"; }

  // Check type access visibility to avoid escaping private types across files
  bool checkTypeVisibility(const Type *type, const ASTNode *node);
  const Type *resolveIfTemplate(const Type *t);
  void checkImplicitCastWarning(const Type *from, const Type *to,
                                const ASTNode *node);
  ExprNode *performImplicitConversion(ExprNode *expr, const Type *to);

  /* Analyzes an AST node to emit warnings if a nodiscard value is ignored */
  void checkNodiscard(const ASTNode *node);
  void checkDeprecated(const DeclNode *decl, const ASTNode *node);

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
  SemaResult visit(const SwitchNode *node);
  SemaResult visit(const CaseNode *node) { return ctx->astCtx.VoidTy; }
  SemaResult visit(const BreakNode *node);
  SemaResult visit(const ContinueNode *node);
  SemaResult visit(const ReturnNode *node);
  SemaResult visit(const CastNode *node);
  SemaResult visit(const ParamDeclNode *node);
  SemaResult visit(const ModuleNode *node);
  SemaResult visit(const UnionDeclNode *node);
  SemaResult visit(const StructDeclNode *node);
  SemaResult visit(const ClassDeclNode *node);
  SemaResult visit(const MemberAccessNode *node);
  SemaResult visit(const AnnotationDeclNode *node);
  SemaResult visit(const TypedefDeclNode *node);
  SemaResult visit(const AnnotationNode *node);
  SemaResult visit(const ArraySubscriptNode *node);
  SemaResult visit(const ArrayLiteralNode *node);
  SemaResult visit(const NewExprNode *node);
  SemaResult visit(const DeleteExprNode *node);
  SemaResult visit(const TypeLiteralNode *node);
  SemaResult visit(const NullNode *node);
  SemaResult visit(const EnumDeclNode *node);
  SemaResult visit(const EnumMemberNode *node);
  SemaResult visit(const ImplicitCastNode *node);
};

class SemaPipeline {
  std::vector<std::unique_ptr<SemaPass>> passes;

public:
  SemaPipeline();
  bool run(const ModuleNode *module, SemaContext &ctx);
};

} // namespace utopia