#include "utopia/Sema/Sema.hpp"

namespace utopia {

bool ControlFlowPass::run(const ModuleNode *module, SemaContext &context) {
  ctx = &context;
  dispatch(module);
  return true;
}

void ControlFlowPass::visit(const ModuleNode *node) {
  if (visitedModules.contains(node))
    return;
  visitedModules.insert(node);

  auto prevFile = ctx->currentFile;
  ctx->setCurrentFile(node->filePath);

  for (const auto &stmt : node->statements) {
    isReachable = true;
    alreadyInUnreachable = false;
    initStates.clear();

    dispatch(stmt);
  }

  ctx->setCurrentFile(prevFile);
}

void ControlFlowPass::visit(const FunctionDeclNode *node) {
  if (node->body) {
    bool prevReachable = isReachable;
    bool prevAlready = alreadyInUnreachable;
    auto prevInit = initStates;

    isReachable = true;
    alreadyInUnreachable = false;
    initStates.clear();

    dispatch(node->body);

    isReachable = prevReachable;
    alreadyInUnreachable = prevAlready;
    initStates = prevInit;
  }
}

void ControlFlowPass::visit(const BlockNode *node) {
  bool reportedUnreachable = false;
  for (const auto &stmt : node->statements) {
    if (!isReachable && !alreadyInUnreachable && !reportedUnreachable) {
      const ASTNode *lastStmt = node->statements.back();
      ctx->diags.report({DiagLevel::Inactive, stmt->line, stmt->column,
                         stmt->length, "Unreachable code detected.",
                         std::string(ctx->currentFile), lastStmt->endLine});
      reportedUnreachable = true;
    }

    bool prevAlready = alreadyInUnreachable;
    if (!isReachable)
      alreadyInUnreachable = true;

    dispatch(stmt);

    alreadyInUnreachable = prevAlready;
  }
}

void ControlFlowPass::visit(const IfNode *node) {
  dispatch(node->condition);

  bool initialReachable = isReachable;
  auto initialStates = initStates;

  dispatch(node->thenBlock);
  bool thenReachable = isReachable;
  auto thenStates = initStates;

  isReachable = initialReachable;
  initStates = initialStates;

  bool elseReachable = initialReachable;
  if (node->elseBlock) {
    dispatch(node->elseBlock);
    elseReachable = isReachable;
  }

  isReachable = thenReachable || elseReachable;

  /* State intersection: A variable is strictly initialized after an IF branch
     only if it's evaluated and initialized across all possible control paths */
  for (auto &[var, state] : initStates) {
    if (state) {
      auto it = thenStates.find(var);
      if (it == thenStates.end() || !it->second) {
        state = false;
      }
    }
  }
}

void ControlFlowPass::visit(const ForNode *node) {
  bool wasReachable = isReachable;
  if (node->initStatement)
    dispatch(node->initStatement);
  if (node->condition)
    dispatch(node->condition);

  auto initialStates = initStates;
  dispatch(node->body);
  if (node->increment)
    dispatch(node->increment);

  initStates = initialStates;
  isReachable = wasReachable;
}

void ControlFlowPass::visit(const WhileNode *node) {
  bool wasReachable = isReachable;
  dispatch(node->condition);
  auto initialStates = initStates;
  dispatch(node->body);

  initStates = initialStates;
  isReachable = wasReachable;
}

void ControlFlowPass::visit(const SwitchNode *node) {
  dispatch(node->condition);

  bool wasReachable = isReachable;
  auto initialStates = initStates;
  bool anyReachable = false;

  std::unordered_map<const VarDeclNode *, bool> combinedStates;

  for (auto *c : node->cases) {
    isReachable = wasReachable;
    initStates = initialStates;

    if (c->value)
      dispatch(c->value);

    bool reportedUnreachable = false;
    for (auto *s : c->statements) {
      if (!isReachable && !alreadyInUnreachable && !reportedUnreachable) {
        const ASTNode *lastStmt = c->statements.back();
        ctx->diags.report({DiagLevel::Inactive, s->line, s->column, s->length,
                           "Unreachable code detected.",
                           std::string(ctx->currentFile), lastStmt->endLine});
        reportedUnreachable = true;
      }

      bool prevAlready = alreadyInUnreachable;
      if (!isReachable)
        alreadyInUnreachable = true;

      dispatch(s);

      alreadyInUnreachable = prevAlready;
    }

    anyReachable |= isReachable;

    if (combinedStates.empty()) {
      combinedStates = initStates;
    } else {
      for (auto &[var, state] : combinedStates) {
        if (state) {
          auto it = initStates.find(var);
          if (it == initStates.end() || !it->second) {
            state = false;
          }
        }
      }
    }
  }

  if (!node->hasDefault) {
    anyReachable |= wasReachable;
    initStates = initialStates;
  } else {
    initStates = combinedStates;
  }

  isReachable = anyReachable;
}

void ControlFlowPass::visit(const CaseNode *node) {}

void ControlFlowPass::visit(const BreakNode *node) { isReachable = false; }

void ControlFlowPass::visit(const ContinueNode *node) { isReachable = false; }

void ControlFlowPass::visit(const ReturnNode *node) {
  if (node->value)
    dispatch(node->value);
  isReachable = false;
}

void ControlFlowPass::visit(const VarDeclNode *node) {
  if (node->initializer) {
    dispatch(node->initializer);
    initStates[node] = true;
  } else {
    initStates[node] = false;
  }
}

void ControlFlowPass::visit(const AssignNode *node) {
  dispatch(node->value);

  bool prevAssign = isAssignTarget;
  isAssignTarget = true;
  dispatch(node->target);
  isAssignTarget = prevAssign;
}

void ControlFlowPass::visit(const VariableNode *node) {
  if (isAssignTarget) {
    if (node->resolvedDecl && node->resolvedDecl->kind == NodeKind::VarDecl) {
      initStates[static_cast<const VarDeclNode *>(node->resolvedDecl)] = true;
    }
  } else {
    if (node->resolvedDecl && node->resolvedDecl->kind == NodeKind::VarDecl) {
      auto varDecl = static_cast<const VarDeclNode *>(node->resolvedDecl);
      if (!varDecl->isGlobal && !varDecl->isStatic) {
        auto it = initStates.find(varDecl);
        if (it != initStates.end() && !it->second) {
          ctx->diags.report({DiagLevel::Warning, node->line, node->column,
                             node->length,
                             "Use of uninitialized variable '" +
                                 std::string(node->name) + "'.",
                             std::string(ctx->currentFile), node->endLine});

          /* Prevent redundant warning cascades for the same variable */
          initStates[varDecl] = true;
        }
      }
    }
  }
}

void ControlFlowPass::visit(const UnaryOpNode *node) { dispatch(node->expr); }

void ControlFlowPass::visit(const BinaryOpNode *node) {
  dispatch(node->left);
  dispatch(node->right);
}

void ControlFlowPass::visit(const FunctionCallNode *node) {
  dispatch(node->target);
  for (auto *arg : node->args) {
    dispatch(arg);
  }
}

void ControlFlowPass::visit(const CastNode *node) { dispatch(node->expr); }

void ControlFlowPass::visit(const MemberAccessNode *node) {
  dispatch(node->object);
}

void ControlFlowPass::visit(const ArraySubscriptNode *node) {
  dispatch(node->base);
  dispatch(node->index);
}

void ControlFlowPass::visit(const ArrayLiteralNode *node) {
  for (auto *elem : node->elements) {
    dispatch(elem);
  }
}

void ControlFlowPass::visit(const NewExprNode *node) {
  if (node->arraySize)
    dispatch(node->arraySize);
  for (auto *arg : node->args) {
    dispatch(arg);
  }
}

void ControlFlowPass::visit(const DeleteExprNode *node) { dispatch(node->ptr); }

void ControlFlowPass::visit(const ImplicitCastNode *node) {
  dispatch(node->expr);
}

} // namespace utopia