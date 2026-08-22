#include "LspCore.hpp"
#include "utopia/Common/Warnings.hpp"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace utopia::lsp {

namespace {

/* Text helpers */

/* Offset of a 1-based (line, column) position in the document. */
size_t offsetAt(const std::string &text, int line, int col) {
  size_t offset = 0;
  int current = 1;
  while (current < line) {
    size_t nl = text.find('\n', offset);
    if (nl == std::string::npos)
      return text.size();
    offset = nl + 1;
    current++;
  }
  return std::min(text.size(), offset + std::max(0, col - 1));
}

/* The full source line 'line' (1-based), without the trailing newline. */
std::string lineText(const std::string &text, int line) {
  size_t start = offsetAt(text, line, 1);
  size_t nl = text.find('\n', start);
  return text.substr(start, nl == std::string::npos ? std::string::npos
                                                    : nl - start);
}

/* LSP range covering the whole line 'line' (1-based), including its
 * trailing newline. */
json wholeLineRange(const std::string &text, int line) {
  size_t start = offsetAt(text, line, 1);
  size_t nl = text.find('\n', start);
  size_t end = (nl == std::string::npos) ? text.size() : nl + 1;
  return {{"start", {{"line", line - 1}, {"character", 0}}},
          {"end", {{"line", line - 1}, {"character", (int)(end - start)}}}};
}

json makeRange(int startLine, int startCol, int endLine, int endCol) {
  return {{"start", {{"line", startLine - 1}, {"character", startCol - 1}}},
          {"end", {{"line", endLine - 1}, {"character", endCol - 1}}}};
}

std::string trim(const std::string &s) {
  size_t b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos)
    return "";
  size_t e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

bool isBlankLine(const std::string &text, int line) {
  return trim(lineText(text, line)).empty();
}

/* Diagnostic filtering */

/* Whether the diagnostic overlaps the requested range (null = no filter). */
bool diagInRange(const Diagnostic &d, const json &rangeFilter) {
  if (rangeFilter.is_null())
    return true;
  int startLine = rangeFilter["start"]["line"].get<int>() + 1;
  int endLine = rangeFilter["end"]["line"].get<int>() + 1;
  int diagStart = d.line;
  int diagEnd = d.endLine > 0 ? d.endLine : d.line;
  return diagEnd >= startLine && diagStart <= endLine;
}

std::string diagCode(const Diagnostic &d) {
  return std::string(warningName(d.warningKind));
}

/* AST lookups */

/* Recursively collects every declaration and using directive in the module
 * tree with their source spans. */
void collectDecls(const ASTNode *node, std::vector<const DeclNode *> &decls,
                  std::vector<const UsingNode *> &usings) {
  if (!node)
    return;
  if (auto *decl = llvm::dyn_cast<DeclNode>(node))
    decls.push_back(decl);
  if (auto *u = llvm::dyn_cast<UsingNode>(node))
    usings.push_back(u);

  switch (node->kind) {
  case NodeKind::Module: {
    auto *m = static_cast<const ModuleNode *>(node);
    for (const auto *stmt : m->statements)
      collectDecls(stmt, decls, usings);
    for (const auto *imp : m->importedModules)
      collectDecls(imp, decls, usings);
    for (const auto *exp : m->exportedModules)
      collectDecls(exp, decls, usings);
    break;
  }
  case NodeKind::NamespaceDecl: {
    auto *ns = static_cast<const NamespaceDeclNode *>(node);
    for (const auto *stmt : ns->statements)
      collectDecls(stmt, decls, usings);
    break;
  }
  case NodeKind::FunctionDecl: {
    auto *fn = static_cast<const FunctionDeclNode *>(node);
    for (const auto *p : fn->params)
      collectDecls(p, decls, usings);
    collectDecls(fn->body, decls, usings);
    for (const auto *init : fn->fieldInitializers)
      collectDecls(init, decls, usings);
    if (fn->superCall)
      collectDecls(fn->superCall, decls, usings);
    break;
  }
  case NodeKind::StructDecl: {
    auto *s = static_cast<const StructDeclNode *>(node);
    for (const auto *f : s->fields)
      collectDecls(f, decls, usings);
    for (const auto *m : s->methods)
      collectDecls(m, decls, usings);
    break;
  }
  case NodeKind::ClassDecl: {
    auto *c = static_cast<const ClassDeclNode *>(node);
    for (const auto *f : c->fields)
      collectDecls(f, decls, usings);
    for (const auto *m : c->methods)
      collectDecls(m, decls, usings);
    break;
  }
  case NodeKind::UnionDecl: {
    auto *u = static_cast<const UnionDeclNode *>(node);
    for (const auto *f : u->fields)
      collectDecls(f, decls, usings);
    for (const auto *m : u->methods)
      collectDecls(m, decls, usings);
    break;
  }
  case NodeKind::AnnotationDecl: {
    auto *a = static_cast<const AnnotationDeclNode *>(node);
    for (const auto *f : a->fields)
      collectDecls(f, decls, usings);
    if (a->constructor)
      collectDecls(a->constructor, decls, usings);
    break;
  }
  case NodeKind::Lambda: {
    auto *l = static_cast<const LambdaNode *>(node);
    for (const auto *p : l->params)
      collectDecls(p, decls, usings);
    collectDecls(l->body, decls, usings);
    collectDecls(l->exprBody, decls, usings);
    break;
  }
  case NodeKind::Block: {
    auto *b = static_cast<const BlockNode *>(node);
    for (const auto *stmt : b->statements)
      collectDecls(stmt, decls, usings);
    break;
  }
  case NodeKind::If: {
    auto *i = static_cast<const IfNode *>(node);
    collectDecls(i->condition, decls, usings);
    collectDecls(i->thenBlock, decls, usings);
    collectDecls(i->elseBlock, decls, usings);
    break;
  }
  case NodeKind::For: {
    auto *f = static_cast<const ForNode *>(node);
    collectDecls(f->initStatement, decls, usings);
    collectDecls(f->condition, decls, usings);
    collectDecls(f->increment, decls, usings);
    collectDecls(f->body, decls, usings);
    break;
  }
  case NodeKind::ForIn: {
    auto *f = static_cast<const ForInNode *>(node);
    collectDecls(f->loopVar, decls, usings);
    collectDecls(f->iterable, decls, usings);
    collectDecls(f->body, decls, usings);
    break;
  }
  case NodeKind::While: {
    auto *w = static_cast<const WhileNode *>(node);
    collectDecls(w->condition, decls, usings);
    collectDecls(w->body, decls, usings);
    break;
  }
  case NodeKind::Switch: {
    auto *s = static_cast<const SwitchNode *>(node);
    collectDecls(s->condition, decls, usings);
    for (const auto *c : s->cases)
      collectDecls(c, decls, usings);
    break;
  }
  case NodeKind::Case: {
    auto *c = static_cast<const CaseNode *>(node);
    collectDecls(c->value, decls, usings);
    for (const auto *stmt : c->statements)
      collectDecls(stmt, decls, usings);
    break;
  }
  case NodeKind::Try: {
    auto *t = static_cast<const TryStmtNode *>(node);
    collectDecls(t->body, decls, usings);
    for (const auto *clause : t->clauses)
      collectDecls(clause->body, decls, usings);
    break;
  }
  case NodeKind::VarDecl: {
    auto *v = static_cast<const VarDeclNode *>(node);
    collectDecls(v->initializer, decls, usings);
    break;
  }
  default:
    break;
  }
}

const DeclNode *declAt(const DocumentState &doc, int line, int col,
                       std::vector<const UsingNode *> &outUsings) {
  std::vector<const DeclNode *> decls;
  collectDecls(doc.ast, decls, outUsings);
  for (const DeclNode *d : decls) {
    /* Warnings are positioned at the identifier; declarations are recorded
     * at their type/modifier position. */
    if (d->line == line &&
        (d->column == col ||
         (d->identifierColumn > 0 && d->identifierColumn == col)))
      return d;
  }
  return nullptr;
}

const UsingNode *usingAt(const std::vector<const UsingNode *> &usings,
                         int line, int col) {
  for (const UsingNode *u : usings) {
    if (u->line == line && u->column == col)
      return u;
  }
  return nullptr;
}

const ModuleNode::DirectiveInfo *importAt(const DocumentState &doc, int line,
                                          int col) {
  for (const auto &info : doc.ast->importInfo) {
    if (info.line == line && info.column == col)
      return &info;
  }
  return nullptr;
}

/* Fix computation */

/* TextEdits removing the full lines [start, end] (1-based, inclusive). */
json deleteLinesEdit(const std::string &text, int startLine, int endLine) {
  json edits = json::array();
  for (int line = startLine; line <= endLine; ++line) {
    edits.push_back({{"range", wholeLineRange(text, line)}, {"newText", ""}});
  }
  return edits;
}

/* 0-based LSP position of a byte offset. */
json positionAt(const std::string &text, size_t offset) {
  int line = 0;
  size_t lineStart = 0;
  for (size_t i = 0; i < offset && i < text.size(); ++i) {
    if (text[i] == '\n') {
      line++;
      lineStart = i + 1;
    }
  }
  return {{"line", line}, {"character", (int)(offset - lineStart)}};
}

/* 1-based line number of a byte offset. */
int lineAt(const std::string &text, size_t offset) {
  int line = 1;
  for (size_t i = 0; i < offset && i < text.size(); ++i) {
    if (text[i] == '\n')
      line++;
  }
  return line;
}

/* Normalizes the text edits of a fix:
 *   - merges adjacent/overlapping whole-line deletions into one edit
 *     (non-deletion edits in between do not break the merge chain),
 *   - when a deletion removed a block at the start of the file or right
 *     after a blank line, also consumes the blank line that follows it, so
 *     no empty line is left where the fix landed,
 *   - drops any edit fully contained in a deletion (e.g. a parameter
 *     rename inside a removed function). */
json applyDeletionPolicy(const std::string &text, const json &edits) {
  struct Edit {
    json obj;
    size_t start;
    size_t end;
    bool deletion;
  };

  std::vector<Edit> list;
  for (const auto &e : edits) {
    if (!e.contains("range") || !e.contains("newText"))
      continue;
    Edit ed;
    ed.obj = e;
    ed.start = offsetAt(text, e["range"]["start"]["line"].get<int>() + 1,
                        e["range"]["start"]["character"].get<int>() + 1);
    ed.end = offsetAt(text, e["range"]["end"]["line"].get<int>() + 1,
                      e["range"]["end"]["character"].get<int>() + 1);
    ed.deletion = e["newText"].get<std::string>().empty();
    list.push_back(ed);
  }
  std::sort(list.begin(), list.end(),
            [](const Edit &a, const Edit &b) { return a.start < b.start; });

  /* Merge touching or overlapping deletion runs. */
  std::vector<Edit> delRuns;
  for (auto &ed : list) {
    if (!ed.deletion)
      continue;
    if (!delRuns.empty() && ed.start <= delRuns.back().end) {
      delRuns.back().end = std::max(delRuns.back().end, ed.end);
      continue;
    }
    delRuns.push_back(ed);
  }

  /* Swallow the blank line left behind by a deletion at the file start or
   * right after another blank line. */
  for (auto &ed : delRuns) {
    int startLine = lineAt(text, ed.start);
    int endLine = lineAt(text, ed.end);
    /* The end usually points at the start of the first line after the
     * deleted block. */
    if (ed.end > 0 && text[ed.end - 1] == '\n')
      endLine--;
    bool prevBlank = startLine <= 1 || isBlankLine(text, startLine - 1);
    if (prevBlank && isBlankLine(text, endLine + 1)) {
      size_t nl = text.find('\n', ed.end);
      if (nl != std::string::npos)
        ed.end = nl + 1;
    }
  }

  /* Keep non-deletion edits that are not fully inside a deletion. */
  std::vector<Edit> kept;
  for (auto &ed : list) {
    if (ed.deletion)
      continue;
    bool contained = false;
    for (const auto &del : delRuns) {
      if (ed.start >= del.start && ed.end <= del.end) {
        contained = true;
        break;
      }
    }
    if (!contained)
      kept.push_back(ed);
  }

  /* Interleave both sets in document order. */
  std::vector<Edit> merged;
  size_t i = 0, j = 0;
  while (i < delRuns.size() || j < kept.size()) {
    if (j >= kept.size() ||
        (i < delRuns.size() && delRuns[i].start <= kept[j].start)) {
      merged.push_back(delRuns[i++]);
    } else {
      merged.push_back(kept[j++]);
    }
  }

  json out = json::array();
  for (auto &ed : merged) {
    json obj = ed.obj;
    obj["range"]["start"] = positionAt(text, ed.start);
    obj["range"]["end"] = positionAt(text, ed.end);
    out.push_back(obj);
  }
  return out;
}

/* True when a line prefix consists only of whitespace and declaration
 * modifiers (visibility, static, final, const): the declaration then owns
 * its line and can be removed as a whole. */
bool prefixIsModifiers(const std::string &prefix) {
  static const char *const modifiers[] = {"private", "public", "protected",
                                          "static",  "final",  "const",
                                          "abstract", "override", "virtual"};
  size_t pos = 0;
  while (pos < prefix.size()) {
    size_t ws = prefix.find_first_not_of(" \t", pos);
    if (ws == std::string::npos)
      break;
    size_t end = prefix.find_first_of(" \t", ws);
    if (end == std::string::npos)
      end = prefix.size();
    std::string tok = prefix.substr(ws, end - ws);
    bool known = false;
    for (const char *m : modifiers) {
      if (tok == m) {
        known = true;
        break;
      }
    }
    if (!known)
      return false;
    pos = end;
  }
  return true;
}

/* True when the declaration starts at the beginning of its line, so the
 * whole-line removal below is safe. The decl's position points at its
 * return type, so a prefix of visibility/modifier keywords is accepted. */
bool declStartsLine(const std::string &text, const DeclNode *decl) {
  std::string prefix = lineText(text, decl->line).substr(0, decl->column - 1);
  return prefixIsModifiers(prefix);
}

/* The first source line of a declaration: annotations and modifier
 * keywords may sit above or before the recorded position. */
int declStartLine(const DeclNode *decl) {
  int line = decl->line;
  if (!decl->annotations.empty()) {
    int annLine = decl->annotations.front()->line;
    if (annLine > 0 && annLine < line)
      line = annLine;
  }
  return line;
}

/* TextEdits removing a whole declaration (annotations included) that owns
 * its lines. Empty when the edit would be unsafe. */
json removeDeclEdit(const std::string &text, const DeclNode *decl) {
  if (!declStartsLine(text, decl))
    return json();
  int startLine = declStartLine(decl);
  int endLine = decl->endLine > decl->line ? decl->endLine : decl->line;
  return deleteLinesEdit(text, startLine, endLine);
}

/* True when the initializer may have side effects (calls, allocation,
 * assignment): such initializers must be kept when removing a variable. */
bool hasSideEffects(const ExprNode *expr) {
  if (!expr)
    return false;
  if (llvm::isa<FunctionCallNode>(expr) || llvm::isa<NewExprNode>(expr) ||
      llvm::isa<AssignNode>(expr) || llvm::isa<DeleteExprNode>(expr) ||
      llvm::isa<AwaitExprNode>(expr) || llvm::isa<DestructorCallNode>(expr))
    return true;
  if (auto *b = llvm::dyn_cast<BinaryOpNode>(expr))
    return hasSideEffects(b->left) || hasSideEffects(b->right);
  if (auto *u = llvm::dyn_cast<UnaryOpNode>(expr))
    return u->op == "++" || u->op == "--" || hasSideEffects(u->expr);
  if (auto *t = llvm::dyn_cast<TernaryOpNode>(expr))
    return hasSideEffects(t->condition) || hasSideEffects(t->trueExpr) ||
           hasSideEffects(t->falseExpr);
  if (auto *c = llvm::dyn_cast<CastNode>(expr))
    return hasSideEffects(c->expr);
  if (auto *ic = llvm::dyn_cast<ImplicitCastNode>(expr))
    return hasSideEffects(ic->expr);
  if (auto *ce = llvm::dyn_cast<ConstExprNode>(expr))
    return hasSideEffects(ce->expr);
  if (auto *arr = llvm::dyn_cast<ArrayLiteralNode>(expr)) {
    for (const auto *e : arr->elements)
      if (hasSideEffects(e))
        return true;
  }
  if (auto *map = llvm::dyn_cast<MapLiteralNode>(expr)) {
    for (const auto *k : map->keys)
      if (hasSideEffects(k))
        return true;
    for (const auto *v : map->values)
      if (hasSideEffects(v))
        return true;
  }
  if (auto *sub = llvm::dyn_cast<ArraySubscriptNode>(expr)) {
    if (sub->overloadedOperator)
      return true;
    return hasSideEffects(sub->base) || hasSideEffects(sub->index);
  }
  if (auto *ma = llvm::dyn_cast<MemberAccessNode>(expr)) {
    if (ma->resolvedMethod || ma->isMethodRef)
      return true;
    return hasSideEffects(ma->object);
  }
  return false;
}

/* TextEdits removing an unused variable. Side-effecting initializers are
 * kept as a bare expression statement. */
json removeVariableEdit(const std::string &text, const VarDeclNode *var) {
  int endLine = var->endLine > 0 ? var->endLine : var->line;
  int endCol = var->column + std::max(1, var->length);

  if (var->initializer && hasSideEffects(var->initializer)) {
    int initEndLine = var->initializer->endLine > 0
                          ? var->initializer->endLine
                          : var->initializer->line;
    int initEndCol = var->initializer->column + var->initializer->length;
    /* Consume the statement's own ';' so the kept call does not end up
     * with a double semicolon. */
    size_t endOff = offsetAt(text, initEndLine, initEndCol);
    if (endOff < text.size() && text[endOff] == ';')
      initEndCol++;
    size_t initStart =
        offsetAt(text, var->initializer->line, var->initializer->column);
    size_t initEnd =
        offsetAt(text, initEndLine, initEndCol);
    std::string initText = text.substr(initStart, initEnd - initStart);
    initText = trim(initText);
    if (!initText.ends_with(";"))
      initText += ";";
    return json::array({json{{"range",
                              makeRange(var->line, var->column, initEndLine,
                                        initEndCol)},
                             {"newText", initText}}});
  }

  /* Remove the whole line when the declaration owns it. */
  std::string leading = lineText(text, var->line).substr(0, var->column - 1);
  if (trim(leading).empty()) {
    return deleteLinesEdit(text, var->line, endLine);
  }
  /* Inline declaration: remove the exact statement span. */
  return json::array({json{{"range",
                            makeRange(var->line, var->column, endLine, endCol)},
                           {"newText", ""}}});
}

/* Whether removing a private field is safe: the field owns its whole line
 * (only modifiers precede it, nothing follows the terminating ';') and no
 * constructor binds it through an initializing formal. */
bool fieldRemovalSafe(const std::string &text, const VarDeclNode *field) {
  if (field->endLine != field->line)
    return false;
  std::string line = lineText(text, field->line);
  std::string prefix = line.substr(0, field->column - 1);
  if (!prefixIsModifiers(prefix))
    return false;

  std::string stmt = trim(line.substr(field->column - 1));
  /* The statement must end the line with a single ';' (no multi-field or
   * multi-statement lines). */
  if (!stmt.ends_with(";"))
    return false;
  if (stmt.find(';') != stmt.size() - 1)
    return false;

  std::string needle = "this." + std::string(field->varName);
  return text.find(needle) == std::string::npos;
}

/* TextEdit renaming an unused parameter to '_name' (safe for positional
 * parameters: call sites are unaffected). */
json renameParamEdit(const ParamDeclNode *param) {
  int idCol = param->identifierColumn > 0 ? param->identifierColumn
                                          : param->column;
  int idLen = param->identifierLength > 0 ? param->identifierLength
                                          : (int)param->name.size();
  return json{{"range", makeRange(param->line, idCol, param->line,
                                  idCol + idLen)},
              {"newText", "_" + std::string(param->name)}};
}

/* TextEdits removing an unused import/export directive line. */
json removeImportEdit(const std::string &text,
                      const ModuleNode::DirectiveInfo &info) {
  std::string leading = lineText(text, info.line).substr(0, info.column - 1);
  if (trim(leading).empty()) {
    return deleteLinesEdit(text, info.line, info.endLine);
  }
  return json{json{{"range",
                    makeRange(info.line, info.column, info.endLine,
                              info.endColumn)},
                   {"newText", ""}}};
}

/* Computes the fix edits for one diagnostic. Returns false when no
 * automatic fix applies. */
bool computeFix(const DocumentState &doc, const Diagnostic &d,
                const std::string &code, json &edits) {
  std::vector<const UsingNode *> usings;
  const DeclNode *decl = declAt(doc, d.line, d.column, usings);

  if (code == "unused_import") {
    if (const ModuleNode::DirectiveInfo *info = importAt(doc, d.line, d.column)) {
      edits = removeImportEdit(doc.text, *info);
      return !edits.empty();
    }
  } else if (code == "unused_using") {
    if (const UsingNode *u = usingAt(usings, d.line, d.column)) {
      edits = deleteLinesEdit(doc.text, u->line, u->line);
      return true;
    }
  } else if (code == "unused_variable") {
    if (auto *var = llvm::dyn_cast_or_null<VarDeclNode>(decl)) {
      edits = removeVariableEdit(doc.text, var);
      return !edits.empty();
    }
  } else if (code == "unused_field") {
    if (auto *field = llvm::dyn_cast_or_null<VarDeclNode>(decl)) {
      if (fieldRemovalSafe(doc.text, field)) {
        edits = deleteLinesEdit(doc.text, field->line, field->endLine);
        return true;
      }
    }
  } else if (code == "unused_parameter") {
    if (auto *param = llvm::dyn_cast_or_null<ParamDeclNode>(decl)) {
      if (!param->isNamed) {
        edits = {renameParamEdit(param)};
        return true;
      }
    }
  } else if (code == "unused_function" || code == "unused_method" ||
             code == "unused_type") {
    if (decl) {
      edits = removeDeclEdit(doc.text, decl);
      return !edits.empty();
    }
  }
  return false;
}

/* Project-level disable (build.yaml) */

/* Returns the build.yaml path for the document, or "" when no project root
 * exists. */
std::string projectManifestPath(const DocumentState &doc,
                                const std::string &uri,
                                const std::string &filePath) {
  ModuleLoaderConfig config =
      documents.configFor(uri, std::filesystem::path(filePath));
  if (config.projectRoot.empty())
    return "";
  return (config.projectRoot / "build.yaml").string();
}

/* Rewrites build.yaml to disable the warning kind. Works with both the map
 * form ('warnings: { unused_import: false }') and the list form
 * ('warnings: [unused_import]'). */
std::string manifestWithDisabledWarning(const std::string &text,
                                        const std::string &name) {
  /* Split into lines. */
  std::vector<std::string> lines;
  std::string cur;
  for (char c : text) {
    if (c == '\n') {
      lines.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  lines.push_back(cur);

  int buildIdx = -1;
  int warningsIdx = -1;
  std::string buildIndent = "  ";
  std::string warningsIndent = "  ";
  std::string baseIndent = "";

  for (size_t i = 0; i < lines.size(); ++i) {
    std::string t = trim(lines[i]);
    if (t == "build:") {
      buildIdx = (int)i;
      for (size_t j = i + 1; j < lines.size(); ++j) {
        if (!trim(lines[j]).empty()) {
          size_t lead = lines[j].find_first_not_of(" \t");
          if (lead != std::string::npos)
            buildIndent = lines[j].substr(0, lead);
          break;
        }
      }
    } else if (t == "warnings:") {
      warningsIdx = (int)i;
      size_t lead = lines[i].find_first_not_of(" \t");
      if (lead != std::string::npos)
        warningsIndent = lines[i].substr(0, lead);
    }
  }

  /* Find the extent of an existing warnings block. */
  int lastWarnIdx = warningsIdx;
  bool listForm = false;
  if (warningsIdx >= 0) {
    for (size_t i = (size_t)warningsIdx + 1; i < lines.size(); ++i) {
      if (trim(lines[i]).empty())
        continue;
      size_t lead = lines[i].find_first_not_of(" \t");
      std::string indent = (lead == std::string::npos) ? ""
                                                       : lines[i].substr(0, lead);
      if (indent.size() <= warningsIndent.size())
        break;
      if (trim(lines[i]).starts_with("- "))
        listForm = true;
      lastWarnIdx = (int)i;
    }
  }

  /* Build the entry text. */
  std::string entry;
  std::string entryIndent;
  if (warningsIdx >= 0 && lastWarnIdx > warningsIdx) {
    if (listForm) {
      entry = warningsIndent + "  - " + name;
    } else {
      entry = warningsIndent + "  " + name + ": false";
    }
  } else if (warningsIdx >= 0) {
    entry = warningsIndent + "  " + name + ": false";
  } else if (buildIdx >= 0) {
    entry = buildIndent + "warnings:\n" + buildIndent + "  " + name + ": false";
  } else {
    entry = "build:\n  warnings:\n    " + name + ": false";
  }

  /* Insert after the anchor line (or append at the end). */
  int insertAfter = -1;
  if (warningsIdx >= 0) {
    insertAfter = lastWarnIdx;
  } else if (buildIdx >= 0) {
    insertAfter = buildIdx;
  }

  std::string result;
  if (insertAfter < 0) {
    result = text;
    if (!result.empty() && result.back() != '\n')
      result += "\n";
    if (!result.empty())
      result += "\n";
    result += entry + "\n";
    return result;
  }

  size_t anchorStart = offsetAt(text, insertAfter + 1, 1);
  size_t nl = text.find('\n', anchorStart);
  size_t anchorEnd = (nl == std::string::npos) ? text.size() : nl;

  result = text.substr(0, anchorEnd) + "\n" + entry + text.substr(anchorEnd);
  return result;
}

/* Code action builders */

json codeAction(const std::string &title, const json &edits,
                const std::vector<const Diagnostic *> &diags,
                const std::string &uri) {
  json diagnostics = json::array();
  for (const Diagnostic *d : diags) {
    int startLine = d->line > 0 ? d->line - 1 : 0;
    int startCol = d->column > 0 ? d->column - 1 : 0;
    int endLine = d->endLine > 0 ? d->endLine - 1 : startLine;
    int endCol = startCol + std::max(1, d->length);
    diagnostics.push_back(
        {{"range",
          {{"start", {{"line", startLine}, {"character", startCol}}},
           {"end", {{"line", endLine}, {"character", endCol}}}}},
         {"message", d->message},
         {"severity", 2},
         {"source", "utopia"}});
  }

  json action = {{"title", title}, {"kind", "quickfix"}};
  if (!diagnostics.empty())
    action["diagnostics"] = diagnostics;
  action["edit"] = {{"changes", {{uri, edits}}}};
  return action;
}

/* Inserts a line-scoped suppression comment directly above the line. */
json suppressLineEdit(const std::string &code, int line) {
  return json{{"range",
               {{"start", {{"line", line - 1}, {"character", 0}}},
                {"end", {{"line", line - 1}, {"character", 0}}}}},
              {"newText", "// @ignore-warning " + code + "\n"}};
}

/* Inserts a file-scoped suppression at the top of the file, or extends an
 * existing '@ignore-warnings' comment. */
json suppressFileEdit(const std::string &text, const std::string &code) {
  int firstLine = 1;
  int guard = 0;
  while (guard++ < 1000 && isBlankLine(text, firstLine))
    firstLine++;
  std::string first = lineText(text, firstLine);
  std::string t = trim(first);
  if (t.starts_with("// @ignore-warnings")) {
    int col = (int)first.size();
    return json{{"range",
                 {{"start", {{"line", firstLine - 1}, {"character", col}}},
                  {"end", {{"line", firstLine - 1}, {"character", col}}}}},
                {"newText", ", " + code}};
  }
  return json{{"range",
               {{"start", {{"line", 0}, {"character", 0}}},
                {"end", {{"line", 0}, {"character", 0}}}}},
              {"newText", "// @ignore-warnings " + code + "\n"}};
}

} // namespace

void handleCodeAction(const json &req) {
  syncWorker();

  std::string uri = req["params"]["textDocument"]["uri"];
  json rangeFilter =
      req["params"].contains("range") ? req["params"]["range"] : json();

  json res = json::array();

  DocumentState doc;
  if (!documents.get(uri, doc) || !doc.ast)
    return sendResponse(
        {{"jsonrpc", "2.0"}, {"id", requestId(req)}, {"result", res}});

  std::string filePath = uriToPath(uri);

  /* Diagnostics of this document that carry a warning code. Range-filtered
   * ones drive the per-diagnostic actions; bulk actions always consider the
   * whole document. */
  std::vector<const Diagnostic *> allDiags;
  for (const auto &d : doc.diags->getDiagnostics()) {
    if (d.warningKind == WarningKind::None)
      continue;
    if (!d.filePath.empty() && d.filePath != filePath)
      continue;
    allDiags.push_back(&d);
  }

  std::vector<const Diagnostic *> diags;
  for (const Diagnostic *d : allDiags) {
    if (diagInRange(*d, rangeFilter))
      diags.push_back(d);
  }

  /* Group by warning code to offer per-kind bulk fixes. */
  std::map<std::string, std::vector<const Diagnostic *>> byCode;
  for (const Diagnostic *d : allDiags)
    byCode[diagCode(*d)].push_back(d);

  std::string manifestPath = projectManifestPath(doc, uri, filePath);
  bool manifestExists = !manifestPath.empty() &&
                        std::filesystem::exists(manifestPath);
  std::string manifestUri =
      manifestPath.empty() ? "" : pathToUri(manifestPath);

  for (const auto &[code, codeDiags] : byCode) {
    for (const Diagnostic *d : codeDiags) {
      /* Fix this warning */
      json edits;
      if (computeFix(doc, *d, code, edits)) {
        edits = applyDeletionPolicy(doc.text, edits);
        res.push_back(codeAction(
            "Utopia: Fix this warning (" + code + ")", edits,
            {d}, uri));
      }

      /* Disable on this line */
      res.push_back(codeAction(
          "Utopia: Disable '" + code + "' on this line",
          json::array({suppressLineEdit(code, d->line)}), {d}, uri));

      /* Disable in this file */
      json fileEdit = suppressFileEdit(doc.text, code);
      res.push_back(codeAction(
          "Utopia: Disable '" + code + "' in this file",
          json::array({fileEdit}), {d}, uri));

      /* Disable in this project (build.yaml) */
      if (manifestExists) {
        std::string manifestText;
        {
          std::ifstream in(manifestPath);
          std::stringstream buffer;
          buffer << in.rdbuf();
          manifestText = buffer.str();
        }
        std::string newText =
            manifestWithDisabledWarning(manifestText, code);
        json manifestEdit =
            json{{"range",
                  {{"start", {{"line", 0}, {"character", 0}}},
                   {"end",
                    {{"line", (int)std::count(manifestText.begin(),
                                              manifestText.end(), '\n')},
                     {"character", 0}}}}},
                 {"newText", newText}};
        res.push_back(codeAction(
            "Utopia: Disable '" + code + "' in this project (" +
                manifestPath.substr(manifestPath.find_last_of('/') + 1) + ")",
            json::array({manifestEdit}), {d}, manifestUri));
      }
    }

    /* Fix all similar warnings in this document */
    json allEdits = json::array();
    bool anyFix = false;
    for (const Diagnostic *d : codeDiags) {
      json edits;
      if (computeFix(doc, *d, code, edits)) {
        json normalized = applyDeletionPolicy(doc.text, edits);
        for (const auto &e : normalized)
          allEdits.push_back(e);
        anyFix = true;
      }
    }
    allEdits = applyDeletionPolicy(doc.text, allEdits);
    if (anyFix) {
      res.push_back(codeAction(
          "Utopia: Fix all '" + code + "' warnings in this document",
          allEdits, codeDiags, uri));
    }
  }

  /* Fix all fixable warnings in this document */
  json allFixEdits = json::array();
  bool anyFixable = false;
  for (const Diagnostic *d : allDiags) {
    json edits;
    if (computeFix(doc, *d, diagCode(*d), edits)) {
      json normalized = applyDeletionPolicy(doc.text, edits);
      for (const auto &e : normalized)
        allFixEdits.push_back(e);
      anyFixable = true;
    }
  }
  allFixEdits = applyDeletionPolicy(doc.text, allFixEdits);
  if (anyFixable) {
    res.push_back(codeAction(
        "Utopia: Fix all warnings with fixes in this document", allFixEdits,
        allDiags, uri));
  }

  sendResponse(
      {{"jsonrpc", "2.0"}, {"id", requestId(req)}, {"result", res}});
}

} // namespace utopia::lsp
