#pragma once
#include "utopia/AST/ASTContext.hpp"
#include "utopia/AST/ASTVisitor.hpp"
#include <string_view>
#include <unordered_map>
#include <vector>

namespace utopia {

class ASTCloner : public ASTVisitor<ASTCloner, ASTNode *> {
  ASTContext &ctx;
  const std::unordered_map<std::string_view, const Type *> &typeMap;
  std::string_view baseName;
  std::string_view mangledName;

public:
  ASTCloner(ASTContext &ctx,
            const std::unordered_map<std::string_view, const Type *> &tMap,
            std::string_view baseName = "", std::string_view mangledName = "");

  const Type *cloneType(const Type *t);

  template <typename T>
  llvm::ArrayRef<T *> cloneArray(llvm::ArrayRef<T *> arr) {
    std::vector<T *> cloned;
    for (auto *item : arr) {
      if (item)
        cloned.push_back(static_cast<T *>(dispatch(item)));
    }
    return ctx.copyArray<T *>(cloned);
  }

  ASTNode *visit(const NumberNode *n);
  ASTNode *visit(const BoolNode *n);
  ASTNode *visit(const CharNode *n);
  ASTNode *visit(const RuneNode *n);
  ASTNode *visit(const StringNode *n);
  ASTNode *visit(const NullNode *n);
  ASTNode *visit(const VariableNode *n);
  ASTNode *visit(const UnaryOpNode *n);
  ASTNode *visit(const BinaryOpNode *n);
  ASTNode *visit(const AssignNode *n);
  ASTNode *visit(const ArrayLiteralNode *n);
  ASTNode *visit(const ArraySubscriptNode *n);
  ASTNode *visit(const MemberAccessNode *n);
  ASTNode *visit(const FunctionCallNode *n);
  ASTNode *visit(const CastNode *n);
  ASTNode *visit(const NewExprNode *n);
  ASTNode *visit(const DeleteExprNode *n);
  ASTNode *visit(const BlockNode *n);
  ASTNode *visit(const IfNode *n);
  ASTNode *visit(const ForNode *n);
  ASTNode *visit(const WhileNode *n);
  ASTNode *visit(const SwitchNode *n);
  ASTNode *visit(const CaseNode *n);
  ASTNode *visit(const BreakNode *n);
  ASTNode *visit(const ContinueNode *n);
  ASTNode *visit(const ReturnNode *n);
  ASTNode *visit(const VarDeclNode *n);
  ASTNode *visit(const ParamDeclNode *n);
  ASTNode *visit(const FunctionDeclNode *n);
  ASTNode *visit(const UnionDeclNode *n);
  ASTNode *visit(const ClassDeclNode *n);
  ASTNode *visit(const StructDeclNode *n);
  ASTNode *visit(const ModuleNode *n);
  ASTNode *visit(const TypedefDeclNode *n);
  ASTNode *visit(const EnumDeclNode *n);
  ASTNode *visit(const EnumMemberNode *n);
  ASTNode *visit(const AnnotationDeclNode *n);
  ASTNode *visit(const AnnotationNode *n);
  ASTNode *visit(const ImplicitCastNode *n);
};

} // namespace utopia