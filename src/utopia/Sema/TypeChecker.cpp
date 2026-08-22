#include "utopia/AST/ASTCloner.hpp"
#include "utopia/CodeGen/Mangler.hpp"
#include "utopia/Common/Logger.hpp"
#include "utopia/Sema/EffectAnalyzer.hpp"
#include "utopia/Sema/Sema.hpp"
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>

namespace {
/* Counter for the hidden temporaries of desugared for-in loops
 * ('__forin_range_N$'), keeping nested loops' names unique across every
 * pipeline run (the driver may re-run Sema). */
int forInTempCounter = 0;
}

namespace utopia {

namespace {
/* True when 't' is (a pointer to) a function type. */
static bool isFunctionPointerType(const Type *t) {
  if (!t)
    return false;
  const Type *u = t->getUnqualifiedType();
  if (u->isPointerType()) {
    const Type *p = static_cast<const PointerType *>(u)
                        ->getPointeeType()
                        ->getUnqualifiedType();
    return p->getKind() == TypeKind::Function;
  }
  return u->getKind() == TypeKind::Function;
}

/* Returns the function pointer type that should drive lambda inference for a
 * declaration of type 't': the type itself when it is a function pointer, or
 * the element type when 't' is an array of function pointers. */
static const Type *getExpectedFunctionPtrType(const Type *t) {
  if (!t)
    return nullptr;
  const Type *u = t->getUnqualifiedType();
  if (auto *arr = llvm::dyn_cast<ArrayType>(u)) {
    const Type *e = arr->getElementType()->getUnqualifiedType();
    return isFunctionPointerType(e) ? e : nullptr;
  }
  return isFunctionPointerType(u) ? t : nullptr;
}

enum class OpCategory {
  Logical,
  Relational,
  BitwiseOrModulo,
  Arithmetic,
  Unknown
};

/* Reconstructs the namespace a declaration lives in from its fully-qualified
 * name. Template instantiation type-checks the cloned body from the call site,
 * where the namespace stack is empty, so lookups inside generic bodies fail to
 * resolve namespace-scoped symbols. Pushing the original namespace during
 * instantiation restores the intended lookup context.
 *
 * fqName forms handled:
 *   "fn"                    -> ""                 (file scope)
 *   "NS.fn"                 -> "NS"
 *   "NS.Rec.fn" (method)    -> "NS"               (record name is stripped)
 */
static std::string getDeclNamespace(const DeclNode *decl) {
  std::string fq(decl->fqName);

  /* Methods and constructors do not carry a parser-set fqName; their
   * namespace is derived from the enclosing record's qualified name
   * ("A.B.Rec" -> "A.B"). */
  if (auto *fn = llvm::dyn_cast<FunctionDeclNode>(decl)) {
    if (fn->parentRecord) {
      std::string recName(fn->parentRecord->getName());
      size_t recDot = recName.find_last_of('.');
      if (fq.empty()) {
        return (recDot == std::string::npos) ? "" : recName.substr(0, recDot);
      }
      if (fq.substr(0, fq.find_last_of('.')) == recName) {
        return (recDot == std::string::npos) ? "" : recName.substr(0, recDot);
      }
    }
  }

  size_t lastDot = fq.find_last_of('.');
  if (lastDot == std::string::npos)
    return "";
  return fq.substr(0, lastDot);
}

/* Pushes every namespace segment of 'ns' (e.g. "A.B" -> push "A", push "B")
 * and returns the number of segments pushed, so callers can pop them back. */
static size_t pushDeclNamespace(SemaContext *ctx, const std::string &ns) {
  size_t count = 0;
  size_t start = 0;
  while (start < ns.size()) {
    size_t dot = ns.find('.', start);
    std::string part = (dot == std::string::npos) ? ns.substr(start)
                                                  : ns.substr(start, dot - start);
    ctx->pushNamespace(ctx->astCtx.copyString(part));
    count++;
    if (dot == std::string::npos)
      break;
    start = dot + 1;
  }
  return count;
}

/* Parses the integer argument of an '@align' annotation. Out-of-range
 * literals make std::stoull throw, which would otherwise escape as an
 * internal exception with no source location: report the error and signal
 * failure instead. */
static bool parseAlignValue(SemaContext *ctx, const NumberNode *num,
                            const AnnotationNode *ann, uint64_t &outValue) {
  try {
    outValue = std::stoull(std::string(num->raw), nullptr, 0);
    return true;
  } catch (const std::exception &) {
    ctx->reportError(ann->line, ann->column, ann->length,
                     "Alignment value is out of range.");
    return false;
  }
}

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

/* True when 'sub' is 'super' or a subclass of it (including interfaces). */
static bool isSubtypeClass(const ClassType *sub, const ClassType *super) {
  if (!sub || !super)
    return false;
  if (sub == super)
    return true;
  if (sub->getBaseClass()) {
    if (auto *pBase = llvm::dyn_cast<ClassType>(
            sub->getBaseClass()->getUnqualifiedType())) {
      if (isSubtypeClass(pBase, super))
        return true;
    }
  }
  for (const Type *iface : sub->getInterfaces()) {
    if (auto *pIface =
            llvm::dyn_cast<ClassType>(iface->getUnqualifiedType())) {
      if (isSubtypeClass(pIface, super))
        return true;
    }
  }
  return false;
}

/* The declaration's simple name, for diagnostics. */
static std::string_view templateDeclName(const DeclNode *d) {
  if (auto *c = llvm::dyn_cast<ClassDeclNode>(d))
    return c->name;
  if (auto *s = llvm::dyn_cast<StructDeclNode>(d))
    return s->name;
  if (auto *u = llvm::dyn_cast<UnionDeclNode>(d))
    return u->name;
  if (auto *f = llvm::dyn_cast<FunctionDeclNode>(d))
    return f->name;
  return d->fqName;
}

/* The constraint as source text, for diagnostics. */
static std::string templateConstraintToString(const TemplateConstraint &tc) {
  switch (tc.kind) {
  case TemplateConstraintKind::None:
    return "Object";
  case TemplateConstraintKind::Object:
    return "Object";
  case TemplateConstraintKind::Record:
    return "Record";
  case TemplateConstraintKind::Number:
    return "Number";
  case TemplateConstraintKind::Integer:
    return "Integer";
  case TemplateConstraintKind::FloatingPoint:
    return "FloatingPoint";
  case TemplateConstraintKind::Class:
    return tc.classType ? tc.classType->toString() : "<class>";
  }
  return "Object";
}

/* Deduces the parameters of a partial specialization from its pattern
 * against the requested arguments: a parameter position binds the argument
 * (consistently across repeats), a concrete pattern type must equal the
 * argument. Returns true and fills 'outMap' when the pattern matches. */
static bool matchSpecializationPattern(
    llvm::ArrayRef<const Type *> pattern, llvm::ArrayRef<const Type *> args,
    std::unordered_map<std::string_view, const Type *> &outMap) {
  if (pattern.size() != args.size())
    return false;
  for (size_t i = 0; i < pattern.size(); ++i) {
    const Type *p = pattern[i]->getUnqualifiedType();
    if (p->getKind() == TypeKind::TemplateParam) {
      auto *pt = static_cast<const TemplateParamType *>(p);
      auto it = outMap.find(pt->getName());
      if (it != outMap.end()) {
        if (it->second->toString() != args[i]->toString())
          return false;
      } else {
        outMap[pt->getName()] = args[i];
      }
    } else {
      if (p->toString() != args[i]->getUnqualifiedType()->toString())
        return false;
    }
  }
  return true;
}

static std::string templateArgsToString(llvm::ArrayRef<const Type *> args) {
  std::string s;
  for (size_t i = 0; i < args.size(); ++i) {
    if (i)
      s += ", ";
    s += args[i]->toString();
  }
  return s;
}
} // namespace

bool TypeCheckPass::run(const ModuleNode *module, SemaContext &context) {
  ctx = &context;
  auto result = dispatch(module);

  if (!result && !ctx->hasErrors()) {
    SemaResult err =
        ctx->reportError(result.error().line, result.error().column,
                         result.error().length, result.error().message);
  }

  /* Fixed-point interprocedural analysis: a function may unwind when its
   * body throws directly, or when it calls (directly, virtually or through
   * a function pointer) a function that may unwind. Extern/intrinsic
   * functions cannot throw; lambdas are analyzed as their own synthesized
   * functions, so their bodies do not affect the enclosing function. */
  bool changed = true;
  while (changed) {
    changed = false;
    for (const auto *fn : allFunctions) {
      if (fn->mayUnwind)
        continue;
      if (computeFunctionMayUnwind(fn)) {
        const_cast<FunctionDeclNode *>(fn)->mayUnwind = true;
        changed = true;
      }
    }
  }

  return !ctx->hasErrors();
}

/* Recursively walks a statement tree looking for anything that could make
 * the containing function unwind. Lambda bodies are excluded: they compile
 * to their own synthesized functions. */
static bool statementMayUnwind(const ASTNode *node) {
  if (!node)
    return false;

  switch (node->kind) {
  case NodeKind::Throw:
    return true;
  case NodeKind::Lambda:
    return false;
  case NodeKind::FunctionCall: {
    const auto *call = static_cast<const FunctionCallNode *>(node);
    if (!call->resolvedFunc) {
      /* Indirect (function-pointer) calls may unwind. */
      return true;
    }
    const auto *callee = call->resolvedFunc;
    if (callee->isExtern || callee->isIntrinsic || callee->isAsync)
      return false;
    if (callee->isVirtual || callee->isOverride)
      return true; /* an override may throw */
    return callee->mayUnwind;
  }
  case NodeKind::Block: {
    const auto *block = static_cast<const BlockNode *>(node);
    for (const auto *stmt : block->statements) {
      if (statementMayUnwind(stmt))
        return true;
    }
    return false;
  }
  case NodeKind::If: {
    const auto *ifStmt = static_cast<const IfNode *>(node);
    return statementMayUnwind(ifStmt->thenBlock) ||
           statementMayUnwind(ifStmt->elseBlock);
  }
  case NodeKind::For: {
    const auto *forStmt = static_cast<const ForNode *>(node);
    return statementMayUnwind(forStmt->body);
  }
  case NodeKind::ForIn: {
    const auto *forInStmt = static_cast<const ForInNode *>(node);
    return statementMayUnwind(forInStmt->body);
  }
  case NodeKind::While: {
    const auto *whileStmt = static_cast<const WhileNode *>(node);
    return statementMayUnwind(whileStmt->body);
  }
  case NodeKind::Switch: {
    const auto *switchStmt = static_cast<const SwitchNode *>(node);
    for (const auto *c : switchStmt->cases) {
      for (const auto *stmt : c->statements) {
        if (statementMayUnwind(stmt))
          return true;
      }
    }
    return false;
  }
  case NodeKind::Try: {
    const auto *tryStmt = static_cast<const TryStmtNode *>(node);
    return statementMayUnwind(tryStmt->body);
  }
  case NodeKind::Assign: {
    const auto *assign = static_cast<const AssignNode *>(node);
    return statementMayUnwind(assign->value);
  }
  default:
    return false;
  }
}

bool TypeCheckPass::computeFunctionMayUnwind(const FunctionDeclNode *fn) {
  if (fn->hasEH)
    return true;
  if (fn->superCall && statementMayUnwind(fn->superCall))
    return true;
  for (const auto *init : fn->fieldInitializers) {
    if (statementMayUnwind(init))
      return true;
  }
  if (statementMayUnwind(fn->body))
    return true;
  return false;
}

const Type *TypeCheckPass::resolveIfTemplate(const Type *t) {
  if (!t)
    return nullptr;

  /* Namespace-qualified lookup without the module-visibility filter, mirroring
   * the parser's global resolution. See the placeholder recovery below. */
  auto lookupUnfiltered = [](SemaContext *semaCtx,
                             std::string_view name) {
    auto res = semaCtx->symTable.lookupExact(name);
    if (!res.empty())
      return res;

    std::string ns = semaCtx->getCurrentNamespace();
    while (!ns.empty()) {
      res = semaCtx->symTable.lookupExact(ns + "." + std::string(name));
      if (!res.empty())
        return res;
      size_t pos = ns.find_last_of('.');
      if (pos != std::string_view::npos) {
        ns = ns.substr(0, pos);
      } else {
        break;
      }
    }

    for (auto it = semaCtx->symTable.getScopes().rbegin();
         it != semaCtx->symTable.getScopes().rend(); ++it) {
      for (const auto &u : it->usings) {
        res = semaCtx->symTable.lookupExact(u + "." + std::string(name));
        if (!res.empty())
          return res;
      }
    }
    return res;
  };

  /* Resolve template instantiations hidden behind reference, const and
   * pointer wrappers ('const Map<K, V>&'): the instantiated parameter types
   * of a template class method must compare equal to the record type the
   * caller passes, otherwise calls between the class's own methods fail
   * with an unresolved 'Map<String, int32>' vs 'Map_String_int32' mismatch. */
  if (auto *refTy = llvm::dyn_cast<ReferenceType>(t)) {
    const Type *pointee = resolveIfTemplate(refTy->getPointeeType());
    return pointee == refTy->getPointeeType()
               ? t
               : ctx->astCtx.getReferenceType(pointee);
  }
  if (auto *rvRefTy = llvm::dyn_cast<RValueReferenceType>(t)) {
    const Type *pointee = resolveIfTemplate(rvRefTy->getPointeeType());
    return pointee == rvRefTy->getPointeeType()
               ? t
               : ctx->astCtx.getRValueReferenceType(pointee);
  }
  if (auto *ptrTy = llvm::dyn_cast<PointerType>(t)) {
    const Type *pointee = resolveIfTemplate(ptrTy->getPointeeType());
    return pointee == ptrTy->getPointeeType()
               ? t
               : ctx->astCtx.getPointerType(pointee);
  }
  if (auto *cTy = llvm::dyn_cast<ConstType>(t)) {
    const Type *base = resolveIfTemplate(cTy->getBaseType());
    return base == cTy->getBaseType()
               ? t
               : ctx->astCtx.getConstType(base);
  }
  if (auto *arrTy = llvm::dyn_cast<ArrayType>(t)) {
    const Type *elem = resolveIfTemplate(arrTy->getElementType());
    return elem == arrTy->getElementType()
               ? t
               : ctx->astCtx.getArrayType(elem, arrTy->getSize());
  }

  /* The parser falls back to a template parameter placeholder when a type
   * name cannot be resolved at parse time (e.g. a cross-module reference
   * whose module is parsed later). At semantic time such placeholders can
   * be recovered through the symbol table: resolve them to the declared
   * record/alias/enum when one is visible. Genuine template parameters
   * (T, R, ...) have no declaration and stay untouched. */
  if (auto *tp = llvm::dyn_cast<TemplateParamType>(t)) {
    auto decls = ctx->lookup(tp->getName());
    if (decls.empty()) {
      /* The parser resolves type names globally, so the placeholder must
       * recover the same way: the visibility-filtered lookup can miss a
       * type whose declaring module was parsed later (or is only reachable
       * through a sibling import). Without this fallback the resolution
       * would depend on parse order and produce false mismatches (e.g. an
       * '@override' check comparing a placeholder name against the resolved
       * type name). */
      decls = lookupUnfiltered(ctx, tp->getName());
    }
    for (const auto *d : decls) {
      if (d->kind == NodeKind::ClassDecl ||
          d->kind == NodeKind::StructDecl ||
          d->kind == NodeKind::UnionDecl) {
        if (const Type *rt = ctx->astCtx.getRecordType(d->fqName))
          return rt;
        continue;
      }
      if (d->kind == NodeKind::TypedefDecl) {
        if (const Type *at = ctx->astCtx.getTypeAlias(d->fqName))
          return at;
        continue;
      }
      if (d->kind == NodeKind::EnumDecl) {
        if (const Type *et = ctx->astCtx.getEnumTypeByName(d->fqName))
          return et;
      }
    }
    return t;
  }

  if (auto *fnTy = llvm::dyn_cast<FunctionType>(t)) {
    std::vector<const Type *> resolvedParams;
    for (const auto *p : fnTy->getParamTypes()) {
      resolvedParams.push_back(resolveIfTemplate(p));
    }
    return ctx->astCtx.getFunctionType(
        resolveIfTemplate(fnTy->getReturnType()),
        ctx->astCtx.copyArray<const Type *>(resolvedParams));
  }

  if (auto *instTy = llvm::dyn_cast<TemplateInstType>(t)) {
    if (instTy->getResolvedType())
      return instTy->getResolvedType();

    auto declIt = ctx->templateRegistry.find(instTy->getBaseName());
    if (declIt == ctx->templateRegistry.end()) {
      /* Templates are registered under both their unqualified name and their
       * fully-qualified name; fall back to the unqualified suffix when the
       * type was resolved through a 'using' directive or namespace-qualified
       * syntax. */
      std::string_view bn = instTy->getBaseName();
      size_t dotPos = bn.find_last_of('.');
      if (dotPos != std::string_view::npos) {
        declIt = ctx->templateRegistry.find(bn.substr(dotPos + 1));
      }
    }
    if (declIt == ctx->templateRegistry.end()) {
      (void)ctx->reportError(0, 0, 0,
                             "Template declaration not found: " +
                                 std::string(instTy->getBaseName()));
      return ctx->astCtx.VoidTy;
    }

    const DeclNode *tmplDecl = declIt->second;

    if (ctx->currentModule &&
        !ctx->currentModule->canSee(tmplDecl->declFilePath)) {
      (void)ctx->reportError(0, 0, 0,
                             "Template '" + std::string(instTy->getBaseName()) +
                                 "' is not visible in this module.");
      return ctx->astCtx.VoidTy;
    }

    /* Mangle from the fully-qualified template name: type names parsed before
     * imported modules are loaded may be unqualified ('unique_ptr'), so using
     * the registry's fqName keeps the instantiation name stable and
     * namespace-aware across the type, call, and reference paths. Template
     * arguments are resolved first so nested instantiations mangle
     * identically from every resolution site. */
    std::vector<const Type *> resolvedArgs;
    for (const auto *arg : instTy->getTemplateArgs())
      resolvedArgs.push_back(resolveIfTemplate(arg));

    /* The primary template's constraints ('T extends X') must hold for every
     * instantiation, including those served by a specialization. */
    if (!checkTemplateConstraints(tmplDecl, resolvedArgs, nullptr))
      return ctx->astCtx.VoidTy;

    std::string mangledName = std::string(declIt->second->fqName);
    for (const auto *resArg : resolvedArgs) {
      std::string argStr = resArg->toString();
      for (char &c : argStr) {
        if (!isalnum(c))
          c = '_';
      }
      mangledName += "_" + argStr;
    }
    std::string_view mangledView = ctx->astCtx.copyString(mangledName);

    /* Pick a specialization when one matches the arguments: complete
     * specializations win over partial ones, and the primary template is the
     * fallback. */
    const DeclNode *effectiveDecl = tmplDecl;
    std::unordered_map<std::string_view, const Type *> templateArgMap;
    {
      std::string specKey = std::string(instTy->getBaseName());
      auto specIt = ctx->templateSpecializations.find(specKey);
      if (specIt == ctx->templateSpecializations.end()) {
        size_t dotPos = specKey.find_last_of('.');
        if (dotPos != std::string::npos)
          specIt =
              ctx->templateSpecializations.find(specKey.substr(dotPos + 1));
      }
      if (specIt != ctx->templateSpecializations.end()) {
        const DeclNode *bestSpec = nullptr;
        std::unordered_map<std::string_view, const Type *> bestMap;
        for (const auto *spec : specIt->second) {
          std::unordered_map<std::string_view, const Type *> candMap;
          if (!matchSpecializationPattern(spec->specializationArgs,
                                          resolvedArgs, candMap))
            continue;
          if (bestSpec) {
            (void)ctx->reportError(
                0, 0, 0,
                "Ambiguous template specialization of '" +
                    std::string(specKey) + "' for arguments <" +
                    templateArgsToString(resolvedArgs) +
                    ">: multiple specializations match.");
            return ctx->astCtx.VoidTy;
          }
          bestSpec = spec;
          bestMap = std::move(candMap);
        }
        if (bestSpec) {
          if (!checkTemplateConstraints(bestSpec, resolvedArgs, nullptr))
            return ctx->astCtx.VoidTy;
          if (bestSpec->isTemplate) {
            /* Partial specialization: instantiate its declaration with the
             * deduced parameters under the canonical mangled name. */
            effectiveDecl = bestSpec;
            templateArgMap = std::move(bestMap);
          } else {
            /* Complete specialization: its record already exists under the
             * canonical instantiation name (created at parse time). */
            if (auto *existing = ctx->astCtx.getRecordType(mangledView)) {
              instTy->setResolvedType(existing);
              return existing;
            }
            /* Declared after a matching primary instantiation: fall through
             * and instantiate the specialization's declaration. */
            effectiveDecl = bestSpec;
            templateArgMap.clear();
          }
        }
      }
    }
    if (templateArgMap.empty() && effectiveDecl == tmplDecl) {
      for (size_t i = 0; i < tmplDecl->templateParams.size(); ++i) {
        templateArgMap[tmplDecl->templateParams[i]] = resolvedArgs[i];
      }
    }

    if (llvm::isa<ClassDeclNode>(effectiveDecl) ||
        llvm::isa<StructDeclNode>(effectiveDecl) ||
        llvm::isa<UnionDeclNode>(effectiveDecl)) {
      if (auto *existing = ctx->astCtx.getRecordType(mangledView)) {
        instTy->setResolvedType(existing);
        return existing;
      }

      TypeKind kind = TypeKind::Struct;
      if (llvm::isa<ClassDeclNode>(effectiveDecl))
        kind = TypeKind::Class;
      else if (llvm::isa<UnionDeclNode>(effectiveDecl))
        kind = TypeKind::Union;

      ctx->astCtx.createRecordType(kind, mangledView);
    }

    /* Record the resolved template arguments on the instantiated record so
     * the value type can be recovered from e.g. Future<T> instances. */
    if (auto *recTy = ctx->astCtx.getRecordType(mangledView)) {
      std::string_view baseName = declIt->second->fqName;
      if (baseName.empty()) {
        if (auto *cls = llvm::dyn_cast<ClassDeclNode>(declIt->second))
          baseName = cls->name;
        else if (auto *str = llvm::dyn_cast<StructDeclNode>(declIt->second))
          baseName = str->name;
        else if (auto *uni = llvm::dyn_cast<UnionDeclNode>(declIt->second))
          baseName = uni->name;
      }
      const_cast<RecordType *>(recTy)->setTemplateBaseName(baseName);
      const_cast<RecordType *>(recTy)->setTemplateArgs(
          ctx->astCtx.copyArray<const Type *>(resolvedArgs));
    }

    ASTCloner cloner(ctx->astCtx, templateArgMap);
    DeclNode *instDecl = llvm::cast<DeclNode>(cloner.dispatch(effectiveDecl));

    if (instDecl) {
      instDecl->hasPublicMod = effectiveDecl->hasPublicMod;
      instDecl->hasPrivateMod = effectiveDecl->hasPrivateMod;
      instDecl->hasProtectedMod = effectiveDecl->hasProtectedMod;
      instDecl->annotations = effectiveDecl->annotations;
      instDecl->declFilePath = effectiveDecl->declFilePath;
      instDecl->alignment = effectiveDecl->alignment;
      instDecl->isPacked = effectiveDecl->isPacked;

      /* The mangled name is built from the fully-qualified template name, so
       * it already carries any namespace qualification. */
      instDecl->fqName = ctx->astCtx.copyString(mangledView);

      if (auto *clsDecl = llvm::dyn_cast<ClassDeclNode>(instDecl)) {
        clsDecl->name = mangledView;
      } else if (auto *structDecl = llvm::dyn_cast<StructDeclNode>(instDecl)) {
        structDecl->name = mangledView;
      } else if (auto *unionDecl = llvm::dyn_cast<UnionDeclNode>(instDecl)) {
        unionDecl->name = mangledView;
      } else if (auto *funcDecl = llvm::dyn_cast<FunctionDeclNode>(instDecl)) {
        funcDecl->name = mangledView;
      }

      /* Dynamically repopulate FieldInfo array for RecordType bindings
       * post-clone */
      if (llvm::isa<ClassDeclNode>(instDecl) ||
          llvm::isa<StructDeclNode>(instDecl) ||
          llvm::isa<UnionDeclNode>(instDecl)) {
        RecordType *recTy = ctx->astCtx.getRecordType(mangledView);
        std::vector<FieldInfo> fInfos;
        uint32_t instanceFieldIndex = 0;

        llvm::ArrayRef<VarDeclNode *> fields;
        if (auto *clsDecl = llvm::dyn_cast<ClassDeclNode>(instDecl))
          fields = clsDecl->fields;
        else if (auto *structDecl = llvm::dyn_cast<StructDeclNode>(instDecl))
          fields = structDecl->fields;
        else if (auto *unionDecl = llvm::dyn_cast<UnionDeclNode>(instDecl))
          fields = unionDecl->fields;

        for (size_t i = 0; i < fields.size(); ++i) {
          if (fields[i]->isStatic)
            continue;
          fInfos.push_back({fields[i]->varName, fields[i]->type,
                            instanceFieldIndex++,
                            fields[i]->isPublic(fields[i]->varName),
                            fields[i]->isProtected(fields[i]->varName)});
        }
        recTy->setFields(ctx->astCtx.copyArray<FieldInfo>(fInfos));

        /* Ensure method ownership is retained post-clone */
        auto updateParentRec = [&](llvm::ArrayRef<FunctionDeclNode *> funcs) {
          for (auto *f : funcs) {
            const_cast<FunctionDeclNode *>(f)->parentRecord = recTy;
          }
        };

        if (auto *cls = llvm::dyn_cast<ClassDeclNode>(instDecl)) {
          cls->recordType = recTy;
          recTy->setOpaque(cls->isOpaque);
          updateParentRec(cls->methods);
          updateParentRec(cls->constructors);
          if (cls->destructor) {
            cls->destructor->parentRecord = recTy;
          }
        } else if (auto *str = llvm::dyn_cast<StructDeclNode>(instDecl)) {
          str->recordType = recTy;
          recTy->setOpaque(str->isOpaque);
          updateParentRec(str->methods);
          updateParentRec(str->constructors);
          if (str->destructor) {
            str->destructor->parentRecord = recTy;
          }
        } else if (auto *uni = llvm::dyn_cast<UnionDeclNode>(instDecl)) {
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

      /* Instantiate inside a dedicated scope so that the cloned record's
       * fields and methods (registered during typechecking) do not leak into
       * the enclosing lexical scope, which would collide with user variables
       * named like a field (e.g. a local 'ptr' vs. unique_ptr's 'ptr'). */
      ctx->pushScope();

      /* Type-check the clone from the namespace where the template was
       * declared, otherwise generic method bodies cannot resolve
       * namespace-scoped symbols. */
      std::string instNs = getDeclNamespace(effectiveDecl);
      size_t nsDepth = pushDeclNamespace(ctx, instNs);

      dcp.dispatch(instDecl);
      dispatch(instDecl);

      for (size_t i = 0; i < nsDepth; ++i)
        ctx->popNamespace();
      ctx->popScope();

      /* Re-register the instantiated declaration at module scope so member
       * resolution (ctx->lookup of the mangled record name) and later calls
       * can find it from any function. */
      ctx->symTable.addGlobalSymbol(instDecl->fqName, instDecl);

      ctx->setCurrentFile(prevFile);
    }

    const Type *res = ctx->astCtx.VoidTy;
    if (llvm::isa<ClassDeclNode>(effectiveDecl) ||
        llvm::isa<StructDeclNode>(effectiveDecl) ||
        llvm::isa<UnionDeclNode>(effectiveDecl)) {
      res = ctx->astCtx.getRecordType(mangledView);
    }
    instTy->setResolvedType(res);
    return res;
  }

  if (auto *p = llvm::dyn_cast<PointerType>(t)) {
    return ctx->astCtx.getPointerType(resolveIfTemplate(p->getPointeeType()));
  }
  if (auto *r = llvm::dyn_cast<ReferenceType>(t)) {
    return ctx->astCtx.getReferenceType(resolveIfTemplate(r->getPointeeType()));
  }
  if (auto *rv = llvm::dyn_cast<RValueReferenceType>(t)) {
    return ctx->astCtx.getRValueReferenceType(
        resolveIfTemplate(rv->getPointeeType()));
  }
  if (auto *c = llvm::dyn_cast<ConstType>(t)) {
    return ctx->astCtx.getConstType(resolveIfTemplate(c->getBaseType()));
  }
  if (auto *a = llvm::dyn_cast<ArrayType>(t)) {
    return ctx->astCtx.getArrayType(resolveIfTemplate(a->getElementType()),
                                    a->getSize());
  }
  return t;
}

bool TypeCheckPass::templateConstraintSatisfied(const TemplateConstraint &tc,
                                                const Type *arg) const {
  const Type *unqual = arg->getUnqualifiedType();
  switch (tc.kind) {
  case TemplateConstraintKind::None:
  case TemplateConstraintKind::Object:
    return true;
  case TemplateConstraintKind::Record:
    return unqual->getKind() == TypeKind::Struct ||
           unqual->getKind() == TypeKind::Class ||
           unqual->getKind() == TypeKind::Union;
  case TemplateConstraintKind::Number:
    return unqual->isNumeric();
  case TemplateConstraintKind::Integer:
    return unqual->isInteger();
  case TemplateConstraintKind::FloatingPoint:
    return unqual->isFloat();
  case TemplateConstraintKind::Class: {
    if (unqual->getKind() != TypeKind::Class || !tc.classType)
      return false;
    const Type *cUnqual = tc.classType->getUnqualifiedType();
    if (cUnqual->getKind() != TypeKind::Class)
      return false;
    return isSubtypeClass(static_cast<const ClassType *>(unqual),
                          static_cast<const ClassType *>(cUnqual));
  }
  }
  return false;
}

bool TypeCheckPass::checkTemplateConstraints(
    const DeclNode *tmpl, llvm::ArrayRef<const Type *> args,
    const ASTNode *at) {
  if (tmpl->templateConstraints.empty() || tmpl->templateParams.empty())
    return true;

  int line = 0, col = 0, len = 0;
  if (at) {
    line = at->line;
    col = at->column;
    len = at->length;
  }

  for (size_t i = 0; i < tmpl->templateConstraints.size() && i < args.size();
       ++i) {
    const TemplateConstraint &tc = tmpl->templateConstraints[i];
    if (tc.kind == TemplateConstraintKind::None)
      continue;
    if (templateConstraintSatisfied(tc, args[i]))
      continue;
    (void)ctx->reportError(
        line, col, len,
        "Template argument '" + args[i]->toString() + "' for '" +
            std::string(tmpl->templateParams[i]) +
            "' does not satisfy the constraint '" +
            std::string(tmpl->templateParams[i]) + " extends " +
            templateConstraintToString(tc) + "' on template '" +
            std::string(templateDeclName(tmpl)) + "'.");
    return false;
  }
  return true;
}

const ClassType *
TypeCheckPass::findClassTemplateConstraint(std::string_view name) const {
  auto findIn = [&](const DeclNode *decl) -> const ClassType * {
    if (!decl)
      return nullptr;
    for (size_t i = 0; i < decl->templateParams.size() &&
                       i < decl->templateConstraints.size();
         ++i) {
      if (decl->templateParams[i] != name)
        continue;
      const TemplateConstraint &tc = decl->templateConstraints[i];
      if (tc.kind != TemplateConstraintKind::Class || !tc.classType)
        return nullptr;
      const Type *c = tc.classType->getUnqualifiedType();
      return llvm::dyn_cast<ClassType>(c);
    }
    return nullptr;
  };

  const RecordType *rec = ctx->getCurrentRecordContext();
  if (rec) {
    if (auto *cls = findIn(rec->getDeclaration()))
      return cls;
  }
  return findIn(ctx->getCurrentFunction());
}

bool TypeCheckPass::checkTypeVisibility(const Type *type, const ASTNode *node) {
  if (!type)
    return true;
  const Type *unqual = type->getUnqualifiedType();
  while (auto *arrTy = llvm::dyn_cast<ArrayType>(unqual)) {
    unqual = arrTy->getElementType()->getUnqualifiedType();
  }

  const DeclNode *decl = nullptr;
  std::string_view typeName;

  if (auto *recTy = llvm::dyn_cast<RecordType>(unqual)) {
    decl = recTy->getDeclaration();
    typeName = recTy->getName();
  } else if (auto *enumTy = llvm::dyn_cast<EnumType>(unqual)) {
    decl = enumTy->getDeclaration();
    typeName = enumTy->getName();
  } else if (auto *aliasTy = llvm::dyn_cast<AliasType>(unqual)) {
    decl = aliasTy->getDeclaration();
    typeName = aliasTy->getName();
  }

  if (decl) {
    if (!decl->isPublic(typeName) && decl->declFilePath != ctx->currentFile) {
      (void)ctx->reportError(node->line, node->column, node->length,
                             "Cannot access private type '" +
                                 std::string(typeName) +
                                 "' from outside its file.");
      return false;
    }

    if (ctx->currentModule && !ctx->currentModule->canSee(decl->declFilePath)) {
      (void)ctx->reportError(node->line, node->column, node->length,
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

  if (auto *castNode = llvm::dyn_cast<CastNode>(node)) {
    if (castNode->targetType && castNode->targetType->isVoid()) {
      /* Explicit cast to void suppresses the nodiscard warning */
      return;
    }
    checkNodiscard(castNode->expr);
    return;
  }
  if (auto *implCastNode = llvm::dyn_cast<ImplicitCastNode>(node)) {
    checkNodiscard(implCastNode->expr);
    return;
  }

  bool isNodiscardFunc = false;
  std::string funcName = "";

  if (auto *call = llvm::dyn_cast<FunctionCallNode>(node)) {
    if (call->resolvedFunc) {
      for (const auto *ann : call->resolvedFunc->annotations) {
        if (ann->name == "nodiscard") {
          isNodiscardFunc = true;
          funcName = std::string(call->resolvedFunc->name);
          break;
        }
      }
    }
  } else if (auto *uop = llvm::dyn_cast<UnaryOpNode>(node)) {
    if (uop->overloadedOperator) {
      for (const auto *ann : uop->overloadedOperator->annotations) {
        if (ann->name == "nodiscard") {
          isNodiscardFunc = true;
          funcName = "operator" + std::string(uop->op);
          break;
        }
      }
    }
  } else if (auto *bop = llvm::dyn_cast<BinaryOpNode>(node)) {
    if (bop->overloadedOperator) {
      for (const auto *ann : bop->overloadedOperator->annotations) {
        if (ann->name == "nodiscard") {
          isNodiscardFunc = true;
          funcName = "operator" + std::string(bop->op);
          break;
        }
      }
    }
  } else if (auto *sub = llvm::dyn_cast<ArraySubscriptNode>(node)) {
    if (sub->overloadedOperator) {
      for (const auto *ann : sub->overloadedOperator->annotations) {
        if (ann->name == "nodiscard") {
          isNodiscardFunc = true;
          funcName = "operator[]";
          break;
        }
      }
    }
  } else if (auto *asn = llvm::dyn_cast<AssignNode>(node)) {
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
    ctx->reportWarning(WarningKind::NodiscardIgnored, node->line,
                       node->column, node->length,
                       "Ignoring return value of function '" + funcName +
                           "', declared with '@nodiscard' annotation.",
                       node->endLine);
    return;
  }

  /* Check if the returned type itself is marked as nodiscard */
  if (auto *expr = llvm::dyn_cast<ExprNode>(node)) {
    if (llvm::isa<FunctionCallNode>(node) || llvm::isa<UnaryOpNode>(node) ||
        llvm::isa<BinaryOpNode>(node) || llvm::isa<ArraySubscriptNode>(node)) {
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
              ctx->reportWarning(
                  WarningKind::NodiscardIgnored, node->line, node->column,
                  node->length,
                  "Ignoring return value of type '" + typeName +
                      "', declared with '@nodiscard' annotation.",
                  node->endLine);
              return;
            }
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

      if (auto *varDecl = llvm::dyn_cast<VarDeclNode>(decl)) {
        declKind = "variable";
        declName = std::string(varDecl->varName);
      } else if (auto *paramDecl = llvm::dyn_cast<ParamDeclNode>(decl)) {
        declKind = "parameter";
        declName = std::string(paramDecl->name);
      } else if (auto *funcDecl = llvm::dyn_cast<FunctionDeclNode>(decl)) {
        declKind = funcDecl->isMethod ? "method" : "function";
        declName = std::string(funcDecl->name);
      } else if (auto *structDecl = llvm::dyn_cast<StructDeclNode>(decl)) {
        declKind = "struct";
        declName = std::string(structDecl->name);
      } else if (auto *classDecl = llvm::dyn_cast<ClassDeclNode>(decl)) {
        declKind = "class";
        declName = std::string(classDecl->name);
      } else if (auto *unionDecl = llvm::dyn_cast<UnionDeclNode>(decl)) {
        declKind = "union";
        declName = std::string(unionDecl->name);
      } else if (auto *enumDecl = llvm::dyn_cast<EnumDeclNode>(decl)) {
        declKind = "enum";
        declName = std::string(enumDecl->name);
      } else if (auto *enumMem = llvm::dyn_cast<EnumMemberNode>(decl)) {
        declKind = "enum member";
        declName = std::string(enumMem->name);
      } else if (auto *typedefDecl = llvm::dyn_cast<TypedefDeclNode>(decl)) {
        declKind = "type alias";
        declName = std::string(typedefDecl->aliasName);
      } else if (auto *annDecl = llvm::dyn_cast<AnnotationDeclNode>(decl)) {
        declKind = "annotation";
        declName = std::string(annDecl->name);
      }

      std::string msg = "use of deprecated " + declKind + " '" + declName + "'";
      if (!ann->args.empty()) {
        if (auto *strArg = llvm::dyn_cast<StringNode>(ann->args[0])) {
          msg += ": " + std::string(strArg->value);
        }
      }

      ctx->reportWarning(WarningKind::Deprecated, node->line, node->column,
                         node->length, msg, node->endLine);
    }
  }
}

/* The numeric type an expression behaves as in arithmetic: references
 * resolve to their pointee and enums act as their underlying integer
 * type. */
static const Type *effectiveNumericType(const Type *t) {
  if (!t)
    return t;
  if (t->isReferenceType()) {
    t = static_cast<const ReferenceType *>(t)->getPointeeType();
  } else if (t->getKind() == TypeKind::RValueReference) {
    t = static_cast<const RValueReferenceType *>(t)->getPointeeType();
  }
  const Type *unqual = t->getUnqualifiedType();
  if (auto *eTy = llvm::dyn_cast<EnumType>(unqual))
    return eTy->getUnderlyingType();
  return t;
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
  } else if (raw.ends_with("uz") || raw.ends_with("UZ")) {
    ty = ctx->astCtx.USizeTy;
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
      if (ann->args.size() != 1 || !llvm::isa<NumberNode>(ann->args[0]) ||
          llvm::cast<NumberNode>(ann->args[0])->isFloat) {
        auto err = ctx->reportError(
            ann->line, ann->column, ann->length,
            "The @align annotation requires a single integer constant.");
        hasErrors = true;
      } else {
        uint64_t alignVal = 0;
        if (!parseAlignValue(ctx, llvm::cast<NumberNode>(ann->args[0]), ann,
                             alignVal)) {
          hasErrors = true;
        } else if (alignVal == 0 || (alignVal & (alignVal - 1)) != 0) {
          auto err = ctx->reportError(ann->line, ann->column, ann->length,
                                      "Alignment must be a power of 2.");
          hasErrors = true;
        } else {
          const_cast<UnionDeclNode *>(node)->alignment = alignVal;
        }
      }
    } else if (ann->name == "alignas") {
      auto err =
          ctx->reportError(ann->line, ann->column, ann->length,
                           "The @alignas annotation has been removed; use "
                           "@align(N) instead.");
      hasErrors = true;
    } else if (ann->name == "packed") {
      if (!ann->args.empty()) {
        auto err =
            ctx->reportError(ann->line, ann->column, ann->length,
                             "The @packed annotation does not take arguments.");
        hasErrors = true;
      } else {
        const_cast<UnionDeclNode *>(node)->isPacked = true;
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

  /* Rebuild the field layout with the now-resolved field types so template
   * instantiations (e.g. smart pointer fields) reach code generation. */
  {
    std::vector<FieldInfo> resolvedInfos;
    uint32_t resolvedIdx = 0;
    for (auto *f : node->fields) {
      if (f->isStatic)
        continue;
      resolvedInfos.push_back({f->varName, f->type, resolvedIdx++,
                               f->isPublic(f->varName),
                               f->isProtected(f->varName)});
    }
    const_cast<RecordType *>(const_cast<UnionDeclNode *>(node)->recordType)->setFields(
        ctx->astCtx.copyArray<FieldInfo>(resolvedInfos));
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

  for (const auto *ann : node->annotations) {
    if (ann->name == "align") {
      if (ann->args.size() != 1 || !llvm::isa<NumberNode>(ann->args[0]) ||
          llvm::cast<NumberNode>(ann->args[0])->isFloat) {
        auto err = ctx->reportError(
            ann->line, ann->column, ann->length,
            "The @align annotation requires a single integer constant.");
        hasErrors = true;
      } else {
        uint64_t alignVal = 0;
        if (!parseAlignValue(ctx, llvm::cast<NumberNode>(ann->args[0]), ann,
                             alignVal)) {
          hasErrors = true;
        } else if (alignVal == 0 || (alignVal & (alignVal - 1)) != 0) {
          auto err = ctx->reportError(ann->line, ann->column, ann->length,
                                      "Alignment must be a power of 2.");
          hasErrors = true;
        } else {
          const_cast<StructDeclNode *>(node)->alignment = alignVal;
        }
      }
    } else if (ann->name == "alignas") {
      auto err =
          ctx->reportError(ann->line, ann->column, ann->length,
                           "The @alignas annotation has been removed; use "
                           "@align(N) instead.");
      hasErrors = true;
    } else if (ann->name == "packed") {
      if (!ann->args.empty()) {
        auto err =
            ctx->reportError(ann->line, ann->column, ann->length,
                             "The @packed annotation does not take arguments.");
        hasErrors = true;
      } else {
        const_cast<StructDeclNode *>(node)->isPacked = true;
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

  /* Rebuild the field layout with the now-resolved field types so template
   * instantiations (e.g. smart pointer fields) reach code generation. */
  {
    std::vector<FieldInfo> resolvedInfos;
    uint32_t resolvedIdx = 0;
    for (auto *f : node->fields) {
      if (f->isStatic)
        continue;
      resolvedInfos.push_back({f->varName, f->type, resolvedIdx++,
                               f->isPublic(f->varName),
                               f->isProtected(f->varName)});
    }
    const_cast<RecordType *>(const_cast<StructDeclNode *>(node)->recordType)->setFields(
        ctx->astCtx.copyArray<FieldInfo>(resolvedInfos));
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

  if (node->baseClass) {
    const_cast<ClassDeclNode *>(node)->baseClass =
        resolveIfTemplate(node->baseClass);
    const Type *bTy = node->baseClass->getUnqualifiedType();

    if (bTy->getKind() != TypeKind::Class) {
      ctx->reportError(node->line, node->column, node->length,
                       "A class can only extend another class.");
      hasErrors = true;
      const_cast<ClassDeclNode *>(node)->baseClass = nullptr;
    } else {
      if (auto *baseDecl = llvm::dyn_cast_or_null<ClassDeclNode>(
              static_cast<const ClassType *>(bTy)->getDeclaration())) {
        if (baseDecl->isFinal) {
          ctx->reportError(node->line, node->column, node->length,
                           "Class '" + std::string(node->name) +
                               "' cannot extend the final class '" +
                               std::string(baseDecl->name) + "'.");
          hasErrors = true;
        }
      }

      // Detection of inheritance cycles
      const ClassType *current = static_cast<const ClassType *>(bTy);
      bool cycle = false;
      while (current) {
        if (current->getName() == node->name) {
          cycle = true;
          break;
        }
        if (current->getBaseClass()) {
          if (auto *nextBase = llvm::dyn_cast<ClassType>(
                  current->getBaseClass()->getUnqualifiedType())) {
            current = nextBase;
          } else {
            break;
          }
        } else {
          break;
        }
      }
      if (cycle) {
        ctx->reportError(node->line, node->column, node->length,
                         "Circular inheritance detected.");
        hasErrors = true;
      }
    }
  }

  for (const auto *ann : node->annotations) {
    if (ann->name == "align") {
      if (ann->args.size() != 1 || !llvm::isa<NumberNode>(ann->args[0]) ||
          llvm::cast<NumberNode>(ann->args[0])->isFloat) {
        auto err = ctx->reportError(
            ann->line, ann->column, ann->length,
            "The @align annotation requires a single integer constant.");
        hasErrors = true;
      } else {
        uint64_t alignVal = 0;
        if (!parseAlignValue(ctx, llvm::cast<NumberNode>(ann->args[0]), ann,
                             alignVal)) {
          hasErrors = true;
        } else if (alignVal == 0 || (alignVal & (alignVal - 1)) != 0) {
          auto err = ctx->reportError(ann->line, ann->column, ann->length,
                                      "Alignment must be a power of 2.");
          hasErrors = true;
        } else {
          const_cast<ClassDeclNode *>(node)->alignment = alignVal;
        }
      }
    } else if (ann->name == "alignas") {
      auto err =
          ctx->reportError(ann->line, ann->column, ann->length,
                           "The @alignas annotation has been removed; use "
                           "@align(N) instead.");
      hasErrors = true;
    } else if (ann->name == "packed") {
      if (!ann->args.empty()) {
        auto err =
            ctx->reportError(ann->line, ann->column, ann->length,
                             "The @packed annotation does not take arguments.");
        hasErrors = true;
      } else {
        const_cast<ClassDeclNode *>(node)->isPacked = true;
      }
    }
  }

  auto prevContext = ctx->getCurrentRecordContext();
  ctx->setCurrentRecordContext(node->recordType);

  auto *classTy =
      static_cast<ClassType *>(const_cast<RecordType *>(node->recordType));

  std::vector<const Type *> resolvedInterfaces;
  for (const auto *iface : node->interfaces) {
    const Type *resolved = resolveIfTemplate(iface);
    if (resolved->getUnqualifiedType()->getKind() != TypeKind::Class) {
      ctx->reportError(node->line, node->column, node->length,
                       "Interfaces must be of class type.");
      hasErrors = true;
    } else if (auto *ifaceDecl = llvm::dyn_cast_or_null<ClassDeclNode>(
                   static_cast<const ClassType *>(
                       resolved->getUnqualifiedType())
                       ->getDeclaration())) {
      if (ifaceDecl->isFinal) {
        ctx->reportError(node->line, node->column, node->length,
                         "Class '" + std::string(node->name) +
                             "' cannot implement the final class '" +
                             std::string(ifaceDecl->name) + "'.");
        hasErrors = true;
      }
    }
    resolvedInterfaces.push_back(resolved);
  }
  classTy->setInterfaces(
      ctx->astCtx.copyArray<const Type *>(resolvedInterfaces));
  const_cast<ClassDeclNode *>(node)->interfaces = classTy->getInterfaces();

  std::vector<FieldInfo> fInfos;
  uint32_t instanceFieldIndex = 0;
  bool isPolymorphic = false;

  if (node->baseClass && !hasErrors) {
    if (auto *pType =
            llvm::dyn_cast<ClassType>(node->baseClass->getUnqualifiedType())) {
      if (pType->getIsPolymorphic())
        isPolymorphic = true;
    }
  }
  if (!resolvedInterfaces.empty()) {
    isPolymorphic = true;
  }
  for (auto *method : node->methods) {
    if (method->isAbstract) {
      isPolymorphic = true;
    }
    for (auto *ann : method->annotations) {
      if (ann->name == "virtual" || ann->name == "override") {
        isPolymorphic = true;
        break;
      }
    }
  }

  if (isPolymorphic) {
    instanceFieldIndex = 1;
  }

  // Collection and flattening of base fields for slicing in LLVM
  auto collectBaseFields = [&](const ClassDeclNode *cDecl, auto &self) -> void {
    if (!cDecl)
      return;
    if (cDecl->baseClass) {
      if (auto *pType = llvm::dyn_cast<ClassType>(
              cDecl->baseClass->getUnqualifiedType())) {
        if (auto *pDecl = llvm::dyn_cast_or_null<ClassDeclNode>(
                pType->getDeclaration())) {
          self(pDecl, self);
        }
      }
    }
    for (auto *f : cDecl->fields) {
      if (f->isStatic)
        continue;
      bool isPub = f->isPublic(f->varName);
      bool isProt = f->isProtected(f->varName);
      std::string_view fname = (!isPub && !isProt) ? "" : f->varName;
      fInfos.push_back({fname, f->type, instanceFieldIndex++, isPub, isProt});
    }
  };

  if (node->baseClass && !hasErrors) {
    if (auto *pType =
            llvm::dyn_cast<ClassType>(node->baseClass->getUnqualifiedType())) {
      if (auto *pDecl =
              llvm::dyn_cast_or_null<ClassDeclNode>(pType->getDeclaration())) {
        collectBaseFields(pDecl, collectBaseFields);
      }
    }
  }

  for (auto *f : node->fields) {
    if (f->isStatic)
      continue;
    fInfos.push_back({f->varName, f->type, instanceFieldIndex++,
                      f->isPublic(f->varName), f->isProtected(f->varName)});
  }

  // VTable Layout & Method Override checking
  auto signaturesMatch = [&](const FunctionDeclNode *a,
                             const FunctionDeclNode *b) {
    if (a->name != b->name)
      return false;
    if (a->isConst != b->isConst)
      return false;
    if (!ctx->isSameType(a->returnType, b->returnType))
      return false;
    if (a->params.size() != b->params.size())
      return false;
    for (size_t i = 0; i < a->params.size(); ++i) {
      if (!ctx->isSameType(a->params[i]->type, b->params[i]->type))
        return false;
    }
    return true;
  };

  for (auto *method : node->methods) {
    if (method->isAbstract && !node->isAbstract) {
      ctx->reportError(
          method->line, method->column, method->length,
          "Abstract methods can only be declared within abstract classes.");
      hasErrors = true;
    }

    bool isVirtual = method->isAbstract;
    bool explicitOverride = false;
    for (auto *ann : method->annotations) {
      if (ann->name == "virtual")
        isVirtual = true;
      if (ann->name == "override")
        explicitOverride = true;
    }

    bool foundInBaseOrIface = false;
    auto checkBaseHierarchy = [&](const ClassDeclNode *cDecl,
                                  bool fromInterface, auto &self) -> void {
      if (!cDecl || foundInBaseOrIface)
        return;
      for (auto *pm : cDecl->methods) {
        if (signaturesMatch(method, pm)) {
          if (!pm->isVirtual && !pm->isOverride && !pm->isAbstract) {
            ctx->reportError(
                method->line, method->column, method->length,
                "Method '" + std::string(method->name) +
                    "' marked @override but base method is not virtual.");
            hasErrors = true;
          }
          foundInBaseOrIface = true;
          return;
        }
      }
      if (cDecl->baseClass) {
        if (auto *pType = llvm::dyn_cast<ClassType>(
                cDecl->baseClass->getUnqualifiedType())) {
          self(llvm::dyn_cast_or_null<ClassDeclNode>(pType->getDeclaration()),
               fromInterface, self);
        }
      }
      for (auto *iface : cDecl->interfaces) {
        if (auto *iType =
                llvm::dyn_cast<ClassType>(iface->getUnqualifiedType())) {
          self(llvm::dyn_cast_or_null<ClassDeclNode>(iType->getDeclaration()),
               true, self);
        }
      }
    };

    if (node->baseClass) {
      if (auto *pType = llvm::dyn_cast<ClassType>(
              node->baseClass->getUnqualifiedType())) {
        checkBaseHierarchy(
            llvm::dyn_cast_or_null<ClassDeclNode>(pType->getDeclaration()),
            false, checkBaseHierarchy);
      }
    }
    if (!foundInBaseOrIface) {
      for (auto *iface : resolvedInterfaces) {
        if (auto *iType =
                llvm::dyn_cast<ClassType>(iface->getUnqualifiedType())) {
          checkBaseHierarchy(
              llvm::dyn_cast_or_null<ClassDeclNode>(iType->getDeclaration()),
              true, checkBaseHierarchy);
        }
        if (foundInBaseOrIface)
          break;
      }
    }

    if (foundInBaseOrIface) {
      isVirtual = true;
    }

    if (explicitOverride && !foundInBaseOrIface) {
      ctx->reportError(method->line, method->column, method->length,
                       "Method '" + std::string(method->name) +
                           "' marked @override but no matching method found "
                           "in base classes or interfaces.");
      hasErrors = true;
    }

    const_cast<FunctionDeclNode *>(method)->isVirtual = isVirtual;
    const_cast<FunctionDeclNode *>(method)->isOverride =
        foundInBaseOrIface || explicitOverride;

    if (method->isOverride || method->isVirtual) {
      /* Same-named methods share a global slot (base, interface and
       * override alike); the registry re-stamps every indexed method when a
       * new name shifts the earlier ranks. */
      const_cast<FunctionDeclNode *>(method)->vtableIndex =
          ctx->assignVTableSlot(const_cast<FunctionDeclNode *>(method));
    }
  }

  if (!node->isAbstract && !hasErrors) {
    auto hasImplementation = [&](const FunctionDeclNode *reqMethod) {
      const ClassDeclNode *current = node;
      while (current) {
        for (const auto *m : current->methods) {
          if (!m->isAbstract && signaturesMatch(m, reqMethod)) {
            return true;
          }
        }
        if (current->baseClass) {
          if (auto *pType = llvm::dyn_cast<ClassType>(
                  current->baseClass->getUnqualifiedType())) {
            current =
                llvm::dyn_cast_or_null<ClassDeclNode>(pType->getDeclaration());
          } else {
            current = nullptr;
          }
        } else {
          current = nullptr;
        }
      }
      return false;
    };

    auto collectRequiredMethods = [&](const ClassDeclNode *ifaceDecl,
                                      auto &self) -> void {
      if (!ifaceDecl)
        return;
      for (const auto *m : ifaceDecl->methods) {
        if (m->isStatic || m->name == ifaceDecl->name || m->name == "~")
          continue;
        if (!m->isPublic(m->name) && !m->isProtected(m->name))
          continue;

        if (!hasImplementation(m)) {
          ctx->reportError(node->line, node->column, node->length,
                           "Class '" + std::string(node->name) +
                               "' must implement method '" +
                               std::string(m->name) + "' from interface '" +
                               std::string(ifaceDecl->name) + "'.");
          hasErrors = true;
        }
      }
      if (ifaceDecl->baseClass) {
        if (auto *pType = llvm::dyn_cast<ClassType>(
                ifaceDecl->baseClass->getUnqualifiedType())) {
          self(llvm::dyn_cast_or_null<ClassDeclNode>(pType->getDeclaration()),
               self);
        }
      }
      for (const auto *superIface : ifaceDecl->interfaces) {
        if (auto *pType =
                llvm::dyn_cast<ClassType>(superIface->getUnqualifiedType())) {
          self(llvm::dyn_cast_or_null<ClassDeclNode>(pType->getDeclaration()),
               self);
        }
      }
    };

    for (const auto *iface : resolvedInterfaces) {
      if (auto *iType =
              llvm::dyn_cast<ClassType>(iface->getUnqualifiedType())) {
        if (auto *iDecl = llvm::dyn_cast_or_null<ClassDeclNode>(
                iType->getDeclaration())) {
          collectRequiredMethods(iDecl, collectRequiredMethods);
        }
      }
    }
  }

  /* The base class must survive unrelated class-body errors: dropping it
   * here would break upcasting (derived-to-base pointer conversions) and
   * produce misleading cascade errors after any method-level diagnostic.
   * Resolution failures already null node->baseClass above. */
  classTy->setBaseClass(node->baseClass);
  classTy->setIsPolymorphic(isPolymorphic);
  classTy->setIsAbstract(node->isAbstract);

  /* Dispatch fields up front so template-typed fields are resolved
   * (resolveIfTemplate) and the FieldInfo layout below carries resolved
   * types instead of raw TemplateInstType placeholders. */
  for (const auto *field : node->fields) {
    auto res = dispatch(field);
    if (!res)
      hasErrors = true;
  }

  /* Rebuild the field layout with the now-resolved field types. */
  {
    std::vector<FieldInfo> resolvedInfos;
    uint32_t resolvedIdx = 0;
    if (isPolymorphic) {
      resolvedIdx = 1;
    }
    if (node->baseClass && !hasErrors) {
      if (auto *pType =
              llvm::dyn_cast<ClassType>(node->baseClass->getUnqualifiedType())) {
        if (auto *pDecl = llvm::dyn_cast_or_null<ClassDeclNode>(
                pType->getDeclaration())) {
          auto collectResolved =
              [&](const ClassDeclNode *cDecl, auto &self) -> void {
            if (!cDecl)
              return;
            if (cDecl->baseClass) {
              if (auto *pt = llvm::dyn_cast<ClassType>(
                      cDecl->baseClass->getUnqualifiedType())) {
                if (auto *pd = llvm::dyn_cast_or_null<ClassDeclNode>(
                        pt->getDeclaration())) {
                  self(pd, self);
                }
              }
            }
            for (auto *f : cDecl->fields) {
              if (f->isStatic)
                continue;
              bool isPub = f->isPublic(f->varName);
              bool isProt = f->isProtected(f->varName);
              std::string_view fname = (!isPub && !isProt) ? "" : f->varName;
              resolvedInfos.push_back(
                  {fname, f->type, resolvedIdx++, isPub, isProt});
            }
          };
          collectResolved(pDecl, collectResolved);
        }
      }
    }
    for (auto *f : node->fields) {
      if (f->isStatic)
        continue;
      resolvedInfos.push_back({f->varName, f->type, resolvedIdx++,
                               f->isPublic(f->varName),
                               f->isProtected(f->varName)});
    }
    classTy->setFields(ctx->astCtx.copyArray<FieldInfo>(resolvedInfos));
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

  /* Dart-style const constructors: validate all of them after the fields
   * (and this-params) are resolved. */
  for (const auto *ctor : node->constructors) {
    this->checkConstConstructor(ctor);
  }

  /* Dart 'final' field rules: a final field initialized at its declaration
   * cannot be re-initialized by any constructor, and a final field without
   * a declaration initializer must be initialized by every constructor
   * (through 'this.x' parameters or ': this.x = expr' entries). */
  for (const auto *field : node->fields) {
    if (!field->isFinal || field->isStatic)
      continue;
    for (const auto *ctor : node->constructors) {
      bool viaThisParam = false;
      bool viaFieldInit = false;
      for (const auto *param : ctor->params) {
        if (param->isThisParam && param->name == field->varName) {
          viaThisParam = true;
          break;
        }
      }
      for (const auto *init : ctor->fieldInitializers) {
        if (auto *target = llvm::dyn_cast<MemberAccessNode>(init->target)) {
          if (target->memberName == field->varName) {
            viaFieldInit = true;
            break;
          }
        }
      }
      if (field->initializer && (viaThisParam || viaFieldInit)) {
        ctx->reportError(ctor->line, ctor->column, ctor->length,
                         "The final field '" + std::string(field->varName) +
                             "' is already initialized at its declaration.");
        hasErrors = true;
      }
      if (viaThisParam && viaFieldInit) {
        ctx->reportError(ctor->line, ctor->column, ctor->length,
                         "The final field '" + std::string(field->varName) +
                             "' is initialized twice in the initializer list "
                             "of this constructor.");
        hasErrors = true;
      }
      if (!field->initializer && !viaThisParam && !viaFieldInit) {
        ctx->reportError(ctor->line, ctor->column, ctor->length,
                         "The final field '" + std::string(field->varName) +
                             "' must be initialized in every constructor or "
                             "at its declaration.");
        hasErrors = true;
      }
    }
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

      auto parseEnumInt = [&](const NumberNode *num,
                              const std::string &kind) -> bool {
        try {
          val = std::stoll(std::string(num->raw), nullptr, 0);
          return true;
        } catch (const std::exception &) {
          ctx->reportError(num->line, num->column, num->length,
                           kind + " value is out of range for an enum member");
          hasErrors = true;
          return false;
        }
      };

      if (init->kind == NodeKind::Number) {
        parseEnumInt(static_cast<const NumberNode *>(init),
                     "Enum member literal");
      } else if (init->kind == NodeKind::UnaryOp) {
        auto uop = static_cast<const UnaryOpNode *>(init);
        if (uop->op == "-" && uop->expr->kind == NodeKind::Number) {
          if (!parseEnumInt(static_cast<const NumberNode *>(uop->expr),
                            "Enum member literal")) {
            continue;
          }
          val = -val;
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
  /* Resolve template-instance placeholders ('Future<int32>') so
   * sizeof/typeof/alignof see the concrete record type at codegen. */
  const_cast<TypeLiteralNode *>(node)->representedType =
      resolveIfTemplate(node->representedType);

  /* A placeholder that survives resolution is an unresolved type name
   * ('sizeof(UndefinedType)'): template bodies are not type-checked until
   * instantiation, so by the time this runs genuine template parameters
   * have already been substituted away. Report it here instead of leaving
   * codegen to trip on a location-less error. */
  const Type *rep = node->representedType;
  if (rep && rep->getKind() == TypeKind::TemplateParam) {
    return ctx->reportError(node->line, node->column, node->length,
                            "Unknown type in type literal: '" +
                                rep->toString() + "'");
  }

  node->exprType = ctx->astCtx.TypeValTy;
  node->isLValue = false;
  return node->exprType;
}

SemaResult TypeCheckPass::visit(const VariableNode *node) {
  if (node->name == "super") {
    const RecordType *currRec = ctx->getCurrentRecordContext();
    if (!currRec || currRec->getKind() != TypeKind::Class) {
      return ctx->reportError(node->line, node->column, node->length,
                              "Use of 'super' outside of a class method.");
    }
    auto *classTy = static_cast<const ClassType *>(currRec);
    if (!classTy->getBaseClass()) {
      return ctx->reportError(node->line, node->column, node->length,
                              "Class '" + std::string(classTy->getName()) +
                                  "' has no base class.");
    }
    const Type *baseTy = classTy->getBaseClass()->getUnqualifiedType();
    const_cast<VariableNode *>(node)->representedType = baseTy;
    node->exprType = ctx->astCtx.getPointerType(baseTy);
    node->isLValue = false;
    return node->exprType;
  }

  if (!node->templateArgs.empty()) {
    auto decls = ctx->lookup(node->name);
    DeclNode *tmplDecl = nullptr;
    for (auto *d : decls) {
      if (auto *funcDecl = llvm::dyn_cast<FunctionDeclNode>(d)) {
        if (funcDecl->isTemplate) {
          tmplDecl = const_cast<DeclNode *>(d);
          break;
        }
      }
    }

    if (!tmplDecl) {
      auto it = ctx->templateRegistry.find(node->name);
      if (it != ctx->templateRegistry.end() &&
          llvm::isa<FunctionDeclNode>(it->second)) {
        if (!ctx->currentModule ||
            ctx->currentModule->canSee(it->second->declFilePath)) {
          tmplDecl = const_cast<DeclNode *>(it->second);
        }
      }
    }

    if (tmplDecl) {
      /* Partial explicit type arguments (deduced by the call site) are
       * completed before this point; any other size mismatch is an error. */
      if (node->templateArgs.size() != tmplDecl->templateParams.size()) {
        return ctx->reportError(
            node->line, node->column, node->length,
            "Wrong number of template arguments for '" +
                std::string(node->name) + "': expected " +
                std::to_string(tmplDecl->templateParams.size()) +
                ", got " + std::to_string(node->templateArgs.size()) + ".");
      }
      /* Mangle from the fully-qualified template name so namespace-qualified
       * templates ('std.make_unique_Inner') resolve consistently with the
       * type-instantiation path. */
      std::string mangledName = std::string(tmplDecl->fqName);
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
        std::vector<const Type *> fnArgs;
        for (size_t i = 0; i < tmplDecl->templateParams.size(); ++i) {
          const Type *resArg = resolveIfTemplate(node->templateArgs[i]);
          templateArgMap[tmplDecl->templateParams[i]] = resArg;
          fnArgs.push_back(resArg);
        }
        if (!checkTemplateConstraints(tmplDecl, fnArgs, node))
          return std::unexpected(ErrorInfo{
              node->line, node->column, node->length,
              "Template constraints are not satisfied"});

        ASTCloner cloner(ctx->astCtx, templateArgMap);
        DeclNode *instDecl = llvm::cast<DeclNode>(cloner.dispatch(tmplDecl));

        if (instDecl) {
          llvm::cast<FunctionDeclNode>(instDecl)->name = mangledView;
          instDecl->fqName = mangledView;
          instDecl->hasPublicMod = tmplDecl->hasPublicMod;
          instDecl->hasPrivateMod = tmplDecl->hasPrivateMod;
          instDecl->hasProtectedMod = tmplDecl->hasProtectedMod;
          instDecl->annotations = tmplDecl->annotations;
          instDecl->declFilePath = tmplDecl->declFilePath;

          /* The substitution is complete: the instantiated function is
           * concrete and its body can be type-checked now. */
          auto *instFn = llvm::cast<FunctionDeclNode>(instDecl);
          instFn->isTemplate = false;
          instFn->templateParams = {};

          if (ctx->currentModule) {
            const_cast<ModuleNode *>(ctx->currentModule)
                ->instantiatedTemplates.push_back(instDecl);
          }
          DeclCollectorPass dcp;
          dcp.ctx = ctx;

          auto prevFile = ctx->currentFile;
          ctx->setCurrentFile(instDecl->declFilePath);

          ctx->pushScope();

          /* Same namespace restoration as the record-template path: generic
           * method/function bodies must resolve symbols from the namespace
           * where the template was originally declared. */
          std::string instNs = getDeclNamespace(tmplDecl);
          size_t nsDepth = pushDeclNamespace(ctx, instNs);

          dcp.dispatch(instDecl);
          dispatch(instDecl);

          for (size_t i = 0; i < nsDepth; ++i)
            ctx->popNamespace();
          ctx->popScope();

          ctx->symTable.addGlobalSymbol(instDecl->fqName, instDecl);

          ctx->setCurrentFile(prevFile);
        }
      }
      const_cast<VariableNode *>(node)->name = mangledView;
    }
  }

  auto decls = ctx->lookup(node->name);

  bool isLocal = false;
  for (auto *d : decls) {
    if (d->kind == NodeKind::ParamDecl ||
        (d->kind == NodeKind::VarDecl &&
         !static_cast<const VarDeclNode *>(d)->isGlobal)) {
      isLocal = true;
      break;
    }
  }

  if (!isLocal) {
    auto thisDecls = ctx->lookup("this");
    if (!thisDecls.empty()) {
      const DeclNode *thisDecl = thisDecls.front();
      if (auto *paramDecl = llvm::dyn_cast<ParamDeclNode>(thisDecl)) {
        const Type *thisTy = paramDecl->type;
        if (auto *ptrTy = llvm::dyn_cast<PointerType>(thisTy)) {
          const Type *pointee = ptrTy->getPointeeType();
          const Type *unqualPointee = pointee->getUnqualifiedType();
          if (auto *clsTy = llvm::dyn_cast<RecordType>(unqualPointee)) {
            if (auto field = clsTy->getField(node->name)) {
              bool isPub = field->isPublic;
              bool isProt = field->isProtected;
              bool canAccess = false;
              auto *currCtx = ctx->getCurrentRecordContext();

              if (isPub) {
                canAccess = true;
              } else if (isProt) {
                if (currCtx == clsTy)
                  canAccess = true;
                else if (currCtx) {
                  const ClassType *currClass =
                      llvm::dyn_cast<ClassType>(currCtx);
                  while (currClass) {
                    if (currClass == clsTy) {
                      canAccess = true;
                      break;
                    }
                    if (currClass->getBaseClass()) {
                      currClass = llvm::dyn_cast<ClassType>(
                          currClass->getBaseClass()->getUnqualifiedType());
                    } else
                      break;
                  }
                }
              } else {
                if (currCtx == clsTy) {
                  canAccess = true;
                }
              }

              if (!canAccess) {
                return ctx->reportError(
                    node->line, node->column, node->length,
                    "Cannot access private/protected field '" +
                        std::string(node->name) + "'");
              }
              const_cast<VariableNode *>(node)->isField = true;
              const_cast<VariableNode *>(node)->fieldIndex = field->index;
              const_cast<VariableNode *>(node)->parentType = clsTy;
              node->exprType = field->type;
              node->isLValue = true;

              if (const DeclNode *recDecl = clsTy->getDeclaration()) {
                llvm::ArrayRef<VarDeclNode *> fields;
                if (auto *cDecl = llvm::dyn_cast<ClassDeclNode>(recDecl))
                  fields = cDecl->fields;
                else if (auto *sDecl = llvm::dyn_cast<StructDeclNode>(recDecl))
                  fields = sDecl->fields;
                else if (auto *uDecl = llvm::dyn_cast<UnionDeclNode>(recDecl))
                  fields = uDecl->fields;

                for (const auto *fDecl : fields) {
                  if (fDecl->varName == node->name) {
                    checkDeprecated(fDecl, node);
                    const_cast<VariableNode *>(node)->resolvedDecl = fDecl;
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
        if (auto *cDecl = llvm::dyn_cast<ClassDeclNode>(recDecl)) {
          cFields = cDecl->fields;
          cMethods = cDecl->methods;
        } else if (auto *sDecl = llvm::dyn_cast<StructDeclNode>(recDecl)) {
          cFields = sDecl->fields;
          cMethods = sDecl->methods;
        } else if (auto *uDecl = llvm::dyn_cast<UnionDeclNode>(recDecl)) {
          cFields = uDecl->fields;
          cMethods = uDecl->methods;
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
                m->effectiveReturnType ? m->effectiveReturnType : m->returnType,
                ctx->astCtx.copyArray<const Type *>(pTypes));
            node->exprType = ctx->astCtx.getPointerType(funcTy);
            node->isLValue = true;
            checkDeprecated(m, node);
            return node->exprType;
          }
        }
      }
    }
  }

  if (decls.empty()) {
    return ctx->reportError(node->line, node->column, node->length,
                            "Undefined identifier: '" +
                                std::string(node->name) + "'");
  }

  const Type *ty = nullptr;
  const DeclNode *target = decls.front();

  while (target) {
    if (auto *td = llvm::dyn_cast_or_null<TypedefDeclNode>(target)) {
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
    } else {
      break;
    }
  }

  if (llvm::isa<StructDeclNode>(target) || llvm::isa<ClassDeclNode>(target) ||
      llvm::isa<UnionDeclNode>(target) || llvm::isa<EnumDeclNode>(target) ||
      llvm::isa<TypedefDeclNode>(target) ||
      llvm::isa<NamespaceDeclNode>(target)) {

    if (auto *nsDecl = llvm::dyn_cast<NamespaceDeclNode>(target)) {
      const_cast<VariableNode *>(node)->resolvedDecl = target;
      node->exprType = ctx->astCtx.NamespaceTy;
      node->isLValue = false;
      checkDeprecated(target, node);
      return node->exprType;
    }

    const Type *repTy = nullptr;
    if (auto *structDecl = llvm::dyn_cast<StructDeclNode>(target))
      repTy = structDecl->recordType;
    else if (auto *classDecl = llvm::dyn_cast<ClassDeclNode>(target))
      repTy = classDecl->recordType;
    else if (auto *unionDecl = llvm::dyn_cast<UnionDeclNode>(target))
      repTy = unionDecl->recordType;
    else if (auto *enumDecl = llvm::dyn_cast<EnumDeclNode>(target))
      repTy = enumDecl->enumType;
    else if (auto *typedefDecl = llvm::dyn_cast<TypedefDeclNode>(target))
      repTy = typedefDecl->aliasType;

    /* A template record in value position ('sizeof(_Entry<K, V>)') must
     * resolve to its instantiation, not the raw template: the raw record's
     * fields still hold template parameters, which would reach codegen. */
    if (repTy && !node->templateArgs.empty() &&
        llvm::isa<RecordType>(repTy->getUnqualifiedType())) {
      const Type *instTy = ctx->astCtx.getTemplateInstType(
          static_cast<const RecordType *>(repTy->getUnqualifiedType())
              ->getName(),
          node->templateArgs);
      repTy = resolveIfTemplate(instTy);
    }

    const_cast<VariableNode *>(node)->resolvedDecl = target;
    const_cast<VariableNode *>(node)->representedType = repTy;
    node->exprType = ctx->astCtx.TypeValTy;
    node->isLValue = false;
    checkDeprecated(target, node);
    return node->exprType;
  }

  if (auto *varTarget = llvm::dyn_cast<VarDeclNode>(target)) {
    if (!target->isPublic(varTarget->varName) &&
        !target->isProtected(varTarget->varName) &&
        target->declFilePath != ctx->currentFile) {
      return ctx->reportError(node->line, node->column, node->length,
                              "Cannot access private variable '" +
                                  std::string(node->name) +
                                  "' from outside its file.");
    }
    ty = varTarget->type;
  } else if (auto *paramTarget = llvm::dyn_cast<ParamDeclNode>(target)) {
    ty = paramTarget->type;
    if (auto *arrTy = llvm::dyn_cast<ArrayType>(ty)) {
      ty = ctx->astCtx.getPointerType(arrTy->getElementType());
    }
  } else if (auto *fDecl = llvm::dyn_cast<FunctionDeclNode>(target)) {
    if (!target->isPublic(fDecl->name) &&
        target->declFilePath != ctx->currentFile) {
      return ctx->reportError(node->line, node->column, node->length,
                              "Cannot access private function '" +
                                  std::string(node->name) +
                                  "' from outside its file.");
    }
    std::vector<const Type *> pTypes;
    for (auto *p : fDecl->params)
      pTypes.push_back(p->type);

    const Type *funcTy = ctx->astCtx.getFunctionType(
        fDecl->effectiveReturnType ? fDecl->effectiveReturnType
                                   : fDecl->returnType,
        ctx->astCtx.copyArray<const Type *>(pTypes));
    ty = ctx->astCtx.getPointerType(funcTy);
  } else if (auto *tdDecl = llvm::dyn_cast<TypedefDeclNode>(target)) {
    if (!target->isPublic(tdDecl->aliasName) &&
        target->declFilePath != ctx->currentFile) {
      return ctx->reportError(node->line, node->column, node->length,
                              "Cannot access private type alias '" +
                                  std::string(node->name) +
                                  "' from outside its file.");
    }
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

  /* Apply an active 'is' type promotion: 'if (x is T)' narrows the variable
   * to T (preserving pointer/reference indirection) inside the block. */
  for (const auto &p : promotions) {
    if (p.active && p.decl == target) {
      node->exprType = p.type;
      return p.type;
    }
  }
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

  /* Dart-style type promotion: 'if (x is T) { ... }' narrows the variable's
   * type inside the block, and 'if (x is! T) { } else { }' narrows it inside
   * the else block. */
  std::vector<Promotion> thenP, elseP;
  collectPromotions(node->condition, thenP, elseP);

  size_t promoDepth = promotions.size();
  promotions.insert(promotions.end(), thenP.begin(), thenP.end());
  auto thenRes = dispatch(node->thenBlock);
  promotions.resize(promoDepth);
  if (!thenRes)
    return thenRes;

  if (node->elseBlock) {
    promotions.insert(promotions.end(), elseP.begin(), elseP.end());
    auto elseRes = dispatch(node->elseBlock);
    promotions.resize(promoDepth);
    if (!elseRes)
      return elseRes;
  }

  return ctx->astCtx.VoidTy;
}

void TypeCheckPass::collectPromotions(const ExprNode *cond,
                                      std::vector<Promotion> &thenP,
                                      std::vector<Promotion> &elseP) {
  if (!cond)
    return;

  if (auto *isNode = llvm::dyn_cast<IsExprNode>(cond)) {
    auto *var = llvm::dyn_cast<VariableNode>(isNode->expr);
    if (!var || !var->resolvedDecl || !isNode->promotionType)
      return;
    if (!isNode->isNegated) {
      /* 'x is T' true ⇒ x is T inside the then block. */
      thenP.push_back({var->resolvedDecl, isNode->promotionType, true});
    } else {
      /* 'x is! T' false ⇒ x is T inside the else block. */
      elseP.push_back({var->resolvedDecl, isNode->promotionType, true});
    }
    return;
  }

  auto *bop = llvm::dyn_cast<BinaryOpNode>(cond);
  if (!bop || (bop->op != "&&" && bop->op != "||"))
    return;

  std::vector<Promotion> tL, eL, tR, eR;
  collectPromotions(bop->left, tL, eL);
  collectPromotions(bop->right, tR, eR);
  if (bop->op == "&&") {
    /* 'A && B' true ⇒ both sides are true. */
    thenP = tL;
    thenP.insert(thenP.end(), tR.begin(), tR.end());
  } else {
    /* 'A || B' false ⇒ both sides are false. */
    elseP = eL;
    elseP.insert(elseP.end(), eR.begin(), eR.end());
  }
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

/* Dart-style 'for (var x in expr)' loop.
 *
 * Two lowerings are produced, both executed as plain code with no vtables,
 * no Iterable hierarchy and no allocation:
 *
 *   - Record (or pointer/reference-to-record) iterables follow the
 *     structural protocol 'expr.iterator() -> { bool moveNext(); T&
 *     current(); }':
 *         T& __range = expr;                 // no copy; an rvalue is
 *                                            // materialized in a stack
 *                                            // temp for the loop's life
 *         var __it = __range.iterator();
 *         while (__it.moveNext()) {
 *           <loopVar> = __it.current();
 *           <body>
 *         }
 *     The range binding keeps iterators pointing into an rvalue alive
 *     (C++ lifetime-extension semantics), so 'for (var x in getList())'
 *     is safe. @inline'd iterators collapse to an index loop in LLVM.
 *
 *   - Array iterables ('T[N]', including '[...]' literals) lower to a
 *     plain index loop over the array itself:
 *         for (usize i = 0; i < N; i++) {
 *           <loopVar> = <expr>[i];
 *           <body>
 *         }
 *     The array is evaluated once (it is the subscript base), and element
 *     access is a raw GEP, the maximum-zero-cost path.
 *
 * The header variable semantics follow the declaration form:
 *   'var x'    copy of the element each iteration (rebindable, Dart-like)
 *   'final x'  non-rebindable copy
 *   'var& x'   binds a reference to the element (no copy)
 *   'final& x' binds a const reference to the element
 *   'T x'      explicit element type (cast-checked each iteration)
 */
SemaResult TypeCheckPass::visit(const ForInNode *node) {
  ScopeGuard guard(*ctx, ScopeKind::ControlFlowInit);
  auto &ac = ctx->astCtx;

  const int line = node->line;
  const int col = node->column;
  const int len = node->length;

  auto iterableRes = dispatch(node->iterable);
  if (!iterableRes)
    return std::unexpected(iterableRes.error());

  const Type *iterableTy = (*iterableRes)->getUnqualifiedType();

  /* Fixed-size arrays: index loop, no iterator protocol. */
  if (iterableTy->getKind() == TypeKind::Array) {
    return lowerForInArray(node, iterableTy, line, col, len);
  }

  if (!llvm::isa<RecordType>(iterableTy)) {
    return ctx->reportError(
        line, col, len,
        "Cannot iterate a value of type '" + iterableTy->toString() +
            "': for-in requires a record type with an 'iterator()' method "
            "(List, Map, String views, or a user type providing the "
            "iterator()/moveNext()/current() protocol), or a fixed-size "
            "array.");
  }

  /* The iterable is evaluated exactly once, as the initializer of the
   * hidden range binding below (Sema already checked it; CodeGen lowers the
   * binding into a reference (direct bind for lvalues, stack temporary for
   * rvalues), so no copy happens either way). */
  std::string rangeName = "__forin_range_" + std::to_string(forInTempCounter++);
  std::string itName = "__forin_it_" + std::to_string(forInTempCounter++);
  std::string_view rangeNameSv = ac.copyString(rangeName);
  std::string_view itNameSv = ac.copyString(itName);

  auto *rangeVar = ac.create<VarDeclNode>(
      ac.getReferenceType(iterableTy), rangeNameSv, node->iterable, line, col,
      len);
  rangeVar->declFilePath = ctx->currentFile;
  rangeVar->isInitialized = true;
  /* Not dispatched: the initializer is already fully checked, and skipping
   * the visit avoids re-checking it (and the spurious rvalue-binding
   * warning for 'for (var x in getList())'). CodeGen executes the decl as a
   * plain reference binding. */
  ctx->addDecl(rangeNameSv, rangeVar);

  /* var __it = __range.iterator(); */
  auto *rangeRef = ac.create<VariableNode>(rangeNameSv, line, col, 1);
  auto *iteratorMa = ac.create<MemberAccessNode>(
      rangeRef, ac.copyString("iterator"), line, col, len);
  auto *iteratorCall =
      ac.create<FunctionCallNode>(iteratorMa, llvm::ArrayRef<ExprNode *>(), llvm::ArrayRef<std::string_view>(), line, col, len);
  auto *itVar =
      ac.create<VarDeclNode>(ac.AutoTy, itNameSv, iteratorCall, line, col, len);
  itVar->declFilePath = ctx->currentFile;
  itVar->isInitialized = true;
  auto itRes = dispatch(itVar);
  if (!itRes)
    return std::unexpected(itRes.error());

  /* Probe 'it.current()' on a throwaway node to learn the element type;
   * the real call below is dispatched exactly once, inside the loop
   * variable declaration. */
  auto *probeItRef = ac.create<VariableNode>(itNameSv, line, col, 1);
  auto *probeCurrentMa = ac.create<MemberAccessNode>(
      probeItRef, ac.copyString("current"), line, col, len);
  auto *probeCurrentCall =
      ac.create<FunctionCallNode>(probeCurrentMa, llvm::ArrayRef<ExprNode *>(), llvm::ArrayRef<std::string_view>(), line, col, len);
  auto curRes = dispatch(probeCurrentCall);
  if (!curRes)
    return std::unexpected(curRes.error());

  const Type *curTy = *curRes;
  if (curTy->isVoid()) {
    return ctx->reportError(line, col, len,
                            "The iterator's 'current()' method must return "
                            "an element type, not 'void'.");
  }

  const Type *elementTy = curTy;
  if (elementTy->isReferenceType()) {
    elementTy = static_cast<const ReferenceType *>(elementTy)->getPointeeType();
  }

  /* Loop variable declaration: 'var x' keeps AutoTy (copy semantics);
   * 'var& x'/'final& x' bind a (const) reference to the element. */
  auto *loopVar = node->loopVar;
  if (node->isRefBinding) {
    const Type *boundTy =
        loopVar->isFinal ? ac.getConstType(elementTy) : elementTy;
    loopVar->type = ac.getReferenceType(boundTy);
    loopVar->isInitialized = true;
  }

  auto *itRef = ac.create<VariableNode>(itNameSv, line, col, 1);
  auto *currentMa = ac.create<MemberAccessNode>(
      itRef, ac.copyString("current"), line, col, len);
  auto *currentCall =
      ac.create<FunctionCallNode>(currentMa, llvm::ArrayRef<ExprNode *>(), llvm::ArrayRef<std::string_view>(), line, col, len);
  loopVar->initializer = currentCall;
  loopVar->isInitialized = true;

  /* bool moveNext() loop condition (checked on a node that is then stored
   * in the desugared tree; it is dispatched only once here. CodeGen runs
   * a separate visitor). */
  auto *mnItRef = ac.create<VariableNode>(itNameSv, line, col, 1);
  auto *mnMa = ac.create<MemberAccessNode>(
      mnItRef, ac.copyString("moveNext"), line, col, len);
  auto *mnCall = ac.create<FunctionCallNode>(mnMa, llvm::ArrayRef<ExprNode *>(), llvm::ArrayRef<std::string_view>(), line, col, len);
  auto mnRes = dispatch(mnCall);
  if (!mnRes)
    return std::unexpected(mnRes.error());
  if (!canImplicitlyCast(*mnRes, ctx->astCtx.BoolTy)) {
    return ctx->reportError(
        line, col, len,
        "The iterator's 'moveNext()' method must return a boolean type.");
  }

  /* Body scope: the loop variable lives here, visible to the body. */
  ScopeGuard bodyGuard(*ctx);
  LoopGuard loopGuard(*ctx);
  auto varRes = dispatch(loopVar);
  if (!varRes)
    return std::unexpected(varRes.error());
  auto bodyRes = dispatch(node->body);
  if (!bodyRes)
    return bodyRes;

  /* Assemble the desugared tree: { T& range = expr; var it = ...;
   * while (it.moveNext()) { loopVar = it.current(); body } } */
  std::vector<ASTNode *> bodyStmts;
  bodyStmts.push_back(loopVar);
  for (auto *s : node->body->statements)
    bodyStmts.push_back(s);
  auto *whileBody = ac.create<BlockNode>(line, col);
  whileBody->statements = ac.copyArray<ASTNode *>(bodyStmts);
  auto *whileNode = ac.create<WhileNode>(mnCall, whileBody, line, col, len);
  whileNode->endLine = node->endLine;

  std::vector<ASTNode *> outerStmts;
  outerStmts.push_back(rangeVar);
  outerStmts.push_back(itVar);
  outerStmts.push_back(whileNode);
  auto *outerBlock = ac.create<BlockNode>(line, col);
  outerBlock->statements = ac.copyArray<ASTNode *>(outerStmts);
  outerBlock->endLine = node->endLine;

  const_cast<ForInNode *>(node)->desugared = outerBlock;
  return ctx->astCtx.VoidTy;
}

SemaResult TypeCheckPass::lowerForInArray(const ForInNode *node,
                                          const Type *arrayTy, int line,
                                          int col, int len) {
  auto &ac = ctx->astCtx;
  const auto *arrTy = static_cast<const ArrayType *>(arrayTy);
  const Type *elemTy = arrTy->getElementType();
  const uint64_t arrSize = arrTy->getSize();

  if (elemTy->isVoid()) {
    return ctx->reportError(line, col, len,
                            "Cannot iterate an untyped (empty) array "
                            "literal: the element type is unknown.");
  }

  /* Hidden counter: 'var __i = 0; __i < N; __i++'. */
  std::string idxName = "__forin_idx_" + std::to_string(forInTempCounter++);
  std::string_view idxNameSv = ac.copyString(idxName);

  auto *zeroNode = ac.create<NumberNode>("0", false, line, col, 1);
  auto *idxVar =
      ac.create<VarDeclNode>(ac.USizeTy, idxNameSv, zeroNode, line, col, len);
  idxVar->declFilePath = ctx->currentFile;
  idxVar->isInitialized = true;
  auto idxRes = dispatch(idxVar);
  if (!idxRes)
    return std::unexpected(idxRes.error());

  auto *idxRef = ac.create<VariableNode>(idxNameSv, line, col, 1);
  std::string sizeStr = std::to_string(arrSize);
  auto *sizeNode = ac.create<NumberNode>(ac.copyString(sizeStr), false, line,
                                         col, (int)sizeStr.length());
  auto *condNode = ac.create<BinaryOpNode>("<", idxRef, sizeNode, line, col);

  auto *incRef = ac.create<VariableNode>(idxNameSv, line, col, 1);
  auto *incNode =
      ac.create<UnaryOpNode>("++", incRef, line, col, /*postfix=*/true);

  /* <loopVar> = <expr>[__i]; the original iterable expression is the
   * subscript base and is evaluated once. */
  auto *subIdx = ac.create<VariableNode>(idxNameSv, line, col, 1);
  auto *subscript = ac.create<ArraySubscriptNode>(node->iterable, subIdx, line,
                                                  col, len);

  auto *loopVar = node->loopVar;
  if (node->isRefBinding) {
    const Type *boundTy =
        loopVar->isFinal ? ac.getConstType(elemTy) : elemTy;
    loopVar->type = ac.getReferenceType(boundTy);
    loopVar->isInitialized = true;
  }
  loopVar->initializer = subscript;
  loopVar->isInitialized = true;

  /* Type-check the synthesized pieces in order (the desugared tree is only
   * executed by CodeGen afterwards). */
  auto condRes = dispatch(condNode);
  if (!condRes)
    return std::unexpected(condRes.error());
  if (!canImplicitlyCast(*condRes, ctx->astCtx.BoolTy)) {
    return ctx->reportError(line, col, len,
                            "Array for-in bound must be a boolean type.");
  }
  auto incRes = dispatch(incNode);
  if (!incRes)
    return std::unexpected(incRes.error());

  ScopeGuard bodyGuard(*ctx);
  LoopGuard loopGuard(*ctx);
  auto varRes = dispatch(loopVar);
  if (!varRes)
    return std::unexpected(varRes.error());
  auto bodyRes = dispatch(node->body);
  if (!bodyRes)
    return bodyRes;

  std::vector<ASTNode *> bodyStmts;
  bodyStmts.push_back(loopVar);
  for (auto *s : node->body->statements)
    bodyStmts.push_back(s);
  auto *bodyBlock = ac.create<BlockNode>(line, col);
  bodyBlock->statements = ac.copyArray<ASTNode *>(bodyStmts);

  auto *forNode = ac.create<ForNode>(idxVar, condNode, incNode, bodyBlock,
                                     line, col, len);
  forNode->endLine = node->endLine;
  const_cast<ForInNode *>(node)->desugared = forNode;
  return ctx->astCtx.VoidTy;
}

SemaResult TypeCheckPass::visit(const WhileNode *node) {
  auto condRes = dispatch(node->condition);
  if (!condRes)
    return std::unexpected(condRes.error());

  if (!canImplicitlyCast(*condRes, ctx->astCtx.BoolTy)) {
    return ctx->reportError(node->condition->line, node->condition->column,
                            node->condition->length,
                            "While loop condition must evaluate to a boolean type.");
  }

  LoopGuard loopGuard(*ctx);
  auto bodyRes = dispatch(node->body);
  if (!bodyRes)
    return bodyRes;

  return ctx->astCtx.VoidTy;
}
/* C++-style exception handling. The try body is type-checked like any
 * other block; each catch clause introduces a scope holding the optional
 * binding variable, typed with the caught type. Catch matching happens at
 * runtime (see CodeGen), so no static compatibility check is required
 * beyond the caught type itself being valid. */
SemaResult TypeCheckPass::visit(const TryStmtNode *node) {
  if (ctx->getCurrentFunction() && ctx->getCurrentFunction()->isAsync) {
    return ctx->reportError(
        node->line, node->column, node->length,
        "'try' is not supported inside async functions.");
  }

  if (const FunctionDeclNode *fn = ctx->getCurrentFunction())
    const_cast<FunctionDeclNode *>(fn)->hasEH = true;

  auto bodyRes = dispatch(node->body);
  if (!bodyRes)
    return bodyRes;

  for (const auto *clause : node->clauses) {
    if (clause->isCatchAll) {
      ctx->enterCatch();
      auto cRes = dispatch(clause->body);
      ctx->exitCatch();
      if (!cRes)
        return cRes;
      continue;
    }

    const Type *catchTy = clause->catchType;
    if (!catchTy || catchTy->isVoid()) {
      return ctx->reportError(
          clause->line, clause->column, clause->length,
          "Catch clause requires a valid caught type.");
    }

    const Type *resolvedTy = resolveIfTemplate(catchTy);
    if (resolvedTy != catchTy)
      const_cast<CatchClauseNode *>(clause)->catchType = resolvedTy;
    catchTy = resolvedTy;

    /* An unresolved catch type (typo) would otherwise propagate a
     * placeholder into codegen, where it fails with a location-less error
     * (or never): report it at the clause. */
    if (catchTy->getKind() == TypeKind::TemplateParam) {
      return ctx->reportError(clause->line, clause->column, clause->length,
                              "Unknown type in catch clause: '" +
                                  catchTy->toString() + "'");
    }

    const Type *unqualTy = catchTy->getUnqualifiedType();
    if (unqualTy->getKind() == TypeKind::Array) {
      return ctx->reportError(
          clause->line, clause->column, clause->length,
          "Arrays cannot be caught; catch the element type instead.");
    }

    if (!checkTypeVisibility(catchTy, clause)) {
      return std::unexpected(ErrorInfo{clause->line, clause->column,
                                       clause->length, "Invalid catch type"});
    }

    /* Catch-by-value of a record with a custom destructor requires a copy
     * constructor (C++ semantics): the thrown object is copied into the
     * local variable and destroyed when the clause exits. */
    if (!catchTy->isReferenceType() &&
        catchTy->getKind() != TypeKind::RValueReference) {
      if (unqualTy->getKind() == TypeKind::Class ||
          unqualTy->getKind() == TypeKind::Struct ||
          unqualTy->getKind() == TypeKind::Union) {
        const auto *recTy = static_cast<const RecordType *>(unqualTy);
        if (const auto *decl = recTy->getDeclaration()) {
          llvm::ArrayRef<FunctionDeclNode *> ctors;
          if (decl->kind == NodeKind::ClassDecl)
            ctors = static_cast<const ClassDeclNode *>(decl)->constructors;
          else if (decl->kind == NodeKind::StructDecl)
            ctors = static_cast<const StructDeclNode *>(decl)->constructors;
          else if (decl->kind == NodeKind::UnionDecl)
            ctors = static_cast<const UnionDeclNode *>(decl)->constructors;

          const FunctionDeclNode *copyCtor = nullptr;
          for (auto *ctor : ctors) {
            if (ctor->params.size() != 1)
              continue;
            const Type *pType = ctor->params[0]->type;
            const Type *pointee = nullptr;
            if (pType->isReferenceType()) {
              pointee = static_cast<const ReferenceType *>(pType)
                            ->getPointeeType();
            } else if (pType->getKind() == TypeKind::RValueReference) {
              pointee = static_cast<const RValueReferenceType *>(pType)
                            ->getPointeeType();
            }
            if (pointee && pointee->getUnqualifiedType() == unqualTy) {
              copyCtor = ctor;
              break;
            }
          }

          const FunctionDeclNode *dtor = nullptr;
          if (decl->kind == NodeKind::ClassDecl)
            dtor = static_cast<const ClassDeclNode *>(decl)->destructor;
          else if (decl->kind == NodeKind::StructDecl)
            dtor = static_cast<const StructDeclNode *>(decl)->destructor;
          else if (decl->kind == NodeKind::UnionDecl)
            dtor = static_cast<const UnionDeclNode *>(decl)->destructor;

          if (copyCtor) {
            if (!copyCtor->isPublic(copyCtor->name) &&
                ctx->getCurrentRecordContext() != recTy) {
              (void)ctx->reportError(
                  clause->line, clause->column, clause->length,
                  "Cannot catch by value: copy constructor is private.");
            }
            const_cast<CatchClauseNode *>(clause)->copyCtor = copyCtor;
          } else if (dtor && !dtor->isImplicit) {
            return ctx->reportError(
                clause->line, clause->column, clause->length,
                "Cannot catch by value: the type has a custom destructor "
                "but no copy constructor.");
          }
        }
      }
    }

    ScopeGuard guard(*ctx);

    if (!clause->varName.empty()) {
      auto *varDecl = ctx->astCtx.create<VarDeclNode>(
          catchTy, clause->varName, nullptr, clause->line, clause->column,
          clause->length);
      varDecl->declFilePath = ctx->currentFile;
      varDecl->isInitialized = true;
      ctx->addDecl(clause->varName, varDecl);
    }

    ctx->enterCatch();
    auto cRes = dispatch(clause->body);
    ctx->exitCatch();
    if (!cRes)
      return cRes;
  }

  return ctx->astCtx.VoidTy;
}

/* 'throw expr;' raises a copy of the expression's value; a bare 'throw;'
 * rethrows the exception currently being handled. Any type can be thrown
 * (C++ rules); records with a custom destructor must be copyable. */
SemaResult TypeCheckPass::visit(const ThrowStmtNode *node) {
  if (ctx->getCurrentFunction() && ctx->getCurrentFunction()->isAsync) {
    return ctx->reportError(
        node->line, node->column, node->length,
        "'throw' is not supported inside async functions.");
  }

  const FunctionDeclNode *fn = ctx->getCurrentFunction();
  if (fn && fn->isMethod && fn->name == "~") {
    return ctx->reportError(node->line, node->column, node->length,
                            "Destructors cannot throw.");
  }

  if (fn)
    const_cast<FunctionDeclNode *>(fn)->hasEH = true;

  if (!node->value) {
    if (!ctx->isInCatch()) {
      return ctx->reportError(
          node->line, node->column, node->length,
          "A bare 'throw;' is only allowed inside a catch clause.");
    }
    const_cast<ThrowStmtNode *>(node)->isRethrow = true;
    return ctx->astCtx.VoidTy;
  }

  auto valRes = dispatch(node->value);
  if (!valRes)
    return std::unexpected(valRes.error());

  const Type *throwTy = *valRes;
  if (throwTy->isVoid()) {
    return ctx->reportError(node->value->line, node->value->column,
                            node->value->length,
                            "Cannot throw a void value.");
  }

  /* Arrays decay to pointers when thrown, mirroring C++. */
  const Type *thrownTy = throwTy;
  if (thrownTy->getUnqualifiedType()->getKind() == TypeKind::Array) {
    thrownTy = ctx->astCtx.getPointerType(
        static_cast<const ArrayType *>(thrownTy->getUnqualifiedType())
            ->getElementType());
  } else if (thrownTy->isReferenceType()) {
    thrownTy = static_cast<const ReferenceType *>(thrownTy)->getPointeeType();
  } else if (thrownTy->getKind() == TypeKind::RValueReference) {
    thrownTy = static_cast<const RValueReferenceType *>(thrownTy)
                   ->getPointeeType();
  }

  const Type *unqualTy = thrownTy->getUnqualifiedType();
  if (unqualTy->getKind() == TypeKind::Class ||
      unqualTy->getKind() == TypeKind::Struct ||
      unqualTy->getKind() == TypeKind::Union) {
    const auto *recTy = static_cast<const RecordType *>(unqualTy);
    if (const auto *decl = recTy->getDeclaration()) {
      llvm::ArrayRef<FunctionDeclNode *> ctors;
      if (decl->kind == NodeKind::ClassDecl)
        ctors = static_cast<const ClassDeclNode *>(decl)->constructors;
      else if (decl->kind == NodeKind::StructDecl)
        ctors = static_cast<const StructDeclNode *>(decl)->constructors;
      else if (decl->kind == NodeKind::UnionDecl)
        ctors = static_cast<const UnionDeclNode *>(decl)->constructors;

      const FunctionDeclNode *copyCtor = nullptr;
      for (auto *ctor : ctors) {
        if (ctor->params.size() != 1)
          continue;
        const Type *pType = ctor->params[0]->type;
        const Type *pointee = nullptr;
        if (pType->isReferenceType()) {
          pointee = static_cast<const ReferenceType *>(pType)
                        ->getPointeeType();
        } else if (pType->getKind() == TypeKind::RValueReference) {
          pointee = static_cast<const RValueReferenceType *>(pType)
                        ->getPointeeType();
        }
        if (pointee && pointee->getUnqualifiedType() == unqualTy) {
          copyCtor = ctor;
          break;
        }
      }

      const FunctionDeclNode *dtor = nullptr;
      if (decl->kind == NodeKind::ClassDecl)
        dtor = static_cast<const ClassDeclNode *>(decl)->destructor;
      else if (decl->kind == NodeKind::StructDecl)
        dtor = static_cast<const StructDeclNode *>(decl)->destructor;
      else if (decl->kind == NodeKind::UnionDecl)
        dtor = static_cast<const UnionDeclNode *>(decl)->destructor;

      if (dtor && !dtor->isImplicit && !copyCtor) {
        return ctx->reportError(
            node->value->line, node->value->column, node->value->length,
            "Cannot throw: the type has a custom destructor but no copy "
            "constructor.");
      }
      if (copyCtor) {
        const_cast<ThrowStmtNode *>(node)->copyCtor = copyCtor;
      }
    }
  }

  return ctx->astCtx.VoidTy;
}

/* 'assert(expr)': the condition must be boolean-compatible; the runtime
 * failure handler reports the source location and aborts. */
SemaResult TypeCheckPass::visit(const AssertStmtNode *node) {
  if (ctx->ndebugEnabled) {
    const_cast<AssertStmtNode *>(node)->isNoOp = true;
    return ctx->astCtx.VoidTy;
  }

  auto condRes = dispatch(node->condition);
  if (!condRes)
    return std::unexpected(condRes.error());

  if (!canImplicitlyCast(*condRes, ctx->astCtx.BoolTy)) {
    return ctx->reportError(node->condition->line, node->condition->column,
                            node->condition->length,
                            "Assert condition must evaluate to a boolean "
                            "type.");
  }

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
          (void)ctx->reportError(
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
      node->op == "+" || node->op == "~" || node->op == "!" ||
      node->op == "*") {
    if (auto *opDecl = resolveOverloadedOperator(*exprType, node->op, {})) {
      if (!opDecl->isPublic(opDecl->name) &&
          ctx->getCurrentRecordContext() != opDecl->parentRecord) {
        return ctx->reportError(node->line, node->column, node->length,
                                "Cannot call private overloaded operator '" +
                                    std::string(opDecl->name) + "'.");
      }
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
    if (!node->expr->isLValue) {
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
    /* Dereference the operand type: incrementing through a reference
     * modifies the referenced value. */
    const Type *opBase = *exprType;
    if (opBase->isReferenceType()) {
      opBase = static_cast<const ReferenceType *>(opBase)->getPointeeType();
    } else if (opBase->getKind() == TypeKind::RValueReference) {
      opBase =
          static_cast<const RValueReferenceType *>(opBase)->getPointeeType();
    }

    if (!opBase->isNumeric()) {
      return ctx->reportError(node->line, node->column, node->length,
                              "Unary operator '" + std::string(node->op) +
                                  "' requires a numeric operand");
    }
    if (!node->expr->isLValue) {
      return ctx->reportError(
          node->line, node->column, node->length,
          "Expression is not assignable (must be an l-value)");
    }
    if (opBase->isConstQualified() ||
        ((*exprType)->isReferenceType() &&
         static_cast<const ReferenceType *>(*exprType)
             ->getPointeeType()
             ->isConstQualified())) {
      return ctx->reportError(node->line, node->column, node->length,
                              "Cannot modify a constant variable");
    }
    const DeclNode *targetDecl = nullptr;
    if (auto *targetVar = llvm::dyn_cast<VariableNode>(node->expr)) {
      targetDecl = targetVar->resolvedDecl;
    } else if (auto *targetMa = llvm::dyn_cast<MemberAccessNode>(node->expr)) {
      targetDecl = targetMa->resolvedDecl;
    }
    if (targetDecl && targetDecl->kind == NodeKind::VarDecl &&
        static_cast<const VarDeclNode *>(targetDecl)->isFinal) {
      return ctx->reportError(node->line, node->column, node->length,
                              "Cannot modify the final variable '" +
                                  std::string(static_cast<const VarDeclNode *>(
                                                  targetDecl)
                                                  ->varName) +
                                  "'.");
    }
    resType = opBase;
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

  const FunctionDeclNode *opDecl =
      resolveOverloadedOperator(*lhs, node->op, {node->right});
  if (opDecl) {
    if (!opDecl->isPublic(opDecl->name) &&
        ctx->getCurrentRecordContext() != opDecl->parentRecord) {
      return ctx->reportError(node->line, node->column, node->length,
                              "Cannot call private overloaded operator '" +
                                  std::string(opDecl->name) + "'.");
    }
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

  /* String literals type as 'const uint8*', so 'lit' + 'lit' does not find
   * String's overloaded operator+; convert both operands to String so the
   * concatenation resolves like 'StringVar + lit'. */
  if (node->op == "+") {
    const Type *strTy = ctx->astCtx.getRecordType("String");
    auto isStringish = [this](const Type *t) {
      const Type *u = t->getUnqualifiedType();
      if (u->isPointerType()) {
        const Type *p = static_cast<const PointerType *>(u)
                            ->getPointeeType()
                            ->getUnqualifiedType();
        return p->getKind() == TypeKind::Builtin &&
               static_cast<const BuiltinType *>(p)->getBuiltinKind() ==
                   BuiltinKind::UInt8;
      }
      if (u->getKind() == TypeKind::Array) {
        return static_cast<const ArrayType *>(u)
                   ->getElementType()
                   ->getUnqualifiedType() == ctx->astCtx.UInt8Ty;
      }
      if (auto *rec = llvm::dyn_cast<RecordType>(u))
        return rec->getName() == "String";
      return false;
    };
    if (strTy && isStringish(*lhs) && isStringish(*rhs)) {
      const_cast<BinaryOpNode *>(node)->left =
          performImplicitConversion(node->left, strTy);
      const_cast<BinaryOpNode *>(node)->right =
          performImplicitConversion(node->right, strTy);
      opDecl = resolveOverloadedOperator(strTy, node->op, {node->right});
      if (opDecl) {
        node->overloadedOperator = opDecl;
        node->exprType = opDecl->returnType;
        return opDecl->returnType;
      }
    }
  }

  OpCategory cat = OpCategory::Unknown;
  if (auto it = opCategoryMap.find(node->op); it != opCategoryMap.end()) {
    cat = it->second;
  }

  /* Pointer arithmetic: 'p + n', 'n + p', 'p - n'. The offset is element
   * scaled in codegen (byte pointers step by byte). */
  if (cat == OpCategory::Arithmetic &&
      (node->op == "+" || node->op == "-")) {
    const Type *lu = (*lhs)->getUnqualifiedType();
    const Type *ru = (*rhs)->getUnqualifiedType();
    bool lhsIsPtr = lu->isPointerType();
    bool rhsIsPtr = ru->isPointerType();
    bool lhsIsInt = lu->isInteger();
    bool rhsIsInt = ru->isInteger();

    if ((lhsIsPtr && rhsIsInt) || (lhsIsInt && rhsIsPtr)) {
      if (node->op == "-" && !lhsIsPtr) {
        return ctx->reportError(
            node->line, node->column, node->length,
            "Cannot subtract a pointer from an integer; the pointer must be "
            "on the left of '-'.");
      }
      const Type *ptrTy = lhsIsPtr ? *lhs : *rhs;
      const Type *pointee =
          static_cast<const PointerType *>(ptrTy->getUnqualifiedType())
              ->getPointeeType()
              ->getUnqualifiedType();
      if (pointee->isVoid()) {
        return ctx->reportError(
            node->line, node->column, node->length,
            "Cannot perform arithmetic on a void*; use a byte pointer (e.g. "
            "RawMemory.ptr).");
      }
      node->exprType = ptrTy;
      node->promotedType = nullptr;
      node->isLValue = false;
      return node->exprType;
    }
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
    /* Pointer comparisons ignore top-level const qualifiers (Dart-style:
     * constness is a value property, not part of the pointer type). */
    const Type *lUnqualP = (*lhs)->getUnqualifiedType();
    const Type *rUnqualP = (*rhs)->getUnqualifiedType();
    if (lUnqualP->isPointerType() && rUnqualP->isPointerType()) {
      const Type *lPointee = static_cast<const PointerType *>(lUnqualP)
                                 ->getPointeeType()
                                 ->getUnqualifiedType();
      const Type *rPointee = static_cast<const PointerType *>(rUnqualP)
                                 ->getPointeeType()
                                 ->getUnqualifiedType();
      if (lPointee->toString() == rPointee->toString()) {
        node->promotedType = ctx->astCtx.getPointerType(lPointee);
        node->exprType = ctx->astCtx.BoolTy;
        return ctx->astCtx.BoolTy;
      }
    }
    if (!canImplicitlyCast(*lhs, *rhs)) {
      return ctx->reportError(node->line, node->column, node->length,
                              "Type mismatch in relational operation.");
    }

    if (node->op != "==" && node->op != "!=") {
      if (!effectiveNumericType(*lhs)->isNumeric() ||
          !effectiveNumericType(*rhs)->isNumeric()) {
        return ctx->reportError(
            node->line, node->column, node->length,
            "Relational inequalities require numeric operands.");
      }
    } else {
      /* '=='/'!=' on records or arrays requires an overloaded operator:
       * LLVM cannot lower a struct/array comparison and emitting one would
       * crash code generation. Enums and primitives compare directly. */
      auto stripRef = [](const Type *t) {
        const Type *u = t->getUnqualifiedType();
        if (u->isReferenceType())
          return static_cast<const ReferenceType *>(u)
              ->getPointeeType()
              ->getUnqualifiedType();
        if (u->getKind() == TypeKind::RValueReference)
          return static_cast<const RValueReferenceType *>(u)
              ->getPointeeType()
              ->getUnqualifiedType();
        return u;
      };
      const Type *lUnqual = stripRef(*lhs);
      const Type *rUnqual = stripRef(*rhs);
      auto needsOverloadedEq = [](const Type *t) {
        TypeKind k = t->getKind();
        return k == TypeKind::Struct || k == TypeKind::Class ||
               k == TypeKind::Union || k == TypeKind::Array;
      };
      if ((needsOverloadedEq(lUnqual) || needsOverloadedEq(rUnqual)) &&
          !opDecl) {
        return ctx->reportError(
            node->line, node->column, node->length,
            "Type '" +
                (needsOverloadedEq(lUnqual) ? lUnqual->toString()
                                            : rUnqual->toString()) +
                "' does not define operator '" + std::string(node->op) +
                "'; declare an overloaded operator to compare it.");
      }
    }

    const Type *res = nullptr;
    if (effectiveNumericType(*lhs)->isNumeric() &&
        effectiveNumericType(*rhs)->isNumeric()) {
      res = ctx->astCtx.getPromotedNumericType(effectiveNumericType(*lhs),
                                               effectiveNumericType(*rhs));
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
    if (!effectiveNumericType(*lhs)->isInteger() ||
        !effectiveNumericType(*rhs)->isInteger()) {
      return ctx->reportError(
          node->line, node->column, node->length,
          "Bitwise and modulo operations require integer operands.");
    }
  } else if (cat == OpCategory::Arithmetic) {
    if (!effectiveNumericType(*lhs)->isNumeric() ||
        !effectiveNumericType(*rhs)->isNumeric()) {
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
  if (effectiveNumericType(*lhs)->isNumeric() &&
      effectiveNumericType(*rhs)->isNumeric()) {
    res = ctx->astCtx.getPromotedNumericType(effectiveNumericType(*lhs),
                                             effectiveNumericType(*rhs));
    checkImplicitCastWarning(*lhs, res, node->left);
    checkImplicitCastWarning(*rhs, res, node->right);
  } else {
    res = (*lhs)->isFloat() ? *lhs : *rhs;
  }

  node->promotedType = res;
  node->exprType = res;
  return res;
}

SemaResult TypeCheckPass::visit(const TernaryOpNode *node) {
  auto condRes = dispatch(node->condition);
  if (!condRes)
    return std::unexpected(ErrorInfo{node->line, node->column, node->length,
                                     "Cascading error in ternary condition"});

  if (!canImplicitlyCast(*condRes, ctx->astCtx.BoolTy)) {
    return ctx->reportError(
        node->condition->line, node->condition->column, node->condition->length,
        "Ternary condition must evaluate to a boolean type.");
  }

  auto trueRes = dispatch(node->trueExpr);
  auto falseRes = dispatch(node->falseExpr);

  if (!trueRes || !falseRes)
    return std::unexpected(ErrorInfo{node->line, node->column, node->length,
                                     "Cascading error in ternary branches"});

  const Type *tType = *trueRes;
  const Type *fType = *falseRes;
  const Type *resType = nullptr;

  if (canImplicitlyCast(fType, tType)) {
    resType = tType;
    const_cast<TernaryOpNode *>(node)->falseExpr =
        performImplicitConversion(node->falseExpr, tType);
  } else if (canImplicitlyCast(tType, fType)) {
    resType = fType;
    const_cast<TernaryOpNode *>(node)->trueExpr =
        performImplicitConversion(node->trueExpr, fType);
  } else if (tType->isNumeric() && fType->isNumeric()) {
    resType = ctx->astCtx.getPromotedNumericType(tType, fType);
    checkImplicitCastWarning(tType, resType, node->trueExpr);
    checkImplicitCastWarning(fType, resType, node->falseExpr);
    const_cast<TernaryOpNode *>(node)->trueExpr =
        performImplicitConversion(node->trueExpr, resType);
    const_cast<TernaryOpNode *>(node)->falseExpr =
        performImplicitConversion(node->falseExpr, resType);
  } else {
    return ctx->reportError(
        node->line, node->column, node->length,
        "Incompatible operand types in ternary operator: '" +
            tType->toString() + "' and '" + fType->toString() + "'.");
  }

  node->promotedType = resType;
  node->exprType = resType;

  if (node->trueExpr->isLValue && node->falseExpr->isLValue &&
      ctx->isSameType(tType, fType)) {
    node->isLValue = true;
  } else {
    node->isLValue = false;
  }

  return resType;
}

SemaResult TypeCheckPass::visit(const VarDeclNode *node) {
  const_cast<VarDeclNode *>(node)->type = resolveIfTemplate(node->type);
  const Type *declType = node->type;

  bool isAuto = declType->getUnqualifiedType()->getKind() == TypeKind::Auto;

  if (isAuto && !node->initializer && !node->isExtern) {
    return ctx->reportError(
        node->line, node->column, node->length,
        "Variables with inferred type must be initialized.");
  }

    if (node->initializer) {
      bool pushedExpected = false;
      if (!isAuto) {
        if (const Type *expFn = getExpectedFunctionPtrType(declType)) {
          ctx->pushExpectedFunctionType(expFn);
          pushedExpected = true;
        }
      }

      /* Dart rule: the initializer of a 'const' variable lives in a const
       * context (implicit const applies to nested constructor calls). */
      if (declType->isConstQualified() && !node->isExtern)
        ctx->constContextDepth++;

      auto initRes = dispatch(node->initializer);

      if (declType->isConstQualified() && !node->isExtern)
        ctx->constContextDepth--;

      if (pushedExpected)
        ctx->popExpectedFunctionType();

      if (!initRes) {
        if (ctx->getScopeDepth() > 1) {
          ctx->addDecl(node->varName, node);
        }
        return initRes;
      }

      /* Validate that the const initializer really is a constant expression
       * and cache its serialized value. */
      if (declType->isConstQualified() && !node->isExtern) {
        std::string key;
        if (!this->constEvaluate(node->initializer, true, key)) {
          return ctx->reportError(
              node->initializer->line, node->initializer->column,
              node->initializer->length,
              "The initializer of a 'const' variable must be a constant "
              "expression.");
        }
        const_cast<VarDeclNode *>(node)->isConstExpr = true;
        const_cast<VarDeclNode *>(node)->constKey = key;
        initRes = node->initializer->exprType;
      }

    if (isAuto) {
      const Type *inferred = *initRes;

      /* Array-to-pointer decay exactly like C++'s auto logic */
      if (inferred->getKind() == TypeKind::Array) {
        inferred = ctx->astCtx.getPointerType(
            static_cast<const ArrayType *>(inferred)->getElementType());
      }

      /* Strip top-level references off the initializer when inferring type */
      if (inferred->isReferenceType()) {
        inferred =
            static_cast<const ReferenceType *>(inferred)->getPointeeType();
      } else if (inferred->getKind() == TypeKind::RValueReference) {
        inferred = static_cast<const RValueReferenceType *>(inferred)
                       ->getPointeeType();
      }

      if (inferred->isConstQualified()) {
        inferred = inferred->getUnqualifiedType();
      }

      if (declType->isConstQualified()) {
        inferred = ctx->astCtx.getConstType(inferred);
      }

      if (node->initializer->kind == NodeKind::Lambda &&
          static_cast<const LambdaNode *>(node->initializer)->unresolved) {
        return ctx->reportError(
            node->line, node->column, node->length,
            "Cannot infer lambda parameter types: no expected function "
            "signature is available.");
      }

      declType = inferred;
      const_cast<VarDeclNode *>(node)->type = declType;
    }
  }

  if (declType->getKind() == TypeKind::TemplateParam) {
    auto err = ctx->reportError(node->line, node->column, node->length,
                                "Unknown type: '" + declType->toString() + "'");
    if (ctx->getScopeDepth() > 1) {
      ctx->addDecl(node->varName, node);
    }
    return err;
  }

  if (!checkTypeVisibility(declType, node)) {
    auto err = std::unexpected(ErrorInfo{node->line, node->column, node->length,
                                         "Type visibility error"});
    if (ctx->getScopeDepth() > 1) {
      ctx->addDecl(node->varName, node);
    }
    return err;
  }

  if (declType->isVoid()) {
    auto err = ctx->reportError(node->line, node->column, node->length,
                                "Variables cannot be of type 'void'");
    if (ctx->getScopeDepth() > 1) {
      ctx->addDecl(node->varName, node);
    }
    return err;
  }

  const Type *checkTy = declType;
  while (checkTy->getKind() == TypeKind::Array) {
    checkTy = static_cast<const ArrayType *>(checkTy)->getElementType();
  }

  /* Record arrays declared without an initializer have their elements
   * default-constructed at runtime, so a parameterless constructor must
   * exist. */
  if (!node->initializer && declType->getKind() == TypeKind::Array) {
    const Type *elemUnqual = checkTy->getUnqualifiedType();
    if (elemUnqual->getKind() == TypeKind::Class ||
        elemUnqual->getKind() == TypeKind::Struct ||
        elemUnqual->getKind() == TypeKind::Union) {
      auto *recTy = static_cast<const RecordType *>(elemUnqual);
      auto *decl = recTy->getDeclaration();
      if (decl) {
        llvm::ArrayRef<FunctionDeclNode *> ctors;
        if (decl->kind == NodeKind::ClassDecl)
          ctors = static_cast<const ClassDeclNode *>(decl)->constructors;
        else if (decl->kind == NodeKind::StructDecl)
          ctors = static_cast<const StructDeclNode *>(decl)->constructors;
        else if (decl->kind == NodeKind::UnionDecl)
          ctors = static_cast<const UnionDeclNode *>(decl)->constructors;

        bool hasDefaultCtor = false;
        for (const auto *ctor : ctors) {
          if (ctor->params.empty()) {
            hasDefaultCtor = true;
            break;
          }
        }
        if (!hasDefaultCtor) {
          auto err = ctx->reportError(
              node->line, node->column, node->length,
              "Array declaration of '" + std::string(recTy->getName()) +
                  "' requires a default constructor.");
          if (ctx->getScopeDepth() > 1) {
            ctx->addDecl(node->varName, node);
          }
          return err;
        }
      }
    }
  }

  if (!checkTy->isPointerType() && !checkTy->isReferenceType() &&
      checkTy->getKind() != TypeKind::RValueReference) {
    if (checkTy->getUnqualifiedType()->getKind() == TypeKind::Class) {
      auto *classTy =
          static_cast<const ClassType *>(checkTy->getUnqualifiedType());
      if (classTy->getIsAbstract()) {
        auto err = ctx->reportError(node->line, node->column, node->length,
                                    "Cannot instantiate abstract class '" +
                                        std::string(classTy->getName()) + "'.");
        if (ctx->getScopeDepth() > 1) {
          ctx->addDecl(node->varName, node);
        }
        return err;
      }
    }
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

    if (!node->isStatic && !node->isExtern && !node->isGlobal) {
      bool isField = ctx->getScopeDepth() == 1;
      if (isField && ctx->getCurrentRecordContext() == recTy) {
        auto err = ctx->reportError(
            node->line, node->column, node->length,
            "Field '" + std::string(node->varName) + "' has incomplete type '" +
                std::string(recTy->getName()) +
                "' (recursive value type). Use a pointer "
                "or reference instead.");
        if (ctx->getScopeDepth() > 1) {
          ctx->addDecl(node->varName, node);
        }
        return err;
      }
    }

    if (recTy->isOpaque()) {
      auto err = ctx->reportError(
          node->line, node->column, node->length,
          "Cannot declare variable of incomplete (opaque) type.");
      if (ctx->getScopeDepth() > 1) {
        ctx->addDecl(node->varName, node);
      }
      return err;
    }
  }

  for (const auto *ann : node->annotations) {
    if (ann->name == "weak") {
      const_cast<VarDeclNode *>(node)->isWeak = true;
    } else if (ann->name == "alignas") {
      (void)ctx->reportError(ann->line, ann->column, ann->length,
                             "The @alignas annotation has been removed; use "
                             "@align(N) instead.");
    } else if (ann->name == "align") {
      if (ann->args.size() != 1 || !llvm::isa<NumberNode>(ann->args[0]) ||
          llvm::cast<NumberNode>(ann->args[0])->isFloat) {
        (void)ctx->reportError(
            ann->line, ann->column, ann->length,
            "The @align annotation requires a single integer constant.");
      } else {
        uint64_t alignVal = 0;
        if (!parseAlignValue(ctx, llvm::cast<NumberNode>(ann->args[0]), ann,
                             alignVal)) {
          /* error already reported */
        } else if (alignVal == 0 || (alignVal & (alignVal - 1)) != 0) {
          (void)ctx->reportError(ann->line, ann->column, ann->length,
                                 "Alignment must be a power of 2.");
        } else {
          const_cast<VarDeclNode *>(node)->alignment = alignVal;
        }
      }
    } else if (ann->name == "packed") {
      if (!ann->args.empty()) {
        (void)ctx->reportError(
            ann->line, ann->column, ann->length,
            "The @packed annotation does not take arguments.");
      } else {
        const_cast<VarDeclNode *>(node)->isPacked = true;
      }
    }
  }

  /* Mark variables defined at module scope to enable accurate side-effect
   * analysis */
  if (ctx->getScopeDepth() == 1 && ctx->getCurrentRecordContext() == nullptr) {
    const_cast<VarDeclNode *>(node)->isGlobal = true;
  }

  if (declType->isConstQualified() && !node->initializer &&
      !declType->isReferenceType() &&
      declType->getKind() != TypeKind::RValueReference && !node->isExtern) {
    (void)ctx->reportError(node->line, node->column, node->length,
                           "Constant variables must be initialized.");
  }

  /* 'final' variables (locals, globals and static fields) require an
   * initializer at the declaration; only instance fields may defer their
   * initialization to the constructors. A variable is a field when the
   * current record's declaration owns it; scope depth alone is unreliable
   * because template instantiation type-checks clones inside an extra
   * scope. */
  if (node->isFinal && !node->initializer && !node->isExtern) {
    bool isFieldDecl = false;
    if (const RecordType *recCtx = ctx->getCurrentRecordContext()) {
      if (const DeclNode *recDecl = recCtx->getDeclaration()) {
        llvm::ArrayRef<VarDeclNode *> fields;
        if (auto *cDecl = llvm::dyn_cast<ClassDeclNode>(recDecl))
          fields = cDecl->fields;
        else if (auto *sDecl = llvm::dyn_cast<StructDeclNode>(recDecl))
          fields = sDecl->fields;
        else if (auto *uDecl = llvm::dyn_cast<UnionDeclNode>(recDecl))
          fields = uDecl->fields;
        for (const auto *f : fields) {
          if (f == node) {
            isFieldDecl = true;
            break;
          }
        }
      }
    }
    if (!isFieldDecl || node->isStatic) {
      (void)ctx->reportError(node->line, node->column, node->length,
                             "The final variable '" +
                                 std::string(node->varName) +
                                 "' must be initialized at its declaration.");
    }
  }

  if (node->isExtern && node->initializer) {
    (void)ctx->reportError(node->line, node->column, node->length,
                           "Extern variables cannot have an initializer.");
  }

  if (declType->isReferenceType() ||
      declType->getKind() == TypeKind::RValueReference) {
    if (!node->initializer && !node->isExtern) {
      (void)ctx->reportError(
          node->line, node->column, node->length,
          "References must be initialized upon declaration.");
    }
  }

  if (node->initializer) {
    const Type *initTy = node->initializer->exprType;

    if (declType->isReferenceType()) {
      if (!node->initializer->isLValue) {
        const Type *pointee =
            static_cast<const ReferenceType *>(declType)->getPointeeType();
        if (!pointee->isConstQualified()) {
          ctx->reportWarning(
              WarningKind::ImplicitCast, node->initializer->line,
              node->initializer->column, node->initializer->length,
              "Binding an r-value to a non-const reference will implicitly "
              "create a stack-allocated temporary.",
              node->initializer->endLine);
        }
      }
    } else if (declType->getKind() == TypeKind::RValueReference) {
      if (node->initializer->isLValue) {
        (void)ctx->reportError(
            node->line, node->column, node->length,
            "Cannot bind an l-value to an r-value reference.");
      }
    } else {
      bool awaitKeepsFuture = false;
      if (node->initializer->kind == NodeKind::Await) {
        auto *awaitNode =
            static_cast<const AwaitExprNode *>(node->initializer);
        /* 'Future<T> a = await fut;': the declared type is the future
         * itself, so the await passes the future through (Dart-style
         * declared Future<T> annotations remain valid). */
        const Type *declInner = nullptr;
        const Type *operandInner = nullptr;
        if (unwrapFutureType(declType, &declInner) &&
            unwrapFutureType(awaitNode->expr->exprType, &operandInner) &&
            declInner == operandInner) {
          awaitKeepsFuture = true;
          const_cast<AwaitExprNode *>(awaitNode)->keepFuture = true;
          initTy = declType;
          const_cast<AwaitExprNode *>(awaitNode)->exprType = declType;
        }
      }
      if (!canImplicitlyCast(initTy, declType)) {
        std::string initTypeStr = initTy ? initTy->toString() : "unknown";
        (void)ctx->reportError(node->line, node->column, node->length,
                               "Cannot initialize variable of type '" +
                                   declType->toString() + "' with type '" +
                                   initTypeStr + "'");
      } else {
        checkImplicitCastWarning(initTy, declType, node->initializer);
        const_cast<VarDeclNode *>(node)->initializer =
            performImplicitConversion(node->initializer, declType);

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
                          currentScore = 1;
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
                /* Disallow bypassing privacy logic when invoking a copy
                 * constructor */
                if (!copyCtor->isPublic(copyCtor->name) &&
                    ctx->getCurrentRecordContext() != recTy) {
                  (void)ctx->reportError(node->line, node->column, node->length,
                                         "Cannot implicitly copy variable. "
                                         "Copy constructor is private.");
                }
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
                  (void)ctx->reportError(
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

  /* An assignment invalidates any active 'is' promotion of the target
   * variable: 'if (x is T) { x = other; ... }' must not keep treating x as
   * T. The target is re-typed with its declared type afterwards so the
   * assignment itself type-checks against the declared variable type. */
  if (node->op == "=") {
    if (auto *targetVar = llvm::dyn_cast<VariableNode>(node->target)) {
      if (targetVar->resolvedDecl) {
        bool invalidated = false;
        for (auto &p : promotions) {
          if (p.active && p.decl == targetVar->resolvedDecl) {
            p.active = false;
            invalidated = true;
          }
        }
        if (invalidated) {
          lhsType = dispatch(node->target);
        }
      }
    }
  }

  /* Propagate the failure instead of dereferencing an empty expected, which
   * previously crashed the compiler on a failed LHS expression. */
  if (!lhsType)
    return lhsType;

  bool pushedExpected = false;
  if (const Type *expFn = getExpectedFunctionPtrType(*lhsType)) {
    ctx->pushExpectedFunctionType(expFn);
    pushedExpected = true;
  }

  auto rhsType = dispatch(node->value);

  if (pushedExpected)
    ctx->popExpectedFunctionType();

  if (!lhsType || !rhsType)
    return std::unexpected(ErrorInfo{node->line, node->column, node->length,
                                     "Cascading error in assignment"});

  /* Raw pointers are assigned with built-in pointer semantics; do not let
   * operator= overloads of the pointee hijack the assignment (e.g.
   * 'other._ptr = null' where _ptr is 'shared_ptr<T>*'). References and
   * r-value references alias the record itself, so their operator= applies. */
  if (!(*lhsType)->getUnqualifiedType()->isPointerType()) {
    if (auto *opDecl =
            resolveOverloadedOperator(*lhsType, node->op, {node->value})) {
      if (!opDecl->isPublic(opDecl->name) &&
          ctx->getCurrentRecordContext() != opDecl->parentRecord) {
        return ctx->reportError(node->line, node->column, node->length,
                                "Cannot call private overloaded operator '" +
                                    std::string(opDecl->name) + "'.");
      }
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
  }

  if (!node->target->isLValue) {
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

  /* Dart 'final' semantics: a final variable or field is written exactly
   * once. Only constructor initializer-list entries (': this.x = expr') may
   * write to a final field; everything else is an error. */
  if (!node->isFieldInit) {
    const DeclNode *targetDecl = nullptr;
    if (auto *targetVar = llvm::dyn_cast<VariableNode>(node->target)) {
      targetDecl = targetVar->resolvedDecl;
    } else if (auto *targetMa =
                   llvm::dyn_cast<MemberAccessNode>(node->target)) {
      targetDecl = targetMa->resolvedDecl;
    }
    if (targetDecl && targetDecl->kind == NodeKind::VarDecl &&
        static_cast<const VarDeclNode *>(targetDecl)->isFinal) {
      return ctx->reportError(
          node->target->line, node->target->column, node->target->length,
          "Cannot assign to the final variable '" +
              std::string(static_cast<const VarDeclNode *>(targetDecl)
                              ->varName) +
              "'.");
    }
  }

  if (node->op != "=") {
    std::string_view binOp = node->op.substr(0, node->op.length() - 1);

    AssignCategory cat = AssignCategory::Unknown;
    if (auto it = assignCatMap.find(binOp); it != assignCatMap.end()) {
      cat = it->second;
    }

    if (cat == AssignCategory::BitwiseOrModulo) {
      if (!effectiveNumericType(*lhsType)->isInteger() ||
          !effectiveNumericType(*rhsType)->isInteger()) {
        return ctx->reportError(
            node->line, node->column, node->length,
            "Bitwise and modulo assignments require integer operands.");
      }
    } else if (cat == AssignCategory::Arithmetic) {
      if (!effectiveNumericType(*lhsType)->isNumeric() ||
          !effectiveNumericType(*rhsType)->isNumeric()) {
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
  if (node->isSuperAccess ||
      (node->object->kind == NodeKind::Variable &&
       static_cast<const VariableNode *>(node->object)->name == "super")) {
    const RecordType *currRec = ctx->getCurrentRecordContext();
    if (!currRec || currRec->getKind() != TypeKind::Class) {
      return ctx->reportError(node->line, node->column, node->length,
                              "Use of 'super' outside of a class method.");
    }
    auto *classTy = static_cast<const ClassType *>(currRec);
    if (!classTy->getBaseClass()) {
      return ctx->reportError(node->line, node->column, node->length,
                              "Class '" + std::string(classTy->getName()) +
                                  "' has no base class.");
    }

    const Type *baseTy = classTy->getBaseClass()->getUnqualifiedType();
    if (!llvm::isa<ClassType>(baseTy)) {
      return ctx->reportError(node->line, node->column, node->length,
                              "Base type is not a class.");
    }
    auto *baseClassTy = static_cast<const ClassType *>(baseTy);

    if (auto field = baseClassTy->getField(node->memberName)) {
      if (!field->isPublic && !field->isProtected) {
        return ctx->reportError(node->line, node->column, node->length,
                                "Cannot access private field '" +
                                    std::string(node->memberName) +
                                    "' of base class.");
      }
      const_cast<MemberAccessNode *>(node)->isSuperAccess = true;
      const_cast<MemberAccessNode *>(node)->fieldIndex = field->index;
      node->exprType = field->type;
      node->isLValue = true;
      return field->type;
    }

    const DeclNode *baseDecl = baseClassTy->getDeclaration();
    if (baseDecl && llvm::isa<ClassDeclNode>(baseDecl)) {
      auto *cDecl = static_cast<const ClassDeclNode *>(baseDecl);
      for (auto *m : cDecl->methods) {
        if (m->name == node->memberName) {
          if (!m->isPublic(m->name) && !m->isProtected(m->name)) {
            return ctx->reportError(node->line, node->column, node->length,
                                    "Cannot access private method '" +
                                        std::string(node->memberName) +
                                        "' of base class.");
          }
          const_cast<MemberAccessNode *>(node)->isSuperAccess = true;
          const_cast<MemberAccessNode *>(node)->isMethodRef = true;
          const_cast<MemberAccessNode *>(node)->resolvedMethod = m;
          node->exprType = ctx->astCtx.VoidTy;
          return ctx->astCtx.VoidTy;
        }
      }
    }

    return ctx->reportError(node->line, node->column, node->length,
                            "No member named '" +
                                std::string(node->memberName) +
                                "' in base class.");
  }

  auto objType = dispatch(node->object);
  if (!objType)
    return std::unexpected(ErrorInfo{node->line, node->column, node->length,
                                     "Invalid object in member access"});

  const DeclNode *staticAccessDecl = nullptr;
  const RecordType *recordTy = nullptr;

  if (objType && *objType == ctx->astCtx.NamespaceTy) {
    const DeclNode *nsDecl = nullptr;
    if (auto *varNode = llvm::dyn_cast<VariableNode>(node->object)) {
      nsDecl = varNode->resolvedDecl;
    } else if (auto *maNode = llvm::dyn_cast<MemberAccessNode>(node->object)) {
      nsDecl = maNode->resolvedDecl;
    }

    if (nsDecl && llvm::isa<NamespaceDeclNode>(nsDecl)) {
      auto *nsNode = static_cast<const NamespaceDeclNode *>(nsDecl);
      std::string targetFQName =
          std::string(nsNode->fqName) + "." + std::string(node->memberName);

      auto decls = ctx->symTable.lookupExact(targetFQName, ctx->currentModule);
      if (decls.empty()) {
        /* Template functions are kept in the template registry instead of the
         * symbol table, so a qualified call like 'Memory.make_unique<int>(...)'
         * needs an explicit lookup plus instantiation here to resolve like the
         * unqualified form. */
        const DeclNode *tmplDecl = nullptr;
        auto regIt = ctx->templateRegistry.find(targetFQName);
        if (regIt != ctx->templateRegistry.end() &&
            llvm::isa<FunctionDeclNode>(regIt->second) &&
            (!ctx->currentModule ||
             ctx->currentModule->canSee(regIt->second->declFilePath))) {
          tmplDecl = regIt->second;
        }

        if (tmplDecl && !node->templateArgs.empty()) {
          const auto *tfn = static_cast<const FunctionDeclNode *>(tmplDecl);
          /* Partial explicit type arguments are completed by call-site
           * deduction before reaching this point. */
          if (node->templateArgs.size() != tfn->templateParams.size()) {
            return ctx->reportError(
                node->line, node->column, node->length,
                "Wrong number of template arguments for '" +
                    std::string(node->memberName) + "': expected " +
                    std::to_string(tfn->templateParams.size()) + ", got " +
                    std::to_string(node->templateArgs.size()) + ".");
          }
          std::string mangledName = std::string(tmplDecl->fqName);
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

          auto instDecls =
              ctx->symTable.lookupExact(mangledView, ctx->currentModule);
          if (instDecls.empty()) {
            std::unordered_map<std::string_view, const Type *> templateArgMap;
            for (size_t i = 0; i < tfn->templateParams.size(); ++i) {
              templateArgMap[tfn->templateParams[i]] =
                  resolveIfTemplate(node->templateArgs[i]);
            }

            ASTCloner cloner(ctx->astCtx, templateArgMap);
            DeclNode *instDecl =
                llvm::cast<DeclNode>(cloner.dispatch(tmplDecl));

            if (instDecl) {
              llvm::cast<FunctionDeclNode>(instDecl)->name = mangledView;
              instDecl->fqName = mangledView;
              instDecl->hasPublicMod = tmplDecl->hasPublicMod;
              instDecl->hasPrivateMod = tmplDecl->hasPrivateMod;
              instDecl->hasProtectedMod = tmplDecl->hasProtectedMod;
              instDecl->annotations = tmplDecl->annotations;
              instDecl->declFilePath = tmplDecl->declFilePath;

              auto *instFn = llvm::cast<FunctionDeclNode>(instDecl);
              instFn->isTemplate = false;
              instFn->templateParams = {};

              if (ctx->currentModule) {
                const_cast<ModuleNode *>(ctx->currentModule)
                    ->instantiatedTemplates.push_back(instDecl);
              }
              DeclCollectorPass dcp;
              dcp.ctx = ctx;

              auto prevFile = ctx->currentFile;
              ctx->setCurrentFile(instDecl->declFilePath);

              ctx->pushScope();
              dcp.dispatch(instDecl);
              dispatch(instDecl);
              ctx->popScope();

              ctx->symTable.addGlobalSymbol(instDecl->fqName, instDecl);
              ctx->setCurrentFile(prevFile);
            }
          }
          decls = ctx->symTable.lookupExact(mangledView, ctx->currentModule);
        } else if (tmplDecl && llvm::isa<FunctionDeclNode>(tmplDecl)) {
          /* A template function called without explicit type arguments
           * ('Memory.construct(ptr, ...)') resolves to the template
           * declaration itself; call-site handling may deduce the type
           * arguments from the arguments (see checkMemoryConstruct). */
          const_cast<MemberAccessNode *>(node)->resolvedDecl = tmplDecl;
          auto *tfn = static_cast<const FunctionDeclNode *>(tmplDecl);
          std::vector<const Type *> pTypes;
          for (auto *p : tfn->params)
            pTypes.push_back(p->type);
          const Type *funcTy = ctx->astCtx.getFunctionType(
              tfn->effectiveReturnType ? tfn->effectiveReturnType
                                       : tfn->returnType,
              ctx->astCtx.copyArray<const Type *>(pTypes));
          node->exprType = ctx->astCtx.getPointerType(funcTy);
          node->isLValue = true;
          return node->exprType;
        }
      }
      if (decls.empty()) {
        return ctx->reportError(
            node->line, node->column, node->length,
            "No member named '" + std::string(node->memberName) +
                "' in namespace '" + std::string(nsNode->fqName) + "'");
      }
      const DeclNode *target = decls.front();

      if (llvm::isa<NamespaceDeclNode>(target)) {
        const_cast<MemberAccessNode *>(node)->resolvedDecl = target;
        node->exprType = ctx->astCtx.NamespaceTy;
        node->isLValue = false;
        return node->exprType;
      }

      if (llvm::isa<StructDeclNode>(target) ||
          llvm::isa<ClassDeclNode>(target) ||
          llvm::isa<UnionDeclNode>(target) || llvm::isa<EnumDeclNode>(target) ||
          llvm::isa<TypedefDeclNode>(target)) {
        const Type *repTy = nullptr;
        if (auto *structDecl = llvm::dyn_cast<StructDeclNode>(target))
          repTy = structDecl->recordType;
        else if (auto *classDecl = llvm::dyn_cast<ClassDeclNode>(target))
          repTy = classDecl->recordType;
        else if (auto *unionDecl = llvm::dyn_cast<UnionDeclNode>(target))
          repTy = unionDecl->recordType;
        else if (auto *enumDecl = llvm::dyn_cast<EnumDeclNode>(target))
          repTy = enumDecl->enumType;
        else if (auto *typedefDecl = llvm::dyn_cast<TypedefDeclNode>(target))
          repTy = typedefDecl->aliasType;

        const_cast<MemberAccessNode *>(node)->resolvedDecl = target;
        const_cast<MemberAccessNode *>(node)->representedType = repTy;
        node->exprType = ctx->astCtx.TypeValTy;
        node->isLValue = false;
        checkDeprecated(target, node);
        return node->exprType;
      }

      if (auto *varTarget = llvm::dyn_cast<VarDeclNode>(target)) {
        if (!target->isPublic(varTarget->varName) &&
            target->declFilePath != ctx->currentFile) {
          return ctx->reportError(node->line, node->column, node->length,
                                  "Cannot access private variable.");
        }
        const_cast<MemberAccessNode *>(node)->resolvedDecl = target;
        const_cast<MemberAccessNode *>(node)->isStaticFieldRef = true;
        node->exprType = varTarget->type;
        node->isLValue = true;
        checkDeprecated(target, node);
        return node->exprType;
      }

      if (auto *fDecl = llvm::dyn_cast<FunctionDeclNode>(target)) {
        if (!target->isPublic(fDecl->name) &&
            target->declFilePath != ctx->currentFile) {
          return ctx->reportError(node->line, node->column, node->length,
                                  "Cannot access private function.");
        }
        const_cast<MemberAccessNode *>(node)->resolvedDecl = target;

        std::vector<const Type *> pTypes;
        for (auto *p : fDecl->params)
          pTypes.push_back(p->type);
        const Type *funcTy = ctx->astCtx.getFunctionType(
            fDecl->effectiveReturnType ? fDecl->effectiveReturnType
                                       : fDecl->returnType,
            ctx->astCtx.copyArray<const Type *>(pTypes));
        node->exprType = ctx->astCtx.getPointerType(funcTy);
        node->isLValue = true;
        checkDeprecated(target, node);
        return node->exprType;
      }
    }
  }

  const DeclNode *objDecl = nullptr;
  if (auto *varNode = llvm::dyn_cast<VariableNode>(node->object)) {
    objDecl = varNode->resolvedDecl;
  } else if (auto *maObj = llvm::dyn_cast<MemberAccessNode>(node->object)) {
    objDecl = maObj->resolvedDecl;
  }

  if (objDecl) {
    if (auto *enumDecl = llvm::dyn_cast<EnumDeclNode>(objDecl)) {
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
    } else if (llvm::isa<ClassDeclNode>(objDecl) ||
               llvm::isa<StructDeclNode>(objDecl) ||
               llvm::isa<UnionDeclNode>(objDecl)) {
      staticAccessDecl = objDecl;

      /* Template class used as a static-access object: 'Future<int>.value'
       * instantiates the record so the static member resolves on the
       * instantiated class. The class-level arguments may appear either on
       * the object ('Future<int>.value(...)') or on the member access itself
       * ('Future.value<int>(...)'); the latter form additionally carries the
       * method's own template arguments, so it only binds the class when the
       * record template takes exactly that many parameters. */
      if (auto *varNode = llvm::dyn_cast<VariableNode>(node->object)) {
        llvm::ArrayRef<const Type *> classArgs = varNode->templateArgs;
        if (classArgs.empty() && !node->templateArgs.empty() &&
            objDecl->isTemplate) {
          size_t templateParamCount = 0;
          if (auto *oc = llvm::dyn_cast<ClassDeclNode>(objDecl))
            templateParamCount = oc->templateParams.size();
          else if (auto *os = llvm::dyn_cast<StructDeclNode>(objDecl))
            templateParamCount = os->templateParams.size();
          else if (auto *ou = llvm::dyn_cast<UnionDeclNode>(objDecl))
            templateParamCount = ou->templateParams.size();
          if (templateParamCount == node->templateArgs.size()) {
            classArgs = node->templateArgs;
          }
        }
        if (!classArgs.empty() && objDecl->isTemplate) {
          std::vector<const Type *> resolvedArgs;
          for (const auto *arg : classArgs) {
            resolvedArgs.push_back(resolveIfTemplate(arg));
          }
          auto argsCopy = ctx->astCtx.copyArray<const Type *>(resolvedArgs);
          std::string_view objBaseName = objDecl->fqName;
          if (objBaseName.empty()) {
            if (auto *oc = llvm::dyn_cast<ClassDeclNode>(objDecl))
              objBaseName = oc->name;
            else if (auto *os = llvm::dyn_cast<StructDeclNode>(objDecl))
              objBaseName = os->name;
            else if (auto *ou = llvm::dyn_cast<UnionDeclNode>(objDecl))
              objBaseName = ou->name;
          }
          auto *instTy = ctx->astCtx.getTemplateInstType(objBaseName,
                                                         argsCopy);
          const Type *resolved = resolveIfTemplate(instTy);
          if (resolved) {
            const Type *unqual = resolved->getUnqualifiedType();
            if (unqual->getKind() == TypeKind::Class ||
                unqual->getKind() == TypeKind::Struct ||
                unqual->getKind() == TypeKind::Union) {
              recordTy = static_cast<const RecordType *>(unqual);
              staticAccessDecl = recordTy->getDeclaration();
              const_cast<VariableNode *>(varNode)->resolvedDecl =
                  staticAccessDecl;
            }
          }
        }
      }

      if (!recordTy) {
        if (auto *cDecl = llvm::dyn_cast<ClassDeclNode>(staticAccessDecl))
          recordTy = cDecl->recordType;
        else if (auto *sDecl = llvm::dyn_cast<StructDeclNode>(staticAccessDecl))
          recordTy = sDecl->recordType;
        else if (auto *uDecl = llvm::dyn_cast<UnionDeclNode>(staticAccessDecl))
          recordTy = uDecl->recordType;
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

    /* A template parameter is only usable for member access when it is
     * class-constrained ('T extends Animal'): the member resolves against
     * the constraint bound, so the instantiated call dispatches on the
     * bound's method/field (virtual calls still hit the vtable). */
    if (unqualBaseTy->getKind() == TypeKind::TemplateParam) {
      auto *paramTy = static_cast<const TemplateParamType *>(unqualBaseTy);
      const ClassType *constraintClass =
          findClassTemplateConstraint(paramTy->getName());
      if (!constraintClass) {
        return ctx->reportError(
            node->line, node->column, node->length,
            "Member access on template parameter '" +
                std::string(paramTy->getName()) +
                "': add a 'T extends Class' constraint to access members "
                "of a template parameter.");
      }
      unqualBaseTy = constraintClass;
    }

    if (!llvm::isa<RecordType>(unqualBaseTy)) {
      return ctx->reportError(node->line, node->column, node->length,
                              "Member access on non-record type");
    }

    recordTy = llvm::cast<RecordType>(unqualBaseTy);

    if (recordTy->isOpaque()) {
      return ctx->reportError(
          node->line, node->column, node->length,
          "Cannot access member of incomplete (opaque) type");
    }

    /* Field lookup walks up the inheritance chain so inherited fields are
     * accessible through the instance ('obj.field'), not only via 'super'. */
    const RecordType *fieldRecTy = recordTy;
    const FieldInfo *field = nullptr;
    while (fieldRecTy) {
      field = fieldRecTy->getField(node->memberName);
      if (field)
        break;
      if (fieldRecTy->getKind() == TypeKind::Class) {
        const auto *classTy = static_cast<const ClassType *>(fieldRecTy);
        if (!classTy->getBaseClass())
          break;
        const Type *baseUnqual =
            classTy->getBaseClass()->getUnqualifiedType();
        fieldRecTy =
            llvm::dyn_cast_or_null<RecordType>(baseUnqual);
        if (fieldRecTy && fieldRecTy->getKind() != TypeKind::Class)
          break;
      } else {
        break;
      }
    }

    if (field) {
      const RecordType *declaringRecTy = fieldRecTy;
      bool isPub = field->isPublic;
      bool isProt = field->isProtected;
      bool canAccess = false;
      auto *currCtx = ctx->getCurrentRecordContext();

      if (isPub) {
        canAccess = true;
      } else if (isProt) {
        if (currCtx == declaringRecTy)
          canAccess = true;
        else if (currCtx) {
          const ClassType *currClass = llvm::dyn_cast<ClassType>(currCtx);
          while (currClass) {
            if (currClass == declaringRecTy) {
              canAccess = true;
              break;
            }
            if (currClass->getBaseClass()) {
              if (auto *pType = llvm::dyn_cast<ClassType>(
                      currClass->getBaseClass()->getUnqualifiedType())) {
                currClass = pType;
              } else {
                break;
              }
            } else
              break;
          }
        }
      } else {
        /* Private members are file-scoped (enforced across module
         * boundaries, matching the VariableNode access paths): any code
         * declared in the same file as the owning record may access
         * them, so sibling classes (e.g. shared_ptr/weak_ptr) can
         * cooperate within one module. */
        const DeclNode *ownerDecl =
            declaringRecTy ? declaringRecTy->getDeclaration() : nullptr;
        if (currCtx == declaringRecTy ||
            (ownerDecl && ownerDecl->declFilePath == ctx->currentFile)) {
          canAccess = true;
        }
      }

      if (!canAccess) {
        return ctx->reportError(node->line, node->column, node->length,
                                "Cannot access private/protected field '" +
                                    std::string(node->memberName) + "'");
      }

      const_cast<MemberAccessNode *>(node)->fieldIndex = field->index;
      node->exprType = field->type;
      node->isLValue = true;

      /* The declaring record may differ from the object's type when the
       * field is inherited; attach the field declaration so later checks
       * (final writes) can inspect the declaration. */
      const DeclNode *declaringDecl =
          fieldRecTy ? fieldRecTy->getDeclaration() : nullptr;
      if (declaringDecl) {
        llvm::ArrayRef<VarDeclNode *> fields;
        if (auto *cDecl = llvm::dyn_cast<ClassDeclNode>(declaringDecl))
          fields = cDecl->fields;
        else if (auto *sDecl = llvm::dyn_cast<StructDeclNode>(declaringDecl))
          fields = sDecl->fields;
        else if (auto *uDecl = llvm::dyn_cast<UnionDeclNode>(declaringDecl))
          fields = uDecl->fields;

        for (const auto *fDecl : fields) {
          if (fDecl->varName == node->memberName) {
            const_cast<MemberAccessNode *>(node)->resolvedDecl = fDecl;
            break;
          }
        }
      }

      if (const DeclNode *recDecl = recordTy->getDeclaration()) {
        llvm::ArrayRef<VarDeclNode *> fields;
        if (auto *cDecl = llvm::dyn_cast<ClassDeclNode>(recDecl))
          fields = cDecl->fields;
        else if (auto *sDecl = llvm::dyn_cast<StructDeclNode>(recDecl))
          fields = sDecl->fields;
        else if (auto *uDecl = llvm::dyn_cast<UnionDeclNode>(recDecl))
          fields = uDecl->fields;

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
      if (llvm::isa<ClassDeclNode>(d) || llvm::isa<StructDeclNode>(d) ||
          llvm::isa<UnionDeclNode>(d)) {
        recDecl = d;
        break;
      }
    }

    /* Fall back to the declaration attached to the record type itself. This
     * covers template instantiations that were registered during instantiation
     * but whose mangled class name may not be visible in the current lexical
     * scope. */
    if (!recDecl) {
      recDecl = recordTy->getDeclaration();
    }

    if (recDecl) {
      if (staticAccessDecl) {
        /* Dart-style named constructor reference: 'Foo.named(...)' must
         * win over the field/method walk (the name may collide with an
         * instance field). */
        if (staticAccessDecl->kind == NodeKind::ClassDecl) {
          for (const auto *ctor :
               static_cast<const ClassDeclNode *>(staticAccessDecl)
                   ->constructors) {
            if (ctor->isNamedCtor && ctor->name == node->memberName) {
              const_cast<MemberAccessNode *>(node)->isMethodRef = true;
              const_cast<MemberAccessNode *>(node)->resolvedMethod = ctor;
              node->exprType = ctx->astCtx.VoidTy;
              return ctx->astCtx.VoidTy;
            }
          }
        }
        llvm::ArrayRef<VarDeclNode *> staticFields;
        if (auto *cDecl = llvm::dyn_cast<ClassDeclNode>(staticAccessDecl))
          staticFields = cDecl->fields;
        else if (auto *sDecl = llvm::dyn_cast<StructDeclNode>(staticAccessDecl))
          staticFields = sDecl->fields;
        else if (auto *uDecl = llvm::dyn_cast<UnionDeclNode>(staticAccessDecl))
          staticFields = uDecl->fields;

        for (const auto *f : staticFields) {
          if (f->varName == node->memberName) {
            if (!f->isStatic) {
              return ctx->reportError(node->line, node->column, node->length,
                                      "Cannot access non-static field '" +
                                          std::string(f->varName) +
                                          "' without an instance.");
            }

            bool isPub = f->isPublic(f->varName);
            bool isProt = f->isProtected(f->varName);
            bool canAccess = false;
            auto *currCtx = ctx->getCurrentRecordContext();

            if (isPub) {
              canAccess = true;
            } else if (isProt) {
              if (currCtx == recordTy)
                canAccess = true;
              else if (currCtx) {
                const ClassType *currClass = llvm::dyn_cast<ClassType>(currCtx);
                while (currClass) {
                  if (currClass == recordTy) {
                    canAccess = true;
                    break;
                  }
                  if (currClass->getBaseClass()) {
                    if (auto *pType = llvm::dyn_cast<ClassType>(
                            currClass->getBaseClass()->getUnqualifiedType())) {
                      currClass = pType;
                    } else {
                      break;
                    }
                  } else
                    break;
                }
              }
            } else {
              if ((currCtx && currCtx->getDeclaration() == staticAccessDecl) ||
                  f->declFilePath == ctx->currentFile) {
                canAccess = true;
              }
            }

            if (!canAccess) {
              return ctx->reportError(
                  node->line, node->column, node->length,
                  "Cannot access private/protected static field '" +
                      std::string(f->varName) + "'.");
            }
            const_cast<MemberAccessNode *>(node)->isStaticFieldRef = true;
            const_cast<MemberAccessNode *>(node)->resolvedDecl = f;
            node->exprType = f->type;
            node->isLValue = true;
            checkDeprecated(f, node);
            return f->type;
          }
        }
      }

      const FunctionDeclNode *bestMatch = nullptr;
      int bestScore = std::numeric_limits<int>::min();
      std::vector<std::vector<std::string>> overloadErrors;
      std::vector<ExprNode *> bestResolvedArgs;

      const DeclNode *currentRecDecl = recDecl;
      while (currentRecDecl) {
        llvm::ArrayRef<FunctionDeclNode *> methods;
        if (auto *cDecl = llvm::dyn_cast<ClassDeclNode>(currentRecDecl))
          methods = cDecl->methods;
        else if (auto *sDecl = llvm::dyn_cast<StructDeclNode>(currentRecDecl))
          methods = sDecl->methods;

        if (!node->templateArgs.empty()) {
          /* Instantiate EVERY matching method template with the explicit
           * type arguments: overloads like Future.delayed<R>(int64, fn) and
           * Future.delayed<R>(Duration, fn) must all be available so the
           * call-site overload resolution picks the right one. */
          for (auto *tmplDecl : methods) {
            if (tmplDecl->name != node->memberName || !tmplDecl->isTemplate)
              continue;
            if (node->templateArgs.size() != tmplDecl->templateParams.size())
              continue;
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
              std::vector<const Type *> fnArgs;
              for (size_t i = 0; i < tmplDecl->templateParams.size(); ++i) {
                const Type *resArg = resolveIfTemplate(node->templateArgs[i]);
                templateArgMap[tmplDecl->templateParams[i]] = resArg;
                fnArgs.push_back(resArg);
              }
              if (!checkTemplateConstraints(tmplDecl, fnArgs, node)) {
                return std::unexpected(ErrorInfo{
                    node->line, node->column, node->length,
                    "Template constraints are not satisfied"});
              }

              ASTCloner cloner(ctx->astCtx, templateArgMap);
              DeclNode *instDecl =
                  static_cast<DeclNode *>(cloner.dispatch(tmplDecl));

              if (instDecl && instDecl->kind == NodeKind::FunctionDecl) {
                auto *fnDecl = static_cast<FunctionDeclNode *>(instDecl);
                fnDecl->name = mangledView;

                std::string originalFq = std::string(tmplDecl->fqName);
                size_t lastDot = originalFq.find_last_of('.');
                std::string newFq;
                if (lastDot != std::string::npos) {
                  newFq = originalFq.substr(0, lastDot + 1) +
                          std::string(mangledView);
                } else {
                  newFq = std::string(mangledView);
                }
                fnDecl->fqName = ctx->astCtx.copyString(newFq);

                fnDecl->isMethod = true;
                fnDecl->isStatic = tmplDecl->isStatic;
                fnDecl->hasPublicMod = tmplDecl->hasPublicMod;
                fnDecl->hasPrivateMod = tmplDecl->hasPrivateMod;
                fnDecl->hasProtectedMod = tmplDecl->hasProtectedMod;
                fnDecl->annotations = tmplDecl->annotations;
                fnDecl->declFilePath = tmplDecl->declFilePath;
                fnDecl->isIntrinsic = tmplDecl->isIntrinsic;
                fnDecl->intrinsicName = tmplDecl->intrinsicName;
                fnDecl->isTemplate = false;
                fnDecl->templateParams = {};

                if (!fnDecl->isStatic && !fnDecl->params.empty() &&
                    fnDecl->params.front()->name == "this") {
                  const_cast<ParamDeclNode *>(fnDecl->params.front())->type =
                      ctx->astCtx.getPointerType(recordTy);
                }

                std::vector<FunctionDeclNode *> updatedMethods(methods.begin(),
                                                               methods.end());
                updatedMethods.push_back(fnDecl);

                if (currentRecDecl->kind == NodeKind::ClassDecl) {
                  methods =
                      ctx->astCtx.copyArray<FunctionDeclNode *>(updatedMethods);
                  const_cast<ClassDeclNode *>(
                      static_cast<const ClassDeclNode *>(currentRecDecl))
                      ->methods = methods;
                } else if (currentRecDecl->kind == NodeKind::StructDecl) {
                  methods =
                      ctx->astCtx.copyArray<FunctionDeclNode *>(updatedMethods);
                  const_cast<StructDeclNode *>(
                      static_cast<const StructDeclNode *>(currentRecDecl))
                      ->methods = methods;
                } else {
                  methods =
                      ctx->astCtx.copyArray<FunctionDeclNode *>(updatedMethods);
                  const_cast<UnionDeclNode *>(
                      static_cast<const UnionDeclNode *>(currentRecDecl))
                      ->methods = methods;
                }

                fnDecl->mangledName =
                    Mangler::mangle(fnDecl, std::string(recordTy->getName()));

                auto prevContext = ctx->getCurrentRecordContext();
                auto prevFile = ctx->currentFile;

                ctx->setCurrentRecordContext(recordTy);
                ctx->setCurrentFile(fnDecl->declFilePath);

                /* Restore the namespace the method template was declared in
                 * (see getDeclNamespace): generic bodies must resolve
                 * namespace-scoped symbols from their declaration site. */
                std::string instNs = getDeclNamespace(tmplDecl);
                size_t nsDepth = pushDeclNamespace(ctx, instNs);

                dispatch(fnDecl);

                for (size_t i = 0; i < nsDepth; ++i)
                  ctx->popNamespace();

                ctx->setCurrentFile(prevFile);
                ctx->setCurrentRecordContext(prevContext);
              }
            }
          }
        }

        bool methodFoundInLevel = false;

        for (const auto *method : methods) {
          if (method->name == node->memberName) {
            methodFoundInLevel = true;
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

            bool isPub = method->isPublic(method->name);
            bool isProt = method->isProtected(method->name);
            bool canAccess = false;
            auto *currCtx = ctx->getCurrentRecordContext();

            if (isPub) {
              canAccess = true;
            } else if (isProt) {
              if (currCtx == recordTy)
                canAccess = true;
              else if (currCtx) {
                const ClassType *currClass = llvm::dyn_cast<ClassType>(currCtx);
                while (currClass) {
                  if (currClass == recordTy) {
                    canAccess = true;
                    break;
                  }
                  if (currClass->getBaseClass()) {
                    if (auto *pType = llvm::dyn_cast<ClassType>(
                            currClass->getBaseClass()->getUnqualifiedType())) {
                      currClass = pType;
                    } else {
                      break;
                    }
                  } else
                    break;
                }
              }
            } else {
              if ((currCtx && currCtx->getDeclaration() == currentRecDecl) ||
                  method->declFilePath == ctx->currentFile) {
                canAccess = true;
              }
            }

            if (!canAccess) {
              return ctx->reportError(
                  node->line, node->column, node->length,
                  "Cannot access private/protected method '" +
                      std::string(method->name) + "'");
            }

            const_cast<MemberAccessNode *>(node)->isMethodRef = true;
            const_cast<MemberAccessNode *>(node)->resolvedMethod = method;
            node->exprType = ctx->astCtx.VoidTy;
            checkDeprecated(method, node);
            return ctx->astCtx.VoidTy;
          }
        }

        /* Dart-style named constructor reference: 'Foo.named(...)'.
         * Constructors are not inherited, so only the root level matches. */
        if (staticAccessDecl && currentRecDecl == recDecl &&
            currentRecDecl->kind == NodeKind::ClassDecl) {
          for (const auto *ctor :
               static_cast<const ClassDeclNode *>(currentRecDecl)->constructors) {
            if (ctor->isNamedCtor && ctor->name == node->memberName) {
              const_cast<MemberAccessNode *>(node)->isMethodRef = true;
              const_cast<MemberAccessNode *>(node)->resolvedMethod = ctor;
              const_cast<MemberAccessNode *>(node)->resolvedDecl =
                  currentRecDecl;
              node->exprType = ctx->astCtx.VoidTy;
              return ctx->astCtx.VoidTy;
            }
          }
        }

        if (currentRecDecl->kind == NodeKind::ClassDecl) {
          const ClassDeclNode *cDecl =
              static_cast<const ClassDeclNode *>(currentRecDecl);
          if (cDecl->baseClass) {
            if (auto *pType = llvm::dyn_cast<ClassType>(
                    cDecl->baseClass->getUnqualifiedType())) {
              currentRecDecl = pType->getDeclaration();
            } else {
              break;
            }
          } else {
            break;
          }
        } else {
          break;
        }
      }
    }
  }

  if (!staticAccessDecl) {
    /*
     * Rust-style auto-deref: when the member is not found on the record
     * itself, check whether the object type overloads 'operator*'. If so,
     * wrap the object in a unary dereference node and retry the access on the
     * resulting pointee type. Each implicit wrap adds one '*' node to the
     * object chain, so the walk below also bounds the recursion depth of
     * nested smart pointers.
 */
    int derefCount = 0;
    ExprNode *curr = node->object;
    while (curr && curr->kind == NodeKind::UnaryOp) {
      auto *uop = static_cast<UnaryOpNode *>(curr);
      if (uop->op == "*") {
        derefCount++;
        curr = uop->expr;
      } else {
        break;
      }
    }

    if (derefCount < 64) {
      if (resolveOverloadedOperator(*objType, "*", {})) {
        auto *derefNode = ctx->astCtx.create<UnaryOpNode>(
            "*", node->object, node->line, node->column);
        auto derefRes = dispatch(derefNode);
        if (derefRes) {
          const_cast<MemberAccessNode *>(node)->object = derefNode;
          return visit(node);
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

static bool hasEscapingBreak(const ASTNode *node, int breakableDepth = 0) {
  if (!node)
    return false;

  switch (node->kind) {
  case NodeKind::Break:
    /* A break with depth 0 belongs to the loop we are actively evaluating */
    return breakableDepth == 0;
  case NodeKind::Block: {
    const auto *block = static_cast<const BlockNode *>(node);
    for (const auto *stmt : block->statements) {
      if (hasEscapingBreak(stmt, breakableDepth))
        return true;
    }
    return false;
  }
  case NodeKind::If: {
    const auto *ifNode = static_cast<const IfNode *>(node);
    if (hasEscapingBreak(ifNode->thenBlock, breakableDepth))
      return true;
    if (hasEscapingBreak(ifNode->elseBlock, breakableDepth))
      return true;
    return false;
  }
  case NodeKind::While: {
    const auto *wNode = static_cast<const WhileNode *>(node);
    /* Increment depth because any break inside will bind to this inner loop */
    return hasEscapingBreak(wNode->body, breakableDepth + 1);
  }
  case NodeKind::For: {
    const auto *fNode = static_cast<const ForNode *>(node);
    return hasEscapingBreak(fNode->body, breakableDepth + 1);
  }
  case NodeKind::ForIn: {
    const auto *fNode = static_cast<const ForInNode *>(node);
    return hasEscapingBreak(fNode->body, breakableDepth + 1);
  }
  case NodeKind::Switch: {
    const auto *sNode = static_cast<const SwitchNode *>(node);
    for (const auto *c : sNode->cases) {
      for (const auto *stmt : c->statements) {
        /* Switch statements also consume break instructions */
        if (hasEscapingBreak(stmt, breakableDepth + 1))
          return true;
      }
    }
    return false;
  }
  case NodeKind::Try: {
    const auto *tNode = static_cast<const TryStmtNode *>(node);
    if (hasEscapingBreak(tNode->body, breakableDepth))
      return true;
    for (const auto *clause : tNode->clauses) {
      if (hasEscapingBreak(clause->body, breakableDepth))
        return true;
    }
    return false;
  }
  default:
    return false;
  }
}

static bool guaranteesReturn(const ASTNode *node) {
  if (!node)
    return false;

  if (node->kind == NodeKind::Return)
    return true;

  /* A throw never falls through. */
  if (node->kind == NodeKind::Throw)
    return true;

  if (node->kind == NodeKind::Block) {
    const auto *block = static_cast<const BlockNode *>(node);
    for (const auto *stmt : block->statements) {
      if (guaranteesReturn(stmt))
        return true;
    }
    return false;
  }

  if (node->kind == NodeKind::If) {
    const auto *ifStmt = static_cast<const IfNode *>(node);
    /* An 'if' statement only guarantees a return if it covers both branches */
    if (!ifStmt->elseBlock)
      return false;
    return guaranteesReturn(ifStmt->thenBlock) &&
           guaranteesReturn(ifStmt->elseBlock);
  }

  if (node->kind == NodeKind::Switch) {
    const auto *switchStmt = static_cast<const SwitchNode *>(node);
    /* A switch must be exhaustive (have a default) to guarantee a return */
    if (!switchStmt->hasDefault)
      return false;

    for (const auto *c : switchStmt->cases) {
      bool caseReturns = false;
      for (const auto *stmt : c->statements) {
        if (guaranteesReturn(stmt)) {
          caseReturns = true;
          break;
        }
      }
      /* If any case drops through without a guaranteed return, the switch fails
 */
      if (!caseReturns)
        return false;
    }
    return true;
  }

  if (node->kind == NodeKind::While) {
    const auto *whileStmt = static_cast<const WhileNode *>(node);
    if (whileStmt->condition &&
        whileStmt->condition->kind == NodeKind::Boolean) {
      const auto *boolCond =
          static_cast<const BoolNode *>(whileStmt->condition);
      /* An infinite loop guarantees that control flow will not fall through
         the end of the function, provided it has no escaping break statements
 */
      if (boolCond->value) {
        if (!hasEscapingBreak(whileStmt->body)) {
          return true;
        }
      }
    }
  }

  if (node->kind == NodeKind::Try) {
    const auto *tryStmt = static_cast<const TryStmtNode *>(node);
    /* The try statement only guarantees a return when the body and every
     * catch clause end in a return or a throw. */
    if (!guaranteesReturn(tryStmt->body))
      return false;
    for (const auto *clause : tryStmt->clauses) {
      if (!guaranteesReturn(clause->body))
        return false;
    }
    return true;
  }

  return false;
}

SemaResult TypeCheckPass::visit(const NamespaceDeclNode *node) {
  ctx->pushNamespace(node->name);

  size_t prevUsings = ctx->getUsingsCount();

  bool hasErrors = false;
  for (const auto *stmt : node->statements) {
    auto res = dispatch(stmt);
    if (!res)
      hasErrors = true;
    checkNodiscard(stmt);
  }

  ctx->resizeUsings(prevUsings);
  ctx->popNamespace();

  if (hasErrors) {
    return std::unexpected(ErrorInfo{node->line, node->column, node->length,
                                     "Errors in namespace declaration"});
  }
  return ctx->astCtx.VoidTy;
}

SemaResult TypeCheckPass::visit(const UsingNode *node) {
  ctx->addUsing(node->name);
  return ctx->astCtx.VoidTy;
}

const Type *TypeCheckPass::getFutureType(const Type *valueType) {
  if (!valueType)
    return nullptr;

  auto decls = ctx->lookup("Future");
  const DeclNode *futureDecl = nullptr;
  for (auto *d : decls) {
    if (auto *cls = llvm::dyn_cast<ClassDeclNode>(d)) {
      if (cls->isTemplate) {
        futureDecl = cls;
        break;
      }
    }
  }
  if (!futureDecl) {
    /* Template registered under its fully-qualified name only. */
    auto regIt = ctx->templateRegistry.find("Future");
    if (regIt == ctx->templateRegistry.end()) {
      regIt = ctx->templateRegistry.find("Future.Future");
      if (regIt == ctx->templateRegistry.end()) {
        (void)ctx->reportError(0, 0, 0,
                               "The 'Future' template is not available; "
                               "async support may be disabled.");
        return ctx->astCtx.VoidTy;
      }
    }
    futureDecl = regIt->second;
  }

  const Type *resolvedValue = resolveIfTemplate(valueType);
  const Type *argsArr[1] = {resolvedValue};
  auto args = ctx->astCtx.copyArray<const Type *>(argsArr);
  std::string_view baseName = futureDecl->fqName;
  if (baseName.empty()) {
    baseName = static_cast<const ClassDeclNode *>(futureDecl)->name;
  }
  auto *instTy = ctx->astCtx.getTemplateInstType(baseName, args);
  return resolveIfTemplate(instTy);
}

bool TypeCheckPass::unwrapFutureType(const Type *t, const Type **outValue) {
  if (!t)
    return false;
  const Type *u = t->getUnqualifiedType();
  while (u->isPointerType() || u->isReferenceType() ||
         u->getKind() == TypeKind::RValueReference) {
    if (u->isPointerType())
      u = static_cast<const PointerType *>(u)->getPointeeType()
              ->getUnqualifiedType();
    else if (u->isReferenceType())
      u = static_cast<const ReferenceType *>(u)->getPointeeType()
              ->getUnqualifiedType();
    else
      u = static_cast<const RValueReferenceType *>(u)->getPointeeType()
              ->getUnqualifiedType();
  }

  std::string_view baseName;
  llvm::ArrayRef<const Type *> args;
  if (auto *inst = llvm::dyn_cast<TemplateInstType>(u)) {
    baseName = inst->getBaseName();
    args = inst->getTemplateArgs();
  } else if (auto *rec = llvm::dyn_cast<RecordType>(u)) {
    baseName = rec->getTemplateBaseName();
    args = rec->getTemplateArgs();
  } else {
    return false;
  }

  if (baseName.empty() || args.size() != 1)
    return false;
  if (baseName != "Future" && baseName != "Future.Future" &&
      !baseName.ends_with(".Future"))
    return false;

  if (outValue)
    *outValue = args[0];
  return true;
}

const FunctionDeclNode *TypeCheckPass::instantiateMethodTemplate(
    const FunctionDeclNode *tmplDecl, const RecordType *recordTy,
    llvm::ArrayRef<const Type *> explicitArgs) {
  if (!tmplDecl || !tmplDecl->isTemplate)
    return nullptr;

  /* Resolve the type arguments: explicit ones are used as-is; otherwise the
   * method's template parameters are bound positionally to the enclosing
   * record's template arguments ('Future<int>.value(5)' binds R = int). */
  std::vector<const Type *> resolvedArgs;
  if (!explicitArgs.empty()) {
    for (const auto *arg : explicitArgs) {
      resolvedArgs.push_back(resolveIfTemplate(arg));
    }
  } else {
    for (const auto *arg : recordTy->getTemplateArgs()) {
      resolvedArgs.push_back(resolveIfTemplate(arg));
    }
  }

  if (resolvedArgs.size() != tmplDecl->templateParams.size())
    return nullptr;

  std::string mangledName = std::string(tmplDecl->name);
  for (const auto *arg : resolvedArgs) {
    std::string argStr = arg->toString();
    for (char &ch : argStr) {
      if (!isalnum(ch))
        ch = '_';
    }
    mangledName += "_" + argStr;
  }
  /* Disambiguate overloads that share template parameters but differ in
   * their parameter lists (e.g. 'sort<T2>()' vs 'sort<T2>(cmp)'): the
   * resolved-argument suffix alone would collide. */
  for (const auto *p : tmplDecl->params) {
    std::string paramStr = p->type ? p->type->toString() : "void";
    for (char &ch : paramStr) {
      if (!isalnum(ch))
        ch = '_';
    }
    mangledName += "_p_" + paramStr;
  }
  std::string_view mangledView = ctx->astCtx.copyString(mangledName);

  /* Reuse an existing instantiation when present. */
  const DeclNode *recDecl = recordTy->getDeclaration();
  llvm::ArrayRef<FunctionDeclNode *> methods;
  if (recDecl) {
    if (recDecl->kind == NodeKind::ClassDecl)
      methods = static_cast<const ClassDeclNode *>(recDecl)->methods;
    else if (recDecl->kind == NodeKind::StructDecl)
      methods = static_cast<const StructDeclNode *>(recDecl)->methods;
    else if (recDecl->kind == NodeKind::UnionDecl)
      methods = static_cast<const UnionDeclNode *>(recDecl)->methods;
    for (auto *m : methods) {
      if (m->name == mangledView)
        return m;
    }
  }

  std::unordered_map<std::string_view, const Type *> templateArgMap;
  for (size_t i = 0; i < tmplDecl->templateParams.size(); ++i) {
    templateArgMap[tmplDecl->templateParams[i]] = resolvedArgs[i];
  }
  if (!checkTemplateConstraints(tmplDecl, resolvedArgs, nullptr)) {
    (void)ctx->reportError(0, 0, 0,
                           "Template constraints are not satisfied for '" +
                               std::string(tmplDecl->name) + "'.");
    return nullptr;
  }

  ASTCloner cloner(ctx->astCtx, templateArgMap);
  DeclNode *instDecl = static_cast<DeclNode *>(cloner.dispatch(tmplDecl));
  if (!instDecl || instDecl->kind != NodeKind::FunctionDecl) {
    return nullptr;
  }

  auto *fnDecl = static_cast<FunctionDeclNode *>(instDecl);
  fnDecl->name = mangledView;

  std::string originalFq = std::string(tmplDecl->fqName);
  size_t lastDot = originalFq.find_last_of('.');
  std::string newFq;
  if (lastDot != std::string::npos) {
    newFq = originalFq.substr(0, lastDot + 1) + std::string(mangledView);
  } else {
    newFq = std::string(mangledView);
  }
  fnDecl->fqName = ctx->astCtx.copyString(newFq);

  fnDecl->isMethod = true;
  fnDecl->isStatic = tmplDecl->isStatic;
  fnDecl->hasPublicMod = tmplDecl->hasPublicMod;
  fnDecl->hasPrivateMod = tmplDecl->hasPrivateMod;
  fnDecl->hasProtectedMod = tmplDecl->hasProtectedMod;
  fnDecl->annotations = tmplDecl->annotations;
  fnDecl->declFilePath = tmplDecl->declFilePath;
  fnDecl->isIntrinsic = tmplDecl->isIntrinsic;
  fnDecl->intrinsicName = tmplDecl->intrinsicName;
  fnDecl->parentRecord = recordTy;
  /* The substitution is complete: the instantiated function is concrete. */
  fnDecl->isTemplate = false;
  fnDecl->templateParams = {};

  if (!fnDecl->isStatic && !fnDecl->params.empty() &&
      fnDecl->params.front()->name == "this") {
    const_cast<ParamDeclNode *>(fnDecl->params.front())->type =
        ctx->astCtx.getPointerType(recordTy);
  }

  if (recDecl) {
    std::vector<FunctionDeclNode *> updatedMethods(methods.begin(),
                                                   methods.end());
    updatedMethods.push_back(fnDecl);
    if (recDecl->kind == NodeKind::ClassDecl)
      const_cast<ClassDeclNode *>(
          static_cast<const ClassDeclNode *>(recDecl))
          ->methods = ctx->astCtx.copyArray<FunctionDeclNode *>(updatedMethods);
    else if (recDecl->kind == NodeKind::StructDecl)
      const_cast<StructDeclNode *>(
          static_cast<const StructDeclNode *>(recDecl))
          ->methods = ctx->astCtx.copyArray<FunctionDeclNode *>(updatedMethods);
    else if (recDecl->kind == NodeKind::UnionDecl)
      const_cast<UnionDeclNode *>(static_cast<const UnionDeclNode *>(recDecl))
          ->methods = ctx->astCtx.copyArray<FunctionDeclNode *>(updatedMethods);
  }

  fnDecl->mangledName = Mangler::mangle(fnDecl, std::string(recordTy->getName()));

  auto prevContext = ctx->getCurrentRecordContext();
  auto prevFile = ctx->currentFile;

  ctx->setCurrentRecordContext(recordTy);
  ctx->setCurrentFile(fnDecl->declFilePath);

  /* Restore the namespace the method template was declared in so its body
   * can resolve namespace-scoped symbols (same fix as the record/function
   * template instantiation paths). */
  std::string instNs = getDeclNamespace(tmplDecl);
  size_t nsDepth = pushDeclNamespace(ctx, instNs);

  dispatch(fnDecl);

  for (size_t i = 0; i < nsDepth; ++i)
    ctx->popNamespace();

  ctx->setCurrentFile(prevFile);
  ctx->setCurrentRecordContext(prevContext);

  return fnDecl;
}

SemaResult TypeCheckPass::visit(const AwaitExprNode *node) {
  auto res = dispatch(node->expr);
  if (!res)
    return std::unexpected(ErrorInfo{node->line, node->column, node->length,
                                     "Failed to evaluate await operand."});

  const FunctionDeclNode *cur = ctx->getCurrentFunction();
  if (cur && !cur->isAsync) {
    return ctx->reportError(
        node->line, node->column, node->length,
        "The 'await' expression is only allowed inside 'async' functions.");
  }
  if (cur)
    const_cast<FunctionDeclNode *>(cur)->hasAwait = true;

  const Type *t = *res;
  const Type *valueTy = nullptr;
  if (unwrapFutureType(t, &valueTy)) {
    node->exprType = valueTy;
    node->isLValue = false;
    return valueTy;
  }
  /* Dart semantics: awaiting a non-Future passes the value through. */
  node->exprType = t;
  return t;
}

SemaResult TypeCheckPass::visit(const FunctionDeclNode *node) {
  if (node->isTemplate)
    return ctx->astCtx.VoidTy;

  const FunctionDeclNode *prevFunc = ctx->getCurrentFunction();
  ctx->setCurrentFunction(node);

  /* Collect the function for the mayUnwind fixed-point analysis. */
  if (std::find(allFunctions.begin(), allFunctions.end(), node) ==
      allFunctions.end()) {
    allFunctions.push_back(node);
  }

  const_cast<FunctionDeclNode *>(node)->returnType =
      resolveIfTemplate(node->returnType);

  if (node->returnType->getKind() == TypeKind::TemplateParam) {
    ctx->setCurrentFunction(prevFunc);
    return ctx->reportError(node->line, node->column, node->length,
                            "Unknown return type: '" +
                                node->returnType->toString() + "'");
  }

  if (!checkTypeVisibility(node->returnType, node)) {
    ctx->setCurrentFunction(prevFunc);
    return std::unexpected(ErrorInfo{node->line, node->column, node->length,
                                     "Type visibility error"});
  }

  /* Async functions return a Future of their declared type. A declared
   * 'Future<T>' return type is allowed and keeps the value type 'T' for the
   * body's return statements (Dart semantics). */
  const Type *valueReturnType = node->returnType;
  if (node->isAsync) {
    const Type *inner = nullptr;
    if (unwrapFutureType(node->returnType, &inner)) {
      valueReturnType = inner;
    }
    const_cast<FunctionDeclNode *>(node)->effectiveReturnType =
        getFutureType(valueReturnType);
  }

  if (node->name == "main" && !node->isMethod) {
    const Type *unqualRet = valueReturnType->getUnqualifiedType();
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
  ctx->setFunctionReturnType(valueReturnType);

  ScopeGuard guard(*ctx, ScopeKind::FunctionParams);
  bool hasErrors = false;

  if (node->isMethod && !node->isExtern && !node->isStatic &&
      node->parentRecord) {
    auto *thisParam = ctx->astCtx.create<ParamDeclNode>(
        ctx->astCtx.getPointerType(node->parentRecord), "this", nullptr, false,
        false, node->line, node->column, 4);
    ctx->addDecl("this", thisParam);
  }

  /* Whether this function is a constructor of its record: several checks
   * (this-parameters, effect analysis) only apply to constructors. */
  bool isCtor = false;

  /* Validate 'this.x' parameters: they are only legal in constructors, and
   * their types come from the record's fields (resolved during declaration
   * collection; re-resolved here so template-instantiated constructors work
   * even when that pass skipped them). */
  if (node->parentRecord) {
    const DeclNode *recDecl = node->parentRecord->getDeclaration();
    llvm::ArrayRef<VarDeclNode *> recFields;
    if (recDecl) {
      if (auto *cDecl = llvm::dyn_cast<ClassDeclNode>(recDecl)) {
        recFields = cDecl->fields;
        for (const auto *c : cDecl->constructors)
          if (c == node) isCtor = true;
      } else if (auto *sDecl = llvm::dyn_cast<StructDeclNode>(recDecl)) {
        recFields = sDecl->fields;
        for (const auto *c : sDecl->constructors)
          if (c == node) isCtor = true;
      } else if (auto *uDecl = llvm::dyn_cast<UnionDeclNode>(recDecl)) {
        recFields = uDecl->fields;
        for (const auto *c : uDecl->constructors)
          if (c == node) isCtor = true;
      }
    }
    for (const auto *param : node->params) {
      if (!param->isThisParam)
        continue;
      if (!isCtor) {
        ctx->reportError(param->line, param->column, param->length,
                         "Initializing formal parameters ('this." +
                             std::string(param->name) +
                             "') can only be used in constructors.");
        hasErrors = true;
        continue;
      }
      if (!param->type) {
        const VarDeclNode *field = nullptr;
        for (const auto *f : recFields) {
          if (f->varName == param->name) {
            field = f;
            break;
          }
        }
        if (!field) {
          ctx->reportError(param->line, param->column, param->length,
                           "No field named '" + std::string(param->name) +
                               "' for 'this." + std::string(param->name) +
                               "'.");
          hasErrors = true;
          continue;
        }
        if (field->isStatic) {
          ctx->reportError(param->line, param->column, param->length,
                           "Field '" + std::string(param->name) +
                               "' is static; 'this." +
                               std::string(param->name) +
                               "' cannot initialize a static field.");
          hasErrors = true;
          continue;
        }
        const_cast<ParamDeclNode *>(param)->type = field->type;
      }
    }
  }

  for (const auto *param : node->params) {
    /* Dispatch the parameter to ensure template instances are properly mapped
     * to their RecordType definitions */
    auto paramRes = dispatch(param);
    if (!paramRes) {
      hasErrors = true;
    }

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

  /* Dart rule: the initializer list (super call + ': this.x = expr'
   * entries) of a const constructor is a constant context, so constructor
   * calls there are implicitly const. */
  bool ctorInitIsConst = node->isConst && node->isMethod && node->parentRecord;
  if (ctorInitIsConst)
    ctx->constContextDepth++;

  if (node->superCall) {
    auto superRes = dispatch(node->superCall);
    if (!superRes) {
      hasErrors = true;
    }
  }

  for (const auto *init : node->fieldInitializers) {
    /* The 'isFieldInit' marker allows the assignment to write to final
     * fields; the target itself is resolved like any member access. */
    auto initRes = dispatch(init);
    if (!initRes) {
      hasErrors = true;
    }
  }

  if (ctorInitIsConst)
    ctx->constContextDepth--;

  if (node->body) {
    auto bodyRes = dispatch(node->body);
    if (!bodyRes) {
      hasErrors = true;
    }

    if (!valueReturnType->isVoid() && !guaranteesReturn(node->body)) {
      SemaResult err =
          ctx->reportError(node->line, node->column, node->length,
                           "This function has a return type of '" +
                               valueReturnType->toString() +
                               "', but doesn't end with a return statement.");
      hasErrors = true;
    }

    /* The coroutine IR generated for async functions cannot be analyzed for
     * effect attributes; skip the analysis so no wrong attributes leak into
     * the coroutine. */
    if (!node->isAsync) {
      EffectAnalyzer ea;
      if (node->superCall)
        ea.dispatch(node->superCall);
      for (const auto *init : node->fieldInitializers)
        ea.dispatch(init);
      ea.dispatch(node->body);

      /* Constructors and destructors do more than their AST body shows:
       * field declaration initializers, 'this.x' parameter stores and
       * field-destructor cleanups are all lowered by CodeGen outside the
       * body. An empty 'Box() {}' over a 'int v = 1;' field would
       * otherwise analyze as memory(none), letting the optimizer delete
       * the constructor call and leave the field uninitialized. */
      bool isDtor = node->name == "~" && node->isMethod;
      if (isCtor || isDtor) {
        ea.writesMem = true;
        if (isDtor) {
          ea.readsMem = true;
          ea.freesMem = true;
        }
      }

      node->isReadNone = !ea.readsMem && !ea.writesMem;
      node->isReadOnly = ea.readsMem && !ea.writesMem;
      node->isNoFree = !ea.freesMem;
      node->isNoSync = !ea.hasSync;
      node->isWillReturn = !ea.potentiallyInfinite;
      node->isMustProgress = true;
    }
  }

  ctx->setFunctionReturnType(prevRet);
  ctx->setCurrentFunction(prevFunc);

  if (hasErrors) {
    return std::unexpected(ErrorInfo{node->line, node->column, node->length,
                                     "Errors in function declaration"});
  }

  return node->effectiveReturnType ? node->effectiveReturnType
                                   : node->returnType;
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
          fDecl->effectiveReturnType ? fDecl->effectiveReturnType
                                     : fDecl->returnType,
          ctx->astCtx.copyArray<const Type *>(pTypes));
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

  auto emitRValueRefWarnings =
      [&](const FunctionDeclNode *func,
          const std::vector<ExprNode *> &resolvedArgs) {
        for (size_t p = 0; p < func->params.size() && p < resolvedArgs.size();
             ++p) {
          const Type *paramType = func->params[p]->type;
          if (paramType->isReferenceType() && !resolvedArgs[p]->isLValue) {
            const Type *pointee =
                static_cast<const ReferenceType *>(paramType)->getPointeeType();
            if (!pointee->isConstQualified()) {
              ctx->reportWarning(
                  WarningKind::ImplicitCast, resolvedArgs[p]->line,
                  resolvedArgs[p]->column, resolvedArgs[p]->length,
                  "Binding an r-value to non-const reference parameter '" +
                      std::string(func->params[p]->name) +
                      "' will implicitly create a stack-allocated temporary.",
                  resolvedArgs[p]->endLine);
            }
          }
        }
      };

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

    if (!errors.empty()) {
      return errors;
    }

    for (size_t p = 0; p < expectedParams; ++p) {
      const Type *paramType = fDecl->params[p]->type;
      /* A not-yet-visited template-class method's parameters may still hold
       * unresolved template instantiations ('const Map<K, V>&'); resolve
       * them here so calls between the class's own methods match. */
      paramType = resolveIfTemplate(paramType);

      if (paramType->getKind() == TypeKind::Array) {
        paramType = ctx->astCtx.getPointerType(
            static_cast<const ArrayType *>(paramType)->getElementType());
      }

      /* Async lambdas only match parameters whose signature returns a
       * Future; sync lambdas only match non-Future returns. Without this
       * filter the 'auto' return placeholder of an unresolved lambda would
       * match the wrong overload and the re-type pass would fail.
       * Future.runOnThread is the exception: it accepts an async lambda and
       * runs it on a worker thread with its own event loop. */
      bool isRunOnThread =
          fDecl->isIntrinsic && fDecl->intrinsicName == "future_runOnThread";
      if (resolvedArgs[p] && resolvedArgs[p]->kind == NodeKind::Lambda &&
          !isRunOnThread) {
        auto *lambda = static_cast<const LambdaNode *>(resolvedArgs[p]);
        const Type *paramFnReturn = nullptr;
        const Type *ptU = paramType->getUnqualifiedType();
        if (ptU->isPointerType()) {
          const Type *ptPointee =
              static_cast<const PointerType *>(ptU)
                  ->getPointeeType()
                  ->getUnqualifiedType();
          if (auto *ptFn = llvm::dyn_cast<FunctionType>(ptPointee)) {
            paramFnReturn = ptFn->getReturnType();
          }
        }
        bool paramIsFuture =
            paramFnReturn && unwrapFutureType(paramFnReturn, nullptr);
        if (lambda->isAsync != paramIsFuture) {
          errors.push_back(
              lambda->isAsync
                  ? "An 'async' lambda cannot be passed where a synchronous "
                    "function is expected."
                  : "A synchronous lambda cannot be passed where an 'async' "
                    "function is expected.");
          continue;
        }
      }

      /* Future.runOnThread accepts an async lambda: the worker runs it with
       * its own event loop and the outer future completes with the lambda's
       * value type. Compare the lambda's Future<X> signature against the
       * X-returning parameter signature. */
      const Type *argTypeForCast = resolvedTypes[p];
      if (isRunOnThread && argTypeForCast) {
        const Type *uArg = argTypeForCast->getUnqualifiedType();
        if (uArg->isPointerType()) {
          const Type *argPointee =
              static_cast<const PointerType *>(uArg)->getPointeeType()
                  ->getUnqualifiedType();
          if (auto *argFn = llvm::dyn_cast<FunctionType>(argPointee)) {
            const Type *argInner = nullptr;
            if (unwrapFutureType(argFn->getReturnType(), &argInner)) {
              std::vector<const Type *> argParams;
              for (const auto *pt : argFn->getParamTypes())
                argParams.push_back(pt);
              const Type *unwrappedFn = ctx->astCtx.getFunctionType(
                  argInner, ctx->astCtx.copyArray<const Type *>(argParams));
              argTypeForCast = ctx->astCtx.getPointerType(unwrappedFn);
            }
          }
        }
      }

      if (!canImplicitlyCast(argTypeForCast, paramType)) {
        std::string gotType = argTypeForCast ? argTypeForCast->toString()
                                             : "unresolved/unknown";
        errors.push_back("Type mismatch for parameter '" +
                         std::string(fDecl->params[p]->name) + "': expected '" +
                         paramType->toString() + "', but got '" + gotType +
                         "'.");
      } else {
        if (explicitlyProvided[p]) {
          /* Exact (or identity-after-unqualification) matches outrank
           * implicit numeric conversions so overload sets like abs(int64) /
           * abs(float64) and min(int32) / min(float64) resolve to the
           * argument's own type instead of the first declared candidate. */
          const Type *uArg = argTypeForCast->getUnqualifiedType();
          const Type *uParam = paramType->getUnqualifiedType();
          if (uArg == uParam) {
            outScore += 10;
          } else if (canImplicitlyCast(argTypeForCast, paramType, false)) {
            outScore += 5;
          }
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
              if (explicitlyProvided[p]) {
                outScore += 1;
              }
            } else if (explicitlyProvided[p]) {
              outScore += 3;
            }
          } else if (explicitlyProvided[p]) {
            outScore += 2;
          }
        } else if (explicitlyProvided[p]) {
          outScore += 1;
        }

        if (!explicitlyProvided[p]) {
          outScore -= 1;
        }
      }
    }

    if (!errors.empty()) {
      return errors;
    }

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

  if (node->isSuperCall ||
      (node->target->kind == NodeKind::Variable &&
       static_cast<const VariableNode *>(node->target)->name == "super")) {
    const RecordType *currRec = ctx->getCurrentRecordContext();
    if (!currRec || currRec->getKind() != TypeKind::Class) {
      return ctx->reportError(node->line, node->column, node->length,
                              "Use of 'super()' outside of a class method.");
    }
    auto *classTy = static_cast<const ClassType *>(currRec);
    if (!classTy->getBaseClass()) {
      return ctx->reportError(node->line, node->column, node->length,
                              "Class '" + std::string(classTy->getName()) +
                                  "' has no base class.");
    }

    const FunctionDeclNode *currFunc = ctx->getCurrentFunction();
    if (!currFunc) {
      return ctx->reportError(node->line, node->column, node->length,
                              "super() can only be called inside a method.");
    }

    const Type *baseTy = classTy->getBaseClass()->getUnqualifiedType();
    auto *baseClassTy = static_cast<const ClassType *>(baseTy);
    const DeclNode *baseDecl = baseClassTy->getDeclaration();

    if (!baseDecl || !llvm::isa<ClassDeclNode>(baseDecl)) {
      return ctx->reportError(node->line, node->column, node->length,
                              "Invalid base class declaration.");
    }

    auto *baseCDecl = static_cast<const ClassDeclNode *>(baseDecl);
    std::string_view targetMethodName = currFunc->name;
    /* Constructors are named after the record's SIMPLE name; the record
     * type carries the fully qualified name ('Demo.Player'), so compare
     * against the last name component. */
    bool isConstructorCall = false;
    if (currFunc->parentRecord) {
      std::string recName = std::string(currFunc->parentRecord->getName());
      size_t dot = recName.find_last_of('.');
      std::string simpleName =
          (dot == std::string::npos) ? recName : recName.substr(dot + 1);
      isConstructorCall = (currFunc->name == simpleName);
    }

    std::vector<const FunctionDeclNode *> candidates;
    if (isConstructorCall) {
      for (auto *c : baseCDecl->constructors) {
        if (!c->isImplicit &&
            (c->isPublic(c->name) || c->isProtected(c->name))) {
          candidates.push_back(c);
        }
      }
    } else {
      for (auto *m : baseCDecl->methods) {
        if (m->name == targetMethodName) {
          if (m->isPublic(m->name) || m->isProtected(m->name)) {
            candidates.push_back(m);
          }
        }
      }
    }

    if (candidates.empty()) {
      if (isConstructorCall) {
        return ctx->reportError(
            node->line, node->column, node->length,
            "Base class '" + std::string(baseClassTy->getName()) +
                "' does not define any accessible constructors.");
      } else {
        return ctx->reportError(node->line, node->column, node->length,
                                "Base class '" +
                                    std::string(baseClassTy->getName()) +
                                    "' does not define method '" +
                                    std::string(targetMethodName) + "'.");
      }
    }

    const FunctionDeclNode *bestMatch = nullptr;
    int bestScore = std::numeric_limits<int>::min();
    std::vector<std::vector<std::string>> overloadErrors;
    std::vector<ExprNode *> bestResolvedArgs;

    for (const auto *cand : candidates) {
      int score = 0;
      std::vector<ExprNode *> resolvedArgs;
      auto errs = checkMatch(cand, score, resolvedArgs);
      if (errs.empty()) {
        if (score > bestScore) {
          bestScore = score;
          bestMatch = cand;
          bestResolvedArgs = resolvedArgs;
          overloadErrors.clear();
        }
      } else {
        overloadErrors.push_back(errs);
      }
    }

    if (!bestMatch) {
      std::string finalErr =
          "No matching overload for 'super" +
          (isConstructorCall ? std::string("")
                             : "." + std::string(targetMethodName)) +
          "()' call.";
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

    const_cast<FunctionCallNode *>(node)->resolvedFunc = bestMatch;
    const_cast<FunctionCallNode *>(node)->args =
        ctx->astCtx.copyArray<ExprNode *>(bestResolvedArgs);
    const_cast<FunctionCallNode *>(node)->argNames = {};
    emitRValueRefWarnings(bestMatch, bestResolvedArgs);

    const_cast<FunctionCallNode *>(node)->isSuperCall = true;
    node->exprType = resolveIfTemplate(bestMatch->effectiveReturnType
                         ? bestMatch->effectiveReturnType
                         : bestMatch->returnType);
    node->isLValue = node->exprType->isReferenceType();
    return node->exprType;
  }

  if (node->target->kind == NodeKind::Variable) {
    auto *varTarget = static_cast<const VariableNode *>(node->target);

    /* Template argument deduction: 'findMax(100, 250)' / 'Box(42)'. When the
     * callee is a template and the type arguments are missing (or only given
     * partially), deduce them from the call arguments and fall through to
     * the regular explicit-instantiation path below. */
    bool dedIsLocal = false;
    for (auto *d : ctx->lookup(varTarget->name)) {
      if (d->kind == NodeKind::ParamDecl ||
          (d->kind == NodeKind::VarDecl &&
           !static_cast<const VarDeclNode *>(d)->isGlobal)) {
        dedIsLocal = true;
        break;
      }
    }
    if (!dedIsLocal) {
      const DeclNode *tmpl = nullptr;
      auto regIt = ctx->templateRegistry.find(varTarget->name);
      if (regIt != ctx->templateRegistry.end()) {
        tmpl = regIt->second;
      } else {
        for (auto *d : ctx->lookup(varTarget->name)) {
          if (d->isTemplate &&
              (d->kind == NodeKind::ClassDecl ||
               d->kind == NodeKind::StructDecl ||
               d->kind == NodeKind::UnionDecl)) {
            tmpl = d;
            break;
          }
        }
      }
      if (tmpl && tmpl->isTemplate &&
          varTarget->templateArgs.size() < tmpl->templateParams.size()) {
        bool isCtorCall = tmpl->kind == NodeKind::ClassDecl ||
                          tmpl->kind == NodeKind::StructDecl ||
                          tmpl->kind == NodeKind::UnionDecl;
        /* Class templates only support the all-or-nothing form: partial
         * explicit type arguments are not allowed (C++ rule). */
        if (!isCtorCall || varTarget->templateArgs.empty()) {
          std::vector<const Type *> completeArgs;
          std::string dedErr;
          bool ok = false;
          if (auto *tfn = llvm::dyn_cast<FunctionDeclNode>(tmpl)) {
            /* Memory.construct<T> deduces its type from the destination
             * pointer rather than the template parameter list; it is handled
             * by checkMemoryConstruct below. The type-queries sizeof<T> and
             * alignof<T> are likewise skipped: their argument is a dependent
             * type value inside template bodies, and the template parameter
             * is substituted by the enclosing template's instantiation
             * (C++ defers these deductions to the point of instantiation). */
            if (!(tfn->isIntrinsic &&
                  (tfn->intrinsicName == "memory_construct" ||
                   tfn->intrinsicName == "sizeof_expr" ||
                   tfn->intrinsicName == "alignof_expr"))) {
              ok = deduceTemplateArguments(tfn, argTypes, node->argNames,
                                           varTarget->templateArgs,
                                           completeArgs, dedErr);
            }
          } else {
            ok = deduceClassTemplateArguments(tmpl, argTypes, node->argNames,
                                              completeArgs, dedErr);
          }
          if (ok) {
            /* Resolve the deduced arguments before they are stored on the
             * call site: nested instantiations must be resolved records so
             * the mangled names and cloned signatures match the regular
             * type-resolution path ('sizeof(Future<int>)' must instantiate
             * exactly like 'sizeof<Future<int>>' would). */
            for (auto &dedArg : completeArgs)
              dedArg = resolveIfTemplate(dedArg);
            if (!checkTemplateConstraints(tmpl, completeArgs, node)) {
              return std::unexpected(ErrorInfo{
                  node->line, node->column, node->length,
                  "Template constraints are not satisfied"});
            }
            const_cast<VariableNode *>(varTarget)->templateArgs =
                ctx->astCtx.copyArray<const Type *>(completeArgs);
          } else {
            /* Only report the deduction failure when no non-template
             * candidate could take the call instead. */
            bool hasOtherCandidate = false;
            for (auto *d : ctx->lookup(varTarget->name)) {
              if (d->isTemplate)
                continue;
              if (d->kind == NodeKind::FunctionDecl ||
                  d->kind == NodeKind::ClassDecl ||
                  d->kind == NodeKind::StructDecl ||
                  d->kind == NodeKind::UnionDecl) {
                hasOtherCandidate = true;
                break;
              }
            }
            const RecordType *curRec = ctx->getCurrentRecordContext();
            const DeclNode *curRecDecl =
                curRec ? curRec->getDeclaration() : nullptr;
            while (curRecDecl && !hasOtherCandidate) {
              llvm::ArrayRef<FunctionDeclNode *> methods;
              if (curRecDecl->kind == NodeKind::ClassDecl)
                methods = static_cast<const ClassDeclNode *>(curRecDecl)
                              ->methods;
              else if (curRecDecl->kind == NodeKind::StructDecl)
                methods = static_cast<const StructDeclNode *>(curRecDecl)
                              ->methods;
              else if (curRecDecl->kind == NodeKind::UnionDecl)
                methods = static_cast<const UnionDeclNode *>(curRecDecl)
                              ->methods;
              for (const auto *m : methods) {
                if (m->name == varTarget->name && !m->isTemplate) {
                  hasOtherCandidate = true;
                  break;
                }
              }
              if (curRecDecl->kind == NodeKind::ClassDecl) {
                const ClassDeclNode *cDecl =
                    static_cast<const ClassDeclNode *>(curRecDecl);
                if (cDecl->baseClass) {
                  if (auto *pType = llvm::dyn_cast<ClassType>(
                          cDecl->baseClass->getUnqualifiedType())) {
                    curRecDecl = pType->getDeclaration();
                  } else {
                    break;
                  }
                } else {
                  break;
                }
              } else {
                break;
              }
            }
            if (!hasOtherCandidate) {
              return ctx->reportError(
                  node->line, node->column, node->length,
                  "Could not deduce template arguments for '" +
                      std::string(varTarget->name) + "': " + dedErr + ".");
            }
          }
        }
      }
    }

    if (!varTarget->templateArgs.empty()) {
      /* Template classes used as constructor calls (e.g.
       * 'unique_ptr<Foo>(ptr)') are instantiated here so the target can be
       * renamed to its mangled record name and resolved through constructor
       * candidate matching below. */
      auto regIt = ctx->templateRegistry.find(varTarget->name);
      if (regIt != ctx->templateRegistry.end() &&
          (llvm::isa<ClassDeclNode>(regIt->second) ||
           llvm::isa<StructDeclNode>(regIt->second) ||
           llvm::isa<UnionDeclNode>(regIt->second))) {
        /* Use the fully-qualified template name so the mangled instantiation
         * name matches the one produced by the type-resolution path. */
        const DeclNode *tmpl = regIt->second;
        std::string_view baseName = tmpl->fqName;
        auto argsCopy = ctx->astCtx.copyArray<const Type *>(
            varTarget->templateArgs);
        auto *instTy =
            ctx->astCtx.getTemplateInstType(baseName, argsCopy);
        const Type *resolved = resolveIfTemplate(instTy);
        if (resolved) {
          const Type *unqual = resolved->getUnqualifiedType();
          if (unqual->getKind() == TypeKind::Class ||
              unqual->getKind() == TypeKind::Struct ||
              unqual->getKind() == TypeKind::Union) {
            auto *recTy = static_cast<const RecordType *>(unqual);
            const_cast<VariableNode *>(varTarget)->name = recTy->getName();
          }
        }
      } else {
        /* Force template function instantiation up front so the target is
         * renamed to its mangled form (e.g. 'make_unique_Inner') and resolves
         * through the direct call matching below. */
        (void)dispatch(varTarget);
      }
    }

    std::string_view name = varTarget->name;
    auto decls = ctx->lookup(name);

    bool isLocal = false;
    for (auto *d : decls) {
      if (d->kind == NodeKind::ParamDecl ||
          (d->kind == NodeKind::VarDecl &&
           !static_cast<const VarDeclNode *>(d)->isGlobal)) {
        isLocal = true;
        break;
      }
    }

    if (!isLocal) {
      const RecordType *thisRecTy = nullptr;
      auto thisDecls = ctx->lookup("this");
      if (!thisDecls.empty()) {
        if (auto *paramDecl =
                llvm::dyn_cast<ParamDeclNode>(thisDecls.front())) {
          if (auto *ptrTy = llvm::dyn_cast<PointerType>(paramDecl->type)) {
            if (auto *clsTy = llvm::dyn_cast<RecordType>(
                    ptrTy->getPointeeType()->getUnqualifiedType())) {
              thisRecTy = clsTy;
            }
          }
        }
      }

      if (thisRecTy) {
        const DeclNode *recDecl = thisRecTy->getDeclaration();
        bool isMethod = false;
        const DeclNode *currentRecDecl = recDecl;

        while (currentRecDecl) {
          llvm::ArrayRef<FunctionDeclNode *> methods;
          if (currentRecDecl->kind == NodeKind::ClassDecl)
            methods =
                static_cast<const ClassDeclNode *>(currentRecDecl)->methods;
          else if (currentRecDecl->kind == NodeKind::StructDecl)
            methods =
                static_cast<const StructDeclNode *>(currentRecDecl)->methods;
          else if (currentRecDecl->kind == NodeKind::UnionDecl)
            methods =
                static_cast<const UnionDeclNode *>(currentRecDecl)->methods;

          for (const auto *m : methods) {
            if (m->name == name && !m->isStatic) {
              isMethod = true;
              break;
            }
          }

          if (isMethod)
            break;

          if (currentRecDecl->kind == NodeKind::ClassDecl) {
            const ClassDeclNode *cDecl =
                static_cast<const ClassDeclNode *>(currentRecDecl);
            if (cDecl->baseClass) {
              if (auto *pType = llvm::dyn_cast<ClassType>(
                      cDecl->baseClass->getUnqualifiedType())) {
                currentRecDecl = pType->getDeclaration();
              } else {
                break;
              }
            } else {
              break;
            }
          } else {
            break;
          }
        }

        if (isMethod) {
          auto *thisVar = ctx->astCtx.create<VariableNode>(
              "this", varTarget->line, varTarget->column, 4);
          auto thisRes = dispatch(thisVar);
          if (thisRes) {
            auto *maNode = ctx->astCtx.create<MemberAccessNode>(
                thisVar, name, varTarget->line, varTarget->column,
                varTarget->length);
            maNode->templateArgs = varTarget->templateArgs;
            const_cast<FunctionCallNode *>(node)->target = maNode;
          }
        }
      }
    }

    if (node->target->kind == NodeKind::Variable) {
      if (!isLocal) {
        if (const RecordType *recTy = ctx->getCurrentRecordContext()) {
          const DeclNode *recDecl = recTy->getDeclaration();
          if (recDecl) {
            llvm::ArrayRef<FunctionDeclNode *> cMethods;
            if (auto *cDecl = llvm::dyn_cast<ClassDeclNode>(recDecl))
              cMethods = cDecl->methods;
            else if (auto *sDecl = llvm::dyn_cast<StructDeclNode>(recDecl))
              cMethods = sDecl->methods;
            else if (auto *uDecl = llvm::dyn_cast<UnionDeclNode>(recDecl))
              cMethods = uDecl->methods;

            llvm::SmallVector<const DeclNode *, 2> staticDecls;
            for (auto *m : cMethods) {
              if (m->isStatic && m->name == name) {
                staticDecls.push_back(m);
              }
            }
            if (!staticDecls.empty()) {
              for (auto *d : decls)
                staticDecls.push_back(d);
              decls = staticDecls;
            }
          }
        }
      }

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
        if (d->kind == NodeKind::FunctionDecl ||
            d->kind == NodeKind::ClassDecl || d->kind == NodeKind::StructDecl ||
            d->kind == NodeKind::UnionDecl) {
          hasCallable = true;
          break;
        }
      }

      if (hasCallable) {
        const FunctionDeclNode *bestMatch = nullptr;
        int bestScore = std::numeric_limits<int>::min();
        bool isConstructorCall = false;
        const RecordType *constructorRecTy = nullptr;
        std::vector<std::vector<std::string>> overloadErrors;
        std::vector<ExprNode *> bestResolvedArgs;

        for (auto targetDecl : decls) {
          if (targetDecl->kind == NodeKind::ClassDecl ||
              targetDecl->kind == NodeKind::StructDecl ||
              targetDecl->kind == NodeKind::UnionDecl) {
            const RecordType *recTy = nullptr;
            if (targetDecl->kind == NodeKind::ClassDecl)
              recTy =
                  static_cast<const ClassDeclNode *>(targetDecl)->recordType;
            else if (targetDecl->kind == NodeKind::StructDecl)
              recTy =
                  static_cast<const StructDeclNode *>(targetDecl)->recordType;
            else
              recTy =
                  static_cast<const UnionDeclNode *>(targetDecl)->recordType;

            llvm::ArrayRef<FunctionDeclNode *> ctors;
            if (targetDecl->kind == NodeKind::ClassDecl)
              ctors =
                  static_cast<const ClassDeclNode *>(targetDecl)->constructors;
            else if (targetDecl->kind == NodeKind::StructDecl)
              ctors =
                  static_cast<const StructDeclNode *>(targetDecl)->constructors;
            else
              ctors =
                  static_cast<const UnionDeclNode *>(targetDecl)->constructors;

            for (auto *ctor : ctors) {
              if (!ctor->isPublic(ctor->name) &&
                  ctx->getCurrentRecordContext() != recTy) {
                overloadErrors.push_back({"Constructor is private."});
                continue;
              }

              int score = 0;
              std::vector<ExprNode *> resolvedArgs;
              auto errs = checkMatch(ctor, score, resolvedArgs);
              if (errs.empty()) {
                if (score > bestScore) {
                  bestScore = score;
                  bestMatch = ctor;
                  bestResolvedArgs = resolvedArgs;
                  isConstructorCall = true;
                  constructorRecTy = recTy;
                  overloadErrors.clear();
                }
              } else {
                overloadErrors.push_back(errs);
              }
            }
          } else if (targetDecl->kind == NodeKind::FunctionDecl) {
            auto fDecl = static_cast<const FunctionDeclNode *>(targetDecl);

            /* Memory.construct<T>(ptr, args...) called unqualified (via
             * 'using Memory;'): the argument checking is call-site specific,
             * so intercept before the generic signature match. */
            if (fDecl->isIntrinsic &&
                fDecl->intrinsicName == "memory_construct") {
              auto ctorRes =
                  checkMemoryConstruct(node, varTarget->templateArgs);
              if (ctorRes) {
                bestScore = std::numeric_limits<int>::max();
                bestMatch = fDecl;
                overloadErrors.clear();
              }
              continue;
            }

            if (!fDecl->isPublic(fDecl->name)) {
              if (fDecl->isMethod && !fDecl->isExtern) {
                const RecordType *recTy = nullptr;
                if (fDecl->parentRecord) {
                  recTy = fDecl->parentRecord;
                } else {
                  recTy = ctx->astCtx.getRecordType(name);
                }
                if (recTy && ctx->getCurrentRecordContext() != recTy) {
                  overloadErrors.push_back({"Method is private."});
                  continue;
                }
              } else {
                if (fDecl->declFilePath != ctx->currentFile) {
                  overloadErrors.push_back(
                      {"Function is private to its file."});
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

                isConstructorCall = false;
                constructorRecTy = nullptr;

                if (fDecl->isMethod && fDecl->parentRecord) {
                  std::string_view recName = fDecl->parentRecord->getName();
                  size_t dotPos = recName.find_last_of('.');
                  std::string_view simpleRecName =
                      (dotPos != std::string_view::npos)
                          ? recName.substr(dotPos + 1)
                          : recName;
                  if (fDecl->name == simpleRecName || fDecl->isNamedCtor) {
                    isConstructorCall = true;
                    constructorRecTy = fDecl->parentRecord;
                  }
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

          if (auto lambdaRes = resolveLambdaArgs(bestMatch, bestResolvedArgs);
              !lambdaRes) {
            return lambdaRes;
          }

          if (isConstructorCall) {
            node->exprType = constructorRecTy;
            node->isLValue = false;
          } else {
            node->exprType = resolveIfTemplate(
                bestMatch->effectiveReturnType
                    ? bestMatch->effectiveReturnType
                    : bestMatch->returnType);
            node->isLValue = (node->exprType->isReferenceType());
          }

          /* Implicit const (Dart 2 rule): inside a const context, invoking
           * a const constructor produces a canonical const object. */
          if (isConstructorCall && ctx->constContextDepth > 0 &&
              bestMatch->isConst) {
            std::string key;
            if (!this->constEvaluate(node, true, key)) {
              return ctx->reportError(
                  node->line, node->column, node->length,
                  "Arguments of a const object creation must be constant "
                  "expressions.");
            }
          }

          if (node->target->kind == NodeKind::Variable) {
            const_cast<VariableNode *>(
                static_cast<const VariableNode *>(node->target))
                ->resolvedDecl = bestMatch;
          }

          checkDeprecated(bestMatch, node);
          emitRValueRefWarnings(bestMatch, bestResolvedArgs);

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
  }

  if (node->target->kind == NodeKind::MemberAccess) {
    auto ma = static_cast<const MemberAccessNode *>(node->target);
    auto maRes = dispatch(ma);
    if (!maRes)
      return maRes;

    /* Namespace-qualified template functions called without explicit type
     * arguments ('std.findMax(100, 250)'): the member access resolves to the
     * raw template (function-pointer type); deduce the arguments from the
     * call and re-resolve so the instantiated function is used. */
    if (ma->templateArgs.empty() && ma->resolvedDecl &&
        ma->resolvedDecl->kind == NodeKind::FunctionDecl &&
        ma->resolvedDecl->isTemplate) {
      auto *tfn = static_cast<const FunctionDeclNode *>(ma->resolvedDecl);
      if (!(tfn->isIntrinsic && tfn->intrinsicName == "memory_construct")) {
        std::vector<const Type *> deducedArgs;
        std::string dedErr;
        if (deduceTemplateArguments(tfn, argTypes, node->argNames, {},
                                    deducedArgs, dedErr)) {
          if (!checkTemplateConstraints(tfn, deducedArgs, node)) {
            return std::unexpected(ErrorInfo{
                node->line, node->column, node->length,
                "Template constraints are not satisfied"});
          }
          const_cast<MemberAccessNode *>(ma)->templateArgs =
              ctx->astCtx.copyArray<const Type *>(deducedArgs);
          auto reRes = dispatch(ma);
          if (!reRes)
            return reRes;
        } else {
          return ctx->reportError(
              node->line, node->column, node->length,
              "Could not deduce template arguments for '" +
                  std::string(ma->memberName) + "': " + dedErr + ".");
        }
      }
    }

    if (ma->isMethodRef) {
      const RecordType *recordTy = nullptr;
      const DeclNode *recDecl = nullptr;

      if (ma->object->kind == NodeKind::Variable) {
        auto varNode = static_cast<const VariableNode *>(ma->object);
        if (varNode->resolvedDecl &&
            (varNode->resolvedDecl->kind == NodeKind::ClassDecl ||
             varNode->resolvedDecl->kind == NodeKind::StructDecl ||
             varNode->resolvedDecl->kind == NodeKind::UnionDecl)) {
          recDecl = varNode->resolvedDecl;
        }
      } else if (ma->object->kind == NodeKind::MemberAccess) {
        auto maObj = static_cast<const MemberAccessNode *>(ma->object);
        if (maObj->resolvedDecl &&
            (maObj->resolvedDecl->kind == NodeKind::ClassDecl ||
             maObj->resolvedDecl->kind == NodeKind::StructDecl ||
             maObj->resolvedDecl->kind == NodeKind::UnionDecl)) {
          recDecl = maObj->resolvedDecl;
        }
      }

      if (recDecl) {
        if (recDecl->kind == NodeKind::ClassDecl)
          recordTy = static_cast<const ClassDeclNode *>(recDecl)->recordType;
        else if (recDecl->kind == NodeKind::StructDecl)
          recordTy = static_cast<const StructDeclNode *>(recDecl)->recordType;
        else if (recDecl->kind == NodeKind::UnionDecl)
          recordTy = static_cast<const UnionDeclNode *>(recDecl)->recordType;
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
            unqualBaseTy->getKind() == TypeKind::Class ||
            unqualBaseTy->getKind() == TypeKind::Union) {
          recordTy = static_cast<const RecordType *>(unqualBaseTy);
          auto recordDecls = ctx->lookup(recordTy->getName());

          for (auto *d : recordDecls) {
            if (d->kind == NodeKind::ClassDecl ||
                d->kind == NodeKind::StructDecl ||
                d->kind == NodeKind::UnionDecl) {
              recDecl = d;
              break;
            }
          }

          if (!recDecl) {
            recDecl = recordTy->getDeclaration();
          }
        }
      }

      if (recDecl) {
        const FunctionDeclNode *bestMatch = nullptr;
        int bestScore = std::numeric_limits<int>::min();
        std::vector<std::vector<std::string>> overloadErrors;
        std::vector<ExprNode *> bestResolvedArgs;

        /* Collect methods from the whole hierarchy (most-derived first,
         * then base classes and interfaces): inherited methods must be
         * callable on derived-typed receivers, not only through base
         * pointers or 'super'. Private base/interface methods are only
         * visible from their declaring file, mirroring the direct-call
         * rule (the receiver's own methods keep their existing rules). */
        std::vector<const FunctionDeclNode *> allMethods;
        if (recDecl->kind == NodeKind::ClassDecl) {
          const ClassDeclNode *curClass =
              static_cast<const ClassDeclNode *>(recDecl);
          bool isRoot = true;
          while (curClass) {
            for (const auto *m : curClass->methods) {
              if (!isRoot && !m->isPublic(m->name) &&
                  !m->isProtected(m->name) &&
                  m->declFilePath != ctx->currentFile)
                continue;
              allMethods.push_back(m);
            }
            for (const auto *iface : curClass->interfaces) {
              if (const auto *iType = llvm::dyn_cast<ClassType>(
                      iface->getUnqualifiedType())) {
                const ClassDeclNode *ifaceDecl =
                    llvm::dyn_cast_or_null<ClassDeclNode>(
                        iType->getDeclaration());
                while (ifaceDecl) {
                  for (const auto *m : ifaceDecl->methods) {
                    if (!m->isPublic(m->name) && !m->isProtected(m->name) &&
                        m->declFilePath != ctx->currentFile)
                      continue;
                    allMethods.push_back(m);
                  }
                  if (ifaceDecl->baseClass) {
                    if (const auto *pType = llvm::dyn_cast<ClassType>(
                            ifaceDecl->baseClass->getUnqualifiedType())) {
                      ifaceDecl =
                          llvm::dyn_cast_or_null<ClassDeclNode>(
                              pType->getDeclaration());
                    } else {
                      break;
                    }
                  } else {
                    break;
                  }
                }
              }
            }
            if (curClass->baseClass) {
              if (const auto *pType = llvm::dyn_cast<ClassType>(
                      curClass->baseClass->getUnqualifiedType())) {
                curClass = llvm::dyn_cast_or_null<ClassDeclNode>(
                    pType->getDeclaration());
                isRoot = false;
              } else {
                break;
              }
            } else {
              break;
            }
          }
        } else if (recDecl->kind == NodeKind::StructDecl) {
          for (const auto *m :
               static_cast<const StructDeclNode *>(recDecl)->methods) {
            allMethods.push_back(m);
          }
        } else {
          for (const auto *m :
               static_cast<const UnionDeclNode *>(recDecl)->methods) {
            allMethods.push_back(m);
          }
        }

        for (const auto *method : allMethods) {
          if (method->name == ma->memberName) {
            /* Method-level templates: full explicit type arguments are
             * applied as-is; otherwise the arguments are deduced from the
             * call (C++-style deduction, e.g. 'box.wrap(5)' deduces
             * T = int32). When deduction fails, a fallback binds the
             * method's template parameters positionally to the enclosing
             * record's template arguments ('Future<int>.value(5)' binds
             * R = int). */
            const FunctionDeclNode *candidate = method;
            if (method->isTemplate && recordTy) {
              candidate = nullptr;
              if (!ma->templateArgs.empty() &&
                  ma->templateArgs.size() == method->templateParams.size()) {
                /* Explicit type arguments ('Future.delayed<void>(...)') are
                 * applied to every matching overload so the usual overload
                 * resolution picks the right signature. */
                candidate = instantiateMethodTemplate(method, recordTy,
                                                      ma->templateArgs);
              } else {
                std::vector<const Type *> deducedArgs;
                std::string dedErr;
                if (deduceTemplateArguments(method, argTypes, node->argNames,
                                            ma->templateArgs, deducedArgs,
                                            dedErr)) {
                  const RecordType *targetRec = recordTy;
                  /* Static method templates on an uninstantiated template
                   * record ('Future.delayed(10, fn)'): the deduced method
                   * arguments also bind the record's own template
                   * parameters, mirroring the language idiom where
                   * 'Future<T>.delayed(...)' sets both (the method's
                   * parameters and the record's). The record must be
                   * instantiated so the method body resolves its members. */
                  if (recDecl && recDecl->isTemplate &&
                      !recordTy->isTemplateInstantiation() &&
                      recDecl->templateParams.size() ==
                          method->templateParams.size()) {
                    std::string_view baseName = recDecl->fqName;
                    if (baseName.empty()) {
                      if (auto *c = llvm::dyn_cast<ClassDeclNode>(recDecl))
                        baseName = c->name;
                      else if (auto *s = llvm::dyn_cast<StructDeclNode>(recDecl))
                        baseName = s->name;
                      else if (auto *u = llvm::dyn_cast<UnionDeclNode>(recDecl))
                        baseName = u->name;
                    }
                    auto argsRef =
                        ctx->astCtx.copyArray<const Type *>(deducedArgs);
                    const Type *inst = resolveIfTemplate(
                        ctx->astCtx.getTemplateInstType(baseName, argsRef));
                    if (inst) {
                      const Type *u = inst->getUnqualifiedType();
                      if (u->getKind() == TypeKind::Class ||
                          u->getKind() == TypeKind::Struct ||
                          u->getKind() == TypeKind::Union) {
                        targetRec = static_cast<const RecordType *>(u);
                      }
                    }
                  }
                  candidate = instantiateMethodTemplate(method, targetRec,
                                                        deducedArgs);
                } else if (ma->templateArgs.empty() &&
                           recordTy->isTemplateInstantiation() &&
                           method->templateParams.size() ==
                               recordTy->getTemplateArgs().size()) {
                  /* Bind the method's template parameters positionally to the
                   * enclosing record's template arguments. */
                  candidate = instantiateMethodTemplate(method, recordTy,
                                                        ma->templateArgs);
                }
              }
            }
            if (!candidate)
              continue;

            int score = 0;
            std::vector<ExprNode *> resolvedArgs;
            auto errs = checkMatch(candidate, score, resolvedArgs);
            if (errs.empty()) {
              if (score > bestScore) {
                bestScore = score;
                bestMatch = candidate;
                bestResolvedArgs = resolvedArgs;
                overloadErrors.clear();
              }
            } else {
              overloadErrors.push_back(errs);
            }
          }
        }
        /* Dart-style named constructor: 'Foo.named(args)'. Constructors
         * live outside the method tables, so resolve them explicitly when
         * the receiver is a class reference. */
        if (recDecl && recDecl->kind == NodeKind::ClassDecl) {
          bool staticClassRef = false;
          if (auto *v = llvm::dyn_cast<VariableNode>(ma->object))
            staticClassRef = v->resolvedDecl == recDecl;
          else if (auto *m = llvm::dyn_cast<MemberAccessNode>(ma->object))
            staticClassRef = m->resolvedDecl == recDecl;
          if (staticClassRef) {
            for (const auto *ctor :
                 static_cast<const ClassDeclNode *>(recDecl)->constructors) {
              if (!ctor->isNamedCtor || ctor->name != ma->memberName)
                continue;
              int score = 0;
              std::vector<ExprNode *> resolvedArgs;
              auto errs = checkMatch(ctor, score, resolvedArgs);
              if (errs.empty()) {
                if (score > bestScore) {
                  bestScore = score;
                  bestMatch = ctor;
                  bestResolvedArgs = resolvedArgs;
                  overloadErrors.clear();
                }
              } else {
                overloadErrors.push_back(errs);
              }
            }
          }
        }
        if (bestMatch) {
          const_cast<MemberAccessNode *>(ma)->resolvedMethod = bestMatch;
          const_cast<FunctionCallNode *>(node)->resolvedFunc = bestMatch;

          const_cast<FunctionCallNode *>(node)->args =
              ctx->astCtx.copyArray<ExprNode *>(bestResolvedArgs);
          const_cast<FunctionCallNode *>(node)->argNames = {};

          if (auto lambdaRes = resolveLambdaArgs(bestMatch, bestResolvedArgs);
              !lambdaRes) {
            return lambdaRes;
          }

          if (bestMatch->isNamedCtor) {
            const Type *recTy = nullptr;
            if (auto *c = llvm::dyn_cast<ClassDeclNode>(recDecl))
              recTy = c->recordType;
            node->exprType = recTy;
            node->isLValue = false;
          } else {
            node->exprType = resolveIfTemplate(
                bestMatch->effectiveReturnType
                    ? bestMatch->effectiveReturnType
                    : bestMatch->returnType);
            node->isLValue = (node->exprType->isReferenceType());
          }

          /* Implicit const for named constructor calls (Dart 2 rule). */
          if (bestMatch->isNamedCtor && ctx->constContextDepth > 0 &&
              bestMatch->isConst) {
            std::string key;
            if (!this->constEvaluate(node, true, key)) {
              return ctx->reportError(
                  node->line, node->column, node->length,
                  "Arguments of a const object creation must be constant "
                  "expressions.");
            }
          }

          checkDeprecated(bestMatch, node);
          emitRValueRefWarnings(bestMatch, bestResolvedArgs);

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
    } else if (ma->resolvedDecl &&
               (ma->resolvedDecl->kind == NodeKind::ClassDecl ||
                ma->resolvedDecl->kind == NodeKind::StructDecl ||
                ma->resolvedDecl->kind == NodeKind::UnionDecl)) {
      const RecordType *recordTy = nullptr;
      if (ma->resolvedDecl->kind == NodeKind::ClassDecl)
        recordTy =
            static_cast<const ClassDeclNode *>(ma->resolvedDecl)->recordType;
      else if (ma->resolvedDecl->kind == NodeKind::StructDecl)
        recordTy =
            static_cast<const StructDeclNode *>(ma->resolvedDecl)->recordType;
      else
        recordTy =
            static_cast<const UnionDeclNode *>(ma->resolvedDecl)->recordType;

      llvm::ArrayRef<FunctionDeclNode *> ctors;
      if (ma->resolvedDecl->kind == NodeKind::ClassDecl)
        ctors =
            static_cast<const ClassDeclNode *>(ma->resolvedDecl)->constructors;
      else if (ma->resolvedDecl->kind == NodeKind::StructDecl)
        ctors =
            static_cast<const StructDeclNode *>(ma->resolvedDecl)->constructors;
      else
        ctors =
            static_cast<const UnionDeclNode *>(ma->resolvedDecl)->constructors;

      const FunctionDeclNode *bestMatch = nullptr;
      int bestScore = std::numeric_limits<int>::min();
      std::vector<std::vector<std::string>> overloadErrors;
      std::vector<ExprNode *> bestResolvedArgs;

      for (const auto *ctor : ctors) {
        if (!ctor->isPublic(ctor->name) &&
            ctx->getCurrentRecordContext() != recordTy) {
          overloadErrors.push_back({"Constructor is private."});
          continue;
        }

        int score = 0;
        std::vector<ExprNode *> resolvedArgs;
        auto errs = checkMatch(ctor, score, resolvedArgs);
        if (errs.empty()) {
          if (score > bestScore) {
            bestScore = score;
            bestMatch = ctor;
            bestResolvedArgs = resolvedArgs;
            overloadErrors.clear();
          }
        } else {
          overloadErrors.push_back(errs);
        }
      }

      if (bestMatch) {
        const_cast<FunctionCallNode *>(node)->resolvedFunc = bestMatch;
        const_cast<FunctionCallNode *>(node)->args =
            ctx->astCtx.copyArray<ExprNode *>(bestResolvedArgs);
        const_cast<FunctionCallNode *>(node)->argNames = {};

        if (auto lambdaRes = resolveLambdaArgs(bestMatch, bestResolvedArgs);
            !lambdaRes) {
          return lambdaRes;
        }

        node->exprType = recordTy;
        node->isLValue = false;
        checkDeprecated(bestMatch, node);
        emitRValueRefWarnings(bestMatch, bestResolvedArgs);

        return node->exprType;
      }

      std::string finalErr = "No matching constructor found for '" +
                             std::string(recordTy->getName()) + "'.";
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
    } else if (ma->resolvedDecl &&
               ma->resolvedDecl->kind == NodeKind::FunctionDecl) {
      const_cast<FunctionCallNode *>(node)->resolvedFunc =
          static_cast<const FunctionDeclNode *>(ma->resolvedDecl);

      /* Memory.construct<T>(ptr, args...) / Memory.destruct(ptr) are
       * compiler intrinsics with call-site-specific argument checking;
       * intercept them before the generic function-pointer path. */
      if (node->resolvedFunc->isIntrinsic) {
        if (node->resolvedFunc->intrinsicName == "memory_construct") {
          return checkMemoryConstruct(node, ma->templateArgs);
        }
        if (node->resolvedFunc->intrinsicName == "memory_destruct") {
          return checkMemoryDestruct(node);
        }
      }
    }
  }

  auto targetTyRes = dispatch(node->target);
  if (targetTyRes) {
    const Type *unqual = (*targetTyRes)->getUnqualifiedType();
    if (unqual->isPointerType()) {
      const Type *pointee =
          static_cast<const PointerType *>(unqual)->getPointeeType();
      if (pointee->getKind() == TypeKind::Function) {
        auto fTy = static_cast<const FunctionType *>(pointee);

        /* A lambda used as a direct call target has no signature context, so
         * its untyped parameters are inferred from the call's arguments. */
        if (node->target->kind == NodeKind::Lambda &&
            static_cast<const LambdaNode *>(node->target)->unresolved) {
          auto inferredFnTy = ctx->astCtx.getFunctionType(
              ctx->astCtx.AutoTy,
              ctx->astCtx.copyArray<const Type *>(argTypes));
          ctx->pushExpectedFunctionType(
              ctx->astCtx.getPointerType(inferredFnTy));
          auto targetRes = dispatch(node->target);
          ctx->popExpectedFunctionType();

          const Type *resolvedUnqual =
              node->target->exprType
                  ? node->target->exprType->getUnqualifiedType()
                  : nullptr;
          if (!targetRes || !resolvedUnqual ||
              !resolvedUnqual->isPointerType() ||
              static_cast<const PointerType *>(resolvedUnqual)
                      ->getPointeeType()
                      ->getKind() != TypeKind::Function) {
            return ctx->reportError(
                node->line, node->column, node->length,
                "Failed to infer lambda parameter types from call arguments.");
          }
          pointee = static_cast<const PointerType *>(resolvedUnqual)
                        ->getPointeeType();
          fTy = static_cast<const FunctionType *>(pointee);
        }

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
              const_cast<ExprNode *>(node->args[i]), fTy->getParamTypes()[i]));
        }

        if (auto lambdaRes = resolveLambdaArgs(fTy, resolvedArgs); !lambdaRes) {
          return lambdaRes;
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
  bool isSrcBool = srcUnqual->getKind() == TypeKind::Builtin &&
                   static_cast<const BuiltinType *>(srcUnqual)
                       ->getBuiltinKind() == BuiltinKind::Bool;
  bool isDestBool = destUnqual->getKind() == TypeKind::Builtin &&
                    static_cast<const BuiltinType *>(destUnqual)
                        ->getBuiltinKind() == BuiltinKind::Bool;
  bool isSrcPtr = srcUnqual->isPointerType();
  bool isDestPtr = destUnqual->isPointerType();
  bool isSrcEnum = srcUnqual->getKind() == TypeKind::Enum;
  bool isDestEnum = destUnqual->getKind() == TypeKind::Enum;

  if ((isSrcNumeric && isDestNumeric) || (isSrcPtr && isDestPtr) ||
      (isSrcPtr && isDestNumeric) || (isSrcNumeric && isDestPtr) ||
      (isSrcEnum && isDestNumeric) || (isSrcNumeric && isDestEnum) ||
      (isSrcEnum && isDestEnum) || (isSrcBool && isDestNumeric) ||
      (isSrcNumeric && isDestBool) || (isSrcBool && isDestBool) ||
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
   * exists. The most specific constructor (exact type match first, then
   * closest width / signedness) is selected. */
  if (destUnqual->getKind() == TypeKind::Class ||
      destUnqual->getKind() == TypeKind::Struct ||
      destUnqual->getKind() == TypeKind::Union) {
    auto *recTy = static_cast<const RecordType *>(destUnqual);
    if (const FunctionDeclNode *ctor =
            findBestConversionCtor(*srcType, recTy)) {
      if (!ctor->isPublic(ctor->name) &&
          ctx->getCurrentRecordContext() != recTy) {
        return ctx->reportError(
            node->line, node->column, node->length,
            "Cannot cast to '" + destType->toString() +
                "'. Conversion constructor is private.");
      }
      const_cast<CastNode *>(node)->conversionConstructor = ctor;
      node->exprType = destType;
      node->isLValue = false;
      return destType;
    }
  }

  return ctx->reportError(node->line, node->column, node->length,
                          "Invalid cast: unsupported type conversion");
}

SemaResult TypeCheckPass::visit(const IsExprNode *node) {
  const_cast<IsExprNode *>(node)->targetType =
      resolveIfTemplate(node->targetType);

  /* An unresolved target ('x is UndefinedType') would otherwise compile to
   * a constant 'false' with no diagnostic: report it as a real error. A
   * class-constrained template parameter ('x is T' with 'T extends Animal')
   * is erased to its constraint bound, like Dart. */
  if (node->targetType->getKind() == TypeKind::TemplateParam) {
    auto *paramTy =
        static_cast<const TemplateParamType *>(node->targetType);
    const ClassType *constraintClass =
        findClassTemplateConstraint(paramTy->getName());
    if (!constraintClass) {
      return ctx->reportError(node->line, node->column, node->length,
                              "Unknown type in 'is' expression: '" +
                                  node->targetType->toString() + "'");
    }
    const_cast<IsExprNode *>(node)->targetType = constraintClass;
  }

  auto srcRes = dispatch(node->expr);
  if (!srcRes) {
    return std::unexpected(ErrorInfo{node->line, node->column, node->length,
                                     "Cascading error in 'is' expression"});
  }

  const Type *srcType = (*srcRes)->getUnqualifiedType();
  const Type *target = node->targetType->getUnqualifiedType();

  /* Strip pointer/reference indirection to reach the operand's record type.
   * The indirection is preserved in the promotion type so the narrowed
   * variable keeps working in member access and calls. */
  const Type *operandBase = srcType;
  bool isIndirect = false;
  const Type *promotedTy = node->targetType;
  if (operandBase->isPointerType()) {
    operandBase = static_cast<const PointerType *>(operandBase)
                      ->getPointeeType()
                      ->getUnqualifiedType();
    isIndirect = true;
    promotedTy = ctx->astCtx.getPointerType(node->targetType);
  } else if (operandBase->isReferenceType()) {
    operandBase = static_cast<const ReferenceType *>(operandBase)
                      ->getPointeeType()
                      ->getUnqualifiedType();
    isIndirect = true;
    promotedTy = ctx->astCtx.getReferenceType(node->targetType);
  } else if (operandBase->getKind() == TypeKind::RValueReference) {
    operandBase = static_cast<const RValueReferenceType *>(operandBase)
                      ->getPointeeType()
                      ->getUnqualifiedType();
    isIndirect = true;
    promotedTy = ctx->astCtx.getRValueReferenceType(node->targetType);
  }

  bool srcIsClass = operandBase->getKind() == TypeKind::Class;
  bool targetIsClass = target->getKind() == TypeKind::Class;

  int result = -1; /* -1: runtime check required */
  if (srcIsClass && targetIsClass) {
    const ClassType *srcClass = static_cast<const ClassType *>(operandBase);
    const ClassType *tgtClass = static_cast<const ClassType *>(target);
    if (isSubtypeClass(srcClass, tgtClass)) {
      /* The static type is already compatible: always true. */
      result = 1;
    } else if (isSubtypeClass(tgtClass, srcClass)) {
      /* The object's dynamic type may be a subclass: inspect it at runtime. */
      if (!isIndirect) {
        /* A by-value object's dynamic type is exactly its static type. */
        result = 0;
      } else if (!srcClass->getIsPolymorphic()) {
        return ctx->reportError(
            node->line, node->column, node->length,
            "Cannot test '" + (*srcRes)->toString() + "' with 'is " +
                node->targetType->toString() +
                "': the class is not polymorphic, so the dynamic type cannot "
                "be inspected at runtime. Mark a method '@virtual' or add an "
                "interface to make it polymorphic.");
      } else {
        const_cast<IsExprNode *>(node)->operandClassType = srcClass;
      }
    } else if (tgtClass->getIsAbstract() && isIndirect &&
               srcClass->getIsPolymorphic()) {
      /* Interface target: the static class does not implement it, but a
       * subclass of the static type may, so the dynamic type decides. */
      const_cast<IsExprNode *>(node)->operandClassType = srcClass;
    } else {
      /* Unrelated types (a class can never be a sibling class): false. */
      result = 0;
    }
  } else {
    /* Non-class types carry no runtime identity: the answer is static. */
    result = ctx->isSameType(operandBase, target) ? 1 : 0;
  }

  if (result >= 0) {
    const_cast<IsExprNode *>(node)->staticResult =
        node->isNegated ? 1 - result : result;
  }

  if (node->expr->kind == NodeKind::Variable) {
    const_cast<IsExprNode *>(node)->promotionType = promotedTy;
  }

  node->exprType = ctx->astCtx.BoolTy;
  node->isLValue = false;
  return ctx->astCtx.BoolTy;
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

  bool pushedExpected = false;
  if (const Type *expFn = getExpectedFunctionPtrType(expectedRet)) {
    ctx->pushExpectedFunctionType(expFn);
    pushedExpected = true;
  }

  auto valType = dispatch(node->value);

  if (pushedExpected)
    ctx->popExpectedFunctionType();

  if (!valType)
    return std::unexpected(ErrorInfo{node->line, node->column, node->length,
                                     "Cascading error in return expression"});

  if (expectedRet->isReferenceType()) {
    if (!node->value->isLValue) {
      return ctx->reportError(node->line, node->column, node->length,
                              "Cannot return a non-lvalue as a reference.");
    }
  }

  if ((expectedRet->isVoid() && !(*valType)->isVoid()) ||
      (!expectedRet->isVoid() && !canImplicitlyCast(*valType, expectedRet))) {
    /* Async functions may return a Future<T> of their value type
     * ('return fut;'); the codegen awaits it implicitly. */
    const FunctionDeclNode *cur = ctx->getCurrentFunction();
    const Type *inner = nullptr;
    if (cur && cur->isAsync && unwrapFutureType(*valType, &inner) &&
        canImplicitlyCast(inner, expectedRet)) {
      const_cast<ReturnNode *>(node)->implicitAwait = true;
      const_cast<ReturnNode *>(node)->value =
          performImplicitConversion(node->value, expectedRet);
      return *valType;
    }
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
        (void)ctx->reportError(elem->line, elem->column, elem->length,
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

SemaResult TypeCheckPass::visit(const MapLiteralNode *node) {
  const Type *keyType = nullptr;
  const Type *valueType = nullptr;
  bool hasErrors = false;

  if (node->keys.size() != node->values.size()) {
    return ctx->reportError(node->line, node->column, node->length,
                            "Map literal has mismatched keys and values.");
  }

  auto unifyTypes = [&](const Type *&unified, const Type *candidate,
                        const ASTNode *at, std::string_view what) {
    if (!unified) {
      unified = candidate;
    } else if (!canImplicitlyCast(candidate, unified)) {
      if (canImplicitlyCast(unified, candidate)) {
        unified = candidate;
      } else {
        (void)ctx->reportError(at->line, at->column, at->length,
                               "Map literal " + std::string(what) +
                                   " type mismatch: '" +
                                   unified->toString() + "' vs '" +
                                   candidate->toString() + "'.");
        hasErrors = true;
      }
    }
  };

  for (size_t i = 0; i < node->keys.size(); i++) {
    auto keyRes = dispatch(node->keys[i]);
    if (!keyRes) {
      hasErrors = true;
    } else {
      unifyTypes(keyType, *keyRes, node->keys[i], "key");
    }
    auto valRes = dispatch(node->values[i]);
    if (!valRes) {
      hasErrors = true;
    } else {
      unifyTypes(valueType, *valRes, node->values[i], "value");
    }
  }

  if (hasErrors) {
    return std::unexpected(ErrorInfo{node->line, node->column, node->length,
                                     "Errors in map literal entries"});
  }

  if (!keyType) {
    keyType = ctx->astCtx.VoidTy;
  }
  if (!valueType) {
    valueType = ctx->astCtx.VoidTy;
  }

  std::vector<ExprNode *> promotedKeys;
  std::vector<ExprNode *> promotedValues;
  for (const auto *key : node->keys) {
    promotedKeys.push_back(
        performImplicitConversion(const_cast<ExprNode *>(key), keyType));
  }
  for (const auto *value : node->values) {
    promotedValues.push_back(
        performImplicitConversion(const_cast<ExprNode *>(value), valueType));
  }
  const_cast<MapLiteralNode *>(node)->keys =
      ctx->astCtx.copyArray<ExprNode *>(promotedKeys);
  const_cast<MapLiteralNode *>(node)->values =
      ctx->astCtx.copyArray<ExprNode *>(promotedValues);

  const Type *mapType =
      ctx->astCtx.getMapLiteralType(keyType, valueType, node->keys.size());
  node->exprType = mapType;
  return mapType;
}

SemaResult TypeCheckPass::visit(const ModuleNode *node) {
  if (visitedModules.contains(node))
    return ctx->astCtx.VoidTy;
  visitedModules.insert(node);

  size_t prevUsings = ctx->getUsingsCount();

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

  ctx->resizeUsings(prevUsings);

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

  /* A raw pointer base always means built-in element access ('arr[i]');
   * the pointee's operator[] must not hijack it through pointer auto-deref. */
  if (!(*baseType)->getUnqualifiedType()->isPointerType()) {
    if (auto *opDecl =
            resolveOverloadedOperator(*baseType, "[]", {node->index})) {
      if (!opDecl->isPublic(opDecl->name) &&
          ctx->getCurrentRecordContext() != opDecl->parentRecord) {
        return ctx->reportError(node->line, node->column, node->length,
                                "Cannot call private overloaded operator '[]'.");
      }
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

/* Finds a custom 'operator new' / 'operator delete' declared as a static
 * method of the record, or at file scope in the current module. */
static const FunctionDeclNode *findRecordAllocationOperator(
    const RecordType *recTy, std::string_view opName) {
  const DeclNode *decl = recTy->getDeclaration();
  if (!decl)
    return nullptr;
  llvm::ArrayRef<FunctionDeclNode *> methods;
  if (decl->kind == NodeKind::ClassDecl)
    methods = static_cast<const ClassDeclNode *>(decl)->methods;
  else if (decl->kind == NodeKind::StructDecl)
    methods = static_cast<const StructDeclNode *>(decl)->methods;
  else if (decl->kind == NodeKind::UnionDecl)
    methods = static_cast<const UnionDeclNode *>(decl)->methods;
  for (const auto *m : methods) {
    if (m->name == opName && m->isStatic && !m->isImplicit) {
      return m;
    }
  }
  return nullptr;
}

SemaResult TypeCheckPass::checkMemoryConstruct(
    const FunctionCallNode *node, llvm::ArrayRef<const Type *> templateArgs) {
  /* The destination pointer must be the first argument. */
  if (node->args.empty()) {
    return ctx->reportError(
        node->line, node->column, node->length,
        "Memory.construct requires a destination pointer as its first "
        "argument.");
  }

  const Type *ptrTy = node->args[0]->exprType;
  if (!ptrTy || !ptrTy->getUnqualifiedType()->isPointerType()) {
    return ctx->reportError(
        node->line, node->column, node->length,
        "Memory.construct requires a destination pointer as its first "
        "argument.");
  }

  /* The constructed type: explicit type argument ('Memory.construct<Item>')
   * or deduced from the destination pointer's pointee. */
  const Type *T = nullptr;
  if (!templateArgs.empty()) {
    if (templateArgs.size() != 1) {
      return ctx->reportError(node->line, node->column, node->length,
                              "Memory.construct accepts a single type "
                              "argument.");
    }
    T = resolveIfTemplate(templateArgs[0]);
  } else {
    const Type *pointee =
        static_cast<const PointerType *>(
            ptrTy->getUnqualifiedType())
            ->getPointeeType()
            ->getUnqualifiedType();
    if (pointee->isVoid()) {
      return ctx->reportError(
          node->line, node->column, node->length,
          "Memory.construct cannot deduce the constructed type from a void* "
          "destination; pass it explicitly: Memory.construct<T>(ptr, ...).");
    }
    T = pointee;
  }
  if (!T || T->isVoid()) {
    return ctx->reportError(node->line, node->column, node->length,
                            "Memory.construct requires a valid constructed "
                            "type.");
  }

  /* Lower to a placement NewExprNode: constructor overload resolution,
   * argument conversions and codegen are shared with 'new'. */
  std::vector<ExprNode *> ctorArgs(node->args.begin() + 1, node->args.end());
  llvm::ArrayRef<ExprNode *> argsRef =
      ctx->astCtx.copyArray<ExprNode *>(ctorArgs);

  llvm::ArrayRef<std::string_view> namesRef = {};
  if (node->argNames.size() == node->args.size()) {
    std::vector<std::string_view> ctorNames(node->argNames.begin() + 1,
                                            node->argNames.end());
    namesRef = ctx->astCtx.copyArray<std::string_view>(ctorNames);
  }

  auto *newNode = ctx->astCtx.create<NewExprNode>(
      T, nullptr, argsRef, namesRef, true, node->line, node->column,
      node->length);
  newNode->placementExpr = const_cast<ExprNode *>(node->args[0]);

  auto res = visit(newNode);
  if (!res) {
    return std::unexpected(ErrorInfo{node->line, node->column, node->length,
                                     "Cascading error in Memory.construct"});
  }

  const_cast<FunctionCallNode *>(node)->loweredNew = newNode;
  node->exprType = newNode->exprType;
  node->isLValue = false;
  return node->exprType;
}

SemaResult TypeCheckPass::checkMemoryDestruct(const FunctionCallNode *node) {
  if (node->args.size() != 1) {
    return ctx->reportError(
        node->line, node->column, node->length,
        "Memory.destruct expects a single pointer argument.");
  }

  const Type *argTy = node->args[0]->exprType;
  if (!argTy || !argTy->getUnqualifiedType()->isPointerType()) {
    return ctx->reportError(
        node->line, node->column, node->length,
        "Memory.destruct expects a pointer to a constructed object.");
  }

  const Type *pointee =
      static_cast<const PointerType *>(argTy->getUnqualifiedType())
          ->getPointeeType()
          ->getUnqualifiedType();
  if (pointee->isVoid()) {
    return ctx->reportError(
        node->line, node->column, node->length,
        "Memory.destruct cannot determine the destructor of a void*; pass a "
        "typed pointer.");
  }

  node->exprType = ctx->astCtx.VoidTy;
  node->isLValue = false;
  return ctx->astCtx.VoidTy;
}

SemaResult TypeCheckPass::visit(const NewExprNode *node) {
  /* Class-template argument deduction: 'new Box(42)' deduces the template
   * arguments from the constructor call; 'new Box<int>(42)' stays explicit. */
  const Type *allocU = node->allocatedType->getUnqualifiedType();
  const DeclNode *ctadRec = nullptr;
  if (allocU->getKind() != TypeKind::TemplateInst) {
    if (auto *tp = llvm::dyn_cast<TemplateParamType>(allocU)) {
      auto regIt = ctx->templateRegistry.find(tp->getName());
      if (regIt != ctx->templateRegistry.end() && regIt->second->isTemplate &&
          (regIt->second->kind == NodeKind::ClassDecl ||
           regIt->second->kind == NodeKind::StructDecl ||
           regIt->second->kind == NodeKind::UnionDecl)) {
        ctadRec = regIt->second;
      }
    } else if (auto *rec = llvm::dyn_cast<RecordType>(allocU)) {
      const DeclNode *decl = rec->getDeclaration();
      if (decl && decl->isTemplate) {
        ctadRec = decl;
      } else {
        /* Template records never receive a declaration on their uninstantiated
         * record type (DeclCollector registers them in the template registry
         * instead), so fall back to the registry by record name. */
        auto regIt = ctx->templateRegistry.find(rec->getName());
        if (regIt != ctx->templateRegistry.end() &&
            regIt->second->isTemplate &&
            (regIt->second->kind == NodeKind::ClassDecl ||
             regIt->second->kind == NodeKind::StructDecl ||
             regIt->second->kind == NodeKind::UnionDecl)) {
          ctadRec = regIt->second;
        }
      }
    }
  }
  if (ctadRec) {
    std::vector<const Type *> argTypes;
    bool hasErrors = false;
    for (const auto &arg : node->args) {
      const Type *t = arg->exprType;
      if (!t) {
        auto argType = dispatch(arg);
        if (!argType) {
          hasErrors = true;
        } else {
          t = *argType;
        }
      }
      if (t)
        argTypes.push_back(t);
    }
    if (!hasErrors) {
      std::vector<const Type *> deducedArgs;
      std::string dedErr;
      if (deduceClassTemplateArguments(ctadRec, argTypes, node->argNames,
                                       deducedArgs, dedErr)) {
        std::string_view baseName = ctadRec->fqName;
        if (baseName.empty()) {
          if (auto *c = llvm::dyn_cast<ClassDeclNode>(ctadRec))
            baseName = c->name;
          else if (auto *s = llvm::dyn_cast<StructDeclNode>(ctadRec))
            baseName = s->name;
          else if (auto *u = llvm::dyn_cast<UnionDeclNode>(ctadRec))
            baseName = u->name;
        }
        auto argsRef = ctx->astCtx.copyArray<const Type *>(deducedArgs);
        const_cast<NewExprNode *>(node)->allocatedType =
            ctx->astCtx.getTemplateInstType(baseName, argsRef);
      } else {
        return ctx->reportError(node->line, node->column, node->length,
                                dedErr + ".");
      }
    }
  }

  const_cast<NewExprNode *>(node)->allocatedType =
      resolveIfTemplate(node->allocatedType);

  if (!checkTypeVisibility(node->allocatedType, node)) {
    return std::unexpected(ErrorInfo{node->line, node->column, node->length,
                                     "Type visibility error"});
  }

  /* Construct-in-place (Memory.construct lowering): the destination may be
   * a typed pointer, void*, or any byte pointer (e.g. RawMemory.ptr). */
  if (node->placementExpr) {
    auto placementType = dispatch(node->placementExpr);
    if (!placementType) {
      return std::unexpected(ErrorInfo{node->line, node->column,
                                       node->length,
                                       "Cascading error in Memory.construct"});
    }
    const Type *placeUnqual = (*placementType)->getUnqualifiedType();
    if (!placeUnqual->isPointerType()) {
      return ctx->reportError(
          node->line, node->column, node->length,
          "Memory.construct requires a destination pointer.");
    }
  }

  /* Resolve the custom allocator / deallocator ('operator new' /
   * 'operator delete' as a static class method or at file scope). */
  {
    const Type *allocBase = node->allocatedType->getUnqualifiedType();
    while (allocBase->getKind() == TypeKind::Array) {
      allocBase = static_cast<const ArrayType *>(allocBase)
                      ->getElementType()
                      ->getUnqualifiedType();
    }

    const FunctionDeclNode *allocOp = nullptr;
    const FunctionDeclNode *deallocOp = nullptr;
    if (allocBase->getKind() == TypeKind::Class ||
        allocBase->getKind() == TypeKind::Struct ||
        allocBase->getKind() == TypeKind::Union) {
      auto *recTy = static_cast<const RecordType *>(allocBase);
      allocOp = findRecordAllocationOperator(recTy, "operator new");
      deallocOp = findRecordAllocationOperator(recTy, "operator delete");
    }

    if (!allocOp) {
      for (const auto *d : ctx->lookup("operator new")) {
        if (d->kind == NodeKind::FunctionDecl &&
            static_cast<const FunctionDeclNode *>(d)->name == "operator new") {
          allocOp = static_cast<const FunctionDeclNode *>(d);
          break;
        }
      }
    }
    if (!deallocOp) {
      for (const auto *d : ctx->lookup("operator delete")) {
        if (d->kind == NodeKind::FunctionDecl &&
            static_cast<const FunctionDeclNode *>(d)->name ==
                "operator delete") {
          deallocOp = static_cast<const FunctionDeclNode *>(d);
          break;
        }
      }
    }

    if (allocOp) {
      bool okFmt = allocOp->params.size() == 1 &&
                   allocOp->params[0]->type->getUnqualifiedType()->isInteger() &&
                   allocOp->returnType->getUnqualifiedType()->isPointerType();
      if (!okFmt) {
        return ctx->reportError(
            node->line, node->column, node->length,
            "'operator new' must be declared as "
            "'void* operator new(usize size)'.");
      }
    }
    if (deallocOp) {
      bool okFmt = deallocOp->params.size() == 1 &&
                   deallocOp->params[0]->type->getUnqualifiedType()
                       ->isPointerType() &&
                   deallocOp->returnType->getUnqualifiedType()->isVoid();
      if (!okFmt) {
        return ctx->reportError(
            node->line, node->column, node->length,
            "'operator delete' must be declared as "
            "'void operator delete(void* ptr)'.");
      }
    }

    const_cast<NewExprNode *>(node)->allocator = allocOp;
    const_cast<NewExprNode *>(node)->deallocator = deallocOp;
  }

  if (node->arraySize) {
    auto szType = dispatch(node->arraySize);
    if (!szType) {
      return std::unexpected(ErrorInfo{node->line, node->column, node->length,
                                       "Cascading error in new array size"});
    }
    if (!(*szType)->isInteger()) {
      return ctx->reportError(node->line, node->column, node->length,
                              "Array size in 'new' must be an integer type");
    }
    if (node->hasParens && !node->args.empty()) {
      return ctx->reportError(
          node->line, node->column, node->length,
          "Initializer arguments cannot be combined with an array size in "
          "'new'.");
    }

    /* Array elements of record type are default-constructed at runtime, so a
     * parameterless constructor must exist. */
    const Type *elemUnqual = node->allocatedType->getUnqualifiedType();
    if (elemUnqual->getKind() == TypeKind::Class ||
        elemUnqual->getKind() == TypeKind::Struct ||
        elemUnqual->getKind() == TypeKind::Union) {
      auto *recTy = static_cast<const RecordType *>(elemUnqual);
      if (auto *decl = recTy->getDeclaration()) {
        llvm::ArrayRef<FunctionDeclNode *> ctors;
        if (decl->kind == NodeKind::ClassDecl)
          ctors = static_cast<const ClassDeclNode *>(decl)->constructors;
        else if (decl->kind == NodeKind::StructDecl)
          ctors = static_cast<const StructDeclNode *>(decl)->constructors;
        else if (decl->kind == NodeKind::UnionDecl)
          ctors = static_cast<const UnionDeclNode *>(decl)->constructors;

        bool hasDefaultCtor = false;
        for (const auto *ctor : ctors) {
          if (ctor->params.empty()) {
            hasDefaultCtor = true;
            break;
          }
        }
        if (!hasDefaultCtor) {
          return ctx->reportError(
              node->line, node->column, node->length,
              "Array allocation of '" +
                  std::string(recTy->getName()) +
                  "' requires a default constructor.");
        }
      }
    }

    node->exprType = ctx->astCtx.getPointerType(node->allocatedType);
    return node->exprType;
  }

  const Type *unqual = node->allocatedType->getUnqualifiedType();

  if (unqual->getKind() == TypeKind::Class) {
    auto *classTy = static_cast<const ClassType *>(unqual);
    if (classTy->getIsAbstract()) {
      return ctx->reportError(node->line, node->column, node->length,
                              "Cannot instantiate abstract class '" +
                                  std::string(classTy->getName()) + "'.");
    }
  }

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
        /* Dart-style named constructor: 'new Foo.named(...)' only matches
         * the named constructor. */
        if (!node->namedCtorName.empty() &&
            (!ctor->isNamedCtor || ctor->name != node->namedCtorName))
          continue;
        if (node->namedCtorName.empty() && ctor->isNamedCtor)
          continue;
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

        /* Fill parameters the call did not provide: default expressions
         * are used verbatim, optional named parameters without a default
         * fall back to a zero/false/null value, and required parameters
         * produce a diagnostic. Mirrors the function-call matching so
         * 'new' behaves like a direct constructor call. */
        for (size_t p = 0; p < expectedParams; ++p) {
          if (resolvedArgs[p])
            continue;
          const ParamDeclNode *param = ctor->params[p];
          if (param->defaultValue) {
            auto defNode = param->defaultValue;
            if (!defNode->exprType) {
              dispatch(defNode);
            }
            resolvedArgs[p] = defNode;
            resolvedTypes[p] = defNode->exprType;
            continue;
          }
          if (param->isRequired) {
            errors.push_back("Missing required named parameter '" +
                             std::string(param->name) + "'.");
            continue;
          }
          if (!param->isNamed) {
            errors.push_back("Missing mandatory positional parameter '" +
                             std::string(param->name) + "'.");
            continue;
          }
          const Type *pType = param->type;
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

          if (explicitlyProvided[p] && typesMatchExactly(resolvedTypes[p], paramType)) {
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
                if (explicitlyProvided[p]) {
                  currentScore += 1;
                }
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
        /* Trivially copyable fallback: a record without user-defined
         * constructors or destructors accepts a single same-typed value
         * argument with an implicit bitwise copy (e.g.
         * 'Memory.construct<Point>(ptr, p)'). */
        bool hasUserCtor = false;
        for (const auto *ctor : ctors) {
          if (!ctor->isImplicit) {
            hasUserCtor = true;
            break;
          }
        }
        bool hasCustomDtor = false;
        if (decl->kind == NodeKind::ClassDecl)
          hasCustomDtor = static_cast<const ClassDeclNode *>(decl)->destructor &&
                          !static_cast<const ClassDeclNode *>(decl)
                               ->destructor->isImplicit;
        else if (decl->kind == NodeKind::StructDecl)
          hasCustomDtor =
              static_cast<const StructDeclNode *>(decl)->destructor &&
              !static_cast<const StructDeclNode *>(decl)
                   ->destructor->isImplicit;
        else if (decl->kind == NodeKind::UnionDecl)
          hasCustomDtor = static_cast<const UnionDeclNode *>(decl)->destructor &&
                          !static_cast<const UnionDeclNode *>(decl)
                               ->destructor->isImplicit;

        if (!hasUserCtor && !hasCustomDtor && node->args.size() == 1 &&
            (node->argNames.empty() || node->argNames[0].empty())) {
          const Type *argBase =
              argTypes.empty() ? nullptr : argTypes[0];
          if (argBase && argBase->isReferenceType())
            argBase = static_cast<const ReferenceType *>(argBase)
                          ->getPointeeType();
          else if (argBase &&
                   argBase->getKind() == TypeKind::RValueReference)
            argBase = static_cast<const RValueReferenceType *>(argBase)
                          ->getPointeeType();
          const Type *argUnqual =
              argBase ? argBase->getUnqualifiedType() : nullptr;
          /* Types are not guaranteed to be pointer-identical across
           * lookups; compare by kind and name. */
          bool sameType =
              argUnqual && argUnqual->getKind() == unqual->getKind();
          if (sameType &&
              (unqual->getKind() == TypeKind::Class ||
               unqual->getKind() == TypeKind::Struct ||
               unqual->getKind() == TypeKind::Union)) {
            sameType =
                static_cast<const RecordType *>(argUnqual)->getName() ==
                static_cast<const RecordType *>(unqual)->getName();
          }
          if (sameType) {
            const_cast<NewExprNode *>(node)->implicitCopyInit = true;
          }
        }
      }

      if (!bestMatch && !node->implicitCopyInit) {
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

          if (paramType->isReferenceType() && !bestResolvedArgs[p]->isLValue) {
            const Type *pointee =
                static_cast<const ReferenceType *>(paramType)->getPointeeType();
            if (!pointee->isConstQualified()) {
              ctx->reportWarning(
                  WarningKind::ImplicitCast, bestResolvedArgs[p]->line,
                  bestResolvedArgs[p]->column, bestResolvedArgs[p]->length,
                  "Binding an r-value to non-const reference parameter '" +
                      std::string(bestMatch->params[p]->name) +
                      "' will implicitly create a stack-allocated temporary.",
                  bestResolvedArgs[p]->endLine);
            }
          }

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

        if (auto lambdaRes = resolveLambdaArgs(bestMatch, bestResolvedArgs);
            !lambdaRes) {
          return lambdaRes;
        }

        /* Implicit const (Dart 2 rule): 'const x = new Foo(...)' inside a
         * const context creates a canonical const object. */
        if (ctx->constContextDepth > 0 && bestMatch->isConst) {
          std::string key;
          if (!this->constEvaluate(node, true, key)) {
            return ctx->reportError(
                node->line, node->column, node->length,
                "Arguments of a const object creation must be constant "
                "expressions.");
          }
        }
      }
    }
  }

  if (node->hasParens && !node->args.empty() &&
      baseUnqualTy->getKind() != TypeKind::Class &&
      baseUnqualTy->getKind() != TypeKind::Struct &&
      baseUnqualTy->getKind() != TypeKind::Union) {
    /* Scalar allocation with an initializer: 'new int(42)' stores the
     * argument into the object. At most one argument is allowed. */
    if (node->args.size() > 1) {
      return ctx->reportError(
          node->line, node->column, node->length,
          "'new' on a scalar type accepts at most one initializer argument.");
    }

    const Type *allocUnqual = node->allocatedType->getUnqualifiedType();
    auto argType = dispatch(node->args[0]);
    if (!argType) {
      return std::unexpected(ErrorInfo{node->line, node->column,
                                       node->length,
                                       "Cascading error in new argument"});
    }

    if (!canImplicitlyCast(*argType, allocUnqual)) {
      return ctx->reportError(
          node->line, node->column, node->length,
          "Cannot initialize '" + allocUnqual->toString() + "' with '" +
              (*argType)->toString() + "'.");
    }

    const_cast<NewExprNode *>(node)->args = ctx->astCtx.copyArray<ExprNode *>(
        performImplicitConversion(node->args[0], allocUnqual));
  }

  node->exprType = ctx->astCtx.getPointerType(node->allocatedType);
  return node->exprType;
}

SemaResult TypeCheckPass::visit(const DestructorCallNode *node) {
  auto objType = dispatch(node->object);
  if (!objType) {
    return std::unexpected(ErrorInfo{node->line, node->column, node->length,
                                     "Cascading error in destructor call"});
  }

  const Type *objUnqual = (*objType)->getUnqualifiedType();
  if (objUnqual->isPointerType()) {
    objUnqual = static_cast<const PointerType *>(objUnqual)
                    ->getPointeeType()
                    ->getUnqualifiedType();
  } else if (objUnqual->isReferenceType()) {
    objUnqual = static_cast<const ReferenceType *>(objUnqual)
                    ->getPointeeType()
                    ->getUnqualifiedType();
  }

  if (objUnqual->getKind() != TypeKind::Class &&
      objUnqual->getKind() != TypeKind::Struct &&
      objUnqual->getKind() != TypeKind::Union) {
    return ctx->reportError(node->line, node->column, node->length,
                            "Cannot call a destructor on a non-record type.");
  }

  const Type *targetUnqual = node->targetType->getUnqualifiedType();
  if (targetUnqual != objUnqual) {
    return ctx->reportError(
        node->line, node->column, node->length,
        "Destructor type mismatch: expected '" + objUnqual->toString() +
            "'.");
  }

  auto *recTy = static_cast<const RecordType *>(objUnqual);
  const DeclNode *decl = recTy->getDeclaration();
  if (!decl) {
    return ctx->reportError(node->line, node->column, node->length,
                            "Incomplete type in destructor call.");
  }

  const FunctionDeclNode *dtor = nullptr;
  if (decl->kind == NodeKind::ClassDecl)
    dtor = static_cast<const ClassDeclNode *>(decl)->destructor;
  else if (decl->kind == NodeKind::StructDecl)
    dtor = static_cast<const StructDeclNode *>(decl)->destructor;
  else if (decl->kind == NodeKind::UnionDecl)
    dtor = static_cast<const UnionDeclNode *>(decl)->destructor;

  if (!dtor || dtor->isImplicit) {
    return ctx->reportError(node->line, node->column, node->length,
                            "Type '" + objUnqual->toString() +
                                "' has no user-defined destructor.");
  }

  const_cast<DestructorCallNode *>(node)->destructor = dtor;
  node->exprType = ctx->astCtx.VoidTy;
  return node->exprType;
}

/* Dart-style const expressions */

namespace {
/* Returns the resolved constructor behind an object creation expression, or
 * null when 'expr' is not a constructor invocation. */
const FunctionDeclNode *resolveConstCtorCall(const ExprNode *expr) {
  if (auto *n = llvm::dyn_cast<NewExprNode>(expr))
    return n->resolvedConstructor;
  if (auto *call = llvm::dyn_cast<FunctionCallNode>(expr)) {
    const FunctionDeclNode *f = call->resolvedFunc;
    if (!f || !f->parentRecord)
      return nullptr;
    if (f->isNamedCtor) {
      if (auto *ma = llvm::dyn_cast<MemberAccessNode>(call->target))
        return ma->memberName == f->name ? f : nullptr;
      return nullptr;
    }
    std::string_view recName = f->parentRecord->getName();
    size_t dot = recName.find_last_of('.');
    std::string_view simple =
        (dot != std::string_view::npos) ? recName.substr(dot + 1) : recName;
    if (f->name == simple)
      return f;
  }
  return nullptr;
}

/* Serializes a number in a deterministic form (hex floats, plain decimals)
 * so canonical keys are identical across independently compiled modules. */
void serializeNumber(const NumberNode *node, std::string &out) {
  std::string numStr(node->raw);
  bool isHex = numStr.length() > 2 && numStr[0] == '0' &&
               (numStr[1] == 'x' || numStr[1] == 'X');
  if (isHex)
    numStr = numStr.substr(2);
  while (!numStr.empty()) {
    char back = numStr.back();
    if (back == 'u' || back == 'U' || back == 'l' || back == 'L')
      numStr.pop_back();
    else if (!isHex && (back == 'f' || back == 'F'))
      numStr.pop_back();
    else
      break;
  }
  if (node->isFloat) {
    char buf[64];
    double d = strtod(numStr.c_str(), nullptr);
    snprintf(buf, sizeof(buf), "f:%a", d);
    out = buf;
    return;
  }
  /* Parse as unsigned; re-interpret as signed when it fits so '-5' and
   * '5' serialize canonically. */
  unsigned long long raw = strtoull(numStr.c_str(), nullptr, isHex ? 16 : 10);
  long long sv = (long long)raw;
  if (sv >= 0 && raw <= 0x7FFFFFFFFFFFFFFFULL)
    out = "i:" + std::to_string((long long)raw);
  else
    out = "u:" + std::to_string((unsigned long long)raw);
}
} // namespace

bool TypeCheckPass::constEvaluate(const ExprNode *expr, bool inConstContext,
                                  std::string &out) {
  if (!expr)
    return false;
  out.clear();

  if (auto *ce = llvm::dyn_cast<ConstExprNode>(expr))
    return this->constEvaluate(ce->expr, true, out);

  /* Constructor invocation -> canonical const object. Implicit const
   * applies inside const contexts (Dart 2 rule). */
  if (const FunctionDeclNode *ctor = resolveConstCtorCall(expr)) {
    if (!ctor->isConst) {
      return false;
    }

    const RecordType *recTy = ctor->parentRecord;
    if (!recTy) {
      return false;
    }

    /* Canonical key: class name (template args included) | constructor
     * name | (param signature) | (evaluated argument values). The
     * constructor name separates default from named constructors so
     * identical-looking calls through different constructors stay distinct
     * (Dart canonicalization is per-constructor). */
    std::string key = "o:";
    key += std::string(recTy->getName());
    if (recTy->isTemplateInstantiation()) {
      key += "<";
      for (size_t i = 0; i < recTy->getTemplateArgs().size(); ++i) {
        if (i)
          key += ",";
        key += recTy->getTemplateArgs()[i]->toString();
      }
      key += ">";
    }
    key += "|" + std::string(ctor->name);
    key += "|(";
    for (size_t p = 0; p < ctor->params.size(); ++p) {
      if (p)
        key += ",";
      key += ctor->params[p]->type->toString();
    }
    key += ")|(";

    const auto *call = llvm::dyn_cast<FunctionCallNode>(expr);
    const auto *newNode = llvm::dyn_cast<NewExprNode>(expr);
    llvm::ArrayRef<ExprNode *> args =
        call ? call->args
             : (newNode ? newNode->args : llvm::ArrayRef<ExprNode *>());
    llvm::ArrayRef<std::string_view> argNames =
        call ? call->argNames
             : (newNode ? newNode->argNames
                        : llvm::ArrayRef<std::string_view>());

    /* Map each parameter to its argument (positional args bind params in
     * order; named args bind by name). */
    std::vector<const ExprNode *> paramArgs(ctor->params.size(), nullptr);
    size_t positional = 0;
    for (size_t i = 0; i < args.size(); ++i) {
      if (!argNames.empty() && !argNames[i].empty()) {
        for (size_t p = 0; p < ctor->params.size(); ++p)
          if (ctor->params[p]->name == argNames[i])
            paramArgs[p] = args[i];
      } else {
        if (positional < ctor->params.size())
          paramArgs[positional] = args[i];
        ++positional;
      }
    }

    /* Constructor parameters resolve to the caller's arguments while the
     * const initializer (super call, ': this.x = expr' entries) runs. */
    size_t envBase = this->constParamEnv.size();
    for (size_t p = 0; p < ctor->params.size(); ++p)
      this->constParamEnv.push_back({ctor->params[p], paramArgs[p]});

    /* Verify the super-constructor chain is const and evaluate the super
     * calls (their argument keys are needed by code generation to rebuild
     * the embedded base-class fields). */
    bool chainOk = true;
    for (const FunctionDeclNode *c = ctor; c; ) {
      if (!c->isConst) {
        chainOk = false;
        break;
      }
      if (!c->superCall)
        break;
      std::string superKey;
      if (!this->constEvaluate(c->superCall, true, superKey)) {
        chainOk = false;
        break;
      }
      const FunctionDeclNode *baseCtor = resolveConstCtorCall(c->superCall);
      if (!baseCtor) {
        chainOk = false;
        break;
      }
      c = baseCtor;
    }
    if (!chainOk) {
      this->constParamEnv.resize(envBase);
      return false;
    }

    for (size_t p = 0; p < ctor->params.size(); ++p) {
      if (p)
        key += ",";
      if (!paramArgs[p]) {
        this->constParamEnv.resize(envBase);
        return false;
      }
      std::string val;
      if (!this->constEvaluate(paramArgs[p], true, val)) {
        this->constParamEnv.resize(envBase);
        return false;
      }
      key += val;
    }
    key += ")";

    const_cast<ExprNode *>(expr)->isConstExpr = true;
    const_cast<ExprNode *>(expr)->constKey = key;
    /* A const object creation yields a pointer to the canonical instance
     * (like 'new'), matching Utopia's pointer-based class model. */
    const_cast<ExprNode *>(expr)->exprType =
        ctx->astCtx.getPointerType(ctor->parentRecord);
    ctx->astCtx.constObjectCreations[key] = expr;
    this->recordConstObjectFields(key, expr);
    this->constParamEnv.resize(envBase);
    out = key;
    return true;
  }

  if (auto *num = llvm::dyn_cast<NumberNode>(expr)) {
    serializeNumber(num, out);
    const_cast<ExprNode *>(expr)->isConstExpr = true;
    const_cast<ExprNode *>(expr)->constKey = out;
    return true;
  }
  if (auto *boolNode = llvm::dyn_cast<BoolNode>(expr)) {
    out = boolNode->value ? "b:1" : "b:0";
    const_cast<ExprNode *>(expr)->isConstExpr = true;
    const_cast<ExprNode *>(expr)->constKey = out;
    return true;
  }
  if (auto *strNode = llvm::dyn_cast<StringNode>(expr)) {
    std::string s = "s:";
    for (char ch : strNode->value) {
      if (ch == '\\' || ch == ':')
        s += '\\';
      s += ch;
    }
    out = s;
    const_cast<ExprNode *>(expr)->isConstExpr = true;
    const_cast<ExprNode *>(expr)->constKey = out;
    return true;
  }
  if (llvm::isa<NullNode>(expr)) {
    out = "n";
    const_cast<ExprNode *>(expr)->isConstExpr = true;
    const_cast<ExprNode *>(expr)->constKey = out;
    return true;
  }
  if (auto *charNode = llvm::dyn_cast<CharNode>(expr)) {
    out = "c:" + std::to_string(charNode->value);
    const_cast<ExprNode *>(expr)->isConstExpr = true;
    const_cast<ExprNode *>(expr)->constKey = out;
    return true;
  }
  if (auto *runeNode = llvm::dyn_cast<RuneNode>(expr)) {
    out = "r:" + std::to_string(runeNode->value);
    const_cast<ExprNode *>(expr)->isConstExpr = true;
    const_cast<ExprNode *>(expr)->constKey = out;
    return true;
  }

  /* A const variable (const-qualified declaration) with a constant
   * initializer. 'final' variables are NOT constant expressions (Dart
   * rule: only const variables can be referenced in const contexts). */
  if (auto *varNode = llvm::dyn_cast<VariableNode>(expr)) {
    /* Constructor parameters inside a const initializer resolve to the
     * caller's argument expressions (Dart rule). */
    if (varNode->resolvedDecl &&
        varNode->resolvedDecl->kind == NodeKind::ParamDecl) {
      for (const auto &[param, arg] : this->constParamEnv) {
        if (param == varNode->resolvedDecl && arg) {
          if (this->constEvaluate(arg, true, out)) {
            const_cast<ExprNode *>(expr)->isConstExpr = true;
            const_cast<ExprNode *>(expr)->constKey = out;
            return true;
          }
        }
      }
      return false;
    }
    if (varNode->resolvedDecl &&
        varNode->resolvedDecl->kind == NodeKind::VarDecl) {
      const auto *varDecl =
          static_cast<const VarDeclNode *>(varNode->resolvedDecl);
      if (!varDecl->type->isConstQualified())
        return false;
      if (!varDecl->initializer)
        return false;
      if (!varDecl->isConstExpr) {
        /* Evaluate (and cache) the initializer as a const expression. */
        std::string val;
        if (!this->constEvaluate(varDecl->initializer, true, val))
          return false;
        varDecl->isConstExpr = true;
        varDecl->constKey = val;
      }
      out = varDecl->constKey;
      const_cast<ExprNode *>(expr)->isConstExpr = true;
      const_cast<ExprNode *>(expr)->constKey = out;
      return true;
    }
    return false;
  }

  /* Field access on a const object: 'constObj.finalField' (Dart rule). */
  if (auto *ma = llvm::dyn_cast<MemberAccessNode>(expr)) {
    if (ma->isEnumMember) {
      int64_t val = 0;
      if (ma->enumMember)
        val = ma->enumMember->evaluatedValue;
      out = "i:" + std::to_string(val);
      const_cast<ExprNode *>(expr)->isConstExpr = true;
      const_cast<ExprNode *>(expr)->constKey = out;
      return true;
    }
    std::string baseKey;
    if (!this->constEvaluate(ma->object, true, baseKey))
      return false;
    if (baseKey.rfind("o:", 0) != 0)
      return false;
    auto it = ctx->astCtx.constObjectFields.find(baseKey);
    if (it == ctx->astCtx.constObjectFields.end())
      return false;
    auto fit = it->second.find(std::string(ma->memberName));
    if (fit == it->second.end())
      return false;
    out = fit->second;
    const_cast<ExprNode *>(expr)->isConstExpr = true;
    const_cast<ExprNode *>(expr)->constKey = out;
    return true;
  }

  if (auto *castNode = llvm::dyn_cast<CastNode>(expr)) {
    std::string inner;
    if (!this->constEvaluate(castNode->expr, true, inner))
      return false;
    /* Pointer-to-pointer casts (const object references) and numeric
     * conversions keep the underlying value. */
    const Type *src = castNode->expr->exprType
                          ? castNode->expr->exprType->getUnqualifiedType()
                          : nullptr;
    const Type *dst = castNode->targetType
                          ? castNode->targetType->getUnqualifiedType()
                          : nullptr;
    if (src && dst) {
      bool srcPtr = src->isPointerType();
      bool dstPtr = dst->isPointerType();
      bool srcNum = src->isInteger() || src->isFloat();
      bool dstNum = dst->isInteger() || dst->isFloat();
      if (srcPtr && dstPtr && inner.rfind("o:", 0) == 0) {
        out = inner;
        const_cast<ExprNode *>(expr)->isConstExpr = true;
        const_cast<ExprNode *>(expr)->constKey = out;
        return true;
      }
      if (srcNum && dstNum) {
        /* int -> int / float conversions: re-serialize through the value. */
        if (inner.rfind("i:", 0) == 0 || inner.rfind("u:", 0) == 0) {
          long long iv = (inner[1] == 'i')
                             ? atoll(inner.c_str() + 2)
                             : (long long)strtoull(inner.c_str() + 2, nullptr, 10);
          if (dst->isFloat()) {
            char buf[64];
            snprintf(buf, sizeof(buf), "f:%a", (double)iv);
            out = buf;
          } else {
            out = inner;
          }
        } else if (inner.rfind("f:", 0) == 0 && !dst->isFloat()) {
          double dv = strtod(inner.c_str() + 2, nullptr);
          out = "i:" + std::to_string((long long)dv);
        } else {
          out = inner;
        }
        const_cast<ExprNode *>(expr)->isConstExpr = true;
        const_cast<ExprNode *>(expr)->constKey = out;
        return true;
      }
    }
    return false;
  }
  if (auto *implCast = llvm::dyn_cast<ImplicitCastNode>(expr)) {
    std::string inner;
    if (!this->constEvaluate(implCast->expr, true, inner))
      return false;
    out = inner;
    const_cast<ExprNode *>(expr)->isConstExpr = true;
    const_cast<ExprNode *>(expr)->constKey = out;
    return true;
  }

  /* Dart-style const array literal: 'const [1, 2, 3]'. All elements must be
   * constant expressions; identical content canonicalizes to one static
   * backing array (elements of object type are canonical instances, so the
   * serialized key is exact). */
  if (auto *arrNode = llvm::dyn_cast<ArrayLiteralNode>(expr)) {
    std::string elemTypeStr;
    if (arrNode->exprType) {
      const Type *arrTy = arrNode->exprType->getUnqualifiedType();
      if (auto *at = llvm::dyn_cast<ArrayType>(arrTy))
        elemTypeStr = at->getElementType()->getUnqualifiedType()->toString();
      else
        elemTypeStr = arrTy->toString();
    }
    std::string key = "A:" + elemTypeStr + "|";
    for (size_t i = 0; i < arrNode->elements.size(); ++i) {
      if (i)
        key += ",";
      std::string val;
      if (!this->constEvaluate(arrNode->elements[i], true, val))
        return false;
      key += val;
    }
    out = key;
    const_cast<ExprNode *>(expr)->isConstExpr = true;
    const_cast<ExprNode *>(expr)->constKey = key;
    return true;
  }

  if (auto *tern = llvm::dyn_cast<TernaryOpNode>(expr)) {
    std::string cond;
    if (!this->constEvaluate(tern->condition, true, cond))
      return false;
    const ExprNode *branch = nullptr;
    if (cond == "b:1")
      branch = tern->trueExpr;
    else if (cond == "b:0")
      branch = tern->falseExpr;
    else if (cond == "i:1")
      branch = tern->trueExpr;
    else if (cond == "i:0")
      branch = tern->falseExpr;
    else
      return false;
    std::string val;
    if (!this->constEvaluate(branch, true, val))
      return false;
    out = val;
    const_cast<ExprNode *>(expr)->isConstExpr = true;
    const_cast<ExprNode *>(expr)->constKey = out;
    return true;
  }

  if (auto *bop = llvm::dyn_cast<BinaryOpNode>(expr)) {
    std::string lhs, rhs;
    bool lhsOk = this->constEvaluate(bop->left, true, lhs);
    bool rhsOk = lhsOk && this->constEvaluate(bop->right, true, rhs);

    auto isInt = [](const std::string &v) {
      return v.rfind("i:", 0) == 0 || v.rfind("u:", 0) == 0;
    };
    auto isFloat = [](const std::string &v) { return v.rfind("f:", 0) == 0; };
    auto intVal = [](const std::string &v) -> long long {
      return v[1] == 'i' ? atoll(v.c_str() + 2)
                         : (long long)strtoull(v.c_str() + 2, nullptr, 10);
    };
    auto floatVal = [](const std::string &v) -> double {
      return strtod(v.c_str() + 2, nullptr);
    };
    auto toInt = [](long long v) {
      if (v >= 0)
        return "i:" + std::to_string(v);
      return "u:" + std::to_string((unsigned long long)v);
    };
    auto isStr = [](const std::string &v) { return v.rfind("s:", 0) == 0; };

    /* '&&' / '||' short-circuit; both sides must still be constant. */
    if (bop->op == "&&" || bop->op == "||") {
      if (!lhsOk || !isInt(lhs) && !(lhs == "b:0" || lhs == "b:1"))
        return false;
      bool lv = (lhs == "b:1" || lhs == "i:1");
      if (bop->op == "&&" && !lv) {
        out = "b:0";
      } else if (bop->op == "||" && lv) {
        out = "b:1";
      } else {
        if (!rhsOk)
          return false;
        bool rv = (rhs == "b:1" || rhs == "i:1");
        out = bop->op == "&&" ? (rv ? "b:1" : "b:0")
                              : (rv ? "b:1" : "b:0");
      }
      const_cast<ExprNode *>(expr)->isConstExpr = true;
      const_cast<ExprNode *>(expr)->constKey = out;
      return true;
    }

    if (!lhsOk || !rhsOk)
      return false;

    /* String concatenation. */
    if (bop->op == "+" && isStr(lhs) && isStr(rhs)) {
      out = "s:" + lhs.substr(2) + rhs.substr(2);
      const_cast<ExprNode *>(expr)->isConstExpr = true;
      const_cast<ExprNode *>(expr)->constKey = out;
      return true;
    }

    bool floating = isFloat(lhs) || isFloat(rhs);
    bool bothNumeric =
        isInt(lhs) || isFloat(lhs) ? (isInt(rhs) || isFloat(rhs)) : false;

    if (bop->op == "==" || bop->op == "!=") {
      if (isStr(lhs) && isStr(rhs)) {
        bool eq = lhs == rhs;
        out = eq ? "b:1" : "b:0";
      } else if (bothNumeric) {
        bool eq = floating ? floatVal(lhs) == floatVal(rhs)
                           : intVal(lhs) == intVal(rhs);
        out = eq ? "b:1" : "b:0";
      } else {
        return false;
      }
      if (bop->op == "!=")
        out = (out == "b:1") ? "b:0" : "b:1";
      const_cast<ExprNode *>(expr)->isConstExpr = true;
      const_cast<ExprNode *>(expr)->constKey = out;
      return true;
    }
    if (bop->op == "<" || bop->op == "<=" || bop->op == ">" ||
        bop->op == ">=") {
      if (!bothNumeric)
        return false;
      bool r = false;
      if (floating) {
        double a = floatVal(lhs), b = floatVal(rhs);
        r = bop->op == "<" ? a < b
            : bop->op == "<=" ? a <= b
            : bop->op == ">" ? a > b
                             : a >= b;
      } else {
        long long a = intVal(lhs), b = intVal(rhs);
        r = bop->op == "<" ? a < b
            : bop->op == "<=" ? a <= b
            : bop->op == ">" ? a > b
                             : a >= b;
      }
      out = r ? "b:1" : "b:0";
      const_cast<ExprNode *>(expr)->isConstExpr = true;
      const_cast<ExprNode *>(expr)->constKey = out;
      return true;
    }
    if (bop->op == "+" || bop->op == "-" || bop->op == "*" ||
        bop->op == "/" || bop->op == "%" || bop->op == "&" ||
        bop->op == "|" || bop->op == "^" || bop->op == "<<" ||
        bop->op == ">>") {
      if (!bothNumeric)
        return false;
      if (floating) {
        double a = floatVal(lhs), b = floatVal(rhs), r = 0;
        if (bop->op == "+")
          r = a + b;
        else if (bop->op == "-")
          r = a - b;
        else if (bop->op == "*")
          r = a * b;
        else if (bop->op == "/") {
          if (b == 0.0)
            return false;
          r = a / b;
        } else {
          return false;
        }
        char buf[64];
        snprintf(buf, sizeof(buf), "f:%a", r);
        out = buf;
      } else {
        long long a = intVal(lhs), b = intVal(rhs), r = 0;
        switch (bop->op[0]) {
        case '+':
          r = a + b;
          break;
        case '-':
          r = a - b;
          break;
        case '*':
          r = a * b;
          break;
        case '/':
          if (b == 0)
            return false;
          r = a / b;
          break;
        case '%':
          if (b == 0)
            return false;
          r = a % b;
          break;
        case '&':
          r = a & b;
          break;
        case '|':
          r = a | b;
          break;
        case '^':
          r = a ^ b;
          break;
        default:
          return false;
        }
        if (bop->op == "<<")
          r = a << (b & 63);
        else if (bop->op == ">>")
          r = a >> (b & 63);
        out = toInt(r);
      }
      const_cast<ExprNode *>(expr)->isConstExpr = true;
      const_cast<ExprNode *>(expr)->constKey = out;
      return true;
    }
    return false;
  }

  if (auto *uop = llvm::dyn_cast<UnaryOpNode>(expr)) {
    std::string inner;
    if (!this->constEvaluate(uop->expr, true, inner))
      return false;
    auto isIntV = [](const std::string &v) {
      return v.rfind("i:", 0) == 0 || v.rfind("u:", 0) == 0;
    };
    auto isFloatV = [](const std::string &v) { return v.rfind("f:", 0) == 0; };
    auto intV = [](const std::string &v) -> long long {
      return v[1] == 'i' ? atoll(v.c_str() + 2)
                         : (long long)strtoull(v.c_str() + 2, nullptr, 10);
    };
    auto floatV = [](const std::string &v) -> double {
      return strtod(v.c_str() + 2, nullptr);
    };
    auto toIntV = [](long long v) {
      if (v >= 0)
        return "i:" + std::to_string(v);
      return "u:" + std::to_string((unsigned long long)v);
    };
    if (uop->op == "-") {
      if (isIntV(inner)) {
        long long v = intV(inner);
        if (v == std::numeric_limits<long long>::min()) {
          out = "u:9223372036854775808";
        } else {
          out = toIntV(-v);
        }
      }
      else if (isFloatV(inner)) {
        char buf[64];
        snprintf(buf, sizeof(buf), "f:%a", -floatV(inner));
        out = buf;
      } else
        return false;
    } else if (uop->op == "+") {
      out = inner;
    } else if (uop->op == "!") {
      if (inner == "b:1")
        out = "b:0";
      else if (inner == "b:0")
        out = "b:1";
      else
        return false;
    } else if (uop->op == "~") {
      if (isIntV(inner))
        out = toIntV(~intV(inner));
      else
        return false;
    } else {
      return false;
    }
    const_cast<ExprNode *>(expr)->isConstExpr = true;
    const_cast<ExprNode *>(expr)->constKey = out;
    return true;
  }

  return false;
}

bool TypeCheckPass::constObjectFieldValue(const ExprNode *creation,
                                          std::string_view fieldName,
                                          std::string &out) {
  const FunctionDeclNode *ctor = resolveConstCtorCall(creation);
  if (!ctor)
    return false;

  const auto *call = llvm::dyn_cast<FunctionCallNode>(creation);
  const auto *newNode = llvm::dyn_cast<NewExprNode>(creation);
  llvm::ArrayRef<ExprNode *> args =
      call ? call->args
           : (newNode ? newNode->args : llvm::ArrayRef<ExprNode *>());
  llvm::ArrayRef<std::string_view> argNames =
      call ? call->argNames
           : (newNode ? newNode->argNames
                      : llvm::ArrayRef<std::string_view>());

  /* this-parameter for the field. */
  for (size_t p = 0; p < ctor->params.size(); ++p) {
    if (!ctor->params[p]->isThisParam)
      continue;
    if (ctor->params[p]->name != fieldName)
      continue;
    const ExprNode *arg = nullptr;
    size_t positional = 0;
    for (size_t i = 0; i < args.size(); ++i) {
      if (!argNames.empty() && !argNames[i].empty()) {
        if (ctor->params[p]->name == argNames[i])
          arg = args[i];
      } else {
        if (positional == p)
          arg = args[i];
        ++positional;
      }
    }
    if (!arg)
      return false;
    return this->constEvaluate(arg, true, out);
  }

  /* ': this.field = expr' initializer-list entry. */
  for (const auto *init : ctor->fieldInitializers) {
    if (auto *target = llvm::dyn_cast<MemberAccessNode>(init->target)) {
      if (target->memberName == fieldName)
        return this->constEvaluate(init->value, true, out);
    }
  }

  /* Declaration initializer of the field itself. */
  if (ctor->parentRecord) {
    if (const DeclNode *recDecl = ctor->parentRecord->getDeclaration()) {
      llvm::ArrayRef<VarDeclNode *> fields;
      if (auto *c = llvm::dyn_cast<ClassDeclNode>(recDecl))
        fields = c->fields;
      else if (auto *s = llvm::dyn_cast<StructDeclNode>(recDecl))
        fields = s->fields;
      else if (auto *u = llvm::dyn_cast<UnionDeclNode>(recDecl))
        fields = u->fields;
      for (const auto *f : fields) {
        if (!f->isStatic && f->varName == fieldName && f->initializer)
          return this->constEvaluate(f->initializer, true, out);
      }
    }
  }

  /* Inherited field: recurse through the super call. */
  if (ctor->superCall) {
    if (resolveConstCtorCall(ctor->superCall))
      return this->constObjectFieldValue(ctor->superCall, fieldName, out);
  }
  return false;
}

void TypeCheckPass::recordConstObjectFields(const std::string &objectKey,
                                            const ExprNode *creation) {
  const FunctionDeclNode *ctor = resolveConstCtorCall(creation);
  if (!ctor)
    return;
  if (!ctor->parentRecord)
    return;

  auto &fields = ctx->astCtx.constObjectFields[objectKey];
  const ClassDeclNode *cls = nullptr;
  if (const DeclNode *recDecl = ctor->parentRecord->getDeclaration())
    cls = llvm::dyn_cast<ClassDeclNode>(recDecl);
  if (cls) {
    for (const auto *f : cls->fields) {
      if (f->isStatic)
        continue;
      std::string val;
      if (this->constObjectFieldValue(creation, f->varName, val))
        fields[std::string(f->varName)] = val;
    }
  }
  if (ctor->superCall) {
    if (resolveConstCtorCall(ctor->superCall))
      this->recordConstObjectFields(objectKey, ctor->superCall);
  }
}

void TypeCheckPass::checkConstConstructor(const FunctionDeclNode *ctor) {
  if (!ctor->isConst)
    return;

  /* Dart: a const constructor must have an empty body (assert-only bodies
   * are not expressible in Utopia). */
  if (ctor->body && !ctor->body->statements.empty()) {
    (void)ctx->reportError(ctor->line, ctor->column, ctor->length,
                           "A const constructor must have an empty body.");
  }
  if (ctor->isAsync) {
    (void)ctx->reportError(ctor->line, ctor->column, ctor->length,
                           "A const constructor cannot be 'async'.");
  }
  if (ctor->isVariadic) {
    (void)ctx->reportError(ctor->line, ctor->column, ctor->length,
                           "A const constructor cannot be variadic.");
  }

  /* Dart: all instance fields of a class with a const constructor must be
   * final, and their declaration initializers must be constant
   * expressions (needed to build the canonical instance). */
  if (!ctor->parentRecord)
    return;
  if (const DeclNode *recDecl = ctor->parentRecord->getDeclaration()) {
    if (auto *cls = llvm::dyn_cast<ClassDeclNode>(recDecl)) {
      for (const auto *f : cls->fields) {
        if (f->isStatic)
          continue;
        if (!f->isFinal) {
          (void)ctx->reportError(
              f->line, f->column, f->length,
              "Class '" + std::string(cls->name) +
                  "' has a const constructor, so all instance fields must "
                  "be 'final' (Dart rule). Field '" +
                  std::string(f->varName) + "' is not final.");
          continue;
        }
        if (f->initializer) {
          std::string val;
          if (!this->constEvaluate(f->initializer, true, val)) {
            (void)ctx->reportError(
                f->line, f->column, f->length,
                "The declaration initializer of final field '" +
                    std::string(f->varName) +
                    "' must be a constant expression for the class to have "
                    "a const constructor.");
          }
        }
      }
    } else if (auto *st = llvm::dyn_cast<StructDeclNode>(recDecl)) {
      (void)ctx->reportError(ctor->line, ctor->column, ctor->length,
                             "Const constructors are only supported on "
                             "classes (structs are value types).");
      (void)st;
    }
  }
}

SemaResult TypeCheckPass::visit(const ConstExprNode *node) {
  /* Establish the const context BEFORE dispatching the inner expression so
   * nested constructor calls resolve as canonical const objects (implicit
   * const) with their pointer types in place for overload resolution. */
  ctx->constContextDepth++;
  auto res = dispatch(node->expr);
  ctx->constContextDepth--;
  if (!res)
    return res;

  std::string key;
  if (!this->constEvaluate(node->expr, true, key)) {
    return ctx->reportError(node->line, node->column, node->length,
                            "Not a constant expression.");
  }

  node->isConstExpr = true;
  node->constKey = key;
  node->isLValue = false;
  if (const FunctionDeclNode *ctor = resolveConstCtorCall(node->expr)) {
    node->exprType = ctx->astCtx.getPointerType(ctor->parentRecord);
  } else {
    node->exprType = node->expr->exprType;
  }
  return node->exprType;
}

SemaResult TypeCheckPass::visit(const DeleteExprNode *node) {
  auto ptrTy = dispatch(node->ptr);
  if (!ptrTy) {
    return std::unexpected(ErrorInfo{node->line, node->column, node->length,
                                     "Cascading error in delete"});
  }

  /* Canonical const objects live in static read-only storage; their
   * destructors never run. Deleting one would destroy static memory, so
   * it is a compile-time error (Dart has no delete; this is the manual
   * memory adaptation). */
  if (node->ptr->isConstExpr) {
    return ctx->reportError(node->line, node->column, node->length,
                            "Cannot delete a canonical const object (it "
                            "lives in static storage and is never freed).");
  }
  if (auto *varNode = llvm::dyn_cast<VariableNode>(node->ptr)) {
    if (varNode->resolvedDecl &&
        varNode->resolvedDecl->kind == NodeKind::VarDecl &&
        static_cast<const VarDeclNode *>(varNode->resolvedDecl)->isConstExpr) {
      return ctx->reportError(
          node->line, node->column, node->length,
          "Cannot delete a canonical const object (it lives in static "
          "storage and is never freed).");
    }
  }

  if (!(*ptrTy)->getUnqualifiedType()->isPointerType()) {
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

  /* Resolve the custom deallocator ('operator delete'). */
  const FunctionDeclNode *deallocOp = nullptr;
  if (unqual->getKind() == TypeKind::Class ||
      unqual->getKind() == TypeKind::Struct ||
      unqual->getKind() == TypeKind::Union) {
    deallocOp = findRecordAllocationOperator(
        static_cast<const RecordType *>(unqual), "operator delete");
  }
  if (!deallocOp) {
    for (const auto *d : ctx->lookup("operator delete")) {
      if (d->kind == NodeKind::FunctionDecl &&
          static_cast<const FunctionDeclNode *>(d)->name ==
              "operator delete") {
        deallocOp = static_cast<const FunctionDeclNode *>(d);
        break;
      }
    }
  }
  if (deallocOp) {
    bool okFmt = deallocOp->params.size() == 1 &&
                 deallocOp->params[0]->type->getUnqualifiedType()
                     ->isPointerType() &&
                 deallocOp->returnType->getUnqualifiedType()->isVoid();
    if (!okFmt) {
      return ctx->reportError(
          node->line, node->column, node->length,
          "'operator delete' must be declared as "
          "'void operator delete(void* ptr)'.");
    }
    const_cast<DeleteExprNode *>(node)->deallocator = deallocOp;
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

/* Walks a lambda block body in statement order while binding its declared
 * locals to the current scope. This lets each 'return' value expression be
 * dispatched to infer the lambda's return type even when the lambda has no
 * expected signature (e.g. IIFEs or direct calls), where locals declared
 * earlier in the body must already be visible. */
namespace {
struct LambdaReturnInferer : public ASTVisitor<LambdaReturnInferer, void> {
  SemaContext &ctx;
  TypeCheckPass &pass;
  const Type *inferred = nullptr;

  LambdaReturnInferer(SemaContext &c, TypeCheckPass &p) : ctx(c), pass(p) {}

  void walkStatements(llvm::ArrayRef<ASTNode *> stmts) {
    for (const auto *s : stmts) {
      if (inferred)
        return;
      if (s->kind == NodeKind::VarDecl) {
        const auto *vd = static_cast<const VarDeclNode *>(s);
        ctx.addDecl(vd->varName, static_cast<const DeclNode *>(vd));
      }
      dispatch(s);
    }
  }

  void visit(const ReturnNode *node) {
    if (!inferred && node->value) {
      if (auto retTy = pass.dispatch(node->value)) {
        inferred = *retTy;
      }
    }
  }
  void visit(const BlockNode *node) {
    ScopeGuard guard(ctx);
    walkStatements(node->statements);
  }
  void visit(const IfNode *node) {
    if (node->thenBlock)
      dispatch(node->thenBlock);
    if (node->elseBlock)
      dispatch(node->elseBlock);
  }
  void visit(const WhileNode *node) {
    if (node->body)
      dispatch(node->body);
  }
  void visit(const ForNode *node) {
    if (node->initStatement) {
      if (node->initStatement->kind == NodeKind::VarDecl) {
        const auto *vd =
            static_cast<const VarDeclNode *>(node->initStatement);
        ctx.addDecl(vd->varName, static_cast<const DeclNode *>(vd));
      }
      dispatch(node->initStatement);
    }
    if (node->body)
      dispatch(node->body);
  }
  void visit(const SwitchNode *node) {
    for (const auto *c : node->cases)
      dispatch(c);
  }
  void visit(const CaseNode *node) { walkStatements(node->statements); }
  void visit(const LambdaNode *) {} /* nested lambdas are typed on their own */
  void visit(const ASTNode *) {}    /* no-op fallback for other node kinds */
};

/* The expected signature of a lambda drives only its own parameter/return
 * typing; it must not leak into nested lambdas inside the body. This guard
 * hides it while the body is processed and restores it afterwards so the
 * caller's bookkeeping stays balanced. */
struct ExpectedFnGuard {
  SemaContext &ctx;
  bool active;
  const Type *saved;

  ExpectedFnGuard(SemaContext &c, const Type *expected)
      : ctx(c), active(expected != nullptr), saved(expected) {
    if (active)
      ctx.popExpectedFunctionType();
  }
  ~ExpectedFnGuard() {
    if (active)
      ctx.pushExpectedFunctionType(saved);
  }
};

/* Lambdas lower to plain function pointers, so they cannot capture variables
 * from an enclosing function's stack. This checker walks a lambda body (not
 * descending into nested lambdas, which are validated on their own) and flags
 * every reference to an enclosing local or parameter. */
struct LambdaCaptureChecker {
  SemaContext &ctx;
  const LambdaNode *lambda;
  std::vector<std::vector<std::string_view>> frames;
  std::vector<const VariableNode *> violations;

  explicit LambdaCaptureChecker(SemaContext &c, const LambdaNode *l)
      : ctx(c), lambda(l) {}

  void check(const ASTNode *node) { walk(node); }

  void walk(const ASTNode *node) {
    if (!node)
      return;
    switch (node->kind) {
    case NodeKind::Variable:
      checkVar(static_cast<const VariableNode *>(node));
      break;
    case NodeKind::UnaryOp:
      walk(static_cast<const UnaryOpNode *>(node)->expr);
      break;
    case NodeKind::BinaryOp:
      walk(static_cast<const BinaryOpNode *>(node)->left);
      walk(static_cast<const BinaryOpNode *>(node)->right);
      break;
    case NodeKind::TernaryOp: {
      auto *t = static_cast<const TernaryOpNode *>(node);
      walk(t->condition);
      walk(t->trueExpr);
      walk(t->falseExpr);
      break;
    }
    case NodeKind::Lambda:
      break; /* nested lambdas are validated by their own visit */
    case NodeKind::Assign:
      walk(static_cast<const AssignNode *>(node)->target);
      walk(static_cast<const AssignNode *>(node)->value);
      break;
    case NodeKind::Block: {
      frames.emplace_back();
      for (const auto *s : static_cast<const BlockNode *>(node)->statements)
        walk(s);
      frames.pop_back();
      break;
    }
    case NodeKind::If: {
      auto *i = static_cast<const IfNode *>(node);
      walk(i->condition);
      walk(i->thenBlock);
      walk(i->elseBlock);
      break;
    }
    case NodeKind::For: {
      auto *f = static_cast<const ForNode *>(node);
      walk(f->initStatement);
      walk(f->condition);
      walk(f->increment);
      walk(f->body);
      break;
    }
    case NodeKind::ForIn: {
      auto *f = static_cast<const ForInNode *>(node);
      walk(f->iterable);
      walk(f->body);
      break;
    }
    case NodeKind::While: {
      auto *w = static_cast<const WhileNode *>(node);
      walk(w->condition);
      walk(w->body);
      break;
    }
    case NodeKind::Switch: {
      auto *s = static_cast<const SwitchNode *>(node);
      walk(s->condition);
      for (const auto *c : s->cases)
        walk(c);
      break;
    }
    case NodeKind::Case: {
      auto *c = static_cast<const CaseNode *>(node);
      walk(c->value);
      for (const auto *s : c->statements)
        walk(s);
      break;
    }
    case NodeKind::FunctionCall: {
      auto *call = static_cast<const FunctionCallNode *>(node);
      walk(call->target);
      for (const auto *a : call->args)
        walk(a);
      break;
    }
    case NodeKind::Return:
      walk(static_cast<const ReturnNode *>(node)->value);
      break;
    case NodeKind::Cast:
      walk(static_cast<const CastNode *>(node)->expr);
      break;
    case NodeKind::Is:
      walk(static_cast<const IsExprNode *>(node)->expr);
      break;
    case NodeKind::MemberAccess:
      walk(static_cast<const MemberAccessNode *>(node)->object);
      break;
    case NodeKind::ArraySubscript: {
      auto *sub = static_cast<const ArraySubscriptNode *>(node);
      walk(sub->base);
      walk(sub->index);
      break;
    }
    case NodeKind::ArrayLiteral:
      for (const auto *e : static_cast<const ArrayLiteralNode *>(node)->elements)
        walk(e);
      break;
    case NodeKind::New: {
      auto *n = static_cast<const NewExprNode *>(node);
      walk(n->arraySize);
      for (const auto *a : n->args)
        walk(a);
      break;
    }
    case NodeKind::Delete:
      walk(static_cast<const DeleteExprNode *>(node)->ptr);
      break;
    case NodeKind::DestructorCall:
      walk(static_cast<const DestructorCallNode *>(node)->object);
      break;
    case NodeKind::VarDecl: {
      auto *v = static_cast<const VarDeclNode *>(node);
      if (!frames.empty())
        frames.back().push_back(v->varName);
      walk(v->initializer);
      break;
    }
    case NodeKind::ImplicitCast:
      walk(static_cast<const ImplicitCastNode *>(node)->expr);
      break;
    default:
      break; /* leaves and declarations */
    }
  }

private:
  void checkVar(const VariableNode *node) {
    for (const auto &frame : frames) {
      for (auto name : frame) {
        if (name == node->name)
          return; /* declared inside the lambda body */
      }
    }
    for (const auto *p : lambda->params) {
      if (p->name == node->name)
        return; /* the lambda's own parameter */
    }

    auto decls = ctx.lookup(node->name);
    for (const auto *d : decls) {
      if (d->kind == NodeKind::FunctionDecl)
        return; /* plain function reference */
      if (d->kind == NodeKind::VarDecl &&
          static_cast<const VarDeclNode *>(d)->isGlobal)
        return; /* global variable */
      if (d->kind == NodeKind::EnumDecl || d->kind == NodeKind::EnumMember ||
          d->kind == NodeKind::ClassDecl || d->kind == NodeKind::StructDecl ||
          d->kind == NodeKind::UnionDecl || d->kind == NodeKind::TypedefDecl ||
          d->kind == NodeKind::NamespaceDecl ||
          d->kind == NodeKind::AnnotationDecl)
        return; /* type/namespace references */
    }
    if (!decls.empty())
      violations.push_back(node);
  }
};
} // namespace

SemaResult
TypeCheckPass::resolveLambdaArgs(const FunctionDeclNode *fn,
                                 const std::vector<ExprNode *> &resolvedArgs) {
  std::vector<const Type *> paramTypes;
  for (const auto *p : fn->params)
    paramTypes.push_back(p->type);
  return resolveLambdaArgs(
      ctx->astCtx.getFunctionType(ctx->astCtx.VoidTy,
                                  ctx->astCtx.copyArray<const Type *>(paramTypes)),
      resolvedArgs);
}

SemaResult
TypeCheckPass::resolveLambdaArgs(const FunctionType *fTy,
                                 const std::vector<ExprNode *> &resolvedArgs) {
  for (size_t i = 0; i < resolvedArgs.size() &&
                     i < fTy->getParamTypes().size();
       ++i) {
    auto *lambda = llvm::dyn_cast<LambdaNode>(resolvedArgs[i]);
    if (!lambda || !lambda->unresolved)
      continue;

    const Type *paramTy = fTy->getParamTypes()[i];
    ctx->pushExpectedFunctionType(paramTy);
    auto res = dispatch(lambda);
    ctx->popExpectedFunctionType();

    if (!res)
      return std::unexpected(ErrorInfo{lambda->line, lambda->column,
                                       lambda->length,
                                       "Failed to type lambda argument"});
    if (lambda->unresolved) {
      return ctx->reportError(lambda->line, lambda->column, lambda->length,
                              "Cannot infer lambda parameter types: no "
                              "expected function signature is available.");
    }
  }
  return SemaResult(ctx->astCtx.VoidTy);
}

SemaResult TypeCheckPass::visit(const LambdaNode *node) {
  const Type *expected = ctx->getExpectedFunctionType();
  const FunctionType *expectedFn = nullptr;
  if (expected) {
    const Type *u = expected->getUnqualifiedType();
    if (u->isReferenceType()) {
      u = static_cast<const ReferenceType *>(u)->getPointeeType()->getUnqualifiedType();
    } else if (u->getKind() == TypeKind::RValueReference) {
      u = static_cast<const RValueReferenceType *>(u)->getPointeeType()->getUnqualifiedType();
    }
    if (u->isPointerType()) {
      u = static_cast<const PointerType *>(u)->getPointeeType()->getUnqualifiedType();
    }
    if (auto *ft = llvm::dyn_cast<FunctionType>(u)) {
      expectedFn = ft;
    }
  }

  /* Parameter types: explicit types are kept; untyped parameters are filled
   * from the expected signature. */
  bool missingContext = false;
  for (size_t i = 0; i < node->params.size(); ++i) {
    const_cast<ParamDeclNode *>(node->params[i])->type =
        resolveIfTemplate(node->params[i]->type);
    const Type *paramTy = node->params[i]->type;
    /* 'Auto' here is the placeholder left by a previous unresolved dispatch,
     * not a user-written type, so it is always re-typed from the context. */
    bool isPlaceholder =
        !paramTy || paramTy->getUnqualifiedType()->getKind() == TypeKind::Auto;
    if (isPlaceholder) {
      if (expectedFn && i < expectedFn->getParamTypes().size()) {
        const_cast<ParamDeclNode *>(node->params[i])->type =
            expectedFn->getParamTypes()[i];
      } else {
        const_cast<ParamDeclNode *>(node->params[i])->type =
            ctx->astCtx.AutoTy;
        missingContext = true;
      }
    }
  }

  /* Return type precedence: explicit > context > body inference. An 'auto'
   * expected return (call-site inference) forces inference from the body. */
  const Type *returnType = nullptr;
  if (node->explicitReturnType) {
    returnType = resolveIfTemplate(node->explicitReturnType);
  } else if (expectedFn &&
             expectedFn->getReturnType()->getUnqualifiedType()->getKind() !=
                 TypeKind::Auto) {
    returnType = expectedFn->getReturnType();
  }

  /* Async lambdas complete with a Future of their value type. When the
   * expected signature already returns a Future, unwrap it so the body is
   * checked against the value type (Dart semantics). */
  if (node->isAsync) {
    const Type *inner = nullptr;
    if (returnType && unwrapFutureType(returnType, &inner)) {
      returnType = inner;
    }
  }

  /* The expected signature must not leak into nested lambdas inside the body;
   * it is hidden from here on and restored when this visit completes. */
  ExpectedFnGuard expectedGuard(*ctx, expected);

  if (missingContext && !returnType) {
    /* Untyped parameters with no context: leave the lambda unresolved so the
     * surrounding call can re-type it with the matched parameter signature.
     * The body is not dispatched here; it will be fully checked once the
     * expected signature is known (see resolveLambdaArgs). */
    const_cast<LambdaNode *>(node)->unresolved = true;
    std::vector<const Type *> autoParams;
    for (const auto *p : node->params)
      autoParams.push_back(p->type);
    auto fnTy = ctx->astCtx.getFunctionType(
        ctx->astCtx.AutoTy, ctx->astCtx.copyArray<const Type *>(autoParams));
    node->exprType = ctx->astCtx.getPointerType(fnTy);
    node->isLValue = false;

    /* Reject captures even when the body is not dispatched yet (e.g. a
     * lambda used as a call target without a signature context). */
    LambdaCaptureChecker captureChecker(*ctx, node);
    captureChecker.check(node->isExpressionBody
                             ? static_cast<const ASTNode *>(node->exprBody)
                             : static_cast<const ASTNode *>(node->body));
    for (const auto *v : captureChecker.violations) {
      ctx->reportError(
          v->line, v->column, v->length,
          "Lambda captures local variable '" + std::string(v->name) +
              "'; lambdas are function pointers and cannot capture enclosing "
              "function variables.");
    }

    return node->exprType;
  }

  if (!returnType) {
    /* Parameters must be in scope so the body can reference them while the
     * return type is inferred. */
    ScopeGuard guard(*ctx, ScopeKind::FunctionParams);
    for (const auto *p : node->params)
      ctx->addDecl(p->name, p);

    if (node->isExpressionBody) {
      auto exprTy = dispatch(node->exprBody);
      if (!exprTy)
        return std::unexpected(ErrorInfo{node->line, node->column,
                                         node->length,
                                         "Failed to infer lambda return type"});
      returnType = *exprTy;
    } else {
      LambdaReturnInferer inferer(*ctx, *this);
      inferer.dispatch(node->body);
      returnType = inferer.inferred ? inferer.inferred : ctx->astCtx.VoidTy;
    }
  }

  /* Lower the expression body: '=> expr' becomes 'return expr;' unless the
   * lambda returns void. */
  BlockNode *finalBody = node->body;
  if (node->isExpressionBody) {
    auto block = ctx->astCtx.create<BlockNode>(node->line, node->column);
    ASTNode *stmt = node->exprBody;
    if (!returnType->isVoid()) {
      stmt = ctx->astCtx.create<ReturnNode>(node->exprBody, node->line,
                                            node->column, node->length);
    }
    block->statements = ctx->astCtx.copyArray<ASTNode *>(stmt);
    block->isExpressionBody = true;
    block->finalize(node->endLine ? node->endLine : node->line);
    finalBody = block;
  }

  /* Lambdas lower to function pointers, so captures of the enclosing
   * function's stack are rejected. */
  LambdaCaptureChecker captureChecker(*ctx, node);
  captureChecker.check(finalBody);
  for (const auto *v : captureChecker.violations) {
    ctx->reportError(
        v->line, v->column, v->length,
        "Lambda captures local variable '" + std::string(v->name) +
            "'; lambdas are function pointers and cannot capture enclosing "
            "function variables.");
  }
  if (!captureChecker.violations.empty())
    return std::unexpected(ErrorInfo{node->line, node->column, node->length,
                                     "Lambda capture error"});

  std::string nameStr =
      "__lambda_" + std::to_string(ctx->lambdaCounter++);  std::string_view name = ctx->astCtx.copyString(nameStr);
  auto *fnDecl = ctx->astCtx.create<FunctionDeclNode>(
      returnType, name, node->line, node->column, false, false, false, false);
  fnDecl->params = node->params;
  fnDecl->body = finalBody;
  fnDecl->fqName = name;
  fnDecl->declFilePath = ctx->currentFile;
  fnDecl->mangledName = Mangler::mangle(fnDecl);
  fnDecl->isAsync = node->isAsync;
  const_cast<LambdaNode *>(node)->synthesizedFunc = fnDecl;
  const_cast<LambdaNode *>(node)->mangledName = fnDecl->mangledName;

  auto bodyRes = dispatch(fnDecl);
  if (!bodyRes) {
    return std::unexpected(ErrorInfo{node->line, node->column, node->length,
                                     "Errors in lambda body"});
  }

  if (expectedFn) {
    if (node->params.size() != expectedFn->getParamTypes().size()) {
      return ctx->reportError(
          node->line, node->column, node->length,
          "Lambda has " + std::to_string(node->params.size()) +
              " parameters, but the expected function signature takes " +
              std::to_string(expectedFn->getParamTypes().size()) + ".");
    }
    for (size_t i = 0; i < node->params.size(); ++i) {
      const Type *pType =
          node->params[i]->type->getUnqualifiedType();
      const Type *eType = expectedFn->getParamTypes()[i]->getUnqualifiedType();
      if (pType != eType) {
        return ctx->reportError(
            node->line, node->column, node->length,
            "Lambda parameter " + std::to_string(i + 1) +
                " type mismatch: expected '" + eType->toString() +
                "', but lambda declares '" + pType->toString() + "'.");
      }
    }
    /* Async lambdas are compared against the expected signature with their
     * effective (Future-wrapped) return type. The expected return type is
     * resolved first so template-instance placeholders (e.g. Future<void>
     * in an instantiated method signature) compare equal. */
    const Type *effectiveReturn = node->isAsync ? getFutureType(returnType)
                                                : returnType;
    const Type *expectedRet = resolveIfTemplate(expectedFn->getReturnType());
    if (expectedRet->getUnqualifiedType()->getKind() != TypeKind::Auto &&
        !canImplicitlyCast(effectiveReturn, expectedRet)) {
      return ctx->reportError(
          node->line, node->column, node->length,
          "Lambda return type mismatch: expected '" +
              expectedFn->getReturnType()->toString() + "', got '" +
              effectiveReturn->toString() + "'.");
    }
  }

  std::vector<const Type *> paramTypes;
  for (const auto *p : node->params)
    paramTypes.push_back(p->type);
  const Type *fnReturn =
      node->isAsync ? getFutureType(returnType) : returnType;
  auto fnTy = ctx->astCtx.getFunctionType(
      fnReturn, ctx->astCtx.copyArray<const Type *>(paramTypes));
  node->exprType = ctx->astCtx.getPointerType(fnTy);
  node->isLValue = false;
  const_cast<LambdaNode *>(node)->unresolved = false;
  return node->exprType;
}

} // namespace utopia