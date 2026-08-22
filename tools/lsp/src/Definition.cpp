#include "LspCore.hpp"

namespace utopia::lsp {

namespace {

/* Locates the 'import "..."' / 'export "..."' statement whose string literal
 * covers (line, col) in the module, and returns the resolved module path of
 * that import. This powers go-to-file navigation from import/export lines. */
std::string resolveImportAt(const DocumentState &doc, int line, int col) {
  if (!doc.ast)
    return "";

  /* rawImports/rawExports keep the paths in source order; importedModules/
   * exportedModules hold the resolved modules in the same order. Match by
   * scanning the source line for the quoted string around the cursor. */
  const std::string &text = doc.text;
  int currentLine = 1;
  size_t lineStart = 0;
  for (size_t i = 0; i < text.length(); ++i) {
    if (currentLine == line) {
      lineStart = i;
      break;
    }
    if (text[i] == '\n')
      currentLine++;
  }

  size_t lineEnd = text.find('\n', lineStart);
  if (lineEnd == std::string::npos)
    lineEnd = text.length();
  std::string_view srcLine(text.data() + lineStart, lineEnd - lineStart);

  size_t quotePos = srcLine.find('"');
  if (quotePos == std::string_view::npos)
    return "";
  size_t quoteEnd = srcLine.find('"', quotePos + 1);
  if (quoteEnd == std::string_view::npos)
    return "";
  std::string_view pathStr = srcLine.substr(quotePos + 1, quoteEnd - quotePos - 1);

  /* Cursor must be inside the quoted string. */
  int quoteAbsCol = (int)quotePos + 1; /* 1-based */
  if (col < quoteAbsCol || col > quoteAbsCol + (int)pathStr.length())
    return "";

  bool isExport = srcLine.find("export") != std::string_view::npos;
  const auto &rawList = isExport ? doc.ast->rawExports : doc.ast->rawImports;
  const auto &modList =
      isExport ? doc.ast->exportedModules : doc.ast->importedModules;

  /* The loader prepends the prelude module to importedModules, so the index
   * of a path in rawList does not match its resolved module: locate the
   * module by resolved path instead. */
  std::string expectedPath;
  for (size_t i = 0; i < rawList.size(); ++i) {
    if (rawList[i] == pathStr) {
      if (i < modList.size() && modList[i]) {
        /* Try the exact index first (correct when the prelude was already
         * loaded and not re-prepended). */
        std::string candidate(modList[i]->filePath);
        if (candidate.ends_with(pathStr))
          return candidate;
      }
      break;
    }
  }
  for (const auto *mod : modList) {
    if (mod && mod->filePath.ends_with(pathStr))
      return std::string(mod->filePath);
  }

  /* Package/stdio URIs ('package:...', 'utopia:...', 'prelude') do not
   * appear as file-path suffixes: resolve them like the compiler does. */
  /* Copy through the string_view: the arena storage behind it is not
   * NUL-terminated, so data() alone would read into unrelated allocations. */
  std::string filePath(doc.ast->filePath);
  ModuleLoaderConfig config = documents.configFor(
      pathToUri(filePath), std::filesystem::path(filePath));
  std::string resolved = resolveModuleUriLsp(
      config, std::string(pathStr), std::filesystem::path(filePath).parent_path());
  if (!resolved.empty())
    return resolved;
  return "";
}

/* Resolves the declaration under the cursor following the same node kinds
 * the hover handler covers. */
const DeclNode *resolveDefinitionTarget(const DocumentState &doc, int line,
                                        int col, const ASTNode *node) {
  if (!node)
    return nullptr;

  if (auto *varNode = llvm::dyn_cast_or_null<VariableNode>(node)) {
    if (varNode->resolvedDecl)
      return varNode->resolvedDecl;
    if (varNode->isField && varNode->parentType) {
      auto recTy = static_cast<const RecordType *>(varNode->parentType);
      auto decl = recTy->getDeclaration();
      if (decl) {
        llvm::ArrayRef<VarDeclNode *> fields;
        if (auto *cDecl = llvm::dyn_cast<ClassDeclNode>(decl))
          fields = cDecl->fields;
        else if (auto *sDecl = llvm::dyn_cast<StructDeclNode>(decl))
          fields = sDecl->fields;
        else if (auto *uDecl = llvm::dyn_cast<UnionDeclNode>(decl))
          fields = uDecl->fields;
        for (auto *f : fields)
          if (f->varName == varNode->name)
            return f;
      }
    }
  } else if (auto *callNode = llvm::dyn_cast_or_null<FunctionCallNode>(node)) {
    return callNode->resolvedFunc;
  } else if (auto *ma = llvm::dyn_cast_or_null<MemberAccessNode>(node)) {
    if (ma->isMethodRef)
      return ma->resolvedMethod;
    if (ma->isStaticFieldRef)
      return ma->resolvedDecl;
    if (ma->isEnumMember)
      return ma->enumMember;
    if (ma->resolvedDecl)
      return ma->resolvedDecl;

    /* Unresolved member access: fall back to field lookup on the object's
     * record type. */
    const Type *baseTy = ma->object ? ma->object->exprType : nullptr;
    if (baseTy) {
      if (baseTy->isPointerType())
        baseTy = static_cast<const PointerType *>(baseTy)->getPointeeType();
      else if (baseTy->isReferenceType() ||
               baseTy->getKind() == TypeKind::RValueReference)
        baseTy = static_cast<const ReferenceType *>(baseTy)->getPointeeType();

      const Type *unqual = baseTy->getUnqualifiedType();
      if (unqual->getKind() == TypeKind::Class ||
          unqual->getKind() == TypeKind::Struct ||
          unqual->getKind() == TypeKind::Union) {
        auto recTy = static_cast<const RecordType *>(unqual);
        auto decl = recTy->getDeclaration();
        if (decl) {
          llvm::ArrayRef<VarDeclNode *> fields;
          if (auto *cDecl = llvm::dyn_cast<ClassDeclNode>(decl))
            fields = cDecl->fields;
          else if (auto *sDecl = llvm::dyn_cast<StructDeclNode>(decl))
            fields = sDecl->fields;
          else if (auto *uDecl = llvm::dyn_cast<UnionDeclNode>(decl))
            fields = uDecl->fields;
          for (auto *f : fields)
            if (f->varName == ma->memberName)
              return f;
        }
      }
    }
  } else if (auto *newNode = llvm::dyn_cast_or_null<NewExprNode>(node)) {
    return newNode->resolvedConstructor;
  } else if (auto *castNode = llvm::dyn_cast_or_null<CastNode>(node)) {
    return getTypeDeclaration(castNode->targetType);
  } else if (auto *declNode = llvm::dyn_cast_or_null<DeclNode>(node)) {
    auto loc = getExactNameLocation(doc.text, declNode);

    /* The cursor may sit on the declaration's type rather than its name. */
    if (col - 1 < loc.col) {
      const Type *t = nullptr;
      if (auto *varDecl = llvm::dyn_cast<VarDeclNode>(declNode))
        t = varDecl->type;
      else if (auto *funcDecl = llvm::dyn_cast<FunctionDeclNode>(declNode))
        t = funcDecl->returnType;
      else if (auto *paramDecl = llvm::dyn_cast<ParamDeclNode>(declNode))
        t = paramDecl->type;

      if (t) {
        if (auto typeDecl = getTypeDeclaration(t))
          return typeDecl;
        return nullptr;
      }
    }
    return declNode;
  }
  return nullptr;
}

} // namespace

void handleDefinition(const json &req) {
  syncWorker();
  std::string uri = req["params"]["textDocument"]["uri"];
  int line = req["params"]["position"]["line"].get<int>() + 1;
  int col = req["params"]["position"]["character"].get<int>() + 1;

  json res = json::array();

  DocumentState doc;
  if (documents.get(uri, doc) && doc.ast) {
    /* Go-to-file from import/export statements. */
    std::string importTarget = resolveImportAt(doc, line, col);
    if (!importTarget.empty()) {
      std::string targetText = documents.textFor(pathToUri(importTarget));
      res = {{"uri", pathToUri(importTarget)},
             {"range",
              {{"start", {{"line", 0}, {"character", 0}}},
               {"end", {{"line", 0}, {"character", 0}}}}}};
      (void)targetText;
    } else {
      SearchVisitor searcher(line, col, &doc.text);
      const ASTNode *node = searcher.find(doc.ast);
      const DeclNode *targetDecl = resolveDefinitionTarget(doc, line, col, node);

      if (targetDecl) {
        std::string targetUri = uri;
        std::string targetText = doc.text;

        if (!targetDecl->declFilePath.empty()) {
          targetUri = pathToUri(targetDecl->declFilePath);
          if (targetUri != uri) {
            targetText = documents.textFor(targetUri);
          }
        }

        auto loc = getExactNameLocation(targetText, targetDecl);
        int defLine = loc.line;
        int defCol = loc.col;
        int defLen = loc.length;

        res = {
            {"uri", targetUri},
            {"range",
             {{"start", {{"line", defLine}, {"character", defCol}}},
              {"end", {{"line", defLine}, {"character", defCol + defLen}}}}}};
      }
    }
  }
  sendResponse({{"jsonrpc", "2.0"}, {"id", requestId(req)}, {"result", res}});
}

void handleTypeDefinition(const json &req) {
  syncWorker();
  std::string uri = req["params"]["textDocument"]["uri"];
  int line = req["params"]["position"]["line"].get<int>() + 1;
  int col = req["params"]["position"]["character"].get<int>() + 1;

  json res = json::array();

  DocumentState doc;
  if (documents.get(uri, doc) && doc.ast) {
    SearchVisitor searcher(line, col, &doc.text);
    const ASTNode *node = searcher.find(doc.ast);
    const Type *type = nullptr;

    if (auto *varNode = llvm::dyn_cast_or_null<VariableNode>(node)) {
      if (varNode->resolvedDecl) {
        if (auto *v = llvm::dyn_cast<VarDeclNode>(varNode->resolvedDecl))
          type = v->type;
        else if (auto *p = llvm::dyn_cast<ParamDeclNode>(varNode->resolvedDecl))
          type = p->type;
      }
    } else if (auto *ma = llvm::dyn_cast_or_null<MemberAccessNode>(node)) {
      type = ma->exprType;
    } else if (auto *callNode = llvm::dyn_cast_or_null<FunctionCallNode>(node)) {
      type = callNode->exprType;
    }

    const DeclNode *typeDecl = type ? getTypeDeclaration(type) : nullptr;
    if (typeDecl) {
      std::string targetUri = uri;
      std::string targetText = doc.text;
      if (!typeDecl->declFilePath.empty()) {
        targetUri = pathToUri(typeDecl->declFilePath);
        if (targetUri != uri)
          targetText = documents.textFor(targetUri);
      }
      auto loc = getExactNameLocation(targetText, typeDecl);
      res = {
          {"uri", targetUri},
          {"range",
           {{"start", {{"line", loc.line}, {"character", loc.col}}},
            {"end", {{"line", loc.line}, {"character", loc.col + loc.length}}}}}};
    }
  }
  sendResponse({{"jsonrpc", "2.0"}, {"id", requestId(req)}, {"result", res}});
}

void handleImplementation(const json &req) {
  syncWorker();
  std::string uri = req["params"]["textDocument"]["uri"];
  int line = req["params"]["position"]["line"].get<int>() + 1;
  int col = req["params"]["position"]["character"].get<int>() + 1;

  json res = json::array();

  DocumentState doc;
  if (documents.get(uri, doc) && doc.ast) {
    SearchVisitor searcher(line, col, &doc.text);
    const ASTNode *node = searcher.find(doc.ast);
    const DeclNode *target = resolveDefinitionTarget(doc, line, col, node);
    if (!target)
      target = llvm::dyn_cast_or_null<DeclNode>(node);

    if (target) {
      /* For an interface/abstract method, find overrides in derived
       * classes. For everything else, fall back to the declaration. */
      std::string methodName;
      if (auto *fn = llvm::dyn_cast<FunctionDeclNode>(target))
        methodName = std::string(fn->name);

      GlobalSymbols globals = collectGlobals(doc.ast);

      auto addLocation = [&](const DeclNode *decl) {
        std::string targetUri = uri;
        std::string targetText = doc.text;
        if (!decl->declFilePath.empty()) {
          targetUri = pathToUri(decl->declFilePath);
          if (targetUri != uri)
            targetText = documents.textFor(targetUri);
        }
        auto loc = getExactNameLocation(targetText, decl);
        res.push_back(
            {{"uri", targetUri},
             {"range",
              {{"start", {{"line", loc.line}, {"character", loc.col}}},
               {"end",
                {{"line", loc.line}, {"character", loc.col + loc.length}}}}}});
      };

      if (!methodName.empty()) {
        for (const auto *decl : globals.rootGlobals) {
          auto *c = llvm::dyn_cast_or_null<ClassDeclNode>(decl);
          if (!c)
            continue;
          for (const auto *m : c->methods) {
            if (m->name == methodName && m != target) {
              addLocation(m);
            }
          }
        }
      }

      if (res.empty() && target) {
        addLocation(target);
      }
    }
  }
  sendResponse({{"jsonrpc", "2.0"}, {"id", requestId(req)}, {"result", res}});
}

} // namespace utopia::lsp
