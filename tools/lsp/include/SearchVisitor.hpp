#pragma once
#include "utopia/AST/ASTVisitor.hpp"

namespace utopia::lsp {

class SearchVisitor : public ASTVisitor<SearchVisitor, const ASTNode *> {
  int targetLine;
  int targetCol;

public:
  const FunctionCallNode *innermostCall = nullptr;

  SearchVisitor(int line, int col) : targetLine(line), targetCol(col) {}

  const ASTNode *find(const ASTNode *root) {
    if (!root)
      return nullptr;
    return dispatch(root);
  }

  bool isHit(const ASTNode *n) const {
    if (!n)
      return false;

    if (n->endLine > n->line) {
      /* Multiline node: column resolution is unreliable at the boundary,
       * accept any position within the line range after the start line. */
      if (targetLine < n->line || targetLine > n->endLine)
        return false;
      if (targetLine == n->line && targetCol < n->column)
        return false;
      return true;
    }

    return (targetLine == n->line && targetCol >= n->column &&
            targetCol < (n->column + n->length));
  }

  const ASTNode *visit(const NumberNode *n) { return isHit(n) ? n : nullptr; }
  const ASTNode *visit(const BoolNode *n) { return isHit(n) ? n : nullptr; }
  const ASTNode *visit(const CharNode *n) { return isHit(n) ? n : nullptr; }
  const ASTNode *visit(const RuneNode *n) { return isHit(n) ? n : nullptr; }
  const ASTNode *visit(const StringNode *n) { return isHit(n) ? n : nullptr; }
  const ASTNode *visit(const NullNode *n) { return isHit(n) ? n : nullptr; }
  const ASTNode *visit(const VariableNode *n) { return isHit(n) ? n : nullptr; }

  const ASTNode *visit(const UnaryOpNode *n) {
    if (auto found = find(n->expr))
      return found;
    return isHit(n) ? n : nullptr;
  }

  const ASTNode *visit(const AwaitExprNode *n) {
    if (auto found = find(n->expr))
      return found;
    return isHit(n) ? n : nullptr;
  }

  const ASTNode *visit(const BinaryOpNode *n) {
    if (auto L = find(n->left))
      return L;
    if (auto R = find(n->right))
      return R;
    return isHit(n) ? n : nullptr;
  }

  const ASTNode *visit(const TernaryOpNode *n) {
    if (auto found = find(n->condition))
      return found;
    if (auto found = find(n->trueExpr))
      return found;
    if (auto found = find(n->falseExpr))
      return found;
    return isHit(n) ? n : nullptr;
  }

  const ASTNode *visit(const AssignNode *n) {
    if (auto found = find(n->target))
      return found;
    if (auto found = find(n->value))
      return found;
    return isHit(n) ? n : nullptr;
  }

  const ASTNode *visit(const BlockNode *n) {
    for (auto *s : n->statements)
      if (auto found = find(s))
        return found;
    return nullptr;
  }

  const ASTNode *visit(const IfNode *n) {
    if (auto found = find(n->condition))
      return found;
    if (auto found = find(n->thenBlock))
      return found;
    if (n->elseBlock)
      if (auto found = find(n->elseBlock))
        return found;
    return isHit(n) ? n : nullptr;
  }

  const ASTNode *visit(const ForNode *n) {
    if (n->initStatement)
      if (auto found = find(n->initStatement))
        return found;
    if (n->condition)
      if (auto found = find(n->condition))
        return found;
    if (n->increment)
      if (auto found = find(n->increment))
        return found;
    if (auto found = find(n->body))
      return found;
    return isHit(n) ? n : nullptr;
  }

  const ASTNode *visit(const WhileNode *n) {
    if (auto found = find(n->condition))
      return found;
    if (auto found = find(n->body))
      return found;
    return isHit(n) ? n : nullptr;
  }

  const ASTNode *visit(const SwitchNode *n) {
    if (auto found = find(n->condition))
      return found;
    for (auto *c : n->cases)
      if (auto found = find(c))
        return found;
    return isHit(n) ? n : nullptr;
  }

  const ASTNode *visit(const CaseNode *n) {
    if (n->value)
      if (auto found = find(n->value))
        return found;
    for (auto *s : n->statements)
      if (auto found = find(s))
        return found;
    return isHit(n) ? n : nullptr;
  }

  const ASTNode *visit(const BreakNode *n) { return isHit(n) ? n : nullptr; }
  const ASTNode *visit(const ContinueNode *n) { return isHit(n) ? n : nullptr; }

  const ASTNode *visit(const ReturnNode *n) {
    if (n->value)
      if (auto found = find(n->value))
        return found;
    return isHit(n) ? n : nullptr;
  }

  const ASTNode *visit(const CastNode *n) {
    if (auto found = find(n->expr))
      return found;
    return isHit(n) ? n : nullptr;
  }

  const ASTNode *visit(const ImplicitCastNode *n) {
    if (auto found = find(n->expr))
      return found;
    return isHit(n) ? n : nullptr;
  }

  const ASTNode *visit(const FunctionCallNode *n) {
    if (isHit(n)) {
      innermostCall = n;
    }
    if (auto found = find(n->target))
      return found;
    for (auto *a : n->args)
      if (auto found = find(a))
        return found;
    return isHit(n) ? n : nullptr;
  }

  const ASTNode *visit(const LambdaNode *n) {
    for (auto *p : n->params)
      if (auto found = find(p))
        return found;
    if (n->isExpressionBody && n->exprBody) {
      if (auto found = find(n->exprBody))
        return found;
    } else if (n->body) {
      if (auto found = find(n->body))
        return found;
    }
    return isHit(n) ? n : nullptr;
  }

  const ASTNode *visit(const MemberAccessNode *n) {
    if (auto found = find(n->object))
      return found;
    return isHit(n) ? n : nullptr;
  }

  const ASTNode *visit(const ArraySubscriptNode *n) {
    if (auto found = find(n->base))
      return found;
    if (auto found = find(n->index))
      return found;
    return isHit(n) ? n : nullptr;
  }

  const ASTNode *visit(const ArrayLiteralNode *n) {
    for (auto *e : n->elements)
      if (auto found = find(e))
        return found;
    return isHit(n) ? n : nullptr;
  }

  const ASTNode *visit(const NewExprNode *n) {
    if (n->arraySize)
      if (auto found = find(n->arraySize))
        return found;
    for (auto *a : n->args)
      if (auto found = find(a))
        return found;
    return isHit(n) ? n : nullptr;
  }

  const ASTNode *visit(const DestructorCallNode *n) {
    return n->object;
  }

  const ASTNode *visit(const DeleteExprNode *n) {
    if (auto found = find(n->ptr))
      return found;
    return isHit(n) ? n : nullptr;
  }

  const ASTNode *visit(const TypeLiteralNode *n) {
    return isHit(n) ? n : nullptr;
  }

  const ASTNode *visit(const ModuleNode *n) {
    for (auto *s : n->statements)
      if (auto found = find(s))
        return found;
    return nullptr;
  }

  const ASTNode *visit(const VarDeclNode *n) {
    if (n->initializer)
      if (auto found = find(n->initializer))
        return found;
    return isHit(n) ? n : nullptr;
  }

  const ASTNode *visit(const ParamDeclNode *n) {
    if (n->defaultValue)
      if (auto found = find(n->defaultValue))
        return found;
    return isHit(n) ? n : nullptr;
  }

  const ASTNode *visit(const FunctionDeclNode *n) {
    if (n->isImplicit)
      return nullptr;

    if (n->body)
      if (auto found = find(n->body))
        return found;

    return isHit(n) ? n : nullptr;
  }

  const ASTNode *visit(const StructDeclNode *n) {
    for (auto *f : n->fields)
      if (auto found = find(f))
        return found;
    for (auto *c : n->constructors)
      if (auto found = find(c))
        return found;
    for (auto *m : n->methods)
      if (auto found = find(m))
        return found;
    if (n->destructor)
      if (auto found = find(n->destructor))
        return found;
    return isHit(n) ? n : nullptr;
  }

  const ASTNode *visit(const ClassDeclNode *n) {
    for (auto *f : n->fields)
      if (auto found = find(f))
        return found;
    for (auto *c : n->constructors)
      if (auto found = find(c))
        return found;
    for (auto *m : n->methods)
      if (auto found = find(m))
        return found;
    if (n->destructor)
      if (auto found = find(n->destructor))
        return found;
    return isHit(n) ? n : nullptr;
  }

  const ASTNode *visit(const UnionDeclNode *n) {
    for (auto *f : n->fields)
      if (auto found = find(f))
        return found;
    for (auto *c : n->constructors)
      if (auto found = find(c))
        return found;
    for (auto *m : n->methods)
      if (auto found = find(m))
        return found;
    if (n->destructor)
      if (auto found = find(n->destructor))
        return found;
    return isHit(n) ? n : nullptr;
  }

  const ASTNode *visit(const EnumDeclNode *n) {
    for (auto *m : n->members)
      if (auto found = find(m))
        return found;
    return isHit(n) ? n : nullptr;
  }

  const ASTNode *visit(const EnumMemberNode *n) {
    if (n->initializer)
      if (auto found = find(n->initializer))
        return found;
    return isHit(n) ? n : nullptr;
  }

  const ASTNode *visit(const NamespaceDeclNode *n) {
    for (auto *s : n->statements)
      if (auto found = find(s))
        return found;
    return isHit(n) ? n : nullptr;
  }

  const ASTNode *visit(const UsingNode *n) { return isHit(n) ? n : nullptr; }

  const ASTNode *visit(const AnnotationDeclNode *n) {
    for (auto *f : n->fields)
      if (auto found = find(f))
        return found;
    if (n->constructor)
      if (auto found = find(n->constructor))
        return found;
    return isHit(n) ? n : nullptr;
  }

  const ASTNode *visit(const TypedefDeclNode *n) {
    return isHit(n) ? n : nullptr;
  }

  const ASTNode *visit(const AnnotationNode *n) {
    for (auto *a : n->args)
      if (auto found = find(a))
        return found;
    return isHit(n) ? n : nullptr;
  }
};

} // namespace utopia::lsp