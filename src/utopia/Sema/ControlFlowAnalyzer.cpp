#include "utopia/Sema/Sema.hpp"

namespace utopia {

void ControlFlowPass::visit(const NamespaceDeclNode *node) {
  for (const auto *stmt : node->statements) {
    dispatch(stmt);
  }
}

void ControlFlowPass::visit(const UsingNode *node) {}

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
  if (node->superCall) {
    dispatch(node->superCall);
  }
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

  bool prevAssignTarget = isAssignTarget;
  isAssignTarget = true;
  dispatch(node->target);
  isAssignTarget = prevAssignTarget;
}

void ControlFlowPass::visit(const VariableNode *node) {
  if (node->resolvedDecl) {
    if (auto *varDecl = llvm::dyn_cast<VarDeclNode>(node->resolvedDecl)) {
      if (!varDecl->isGlobal && !varDecl->isStatic && !varDecl->isExtern) {
        if (isAssignTarget) {
          initStates[varDecl] = true;
        } else {
          auto it = initStates.find(varDecl);
          if (it != initStates.end() && !it->second) {
            ctx->diags.report({DiagLevel::Warning, node->line, node->column,
                               node->length,
                               "Use of uninitialized variable '" +
                                   std::string(node->name) + "'",
                               std::string(ctx->currentFile)});
            initStates[varDecl] = true;
          }
        }
      }
    }
  }
}

void ControlFlowPass::visit(const UnaryOpNode *node) {
  if (node->op == "&") {    bool prevAssignTarget = isAssignTarget;
    isAssignTarget = true;
    dispatch(node->expr);
    isAssignTarget = prevAssignTarget;

    if (auto *varNode = llvm::dyn_cast<VariableNode>(node->expr)) {
      if (varNode->resolvedDecl) {
        if (auto *varDecl =
                llvm::dyn_cast<VarDeclNode>(varNode->resolvedDecl)) {
          initStates[varDecl] = true;
        }
      }
    }
  } else {
    dispatch(node->expr);
  }
}

void ControlFlowPass::visit(const BinaryOpNode *node) {
  dispatch(node->left);
  dispatch(node->right);
}

void ControlFlowPass::visit(const TernaryOpNode *node) {
  dispatch(node->condition);

  bool initialReachable = isReachable;
  auto initialStates = initStates;

  dispatch(node->trueExpr);
  bool thenReachable = isReachable;
  auto thenStates = initStates;

  isReachable = initialReachable;
  initStates = initialStates;

  dispatch(node->falseExpr);
  bool elseReachable = isReachable;

  isReachable = thenReachable || elseReachable;

  for (auto &[var, state] : initStates) {
    if (state) {
      auto it = thenStates.find(var);
      if (it == thenStates.end() || !it->second) {
        state = false;
      }
    }
  }
}

void ControlFlowPass::visit(const FunctionCallNode *node) {
  if (node->target) {
    dispatch(node->target);
  }

  for (size_t i = 0; i < node->args.size(); ++i) {
    auto *arg = node->args[i];
    bool isMutableOutParam = false;
    const Type *paramType = nullptr;

    if (node->resolvedFunc && i < node->resolvedFunc->params.size()) {
      paramType = node->resolvedFunc->params[i]->type;
    } else if (node->target && node->target->exprType) {
      const Type *unqual = node->target->exprType->getUnqualifiedType();
      if (unqual->isPointerType()) {
        const Type *pointee =
            static_cast<const PointerType *>(unqual)->getPointeeType();
        if (pointee->getKind() == TypeKind::Function) {
          auto *fTy = static_cast<const FunctionType *>(pointee);
          if (i < fTy->getParamTypes().size()) {
            paramType = fTy->getParamTypes()[i];
          }
        }
      }
    }

    if (paramType) {
      if (paramType->isPointerType() || paramType->isReferenceType()) {
        const Type *pointee = nullptr;
        if (paramType->isPointerType()) {
          pointee =
              static_cast<const PointerType *>(paramType)->getPointeeType();
        } else {
          pointee =
              static_cast<const ReferenceType *>(paramType)->getPointeeType();
        }

        if (pointee && !pointee->isConstQualified()) {
          isMutableOutParam = true;
        }
      }
    }

    if (isMutableOutParam) {
      bool prevAssignTarget = isAssignTarget;
      isAssignTarget = true;
      dispatch(arg);
      isAssignTarget = prevAssignTarget;

      if (auto *varNode = llvm::dyn_cast<VariableNode>(arg)) {
        if (varNode->resolvedDecl) {
          if (auto *varDecl =
                  llvm::dyn_cast<VarDeclNode>(varNode->resolvedDecl)) {
            initStates[varDecl] = true;
          }
        }
      } else if (auto *uop = llvm::dyn_cast<UnaryOpNode>(arg)) {
        if (uop->op == "&") {
          if (auto *varNode = llvm::dyn_cast<VariableNode>(uop->expr)) {
            if (varNode->resolvedDecl) {
              if (auto *varDecl =
                      llvm::dyn_cast<VarDeclNode>(varNode->resolvedDecl)) {
                initStates[varDecl] = true;
              }
            }
          }
        }
      }
    } else {
      dispatch(arg);
    }
  }
}

void ControlFlowPass::visit(const CastNode *node) { dispatch(node->expr); }
void ControlFlowPass::visit(const AwaitExprNode *node) {
  dispatch(node->expr);
}

void ControlFlowPass::visit(const MemberAccessNode *node) {
  dispatch(node->object);
}

void ControlFlowPass::visit(const ArraySubscriptNode *node) {
  bool prevAssignTarget = isAssignTarget;
  isAssignTarget = false;
  dispatch(node->index);
  isAssignTarget = prevAssignTarget;

  dispatch(node->base);
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