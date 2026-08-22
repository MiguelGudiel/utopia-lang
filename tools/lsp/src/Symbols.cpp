#include "LspCore.hpp"

namespace utopia::lsp {

namespace {

/* Symbol kinds per LSP: file=1, module=2, namespace=3, package=4, class=5,
 * method=6, property=7, field=8, constructor=9, enum=10, interface=11,
 * function=12, variable=13, constant=14, string=15, number=16, boolean=17,
 * array=18, object=19, key=20, null=21, enumMember=22, struct=23, event=24,
 * operator=25, typeParameter=26. */
int symbolKindFor(const DeclNode *decl) {
  switch (decl->kind) {
  case NodeKind::ClassDecl:
    return 5;
  case NodeKind::StructDecl:
    return 23;
  case NodeKind::UnionDecl:
    return 23;
  case NodeKind::EnumDecl:
    return 10;
  case NodeKind::EnumMember:
    return 22;
  case NodeKind::FunctionDecl: {
    auto *fn = static_cast<const FunctionDeclNode *>(decl);
    if (fn->parentRecord &&
        fn->name == fn->parentRecord->getName()) {
      return 9; /* constructor */
    }
    return fn->isMethod ? 6 : 12;
  }
  case NodeKind::VarDecl: {
    auto *v = static_cast<const VarDeclNode *>(decl);
    if (v->isStatic)
      return 14; /* constant */
    if (v->isFinal)
      return 14;
    return 13; /* variable */
  }
  case NodeKind::ParamDecl:
    return 13;
  case NodeKind::TypedefDecl:
    return 23;
  case NodeKind::AnnotationDecl:
    return 5;
  case NodeKind::NamespaceDecl:
    return 3;
  default:
    return 12;
  }
}

/* The full range of a declaration ends where its last line ends: the AST's
 * 'length' is a span, not an absolute column, and VS Code rejects symbols
 * whose selectionRange is not contained in the full range. */
json symbolRangeFor(const std::string &docText, const DeclNode *decl,
                    const SourceLocation &sel) {
  int startLine = decl->line > 0 ? decl->line - 1 : 0;
  int startCol = decl->column > 0 ? decl->column - 1 : 0;
  int endLine = decl->endLine > decl->line ? decl->endLine - 1 : startLine;

  int endCol = 0;
  {
    int currentLine = 0;
    for (size_t i = 0; i < docText.length(); ++i) {
      if (currentLine == endLine) {
        endCol = (int)i;
        break;
      }
      if (docText[i] == '\n')
        currentLine++;
    }
    if (currentLine < endLine)
      endCol = (int)docText.length();
  }

  /* Containment guarantee: stretch the full range over the selection. */
  if (sel.line < startLine || (sel.line == startLine && sel.col < startCol)) {
    startLine = sel.line;
    startCol = sel.col;
  }
  int selEnd = sel.col + sel.length;
  if (sel.line > endLine || (sel.line == endLine && selEnd > endCol)) {
    endLine = sel.line;
    endCol = selEnd;
  }

  return {{"start", {{"line", startLine}, {"character", startCol}}},
          {"end", {{"line", endLine}, {"character", endCol}}}};
}

json symbolInfoFor(const std::string &docText, const DeclNode *decl) {
  auto loc = getExactNameLocation(docText, decl);
  std::string name;
  if (auto *fn = llvm::dyn_cast<FunctionDeclNode>(decl))
    name = std::string(fn->name);
  else if (auto *v = llvm::dyn_cast<VarDeclNode>(decl))
    name = std::string(v->varName);
  else if (auto *c = llvm::dyn_cast<ClassDeclNode>(decl))
    name = std::string(c->name);
  else if (auto *s = llvm::dyn_cast<StructDeclNode>(decl))
    name = std::string(s->name);
  else if (auto *u = llvm::dyn_cast<UnionDeclNode>(decl))
    name = std::string(u->name);
  else if (auto *e = llvm::dyn_cast<EnumMemberNode>(decl))
    name = std::string(e->name);
  else if (auto *n = llvm::dyn_cast<NamespaceDeclNode>(decl))
    name = std::string(n->name);
  else
    name = std::string(decl->fqName);

  return {{"name", name},
          {"kind", symbolKindFor(decl)},
          {"range", symbolRangeFor(docText, decl, loc)},
          {"selectionRange",
           {{"start", {{"line", loc.line}, {"character", loc.col}}},
            {"end", {{"line", loc.line},
                     {"character", loc.col + loc.length}}}}}};
}

/* Recursively builds the document-symbol tree for a record declaration. */
json recordSymbols(const std::string &docText, const DeclNode *rec) {
  json children = json::array();
  llvm::ArrayRef<VarDeclNode *> fields;
  llvm::ArrayRef<FunctionDeclNode *> methods;
  llvm::ArrayRef<FunctionDeclNode *> ctors;
  FunctionDeclNode *dtor = nullptr;

  if (rec->kind == NodeKind::ClassDecl) {
    auto *c = static_cast<const ClassDeclNode *>(rec);
    fields = c->fields;
    methods = c->methods;
    ctors = c->constructors;
    dtor = c->destructor;
  } else if (rec->kind == NodeKind::StructDecl) {
    auto *s = static_cast<const StructDeclNode *>(rec);
    fields = s->fields;
    methods = s->methods;
    ctors = s->constructors;
    dtor = s->destructor;
  } else if (rec->kind == NodeKind::UnionDecl) {
    auto *u = static_cast<const UnionDeclNode *>(rec);
    fields = u->fields;
    methods = u->methods;
    ctors = u->constructors;
    dtor = u->destructor;
  }

  for (auto *f : fields)
    if (f && !f->varName.empty())
      children.push_back(symbolInfoFor(docText, f));
  for (auto *c : ctors)
    if (!c->isImplicit)
      children.push_back(symbolInfoFor(docText, c));
  for (auto *m : methods)
    if (!m->isImplicit)
      children.push_back(symbolInfoFor(docText, m));
  if (dtor && !dtor->isImplicit)
    children.push_back(symbolInfoFor(docText, dtor));

  json info = symbolInfoFor(docText, rec);
  if (!children.empty())
    info["children"] = children;
  return info;
}

void collectSymbolsFromStatements(llvm::ArrayRef<ASTNode *> statements,
                                  const std::string &docText, json &out) {
  for (const auto *stmt : statements) {
    if (stmt->kind == NodeKind::FunctionDecl) {
      auto *fn = static_cast<const FunctionDeclNode *>(stmt);
      if (fn->isImplicit)
        continue;
      out.push_back(symbolInfoFor(docText, fn));
    } else if (stmt->kind == NodeKind::VarDecl) {
      out.push_back(
          symbolInfoFor(docText, static_cast<const VarDeclNode *>(stmt)));
    } else if (stmt->kind == NodeKind::ClassDecl ||
               stmt->kind == NodeKind::StructDecl ||
               stmt->kind == NodeKind::UnionDecl) {
      out.push_back(recordSymbols(docText, static_cast<const DeclNode *>(stmt)));
    } else if (stmt->kind == NodeKind::EnumDecl) {
      json children = json::array();
      auto *e = static_cast<const EnumDeclNode *>(stmt);
      for (auto *m : e->members)
        children.push_back(symbolInfoFor(docText, m));
      json info = symbolInfoFor(docText, e);
      if (!children.empty())
        info["children"] = children;
      out.push_back(info);
    } else if (stmt->kind == NodeKind::TypedefDecl ||
               stmt->kind == NodeKind::AnnotationDecl) {
      out.push_back(symbolInfoFor(docText, static_cast<const DeclNode *>(stmt)));
    } else if (stmt->kind == NodeKind::NamespaceDecl) {
      auto *ns = static_cast<const NamespaceDeclNode *>(stmt);
      json children = json::array();
      collectSymbolsFromStatements(ns->statements, docText, children);
      json info = symbolInfoFor(docText, ns);
      if (!children.empty())
        info["children"] = children;
      out.push_back(info);
    }
  }
}

void collectModuleSymbols(const ModuleNode *mod, const std::string &docText,
                          json &out,
                          std::unordered_set<const ModuleNode *> &visited) {
  if (!mod || !visited.insert(mod).second)
    return;
  collectSymbolsFromStatements(mod->statements, docText, out);
}

} // namespace

void handleDocumentSymbols(const json &req) {
  syncWorker();
  std::string uri = req["params"]["textDocument"]["uri"];
  json res = json::array();

  DocumentState doc;
  if (documents.get(uri, doc) && doc.ast) {
    std::unordered_set<const ModuleNode *> visited;
    collectModuleSymbols(doc.ast, doc.text, res, visited);
  }

  sendResponse({{"jsonrpc", "2.0"}, {"id", requestId(req)}, {"result", res}});
}

void handleWorkspaceSymbols(const json &req) {
  syncWorker();
  std::string query;
  if (req["params"].contains("query")) {
    query = req["params"]["query"].get<std::string>();
  }

  json res = json::array();

  /* Workspace symbols search every analyzed document. */
  for (const auto &[uri, doc] : documents.snapshot()) {
    if (!doc.ast)
      continue;
    GlobalSymbols globals = collectGlobals(doc.ast);
    auto match = [&](const DeclNode *decl) {
      if (query.empty())
        return true;
      std::string_view name = decl->fqName;
      size_t pos = name.find_last_of('.');
      if (pos != std::string_view::npos)
        name = name.substr(pos + 1);
      return name.find(query) != std::string_view::npos;
    };
    for (const auto *decl : globals.rootGlobals) {
      if (!match(decl))
        continue;
      json item = symbolInfoFor(doc.text, decl);
      item["location"] = {{"uri", uri},
                          {"range", item["range"]}};
      item.erase("range");
      item.erase("selectionRange");
      res.push_back(item);
    }
  }

  sendResponse({{"jsonrpc", "2.0"}, {"id", requestId(req)}, {"result", res}});
}

} // namespace utopia::lsp
