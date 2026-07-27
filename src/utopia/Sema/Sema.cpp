#include "utopia/Sema/Sema.hpp"
#include "utopia/CodeGen/Mangler.hpp"

namespace utopia {

SemaPipeline::SemaPipeline() {
  passes.push_back(std::make_unique<DeclCollectorPass>());
  passes.push_back(std::make_unique<TypeCheckPass>());
}

bool SemaPipeline::run(const ModuleNode *module, SemaContext &ctx) {
  std::cout << "[Sema Debug] Initiating semantic analysis pipeline...\n"
            << std::flush;

  for (auto &pass : passes) {
    std::cout << "[Sema Debug] Executing pass: " << pass->getName() << "\n"
              << std::flush;

    try {
      if (!pass->run(module, ctx)) {
        std::cerr << "[Sema Debug] Pass aborted due to unexpected failure: "
                  << pass->getName() << "\n"
                  << std::flush;
        return false;
      }
    } catch (const std::exception &e) {
      std::cerr << "\033[1;31m[Fatal]\033[0m Exception caught in Sema pass '"
                << pass->getName() << "': " << e.what() << "\n"
                << std::flush;
      return false;
    } catch (...) {
      std::cerr << "\033[1;31m[Fatal]\033[0m Hardware/OS fault in Sema pass '"
                << pass->getName() << "'.\n"
                << std::flush;
      return false;
    }

    if (ctx.hasErrors()) {
      std::cerr << "[Sema Debug] Semantic integrity compromised during pass: "
                << pass->getName() << "\n"
                << std::flush;
      return false;
    }

    std::cout << "[Sema Debug] Pass completed successfully: " << pass->getName()
              << "\n"
              << std::flush;
  }
  return true;
}

bool DeclCollectorPass::run(const ModuleNode *module, SemaContext &context) {
  ctx = &context;
  dispatch(module);
  return !ctx->hasErrors();
}

void DeclCollectorPass::visit(const ModuleNode *node) {
  if (visitedModules.contains(node))
    return;
  visitedModules.insert(node);

  for (const auto *imp : node->importedModules) {
    dispatch(imp);
  }

  auto prevFile = ctx->currentFile;
  ctx->setCurrentFile(node->filePath);

  for (const auto &stmt : node->statements) {
    if (stmt->kind == NodeKind::FunctionDecl ||
        stmt->kind == NodeKind::VarDecl || stmt->kind == NodeKind::StructDecl ||
        stmt->kind == NodeKind::ClassDecl ||
        stmt->kind == NodeKind::AnnotationDecl) {
      dispatch(stmt);
    }
  }

  ctx->setCurrentFile(prevFile);
}

void DeclCollectorPass::visit(const AnnotationDeclNode *node) {
  ctx->addDecl(node->name, node);
  auto *recTy = ctx->astCtx.getRecordType(node->name);
  const_cast<AnnotationDeclNode *>(node)->recordType = recTy;
  recTy->setDeclaration(node);

  if (node->constructor) {
    const_cast<FunctionDeclNode *>(node->constructor)->mangledName =
        Mangler::mangle(node->constructor, std::string(node->name));
    ctx->addDecl(node->name, node->constructor);
  }
}

SemaResult TypeCheckPass::visit(const AnnotationDeclNode *node) {
  for (const auto *field : node->fields) {
    auto res = dispatch(field);
    if (!res)
      return res;
  }
  if (node->constructor) {
    auto res = dispatch(node->constructor);
    if (!res)
      return res;
  }
  return ctx->astCtx.VoidTy;
}

SemaResult TypeCheckPass::visit(const AnnotationNode *node) {
  for (const auto *arg : node->args) {
    auto res = dispatch(arg);
    if (!res)
      return res;
  }
  return ctx->astCtx.VoidTy;
}

void DeclCollectorPass::visit(const FunctionDeclNode *node) {
  if (node->mangledName.empty()) {
    const_cast<FunctionDeclNode *>(node)->mangledName = Mangler::mangle(node);
  }
  ctx->addDecl(node->name, node);
}

void DeclCollectorPass::visit(const VarDeclNode *node) {
  ctx->addDecl(node->varName, node);
}

void DeclCollectorPass::visit(const StructDeclNode *node) {
  ctx->addDecl(node->name, node);
  const_cast<StructDeclNode *>(node)->recordType =
      ctx->astCtx.getRecordType(node->name);
}

void DeclCollectorPass::visit(const ClassDeclNode *node) {
  ctx->addDecl(node->name, node);

  auto *recTy = ctx->astCtx.getRecordType(node->name);
  const_cast<ClassDeclNode *>(node)->recordType = recTy;
  recTy->setDeclaration(node);

  for (auto *ctor : node->constructors) {
    const_cast<FunctionDeclNode *>(ctor)->mangledName =
        Mangler::mangle(ctor, std::string(node->name));
    ctx->addDecl(node->name, ctor);
  }
  if (node->destructor) {
    const_cast<FunctionDeclNode *>(node->destructor)->mangledName =
        Mangler::mangle(node->destructor, std::string(node->name));
  }
  for (auto *method : node->methods) {
    const_cast<FunctionDeclNode *>(method)->mangledName =
        Mangler::mangle(method, std::string(node->name));
  }
}

bool TypeCheckPass::run(const ModuleNode *module, SemaContext &context) {
  ctx = &context;
  auto result = dispatch(module);

  if (!result && !ctx->hasErrors()) {
    ctx->reportError(result.error().line, result.error().column,
                     result.error().length, result.error().message);
  }

  return !ctx->hasErrors();
}

SemaResult TypeCheckPass::visit(const NumberNode *node) {
  std::string_view raw = node->raw;
  const Type *ty = ctx->astCtx.Int32Ty;

  if (raw.ends_with('f') || raw.ends_with('F')) {
    ty = ctx->astCtx.Float32Ty;
  } else if (raw.ends_with("ul") || raw.ends_with("UL") ||
             raw.ends_with("lu") || raw.ends_with("LU")) {
    ty = ctx->astCtx.UInt64Ty;
  } else if (raw.ends_with('u') || raw.ends_with('U')) {
    ty = ctx->astCtx.UInt32Ty;
  } else if (raw.ends_with('l') || raw.ends_with('L')) {
    ty = ctx->astCtx.Int64Ty;
  } else if (node->isFloat) {
    ty = ctx->astCtx.Float64Ty;
  } else {
    try {
      uint64_t val = std::stoull(std::string(raw));
      if (val > 9223372036854775807ULL) {
        ty = ctx->astCtx.UInt64Ty;
      } else if (val > 4294967295ULL) {
        ty = ctx->astCtx.Int64Ty;
      } else if (val > 2147483647ULL) {
        ty = ctx->astCtx.UInt32Ty;
      }
    } catch (const std::out_of_range &) {
      return ctx->reportError(node->line, node->column, node->length,
                              "Integer literal out of range");
    }
  }

  node->exprType = ty;
  return ty;
}

SemaResult TypeCheckPass::visit(const BoolNode *node) {
  const Type *ty = ctx->astCtx.BoolTy;
  node->exprType = ty;
  return ty;
}

SemaResult TypeCheckPass::visit(const CharNode *node) {
  const Type *ty = ctx->astCtx.UInt8Ty;
  node->exprType = ty;
  return ty;
}

SemaResult TypeCheckPass::visit(const RuneNode *node) {
  const Type *ty = ctx->astCtx.UInt32Ty;
  node->exprType = ty;
  return ty;
}

SemaResult TypeCheckPass::visit(const StringNode *node) {
  const Type *ty = ctx->astCtx.getPointerType(ctx->astCtx.UInt8Ty);
  node->exprType = ty;
  return ty;
}

SemaResult TypeCheckPass::visit(const StructDeclNode *node) {
  for (const auto *field : node->fields) {
    auto res = dispatch(field);
    if (!res)
      return res;
  }
  return ctx->astCtx.VoidTy;
}

SemaResult TypeCheckPass::visit(const ClassDeclNode *node) {
  for (const auto *field : node->fields) {
    auto res = dispatch(field);
    if (!res)
      return res;
  }
  for (const auto *ctor : node->constructors) {
    auto res = dispatch(ctor);
    if (!res)
      return res;
  }
  if (node->destructor) {
    auto res = dispatch(node->destructor);
    if (!res)
      return res;
  }
  for (const auto *method : node->methods) {
    auto res = dispatch(method);
    if (!res)
      return res;
  }
  return ctx->astCtx.VoidTy;
}

SemaResult TypeCheckPass::visit(const VariableNode *node) {
  auto decls = ctx->lookup(node->name);
  if (decls.empty()) {
    auto thisDecls = ctx->lookup("this");
    if (!thisDecls.empty()) {
      const DeclNode *thisDecl = thisDecls.front();
      if (thisDecl->kind == NodeKind::ParamDecl) {
        const Type *thisTy = static_cast<const ParamDeclNode *>(thisDecl)->type;
        if (thisTy->isPointerType()) {
          const Type *pointee =
              static_cast<const PointerType *>(thisTy)->getPointeeType();
          if (pointee->getKind() == TypeKind::Class) {
            auto clsTy = static_cast<const ClassType *>(pointee);
            if (auto field = clsTy->getField(node->name)) {
              const_cast<VariableNode *>(node)->isField = true;
              const_cast<VariableNode *>(node)->fieldIndex = field->index;
              const_cast<VariableNode *>(node)->parentType = clsTy;
              node->exprType = field->type;
              return field->type;
            }
          }
        }
      }
    }
    return ctx->reportError(node->line, node->column, node->length,
                            "Undefined identifier: '" +
                                std::string(node->name) + "'");
  }

  const Type *ty = nullptr;
  const DeclNode *target = decls.front();

  if (target->kind == NodeKind::VarDecl) {
    ty = static_cast<const VarDeclNode *>(target)->type;
  } else if (target->kind == NodeKind::ParamDecl) {
    ty = static_cast<const ParamDeclNode *>(target)->type;
  } else if (target->kind == NodeKind::StructDecl ||
             target->kind == NodeKind::ClassDecl) {
    return ctx->astCtx.VoidTy;
  } else {
    return ctx->reportError(node->line, node->column, node->length,
                            "Identifier '" + std::string(node->name) +
                                "' is not a variable");
  }

  node->exprType = ty;
  return ty;
}

SemaResult TypeCheckPass::visit(const UnaryOpNode *node) {
  auto exprType = dispatch(node->expr);
  if (!exprType)
    return std::unexpected(ErrorInfo{node->line, node->column, node->length,
                                     "Cascading error in unary op"});

  const Type *resType = nullptr;
  if (node->op == "&") {
    if (node->expr->kind != NodeKind::Variable &&
        node->expr->kind != NodeKind::UnaryOp &&
        node->expr->kind != NodeKind::MemberAccess) {
      return ctx->reportError(node->line, node->column, node->length,
                              "Cannot take address of r-value");
    }
    resType = ctx->astCtx.getPointerType(*exprType);
  } else if (node->op == "*") {
    const Type *unqualExprType = (*exprType)->getUnqualifiedType();
    if (unqualExprType->isPointerType()) {
      resType =
          static_cast<const PointerType *>(unqualExprType)->getPointeeType();
    } else if (unqualExprType->isReferenceType()) {
      resType =
          static_cast<const ReferenceType *>(unqualExprType)->getPointeeType();
    } else {
      return ctx->reportError(node->line, node->column, node->length,
                              "Cannot dereference non-pointer type");
    }
  } else if (node->op == "-" || node->op == "+") {
    if (!(*exprType)->isNumeric()) {
      return ctx->reportError(node->line, node->column, node->length,
                              "Unary operator '" + std::string(node->op) +
                                  "' requires a numeric operand");
    }
    resType = *exprType;
  } else {
    return ctx->reportError(node->line, node->column, node->length,
                            "Unknown unary operator");
  }

  node->exprType = resType;
  return resType;
}

SemaResult TypeCheckPass::visit(const BinaryOpNode *node) {
  auto lhs = dispatch(node->left);
  auto rhs = dispatch(node->right);

  if (!lhs || !rhs)
    return std::unexpected(ErrorInfo{node->line, node->column, node->length,
                                     "Invalid operands for binary operation"});

  if (!canImplicitlyCast(*lhs, *rhs)) {
    return ctx->reportError(node->line, node->column, node->length,
                            "Type mismatch: " + (*lhs)->toString() + " vs " +
                                (*rhs)->toString());
  }

  const Type *res = (*lhs)->isFloat() ? *lhs : *rhs;
  node->exprType = res;
  return res;
}

SemaResult TypeCheckPass::visit(const VarDeclNode *node) {
  const Type *declType = node->type;

  if (declType->isConstQualified() && !node->initializer &&
      !declType->isReferenceType()) {
    ctx->reportError(node->line, node->column, node->length,
                     "Constant variables must be initialized.");
  }

  if (node->initializer) {
    auto initRes = dispatch(node->initializer);
    if (!initRes) {
      return initRes;
    }

    if (declType->isReferenceType()) {
      if (node->initializer->kind != NodeKind::Variable &&
          node->initializer->kind != NodeKind::UnaryOp &&
          node->initializer->kind != NodeKind::FunctionCall &&
          node->initializer->kind != NodeKind::MemberAccess) {
        ctx->reportError(node->line, node->column, node->length,
                         "Cannot bind a non-lvalue to a reference.");
      }
    } else {
      if (!canImplicitlyCast(*initRes, declType)) {
        std::string initTypeStr = *initRes ? (*initRes)->toString() : "unknown";
        ctx->reportError(node->line, node->column, node->length,
                         "Cannot initialize variable of type '" +
                             declType->toString() + "' with type '" +
                             initTypeStr + "'");
      }
    }
  } else if (declType->isReferenceType()) {
    ctx->reportError(node->line, node->column, node->length,
                     "References must be initialized upon declaration.");
  }

  if (ctx->getScopeDepth() > 1) {
    ctx->addDecl(node->varName, node);
  }
  return declType;
}

SemaResult TypeCheckPass::visit(const AssignNode *node) {
  auto lhsType = dispatch(node->target);
  auto rhsType = dispatch(node->value);

  if (!lhsType || !rhsType)
    return std::unexpected(ErrorInfo{node->line, node->column, node->length,
                                     "Cascading error in assignment"});

  if (node->target->kind != NodeKind::Variable &&
      node->target->kind != NodeKind::UnaryOp &&
      node->target->kind != NodeKind::MemberAccess) {
    return ctx->reportError(
        node->target->line, node->target->column, node->target->length,
        "Expression is not assignable (must be an l-value)");
  }

  if ((*lhsType)->isConstQualified() ||
      ((*lhsType)->isReferenceType() &&
       static_cast<const ReferenceType *>(*lhsType)
           ->getPointeeType()
           ->isConstQualified())) {
    return ctx->reportError(node->target->line, node->target->column,
                            node->target->length,
                            "Cannot assign to a constant variable");
  }

  if (!canImplicitlyCast(*rhsType, *lhsType)) {
    return ctx->reportError(node->line, node->column, node->length,
                            "Invalid assignment. Type mismatch.");
  }

  const Type *unqualLhs = (*lhsType)->getUnqualifiedType();
  if (unqualLhs->getKind() == TypeKind::Class) {
    auto *classTy = static_cast<const ClassType *>(unqualLhs);
    if (auto *classDecl =
            static_cast<const ClassDeclNode *>(classTy->getDeclaration())) {
      if (classDecl->destructor) {
        return ctx->reportError(
            node->line, node->column, node->length,
            "Cannot implicitly copy-assign a class with a destructor.");
      }
    }
  }

  node->exprType = *lhsType;
  return *lhsType;
}

SemaResult TypeCheckPass::visit(const BlockNode *node) {
  ScopeGuard guard(*ctx);
  for (const auto &stmt : node->statements) {
    auto res = dispatch(stmt);
    if (!res)
      return res;
  }
  return ctx->astCtx.VoidTy;
}

SemaResult TypeCheckPass::visit(const MemberAccessNode *node) {
  auto objType = dispatch(node->object);
  if (!objType)
    return std::unexpected(ErrorInfo{node->line, node->column, node->length,
                                     "Invalid object in member access"});

  const Type *baseTy = *objType;
  if (baseTy->isPointerType())
    baseTy = static_cast<const PointerType *>(baseTy)->getPointeeType();
  else if (baseTy->isReferenceType())
    baseTy = static_cast<const ReferenceType *>(baseTy)->getPointeeType();

  if (baseTy->getKind() != TypeKind::Struct &&
      baseTy->getKind() != TypeKind::Class) {
    return ctx->reportError(node->line, node->column, node->length,
                            "Member access on non-record type");
  }

  auto recordTy = static_cast<const RecordType *>(baseTy);

  if (auto field = recordTy->getField(node->memberName)) {
    const_cast<MemberAccessNode *>(node)->fieldIndex = field->index;
    node->exprType = field->type;
    return field->type;
  }

  if (baseTy->getKind() == TypeKind::Class) {
    auto clsDeclDecls = ctx->lookup(recordTy->getName());
    if (!clsDeclDecls.empty() &&
        clsDeclDecls.front()->kind == NodeKind::ClassDecl) {
      auto clsDecl = static_cast<const ClassDeclNode *>(clsDeclDecls.front());
      for (const auto *method : clsDecl->methods) {
        if (method->name == node->memberName) {
          const_cast<MemberAccessNode *>(node)->isMethodRef = true;
          const_cast<MemberAccessNode *>(node)->resolvedMethod = method;
          node->exprType = ctx->astCtx.VoidTy;
          return ctx->astCtx.VoidTy;
        }
      }
    }
  }

  return ctx->reportError(node->line, node->column, node->length,
                          "No member named '" + std::string(node->memberName) +
                              "'");
}

SemaResult TypeCheckPass::visit(const ParamDeclNode *node) {
  return node->type;
}

static bool guaranteesReturn(const ASTNode *node) {
  if (!node)
    return false;
  if (node->kind == NodeKind::Return)
    return true;
  if (node->kind == NodeKind::Block) {
    const auto *block = static_cast<const BlockNode *>(node);
    for (const auto *stmt : block->statements) {
      if (guaranteesReturn(stmt))
        return true;
    }
  }
  return false;
}

SemaResult TypeCheckPass::visit(const FunctionDeclNode *node) {
  ctx->setFunctionReturnType(node->returnType);

  ScopeGuard guard(*ctx);
  for (const auto *param : node->params) {
    ctx->addDecl(param->name, param);
  }

  if (node->body) {
    auto bodyRes = dispatch(node->body);
    if (!bodyRes) {
      return bodyRes;
    }

    if (!node->returnType->isVoid() && !guaranteesReturn(node->body)) {
      auto error = ctx->reportError(node->line, node->column, node->length,
                       "Non-void function must explicitly return a value in "
                       "all control paths.");
    }
  }

  return node->returnType;
}

SemaResult TypeCheckPass::visit(const FunctionCallNode *node) {
  std::vector<const Type *> argTypes;
  for (const auto &arg : node->args) {
    auto argType = dispatch(arg);
    if (!argType)
      return std::unexpected(
          ErrorInfo{node->line, node->column, node->length, "Argument error"});
    argTypes.push_back(*argType);
  }

  if (node->target->kind == NodeKind::MemberAccess) {
    auto ma = static_cast<const MemberAccessNode *>(node->target);
    auto maRes = dispatch(ma);
    if (!maRes)
      return maRes;

    if (ma->isMethodRef) {
      const Type *baseTy = ma->object->exprType;
      if (baseTy->isPointerType())
        baseTy = static_cast<const PointerType *>(baseTy)->getPointeeType();
      else if (baseTy->isReferenceType())
        baseTy = static_cast<const ReferenceType *>(baseTy)->getPointeeType();

      auto recordTy = static_cast<const RecordType *>(baseTy);
      auto clsDeclDecls = ctx->lookup(recordTy->getName());

      const ClassDeclNode *clsDecl = nullptr;
      for (auto *d : clsDeclDecls) {
        if (d->kind == NodeKind::ClassDecl) {
          clsDecl = static_cast<const ClassDeclNode *>(d);
          break;
        }
      }

      if (clsDecl) {
        const FunctionDeclNode *bestMatch = nullptr;
        for (const auto *method : clsDecl->methods) {
          if (method->name == ma->memberName) {
            if (method->params.size() - 1 == argTypes.size()) {
              bool match = true;
              for (size_t i = 0; i < argTypes.size(); ++i) {
                if (!canImplicitlyCast(argTypes[i],
                                       method->params[i + 1]->type)) {
                  match = false;
                  break;
                }
              }
              if (match) {
                bestMatch = method;
                break;
              }
            }
          }
        }
        if (bestMatch) {
          const_cast<MemberAccessNode *>(ma)->resolvedMethod = bestMatch;
          const_cast<FunctionCallNode *>(node)->resolvedFunc = bestMatch;
          node->exprType = bestMatch->returnType;
          return node->exprType;
        }
      }
      return ctx->reportError(node->line, node->column, node->length,
                              "No matching method overload found.");
    }
  }

  if (node->target->kind == NodeKind::Variable) {
    std::string_view name =
        static_cast<const VariableNode *>(node->target)->name;
    auto decls = ctx->lookup(name);

    if (!decls.empty()) {
      const FunctionDeclNode *bestMatch = nullptr;
      bool isConstructorCall = false;

      for (auto targetDecl : decls) {
        if (targetDecl->kind == NodeKind::FunctionDecl) {
          auto fDecl = static_cast<const FunctionDeclNode *>(targetDecl);
          size_t expectedArgs = fDecl->isMethod ? (fDecl->params.size() - 1)
                                                : fDecl->params.size();

          if (expectedArgs == argTypes.size()) {
            bool match = true;
            size_t paramOffset = fDecl->isMethod ? 1 : 0;

            for (size_t i = 0; i < argTypes.size(); ++i) {
              if (!canImplicitlyCast(argTypes[i],
                                     fDecl->params[i + paramOffset]->type)) {
                match = false;
                break;
              }
            }

            if (match) {
              bestMatch = fDecl;
              /* Resolve strict constructor initialization constraints */
              if (fDecl->isMethod &&
                  ctx->astCtx.getRecordType(name) != nullptr) {
                isConstructorCall = true;
              }
              break;
            }
          }
        }
      }

      if (bestMatch) {
        const_cast<FunctionCallNode *>(node)->resolvedFunc = bestMatch;
        if (isConstructorCall) {
          node->exprType = ctx->astCtx.getRecordType(name);
        } else {
          node->exprType = bestMatch->returnType;
        }
        return node->exprType;
      }
    }
  }

  return ctx->reportError(
      node->line, node->column, node->length,
      "Invalid function call target or ambiguous overload.");
}

SemaResult TypeCheckPass::visit(const CastNode *node) {
  auto srcType = dispatch(node->expr);
  const Type *destType = node->targetType;

  if (!srcType)
    return std::unexpected(ErrorInfo{node->line, node->column, node->length,
                                     "Cascading error in cast"});

  if (!(*srcType)->isNumeric() || !destType->isNumeric()) {
    return ctx->reportError(node->line, node->column, node->length,
                            "Casts are currently restricted to numeric types");
  }

  node->exprType = destType;
  return destType;
}

SemaResult TypeCheckPass::visit(const ReturnNode *node) {
  const Type *expectedRet = ctx->getFunctionReturnType();

  if (!node->value) {
    if (!expectedRet->isVoid()) {
      return ctx->reportError(
          node->line, node->column, node->length,
          "Non-void function must return a value of type '" +
              expectedRet->toString() + "'");
    }
    return ctx->astCtx.VoidTy;
  }

  auto valType = dispatch(node->value);
  if (!valType)
    return std::unexpected(ErrorInfo{node->line, node->column, node->length,
                                     "Cascading error in return expression"});

  if (expectedRet->isReferenceType()) {
    if (node->value->kind != NodeKind::Variable &&
        node->value->kind != NodeKind::UnaryOp &&
        node->value->kind != NodeKind::FunctionCall &&
        node->value->kind != NodeKind::MemberAccess) {
      return ctx->reportError(node->line, node->column, node->length,
                              "Cannot return a non-lvalue as a reference.");
    }
    return *valType;
  }

  if (expectedRet->isVoid() || !canImplicitlyCast(*valType, expectedRet)) {
    return ctx->reportError(node->line, node->column, node->length,
                            "Return type mismatch: expected '" +
                                expectedRet->toString() + "', got '" +
                                (*valType)->toString() + "'");
  }

  return *valType;
}

SemaResult TypeCheckPass::visit(const ModuleNode *node) {
  if (visitedModules.contains(node))
    return ctx->astCtx.VoidTy;
  visitedModules.insert(node);

  for (const auto *imp : node->importedModules) {
    auto res = dispatch(imp);
    if (!res)
      return res;
  }

  auto prevFile = ctx->currentFile;
  ctx->setCurrentFile(node->filePath);

  for (const auto &stmt : node->statements) {
    auto res = dispatch(stmt);
    if (!res)
      return res;
  }

  ctx->setCurrentFile(prevFile);
  return ctx->astCtx.VoidTy;
}

} // namespace utopia