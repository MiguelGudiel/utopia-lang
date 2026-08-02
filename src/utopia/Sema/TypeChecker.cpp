#include "utopia/AST/ASTCloner.hpp"
#include "utopia/CodeGen/Mangler.hpp"
#include "utopia/Common/Logger.hpp"
#include "utopia/Sema/EffectAnalyzer.hpp"
#include "utopia/Sema/Sema.hpp"
#include <string>

namespace utopia {

namespace {
/* Operator categorization limits large evaluation chains during semantic passes
 */
enum class OpCategory {
  Logical,
  Relational,
  BitwiseOrModulo,
  Arithmetic,
  Unknown
};

static const std::unordered_map<std::string_view, OpCategory> opCategoryMap = {
    {"&&", OpCategory::Logical},         {"||", OpCategory::Logical},
    {"==", OpCategory::Relational},      {"!=", OpCategory::Relational},
    {"<", OpCategory::Relational},       {">", OpCategory::Relational},
    {"<=", OpCategory::Relational},      {">=", OpCategory::Relational},
    {"<<", OpCategory::BitwiseOrModulo}, {">>", OpCategory::BitwiseOrModulo},
    {"&", OpCategory::BitwiseOrModulo},  {"|", OpCategory::BitwiseOrModulo},
    {"^", OpCategory::BitwiseOrModulo},  {"%", OpCategory::BitwiseOrModulo},
    {"+", OpCategory::Arithmetic},       {"-", OpCategory::Arithmetic},
    {"*", OpCategory::Arithmetic},       {"/", OpCategory::Arithmetic}};

enum class AssignCategory { BitwiseOrModulo, Arithmetic, Unknown };

static const std::unordered_map<std::string_view, AssignCategory> assignCatMap =
    {{"%", AssignCategory::BitwiseOrModulo},
     {"&", AssignCategory::BitwiseOrModulo},
     {"|", AssignCategory::BitwiseOrModulo},
     {"^", AssignCategory::BitwiseOrModulo},
     {"<<", AssignCategory::BitwiseOrModulo},
     {">>", AssignCategory::BitwiseOrModulo},
     {"+", AssignCategory::Arithmetic},
     {"-", AssignCategory::Arithmetic},
     {"*", AssignCategory::Arithmetic},
     {"/", AssignCategory::Arithmetic}};
} // namespace

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

const Type *TypeCheckPass::resolveIfTemplate(const Type *t) {
  if (!t)
    return nullptr;
  if (t->getKind() == TypeKind::TemplateInst) {
    auto *instTy = static_cast<const TemplateInstType *>(t);
    if (instTy->getResolvedType())
      return instTy->getResolvedType();

    auto declIt = ctx->templateRegistry.find(instTy->getBaseName());
    if (declIt == ctx->templateRegistry.end()) {
      ctx->reportError(0, 0, 0,
                       "Template declaration not found: " +
                           std::string(instTy->getBaseName()));
      return ctx->astCtx.VoidTy;
    }

    const DeclNode *tmplDecl = declIt->second;

    if (ctx->currentModule &&
        !ctx->currentModule->canSee(tmplDecl->declFilePath)) {
      ctx->reportError(0, 0, 0,
                       "Template '" + std::string(instTy->getBaseName()) +
                           "' is not visible in this module.");
      return ctx->astCtx.VoidTy;
    }

    std::string mangledName = std::string(instTy->getBaseName());
    for (const auto *arg : instTy->getTemplateArgs()) {
      std::string argStr = arg->toString();
      for (char &c : argStr) {
        if (!isalnum(c))
          c = '_';
      }
      mangledName += "_" + argStr;
    }
    std::string_view mangledView = ctx->astCtx.copyString(mangledName);

    if (tmplDecl->kind == NodeKind::ClassDecl ||
        tmplDecl->kind == NodeKind::StructDecl ||
        tmplDecl->kind == NodeKind::UnionDecl) {
      if (auto *existing = ctx->astCtx.getRecordType(mangledView)) {
        instTy->setResolvedType(existing);
        return existing;
      }

      TypeKind kind = TypeKind::Struct;
      if (tmplDecl->kind == NodeKind::ClassDecl)
        kind = TypeKind::Class;
      else if (tmplDecl->kind == NodeKind::UnionDecl)
        kind = TypeKind::Union;

      ctx->astCtx.createRecordType(kind, mangledView);
    }

    std::unordered_map<std::string_view, const Type *> templateArgMap;
    for (size_t i = 0; i < tmplDecl->templateParams.size(); ++i) {
      templateArgMap[tmplDecl->templateParams[i]] =
          instTy->getTemplateArgs()[i];
    }

    ASTCloner cloner(ctx->astCtx, templateArgMap);
    DeclNode *instDecl = static_cast<DeclNode *>(cloner.dispatch(tmplDecl));

    if (instDecl) {
      instDecl->hasPublicMod = tmplDecl->hasPublicMod;
      instDecl->hasPrivateMod = tmplDecl->hasPrivateMod;
      instDecl->annotations = tmplDecl->annotations;
      instDecl->declFilePath = tmplDecl->declFilePath;

      if (instDecl->kind == NodeKind::ClassDecl) {
        static_cast<ClassDeclNode *>(instDecl)->name = mangledView;
      } else if (instDecl->kind == NodeKind::StructDecl) {
        static_cast<StructDeclNode *>(instDecl)->name = mangledView;
      } else if (instDecl->kind == NodeKind::UnionDecl) {
        static_cast<UnionDeclNode *>(instDecl)->name = mangledView;
      } else if (instDecl->kind == NodeKind::FunctionDecl) {
        static_cast<FunctionDeclNode *>(instDecl)->name = mangledView;
      }

      /* Dynamically repopulate FieldInfo array for RecordType bindings
       * post-clone */
      if (instDecl->kind == NodeKind::ClassDecl ||
          instDecl->kind == NodeKind::StructDecl ||
          instDecl->kind == NodeKind::UnionDecl) {
        RecordType *recTy = ctx->astCtx.getRecordType(mangledView);
        std::vector<FieldInfo> fInfos;
        uint32_t instanceFieldIndex = 0;

        llvm::ArrayRef<VarDeclNode *> fields;
        if (instDecl->kind == NodeKind::ClassDecl)
          fields = static_cast<ClassDeclNode *>(instDecl)->fields;
        else if (instDecl->kind == NodeKind::StructDecl)
          fields = static_cast<StructDeclNode *>(instDecl)->fields;
        else
          fields = static_cast<UnionDeclNode *>(instDecl)->fields;

        for (size_t i = 0; i < fields.size(); ++i) {
          if (fields[i]->isStatic)
            continue;
          fInfos.push_back({fields[i]->varName, fields[i]->type,
                            instanceFieldIndex++,
                            fields[i]->isPublic(fields[i]->varName)});
        }
        recTy->setFields(ctx->astCtx.copyArray<FieldInfo>(fInfos));

        /* Ensure method ownership is retained post-clone */
        auto updateParentRec = [&](llvm::ArrayRef<FunctionDeclNode *> funcs) {
          for (auto *f : funcs) {
            if (!f->isStatic) {
              const_cast<FunctionDeclNode *>(f)->parentRecord = recTy;
            }
          }
        };

        if (instDecl->kind == NodeKind::ClassDecl) {
          auto *cls = static_cast<ClassDeclNode *>(instDecl);
          cls->recordType = recTy;
          recTy->setOpaque(cls->isOpaque);
          updateParentRec(cls->methods);
          updateParentRec(cls->constructors);
          if (cls->destructor) {
            cls->destructor->parentRecord = recTy;
          }
        } else if (instDecl->kind == NodeKind::StructDecl) {
          auto *str = static_cast<StructDeclNode *>(instDecl);
          str->recordType = recTy;
          recTy->setOpaque(str->isOpaque);
          updateParentRec(str->methods);
          updateParentRec(str->constructors);
          if (str->destructor) {
            str->destructor->parentRecord = recTy;
          }
        } else {
          auto *uni = static_cast<UnionDeclNode *>(instDecl);
          uni->recordType = recTy;
          recTy->setOpaque(uni->isOpaque);
          updateParentRec(uni->methods);
          updateParentRec(uni->constructors);
          if (uni->destructor) {
            uni->destructor->parentRecord = recTy;
          }
        }
      }

      if (ctx->currentModule) {
        const_cast<ModuleNode *>(ctx->currentModule)
            ->instantiatedTemplates.push_back(instDecl);
      }
      DeclCollectorPass dcp;
      dcp.ctx = ctx;

      auto prevFile = ctx->currentFile;
      ctx->setCurrentFile(instDecl->declFilePath);

      dcp.dispatch(instDecl);
      dispatch(instDecl);

      ctx->setCurrentFile(prevFile);
    }

    const Type *res = ctx->astCtx.VoidTy;
    if (tmplDecl->kind == NodeKind::ClassDecl ||
        tmplDecl->kind == NodeKind::StructDecl ||
        tmplDecl->kind == NodeKind::UnionDecl) {
      res = ctx->astCtx.getRecordType(mangledView);
    }
    instTy->setResolvedType(res);
    return res;
  }
  if (t->isPointerType()) {
    auto *p = static_cast<const PointerType *>(t);
    return ctx->astCtx.getPointerType(resolveIfTemplate(p->getPointeeType()));
  }
  if (t->isReferenceType()) {
    auto *p = static_cast<const ReferenceType *>(t);
    return ctx->astCtx.getReferenceType(resolveIfTemplate(p->getPointeeType()));
  }
  if (t->getKind() == TypeKind::Array) {
    auto *p = static_cast<const ArrayType *>(t);
    return ctx->astCtx.getArrayType(resolveIfTemplate(p->getElementType()),
                                    p->getSize());
  }
  return t;
}

bool TypeCheckPass::checkTypeVisibility(const Type *type, const ASTNode *node) {
  if (!type)
    return true;
  const Type *unqual = type->getUnqualifiedType();
  while (unqual->getKind() == TypeKind::Array) {
    unqual = static_cast<const ArrayType *>(unqual)
                 ->getElementType()
                 ->getUnqualifiedType();
  }

  const DeclNode *decl = nullptr;
  std::string_view typeName;

  if (unqual->getKind() == TypeKind::Class ||
      unqual->getKind() == TypeKind::Struct ||
      unqual->getKind() == TypeKind::Union) {
    auto *recTy = static_cast<const RecordType *>(unqual);
    decl = recTy->getDeclaration();
    typeName = recTy->getName();
  } else if (unqual->getKind() == TypeKind::Enum) {
    auto *enumTy = static_cast<const EnumType *>(unqual);
    decl = enumTy->getDeclaration();
    typeName = enumTy->getName();
  } else if (unqual->getKind() == TypeKind::Alias) {
    auto *aliasTy = static_cast<const AliasType *>(unqual);
    decl = aliasTy->getDeclaration();
    typeName = aliasTy->getName();
  }

  if (decl) {
    if (!decl->isPublic(typeName) && decl->declFilePath != ctx->currentFile) {
      ctx->reportError(node->line, node->column, node->length,
                       "Cannot access private type '" + std::string(typeName) +
                           "' from outside its file.");
      return false;
    }

    if (ctx->currentModule && !ctx->currentModule->canSee(decl->declFilePath)) {
      ctx->reportError(node->line, node->column, node->length,
                       "Type '" + std::string(typeName) +
                           "' is not visible in this module.");
      return false;
    }
  }
  return true;
}

void TypeCheckPass::checkNodiscard(const ASTNode *node) {
  if (!node)
    return;

  if (node->kind == NodeKind::Cast) {
    auto *castNode = static_cast<const CastNode *>(node);
    if (castNode->targetType && castNode->targetType->isVoid()) {
      /* Explicit cast to void suppresses the nodiscard warning */
      return;
    }
    checkNodiscard(castNode->expr);
    return;
  }
  if (node->kind == NodeKind::ImplicitCast) {
    checkNodiscard(static_cast<const ImplicitCastNode *>(node)->expr);
    return;
  }

  bool isNodiscardFunc = false;
  std::string funcName = "";

  if (node->kind == NodeKind::FunctionCall) {
    auto *call = static_cast<const FunctionCallNode *>(node);
    if (call->resolvedFunc) {
      for (const auto *ann : call->resolvedFunc->annotations) {
        if (ann->name == "nodiscard") {
          isNodiscardFunc = true;
          funcName = std::string(call->resolvedFunc->name);
          break;
        }
      }
    }
  } else if (node->kind == NodeKind::UnaryOp) {
    auto *uop = static_cast<const UnaryOpNode *>(node);
    if (uop->overloadedOperator) {
      for (const auto *ann : uop->overloadedOperator->annotations) {
        if (ann->name == "nodiscard") {
          isNodiscardFunc = true;
          funcName = "operator" + std::string(uop->op);
          break;
        }
      }
    }
  } else if (node->kind == NodeKind::BinaryOp) {
    auto *bop = static_cast<const BinaryOpNode *>(node);
    if (bop->overloadedOperator) {
      for (const auto *ann : bop->overloadedOperator->annotations) {
        if (ann->name == "nodiscard") {
          isNodiscardFunc = true;
          funcName = "operator" + std::string(bop->op);
          break;
        }
      }
    }
  } else if (node->kind == NodeKind::ArraySubscript) {
    auto *sub = static_cast<const ArraySubscriptNode *>(node);
    if (sub->overloadedOperator) {
      for (const auto *ann : sub->overloadedOperator->annotations) {
        if (ann->name == "nodiscard") {
          isNodiscardFunc = true;
          funcName = "operator[]";
          break;
        }
      }
    }
  } else if (node->kind == NodeKind::Assign) {
    auto *asn = static_cast<const AssignNode *>(node);
    if (asn->overloadedOperator) {
      for (const auto *ann : asn->overloadedOperator->annotations) {
        if (ann->name == "nodiscard") {
          isNodiscardFunc = true;
          funcName = "operator" + std::string(asn->op);
          break;
        }
      }
    }
  }

  if (isNodiscardFunc) {
    ctx->diags.report({DiagLevel::Warning, node->line, node->column,
                       node->length,
                       "Ignoring return value of function '" + funcName +
                           "', declared with '@nodiscard' annotation.",
                       std::string(ctx->currentFile), node->endLine});
    return;
  }

  /* Check if the returned type itself is marked as nodiscard */
  if (node->kind == NodeKind::FunctionCall || node->kind == NodeKind::UnaryOp ||
      node->kind == NodeKind::BinaryOp ||
      node->kind == NodeKind::ArraySubscript) {
    const ExprNode *expr = static_cast<const ExprNode *>(node);
    if (expr->exprType) {
      const Type *unqual = expr->exprType->getUnqualifiedType();
      const DeclNode *typeDecl = nullptr;
      std::string typeName = "";

      if (unqual->getKind() == TypeKind::Class ||
          unqual->getKind() == TypeKind::Struct ||
          unqual->getKind() == TypeKind::Union) {
        auto *recTy = static_cast<const RecordType *>(unqual);
        typeDecl = recTy->getDeclaration();
        typeName = std::string(recTy->getName());
      } else if (unqual->getKind() == TypeKind::Enum) {
        auto *enumTy = static_cast<const EnumType *>(unqual);
        typeDecl = enumTy->getDeclaration();
        typeName = std::string(enumTy->getName());
      }

      if (typeDecl) {
        for (const auto *ann : typeDecl->annotations) {
          if (ann->name == "nodiscard") {
            ctx->diags.report({DiagLevel::Warning, node->line, node->column,
                               node->length,
                               "Ignoring return value of type '" + typeName +
                                   "', declared with '@nodiscard' annotation.",
                               std::string(ctx->currentFile), node->endLine});
            return;
          }
        }
      }
    }
  }
}

void TypeCheckPass::checkDeprecated(const DeclNode *decl, const ASTNode *node) {
  if (!decl || !node)
    return;
  for (const auto *ann : decl->annotations) {
    if (ann->name == "deprecated") {
      std::string declKind = "declaration";
      std::string declName = "unknown";

      switch (decl->kind) {
      case NodeKind::VarDecl:
        declKind = "variable";
        declName = std::string(static_cast<const VarDeclNode *>(decl)->varName);
        break;
      case NodeKind::ParamDecl:
        declKind = "parameter";
        declName = std::string(static_cast<const ParamDeclNode *>(decl)->name);
        break;
      case NodeKind::FunctionDecl:
        declKind = static_cast<const FunctionDeclNode *>(decl)->isMethod
                       ? "method"
                       : "function";
        declName =
            std::string(static_cast<const FunctionDeclNode *>(decl)->name);
        break;
      case NodeKind::StructDecl:
        declKind = "struct";
        declName = std::string(static_cast<const StructDeclNode *>(decl)->name);
        break;
      case NodeKind::ClassDecl:
        declKind = "class";
        declName = std::string(static_cast<const ClassDeclNode *>(decl)->name);
        break;
      case NodeKind::UnionDecl:
        declKind = "union";
        declName = std::string(static_cast<const UnionDeclNode *>(decl)->name);
        break;
      case NodeKind::EnumDecl:
        declKind = "enum";
        declName = std::string(static_cast<const EnumDeclNode *>(decl)->name);
        break;
      case NodeKind::EnumMember:
        declKind = "enum member";
        declName = std::string(static_cast<const EnumMemberNode *>(decl)->name);
        break;
      case NodeKind::TypedefDecl:
        declKind = "type alias";
        declName =
            std::string(static_cast<const TypedefDeclNode *>(decl)->aliasName);
        break;
      case NodeKind::AnnotationDecl:
        declKind = "annotation";
        declName =
            std::string(static_cast<const AnnotationDeclNode *>(decl)->name);
        break;
      default:
        break;
      }

      std::string msg = "use of deprecated " + declKind + " '" + declName + "'";
      if (!ann->args.empty() && ann->args[0]->kind == NodeKind::String) {
        msg += ": " + std::string(
                          static_cast<const StringNode *>(ann->args[0])->value);
      }

      ctx->diags.report({DiagLevel::Warning, node->line, node->column,
                         node->length, msg, std::string(ctx->currentFile),
                         node->endLine});
    }
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

SemaResult TypeCheckPass::visit(const NumberNode *node) {
  std::string_view raw = node->raw;
  const Type *ty = ctx->astCtx.Int32Ty;

  /* Shield hex values from being incorrectly typed as Float32 due to 'F'/'f' */
  bool isHex =
      raw.length() > 2 && raw[0] == '0' && (raw[1] == 'x' || raw[1] == 'X');

  if (!isHex && (raw.ends_with('f') || raw.ends_with('F'))) {
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
      uint64_t val = std::stoull(std::string(raw), nullptr, 0);
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
    } catch (const std::invalid_argument &) {
      return ctx->reportError(node->line, node->column, node->length,
                              "Invalid integer literal");
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

SemaResult TypeCheckPass::visit(const UnionDeclNode *node) {
  if (node->isTemplate)
    return ctx->astCtx.VoidTy;

  bool hasErrors = false;

  if (node->isOpaque)
    return ctx->astCtx.VoidTy;

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
            std::string(static_cast<const NumberNode *>(ann->args[0])->raw),
            nullptr, 0);
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

  auto prevContext = ctx->getCurrentRecordContext();
  ctx->setCurrentRecordContext(node->recordType);

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

  ctx->setCurrentRecordContext(prevContext);

  if (hasErrors) {
    return std::unexpected(ErrorInfo{node->line, node->column, node->length,
                                     "Errors in union declaration"});
  }
  return ctx->astCtx.VoidTy;
}

SemaResult TypeCheckPass::visit(const StructDeclNode *node) {
  if (node->isTemplate)
    return ctx->astCtx.VoidTy;

  bool hasErrors = false;

  if (node->isOpaque)
    return ctx->astCtx.VoidTy;

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
            std::string(static_cast<const NumberNode *>(ann->args[0])->raw),
            nullptr, 0);
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

  auto prevContext = ctx->getCurrentRecordContext();
  ctx->setCurrentRecordContext(node->recordType);

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

  ctx->setCurrentRecordContext(prevContext);

  if (hasErrors) {
    return std::unexpected(ErrorInfo{node->line, node->column, node->length,
                                     "Errors in struct declaration"});
  }
  return ctx->astCtx.VoidTy;
}

SemaResult TypeCheckPass::visit(const ClassDeclNode *node) {
  if (node->isTemplate)
    return ctx->astCtx.VoidTy;

  bool hasErrors = false;

  if (node->isOpaque)
    return ctx->astCtx.VoidTy;

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
            std::string(static_cast<const NumberNode *>(ann->args[0])->raw),
            nullptr, 0);
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

  auto prevContext = ctx->getCurrentRecordContext();
  ctx->setCurrentRecordContext(node->recordType);

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

  ctx->setCurrentRecordContext(prevContext);

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
            std::stoll(std::string(static_cast<const NumberNode *>(init)->raw),
                       nullptr, 0);
      } else if (init->kind == NodeKind::UnaryOp) {
        auto uop = static_cast<const UnaryOpNode *>(init);
        if (uop->op == "-" && uop->expr->kind == NodeKind::Number) {
          val = -std::stoll(
              std::string(static_cast<const NumberNode *>(uop->expr)->raw),
              nullptr, 0);
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

SemaResult TypeCheckPass::visit(const TypeLiteralNode *node) {
  node->exprType = ctx->astCtx.TypeValTy;
  node->isLValue = false;
  return node->exprType;
}

SemaResult TypeCheckPass::visit(const VariableNode *node) {
  if (!node->templateArgs.empty()) {
    auto decls = ctx->lookup(node->name);
    DeclNode *tmplDecl = nullptr;
    for (auto *d : decls) {
      if (d->kind == NodeKind::FunctionDecl &&
          static_cast<const FunctionDeclNode *>(d)->isTemplate) {
        tmplDecl = const_cast<DeclNode *>(d);
        break;
      }
    }

    if (!tmplDecl) {
      auto it = ctx->templateRegistry.find(node->name);
      if (it != ctx->templateRegistry.end() &&
          it->second->kind == NodeKind::FunctionDecl) {
        if (!ctx->currentModule ||
            ctx->currentModule->canSee(it->second->declFilePath)) {
          tmplDecl = const_cast<DeclNode *>(it->second);
        }
      }
    }

    if (tmplDecl) {
      std::string mangledName = std::string(node->name);
      for (const auto *arg : node->templateArgs) {
        const Type *resArg = resolveIfTemplate(arg);
        std::string argStr = resArg->toString();
        for (char &c : argStr) {
          if (!isalnum(c))
            c = '_';
        }
        mangledName += "_" + argStr;
      }
      std::string_view mangledView = ctx->astCtx.copyString(mangledName);

      auto instDecls = ctx->lookup(mangledView);
      if (instDecls.empty()) {
        std::unordered_map<std::string_view, const Type *> templateArgMap;
        for (size_t i = 0; i < tmplDecl->templateParams.size(); ++i) {
          templateArgMap[tmplDecl->templateParams[i]] =
              resolveIfTemplate(node->templateArgs[i]);
        }

        ASTCloner cloner(ctx->astCtx, templateArgMap);
        DeclNode *instDecl = static_cast<DeclNode *>(cloner.dispatch(tmplDecl));

        if (instDecl) {
          static_cast<FunctionDeclNode *>(instDecl)->name = mangledView;
          instDecl->hasPublicMod = tmplDecl->hasPublicMod;
          instDecl->hasPrivateMod = tmplDecl->hasPrivateMod;
          instDecl->annotations = tmplDecl->annotations;
          instDecl->declFilePath = tmplDecl->declFilePath;

          if (ctx->currentModule) {
            const_cast<ModuleNode *>(ctx->currentModule)
                ->instantiatedTemplates.push_back(instDecl);
          }
          DeclCollectorPass dcp;
          dcp.ctx = ctx;

          auto prevFile = ctx->currentFile;
          ctx->setCurrentFile(instDecl->declFilePath);

          dcp.dispatch(instDecl);
          dispatch(instDecl);

          ctx->setCurrentFile(prevFile);
        }
      }
      const_cast<VariableNode *>(node)->name = mangledView;
    }
  }

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
          const Type *unqualPointee = pointee->getUnqualifiedType();
          if (unqualPointee->getKind() == TypeKind::Class ||
              unqualPointee->getKind() == TypeKind::Struct ||
              unqualPointee->getKind() == TypeKind::Union) {
            auto clsTy = static_cast<const RecordType *>(unqualPointee);
            if (auto field = clsTy->getField(node->name)) {
              if (!field->isPublic && ctx->getCurrentRecordContext() != clsTy) {
                return ctx->reportError(node->line, node->column, node->length,
                                        "Cannot access private field '" +
                                            std::string(node->name) + "'");
              }
              const_cast<VariableNode *>(node)->isField = true;
              const_cast<VariableNode *>(node)->fieldIndex = field->index;
              const_cast<VariableNode *>(node)->parentType = clsTy;
              node->exprType = field->type;
              node->isLValue = true;

              if (const DeclNode *recDecl = clsTy->getDeclaration()) {
                llvm::ArrayRef<VarDeclNode *> fields;
                if (recDecl->kind == NodeKind::ClassDecl)
                  fields = static_cast<const ClassDeclNode *>(recDecl)->fields;
                else if (recDecl->kind == NodeKind::StructDecl)
                  fields = static_cast<const StructDeclNode *>(recDecl)->fields;
                else if (recDecl->kind == NodeKind::UnionDecl)
                  fields = static_cast<const UnionDeclNode *>(recDecl)->fields;

                for (const auto *fDecl : fields) {
                  if (fDecl->varName == node->name) {
                    checkDeprecated(fDecl, node);
                    break;
                  }
                }
              }

              return field->type;
            }
          }
        }
      }
    }

    if (const RecordType *recTy = ctx->getCurrentRecordContext()) {
      const DeclNode *recDecl = recTy->getDeclaration();
      if (recDecl) {
        llvm::ArrayRef<VarDeclNode *> cFields;
        llvm::ArrayRef<FunctionDeclNode *> cMethods;
        if (recDecl->kind == NodeKind::ClassDecl) {
          cFields = static_cast<const ClassDeclNode *>(recDecl)->fields;
          cMethods = static_cast<const ClassDeclNode *>(recDecl)->methods;
        } else if (recDecl->kind == NodeKind::StructDecl) {
          cFields = static_cast<const StructDeclNode *>(recDecl)->fields;
          cMethods = static_cast<const StructDeclNode *>(recDecl)->methods;
        } else if (recDecl->kind == NodeKind::UnionDecl) {
          cFields = static_cast<const UnionDeclNode *>(recDecl)->fields;
          cMethods = static_cast<const UnionDeclNode *>(recDecl)->methods;
        }

        for (auto *f : cFields) {
          if (f->isStatic && f->varName == node->name) {
            const_cast<VariableNode *>(node)->resolvedDecl = f;
            node->exprType = f->type;
            node->isLValue = true;
            checkDeprecated(f, node);
            return f->type;
          }
        }
        for (auto *m : cMethods) {
          if (m->isStatic && m->name == node->name) {
            const_cast<VariableNode *>(node)->resolvedDecl = m;
            std::vector<const Type *> pTypes;
            for (auto *p : m->params)
              pTypes.push_back(p->type);
            const Type *funcTy = ctx->astCtx.getFunctionType(
                m->returnType, ctx->astCtx.copyArray<const Type *>(pTypes));
            node->exprType = ctx->astCtx.getPointerType(funcTy);
            node->isLValue = true;
            checkDeprecated(m, node);
            return node->exprType;
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

  if (target->kind == NodeKind::StructDecl ||
      target->kind == NodeKind::ClassDecl ||
      target->kind == NodeKind::UnionDecl ||
      target->kind == NodeKind::EnumDecl ||
      target->kind == NodeKind::TypedefDecl) {

    const Type *repTy = nullptr;
    if (target->kind == NodeKind::StructDecl)
      repTy = static_cast<const StructDeclNode *>(target)->recordType;
    else if (target->kind == NodeKind::ClassDecl)
      repTy = static_cast<const ClassDeclNode *>(target)->recordType;
    else if (target->kind == NodeKind::UnionDecl)
      repTy = static_cast<const UnionDeclNode *>(target)->recordType;
    else if (target->kind == NodeKind::EnumDecl)
      repTy = static_cast<const EnumDeclNode *>(target)->enumType;
    else if (target->kind == NodeKind::TypedefDecl)
      repTy = static_cast<const TypedefDeclNode *>(target)->aliasType;

    const_cast<VariableNode *>(node)->resolvedDecl = target;
    const_cast<VariableNode *>(node)->representedType = repTy;
    node->exprType = ctx->astCtx.TypeValTy;
    node->isLValue = false;
    checkDeprecated(target, node);
    return node->exprType;
  }

  if (target->kind == NodeKind::VarDecl) {
    if (!target->isPublic(static_cast<const VarDeclNode *>(target)->varName) &&
        target->declFilePath != ctx->currentFile) {
      return ctx->reportError(node->line, node->column, node->length,
                              "Cannot access private variable '" +
                                  std::string(node->name) +
                                  "' from outside its file.");
    }
    ty = static_cast<const VarDeclNode *>(target)->type;
  } else if (target->kind == NodeKind::ParamDecl) {
    ty = static_cast<const ParamDeclNode *>(target)->type;
    if (ty->getKind() == TypeKind::Array) {
      ty = ctx->astCtx.getPointerType(
          static_cast<const ArrayType *>(ty)->getElementType());
    }
  } else if (target->kind == NodeKind::FunctionDecl) {
    if (!target->isPublic(
            static_cast<const FunctionDeclNode *>(target)->name) &&
        target->declFilePath != ctx->currentFile) {
      return ctx->reportError(node->line, node->column, node->length,
                              "Cannot access private function '" +
                                  std::string(node->name) +
                                  "' from outside its file.");
    }
    auto fDecl = static_cast<const FunctionDeclNode *>(target);
    std::vector<const Type *> pTypes;
    for (auto *p : fDecl->params)
      pTypes.push_back(p->type);

    const Type *funcTy = ctx->astCtx.getFunctionType(
        fDecl->returnType, ctx->astCtx.copyArray<const Type *>(pTypes));
    ty = ctx->astCtx.getPointerType(funcTy);
  } else if (target->kind == NodeKind::TypedefDecl) {
    if (!target->isPublic(
            static_cast<const TypedefDeclNode *>(target)->aliasName) &&
        target->declFilePath != ctx->currentFile) {
      return ctx->reportError(node->line, node->column, node->length,
                              "Cannot access private type alias '" +
                                  std::string(node->name) +
                                  "' from outside its file.");
    }
    ty = ctx->astCtx.VoidTy;
  } else if (target->kind == NodeKind::StructDecl ||
             target->kind == NodeKind::ClassDecl ||
             target->kind == NodeKind::UnionDecl ||
             target->kind == NodeKind::EnumDecl) {
    ty = ctx->astCtx.VoidTy;
  } else {
    return ctx->reportError(node->line, node->column, node->length,
                            "Identifier '" + std::string(node->name) +
                                "' cannot be evaluated as an expression");
  }

  const_cast<VariableNode *>(node)->resolvedDecl = target;
  node->exprType = ty;
  node->isLValue = true;
  checkDeprecated(target, node);
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
  ScopeGuard guard(*ctx, ScopeKind::ControlFlowInit);

  if (node->initStatement) {
    auto initRes = dispatch(node->initStatement);
    if (!initRes)
      return std::unexpected(initRes.error());

    checkNodiscard(node->initStatement);
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

    checkNodiscard(node->increment);
  }

  LoopGuard loopGuard(*ctx);
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

  LoopGuard loopGuard(*ctx);
  auto bodyRes = dispatch(node->body);
  if (!bodyRes)
    return bodyRes;

  return ctx->astCtx.VoidTy;
}

SemaResult TypeCheckPass::visit(const SwitchNode *node) {
  auto condRes = dispatch(node->condition);
  if (!condRes)
    return std::unexpected(condRes.error());

  const Type *condTy = *condRes;
  if (!condTy->isInteger() && condTy->getKind() != TypeKind::Enum) {
    return ctx->reportError(
        node->condition->line, node->condition->column, node->condition->length,
        "Switch condition must be an integer or enum type.");
  }

  SwitchGuard switchGuard(*ctx);
  ScopeGuard scopeGuard(
      *ctx); /* Implicitly groups entire switch body as C/C++ */

  bool hasErrors = false;
  for (auto *c : node->cases) {
    if (c->value) {
      auto valRes = dispatch(c->value);
      if (!valRes) {
        hasErrors = true;
      } else {
        if (!canImplicitlyCast(*valRes, condTy)) {
          ctx->reportError(
              c->value->line, c->value->column, c->value->length,
              "Case value type does not match switch condition type.");
          hasErrors = true;
        } else {
          c->value = performImplicitConversion(c->value, condTy);
        }
      }
    }

    for (auto *s : c->statements) {
      if (!dispatch(s))
        hasErrors = true;

      checkNodiscard(s);
    }
  }

  if (hasErrors) {
    return std::unexpected(ErrorInfo{node->line, node->column, node->length,
                                     "Errors inside switch statement"});
  }

  return ctx->astCtx.VoidTy;
}

SemaResult TypeCheckPass::visit(const BreakNode *node) {
  if (!ctx->isInBreakable()) {
    return ctx->reportError(
        node->line, node->column, node->length,
        "Break statement outside of loop or switch control flow.");
  }
  return ctx->astCtx.VoidTy;
}

SemaResult TypeCheckPass::visit(const ContinueNode *node) {
  if (!ctx->isInLoop()) {
    return ctx->reportError(node->line, node->column, node->length,
                            "Continue statement outside of loop control flow.");
  }
  return ctx->astCtx.VoidTy;
}

SemaResult TypeCheckPass::visit(const UnaryOpNode *node) {
  auto exprType = dispatch(node->expr);
  if (!exprType)
    return std::unexpected(ErrorInfo{node->line, node->column, node->length,
                                     "Cascading error in unary op"});

  if (node->op == "++" || node->op == "--" || node->op == "-" ||
      node->op == "+" || node->op == "~" || node->op == "!") {
    if (auto *opDecl = resolveOverloadedOperator(*exprType, node->op, {})) {
      if (!(opDecl->isMethod && !opDecl->isStatic)) {
        const_cast<UnaryOpNode *>(node)->expr =
            performImplicitConversion(node->expr, opDecl->params[0]->type);
      }
      node->overloadedOperator = opDecl;
      node->exprType = opDecl->returnType;
      return opDecl->returnType;
    }
  }

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
        node->expr->kind != NodeKind::FunctionCall &&
        node->expr->kind != NodeKind::MemberAccess &&
        node->expr->kind != NodeKind::ArraySubscript) {
      return ctx->reportError(node->line, node->column, node->length,
                              "Cannot take address of r-value");
    }

    const Type *baseTy = *exprType;
    if (baseTy->isReferenceType()) {
      baseTy = static_cast<const ReferenceType *>(baseTy)->getPointeeType();
    } else if (baseTy->getKind() == TypeKind::RValueReference) {
      baseTy =
          static_cast<const RValueReferenceType *>(baseTy)->getPointeeType();
    }
    resType = ctx->astCtx.getPointerType(baseTy);
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
  } else if (node->op == "~") {
    if (!(*exprType)->isInteger()) {
      return ctx->reportError(
          node->line, node->column, node->length,
          "Bitwise NOT operator '~' requires an integer operand");
    }
    resType = *exprType;
  } else if (node->op == "++" || node->op == "--") {
    if (!(*exprType)->isNumeric()) {
      return ctx->reportError(node->line, node->column, node->length,
                              "Unary operator '" + std::string(node->op) +
                                  "' requires a numeric operand");
    }
    if (node->expr->kind != NodeKind::Variable &&
        node->expr->kind != NodeKind::MemberAccess &&
        node->expr->kind != NodeKind::ArraySubscript &&
        !(node->expr->kind == NodeKind::UnaryOp &&
          static_cast<const UnaryOpNode *>(node->expr)->op == "*")) {
      return ctx->reportError(
          node->line, node->column, node->length,
          "Expression is not assignable (must be an l-value)");
    }
    if ((*exprType)->isConstQualified() ||
        ((*exprType)->isReferenceType() &&
         static_cast<const ReferenceType *>(*exprType)
             ->getPointeeType()
             ->isConstQualified())) {
      return ctx->reportError(node->line, node->column, node->length,
                              "Cannot modify a constant variable");
    }
    resType = *exprType;
  } else {
    return ctx->reportError(node->line, node->column, node->length,
                            "Unknown unary operator");
  }

  node->exprType = resType;
  if (node->op == "*")
    node->isLValue = true;
  return resType;
}

SemaResult TypeCheckPass::visit(const BinaryOpNode *node) {
  auto lhs = dispatch(node->left);
  auto rhs = dispatch(node->right);

  if (!lhs || !rhs)
    return std::unexpected(ErrorInfo{node->line, node->column, node->length,
                                     "Invalid operands for binary operation"});

  if (auto *opDecl = resolveOverloadedOperator(*lhs, node->op, {node->right})) {
    if (opDecl->isMethod && !opDecl->isStatic) {
      const_cast<BinaryOpNode *>(node)->right =
          performImplicitConversion(node->right, opDecl->params[0]->type);
    } else {
      const_cast<BinaryOpNode *>(node)->left =
          performImplicitConversion(node->left, opDecl->params[0]->type);
      const_cast<BinaryOpNode *>(node)->right =
          performImplicitConversion(node->right, opDecl->params[1]->type);
    }
    node->overloadedOperator = opDecl;
    node->exprType = opDecl->returnType;
    return opDecl->returnType;
  }

  OpCategory cat = OpCategory::Unknown;
  if (auto it = opCategoryMap.find(node->op); it != opCategoryMap.end()) {
    cat = it->second;
  }

  if (cat == OpCategory::Logical) {
    if (!canImplicitlyCast(*lhs, ctx->astCtx.BoolTy) ||
        !canImplicitlyCast(*rhs, ctx->astCtx.BoolTy)) {
      return ctx->reportError(node->line, node->column, node->length,
                              "Logical operations require boolean operands.");
    }
    node->exprType = ctx->astCtx.BoolTy;
    return ctx->astCtx.BoolTy;
  }

  if (cat == OpCategory::Relational) {
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

    const Type *res = nullptr;
    if ((*lhs)->isNumeric() && (*rhs)->isNumeric()) {
      res = ctx->astCtx.getPromotedNumericType(*lhs, *rhs);
      checkImplicitCastWarning(*lhs, res, node->left);
      checkImplicitCastWarning(*rhs, res, node->right);
    } else {
      res = (*lhs)->isFloat() ? *lhs : *rhs;
    }

    node->promotedType = res;
    node->exprType = ctx->astCtx.BoolTy;
    return ctx->astCtx.BoolTy;
  }

  if (cat == OpCategory::BitwiseOrModulo) {
    if (!(*lhs)->isInteger() || !(*rhs)->isInteger()) {
      return ctx->reportError(
          node->line, node->column, node->length,
          "Bitwise and modulo operations require integer operands.");
    }
  } else if (cat == OpCategory::Arithmetic) {
    if (!(*lhs)->isNumeric() || !(*rhs)->isNumeric()) {
      return ctx->reportError(node->line, node->column, node->length,
                              "Binary arithmetic operations are currently "
                              "restricted to numeric types.");
    }
  } else {
    return ctx->reportError(node->line, node->column, node->length,
                            "Unknown binary operator.");
  }

  if (!canImplicitlyCast(*lhs, *rhs)) {
    return ctx->reportError(node->line, node->column, node->length,
                            "Type mismatch: " + (*lhs)->toString() + " vs " +
                                (*rhs)->toString());
  }

  const Type *res = nullptr;
  if ((*lhs)->isNumeric() && (*rhs)->isNumeric()) {
    res = ctx->astCtx.getPromotedNumericType(*lhs, *rhs);
    checkImplicitCastWarning(*lhs, res, node->left);
    checkImplicitCastWarning(*rhs, res, node->right);
  } else {
    res = (*lhs)->isFloat() ? *lhs : *rhs;
  }

  node->promotedType = res;
  node->exprType = res;
  return res;
}

SemaResult TypeCheckPass::visit(const VarDeclNode *node) {
  const_cast<VarDeclNode *>(node)->type = resolveIfTemplate(node->type);
  const Type *declType = node->type;

  /* Check for strictly unresolved types delegated safely from the parser */
  if (declType->getKind() == TypeKind::TemplateParam) {
    return ctx->reportError(node->line, node->column, node->length,
                            "Unknown type: '" + declType->toString() + "'");
  }

  if (!checkTypeVisibility(declType, node)) {
    return std::unexpected(ErrorInfo{node->line, node->column, node->length,
                                     "Type visibility error"});
  }

  // Prevent variables of type 'void'
  if (declType->isVoid()) {
    return ctx->reportError(node->line, node->column, node->length,
                            "Variables cannot be of type 'void'");
  }

  const Type *baseUnqualTy = declType->getUnqualifiedType();
  while (baseUnqualTy->getKind() == TypeKind::Array) {
    baseUnqualTy = static_cast<const ArrayType *>(baseUnqualTy)
                       ->getElementType()
                       ->getUnqualifiedType();
  }
  if (baseUnqualTy->getKind() == TypeKind::Struct ||
      baseUnqualTy->getKind() == TypeKind::Class ||
      baseUnqualTy->getKind() == TypeKind::Union) {
    auto *recTy = static_cast<const RecordType *>(baseUnqualTy);
    if (recTy->isOpaque()) {
      return ctx->reportError(
          node->line, node->column, node->length,
          "Cannot declare variable of incomplete (opaque) type.");
    }
  }

  for (const auto *ann : node->annotations) {
    if (ann->name == "weak") {
      const_cast<VarDeclNode *>(node)->isWeak = true;
    } else if (ann->name == "align") {
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
      !declType->isReferenceType() &&
      declType->getKind() != TypeKind::RValueReference) {
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
    } else if (declType->getKind() == TypeKind::RValueReference) {
      if (node->initializer->isLValue) {
        SemaResult err =
            ctx->reportError(node->line, node->column, node->length,
                             "Cannot bind an l-value to an r-value reference.");
      }
    } else {
      if (!canImplicitlyCast(*initRes, declType)) {
        std::string initTypeStr = *initRes ? (*initRes)->toString() : "unknown";
        SemaResult err = ctx->reportError(
            node->line, node->column, node->length,
            "Cannot initialize variable of type '" + declType->toString() +
                "' with type '" + initTypeStr + "'");
      } else {
        checkImplicitCastWarning(*initRes, declType, node->initializer);
        const_cast<VarDeclNode *>(node)->initializer =
            performImplicitConversion(node->initializer, declType);

        /* Evaluate deep copy construction to prevent shallow copy of aggregates
         * with destructors */
        if (baseUnqualTy->getKind() == TypeKind::Class ||
            baseUnqualTy->getKind() == TypeKind::Struct ||
            baseUnqualTy->getKind() == TypeKind::Union) {

          /* Extract the correctly promoted expression type post-conversion */
          const Type *initTypeStrp = node->initializer->exprType;
          if (initTypeStrp->isReferenceType()) {
            initTypeStrp = static_cast<const ReferenceType *>(initTypeStrp)
                               ->getPointeeType();
          } else if (initTypeStrp->getKind() == TypeKind::RValueReference) {
            initTypeStrp =
                static_cast<const RValueReferenceType *>(initTypeStrp)
                    ->getPointeeType();
          }

          const Type *initUnqual = initTypeStrp->getUnqualifiedType();

          if (baseUnqualTy == initUnqual) {
            auto *recTy = static_cast<const RecordType *>(baseUnqualTy);
            if (auto *decl = recTy->getDeclaration()) {
              llvm::ArrayRef<FunctionDeclNode *> ctors;
              if (decl->kind == NodeKind::ClassDecl)
                ctors = static_cast<const ClassDeclNode *>(decl)->constructors;
              else if (decl->kind == NodeKind::StructDecl)
                ctors = static_cast<const StructDeclNode *>(decl)->constructors;
              else if (decl->kind == NodeKind::UnionDecl)
                ctors = static_cast<const UnionDeclNode *>(decl)->constructors;

              const FunctionDeclNode *copyCtor = nullptr;
              int bestScore = -1;

              for (auto *ctor : ctors) {
                if (ctor->params.size() == 1) {
                  const Type *pType = ctor->params[0]->type;
                  const Type *pointee = nullptr;

                  if (pType->isReferenceType()) {
                    pointee = static_cast<const ReferenceType *>(pType)
                                  ->getPointeeType();
                  } else if (pType->getKind() == TypeKind::RValueReference) {
                    pointee = static_cast<const RValueReferenceType *>(pType)
                                  ->getPointeeType();
                  }

                  if (pointee &&
                      pointee->getUnqualifiedType() == baseUnqualTy) {
                    bool isLValue = node->initializer->isLValue;
                    int currentScore = 0;
                    bool match = true;

                    if (pType->getKind() == TypeKind::RValueReference) {
                      if (isLValue) {
                        match = false;
                      } else {
                        currentScore = 3;
                      }
                    } else if (pType->isReferenceType()) {
                      if (!pointee->isConstQualified()) {
                        if (!isLValue) {
                          match = false;
                        } else {
                          currentScore = 3;
                        }
                      } else {
                        currentScore = 2;
                      }
                    } else {
                      currentScore = 1;
                    }

                    if (match && currentScore > bestScore) {
                      bestScore = currentScore;
                      copyCtor = ctor;
                    }
                  }
                }
              }

              if (copyCtor) {
                const_cast<VarDeclNode *>(node)->copyCtor = copyCtor;
              } else {
                const FunctionDeclNode *dtor = nullptr;
                if (decl->kind == NodeKind::ClassDecl)
                  dtor = static_cast<const ClassDeclNode *>(decl)->destructor;
                else if (decl->kind == NodeKind::StructDecl)
                  dtor = static_cast<const StructDeclNode *>(decl)->destructor;
                else if (decl->kind == NodeKind::UnionDecl)
                  dtor = static_cast<const UnionDeclNode *>(decl)->destructor;

                if (dtor && !dtor->isImplicit) {
                  return ctx->reportError(
                      node->line, node->column, node->length,
                      "Cannot implicitly copy a record with a custom "
                      "destructor. A copy constructor is required.");
                }
              }
            }
          }
        }
      }
    }
  } else if (declType->isReferenceType() ||
             declType->getKind() == TypeKind::RValueReference) {
    SemaResult err =
        ctx->reportError(node->line, node->column, node->length,
                         "References must be initialized upon declaration.");
  }

  if (ctx->getScopeDepth() > 1) {
    ctx->addDecl(node->varName, node);
  }
  return declType;
}

SemaResult TypeCheckPass::visit(const AssignNode *node) {
  bool prevAssignTarget = ctx->isAssignTarget;
  if (node->op == "=") {
    ctx->isAssignTarget = true;
  }
  auto lhsType = dispatch(node->target);
  ctx->isAssignTarget = prevAssignTarget;

  auto rhsType = dispatch(node->value);

  if (!lhsType || !rhsType)
    return std::unexpected(ErrorInfo{node->line, node->column, node->length,
                                     "Cascading error in assignment"});

  if (auto *opDecl =
          resolveOverloadedOperator(*lhsType, node->op, {node->value})) {
    if (opDecl->isMethod && !opDecl->isStatic) {
      const_cast<AssignNode *>(node)->value =
          performImplicitConversion(node->value, opDecl->params[0]->type);
    } else {
      const_cast<AssignNode *>(node)->target =
          performImplicitConversion(node->target, opDecl->params[0]->type);
      const_cast<AssignNode *>(node)->value =
          performImplicitConversion(node->value, opDecl->params[1]->type);
    }
    node->overloadedOperator = opDecl;
    node->exprType = opDecl->returnType;
    node->isLValue = true;
    return opDecl->returnType;
  }

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

  if (node->op != "=") {
    std::string_view binOp = node->op.substr(0, node->op.length() - 1);

    AssignCategory cat = AssignCategory::Unknown;
    if (auto it = assignCatMap.find(binOp); it != assignCatMap.end()) {
      cat = it->second;
    }

    if (cat == AssignCategory::BitwiseOrModulo) {
      if (!(*lhsType)->isInteger() || !(*rhsType)->isInteger()) {
        return ctx->reportError(
            node->line, node->column, node->length,
            "Bitwise and modulo assignments require integer operands.");
      }
    } else if (cat == AssignCategory::Arithmetic) {
      if (!(*lhsType)->isNumeric() || !(*rhsType)->isNumeric()) {
        return ctx->reportError(
            node->line, node->column, node->length,
            "Arithmetic assignments require numeric operands.");
      }
    } else {
      return ctx->reportError(node->line, node->column, node->length,
                              "Unknown assignment operator.");
    }
  }

  if (!canImplicitlyCast(*rhsType, *lhsType)) {
    return ctx->reportError(node->line, node->column, node->length,
                            "Invalid assignment. Type mismatch.");
  }

  checkImplicitCastWarning(*rhsType, *lhsType, node->value);
  const_cast<AssignNode *>(node)->value =
      performImplicitConversion(node->value, *lhsType);

  const Type *baseLhs = *lhsType;
  if (baseLhs->isReferenceType()) {
    baseLhs = static_cast<const ReferenceType *>(baseLhs)->getPointeeType();
  }

  const Type *unqualTargetTy = node->target->exprType->getUnqualifiedType();
  bool isAggregate = (unqualTargetTy->getKind() == TypeKind::Struct ||
                      unqualTargetTy->getKind() == TypeKind::Class ||
                      unqualTargetTy->getKind() == TypeKind::Union ||
                      unqualTargetTy->getKind() == TypeKind::Array);

  const Type *unqualLhs = baseLhs->getUnqualifiedType();
  if (unqualLhs->getKind() == TypeKind::Class ||
      unqualLhs->getKind() == TypeKind::Struct ||
      unqualLhs->getKind() == TypeKind::Union) {
    auto *recTy = static_cast<const RecordType *>(unqualLhs);
    if (auto *decl = recTy->getDeclaration()) {
      const FunctionDeclNode *dtor = nullptr;
      if (decl->kind == NodeKind::ClassDecl)
        dtor = static_cast<const ClassDeclNode *>(decl)->destructor;
      else if (decl->kind == NodeKind::StructDecl)
        dtor = static_cast<const StructDeclNode *>(decl)->destructor;
      else if (decl->kind == NodeKind::UnionDecl)
        dtor = static_cast<const UnionDeclNode *>(decl)->destructor;

      if (dtor && !dtor->isImplicit) {
        return ctx->reportError(
            node->line, node->column, node->length,
            "Cannot implicitly copy-assign a record with a custom destructor. "
            "An overloaded operator= is required.");
      }
    }
  }

  node->exprType = *lhsType;
  node->isLValue = true;
  return *lhsType;
}

SemaResult TypeCheckPass::visit(const BlockNode *node) {
  ScopeGuard guard(*ctx);
  bool hasErrors = false;
  for (const auto &stmt : node->statements) {
    auto res = dispatch(stmt);
    if (!res)
      hasErrors = true;

    checkNodiscard(stmt);
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

  const DeclNode *staticAccessDecl = nullptr;
  const RecordType *recordTy = nullptr;

  if (node->object->kind == NodeKind::Variable) {
    auto varNode = static_cast<const VariableNode *>(node->object);
    if (varNode->resolvedDecl) {
      if (varNode->resolvedDecl->kind == NodeKind::EnumDecl) {
        auto enumDecl =
            static_cast<const EnumDeclNode *>(varNode->resolvedDecl);
        for (auto *mem : enumDecl->members) {
          if (mem->name == node->memberName) {
            if (!mem->isPublic(mem->name) &&
                enumDecl->declFilePath != ctx->currentFile) {
              return ctx->reportError(node->line, node->column, node->length,
                                      "Cannot access private enum member '" +
                                          std::string(mem->name) +
                                          "' from outside its file.");
            }
            const_cast<MemberAccessNode *>(node)->isEnumMember = true;
            const_cast<MemberAccessNode *>(node)->enumMember = mem;
            node->exprType = enumDecl->enumType;
            checkDeprecated(mem, node);
            return node->exprType;
          }
        }
        return ctx->reportError(node->line, node->column, node->length,
                                "Enum '" + std::string(enumDecl->name) +
                                    "' does not contain member '" +
                                    std::string(node->memberName) + "'");
      } else if (varNode->resolvedDecl->kind == NodeKind::ClassDecl ||
                 varNode->resolvedDecl->kind == NodeKind::StructDecl ||
                 varNode->resolvedDecl->kind == NodeKind::UnionDecl) {
        staticAccessDecl = varNode->resolvedDecl;
        if (staticAccessDecl->kind == NodeKind::ClassDecl)
          recordTy =
              static_cast<const ClassDeclNode *>(staticAccessDecl)->recordType;
        else if (staticAccessDecl->kind == NodeKind::StructDecl)
          recordTy =
              static_cast<const StructDeclNode *>(staticAccessDecl)->recordType;
        else
          recordTy =
              static_cast<const UnionDeclNode *>(staticAccessDecl)->recordType;
      }
    }
  }

  if (!staticAccessDecl) {
    const Type *baseTy = *objType;
    const Type *unqualObj = baseTy->getUnqualifiedType();

    if (unqualObj->isPointerType())
      baseTy = static_cast<const PointerType *>(unqualObj)->getPointeeType();
    else if (unqualObj->isReferenceType())
      baseTy = static_cast<const ReferenceType *>(unqualObj)->getPointeeType();
    else if (unqualObj->getKind() == TypeKind::RValueReference)
      baseTy =
          static_cast<const RValueReferenceType *>(unqualObj)->getPointeeType();

    const Type *unqualBaseTy = baseTy->getUnqualifiedType();

    if (unqualBaseTy->getKind() != TypeKind::Struct &&
        unqualBaseTy->getKind() != TypeKind::Class &&
        unqualBaseTy->getKind() != TypeKind::Union) {
      return ctx->reportError(node->line, node->column, node->length,
                              "Member access on non-record type");
    }

    recordTy = static_cast<const RecordType *>(unqualBaseTy);

    if (recordTy->isOpaque()) {
      return ctx->reportError(
          node->line, node->column, node->length,
          "Cannot access member of incomplete (opaque) type");
    }

    if (auto field = recordTy->getField(node->memberName)) {
      if (!field->isPublic && ctx->getCurrentRecordContext() != recordTy) {
        return ctx->reportError(node->line, node->column, node->length,
                                "Cannot access private field '" +
                                    std::string(node->memberName) + "'");
      }
      const_cast<MemberAccessNode *>(node)->fieldIndex = field->index;
      node->exprType = field->type;
      node->isLValue = true;

      if (const DeclNode *recDecl = recordTy->getDeclaration()) {
        llvm::ArrayRef<VarDeclNode *> fields;
        if (recDecl->kind == NodeKind::ClassDecl)
          fields = static_cast<const ClassDeclNode *>(recDecl)->fields;
        else if (recDecl->kind == NodeKind::StructDecl)
          fields = static_cast<const StructDeclNode *>(recDecl)->fields;
        else if (recDecl->kind == NodeKind::UnionDecl)
          fields = static_cast<const UnionDeclNode *>(recDecl)->fields;

        for (const auto *fDecl : fields) {
          if (fDecl->varName == node->memberName) {
            checkDeprecated(fDecl, node);
            break;
          }
        }
      }

      return field->type;
    }
  }

  if (recordTy) {
    auto recordDecls = ctx->lookup(recordTy->getName());
    const DeclNode *recDecl = nullptr;
    for (auto *d : recordDecls) {
      if (d->kind == NodeKind::ClassDecl || d->kind == NodeKind::StructDecl ||
          d->kind == NodeKind::UnionDecl) {
        recDecl = d;
        break;
      }
    }

    if (recDecl) {
      if (staticAccessDecl) {
        llvm::ArrayRef<VarDeclNode *> staticFields;
        if (staticAccessDecl->kind == NodeKind::ClassDecl)
          staticFields =
              static_cast<const ClassDeclNode *>(staticAccessDecl)->fields;
        else if (staticAccessDecl->kind == NodeKind::StructDecl)
          staticFields =
              static_cast<const StructDeclNode *>(staticAccessDecl)->fields;
        else
          staticFields =
              static_cast<const UnionDeclNode *>(staticAccessDecl)->fields;

        for (const auto *f : staticFields) {
          if (f->varName == node->memberName) {
            if (!f->isStatic) {
              return ctx->reportError(node->line, node->column, node->length,
                                      "Cannot access non-static field '" +
                                          std::string(f->varName) +
                                          "' without an instance.");
            }
            if (!f->isPublic(f->varName) &&
                ctx->getCurrentRecordContext() != recordTy) {
              return ctx->reportError(node->line, node->column, node->length,
                                      "Cannot access private static field '" +
                                          std::string(f->varName) + "'.");
            }
            const_cast<MemberAccessNode *>(node)->isStaticFieldRef = true;
            const_cast<MemberAccessNode *>(node)->resolvedVar = f;
            node->exprType = f->type;
            checkDeprecated(f, node);
            return f->type;
          }
        }
      }

      llvm::ArrayRef<FunctionDeclNode *> methods;
      if (recDecl->kind == NodeKind::ClassDecl)
        methods = static_cast<const ClassDeclNode *>(recDecl)->methods;
      else if (recDecl->kind == NodeKind::StructDecl)
        methods = static_cast<const StructDeclNode *>(recDecl)->methods;
      else
        methods = static_cast<const UnionDeclNode *>(recDecl)->methods;

      if (!node->templateArgs.empty()) {
        FunctionDeclNode *tmplDecl = nullptr;
        for (auto *m : methods) {
          if (m->name == node->memberName && m->isTemplate) {
            tmplDecl = m;
            break;
          }
        }

        if (tmplDecl) {
          std::string mangledName = std::string(node->memberName);
          for (const auto *arg : node->templateArgs) {
            const Type *resArg = resolveIfTemplate(arg);
            std::string argStr = resArg->toString();
            for (char &c : argStr) {
              if (!isalnum(c))
                c = '_';
            }
            mangledName += "_" + argStr;
          }
          std::string_view mangledView = ctx->astCtx.copyString(mangledName);

          bool alreadyInstantiated = false;
          for (auto *m : methods) {
            if (m->name == mangledView) {
              alreadyInstantiated = true;
              break;
            }
          }

          if (!alreadyInstantiated) {
            std::unordered_map<std::string_view, const Type *> templateArgMap;
            for (size_t i = 0; i < tmplDecl->templateParams.size(); ++i) {
              templateArgMap[tmplDecl->templateParams[i]] =
                  resolveIfTemplate(node->templateArgs[i]);
            }

            ASTCloner cloner(ctx->astCtx, templateArgMap);
            DeclNode *instDecl =
                static_cast<DeclNode *>(cloner.dispatch(tmplDecl));

            if (instDecl && instDecl->kind == NodeKind::FunctionDecl) {
              auto *fnDecl = static_cast<FunctionDeclNode *>(instDecl);
              fnDecl->name = mangledView;
              fnDecl->isMethod = true;
              fnDecl->isStatic = tmplDecl->isStatic;
              fnDecl->hasPublicMod = tmplDecl->hasPublicMod;
              fnDecl->hasPrivateMod = tmplDecl->hasPrivateMod;
              fnDecl->annotations = tmplDecl->annotations;
              fnDecl->declFilePath = tmplDecl->declFilePath;

              /* Prevent duplicate 'this' pointers from being injected.
               * Instead, safely reinterpret the existing cloned pointer. */
              if (!fnDecl->isStatic && !fnDecl->params.empty() &&
                  fnDecl->params.front()->name == "this") {
                const_cast<ParamDeclNode *>(fnDecl->params.front())->type =
                    ctx->astCtx.getPointerType(recordTy);
              }

              std::vector<FunctionDeclNode *> updatedMethods(methods.begin(),
                                                             methods.end());
              updatedMethods.push_back(fnDecl);

              if (recDecl->kind == NodeKind::ClassDecl) {
                methods =
                    ctx->astCtx.copyArray<FunctionDeclNode *>(updatedMethods);
                const_cast<ClassDeclNode *>(
                    static_cast<const ClassDeclNode *>(recDecl))
                    ->methods = methods;
              } else if (recDecl->kind == NodeKind::StructDecl) {
                methods =
                    ctx->astCtx.copyArray<FunctionDeclNode *>(updatedMethods);
                const_cast<StructDeclNode *>(
                    static_cast<const StructDeclNode *>(recDecl))
                    ->methods = methods;
              } else {
                methods =
                    ctx->astCtx.copyArray<FunctionDeclNode *>(updatedMethods);
                const_cast<UnionDeclNode *>(
                    static_cast<const UnionDeclNode *>(recDecl))
                    ->methods = methods;
              }

              fnDecl->mangledName =
                  Mangler::mangle(fnDecl, std::string(recordTy->getName()));

              auto prevContext = ctx->getCurrentRecordContext();
              auto prevFile = ctx->currentFile;

              ctx->setCurrentRecordContext(recordTy);
              ctx->setCurrentFile(fnDecl->declFilePath);

              dispatch(fnDecl);

              ctx->setCurrentFile(prevFile);
              ctx->setCurrentRecordContext(prevContext);
            }
          }
          const_cast<MemberAccessNode *>(node)->memberName = mangledView;
        }
      }

      for (const auto *method : methods) {
        if (method->name == node->memberName) {
          if (staticAccessDecl && !method->isStatic) {
            return ctx->reportError(node->line, node->column, node->length,
                                    "Cannot access non-static method '" +
                                        std::string(method->name) +
                                        "' without an instance.");
          }
          if (!staticAccessDecl && method->isStatic) {
            return ctx->reportError(node->line, node->column, node->length,
                                    "Cannot access static method '" +
                                        std::string(method->name) +
                                        "' via an instance.");
          }
          if (!method->isPublic(method->name) &&
              ctx->getCurrentRecordContext() != recordTy) {
            return ctx->reportError(node->line, node->column, node->length,
                                    "Cannot access private method '" +
                                        std::string(method->name) + "'");
          }
          const_cast<MemberAccessNode *>(node)->isMethodRef = true;
          const_cast<MemberAccessNode *>(node)->resolvedMethod = method;
          node->exprType = ctx->astCtx.VoidTy;
          checkDeprecated(method, node);
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
  const_cast<ParamDeclNode *>(node)->type = resolveIfTemplate(node->type);

  if (node->type->getKind() == TypeKind::TemplateParam) {
    return ctx->reportError(node->line, node->column, node->length,
                            "Unknown type: '" + node->type->toString() + "'");
  }

  if (!checkTypeVisibility(node->type, node)) {
    return std::unexpected(ErrorInfo{node->line, node->column, node->length,
                                     "Type visibility error"});
  }

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
  if (node->isTemplate)
    return ctx->astCtx.VoidTy;

  const_cast<FunctionDeclNode *>(node)->returnType =
      resolveIfTemplate(node->returnType);

  if (node->returnType->getKind() == TypeKind::TemplateParam) {
    return ctx->reportError(node->line, node->column, node->length,
                            "Unknown return type: '" +
                                node->returnType->toString() + "'");
  }

  if (!checkTypeVisibility(node->returnType, node)) {
    return std::unexpected(ErrorInfo{node->line, node->column, node->length,
                                     "Type visibility error"});
  }

  if (node->name == "main" && !node->isMethod) {
    const Type *unqualRet = node->returnType->getUnqualifiedType();
    bool validRet = false;

    if (unqualRet->isBuiltinType()) {
      auto bKind =
          static_cast<const BuiltinType *>(unqualRet)->getBuiltinKind();
      if (bKind == BuiltinKind::Int32 || bKind == BuiltinKind::Void) {
        validRet = true;
      }
    }

    if (!validRet) {
      ctx->reportError(node->line, node->column, node->length,
                       "The 'main' function must return 'int32' or 'void'.");
    }

    if (!node->params.empty()) {
      if (node->params.size() != 2) {
        ctx->reportError(node->line, node->column, node->length,
                         "The 'main' function must take either 0 arguments or "
                         "exactly 2: (int32 argc, uint8** argv).");
      } else {
        const Type *p0 = node->params[0]->type->getUnqualifiedType();
        const Type *p1 = node->params[1]->type->getUnqualifiedType();

        bool p0Valid = p0->isBuiltinType() &&
                       static_cast<const BuiltinType *>(p0)->getBuiltinKind() ==
                           BuiltinKind::Int32;
        bool p1Valid = false;

        if (p1->isPointerType()) {
          const Type *p1Base = static_cast<const PointerType *>(p1)
                                   ->getPointeeType()
                                   ->getUnqualifiedType();
          if (p1Base->isPointerType()) {
            const Type *p1BaseBase = static_cast<const PointerType *>(p1Base)
                                         ->getPointeeType()
                                         ->getUnqualifiedType();
            if (p1BaseBase->isBuiltinType() &&
                static_cast<const BuiltinType *>(p1BaseBase)
                        ->getBuiltinKind() == BuiltinKind::UInt8) {
              p1Valid = true;
            }
          }
        } else if (p1->getKind() == TypeKind::Array) {
          const Type *p1Elem = static_cast<const ArrayType *>(p1)
                                   ->getElementType()
                                   ->getUnqualifiedType();
          if (p1Elem->isPointerType()) {
            const Type *p1ElemBase = static_cast<const PointerType *>(p1Elem)
                                         ->getPointeeType()
                                         ->getUnqualifiedType();
            if (p1ElemBase->isBuiltinType() &&
                static_cast<const BuiltinType *>(p1ElemBase)
                        ->getBuiltinKind() == BuiltinKind::UInt8) {
              p1Valid = true;
            }
          }
        }

        if (!p0Valid || !p1Valid) {
          ctx->reportError(node->line, node->column, node->length,
                           "The 'main' function arguments must be exactly "
                           "(int32 argc, uint8** argv).");
        }
      }
    }
  }

  const Type *prevRet = ctx->getFunctionReturnType();
  ctx->setFunctionReturnType(node->returnType);

  ScopeGuard guard(*ctx, ScopeKind::FunctionParams);
  bool hasErrors = false;

  if (node->isMethod && !node->isExtern && !node->isStatic &&
      node->parentRecord) {
    auto *thisParam = ctx->astCtx.create<ParamDeclNode>(
        ctx->astCtx.getPointerType(node->parentRecord), "this", nullptr, false,
        false, node->line, node->column, 4);
    ctx->addDecl("this", thisParam);
  }

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

  ctx->setFunctionReturnType(prevRet);

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

  if (hasErrors) {
    return std::unexpected(ErrorInfo{node->line, node->column, node->length,
                                     "Argument evaluation failed."});
  }

  auto checkMatch = [&](const FunctionDeclNode *fDecl, int &outScore,
                        std::vector<ExprNode *> &outResolvedArgs)
      -> std::vector<std::string> {
    outScore = 0;
    size_t expectedParams = fDecl->params.size();

    std::vector<ExprNode *> resolvedArgs(expectedParams, nullptr);
    std::vector<const Type *> resolvedTypes(expectedParams, nullptr);
    std::vector<bool> explicitlyProvided(expectedParams, false);

    size_t posArgCount = 0;
    std::unordered_set<std::string_view> providedNamedArgs;
    std::vector<std::string> errors;

    for (size_t i = 0; i < node->args.size(); ++i) {
      if (node->argNames.empty() || node->argNames[i].empty()) {
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
        if (fDecl->params[posArgCount]->isNamed) {
          errors.push_back(
              "Positional argument provided for named parameter '" +
              std::string(fDecl->params[posArgCount]->name) + "'.");
          return errors;
        }
        resolvedArgs[posArgCount] = node->args[i];
        resolvedTypes[posArgCount] = argTypes[i];
        explicitlyProvided[posArgCount] = true;
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
          if (fDecl->params[p]->name == name) {
            if (!fDecl->params[p]->isNamed) {
              errors.push_back("Parameter '" + std::string(name) +
                               "' cannot be passed as a named argument.");
            }
            resolvedArgs[p] = node->args[i];
            resolvedTypes[p] = argTypes[i];
            explicitlyProvided[p] = true;
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
        if (fDecl->params[p]->defaultValue) {
          auto defNode = fDecl->params[p]->defaultValue;
          if (!defNode->exprType) {
            dispatch(defNode);
          }
          resolvedArgs[p] = defNode;
          resolvedTypes[p] = defNode->exprType;
        } else {
          auto pName = std::string(fDecl->params[p]->name);
          if (fDecl->params[p]->isRequired) {
            errors.push_back("Missing required named parameter '" + pName +
                             "'.");
          } else if (!fDecl->params[p]->isNamed) {
            errors.push_back("Missing mandatory positional parameter '" +
                             pName + "'.");
          } else {
            const Type *pType = fDecl->params[p]->type;
            const Type *unqual = pType->getUnqualifiedType();
            if (unqual->isNumeric()) {
              auto num = ctx->astCtx.create<NumberNode>("0", unqual->isFloat(),
                                                        0, 0, 0);
              num->exprType = pType;
              resolvedArgs[p] = num;
              resolvedTypes[p] = pType;
            } else if (unqual->isBuiltinType() &&
                       static_cast<const BuiltinType *>(unqual)
                               ->getBuiltinKind() == BuiltinKind::Bool) {
              auto bNode = ctx->astCtx.create<BoolNode>(false, 0, 0, 0);
              bNode->exprType = pType;
              resolvedArgs[p] = bNode;
              resolvedTypes[p] = pType;
            } else {
              auto nNode = ctx->astCtx.create<NullNode>(0, 0, 0);
              nNode->exprType = pType;
              resolvedArgs[p] = nNode;
              resolvedTypes[p] = pType;
            }
          }
        }
      }
    }

    if (!errors.empty())
      return errors;

    for (size_t p = 0; p < expectedParams; ++p) {
      const Type *paramType = fDecl->params[p]->type;

      /* Array decay dynamically applied to matching criteria */
      if (paramType->getKind() == TypeKind::Array) {
        paramType = ctx->astCtx.getPointerType(
            static_cast<const ArrayType *>(paramType)->getElementType());
      }

      if (!canImplicitlyCast(resolvedTypes[p], paramType)) {
        std::string gotType = resolvedTypes[p] ? resolvedTypes[p]->toString()
                                               : "unresolved/unknown";
        errors.push_back("Type mismatch for parameter '" +
                         std::string(fDecl->params[p]->name) + "': expected '" +
                         paramType->toString() + "', but got '" + gotType +
                         "'.");
      } else {
        if (explicitlyProvided[p] &&
            canImplicitlyCast(resolvedTypes[p], paramType, false)) {
          outScore += 10;
        }

        bool isLValue = resolvedArgs[p]->isLValue;
        if (paramType->getKind() == TypeKind::RValueReference) {
          if (isLValue) {
            errors.push_back(
                "Cannot bind an l-value to r-value reference parameter '" +
                std::string(fDecl->params[p]->name) + "'.");
          } else if (explicitlyProvided[p]) {
            outScore += 3;
          }
        } else if (paramType->isReferenceType()) {
          const Type *pointee =
              static_cast<const ReferenceType *>(paramType)->getPointeeType();
          if (!pointee->isConstQualified()) {
            if (!isLValue) {
              errors.push_back(
                  "Cannot bind an r-value to non-const reference parameter '" +
                  std::string(fDecl->params[p]->name) + "'.");
            } else if (explicitlyProvided[p]) {
              outScore += 3;
            }
          } else if (explicitlyProvided[p]) {
            outScore += 2;
          }
        } else if (explicitlyProvided[p]) {
          outScore += 1;
        }

        /* Penalize overloads that implicitly fill parameters not explicitly
         * provided by the caller to prefer exact arity match. */
        if (!explicitlyProvided[p]) {
          outScore -= 1;
        }
      }
    }

    if (!errors.empty())
      return errors;

    for (size_t p = 0; p < expectedParams; ++p) {
      const Type *paramType = fDecl->params[p]->type;
      if (paramType->getKind() == TypeKind::Array) {
        paramType = ctx->astCtx.getPointerType(
            static_cast<const ArrayType *>(paramType)->getElementType());
      }
      checkImplicitCastWarning(resolvedTypes[p], paramType, resolvedArgs[p]);
      resolvedArgs[p] = performImplicitConversion(resolvedArgs[p], paramType);
    }

    outResolvedArgs = resolvedArgs;
    return errors;
  };

  if (node->target->kind == NodeKind::MemberAccess) {
    auto ma = static_cast<const MemberAccessNode *>(node->target);
    auto maRes = dispatch(ma);
    if (!maRes)
      return maRes;

    if (ma->isMethodRef) {
      const RecordType *recordTy = nullptr;
      const DeclNode *recDecl = nullptr;

      if (ma->object->kind == NodeKind::Variable) {
        auto varNode = static_cast<const VariableNode *>(ma->object);
        if (varNode->resolvedDecl &&
            (varNode->resolvedDecl->kind == NodeKind::ClassDecl ||
             varNode->resolvedDecl->kind == NodeKind::StructDecl)) {
          recDecl = varNode->resolvedDecl;
          if (recDecl->kind == NodeKind::ClassDecl)
            recordTy = static_cast<const ClassDeclNode *>(recDecl)->recordType;
          else
            recordTy = static_cast<const StructDeclNode *>(recDecl)->recordType;
        }
      }

      if (!recordTy) {
        const Type *baseTy = ma->object->exprType;
        const Type *unqualObj = baseTy->getUnqualifiedType();

        if (unqualObj->isPointerType())
          baseTy =
              static_cast<const PointerType *>(unqualObj)->getPointeeType();
        else if (unqualObj->isReferenceType())
          baseTy =
              static_cast<const ReferenceType *>(unqualObj)->getPointeeType();
        else if (unqualObj->getKind() == TypeKind::RValueReference)
          baseTy = static_cast<const RValueReferenceType *>(unqualObj)
                       ->getPointeeType();

        const Type *unqualBaseTy = baseTy->getUnqualifiedType();

        if (unqualBaseTy->getKind() == TypeKind::Struct ||
            unqualBaseTy->getKind() == TypeKind::Class) {
          recordTy = static_cast<const RecordType *>(unqualBaseTy);
          auto recordDecls = ctx->lookup(recordTy->getName());

          for (auto *d : recordDecls) {
            if (d->kind == NodeKind::ClassDecl ||
                d->kind == NodeKind::StructDecl) {
              recDecl = d;
              break;
            }
          }
        }
      }

      if (recDecl) {
        const FunctionDeclNode *bestMatch = nullptr;
        int bestScore = std::numeric_limits<int>::min();
        std::vector<std::vector<std::string>> overloadErrors;
        std::vector<ExprNode *> bestResolvedArgs;

        llvm::ArrayRef<FunctionDeclNode *> methods;
        if (recDecl->kind == NodeKind::ClassDecl)
          methods = static_cast<const ClassDeclNode *>(recDecl)->methods;
        else
          methods = static_cast<const StructDeclNode *>(recDecl)->methods;

        for (const auto *method : methods) {
          if (method->name == ma->memberName) {
            int score = 0;
            std::vector<ExprNode *> resolvedArgs;
            auto errs = checkMatch(method, score, resolvedArgs);
            if (errs.empty()) {
              if (score > bestScore) {
                bestScore = score;
                bestMatch = method;
                bestResolvedArgs = resolvedArgs;
                overloadErrors.clear();
              }
            } else {
              overloadErrors.push_back(errs);
            }
          }
        }
        if (bestMatch) {
          const_cast<MemberAccessNode *>(ma)->resolvedMethod = bestMatch;
          const_cast<FunctionCallNode *>(node)->resolvedFunc = bestMatch;

          const_cast<FunctionCallNode *>(node)->args =
              ctx->astCtx.copyArray<ExprNode *>(bestResolvedArgs);
          const_cast<FunctionCallNode *>(node)->argNames = {};

          node->exprType = bestMatch->returnType;
          node->isLValue = (node->exprType->isReferenceType());

          checkDeprecated(bestMatch, node);

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
            decls = aliased;
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
      int bestScore = std::numeric_limits<int>::min();
      bool isConstructorCall = false;
      std::vector<std::vector<std::string>> overloadErrors;
      std::vector<ExprNode *> bestResolvedArgs;

      for (auto targetDecl : decls) {
        if (targetDecl->kind == NodeKind::FunctionDecl) {
          auto fDecl = static_cast<const FunctionDeclNode *>(targetDecl);

          if (!fDecl->isPublic(fDecl->name)) {
            if (fDecl->isMethod && !fDecl->isExtern) {
              const RecordType *recTy = ctx->astCtx.getRecordType(name);
              if (recTy && ctx->getCurrentRecordContext() != recTy) {
                overloadErrors.push_back({"Constructor is private."});
                continue;
              }
            } else {
              if (fDecl->declFilePath != ctx->currentFile) {
                overloadErrors.push_back({"Function is private to its file."});
                continue;
              }
            }
          }

          int score = 0;
          std::vector<ExprNode *> resolvedArgs;
          auto errs = checkMatch(fDecl, score, resolvedArgs);
          if (errs.empty()) {
            if (score > bestScore) {
              bestScore = score;
              bestMatch = fDecl;
              bestResolvedArgs = resolvedArgs;
              if (fDecl->isMethod &&
                  ctx->astCtx.getRecordType(name) != nullptr) {
                isConstructorCall = true;
              }
              overloadErrors.clear();
            }
          } else {
            overloadErrors.push_back(errs);
          }
        }
      }

      if (bestMatch) {
        const_cast<FunctionCallNode *>(node)->resolvedFunc = bestMatch;
        const_cast<FunctionCallNode *>(node)->args =
            ctx->astCtx.copyArray<ExprNode *>(bestResolvedArgs);
        const_cast<FunctionCallNode *>(node)->argNames = {};

        if (isConstructorCall) {
          node->exprType = ctx->astCtx.getRecordType(name);
          node->isLValue = false;
        } else {
          node->exprType = bestMatch->returnType;
          node->isLValue = (node->exprType->isReferenceType());
        }

        if (node->target->kind == NodeKind::Variable) {
          const_cast<VariableNode *>(
              static_cast<const VariableNode *>(node->target))
              ->resolvedDecl = bestMatch;
        }

        checkDeprecated(bestMatch, node);

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

        std::vector<ExprNode *> resolvedArgs;
        for (size_t i = 0; i < node->args.size(); ++i) {
          resolvedArgs.push_back(performImplicitConversion(
              node->args[i], fTy->getParamTypes()[i]));
        }

        const_cast<FunctionCallNode *>(node)->args =
            ctx->astCtx.copyArray<ExprNode *>(resolvedArgs);
        node->exprType = fTy->getReturnType();
        node->isLValue = (node->exprType->isReferenceType());
        return node->exprType;
      }
    }
  }

  return ctx->reportError(node->line, node->column, node->length,
                          "Invalid function call target.");
}

SemaResult TypeCheckPass::visit(const CastNode *node) {
  const_cast<CastNode *>(node)->targetType =
      resolveIfTemplate(node->targetType);

  auto srcType = dispatch(node->expr);
  const Type *destType = node->targetType;

  if (!srcType)
    return std::unexpected(ErrorInfo{node->line, node->column, node->length,
                                     "Cascading error in cast"});

  const Type *srcUnqual = (*srcType)->getUnqualifiedType();
  const Type *destUnqual = destType->getUnqualifiedType();

  bool isSrcNumeric = srcUnqual->isNumeric();
  bool isDestNumeric = destUnqual->isNumeric();
  bool isSrcPtr = srcUnqual->isPointerType();
  bool isDestPtr = destUnqual->isPointerType();
  bool isSrcEnum = srcUnqual->getKind() == TypeKind::Enum;
  bool isDestEnum = destUnqual->getKind() == TypeKind::Enum;

  if ((isSrcNumeric && isDestNumeric) || (isSrcPtr && isDestPtr) ||
      (isSrcPtr && isDestNumeric) || (isSrcNumeric && isDestPtr) ||
      (isSrcEnum && isDestNumeric) || (isSrcNumeric && isDestEnum) ||
      (isSrcEnum && isDestEnum) ||
      destType->getKind() == TypeKind::RValueReference ||
      destType->isReferenceType()) {

    if (destType->getKind() == TypeKind::RValueReference) {
      node->isLValue = false;
    } else if (destType->isReferenceType()) {
      node->isLValue = true;
    } else {
      node->isLValue = false;
    }

    node->exprType = destType;
    return destType;
  }

  /* Explore explicit user-defined conversions via constructors.
   * Enables syntax like `Primitive as CustomType` if a matching constructor
   * exists. */
  if (destUnqual->getKind() == TypeKind::Class ||
      destUnqual->getKind() == TypeKind::Struct ||
      destUnqual->getKind() == TypeKind::Union) {
    auto *recTy = static_cast<const RecordType *>(destUnqual);
    if (auto *decl = recTy->getDeclaration()) {
      llvm::ArrayRef<FunctionDeclNode *> ctors;
      if (decl->kind == NodeKind::ClassDecl) {
        ctors = static_cast<const ClassDeclNode *>(decl)->constructors;
      } else if (decl->kind == NodeKind::StructDecl) {
        ctors = static_cast<const StructDeclNode *>(decl)->constructors;
      } else if (decl->kind == NodeKind::UnionDecl) {
        ctors = static_cast<const UnionDeclNode *>(decl)->constructors;
      }

      for (auto *ctor : ctors) {
        if (ctor->params.size() == 1 &&
            canImplicitlyCast(*srcType, ctor->params[0]->type, false)) {
          const_cast<CastNode *>(node)->conversionConstructor = ctor;
          node->exprType = destType;
          node->isLValue = false;
          return destType;
        }
      }
    }
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
  } else {
    checkImplicitCastWarning(*valType, expectedRet, node->value);
    const_cast<ReturnNode *>(node)->value =
        performImplicitConversion(node->value, expectedRet);
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

  std::vector<ExprNode *> promotedElements;
  for (const auto *elem : node->elements) {
    promotedElements.push_back(
        performImplicitConversion(const_cast<ExprNode *>(elem), elemType));
  }
  const_cast<ArrayLiteralNode *>(node)->elements =
      ctx->astCtx.copyArray<ExprNode *>(promotedElements);

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
  for (const auto *exp : node->exportedModules) {
    auto res = dispatch(exp);
    if (!res)
      hasErrors = true;
  }

  auto prevFile = ctx->currentFile;
  auto prevMod = ctx->currentModule;
  ctx->setCurrentFile(node->filePath);
  ctx->currentModule = node;

  for (const auto &stmt : node->statements) {
    auto res = dispatch(stmt);
    if (!res)
      hasErrors = true;

    checkNodiscard(stmt);
  }

  ctx->setCurrentFile(prevFile);
  ctx->currentModule = prevMod;

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

  if (auto *opDecl =
          resolveOverloadedOperator(*baseType, "[]", {node->index})) {
    if (opDecl->isMethod && !opDecl->isStatic) {
      const_cast<ArraySubscriptNode *>(node)->index =
          performImplicitConversion(node->index, opDecl->params[0]->type);
    } else {
      const_cast<ArraySubscriptNode *>(node)->base =
          performImplicitConversion(node->base, opDecl->params[0]->type);
      const_cast<ArraySubscriptNode *>(node)->index =
          performImplicitConversion(node->index, opDecl->params[1]->type);
    }
    node->overloadedOperator = opDecl;
    node->exprType = opDecl->returnType;
    node->isLValue = opDecl->returnType->isReferenceType();
    return opDecl->returnType;
  }

  const Type *unqualBase = (*baseType)->getUnqualifiedType();
  if (unqualBase->isPointerType()) {
    node->exprType =
        static_cast<const PointerType *>(unqualBase)->getPointeeType();
  } else if (unqualBase->getKind() == TypeKind::Array) {
    node->exprType =
        static_cast<const ArrayType *>(unqualBase)->getElementType();
  } else {
    return ctx->reportError(
        node->line, node->column, node->length,
        "Subscripted value is not an array, pointer, or class with operator[]");
  }

  if (!(*indexType)->isInteger()) {
    return ctx->reportError(node->line, node->column, node->length,
                            "Array subscript must be an integer");
  }

  node->isLValue = true;
  return node->exprType;
}

SemaResult TypeCheckPass::visit(const NewExprNode *node) {
  const_cast<NewExprNode *>(node)->allocatedType =
      resolveIfTemplate(node->allocatedType);

  if (!checkTypeVisibility(node->allocatedType, node)) {
    return std::unexpected(ErrorInfo{node->line, node->column, node->length,
                                     "Type visibility error"});
  }

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
  const Type *baseUnqualTy = unqual;
  while (baseUnqualTy->getKind() == TypeKind::Array) {
    baseUnqualTy = static_cast<const ArrayType *>(baseUnqualTy)
                       ->getElementType()
                       ->getUnqualifiedType();
  }

  if (baseUnqualTy->getKind() == TypeKind::Class ||
      baseUnqualTy->getKind() == TypeKind::Struct ||
      baseUnqualTy->getKind() == TypeKind::Union) {
    auto *recTy = static_cast<const RecordType *>(baseUnqualTy);

    if (recTy->isOpaque()) {
      return ctx->reportError(node->line, node->column, node->length,
                              "Cannot allocate incomplete (opaque) type.");
    }

    auto *decl = recTy->getDeclaration();

    if (decl) {
      llvm::ArrayRef<FunctionDeclNode *> ctors;
      if (decl->kind == NodeKind::ClassDecl)
        ctors = static_cast<const ClassDeclNode *>(decl)->constructors;
      else if (decl->kind == NodeKind::StructDecl)
        ctors = static_cast<const StructDeclNode *>(decl)->constructors;
      else if (decl->kind == NodeKind::UnionDecl)
        ctors = static_cast<const UnionDeclNode *>(decl)->constructors;

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
      int bestScore = std::numeric_limits<int>::min();
      std::vector<std::vector<std::string>> overloadErrors;
      std::vector<ExprNode *> bestResolvedArgs;

      for (const auto *ctor : ctors) {
        if (!ctor->isPublic(ctor->name) &&
            ctx->getCurrentRecordContext() != recTy) {
          overloadErrors.push_back({"Constructor is private."});
          continue;
        }

        size_t expectedParams = ctor->params.size();

        std::vector<ExprNode *> resolvedArgs(expectedParams, nullptr);
        std::vector<const Type *> resolvedTypes(expectedParams, nullptr);
        std::vector<bool> explicitlyProvided(expectedParams, false);

        size_t posArgCount = 0;
        std::unordered_set<std::string_view> providedNamedArgs;
        std::vector<std::string> errors;

        for (size_t i = 0; i < node->args.size(); ++i) {
          if (node->argNames.empty() || node->argNames[i].empty()) {
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
            if (ctor->params[posArgCount]->isNamed) {
              errors.push_back(
                  "Positional argument provided for named parameter '" +
                  std::string(ctor->params[posArgCount]->name) + "'.");
              break;
            }
            resolvedArgs[posArgCount] = node->args[i];
            resolvedTypes[posArgCount] = argTypes[i];
            explicitlyProvided[posArgCount] = true;
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
              if (ctor->params[p]->name == name) {
                if (!ctor->params[p]->isNamed) {
                  errors.push_back("Parameter '" + std::string(name) +
                                   "' cannot be passed as a named argument.");
                }
                resolvedArgs[p] = node->args[i];
                resolvedTypes[p] = argTypes[i];
                explicitlyProvided[p] = true;
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
            if (ctor->params[p]->defaultValue) {
              auto defNode = ctor->params[p]->defaultValue;
              if (!defNode->exprType) {
                dispatch(defNode);
              }
              resolvedArgs[p] = defNode;
              resolvedTypes[p] = defNode->exprType;
            } else {
              auto pName = std::string(ctor->params[p]->name);
              if (ctor->params[p]->isRequired) {
                errors.push_back("Missing required named parameter '" + pName +
                                 "'.");
              } else if (!ctor->params[p]->isNamed) {
                errors.push_back("Missing mandatory positional parameter '" +
                                 pName + "'.");
              } else {
                const Type *pType = ctor->params[p]->type;
                const Type *unqual = pType->getUnqualifiedType();
                if (unqual->isNumeric()) {
                  auto num = ctx->astCtx.create<NumberNode>(
                      "0", unqual->isFloat(), 0, 0, 0);
                  num->exprType = pType;
                  resolvedArgs[p] = num;
                  resolvedTypes[p] = pType;
                } else if (unqual->isBuiltinType() &&
                           static_cast<const BuiltinType *>(unqual)
                                   ->getBuiltinKind() == BuiltinKind::Bool) {
                  auto bNode = ctx->astCtx.create<BoolNode>(false, 0, 0, 0);
                  bNode->exprType = pType;
                  resolvedArgs[p] = bNode;
                  resolvedTypes[p] = pType;
                } else {
                  auto nNode = ctx->astCtx.create<NullNode>(0, 0, 0);
                  nNode->exprType = pType;
                  resolvedArgs[p] = nNode;
                  resolvedTypes[p] = pType;
                }
              }
            }
          }
        }

        if (!errors.empty()) {
          overloadErrors.push_back(errors);
          continue;
        }

        int currentScore = 0;
        bool match = true;

        for (size_t p = 0; p < expectedParams; ++p) {
          const Type *paramType = ctor->params[p]->type;

          if (paramType->getKind() == TypeKind::Array) {
            paramType = ctx->astCtx.getPointerType(
                static_cast<const ArrayType *>(paramType)->getElementType());
          }

          if (!canImplicitlyCast(resolvedTypes[p], paramType)) {
            match = false;
            break;
          }

          if (explicitlyProvided[p] &&
              canImplicitlyCast(resolvedTypes[p], paramType, false)) {
            currentScore += 10;
          }

          bool isLValue = resolvedArgs[p]->isLValue;
          if (paramType->getKind() == TypeKind::RValueReference) {
            if (isLValue) {
              match = false;
              break;
            } else if (explicitlyProvided[p]) {
              currentScore += 3;
            }
          } else if (paramType->isReferenceType()) {
            const Type *pointee =
                static_cast<const ReferenceType *>(paramType)->getPointeeType();
            if (!pointee->isConstQualified()) {
              if (!isLValue) {
                match = false;
                break;
              } else if (explicitlyProvided[p]) {
                currentScore += 3;
              }
            } else if (explicitlyProvided[p]) {
              currentScore += 2;
            }
          } else if (explicitlyProvided[p]) {
            currentScore += 1;
          }

          if (!explicitlyProvided[p]) {
            currentScore -= 1;
          }
        }

        if (match && currentScore > bestScore) {
          bestScore = currentScore;
          bestMatch = ctor;
          bestResolvedArgs = resolvedArgs;
        }
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

      if (bestMatch) {
        const_cast<NewExprNode *>(node)->resolvedConstructor = bestMatch;

        for (size_t p = 0; p < bestResolvedArgs.size(); ++p) {
          const Type *paramType = bestMatch->params[p]->type;
          if (paramType->getKind() == TypeKind::Array) {
            paramType = ctx->astCtx.getPointerType(
                static_cast<const ArrayType *>(paramType)->getElementType());
          }
          bestResolvedArgs[p] =
              performImplicitConversion(bestResolvedArgs[p], paramType);
        }

        const_cast<NewExprNode *>(node)->args =
            ctx->astCtx.copyArray<ExprNode *>(bestResolvedArgs);
        const_cast<NewExprNode *>(node)->argNames = {};
      }
    }
  }

  node->exprType = ctx->astCtx.getPointerType(node->allocatedType);
  return node->exprType;
}

SemaResult TypeCheckPass::visit(const DeleteExprNode *node) {
  auto ptrTy = dispatch(node->ptr);
  if (!ptrTy || !(*ptrTy)->getUnqualifiedType()->isPointerType()) {
    return ctx->reportError(node->line, node->column, node->length,
                            "Cannot delete non-pointer type");
  }

  const Type *pointeeTy =
      static_cast<const PointerType *>(node->ptr->exprType)->getPointeeType();
  const Type *unqual = pointeeTy->getUnqualifiedType();

  const FunctionDeclNode *dtor = nullptr;
  if (unqual->getKind() == TypeKind::Class ||
      unqual->getKind() == TypeKind::Struct ||
      unqual->getKind() == TypeKind::Union) {
    auto *recTy = static_cast<const RecordType *>(unqual);

    if (auto *decl = recTy->getDeclaration()) {
      if (decl->kind == NodeKind::ClassDecl) {
        dtor = static_cast<const ClassDeclNode *>(decl)->destructor;
      } else if (decl->kind == NodeKind::StructDecl) {
        dtor = static_cast<const StructDeclNode *>(decl)->destructor;
      } else if (decl->kind == NodeKind::UnionDecl) {
        dtor = static_cast<const UnionDeclNode *>(decl)->destructor;
      }
    }
  }

  node->exprType = ctx->astCtx.VoidTy;
  return ctx->astCtx.VoidTy;
}

SemaResult TypeCheckPass::visit(const NullNode *node) {
  const Type *ty = ctx->astCtx.getPointerType(ctx->astCtx.VoidTy);
  node->exprType = ty;
  return ty;
}

SemaResult TypeCheckPass::visit(const ImplicitCastNode *node) {
  return node->exprType;
}

} // namespace utopia