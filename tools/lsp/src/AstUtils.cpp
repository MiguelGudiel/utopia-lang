#include "LspCore.hpp"
#include <algorithm>
#include <cctype>

namespace utopia::lsp {

SourceLocation getExactNameLocation(const std::string &text,
                                    const DeclNode *decl) {
  if (!decl) {
    return {-1, -1, 0};
  }

  if (decl->identifierColumn > 0 && decl->identifierLength > 0) {
    return {decl->line > 0 ? decl->line - 1 : 0, decl->identifierColumn - 1,
            decl->identifierLength};
  }

  SourceLocation loc{decl->line > 0 ? decl->line - 1 : 0,
                     decl->column > 0 ? decl->column - 1 : 0,
                     decl->length > 0 ? decl->length : 1};
  if (text.empty() || decl->line <= 0)
    return loc;

  std::string_view name;
  if (auto *varDecl = llvm::dyn_cast<VarDeclNode>(decl))
    name = varDecl->varName;
  else if (auto *funcDecl = llvm::dyn_cast<FunctionDeclNode>(decl))
    name = funcDecl->name;
  else if (auto *paramDecl = llvm::dyn_cast<ParamDeclNode>(decl))
    name = paramDecl->name;
  else if (auto *structDecl = llvm::dyn_cast<StructDeclNode>(decl))
    name = structDecl->name;
  else if (auto *classDecl = llvm::dyn_cast<ClassDeclNode>(decl))
    name = classDecl->name;
  else if (auto *unionDecl = llvm::dyn_cast<UnionDeclNode>(decl))
    name = unionDecl->name;
  else if (auto *enumDecl = llvm::dyn_cast<EnumDeclNode>(decl))
    name = enumDecl->name;
  else if (auto *typedefDecl = llvm::dyn_cast<TypedefDeclNode>(decl))
    name = typedefDecl->aliasName;
  else if (auto *enumMem = llvm::dyn_cast<EnumMemberNode>(decl))
    name = enumMem->name;
  else if (auto *annDecl = llvm::dyn_cast<AnnotationDeclNode>(decl))
    name = annDecl->name;
  else if (auto *nsDecl = llvm::dyn_cast<NamespaceDeclNode>(decl))
    name = nsDecl->name;

  if (name.empty())
    return loc;

  int currentLine = 0;
  size_t startIdx = 0;
  for (size_t i = 0; i < text.length(); ++i) {
    if (currentLine == loc.line) {
      startIdx = i + loc.col;
      break;
    }
    if (text[i] == '\n')
      currentLine++;
  }

  if (startIdx >= text.length())
    return loc;

  size_t maxSearchLen = std::min(text.length() - startIdx, (size_t)150);
  std::string_view searchArea(text.data() + startIdx, maxSearchLen);

  size_t searchPos = 0;
  while ((searchPos = searchArea.find(name, searchPos)) !=
         std::string_view::npos) {
    bool leftOk = searchPos == 0 || (!std::isalnum(searchArea[searchPos - 1]) &&
                                     searchArea[searchPos - 1] != '_');
    bool rightOk = searchPos + name.length() >= searchArea.length() ||
                   (!std::isalnum(searchArea[searchPos + name.length()]) &&
                    searchArea[searchPos + name.length()] != '_');

    if (leftOk && rightOk) {
      int newCol = 0;
      int newLine = 0;
      for (size_t i = 0; i < startIdx + searchPos; ++i) {
        if (text[i] == '\n') {
          newLine++;
          newCol = 0;
        } else {
          newCol++;
        }
      }
      return {newLine, newCol, (int)name.length()};
    }
    searchPos += name.length();
  }

  return {loc.line, loc.col, (int)name.length()};
}

std::string resolveModuleUriLsp(const ModuleLoaderConfig &config,
                                const std::string &uri,
                                const std::filesystem::path &currentDir) {
  if (uri == "utopia:builder") {
    std::filesystem::path target = config.buildLibRoot / "builder.utp";
    return std::filesystem::exists(target) ? target.string() : "";
  }
  if (uri == "prelude") {
    std::filesystem::path target = config.preludeRoot / "prelude.utp";
    return std::filesystem::exists(target) ? target.string() : "";
  }
  if (uri.starts_with("utopia:")) {
    std::string libName = uri.substr(7);
    std::filesystem::path target = config.stdlibRoot / libName;
    if (target.extension() != ".utp")
      target += ".utp";
    return std::filesystem::exists(target) ? target.string() : "";
  }
  if (uri.starts_with("package:")) {
    std::string pkgPath = uri.substr(8);
    size_t slashPos = pkgPath.find('/');
    std::string pkgName = pkgPath.substr(0, slashPos);
    std::string subPath =
        (slashPos != std::string::npos) ? pkgPath.substr(slashPos + 1) : pkgPath;
    auto it = config.packages.find(pkgName);
    if (it != config.packages.end()) {
      std::filesystem::path target =
          std::filesystem::path(it->second) / subPath;
      if (target.extension() != ".utp")
        target += ".utp";
      if (std::filesystem::exists(target))
        return std::filesystem::weakly_canonical(target).string();
    }
    return "";
  }

  std::filesystem::path target(uri);
  if (target.extension() != ".utp")
    target += ".utp";
  std::filesystem::path candidate =
      std::filesystem::weakly_canonical(currentDir / target);
  if (std::filesystem::exists(candidate))
    return candidate.string();
  if (std::filesystem::exists(target))
    return std::filesystem::weakly_canonical(target).string();
  return "";
}

const DeclNode *getTypeDeclaration(const Type *ty) {
  if (!ty)
    return nullptr;
  const Type *unqual = ty->getUnqualifiedType();

  while (unqual->isPointerType())
    unqual = static_cast<const PointerType *>(unqual)
                 ->getPointeeType()
                 ->getUnqualifiedType();

  while (unqual->isReferenceType() ||
         unqual->getKind() == TypeKind::RValueReference) {
    if (unqual->isReferenceType())
      unqual = static_cast<const ReferenceType *>(unqual)
                   ->getPointeeType()
                   ->getUnqualifiedType();
    else
      unqual = static_cast<const RValueReferenceType *>(unqual)
                   ->getPointeeType()
                   ->getUnqualifiedType();
  }

  while (unqual->getKind() == TypeKind::Array)
    unqual = static_cast<const ArrayType *>(unqual)
                 ->getElementType()
                 ->getUnqualifiedType();

  if (unqual->getKind() == TypeKind::Class ||
      unqual->getKind() == TypeKind::Struct ||
      unqual->getKind() == TypeKind::Union) {
    return static_cast<const RecordType *>(unqual)->getDeclaration();
  } else if (unqual->getKind() == TypeKind::Enum) {
    return static_cast<const EnumType *>(unqual)->getDeclaration();
  } else if (unqual->getKind() == TypeKind::Alias) {
    return static_cast<const AliasType *>(unqual)->getDeclaration();
  }
  return nullptr;
}

const DeclNode *resolveWithCollector(const std::string &name,
                                     const LocalVarCollector &collector,
                                     SemaContext *sema,
                                     const ModuleNode *root) {
  if (!sema)
    return nullptr;

  /* Locals and parameters of the enclosing function shadow globals. */
  if (collector.closestFunc) {
    for (const auto *p : collector.closestFunc->params) {
      if (p->name == name)
        return p;
    }
  }
  for (const auto *l : collector.locals) {
    if (l->varName == name)
      return l;
  }

  auto decls = sema->symTable.lookupExact(name, root);
  if (!decls.empty())
    return decls.front();

  std::string ns = collector.currentNamespace;
  while (!ns.empty()) {
    decls = sema->symTable.lookupExact(ns + "." + name, root);
    if (!decls.empty())
      return decls.front();
    size_t pos = ns.find_last_of('.');
    if (pos != std::string::npos)
      ns = ns.substr(0, pos);
    else
      break;
  }

  for (const auto &u : collector.activeUsings) {
    decls = sema->symTable.lookupExact(u + "." + name, root);
    if (!decls.empty())
      return decls.front();
  }
  return nullptr;
}

std::string getHoverTextForDecl(const DeclNode *decl) {
  if (!decl)
    return "";
  std::string text;

  if (auto *classDecl = llvm::dyn_cast<ClassDeclNode>(decl)) {
    std::string prefix = classDecl->isAbstract ? "abstract class " : "class ";
    text = "```utopia\n" + prefix + std::string(classDecl->name) + "\n```";
  } else if (auto *structDecl = llvm::dyn_cast<StructDeclNode>(decl)) {
    text = "```utopia\nstruct " + std::string(structDecl->name) + "\n```";
  } else if (auto *unionDecl = llvm::dyn_cast<UnionDeclNode>(decl)) {
    text = "```utopia\nunion " + std::string(unionDecl->name) + "\n```";
  } else if (auto *enumDecl = llvm::dyn_cast<EnumDeclNode>(decl)) {
    text = "```utopia\nenum " + std::string(enumDecl->name) + "\n```";
  } else if (auto *typedefDecl = llvm::dyn_cast<TypedefDeclNode>(decl)) {
    text =
        "```utopia\ntypedef " + std::string(typedefDecl->aliasName) + "\n```";
  } else if (auto *annDecl = llvm::dyn_cast<AnnotationDeclNode>(decl)) {
    text = "```utopia\nannotation " + std::string(annDecl->name) + "\n```";
  } else if (auto *nsDecl = llvm::dyn_cast<NamespaceDeclNode>(decl)) {
    text = "```utopia\nnamespace " + std::string(nsDecl->fqName) + "\n```";
  }

  if (!decl->docString.empty())
    text += "\n---\n" + std::string(decl->docString);

  return text;
}

std::string formatFunctionSignature(const FunctionDeclNode *func) {
  std::string sig = "";

  for (auto *ann : func->annotations) {
    sig += "@" + std::string(ann->name);
    if (!ann->args.empty()) {
      sig += "(...)";
    }
    sig += "\n";
  }

  if (func->isExtern)
    sig += "extern ";

  if (func->returnType && !func->isImplicit) {
    if (!func->rawReturnTypeStr.empty()) {
      sig += std::string(func->rawReturnTypeStr) + " ";
    } else {
      sig += func->returnType->toString() + " ";
    }
  }

  sig += std::string(func->name);

  if (func->isTemplate && !func->templateParams.empty()) {
    sig += "<";
    for (size_t i = 0; i < func->templateParams.size(); ++i) {
      sig += std::string(func->templateParams[i]);
      if (i + 1 < func->templateParams.size())
        sig += ", ";
    }
    sig += ">";
  }

  sig += "(";
  bool inNamed = false;
  bool firstParam = true;

  for (size_t i = 0; i < func->params.size(); ++i) {
    const ParamDeclNode *p = func->params[i];

    /* Skip the implicit instance pointer for methods */
    if (p->name == "this")
      continue;

    if (p->isNamed && !inNamed) {
      if (!firstParam)
        sig += ", ";
      sig += "{";
      inNamed = true;
      firstParam = true;
    } else {
      if (!firstParam)
        sig += ", ";
    }

    firstParam = false;

    if (p->isRequired)
      sig += "required ";

    if (!p->rawTypeStr.empty()) {
      sig += std::string(p->rawTypeStr) + " ";
    } else if (p->type) {
      sig += p->type->toString() + " ";
    }

    sig += std::string(p->name);

    if (p->defaultValue) {
      std::string defStr = Formatter::format(p->defaultValue);
      while (!defStr.empty() &&
             (defStr.back() == '\n' || defStr.back() == '\r' ||
              defStr.back() == ';')) {
        defStr.pop_back();
      }
      sig += " = " + defStr;
    }
  }

  if (func->isVariadic) {
    if (!firstParam)
      sig += ", ";
    sig += "...";
  }

  if (inNamed)
    sig += "}";
  sig += ")";

  if (func->isConst && func->isMethod) {
    sig += " const";
  }

  return sig;
}

std::vector<const FunctionDeclNode *> getOverloads(const FunctionDeclNode *func,
                                                   SemaContext *sema) {
  std::vector<const FunctionDeclNode *> overloads;
  if (!func)
    return overloads;

  if (func->parentRecord) {
    const DeclNode *recDecl = func->parentRecord->getDeclaration();
    if (recDecl) {
      llvm::ArrayRef<FunctionDeclNode *> methods;
      llvm::ArrayRef<FunctionDeclNode *> ctors;

      if (recDecl->kind == NodeKind::ClassDecl) {
        methods = static_cast<const ClassDeclNode *>(recDecl)->methods;
        ctors = static_cast<const ClassDeclNode *>(recDecl)->constructors;
      } else if (recDecl->kind == NodeKind::StructDecl) {
        methods = static_cast<const StructDeclNode *>(recDecl)->methods;
        ctors = static_cast<const StructDeclNode *>(recDecl)->constructors;
      } else if (recDecl->kind == NodeKind::UnionDecl) {
        methods = static_cast<const UnionDeclNode *>(recDecl)->methods;
        ctors = static_cast<const UnionDeclNode *>(recDecl)->constructors;
      }

      if (func->name == func->parentRecord->getName()) {
        for (auto *c : ctors) {
          if (!c->isImplicit)
            overloads.push_back(c);
        }
      } else {
        for (auto *m : methods) {
          if (m->name == func->name)
            overloads.push_back(m);
        }
      }
    }
  } else if (sema) {
    auto results = sema->lookup(func->name);
    for (const DeclNode *d : results) {
      if (d->kind == NodeKind::FunctionDecl) {
        overloads.push_back(static_cast<const FunctionDeclNode *>(d));
      }
    }
  }

  if (std::find(overloads.begin(), overloads.end(), func) == overloads.end()) {
    overloads.insert(overloads.begin(), func);
  }

  return overloads;
}

std::string buildFunctionHover(const FunctionDeclNode *targetFunc) {
  if (!targetFunc)
    return "";
  std::string res =
      "```utopia\n" + formatFunctionSignature(targetFunc) + "\n```";
  if (!targetFunc->docString.empty()) {
    res += "\n---\n" + std::string(targetFunc->docString);
  }
  return res;
}

const DeclNode *getAutoDerefTarget(const DeclNode *recDecl) {
  if (!recDecl)
    return nullptr;
  llvm::ArrayRef<FunctionDeclNode *> methods;
  if (recDecl->kind == NodeKind::ClassDecl)
    methods = static_cast<const ClassDeclNode *>(recDecl)->methods;
  else if (recDecl->kind == NodeKind::StructDecl)
    methods = static_cast<const StructDeclNode *>(recDecl)->methods;
  else if (recDecl->kind == NodeKind::UnionDecl)
    methods = static_cast<const UnionDeclNode *>(recDecl)->methods;
  else
    return nullptr;

  for (const auto *m : methods) {
    if (m->name == "operator*") {
      const Type *ret = m->returnType;
      if (!ret)
        return nullptr;
      const Type *unqual = ret->getUnqualifiedType();
      if (unqual->isPointerType()) {
        unqual = static_cast<const PointerType *>(unqual)
                     ->getPointeeType()
                     ->getUnqualifiedType();
      } else if (unqual->isReferenceType()) {
        unqual = static_cast<const ReferenceType *>(unqual)
                     ->getPointeeType()
                     ->getUnqualifiedType();
      } else if (unqual->getKind() == TypeKind::RValueReference) {
        unqual = static_cast<const RValueReferenceType *>(unqual)
                     ->getPointeeType()
                     ->getUnqualifiedType();
      }
      if (unqual->getKind() == TypeKind::Class ||
          unqual->getKind() == TypeKind::Struct ||
          unqual->getKind() == TypeKind::Union) {
        auto *recTy = static_cast<const RecordType *>(unqual);
        return recTy->getDeclaration();
      }
      return nullptr;
    }
  }
  return nullptr;
}

bool isMemberVisible(const DeclNode *member, const RecordType *memberRecord,
                     const RecordType *accessContext) {
  if (!member)
    return false;

  /* Explicit modifiers take precedence; a leading '_' makes an unmodified
   * member private (matches DeclNode::isPublic). Fields do not carry a
   * fqName, so the name must come from the node's own field. */
  std::string_view memberName = member->fqName;
  if (auto *v = llvm::dyn_cast<VarDeclNode>(member))
    memberName = v->varName;
  else if (auto *fn = llvm::dyn_cast<FunctionDeclNode>(member))
    memberName = fn->name;
  else if (auto *em = llvm::dyn_cast<EnumMemberNode>(member))
    memberName = em->name;

  size_t pos = memberName.find_last_of('.');
  if (pos != std::string_view::npos)
    memberName = memberName.substr(pos + 1);

  bool isPrivate = member->hasPrivateMod;
  bool isProtected = member->hasProtectedMod;
  if (!isPrivate && !isProtected && !member->hasPublicMod) {
    isPrivate = memberName.starts_with("_");
  }

  if (!isPrivate && !isProtected)
    return true;

  if (!accessContext || !memberRecord)
    return false;

  if (memberRecord == accessContext)
    return true;

  /* Protected members are visible from derived classes. */
  if (isProtected) {
    const RecordType *cur = accessContext;
    while (cur) {
      if (cur->getKind() == TypeKind::Class) {
        const Type *base = static_cast<const ClassType *>(cur)->getBaseClass();
        cur = base ? llvm::dyn_cast_or_null<RecordType>(
                         base->getUnqualifiedType())
                   : nullptr;
        if (cur == memberRecord)
          return true;
      } else {
        break;
      }
    }
  }
  return false;
}

GlobalSymbols collectGlobals(const ModuleNode *root) {
  GlobalSymbols out;
  if (!root)
    return out;

  std::unordered_set<const ModuleNode *> visitedMods;

  std::function<void(const ModuleNode *)> collectModule =
      [&](const ModuleNode *mod) {
        if (!mod || visitedMods.contains(mod))
          return;
        visitedMods.insert(mod);

        std::function<void(llvm::ArrayRef<ASTNode *>, const std::string &)>
            collectStmts = [&](llvm::ArrayRef<ASTNode *> stmts,
                               const std::string &currentNs) {
              for (const auto *stmt : stmts) {
                if (stmt->kind == NodeKind::NamespaceDecl) {
                  auto *nsDecl = static_cast<const NamespaceDeclNode *>(stmt);

                  std::string nsName = std::string(nsDecl->name);
                  std::string runningNs = currentNs;

                  /* Multi-part namespaces ('wow.Math') are broken into
                   * individual components so each level resolves. */
                  size_t start = 0;
                  while (true) {
                    size_t dot = nsName.find('.', start);
                    std::string part =
                        (dot == std::string::npos)
                            ? nsName.substr(start)
                            : nsName.substr(start, dot - start);

                    std::string nextNs =
                        runningNs.empty() ? part : runningNs + "." + part;

                    if (runningNs.empty()) {
                      if (std::find(out.rootGlobals.begin(),
                                    out.rootGlobals.end(),
                                    nsDecl) == out.rootGlobals.end()) {
                        out.rootGlobals.push_back(nsDecl);
                      }
                    } else {
                      auto &vec = out.namespaceMembers[runningNs];
                      if (std::find(vec.begin(), vec.end(), nsDecl) ==
                          vec.end()) {
                        vec.push_back(nsDecl);
                      }
                    }

                    runningNs = nextNs;
                    if (dot == std::string::npos)
                      break;
                    start = dot + 1;
                  }

                  collectStmts(nsDecl->statements, runningNs);
                } else if (stmt->kind == NodeKind::FunctionDecl ||
                           stmt->kind == NodeKind::VarDecl ||
                           stmt->kind == NodeKind::ClassDecl ||
                           stmt->kind == NodeKind::StructDecl ||
                           stmt->kind == NodeKind::UnionDecl ||
                           stmt->kind == NodeKind::EnumDecl ||
                           stmt->kind == NodeKind::TypedefDecl ||
                           stmt->kind == NodeKind::AnnotationDecl) {
                  if (currentNs.empty())
                    out.rootGlobals.push_back(
                        static_cast<const DeclNode *>(stmt));
                  else
                    out.namespaceMembers[currentNs].push_back(
                        static_cast<const DeclNode *>(stmt));
                }
              }
            };

        collectStmts(mod->statements, "");

        for (const auto *imp : mod->importedModules)
          collectModule(imp);
        for (const auto *exp : mod->exportedModules)
          collectModule(exp);
      };

  collectModule(root);
  return out;
}

} // namespace utopia::lsp
