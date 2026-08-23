#include "utopia/Sema/UnusedCode.hpp"
#include "utopia/Common/Types.hpp"
#include <algorithm>
#include <llvm/Support/Casting.h>

namespace utopia {

void UnusedCodePass::markUsed(const DeclNode *decl) {
  if (!decl)
    return;
  usedDecls.insert(decl);

  std::string file(decl->declFilePath);
  if (!file.empty()) {
    usedFilesByFile[std::string(ctx->currentFile)].insert(std::move(file));
  }
  if (!decl->fqName.empty()) {
    usedFqNamesByFile[std::string(ctx->currentFile)]
        .insert(std::string(decl->fqName));
  }
}

const DeclNode *UnusedCodePass::resolveRecordType(const Type *type) {
  if (!type)
    return nullptr;

  /* getUnqualifiedType() unwraps aliases and template instantiations, so
   * their declarations must be captured before it: a typedef is a first-
   * class declaration whose file owns it. */
  if (auto *alias = llvm::dyn_cast<AliasType>(type)) {
    if (const DeclNode *decl = alias->getDeclaration())
      return decl;
  }

  const Type *unqual = type->getUnqualifiedType();
  switch (unqual->getKind()) {
  case TypeKind::Struct:
  case TypeKind::Class:
  case TypeKind::Union:
    return static_cast<const RecordType *>(unqual)->getDeclaration();
  case TypeKind::Enum:
    return static_cast<const EnumType *>(unqual)->getDeclaration();
  case TypeKind::Alias: {
    const AliasType *alias = static_cast<const AliasType *>(unqual);
    if (const DeclNode *decl = alias->getDeclaration())
      return decl;
    const Type *target = alias->getTarget();
    return target ? resolveRecordType(target) : nullptr;
  }
  case TypeKind::TemplateInst: {
    const Type *resolved =
        static_cast<const TemplateInstType *>(unqual)->getResolvedType();
    if (resolved)
      return resolveRecordType(resolved);
    return nullptr;
  }
  default:
    return nullptr;
  }
}

void UnusedCodePass::recordTypeUsage(const Type *type) {
  if (!type)
    return;

  /* Aliases and unresolved template instantiations are unwrapped by
   * getUnqualifiedType(): record their declarations explicitly so an import
   * that only provides a typedef still counts as used. */
  if (auto *alias = llvm::dyn_cast<AliasType>(type)) {
    if (const DeclNode *decl = alias->getDeclaration())
      markUsed(decl);
    if (const Type *target = alias->getTarget())
      recordTypeUsage(target);
  } else if (auto *inst = llvm::dyn_cast<TemplateInstType>(type)) {
    if (const Type *resolved = inst->getResolvedType()) {
      recordTypeUsage(resolved);
    } else {
      /* Template bodies are never type-checked, so their instantiated types
       * stay unresolved; fall back to the base template's declaration. */
      auto decls = ctx->lookup(inst->getBaseName());
      for (const DeclNode *d : decls) {
        if (llvm::isa<StructDeclNode>(d) || llvm::isa<ClassDeclNode>(d) ||
            llvm::isa<UnionDeclNode>(d)) {
          markUsed(d);
          break;
        }
      }
    }
  }

  const Type *unqual = type->getUnqualifiedType();
  switch (unqual->getKind()) {
  case TypeKind::Struct:
  case TypeKind::Class:
  case TypeKind::Union:
  case TypeKind::Enum:
  case TypeKind::Alias:
  case TypeKind::TemplateInst: {
    if (const DeclNode *decl = resolveRecordType(type))
      markUsed(decl);
    if (auto *alias = llvm::dyn_cast<AliasType>(unqual)) {
      if (const Type *target = alias->getTarget())
        recordTypeUsage(target);
    }
    if (auto *inst = llvm::dyn_cast<TemplateInstType>(unqual)) {
      for (const Type *arg : inst->getTemplateArgs())
        recordTypeUsage(arg);
    }
    break;
  }
  case TypeKind::Pointer:
    recordTypeUsage(static_cast<const PointerType *>(unqual)->getPointeeType());
    break;
  case TypeKind::Reference:
    recordTypeUsage(
        static_cast<const ReferenceType *>(unqual)->getPointeeType());
    break;
  case TypeKind::RValueReference:
    recordTypeUsage(
        static_cast<const RValueReferenceType *>(unqual)->getPointeeType());
    break;
  case TypeKind::Array:
    recordTypeUsage(static_cast<const ArrayType *>(unqual)->getElementType());
    break;
  case TypeKind::Vector:
    recordTypeUsage(static_cast<const VectorType *>(unqual)->getElementType());
    break;
  default:
    break;
  }
}

bool UnusedCodePass::isPreludeModule(const ModuleNode *module) {
  return module && module->filePath.ends_with("prelude.utp");
}


void UnusedCodePass::collectModuleFiles(
    const ModuleNode *module, std::unordered_set<std::string> &files,
    std::unordered_set<const ModuleNode *> &visited) {
  if (!module || visited.contains(module))
    return;
  visited.insert(module);

  /* The prelude is implicitly injected into every module, so it never makes
   * an explicit import "used". */
  if (!isPreludeModule(module))
    files.insert(std::string(module->filePath));

  for (const ModuleNode *imp : module->importedModules)
    collectModuleFiles(imp, files, visited);
  for (const ModuleNode *exp : module->exportedModules)
    collectModuleFiles(exp, files, visited);
}

bool UnusedCodePass::isPrivateFunc(const FunctionDeclNode *fn) const {
  if (fn->isMethod || fn->parentRecord || fn->isExtern || fn->isIntrinsic ||
      fn->isWeak || fn->isTemplate || fn->isImplicit || !fn->body ||
      fn->name == "main" || fn->name.starts_with("operator"))
    return false;
  return fn->isPrivate(fn->name);
}

bool UnusedCodePass::isPrivateMethod(const FunctionDeclNode *fn) const {
  if (!fn->isMethod || !fn->parentRecord || fn->isExtern || fn->isIntrinsic ||
      fn->isWeak || fn->isTemplate || fn->isImplicit || !fn->body ||
      fn->isVirtual || fn->isOverride || fn->isAbstract || fn->name == "~" ||
      fn->name.starts_with("operator"))
    return false;

  const DeclNode *recDecl =
      fn->parentRecord ? fn->parentRecord->getDeclaration() : nullptr;
  if (!recDecl)
    return false;

  /* Constructors share the name of their record. */
  size_t dot = recDecl->fqName.find_last_of('.');
  std::string_view simpleName =
      (dot == std::string_view::npos) ? recDecl->fqName
                                      : recDecl->fqName.substr(dot + 1);
  if (fn->name == simpleName)
    return false;
  return fn->isPrivate(fn->name);
}

void UnusedCodePass::addParamCandidate(const ParamDeclNode *param) {
  if (suppressCandidates() || !param || param->isThisParam ||
      param->name == "this" || param->name == "_" ||
      param->name.starts_with("_"))
    return;
  paramCandidates.push_back(
      {param, std::string(ctx->currentFile)});
}

bool UnusedCodePass::run(const ModuleNode *module, SemaContext &context) {
  ctx = &context;

  /* The driver re-runs the pipeline once per root translation unit; each run
   * walks the tree reachable from that root with fresh state, so every root
   * is covered. Files are reported at most once across runs through
   * reportedFiles (their candidates are file-private, so later runs never
   * change the verdict). */
  usedDecls.clear();
  usedFilesByFile.clear();
  usedFqNamesByFile.clear();
  declCandidates.clear();
  fieldCandidates.clear();
  paramCandidates.clear();
  usingCandidates.clear();
  importCandidates.clear();
  moduleFileCache.clear();
  visitedModules.clear();
  templateDepth = 0;
  collectingInstantiated = false;

  dispatch(module);
  return true;
}

void UnusedCodePass::visit(const ModuleNode *node) {
  if (visitedModules.contains(node))
    return;
  visitedModules.insert(node);

  std::string filePath(node->filePath);

  /* Record import candidates. Exports never trigger this warning: they are
   * the public surface of a module. */
  for (size_t i = 0; i < node->rawImports.size(); ++i) {
    ImportCandidate cand;
    cand.path = std::string(node->rawImports[i]);
    cand.file = filePath;
    if (i < node->importInfo.size()) {
      cand.line = node->importInfo[i].line;
      cand.column = node->importInfo[i].column;
      cand.endLine = node->importInfo[i].endLine;
      cand.endColumn = node->importInfo[i].endColumn;
      cand.resolved = node->importInfo[i].resolvedModule;
    }

    /* A re-export ('export "x"' next to 'import "x"') counts as a use. */
    bool reExported = false;
    for (std::string_view exp : node->rawExports) {
      if (exp == node->rawImports[i]) {
        reExported = true;
        break;
      }
    }
    if (!reExported && cand.resolved)
      importCandidates.push_back(std::move(cand));
  }

  auto prevFile = ctx->currentFile;
  auto prevMod = ctx->currentModule;
  ctx->setCurrentFile(node->filePath);
  ctx->currentModule = node;

  for (const auto *imp : node->importedModules)
    dispatch(imp);
  for (const auto *exp : node->exportedModules)
    dispatch(exp);
  for (const auto &stmt : node->statements)
    dispatch(stmt);

  /* Instantiated templates are used by construction; visit them only to
   * collect their (resolved) usages, never to report candidates. */
  for (const auto *t : node->instantiatedTemplates) {
    collectingInstantiated = true;
    dispatch(t);
  }
  collectingInstantiated = false;

  /* Report while the context still points at this module: diagnostics carry
   * ctx->currentFile as their source file. */
  reportFile(node, filePath);

  ctx->setCurrentFile(prevFile);
  ctx->currentModule = prevMod;
}

void UnusedCodePass::reportFile(const ModuleNode *node,
                                const std::string &filePath) {
  if (reportedFiles.contains(filePath))
    return;
  reportedFiles.insert(filePath);

  /* Imports: used when a symbol of the imported module tree was referenced
   * from this file (the prelude is excluded: it is injected automatically). */
  const auto &fileRefs = usedFilesByFile[filePath];
  for (auto &cand : importCandidates) {
    if (cand.file != filePath || !cand.resolved)
      continue;

    auto cacheIt = moduleFileCache.find(cand.resolved);
    if (cacheIt == moduleFileCache.end()) {
      std::unordered_set<std::string> files;
      std::unordered_set<const ModuleNode *> visited;
      collectModuleFiles(cand.resolved, files, visited);
      cacheIt = moduleFileCache.emplace(cand.resolved, std::move(files)).first;
    }

    bool used = false;
    for (const auto &f : cacheIt->second) {
      if (fileRefs.contains(f)) {
        used = true;
        break;
      }
    }
    if (used)
      continue;

    int length = (cand.endColumn > cand.column)
                     ? cand.endColumn - cand.column
                     : (int)cand.path.size() + 10;
    ctx->reportWarning(WarningKind::UnusedImport, cand.line, cand.column,
                       length, "Import '" + cand.path + "' is never used.",
                       cand.endLine, true);
  }

  /* Using directives: used when a referenced declaration's fully qualified
   * name starts with the directive's namespace. */
  const auto &fileFqNames = usedFqNamesByFile[filePath];
  for (const auto &cand : usingCandidates) {
    if (cand.file != filePath)
      continue;
    std::string prefix = cand.ns + ".";
    bool used = false;
    for (const auto &fq : fileFqNames) {
      if (fq.starts_with(prefix)) {
        used = true;
        break;
      }
    }
    if (!used) {
      ctx->reportWarning(WarningKind::UnusedUsing, cand.line, cand.column,
                         cand.length,
                         "Using directive for '" + cand.ns +
                             "' never resolves a referenced symbol.",
                         0, true);
    }
  }

  /* Functions / methods / records / variables. */
  for (const auto &cand : declCandidates) {
    if (cand.file != filePath)
      continue;
    if (usedDecls.contains(cand.decl))
      continue;

    std::string name;
    if (auto *fn = llvm::dyn_cast<FunctionDeclNode>(cand.decl))
      name = std::string(fn->name);
    else if (auto *var = llvm::dyn_cast<VarDeclNode>(cand.decl))
      name = std::string(var->varName);
    else if (auto *rec = llvm::dyn_cast<StructDeclNode>(cand.decl))
      name = std::string(rec->name);
    else if (auto *cls = llvm::dyn_cast<ClassDeclNode>(cand.decl))
      name = std::string(cls->name);
    else if (auto *uni = llvm::dyn_cast<UnionDeclNode>(cand.decl))
      name = std::string(uni->name);
    else if (auto *en = llvm::dyn_cast<EnumDeclNode>(cand.decl))
      name = std::string(en->name);
    else if (auto *td = llvm::dyn_cast<TypedefDeclNode>(cand.decl))
      name = std::string(td->aliasName);

    WarningKind kind = WarningKind::None;
    std::string msg;
    if (cand.kindName == "function") {
      kind = WarningKind::UnusedFunction;
      msg = "Private function '" + name + "' is never called.";
    } else if (cand.kindName == "method") {
      kind = WarningKind::UnusedMethod;
      msg = "Private method '" + name + "' is never called.";
    } else if (cand.kindName == "record") {
      kind = WarningKind::UnusedType;
      msg = "Private type '" + name + "' is never referenced.";
    } else {
      kind = WarningKind::UnusedVariable;
      msg = "Variable '" + name + "' is declared but never used.";
    }
    /* Position the diagnostic at the declaration's identifier: the decl's
     * own column points at its return type / modifier prefix. */
    int col = cand.decl->identifierColumn > 0 ? cand.decl->identifierColumn
                                              : cand.decl->column;
    int len = cand.decl->identifierLength > 0 ? cand.decl->identifierLength
                                              : std::max(1, cand.decl->length);
    ctx->reportWarning(kind, cand.decl->line, col, len, msg,
                       cand.decl->endLine, true);
  }

  /* Private fields. */
  for (const auto &cand : fieldCandidates) {
    if (cand.file != filePath)
      continue;
    if (usedDecls.contains(cand.field))
      continue;
    int col = cand.field->identifierColumn > 0 ? cand.field->identifierColumn
                                               : cand.field->column;
    int len = cand.field->identifierLength > 0 ? cand.field->identifierLength
                                               : std::max(1, cand.field->length);
    ctx->reportWarning(WarningKind::UnusedField, cand.field->line, col, len,
                       "Private field '" + std::string(cand.field->varName) +
                           "' is never referenced.",
                       cand.field->endLine, true);
  }

  /* Parameters. */
  for (const auto &cand : paramCandidates) {
    if (cand.file != filePath)
      continue;
    if (usedDecls.contains(cand.param))
      continue;
    int col = cand.param->identifierColumn > 0 ? cand.param->identifierColumn
                                               : cand.param->column;
    int len = cand.param->identifierLength > 0 ? cand.param->identifierLength
                                               : std::max(1, cand.param->length);
    ctx->reportWarning(WarningKind::UnusedParameter, cand.param->line, col, len,
                       "Parameter '" + std::string(cand.param->name) +
                           "' is never used.",
                       cand.param->endLine, true);
  }
}

void UnusedCodePass::visit(const FunctionDeclNode *node) {
  recordTypeUsage(node->returnType);

  bool wasTemplate = node->isTemplate;
  if (wasTemplate)
    templateDepth++;

  for (const auto *p : node->params)
    dispatch(p);

  if (!suppressCandidates()) {
    if (node->isMethod && isPrivateMethod(node)) {
      declCandidates.push_back(
          {node, std::string(ctx->currentFile), "method"});
    } else if (!node->isMethod && isPrivateFunc(node)) {
      declCandidates.push_back(
          {node, std::string(ctx->currentFile), "function"});
    }
  }

  /* Parameters of virtual/override/abstract/operator/extern functions are
   * part of the interface contract: never warn about them. */
  bool paramsExempt = node->isVirtual || node->isOverride || node->isAbstract ||
                      node->isExtern || node->isIntrinsic || node->isImplicit ||
                      node->name.starts_with("operator") || !node->body;
  if (!paramsExempt) {
    for (const auto *p : node->params)
      addParamCandidate(p);
  }

  if (node->superCall)
    dispatch(node->superCall);
  for (const auto *init : node->fieldInitializers)
    dispatch(init);
  if (node->body)
    dispatch(node->body);

  if (wasTemplate)
    templateDepth--;
}

void UnusedCodePass::visit(const VarDeclNode *node) {
  recordTypeUsage(node->type);
  if (node->initializer)
    dispatch(node->initializer);

  if (suppressCandidates())
    return;

  if (node->isExtern || node->isWeak || node->varName == "_")
    return;

  if (node->isGlobal) {
    if (node->isPrivate(node->varName))
      declCandidates.push_back(
          {node, std::string(ctx->currentFile), "variable"});
  } else {
    declCandidates.push_back({node, std::string(ctx->currentFile), "variable"});
  }
}

void UnusedCodePass::visit(const ParamDeclNode *node) {
  recordTypeUsage(node->type);
  if (node->defaultValue)
    dispatch(node->defaultValue);
}

void UnusedCodePass::visit(const UsingNode *node) {
  UsingCandidate cand;
  cand.ns = std::string(node->name);
  cand.file = std::string(ctx->currentFile);
  cand.line = node->line;
  cand.column = node->column;
  cand.length = node->length;
  usingCandidates.push_back(std::move(cand));
}

void UnusedCodePass::visit(const NamespaceDeclNode *node) {
  for (const auto &stmt : node->statements)
    dispatch(stmt);
}

void UnusedCodePass::visit(const StructDeclNode *node) {
  visitRecord(node, node->fields, node->methods, node->constructors,
              node->destructor);
}

void UnusedCodePass::visit(const ClassDeclNode *node) {
  recordTypeUsage(node->baseClass);
  for (const Type *iface : node->interfaces)
    recordTypeUsage(iface);
  visitRecord(node, node->fields, node->methods, node->constructors,
              node->destructor);
}

void UnusedCodePass::visit(const UnionDeclNode *node) {
  visitRecord(node, node->fields, node->methods, node->constructors,
              node->destructor);
}

/* Shared record handling: the record itself as an unused-type candidate,
 * field types/initializers, private fields, and method bodies. */
void UnusedCodePass::visitRecord(
    const DeclNode *node, llvm::ArrayRef<VarDeclNode *> fields,
    llvm::ArrayRef<FunctionDeclNode *> methods,
    llvm::ArrayRef<FunctionDeclNode *> constructors,
    FunctionDeclNode *destructor) {
  size_t dot = node->fqName.find_last_of('.');
  std::string_view simpleName =
      (dot == std::string_view::npos) ? node->fqName
                                      : node->fqName.substr(dot + 1);

  bool wasTemplate = node->isTemplate;
  if (wasTemplate)
    templateDepth++;

  if (!suppressCandidates() && !wasTemplate && node->isPrivate(simpleName)) {
    declCandidates.push_back({node, std::string(ctx->currentFile), "record"});
  }

  for (const auto *f : fields) {
    recordTypeUsage(f->type);
    if (f->initializer)
      dispatch(f->initializer);
    if (!suppressCandidates() && f->isPrivate(f->varName)) {
      fieldCandidates.push_back({f, std::string(ctx->currentFile)});
    }
  }

  for (const auto *m : methods) {
    if (m->isTemplate)
      continue;
    recordTypeUsage(m->returnType);
    for (const auto *p : m->params)
      dispatch(p);

    bool exempt = m->isVirtual || m->isOverride || m->isAbstract ||
                  m->isExtern || m->isIntrinsic || m->isImplicit ||
                  m->name.starts_with("operator") || m->name == "~" ||
                  !m->body;
    if (!exempt && !suppressCandidates() && isPrivateMethod(m)) {
      declCandidates.push_back({m, std::string(ctx->currentFile), "method"});
    }
    if (!exempt && !suppressCandidates()) {
      for (const auto *p : m->params)
        addParamCandidate(p);
    }

    dispatchFunctionBody(m);
  }

  /* Constructor bodies are real code (field initializers, _seedFromTime()
   * calls, ...): their usages must be collected even though constructors
   * themselves are never "unused" candidates. */
  for (const auto *ctor : constructors) {
    if (ctor->isTemplate)
      continue;
    recordTypeUsage(ctor->returnType);
    for (const auto *p : ctor->params)
      dispatch(p);
    dispatchFunctionBody(ctor);
  }

  if (destructor) {
    recordTypeUsage(destructor->returnType);
    for (const auto *p : destructor->params)
      dispatch(p);
    dispatchFunctionBody(destructor);
  }

  if (wasTemplate)
    templateDepth--;
}

/* The shared body traversal of a function-like declaration. */
void UnusedCodePass::dispatchFunctionBody(const FunctionDeclNode *fn) {
  if (fn->superCall)
    dispatch(fn->superCall);
  for (const auto *init : fn->fieldInitializers)
    dispatch(init);
  if (fn->body)
    dispatch(fn->body);
}

void UnusedCodePass::visit(const EnumDeclNode *node) {
  recordTypeUsage(node->underlyingType);
  for (const auto *m : node->members) {
    if (m->initializer)
      dispatch(m->initializer);
  }
  if (!suppressCandidates() && !node->isTemplate && node->isPrivate(node->name)) {
    declCandidates.push_back({node, std::string(ctx->currentFile), "record"});
  }
}

void UnusedCodePass::visit(const TypedefDeclNode *node) {
  recordTypeUsage(node->targetType);
  recordTypeUsage(node->aliasType);
  if (!suppressCandidates() && node->isPrivate(node->aliasName)) {
    declCandidates.push_back({node, std::string(ctx->currentFile), "record"});
  }
}

void UnusedCodePass::visit(const LambdaNode *node) {
  for (const auto *p : node->params) {
    recordTypeUsage(p->type);
    if (p->defaultValue)
      dispatch(p->defaultValue);
    addParamCandidate(p);
  }
  /* Captured enclosing variables are read when the closure runs, so they
   * are used regardless of the lambda body's own walk (the body resolves
   * them to the environment slots, not the outer declarations). */
  for (const auto &cap : node->captures)
    markUsed(cap.decl);
  if (node->body)
    dispatch(node->body);
  if (node->exprBody)
    dispatch(node->exprBody);
}

void UnusedCodePass::visit(const BlockNode *node) {
  for (const auto &stmt : node->statements)
    dispatch(stmt);
}

void UnusedCodePass::visit(const IfNode *node) {
  dispatch(node->condition);
  dispatch(node->thenBlock);
  if (node->elseBlock)
    dispatch(node->elseBlock);
}

void UnusedCodePass::visit(const ForNode *node) {
  if (node->initStatement)
    dispatch(node->initStatement);
  dispatch(node->condition);
  if (node->increment)
    dispatch(node->increment);
  dispatch(node->body);
}

void UnusedCodePass::visit(const ForInNode *node) {
  /* Runs after TypeCheckPass: the loop variable usage is tracked through
   * the lowered form (declaration + while loop). */
  if (node->desugared) {
    dispatch(node->desugared);
    return;
  }
  dispatch(node->loopVar);
  dispatch(node->iterable);
  dispatch(node->body);
}

void UnusedCodePass::visit(const WhileNode *node) {
  dispatch(node->condition);
  dispatch(node->body);
}

void UnusedCodePass::visit(const SwitchNode *node) {
  dispatch(node->condition);
  for (const auto *c : node->cases)
    dispatch(c);
}

void UnusedCodePass::visit(const CaseNode *node) {
  dispatch(node->value);
  for (const auto &stmt : node->statements)
    dispatch(stmt);
}

void UnusedCodePass::visit(const TryStmtNode *node) {
  dispatch(node->body);
  for (const auto *clause : node->clauses) {
    recordTypeUsage(clause->catchType);
    dispatch(clause->body);
  }
}

void UnusedCodePass::visit(const ThrowStmtNode *node) {
  if (node->value)
    dispatch(node->value);
}

void UnusedCodePass::visit(const AssertStmtNode *node) {
  dispatch(node->condition);
}

void UnusedCodePass::visit(const ReturnNode *node) {
  if (node->value)
    dispatch(node->value);
}

void UnusedCodePass::visit(const FunctionCallNode *node) {
  markUsed(node->resolvedFunc);
  if (!node->resolvedFunc) {
    /* Template bodies are never type-checked, so their call targets stay
     * unresolved; fall back to name lookup (covers library functions like
     * malloc/free used only from templates). */
    if (auto *var = llvm::dyn_cast_or_null<VariableNode>(node->target)) {
      auto decls = ctx->lookup(var->name);
      for (const DeclNode *d : decls) {
        if (auto *fn = llvm::dyn_cast<FunctionDeclNode>(d)) {
          markUsed(fn);
          break;
        }
      }
    }
  }
  dispatch(node->target);
  for (const auto *arg : node->args)
    dispatch(arg);
}

void UnusedCodePass::visit(const VariableNode *node) {
  markUsed(node->resolvedDecl);
  recordTypeUsage(node->exprType);
}

void UnusedCodePass::visit(const MemberAccessNode *node) {
  markUsed(node->resolvedMethod);
  markUsed(node->resolvedDecl);
  markUsed(node->enumMember);
  dispatch(node->object);
}

void UnusedCodePass::visit(const AssignNode *node) {
  markUsed(node->overloadedOperator);
  dispatch(node->target);
  dispatch(node->value);
}

void UnusedCodePass::visit(const UnaryOpNode *node) {
  markUsed(node->overloadedOperator);
  dispatch(node->expr);
}

void UnusedCodePass::visit(const BinaryOpNode *node) {
  markUsed(node->overloadedOperator);
  dispatch(node->left);
  dispatch(node->right);
}

void UnusedCodePass::visit(const TernaryOpNode *node) {
  dispatch(node->condition);
  dispatch(node->trueExpr);
  dispatch(node->falseExpr);
}

void UnusedCodePass::visit(const CastNode *node) {
  recordTypeUsage(node->targetType);
  dispatch(node->expr);
}

void UnusedCodePass::visit(const IsExprNode *node) {
  recordTypeUsage(node->targetType);
  dispatch(node->expr);
}

void UnusedCodePass::visit(const NewExprNode *node) {
  recordTypeUsage(node->allocatedType);
  if (node->arraySize)
    dispatch(node->arraySize);
  for (const auto *arg : node->args)
    dispatch(arg);
  if (node->placementExpr)
    dispatch(node->placementExpr);
}

void UnusedCodePass::visit(const DeleteExprNode *node) {
  dispatch(node->ptr);
}

void UnusedCodePass::visit(const ConstExprNode *node) {
  dispatch(node->expr);
}

void UnusedCodePass::visit(const DestructorCallNode *node) {
  markUsed(node->destructor);
  dispatch(node->object);
}

void UnusedCodePass::visit(const ArraySubscriptNode *node) {
  markUsed(node->overloadedOperator);
  dispatch(node->base);
  dispatch(node->index);
}

void UnusedCodePass::visit(const ArrayLiteralNode *node) {
  for (const auto *e : node->elements)
    dispatch(e);
}

void UnusedCodePass::visit(const MapLiteralNode *node) {
  for (const auto *k : node->keys)
    dispatch(k);
  for (const auto *v : node->values)
    dispatch(v);
}

void UnusedCodePass::visit(const AwaitExprNode *node) {
  dispatch(node->expr);
}

void UnusedCodePass::visit(const ImplicitCastNode *node) {
  recordTypeUsage(node->targetType);
  dispatch(node->expr);
}

void UnusedCodePass::visit(const TypeLiteralNode *node) {
  recordTypeUsage(node->representedType);
}

} // namespace utopia
