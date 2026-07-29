#pragma once
#include "utopia/AST/ASTVisitor.hpp"

namespace utopia::lsp {

class SearchVisitor : public ASTVisitor<SearchVisitor, const ASTNode *> {
  int targetLine;
  int targetCol;

public:
  SearchVisitor(int line, int col) : targetLine(line), targetCol(col) {}

  const ASTNode *find(const ASTNode *root) {
    if (!root)
      return nullptr;
    return dispatch(root);
  }

  bool isHit(const ASTNode *n) const {
    if (!n)
      return false;
    return (targetLine == n->line && targetCol >= n->column &&
            targetCol < (n->column + n->length));
  }

  const ASTNode *visit(const NumberNode *n) { return isHit(n) ? n : nullptr; }
  const ASTNode *visit(const VariableNode *n) { return isHit(n) ? n : nullptr; }

  const ASTNode *visit(const BinaryOpNode *n) {
    if (auto L = find(n->left))
      return L;
    if (auto R = find(n->right))
      return R;
    return isHit(n) ? n : nullptr;
  }

  const ASTNode *visit(const VarDeclNode *n) {
    if (n->initializer)
      if (auto found = find(n->initializer))
        return found;
    return isHit(n) ? n : nullptr;
  }

  const ASTNode *visit(const AssignNode *n) {
    if (auto found = find(n->value))
      return found;
    return isHit(n) ? n : nullptr;
  }

  const ASTNode *visit(const BlockNode *n) {
    for (auto &s : n->statements)
      if (auto found = find(s))
        return found;
    return nullptr;
  }

  const ASTNode *visit(const FunctionDeclNode *n) {
    if (auto found = find(n->body))
      return found;
    return isHit(n) ? n : nullptr;
  }

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

  const ASTNode *visit(const FunctionCallNode *n) {
    for (auto &a : n->args)
      if (auto found = find(a))
        return found;
    return isHit(n) ? n : nullptr;
  }

  const ASTNode *visit(const ModuleNode *n) {
    for (auto &s : n->statements)
      if (auto found = find(s))
        return found;
    return nullptr;
  }
};

} // namespace utopia::lsp