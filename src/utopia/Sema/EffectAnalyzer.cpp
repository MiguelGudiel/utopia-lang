#include "utopia/Sema/EffectAnalyzer.hpp"

namespace utopia {

void EffectAnalyzer::visit(const NamespaceDeclNode *n) {
  for (const auto *stmt : n->statements) {
    dispatch(stmt);
  }
}

void EffectAnalyzer::visit(const UsingNode *n) {}

void EffectAnalyzer::visit(const AssignNode *n) {
  writesMem = true;
  dispatch(n->target);
  dispatch(n->value);
}

void EffectAnalyzer::visit(const FunctionCallNode *n) {
  if (n->resolvedFunc) {
    if (n->resolvedFunc->isExtern) {
      writesMem = true;
      readsMem = true;
      freesMem = true;
      hasSync = true;
      potentiallyInfinite = true;
    } else {
      if (!n->resolvedFunc->isReadNone)
        readsMem = true;
      if (!n->resolvedFunc->isReadOnly && !n->resolvedFunc->isReadNone)
        writesMem = true;
      if (!n->resolvedFunc->isNoFree)
        freesMem = true;
      if (!n->resolvedFunc->isNoSync)
        hasSync = true;
      if (!n->resolvedFunc->isWillReturn)
        potentiallyInfinite = true;
    }
  } else {
    writesMem = true;
    readsMem = true;
    freesMem = true;
    hasSync = true;
    potentiallyInfinite = true;
  }
  for (auto *a : n->args)
    dispatch(a);
}

void EffectAnalyzer::visit(const DestructorCallNode *n) {}

void EffectAnalyzer::visit(const DeleteExprNode *n) {
  freesMem = true;
  writesMem = true;
  dispatch(n->ptr);
}

void EffectAnalyzer::visit(const NewExprNode *n) {
  readsMem = true;
  writesMem = true;
  for (auto *a : n->args)
    dispatch(a);
}

void EffectAnalyzer::visit(const ImplicitCastNode *n) {
  /* Implicit conversions invoke constructors which may have side effects */
  writesMem = true;
  readsMem = true;
  potentiallyInfinite = true;
  dispatch(n->expr);
}

void EffectAnalyzer::visit(const LambdaNode *n) {
  /* A lambda defers its effects until called; the body's effects are tracked
   * by the synthesized function's own analysis. */
}

void EffectAnalyzer::visit(const ForNode *n) {
  potentiallyInfinite = true;
  if (n->initStatement)
    dispatch(n->initStatement);
  if (n->condition)
    dispatch(n->condition);
  if (n->increment)
    dispatch(n->increment);
  dispatch(n->body);
}

void EffectAnalyzer::visit(const WhileNode *n) {
  potentiallyInfinite = true;
  dispatch(n->condition);
  dispatch(n->body);
}

void EffectAnalyzer::visit(const SwitchNode *n) {
  dispatch(n->condition);
  for (auto *c : n->cases) {
    if (c->value) {
      dispatch(c->value);
    }
    for (auto *s : c->statements) {
      dispatch(s);
    }
  }
}

void EffectAnalyzer::visit(const UnaryOpNode *n) {
  if (n->op == "*") {
    readsMem = true;
  } else if (n->op == "++" || n->op == "--") {
    readsMem = true;
    writesMem = true;
  }
  dispatch(n->expr);
}

void EffectAnalyzer::visit(const AwaitExprNode *n) {
  /* Await suspends on the event loop: it may not return and it touches the
   * scheduler state. */
  readsMem = true;
  writesMem = true;
  hasSync = true;
  potentiallyInfinite = true;
  dispatch(n->expr);
}

void EffectAnalyzer::visit(const BinaryOpNode *n) {
  dispatch(n->left);
  dispatch(n->right);
}

void EffectAnalyzer::visit(const TernaryOpNode *n) {
  dispatch(n->condition);
  dispatch(n->trueExpr);
  dispatch(n->falseExpr);
}

void EffectAnalyzer::visit(const UnionDeclNode *n) {}

void EffectAnalyzer::visit(const ArraySubscriptNode *n) {
  if (n->overloadedOperator) {
    if (n->overloadedOperator->isExtern) {
      writesMem = true;
      readsMem = true;
      freesMem = true;
      hasSync = true;
      potentiallyInfinite = true;
    } else {
      if (!n->overloadedOperator->isReadNone)
        readsMem = true;
      if (!n->overloadedOperator->isReadOnly &&
          !n->overloadedOperator->isReadNone)
        writesMem = true;
      if (!n->overloadedOperator->isNoFree)
        freesMem = true;
      if (!n->overloadedOperator->isNoSync)
        hasSync = true;
      if (!n->overloadedOperator->isWillReturn)
        potentiallyInfinite = true;
    }
  } else {
    readsMem = true;
  }
  dispatch(n->base);
  dispatch(n->index);
}

void EffectAnalyzer::visit(const MemberAccessNode *n) {
  readsMem = true;
  dispatch(n->object);
}

void EffectAnalyzer::visit(const TypeLiteralNode *n) {}

void EffectAnalyzer::visit(const VariableNode *n) {
  if (n->isField) {
    readsMem = true;
  }
  if (n->resolvedDecl && n->resolvedDecl->kind == NodeKind::VarDecl) {
    auto *varDecl = static_cast<const VarDeclNode *>(n->resolvedDecl);
    if (varDecl->isGlobal || varDecl->isStatic)
      readsMem = true;
  }
}

void EffectAnalyzer::visit(const BlockNode *n) {
  for (auto *s : n->statements)
    dispatch(s);
}

void EffectAnalyzer::visit(const ReturnNode *n) {
  if (n->value)
    dispatch(n->value);
}

void EffectAnalyzer::visit(const IfNode *n) {
  dispatch(n->condition);
  dispatch(n->thenBlock);
  if (n->elseBlock)
    dispatch(n->elseBlock);
}

void EffectAnalyzer::visit(const CastNode *n) {
  if (n->conversionConstructor) {
    writesMem = true;
    readsMem = true;
    potentiallyInfinite = true;
  }
  dispatch(n->expr);
}

void EffectAnalyzer::visit(const VarDeclNode *n) {
  if (n->initializer)
    dispatch(n->initializer);
}

void EffectAnalyzer::visit(const ArrayLiteralNode *n) {
  for (auto *e : n->elements)
    dispatch(e);
}

void EffectAnalyzer::visit(const MapLiteralNode *n) {
  for (auto *k : n->keys)
    dispatch(k);
  for (auto *v : n->values)
    dispatch(v);
}

} // namespace utopia