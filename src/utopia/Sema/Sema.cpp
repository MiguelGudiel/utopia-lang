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
  for (const auto *field : node->fields) {
    auto res = dispatch(field);
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
  }

  if (hasErrors) {
    return std::unexpected(ErrorInfo{node->line, node->column, node->length,
                                     "Errors in function declaration"});
  }

  return node->returnType;
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
        std::vector<std::vector<std::string>> overloadErrors;

        for (const auto *method : clsDecl->methods) {
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
  }

  if (node->target->kind == NodeKind::Variable) {
    std::string_view name =
        static_cast<const VariableNode *>(node->target)->name;
    auto decls = ctx->lookup(name);

    if (decls.empty()) {
      return ctx->reportError(node->line, node->column, node->length,
                              "Undefined function: '" + std::string(name) +
                                  "'");
    }

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
      return ctx->reportError(node->line, node->column, node->length, finalErr);
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

  // Support numeric conversions, pointer <-> pointer casts (e.g. T* to void* or
  // void* to T*), and pointer <-> integer conversions.
  if ((isSrcNumeric && isDestNumeric) || (isSrcPtr && isDestPtr) ||
      (isSrcPtr && isDestNumeric) || (isSrcNumeric && isDestPtr)) {
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

} // namespace utopia