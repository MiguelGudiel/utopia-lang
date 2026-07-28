#include "utopia/Sema/Sema.hpp"
#include "utopia/CodeGen/Mangler.hpp"
#include "utopia/Sema/EffectAnalyzer.hpp"

#include "utopia/Common/Logger.hpp"

namespace utopia {

SemaPipeline::SemaPipeline() {
  passes.push_back(std::make_unique<DeclCollectorPass>());
  passes.push_back(std::make_unique<TypeCheckPass>());
}

bool SemaPipeline::run(const ModuleNode *module, SemaContext &ctx) {
  Logger::debug("[Sema Debug] Initiating semantic analysis pipeline...");

  for (auto &pass : passes) {
    Logger::debug("[Sema Debug] Executing pass: " +
                  std::string(pass->getName()));

    try {
      if (!pass->run(module, ctx)) {
        Logger::debug("[Sema Debug] Pass aborted due to unexpected failure: " +
                      std::string(pass->getName()));
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
      Logger::debug(
          "[Sema Debug] Semantic integrity compromised during pass: " +
          std::string(pass->getName()));
      return false;
    }

    Logger::debug("[Sema Debug] Pass completed successfully: " +
                  std::string(pass->getName()));
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
        stmt->kind == NodeKind::AnnotationDecl ||
        stmt->kind == NodeKind::TypedefDecl ||
        stmt->kind == NodeKind::EnumDecl) {
      dispatch(stmt);
    }
  }

  ctx->setCurrentFile(prevFile);
}

void DeclCollectorPass::visit(const TypedefDeclNode *node) {
  ctx->addDecl(node->aliasName, node);
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
  bool hasErrors = false;
  for (const auto *field : node->fields) {
    auto res = dispatch(field);
    if (!res)
      hasErrors = true;
  }
  if (node->constructor) {
    auto res = dispatch(node->constructor);
    if (!res)
      hasErrors = true;
  }

  if (hasErrors) {
    return std::unexpected(ErrorInfo{node->line, node->column, node->length,
                                     "Errors in annotation declaration"});
  }
  return ctx->astCtx.VoidTy;
}

SemaResult TypeCheckPass::visit(const AnnotationNode *node) {
  bool hasErrors = false;
  for (const auto *arg : node->args) {
    auto res = dispatch(arg);
    if (!res)
      hasErrors = true;
  }

  if (hasErrors) {
    return std::unexpected(ErrorInfo{node->line, node->column, node->length,
                                     "Errors in annotation arguments"});
  }
  return ctx->astCtx.VoidTy;
}

void DeclCollectorPass::visit(const FunctionDeclNode *node) {
  if (node->isExtern) {
    for (const auto *ann : node->annotations) {
      if (ann->name == "extern") {
        if (ann->args.size() == 1 && ann->args[0]->kind == NodeKind::String) {
          node->externAlias =
              static_cast<const StringNode *>(ann->args[0])->value;
        } else {
          SemaResult err = ctx->reportError(
              ann->line, ann->column, ann->length,
              "The @extern annotation requires exactly one string "
              "literal argument.");
        }
      }
    }

    if (node->isMethod && node->externAlias.empty()) {
      SemaResult err =
          ctx->reportError(node->line, node->column, node->length,
                           "Extern methods must specify an @extern annotation "
                           "reflecting the target C function mapping.");
    }

    if (!node->externAlias.empty()) {
      const_cast<FunctionDeclNode *>(node)->mangledName =
          std::string(node->externAlias);
    } else {
      const_cast<FunctionDeclNode *>(node)->mangledName =
          std::string(node->name);
    }
  } else if (node->mangledName.empty()) {
    const_cast<FunctionDeclNode *>(node)->mangledName = Mangler::mangle(node);
  }
  ctx->addDecl(node->name, node);
}

void DeclCollectorPass::visit(const IfNode *node) {
  dispatch(node->condition);
  dispatch(node->thenBlock);
  if (node->elseBlock)
    dispatch(node->elseBlock);
}

void DeclCollectorPass::visit(const ForNode *node) {
  if (node->initStatement)
    dispatch(node->initStatement);
  if (node->condition)
    dispatch(node->condition);
  if (node->increment)
    dispatch(node->increment);
  dispatch(node->body);
}

void DeclCollectorPass::visit(const WhileNode *node) {
  dispatch(node->condition);
  dispatch(node->body);
}

void DeclCollectorPass::visit(const VarDeclNode *node) {
  ctx->addDecl(node->varName, node);
}

void DeclCollectorPass::visit(const StructDeclNode *node) {
  ctx->addDecl(node->name, node);
  const_cast<StructDeclNode *>(node)->recordType =
      ctx->astCtx.getRecordType(node->name);

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
    if (method->isExtern) {
      for (const auto *ann : method->annotations) {
        if (ann->name == "extern") {
          if (ann->args.size() == 1 && ann->args[0]->kind == NodeKind::String) {
            const_cast<FunctionDeclNode *>(method)->externAlias =
                static_cast<const StringNode *>(ann->args[0])->value;
          } else {
            SemaResult err =
                ctx->reportError(ann->line, ann->column, ann->length,
                                 "The @extern annotation requires exactly one "
                                 "string literal argument.");
          }
        }
      }

      if (method->externAlias.empty()) {
        SemaResult err = ctx->reportError(
            method->line, method->column, method->length,
            "Extern methods must specify an @extern annotation "
            "reflecting the target C function mapping.");
      } else {
        const_cast<FunctionDeclNode *>(method)->mangledName =
            std::string(method->externAlias);
      }
    } else {
      const_cast<FunctionDeclNode *>(method)->mangledName =
          Mangler::mangle(method, std::string(node->name));
    }
  }
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
    if (method->isExtern) {
      for (const auto *ann : method->annotations) {
        if (ann->name == "extern") {
          if (ann->args.size() == 1 && ann->args[0]->kind == NodeKind::String) {
            const_cast<FunctionDeclNode *>(method)->externAlias =
                static_cast<const StringNode *>(ann->args[0])->value;
          } else {
            SemaResult err =
                ctx->reportError(ann->line, ann->column, ann->length,
                                 "The @extern annotation requires exactly one "
                                 "string literal argument.");
          }
        }
      }

      if (method->externAlias.empty()) {
        SemaResult err = ctx->reportError(
            method->line, method->column, method->length,
            "Extern methods must specify an @extern annotation "
            "reflecting the target C function mapping.");
      } else {
        const_cast<FunctionDeclNode *>(method)->mangledName =
            std::string(method->externAlias);
      }
    } else {
      const_cast<FunctionDeclNode *>(method)->mangledName =
          Mangler::mangle(method, std::string(node->name));
    }
  }
}

void DeclCollectorPass::visit(const EnumDeclNode *node) {
  ctx->addDecl(node->name, node);
  const_cast<EnumDeclNode *>(node)->enumType =
      ctx->astCtx.getEnumType(node->name, node->underlyingType);
  const_cast<EnumType *>(node->enumType)->setDeclaration(node);
}

bool TypeCheckPass::run(const ModuleNode *module, SemaContext &context) {
  ctx = &context;
  auto result = dispatch(module);

  if (!result && !ctx->hasErrors()) {
    SemaResult err =
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
  bool hasErrors = false;

  /* Validate structural decorators */
  for (const auto *ann : node->annotations) {
    if (ann->name == "align") {
      if (ann->args.size() != 1 || ann->args[0]->kind != NodeKind::Number ||
          static_cast<const NumberNode *>(ann->args[0])->isFloat) {
        auto err = ctx->reportError(
            ann->line, ann->column, ann->length,
            "The @align annotation requires a single integer constant.");
        hasErrors = true;
      } else {
        uint64_t alignVal = std::stoull(
            std::string(static_cast<const NumberNode *>(ann->args[0])->raw));
        if (alignVal == 0 || (alignVal & (alignVal - 1)) != 0) {
          auto err = ctx->reportError(ann->line, ann->column, ann->length,
                                      "Alignment must be a power of 2.");
          hasErrors = true;
        }
      }
    } else if (ann->name == "packed") {
      if (!ann->args.empty()) {
        auto err =
            ctx->reportError(ann->line, ann->column, ann->length,
                             "The @packed annotation does not take arguments.");
        hasErrors = true;
      }
    }
  }

  for (const auto *field : node->fields) {
    auto res = dispatch(field);
    if (!res)
      hasErrors = true;
  }
  for (const auto *ctor : node->constructors) {
    auto res = dispatch(ctor);
    if (!res)
      hasErrors = true;
  }
  if (node->destructor) {
    auto res = dispatch(node->destructor);
    if (!res)
      hasErrors = true;
  }
  for (const auto *method : node->methods) {
    auto res = dispatch(method);
    if (!res)
      hasErrors = true;
  }

  if (hasErrors) {
    return std::unexpected(ErrorInfo{node->line, node->column, node->length,
                                     "Errors in struct declaration"});
  }
  return ctx->astCtx.VoidTy;
}

SemaResult TypeCheckPass::visit(const ClassDeclNode *node) {
  bool hasErrors = false;

  /* Validate structural decorators */
  for (const auto *ann : node->annotations) {
    if (ann->name == "align") {
      if (ann->args.size() != 1 || ann->args[0]->kind != NodeKind::Number ||
          static_cast<const NumberNode *>(ann->args[0])->isFloat) {
        auto err = ctx->reportError(
            ann->line, ann->column, ann->length,
            "The @align annotation requires a single integer constant.");
        hasErrors = true;
      } else {
        uint64_t alignVal = std::stoull(
            std::string(static_cast<const NumberNode *>(ann->args[0])->raw));
        if (alignVal == 0 || (alignVal & (alignVal - 1)) != 0) {
          auto err = ctx->reportError(ann->line, ann->column, ann->length,
                                      "Alignment must be a power of 2.");
          hasErrors = true;
        }
      }
    } else if (ann->name == "packed") {
      if (!ann->args.empty()) {
        auto err =
            ctx->reportError(ann->line, ann->column, ann->length,
                             "The @packed annotation does not take arguments.");
        hasErrors = true;
      }
    }
  }

  for (const auto *field : node->fields) {
    auto res = dispatch(field);
    if (!res)
      hasErrors = true;
  }
  for (const auto *ctor : node->constructors) {
    auto res = dispatch(ctor);
    if (!res)
      hasErrors = true;
  }
  if (node->destructor) {
    auto res = dispatch(node->destructor);
    if (!res)
      hasErrors = true;
  }
  for (const auto *method : node->methods) {
    auto res = dispatch(method);
    if (!res)
      hasErrors = true;
  }

  if (hasErrors) {
    return std::unexpected(ErrorInfo{node->line, node->column, node->length,
                                     "Errors in class declaration"});
  }
  return ctx->astCtx.VoidTy;
}

SemaResult TypeCheckPass::visit(const EnumDeclNode *node) {
  int64_t nextValue = 0;
  bool hasErrors = false;

  for (auto *mem : node->members) {
    if (mem->initializer) {
      auto res = dispatch(mem->initializer);
      if (!res) {
        hasErrors = true;
        continue;
      }

      int64_t val = 0;
      auto *init = mem->initializer;

      // Basic compile-time evaluation for enum values
      if (init->kind == NodeKind::Number) {
        val =
            std::stoll(std::string(static_cast<const NumberNode *>(init)->raw));
      } else if (init->kind == NodeKind::UnaryOp) {
        auto uop = static_cast<const UnaryOpNode *>(init);
        if (uop->op == "-" && uop->expr->kind == NodeKind::Number) {
          val = -std::stoll(
              std::string(static_cast<const NumberNode *>(uop->expr)->raw));
        } else {
          ctx->reportError(
              init->line, init->column, init->length,
              "Enum member initializers must be simple integer constants.");
          hasErrors = true;
        }
      } else {
        ctx->reportError(
            init->line, init->column, init->length,
            "Enum member initializers must be simple integer constants.");
        hasErrors = true;
      }
      mem->evaluatedValue = val;
      nextValue = val + 1;
    } else {
      mem->evaluatedValue = nextValue++;
    }
  }

  if (hasErrors) {
    return std::unexpected(ErrorInfo{node->line, node->column, node->length,
                                     "Errors in enum declaration"});
  }
  return ctx->astCtx.VoidTy;
}

SemaResult TypeCheckPass::visit(const EnumMemberNode *node) {
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

  /* Unwrap typedefs recursively to their underlying entities */
  while (target && target->kind == NodeKind::TypedefDecl) {
    auto td = static_cast<const TypedefDeclNode *>(target);
    if (!td->targetEntityName.empty()) {
      auto aliased = ctx->lookup(td->targetEntityName);
      if (!aliased.empty()) {
        target = aliased.front();
      } else {
        break;
      }
    } else {
      return ctx->reportError(node->line, node->column, node->length,
                              "Type alias '" + std::string(node->name) +
                                  "' cannot be used as an expression.");
    }
  }

  const_cast<VariableNode *>(node)->resolvedDecl = target;

  if (target->kind == NodeKind::VarDecl) {
    ty = static_cast<const VarDeclNode *>(target)->type;
  } else if (target->kind == NodeKind::ParamDecl) {
    ty = static_cast<const ParamDeclNode *>(target)->type;
  } else if (target->kind == NodeKind::FunctionDecl) {
    auto fDecl = static_cast<const FunctionDeclNode *>(target);
    std::vector<const Type *> pTypes;
    for (auto *p : fDecl->params)
      pTypes.push_back(p->type);

    /* Variables mapping directly to a function identifier decay to a function
     * pointer */
    const Type *funcTy = ctx->astCtx.getFunctionType(
        fDecl->returnType, ctx->astCtx.copyArray<const Type *>(pTypes));
    ty = ctx->astCtx.getPointerType(funcTy);
  } else if (target->kind == NodeKind::StructDecl ||
             target->kind == NodeKind::ClassDecl ||
             target->kind == NodeKind::EnumDecl) {
    return ctx->astCtx.VoidTy;
  } else {
    return ctx->reportError(node->line, node->column, node->length,
                            "Identifier '" + std::string(node->name) +
                                "' cannot be evaluated as an expression");
  }

  node->exprType = ty;
  return ty;
}

SemaResult TypeCheckPass::visit(const IfNode *node) {
  auto condRes = dispatch(node->condition);
  if (!condRes) {
    return std::unexpected(ErrorInfo{node->line, node->column, node->length,
                                     "Cascading error in if condition"});
  }

  if (!canImplicitlyCast(*condRes, ctx->astCtx.BoolTy)) {
    return ctx->reportError(node->condition->line, node->condition->column,
                            node->condition->length,
                            "Condition must evaluate to a boolean type.");
  }

  auto thenRes = dispatch(node->thenBlock);
  if (!thenRes)
    return thenRes;

  if (node->elseBlock) {
    auto elseRes = dispatch(node->elseBlock);
    if (!elseRes)
      return elseRes;
  }

  return ctx->astCtx.VoidTy;
}

SemaResult TypeCheckPass::visit(const ForNode *node) {
  ScopeGuard guard(*ctx);

  if (node->initStatement) {
    auto initRes = dispatch(node->initStatement);
    if (!initRes)
      return std::unexpected(initRes.error());
  }

  if (node->condition) {
    auto condRes = dispatch(node->condition);
    if (!condRes)
      return std::unexpected(condRes.error());
    if (!canImplicitlyCast(*condRes, ctx->astCtx.BoolTy)) {
      return ctx->reportError(
          node->condition->line, node->condition->column,
          node->condition->length,
          "For loop condition must evaluate to a boolean type.");
    }
  }

  if (node->increment) {
    auto incRes = dispatch(node->increment);
    if (!incRes)
      return std::unexpected(incRes.error());
  }

  auto bodyRes = dispatch(node->body);
  if (!bodyRes)
    return bodyRes;

  return ctx->astCtx.VoidTy;
}

SemaResult TypeCheckPass::visit(const WhileNode *node) {
  auto condRes = dispatch(node->condition);
  if (!condRes)
    return std::unexpected(condRes.error());

  if (!canImplicitlyCast(*condRes, ctx->astCtx.BoolTy)) {
    return ctx->reportError(
        node->condition->line, node->condition->column, node->condition->length,
        "While loop condition must evaluate to a boolean type.");
  }

  auto bodyRes = dispatch(node->body);
  if (!bodyRes)
    return bodyRes;

  return ctx->astCtx.VoidTy;
}

SemaResult TypeCheckPass::visit(const UnaryOpNode *node) {
  auto exprType = dispatch(node->expr);
  if (!exprType)
    return std::unexpected(ErrorInfo{node->line, node->column, node->length,
                                     "Cascading error in unary op"});

  const Type *resType = nullptr;
  if (node->op == "!") {
    if (!canImplicitlyCast(*exprType, ctx->astCtx.BoolTy)) {
      return ctx->reportError(
          node->line, node->column, node->length,
          "Logical NOT operator requires a boolean operand.");
    }
    resType = ctx->astCtx.BoolTy;
  } else if (node->op == "&") {
    /* Allow array subscripts to be addressable as l-values */
    if (node->expr->kind != NodeKind::Variable &&
        node->expr->kind != NodeKind::UnaryOp &&
        node->expr->kind != NodeKind::MemberAccess &&
        node->expr->kind != NodeKind::ArraySubscript) {
      return ctx->reportError(node->line, node->column, node->length,
                              "Cannot take address of r-value");
    }
    resType = ctx->astCtx.getPointerType(*exprType);
  } else if (node->op == "*") {
    const Type *unqualExprType = (*exprType)->getUnqualifiedType();
    if (unqualExprType->isPointerType()) {
      resType =
          static_cast<const PointerType *>(unqualExprType)->getPointeeType();

      // Explicitly prevent dereferencing void*
      if (resType->isVoid()) {
        return ctx->reportError(node->line, node->column, node->length,
                                "Cannot dereference a void pointer");
      }
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

  if (node->op == "&&" || node->op == "||") {
    if (!canImplicitlyCast(*lhs, ctx->astCtx.BoolTy) ||
        !canImplicitlyCast(*rhs, ctx->astCtx.BoolTy)) {
      return ctx->reportError(node->line, node->column, node->length,
                              "Logical operations require boolean operands.");
    }
    node->exprType = ctx->astCtx.BoolTy;
    return ctx->astCtx.BoolTy;
  }

  if (node->op == "==" || node->op == "!=" || node->op == "<" ||
      node->op == ">" || node->op == "<=" || node->op == ">=") {
    if (!canImplicitlyCast(*lhs, *rhs)) {
      return ctx->reportError(node->line, node->column, node->length,
                              "Type mismatch in relational operation.");
    }

    if (node->op != "==" && node->op != "!=") {
      if (!(*lhs)->isNumeric() || !(*rhs)->isNumeric()) {
        return ctx->reportError(
            node->line, node->column, node->length,
            "Relational inequalities require numeric operands.");
      }
    }

    node->exprType = ctx->astCtx.BoolTy;
    return ctx->astCtx.BoolTy;
  }

  if (!(*lhs)->isNumeric() || !(*rhs)->isNumeric()) {
    return ctx->reportError(node->line, node->column, node->length,
                            "Binary arithmetic operations are currently "
                            "restricted to numeric types.");
  }

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

  // Prevent variables of type 'void'
  if (declType->isVoid()) {
    return ctx->reportError(node->line, node->column, node->length,
                            "Variables cannot be of type 'void'");
  }

  for (const auto *ann : node->annotations) {
    if (ann->name == "align") {
      if (ann->args.size() != 1 || ann->args[0]->kind != NodeKind::Number ||
          static_cast<const NumberNode *>(ann->args[0])->isFloat) {
        SemaResult err = ctx->reportError(
            ann->line, ann->column, ann->length,
            "The @align annotation requires a single integer constant.");
      } else {
        uint64_t alignVal = std::stoull(
            std::string(static_cast<const NumberNode *>(ann->args[0])->raw));
        if (alignVal == 0 || (alignVal & (alignVal - 1)) != 0) {
          SemaResult err = ctx->reportError(ann->line, ann->column, ann->length,
                                            "Alignment must be a power of 2.");
        }
      }
    } else if (ann->name == "packed") {
      SemaResult err =
          ctx->reportError(ann->line, ann->column, ann->length,
                           "The @packed annotation can only be applied to "
                           "record declarations (struct/class).");
    }
  }

  /* Mark variables defined at module scope to enable accurate side-effect
   * analysis */
  if (ctx->getScopeDepth() == 1) {
    const_cast<VarDeclNode *>(node)->isGlobal = true;
  }

  if (declType->isConstQualified() && !node->initializer &&
      !declType->isReferenceType()) {
    SemaResult err =
        ctx->reportError(node->line, node->column, node->length,
                         "Constant variables must be initialized.");
  }

  if (node->initializer) {
    auto initRes = dispatch(node->initializer);
    if (!initRes) {
      return initRes;
    }

    if (declType->isReferenceType()) {
      /* Allow binding references to array subscript elements */
      if (node->initializer->kind != NodeKind::Variable &&
          node->initializer->kind != NodeKind::UnaryOp &&
          node->initializer->kind != NodeKind::FunctionCall &&
          node->initializer->kind != NodeKind::MemberAccess &&
          node->initializer->kind != NodeKind::ArraySubscript) {
        SemaResult err =
            ctx->reportError(node->line, node->column, node->length,
                             "Cannot bind a non-lvalue to a reference.");
      }
    } else {
      if (!canImplicitlyCast(*initRes, declType)) {
        std::string initTypeStr = *initRes ? (*initRes)->toString() : "unknown";
        SemaResult err = ctx->reportError(
            node->line, node->column, node->length,
            "Cannot initialize variable of type '" + declType->toString() +
                "' with type '" + initTypeStr + "'");
      }
    }
  } else if (declType->isReferenceType()) {
    SemaResult err =
        ctx->reportError(node->line, node->column, node->length,
                         "References must be initialized upon declaration.");
  }

  if (ctx->getScopeDepth() > 1) {
    ctx->addDecl(node->varName, node);
  }
  return declType;
}

SemaResult TypeCheckPass::visit(const NullNode *node) {
  const Type *ty = ctx->astCtx.getPointerType(ctx->astCtx.VoidTy);
  node->exprType = ty;
  return ty;
}

SemaResult TypeCheckPass::visit(const AssignNode *node) {
  auto lhsType = dispatch(node->target);
  auto rhsType = dispatch(node->value);

  if (!lhsType || !rhsType)
    return std::unexpected(ErrorInfo{node->line, node->column, node->length,
                                     "Cascading error in assignment"});

  /* Allow array subscripts as valid l-values for assignment targets */
  if (node->target->kind != NodeKind::Variable &&
      node->target->kind != NodeKind::UnaryOp &&
      node->target->kind != NodeKind::MemberAccess &&
      node->target->kind != NodeKind::ArraySubscript) {
    return ctx->reportError(
        node->target->line, node->target->column, node->target->length,
        "Expression is not assignable (must be an l-value)");
  }

  if (node->target->kind == NodeKind::MemberAccess &&
      static_cast<const MemberAccessNode *>(node->target)->isEnumMember) {
    return ctx->reportError(node->target->line, node->target->column,
                            node->target->length,
                            "Cannot assign to an enum member");
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
  if (unqualLhs->getKind() == TypeKind::Class ||
      unqualLhs->getKind() == TypeKind::Struct) {
    auto *recTy = static_cast<const RecordType *>(unqualLhs);
    if (auto *decl = recTy->getDeclaration()) {
      const FunctionDeclNode *dtor = nullptr;
      if (decl->kind == NodeKind::ClassDecl)
        dtor = static_cast<const ClassDeclNode *>(decl)->destructor;
      else if (decl->kind == NodeKind::StructDecl)
        dtor = static_cast<const StructDeclNode *>(decl)->destructor;

      if (dtor && !dtor->isImplicit) {
        return ctx->reportError(
            node->line, node->column, node->length,
            "Cannot implicitly copy-assign a record with a custom destructor.");
      }
    }
  }

  node->exprType = *lhsType;
  return *lhsType;
}

SemaResult TypeCheckPass::visit(const BlockNode *node) {
  ScopeGuard guard(*ctx);
  bool hasErrors = false;
  for (const auto &stmt : node->statements) {
    auto res = dispatch(stmt);
    if (!res)
      hasErrors = true;
  }

  if (hasErrors) {
    return std::unexpected(ErrorInfo{node->line, node->column, node->length,
                                     "Errors in block statements"});
  }
  return ctx->astCtx.VoidTy;
}

SemaResult TypeCheckPass::visit(const MemberAccessNode *node) {
  auto objType = dispatch(node->object);
  if (!objType)
    return std::unexpected(ErrorInfo{node->line, node->column, node->length,
                                     "Invalid object in member access"});

  if (node->object->kind == NodeKind::Variable) {
    auto varNode = static_cast<const VariableNode *>(node->object);
    if (varNode->resolvedDecl &&
        varNode->resolvedDecl->kind == NodeKind::EnumDecl) {
      auto enumDecl = static_cast<const EnumDeclNode *>(varNode->resolvedDecl);
      for (auto *mem : enumDecl->members) {
        if (mem->name == node->memberName) {
          const_cast<MemberAccessNode *>(node)->isEnumMember = true;
          const_cast<MemberAccessNode *>(node)->enumMember = mem;
          node->exprType = enumDecl->enumType;
          return node->exprType;
        }
      }
      return ctx->reportError(node->line, node->column, node->length,
                              "Enum '" + std::string(enumDecl->name) +
                                  "' does not contain member '" +
                                  std::string(node->memberName) + "'");
    }
  }

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

  if (baseTy->getKind() == TypeKind::Class ||
      baseTy->getKind() == TypeKind::Struct) {
    auto recordDecls = ctx->lookup(recordTy->getName());
    if (!recordDecls.empty()) {
      const DeclNode *recDecl = nullptr;
      for (auto *d : recordDecls) {
        if (d->kind == NodeKind::ClassDecl || d->kind == NodeKind::StructDecl) {
          recDecl = d;
          break;
        }
      }

      if (recDecl) {
        llvm::ArrayRef<FunctionDeclNode *> methods;
        if (recDecl->kind == NodeKind::ClassDecl)
          methods = static_cast<const ClassDeclNode *>(recDecl)->methods;
        else
          methods = static_cast<const StructDeclNode *>(recDecl)->methods;

        for (const auto *method : methods) {
          if (method->name == node->memberName) {
            const_cast<MemberAccessNode *>(node)->isMethodRef = true;
            const_cast<MemberAccessNode *>(node)->resolvedMethod = method;
            node->exprType = ctx->astCtx.VoidTy;
            return ctx->astCtx.VoidTy;
          }
        }
      }
    }
  }

  return ctx->reportError(node->line, node->column, node->length,
                          "No member named '" + std::string(node->memberName) +
                              "'");
}

SemaResult TypeCheckPass::visit(const ParamDeclNode *node) {
  // Prevent parameters of type 'void'
  if (node->type->isVoid()) {
    return ctx->reportError(node->line, node->column, node->length,
                            "Parameters cannot be of type 'void'");
  }
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
  bool hasErrors = false;

  for (const auto *param : node->params) {
    if (param->defaultValue) {
      auto defRes = dispatch(param->defaultValue);
      if (!defRes) {
        hasErrors = true;
      } else if (!canImplicitlyCast(*defRes, param->type)) {
        SemaResult err =
            ctx->reportError(param->line, param->column, param->length,
                             "Default value type mismatch for parameter '" +
                                 std::string(param->name) + "'.");
        hasErrors = true;
      }
    }
    ctx->addDecl(param->name, param);
  }

  if (node->body) {
    auto bodyRes = dispatch(node->body);
    if (!bodyRes) {
      hasErrors = true;
    }

    if (!node->returnType->isVoid() && !guaranteesReturn(node->body)) {
      SemaResult err =
          ctx->reportError(node->line, node->column, node->length,
                           "Non-void function must explicitly return "
                           "a value in all control paths.");
      hasErrors = true;
    }

    EffectAnalyzer ea;
    ea.dispatch(node->body);

    node->isReadNone = !ea.readsMem && !ea.writesMem;
    node->isReadOnly = ea.readsMem && !ea.writesMem;
    node->isNoFree = !ea.freesMem;
    node->isNoSync = !ea.hasSync;
    node->isWillReturn = !ea.potentiallyInfinite;
    node->isMustProgress = true;
  }

  if (hasErrors) {
    return std::unexpected(ErrorInfo{node->line, node->column, node->length,
                                     "Errors in function declaration"});
  }

  return node->returnType;
}

SemaResult TypeCheckPass::visit(const TypedefDeclNode *node) {
  if (!node->targetEntityName.empty()) {
    auto decls = ctx->lookup(node->targetEntityName);
    if (decls.empty()) {
      return ctx->reportError(node->line, node->column, node->length,
                              "Undefined identifier in typedef: '" +
                                  std::string(node->targetEntityName) + "'");
    }

    const DeclNode *target = decls.front();

    /* Unwrap target if it's another typedef */
    while (target && target->kind == NodeKind::TypedefDecl) {
      auto td = static_cast<const TypedefDeclNode *>(target);
      if (!td->targetEntityName.empty()) {
        auto aliased = ctx->lookup(td->targetEntityName);
        if (!aliased.empty()) {
          target = aliased.front();
        } else {
          break;
        }
      } else {
        break;
      }
    }

    if (target->kind == NodeKind::FunctionDecl) {
      auto fDecl = static_cast<const FunctionDeclNode *>(target);
      std::vector<const Type *> pTypes;
      for (auto *p : fDecl->params)
        pTypes.push_back(p->type);

      const Type *funcTy = ctx->astCtx.getFunctionType(
          fDecl->returnType, ctx->astCtx.copyArray<const Type *>(pTypes));
      const Type *ptrFuncTy = ctx->astCtx.getPointerType(funcTy);

      node->aliasType->setTarget(ptrFuncTy);
      node->targetType = ptrFuncTy;
    } else {
      return ctx->reportError(
          node->line, node->column, node->length,
          "Typedef target entity must be a function declaration.");
    }
  }
  return ctx->astCtx.VoidTy;
}

SemaResult TypeCheckPass::visit(const FunctionCallNode *node) {
  std::vector<const Type *> argTypes;
  bool hasErrors = false;

  for (const auto &arg : node->args) {
    auto argType = dispatch(arg);
    if (!argType) {
      hasErrors = true;
    } else {
      argTypes.push_back(*argType);
    }
  }

  // Prevent subsequent checks if evaluating any argument fails
  if (hasErrors) {
    return std::unexpected(ErrorInfo{node->line, node->column, node->length,
                                     "Argument evaluation failed."});
  }

  auto checkMatch =
      [&](const FunctionDeclNode *fDecl) -> std::vector<std::string> {
    size_t paramOffset = (fDecl->isMethod && !fDecl->isExtern) ? 1 : 0;
    size_t expectedParams = fDecl->params.size() - paramOffset;

    std::vector<ExprNode *> resolvedArgs(expectedParams, nullptr);
    std::vector<const Type *> resolvedTypes(expectedParams, nullptr);

    size_t posArgCount = 0;
    std::unordered_set<std::string_view> providedNamedArgs;
    std::vector<std::string> errors;

    for (size_t i = 0; i < node->args.size(); ++i) {
      if (node->argNames[i].empty()) {
        if (posArgCount >= expectedParams) {
          if (fDecl->isVariadic) {
            resolvedArgs.push_back(node->args[i]);
            resolvedTypes.push_back(argTypes[i]);
            posArgCount++;
            continue;
          }
          errors.push_back("Too many arguments provided.");
          return errors;
        }
        if (fDecl->params[paramOffset + posArgCount]->isNamed) {
          errors.push_back(
              "Positional argument provided for named parameter '" +
              std::string(fDecl->params[paramOffset + posArgCount]->name) +
              "'.");
          return errors;
        }
        resolvedArgs[posArgCount] = node->args[i];
        resolvedTypes[posArgCount] = argTypes[i];
        posArgCount++;
      } else {
        auto name = node->argNames[i];
        if (providedNamedArgs.contains(name)) {
          errors.push_back("Duplicate named argument '" + std::string(name) +
                           "'.");
          continue;
        }
        providedNamedArgs.insert(name);

        bool found = false;
        for (size_t p = 0; p < expectedParams; ++p) {
          if (fDecl->params[paramOffset + p]->name == name) {
            if (!fDecl->params[paramOffset + p]->isNamed) {
              errors.push_back("Parameter '" + std::string(name) +
                               "' cannot be passed as a named argument.");
            }
            resolvedArgs[p] = node->args[i];
            resolvedTypes[p] = argTypes[i];
            found = true;
            break;
          }
        }
        if (!found) {
          errors.push_back("No such named parameter '" + std::string(name) +
                           "'.");
        }
      }
    }

    for (size_t p = 0; p < expectedParams; ++p) {
      if (!resolvedArgs[p]) {
        if (fDecl->params[paramOffset + p]->defaultValue) {
          resolvedArgs[p] = fDecl->params[paramOffset + p]->defaultValue;
          resolvedTypes[p] =
              fDecl->params[paramOffset + p]->defaultValue->exprType;
        } else {
          auto pName = std::string(fDecl->params[paramOffset + p]->name);
          if (fDecl->params[paramOffset + p]->isRequired) {
            errors.push_back("Missing required named parameter '" + pName +
                             "'.");
          } else if (!fDecl->params[paramOffset + p]->isNamed) {
            errors.push_back("Missing mandatory positional parameter '" +
                             pName + "'.");
          } else {
            errors.push_back("Missing parameter '" + pName + "'.");
          }
        }
      }
    }

    if (!errors.empty())
      return errors;

    for (size_t p = 0; p < expectedParams; ++p) {
      if (!canImplicitlyCast(resolvedTypes[p],
                             fDecl->params[paramOffset + p]->type)) {
        errors.push_back("Type mismatch for parameter '" +
                         std::string(fDecl->params[paramOffset + p]->name) +
                         "': expected '" +
                         fDecl->params[paramOffset + p]->type->toString() +
                         "', but got '" + resolvedTypes[p]->toString() + "'.");
      }
    }

    if (!errors.empty())
      return errors;

    const_cast<FunctionCallNode *>(node)->args =
        ctx->astCtx.copyArray<ExprNode *>(resolvedArgs);
    const_cast<FunctionCallNode *>(node)->argNames = {};
    return errors;
  };

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
      auto recordDecls = ctx->lookup(recordTy->getName());

      const DeclNode *recDecl = nullptr;
      for (auto *d : recordDecls) {
        if (d->kind == NodeKind::ClassDecl || d->kind == NodeKind::StructDecl) {
          recDecl = d;
          break;
        }
      }

      if (recDecl) {
        const FunctionDeclNode *bestMatch = nullptr;
        std::vector<std::vector<std::string>> overloadErrors;

        llvm::ArrayRef<FunctionDeclNode *> methods;
        if (recDecl->kind == NodeKind::ClassDecl)
          methods = static_cast<const ClassDeclNode *>(recDecl)->methods;
        else
          methods = static_cast<const StructDeclNode *>(recDecl)->methods;

        for (const auto *method : methods) {
          if (method->name == ma->memberName) {
            auto errs = checkMatch(method);
            if (errs.empty()) {
              bestMatch = method;
              break;
            } else {
              overloadErrors.push_back(errs);
            }
          }
        }
        if (bestMatch) {
          const_cast<MemberAccessNode *>(ma)->resolvedMethod = bestMatch;
          const_cast<FunctionCallNode *>(node)->resolvedFunc = bestMatch;
          node->exprType = bestMatch->returnType;
          return node->exprType;
        }

        if (overloadErrors.size() == 1) {
          for (size_t i = 0; i < overloadErrors[0].size(); ++i) {
            if (i == overloadErrors[0].size() - 1) {
              return ctx->reportError(node->line, node->column, node->length,
                                      overloadErrors[0][i]);
            } else {
              ctx->reportError(node->line, node->column, node->length,
                               overloadErrors[0][i]);
            }
          }
        } else {
          std::string finalErr = "No matching method overload found for '" +
                                 std::string(ma->memberName) + "'.";
          if (!overloadErrors.empty()) {
            finalErr += " Candidates failed with:\n";
            for (const auto &errList : overloadErrors) {
              finalErr += "- ";
              for (size_t i = 0; i < errList.size(); ++i) {
                finalErr += errList[i];
                if (i < errList.size() - 1)
                  finalErr += ", ";
              }
              finalErr += "\n";
            }
          }
          return ctx->reportError(node->line, node->column, node->length,
                                  finalErr);
        }
      }
    }
  } else if (node->target->kind == NodeKind::Variable) {
    std::string_view name =
        static_cast<const VariableNode *>(node->target)->name;
    auto decls = ctx->lookup(name);

    /* Unwrap typedefs to their underlying entity overload sets */
    if (!decls.empty()) {
      const DeclNode *target = decls.front();
      while (target && target->kind == NodeKind::TypedefDecl) {
        auto td = static_cast<const TypedefDeclNode *>(target);
        if (!td->targetEntityName.empty()) {
          auto aliased = ctx->lookup(td->targetEntityName);
          if (!aliased.empty()) {
            target = aliased.front();
            decls = aliased; // Update overload set!
          } else {
            break;
          }
        } else {
          break;
        }
      }
    }

    bool hasCallable = false;
    for (auto *d : decls) {
      if (d->kind == NodeKind::FunctionDecl) {
        hasCallable = true;
        break;
      }
    }

    if (hasCallable) {
      const FunctionDeclNode *bestMatch = nullptr;
      bool isConstructorCall = false;
      std::vector<std::vector<std::string>> overloadErrors;

      for (auto targetDecl : decls) {
        if (targetDecl->kind == NodeKind::FunctionDecl) {
          auto fDecl = static_cast<const FunctionDeclNode *>(targetDecl);
          auto errs = checkMatch(fDecl);
          if (errs.empty()) {
            bestMatch = fDecl;
            if (fDecl->isMethod && ctx->astCtx.getRecordType(name) != nullptr) {
              isConstructorCall = true;
            }
            break;
          } else {
            overloadErrors.push_back(errs);
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

      if (overloadErrors.size() == 1) {
        for (size_t i = 0; i < overloadErrors[0].size(); ++i) {
          if (i == overloadErrors[0].size() - 1) {
            return ctx->reportError(node->line, node->column, node->length,
                                    overloadErrors[0][i]);
          } else {
            ctx->reportError(node->line, node->column, node->length,
                             overloadErrors[0][i]);
          }
        }
      } else {
        std::string finalErr = "No matching function overload found for '" +
                               std::string(name) + "'.";
        if (!overloadErrors.empty()) {
          finalErr += " Candidates failed with:\n";
          for (const auto &errList : overloadErrors) {
            finalErr += "- ";
            for (size_t i = 0; i < errList.size(); ++i) {
              finalErr += errList[i];
              if (i < errList.size() - 1)
                finalErr += ", ";
            }
            finalErr += "\n";
          }
        }
        return ctx->reportError(node->line, node->column, node->length,
                                finalErr);
      }
    }
  }

  /* General Fallback: Assess if target evaluates to a dynamic function pointer
   */
  auto targetTyRes = dispatch(node->target);
  if (targetTyRes) {
    const Type *unqual = (*targetTyRes)->getUnqualifiedType();
    if (unqual->isPointerType()) {
      const Type *pointee =
          static_cast<const PointerType *>(unqual)->getPointeeType();
      if (pointee->getKind() == TypeKind::Function) {
        auto fTy = static_cast<const FunctionType *>(pointee);

        if (argTypes.size() != fTy->getParamTypes().size()) {
          return ctx->reportError(
              node->line, node->column, node->length,
              "Argument count mismatch for function pointer call.");
        }
        for (size_t i = 0; i < argTypes.size(); i++) {
          if (!canImplicitlyCast(argTypes[i], fTy->getParamTypes()[i])) {
            return ctx->reportError(node->line, node->column, node->length,
                                    "Type mismatch in function pointer call.");
          }
        }

        const_cast<FunctionCallNode *>(node)->args =
            ctx->astCtx.copyArray<ExprNode *>(node->args);
        node->exprType = fTy->getReturnType();
        return node->exprType;
      }
    }
  }

  return ctx->reportError(node->line, node->column, node->length,
                          "Invalid function call target.");
}

SemaResult TypeCheckPass::visit(const CastNode *node) {
  auto srcType = dispatch(node->expr);
  const Type *destType = node->targetType;

  if (!srcType)
    return std::unexpected(ErrorInfo{node->line, node->column, node->length,
                                     "Cascading error in cast"});

  bool isSrcNumeric = (*srcType)->isNumeric();
  bool isDestNumeric = destType->isNumeric();
  bool isSrcPtr = (*srcType)->isPointerType();
  bool isDestPtr = destType->isPointerType();
  bool isSrcEnum = (*srcType)->getKind() == TypeKind::Enum;
  bool isDestEnum = destType->getKind() == TypeKind::Enum;

  // Support numeric conversions, pointer <-> pointer casts (e.g. T* to void* or
  // void* to T*), and pointer <-> integer conversions.
  if ((isSrcNumeric && isDestNumeric) || (isSrcPtr && isDestPtr) ||
      (isSrcPtr && isDestNumeric) || (isSrcNumeric && isDestPtr) ||
      (isSrcEnum && isDestNumeric) || (isSrcNumeric && isDestEnum) ||
      (isSrcEnum && isDestEnum)) {
    node->exprType = destType;
    return destType;
  }

  return ctx->reportError(node->line, node->column, node->length,
                          "Invalid cast: unsupported type conversion");
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
    /* Array subscript elements can be safely returned as references */
    if (node->value->kind != NodeKind::Variable &&
        node->value->kind != NodeKind::UnaryOp &&
        node->value->kind != NodeKind::FunctionCall &&
        node->value->kind != NodeKind::MemberAccess &&
        node->value->kind != NodeKind::ArraySubscript) {
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

SemaResult TypeCheckPass::visit(const ArrayLiteralNode *node) {
  const Type *elemType = nullptr;
  bool hasErrors = false;

  for (const auto *elem : node->elements) {
    auto res = dispatch(elem);
    if (!res) {
      hasErrors = true;
    } else if (!elemType) {
      elemType = *res;
    } else if (!canImplicitlyCast(*res, elemType)) {
      if (canImplicitlyCast(elemType, *res)) {
        elemType = *res;
      } else {
        ctx->reportError(elem->line, elem->column, elem->length,
                         "Array literal element type mismatch.");
        hasErrors = true;
      }
    }
  }

  if (hasErrors) {
    return std::unexpected(ErrorInfo{node->line, node->column, node->length,
                                     "Errors in array literal elements"});
  }

  if (!elemType) {
    elemType = ctx->astCtx.VoidTy;
  }

  const Type *arrType =
      ctx->astCtx.getArrayType(elemType, node->elements.size());
  node->exprType = arrType;
  return arrType;
}

SemaResult TypeCheckPass::visit(const ModuleNode *node) {
  if (visitedModules.contains(node))
    return ctx->astCtx.VoidTy;
  visitedModules.insert(node);

  bool hasErrors = false;

  for (const auto *imp : node->importedModules) {
    auto res = dispatch(imp);
    if (!res)
      hasErrors = true;
  }

  auto prevFile = ctx->currentFile;
  ctx->setCurrentFile(node->filePath);

  for (const auto &stmt : node->statements) {
    auto res = dispatch(stmt);
    if (!res)
      hasErrors = true;
  }

  ctx->setCurrentFile(prevFile);

  if (hasErrors) {
    return std::unexpected(ErrorInfo{node->line, node->column, node->length,
                                     "Errors in module statements"});
  }

  return ctx->astCtx.VoidTy;
}

SemaResult TypeCheckPass::visit(const ArraySubscriptNode *node) {
  auto baseType = dispatch(node->base);
  auto indexType = dispatch(node->index);

  if (!baseType || !indexType)
    return std::unexpected(ErrorInfo{node->line, node->column, node->length,
                                     "Cascading error in array subscript"});

  const Type *unqualBase = (*baseType)->getUnqualifiedType();
  if (unqualBase->isPointerType()) {
    node->exprType =
        static_cast<const PointerType *>(unqualBase)->getPointeeType();
  } else if (unqualBase->getKind() == TypeKind::Array) {
    node->exprType =
        static_cast<const ArrayType *>(unqualBase)->getElementType();
  } else {
    return ctx->reportError(node->line, node->column, node->length,
                            "Subscripted value is not an array or pointer");
  }

  if (!(*indexType)->isInteger()) {
    return ctx->reportError(node->line, node->column, node->length,
                            "Array subscript must be an integer");
  }

  return node->exprType;
}

SemaResult TypeCheckPass::visit(const NewExprNode *node) {
  if (node->arraySize) {
    auto szType = dispatch(node->arraySize);
    if (!szType || !(*szType)->isInteger()) {
      return ctx->reportError(node->line, node->column, node->length,
                              "Array size in 'new' must be an integer type");
    }
    node->exprType = ctx->astCtx.getPointerType(node->allocatedType);
    return node->exprType;
  }

  const Type *unqual = node->allocatedType->getUnqualifiedType();
  if (unqual->getKind() == TypeKind::Class ||
      unqual->getKind() == TypeKind::Struct) {
    auto *recTy = static_cast<const RecordType *>(unqual);
    auto *decl = recTy->getDeclaration();

    if (decl) {
      llvm::ArrayRef<FunctionDeclNode *> ctors;
      if (decl->kind == NodeKind::ClassDecl)
        ctors = static_cast<const ClassDeclNode *>(decl)->constructors;
      else if (decl->kind == NodeKind::StructDecl)
        ctors = static_cast<const StructDeclNode *>(decl)->constructors;

      std::vector<const Type *> argTypes;
      bool hasErrors = false;
      for (const auto &arg : node->args) {
        auto argType = dispatch(arg);
        if (!argType) {
          hasErrors = true;
        } else {
          argTypes.push_back(*argType);
        }
      }

      if (hasErrors) {
        return std::unexpected(
            ErrorInfo{node->line, node->column, node->length,
                      "Argument evaluation failed in new expression."});
      }

      const FunctionDeclNode *bestMatch = nullptr;
      std::vector<std::vector<std::string>> overloadErrors;

      for (const auto *ctor : ctors) {
        size_t paramOffset = 1;
        size_t expectedParams = ctor->params.size() - paramOffset;

        std::vector<ExprNode *> resolvedArgs(expectedParams, nullptr);
        std::vector<const Type *> resolvedTypes(expectedParams, nullptr);

        size_t posArgCount = 0;
        std::unordered_set<std::string_view> providedNamedArgs;
        std::vector<std::string> errors;

        for (size_t i = 0; i < node->args.size(); ++i) {
          if (node->argNames[i].empty()) {
            if (posArgCount >= expectedParams) {
              if (ctor->isVariadic) {
                resolvedArgs.push_back(node->args[i]);
                resolvedTypes.push_back(argTypes[i]);
                posArgCount++;
                continue;
              }
              errors.push_back("Too many arguments provided.");
              break;
            }
            if (ctor->params[paramOffset + posArgCount]->isNamed) {
              errors.push_back(
                  "Positional argument provided for named parameter '" +
                  std::string(ctor->params[paramOffset + posArgCount]->name) +
                  "'.");
              break;
            }
            resolvedArgs[posArgCount] = node->args[i];
            resolvedTypes[posArgCount] = argTypes[i];
            posArgCount++;
          } else {
            auto name = node->argNames[i];
            if (providedNamedArgs.contains(name)) {
              errors.push_back("Duplicate named argument '" +
                               std::string(name) + "'.");
              continue;
            }
            providedNamedArgs.insert(name);

            bool found = false;
            for (size_t p = 0; p < expectedParams; ++p) {
              if (ctor->params[paramOffset + p]->name == name) {
                if (!ctor->params[paramOffset + p]->isNamed) {
                  errors.push_back("Parameter '" + std::string(name) +
                                   "' cannot be passed as a named argument.");
                }
                resolvedArgs[p] = node->args[i];
                resolvedTypes[p] = argTypes[i];
                found = true;
                break;
              }
            }
            if (!found) {
              errors.push_back("No such named parameter '" + std::string(name) +
                               "'.");
            }
          }
        }

        if (!errors.empty()) {
          overloadErrors.push_back(errors);
          continue;
        }

        for (size_t p = 0; p < expectedParams; ++p) {
          if (!resolvedArgs[p]) {
            if (ctor->params[paramOffset + p]->defaultValue) {
              resolvedArgs[p] = ctor->params[paramOffset + p]->defaultValue;
              resolvedTypes[p] =
                  ctor->params[paramOffset + p]->defaultValue->exprType;
            } else {
              auto pName = std::string(ctor->params[paramOffset + p]->name);
              if (ctor->params[paramOffset + p]->isRequired) {
                errors.push_back("Missing required named parameter '" + pName +
                                 "'.");
              } else if (!ctor->params[paramOffset + p]->isNamed) {
                errors.push_back("Missing mandatory positional parameter '" +
                                 pName + "'.");
              } else {
                errors.push_back("Missing parameter '" + pName + "'.");
              }
            }
          }
        }

        if (!errors.empty()) {
          overloadErrors.push_back(errors);
          continue;
        }

        for (size_t p = 0; p < expectedParams; ++p) {
          if (!canImplicitlyCast(resolvedTypes[p],
                                 ctor->params[paramOffset + p]->type)) {
            errors.push_back("Type mismatch for parameter '" +
                             std::string(ctor->params[paramOffset + p]->name) +
                             "': expected '" +
                             ctor->params[paramOffset + p]->type->toString() +
                             "', but got '" + resolvedTypes[p]->toString() +
                             "'.");
          }
        }

        if (!errors.empty()) {
          overloadErrors.push_back(errors);
          continue;
        }

        bestMatch = ctor;
        const_cast<NewExprNode *>(node)->args =
            ctx->astCtx.copyArray<ExprNode *>(resolvedArgs);
        const_cast<NewExprNode *>(node)->argNames = {};
        break;
      }

      if (!bestMatch) {
        std::string finalErr = "No matching constructor found for '" +
                               std::string(recTy->getName()) + "'.";
        if (!overloadErrors.empty()) {
          finalErr += " Candidates failed with:\n";
          for (const auto &errList : overloadErrors) {
            finalErr += "- ";
            for (size_t i = 0; i < errList.size(); ++i) {
              finalErr += errList[i];
              if (i < errList.size() - 1)
                finalErr += ", ";
            }
            finalErr += "\n";
          }
        }
        return ctx->reportError(node->line, node->column, node->length,
                                finalErr);
      }

      const_cast<NewExprNode *>(node)->resolvedConstructor = bestMatch;
    }
  }

  node->exprType = ctx->astCtx.getPointerType(node->allocatedType);
  return node->exprType;
}

SemaResult TypeCheckPass::visit(const DeleteExprNode *node) {
  auto ptrTy = dispatch(node->ptr);
  if (!ptrTy || !(*ptrTy)->isPointerType()) {
    return ctx->reportError(node->line, node->column, node->length,
                            "Cannot delete non-pointer type");
  }

  node->exprType = ctx->astCtx.VoidTy;
  return ctx->astCtx.VoidTy;
}

} // namespace utopia