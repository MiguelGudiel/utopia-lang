#include "utopia/AST/AST.hpp"
#include "utopia/AST/ASTVisitor.hpp"
#include "utopia/Driver/ModuleLoader.hpp"
#include "utopia/Lexer/Lexer.hpp"
#include "utopia/Parser/Parser.hpp"
#include "utopia/Sema/Sema.hpp"
#include <filesystem>
#include <iostream>
#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

using json = nlohmann::json;
namespace fs = std::filesystem;

// --- ESTRUCTURAS DE ESTADO ---

struct FileState {
  std::string content;            // Raw buffer for the lexer
  std::vector<std::string> lines; // Fast line access for LSP lookups
  std::unique_ptr<utopia::ModuleNode> ast;
  utopia::Sema sema;
  utopia::ModuleLoader loader;

  void updateContent(const std::string &newText) {
    size_t startOffset = 0;

    // Ensure the LSP internal buffer is as clean as the Lexer's soul.
    // We strip the BOM here so line[0] starts at the actual text.
    if (newText.size() >= 3 && static_cast<unsigned char>(newText[0]) == 0xEF &&
        static_cast<unsigned char>(newText[1]) == 0xBB &&
        static_cast<unsigned char>(newText[2]) == 0xBF) {
      startOffset = 3;
    }

    content = newText.substr(startOffset);
    lines.clear();

    std::stringstream ss(content);
    std::string line;
    while (std::getline(ss, line)) {
      lines.push_back(line);
    }

    if (!content.empty() && content.back() == '\n') {
      lines.push_back("");
    }
  }
};

std::map<std::string, FileState> documentStates;

// UTILITIES

void sendResponse(const json &response) {
  std::string content =
      response.dump(-1, '?', false, json::error_handler_t::replace);
  std::cout << "Content-Length: " << content.length() << "\r\n\r\n"
            << content << std::flush;
}

void logMessage(const std::string &message) {
  json notification = {
      {"jsonrpc", "2.0"},
      {"method", "window/logMessage"},
      {"params", {{"type", 3}, {"message", "[UtopiaLSP] " + message}}}};
  sendResponse(notification);
}

// This helper reconstructs the source-code look of a symbol for the hover UI.
std::string getSignatureMarkup(utopia::ASTNode *node) {
  std::string signature = "```utp\n";

  if (auto fn = dynamic_cast<utopia::FunctionNode *>(node)) {
    // Add decorators like @override, @deprecated
    for (const auto &dec : fn->decorators) {
      signature += "@" + dec + "\n";
    }

    // Access and storage modifiers
    if (fn->access == utopia::AccessModifier::Public)
      signature += "public ";
    else if (fn->access == utopia::AccessModifier::Private)
      signature += "private ";

    if (fn->isStatic)
      signature += "static ";

    if (fn->inlineState == utopia::InlineState::Inline)
      signature += "inline ";
    else if (fn->inlineState == utopia::InlineState::ForceInline)
      signature += "force_inline ";

    // Return type and name
    if (!fn->isConstructor && !fn->isDestructor) {
      signature += fn->returnType + " ";
    }
    signature += fn->name + "(";

    // Argument list with names and types
    for (size_t i = 0; i < fn->args.size(); ++i) {
      if (fn->args[i].isThisAssign)
        signature += "this.";
      if (fn->args[i].isConst)
        signature += "const ";
      if (!fn->args[i].type.empty())
        signature += fn->args[i].type + " ";
      signature += fn->args[i].name;
      if (i < fn->args.size() - 1)
        signature += ", ";
    }
    signature += ")";

    if (fn->isConstMethod) {
      signature += " const";
    }
  } else if (auto st = dynamic_cast<utopia::StructDeclNode *>(node)) {
    signature += (st->isClass ? "class " : "struct ") + st->name;
    if (!st->baseClass.empty())
      signature += " extends " + st->baseClass;
  } else if (auto v = dynamic_cast<utopia::VarDeclNode *>(node)) {
    if (v->isStatic)
      signature += "static ";
    if (v->isConst)
      signature += "const ";
    signature += v->typeName + " " + v->name;
  }

  signature += "\n```";
  return signature;
}

json createRange(int sl, int sc, int el, int ec) {
  auto fix = [](int n) { return n - 1 < 0 ? 0 : n - 1; };
  return {{"start", {{"line", fix(sl)}, {"character", fix(sc)}}},
          {"end", {{"line", fix(el)}, {"character", fix(ec)}}}};
}

json createLocation(const std::string &uri, int sl, int sc, int el, int ec) {
  return {{"uri", uri}, {"range", createRange(sl, sc, el, ec)}};
}

std::string uriToPath(std::string uri) {
  if (uri.find("file://") == 0)
    return uri.substr(7);
  return uri;
}

// --- ANALIZADOR DE TEXTO PARA CONTEXTO ---

std::string getWordAt(const std::vector<std::string> &lines, int line,
                      int col) {

  if (line < 0 || line >= static_cast<int>(lines.size()))
    return "";

  const std::string &ln = lines[line];
  if (col < 0 || col >= static_cast<int>(ln.length()))
    col = ln.length() - 1;
  if (col < 0)
    return "";

  int start = col, end = col;
  auto isIdent = [](unsigned char c) {
    return std::isalnum(c) || c == '_' || c >= 128;
  };

  while (start >= 0 && isIdent(static_cast<unsigned char>(ln[start])))
    start--;
  while (end < static_cast<int>(ln.length()) &&
         isIdent(static_cast<unsigned char>(ln[end])))
    end++;

  start++;
  return (start < end) ? ln.substr(start, end - start) : "";
}

std::string getWordBeforeDot(const std::vector<std::string> &lines, int line,
                             int col) {
  if (line < 0 || line >= static_cast<int>(lines.size()))
    return "";

  const std::string &ln = lines[line];
  int pos = col - 1;

  /* Skip whitespace and the dot itself */
  while (pos >= 0 &&
         (std::isspace(static_cast<unsigned char>(ln[pos])) || ln[pos] == '.'))
    pos--;

  int end = pos + 1;
  while (pos >= 0 &&
         (std::isalnum(static_cast<unsigned char>(ln[pos])) || ln[pos] == '_'))
    pos--;

  int start = pos + 1;
  return (start < end) ? ln.substr(start, end - start) : "";
}

// --- RECOLECTOR DE SÍMBOLOS LOCALES ---

class NodeFinder : public utopia::ASTVisitor {
public:
  int targetLine, targetCol;
  utopia::ASTNode *bestMatch = nullptr;

  NodeFinder(int line, int col) : targetLine(line), targetCol(col) {}

  bool isInside(utopia::ASTNode *node) {
    if (!node)
      return false;
    if (targetLine < node->line || targetLine > node->endLine)
      return false;
    if (targetLine == node->line && targetCol < node->column)
      return false;
    if (targetLine == node->endLine && targetCol > node->endColumn)
      return false;
    return true;
  }

  void visitNode(utopia::ASTNode *node) {
    if (isInside(node)) {
      bestMatch = node;
    }
  }

  void visit(utopia::ProgramNode *node) override {
    for (auto &s : node->structs)
      s->accept(this);
    for (auto &f : node->functions)
      f->accept(this);
    for (auto &g : node->globalVars)
      g->accept(this);
  }

  void visit(utopia::ModuleNode *node) override {
    for (auto &s : node->structs)
      s->accept(this);
    for (auto &f : node->functions)
      f->accept(this);
    for (auto &g : node->globalVars)
      g->accept(this);
  }

  void visit(utopia::FunctionNode *node) override {
    visitNode(node);
    for (auto &stmt : node->body)
      stmt->accept(this);
  }

  void visit(utopia::StructDeclNode *node) override {
    visitNode(node);
    for (auto &m : node->methods)
      m->accept(this);
  }

  void visit(utopia::BlockNode *node) override {
    visitNode(node);
    for (auto &s : node->statements)
      s->accept(this);
  }

  void visit(utopia::CallNode *node) override {
    visitNode(node);
    if (node->object)
      node->object->accept(this);
    for (auto &arg : node->arguments)
      arg->accept(this);
  }

  void visit(utopia::IfNode *node) override {
    visitNode(node);
    node->condition->accept(this);
    for (auto &s : node->thenBody)
      s->accept(this);
    for (auto &s : node->elseBody)
      s->accept(this);
  }

  void visit(utopia::VarDeclNode *node) override {
    visitNode(node);
    if (node->initializer)
      node->initializer->accept(this);
  }
  void visit(utopia::VariableNode *node) override { visitNode(node); }
  void visit(utopia::NewNode *node) override {
    visitNode(node);
    for (auto &a : node->arguments)
      a->accept(this);
  }
  void visit(utopia::ReturnNode *node) override {
    visitNode(node);
    if (node->returnValue)
      node->returnValue->accept(this);
  }
  void visit(utopia::BinaryOpNode *node) override {
    visitNode(node);
    node->left->accept(this);
    node->right->accept(this);
  }
  void visit(utopia::MemberAccessNode *node) override {
    visitNode(node);
    node->object->accept(this);
  }
  void visit(utopia::WhileNode *node) override {
    visitNode(node);
    node->condition->accept(this);
    for (auto &b : node->body)
      b->accept(this);
  }
  void visit(utopia::ForNode *node) override {
    visitNode(node);
    if (node->init)
      node->init->accept(this);
    if (node->condition)
      node->condition->accept(this);
    if (node->update)
      node->update->accept(this);
    for (auto &b : node->body)
      b->accept(this);
  }

  void visit(utopia::ThisNode *n) override { visitNode(n); }
  void visit(utopia::SuperNode *n) override { visitNode(n); }
  void visit(utopia::NullLiteralNode *n) override { visitNode(n); }
  void visit(utopia::BreakNode *n) override { visitNode(n); }
  void visit(utopia::ContinueNode *n) override { visitNode(n); }
  void visit(utopia::NullAssertNode *n) override {
    visitNode(n);
    n->operand->accept(this);
  }
  void visit(utopia::LogicalNotNode *n) override {
    visitNode(n);
    n->operand->accept(this);
  }
  void visit(utopia::NumberNode *n) override { visitNode(n); }
  void visit(utopia::FloatNode *n) override { visitNode(n); }
  void visit(utopia::BoolNode *n) override { visitNode(n); }
  void visit(utopia::StringNode *n) override { visitNode(n); }
  void visit(utopia::UnaryMinusNode *n) override {
    visitNode(n);
    n->operand->accept(this);
  }
  void visit(utopia::SubscriptNode *n) override {
    visitNode(n);
    n->object->accept(this);
    n->index->accept(this);
  }
  void visit(utopia::AddressOfNode *n) override {
    visitNode(n);
    n->operand->accept(this);
  }
  void visit(utopia::DerefNode *n) override {
    visitNode(n);
    n->operand->accept(this);
  }
  void visit(utopia::DeleteNode *n) override {
    visitNode(n);
    n->pointerExpr->accept(this);
  }
  void visit(utopia::MoveNode *n) override {
    visitNode(n);
    n->operand->accept(this);
  }
  void visit(utopia::CastNode *n) override {
    visitNode(n);
    n->operand->accept(this);
  }
  void visit(utopia::AssignNode *n) override {
    visitNode(n);
    n->target->accept(this);
    n->value->accept(this);
  }
  void visit(utopia::ExtensionNode *n) override {
    visitNode(n);
    for (auto &m : n->methods)
      m->accept(this);
  }
};

class SymbolResolver : public utopia::ASTVisitor {
public:
  int targetLine;
  std::map<std::string, std::string> locals;
  const utopia::Sema &sema;

  SymbolResolver(int line, const utopia::Sema &s) : targetLine(line), sema(s) {}

  void visit(utopia::VarDeclNode *node) override {
    if (node->line <= targetLine)
      locals[node->name] = node->typeName;
  }
  void visit(utopia::FunctionNode *node) override {
    if (targetLine >= node->line && targetLine <= node->endLine) {
      if (node->isMethod) {
        if (!node->isStatic)
          locals["this"] = node->className;
        std::string currentClassName = node->className;
        auto const &customStructs = sema.getCustomStructs();

        while (!currentClassName.empty() &&
               customStructs.count(currentClassName)) {
          auto const &def = customStructs.at(currentClassName);
          for (auto const &[fName, field] : def.fields) {
            if (locals.find(fName) == locals.end()) {
              if (node->isStatic && field.isStatic) {
                locals[fName] = field.type.base;
              } else if (!node->isStatic) {
                locals[fName] = field.type.base;
              }
            }
          }
          currentClassName = def.baseClass;
        }
      }

      for (auto &arg : node->args)
        locals[arg.name] = arg.type;

      for (auto &stmt : node->body)
        stmt->accept(this);
    }
  }
  void visit(utopia::BlockNode *node) override {
    for (auto &stmt : node->statements)
      stmt->accept(this);
  }
  void visit(utopia::IfNode *node) override {
    for (auto &s : node->thenBody)
      s->accept(this);
    for (auto &s : node->elseBody)
      s->accept(this);
  }
  void visit(utopia::WhileNode *node) override {
    for (auto &s : node->body)
      s->accept(this);
  }
  void visit(utopia::ForNode *node) override {
    if (node->init)
      node->init->accept(this);
    for (auto &s : node->body)
      s->accept(this);
  }
  // Stubs para evitar ruidos
  void visit(utopia::StructDeclNode *node) override {
    for (auto &m : node->methods)
      m->accept(this);
  }
  void visit(utopia::ProgramNode *node) override {
    for (auto &s : node->structs)
      s->accept(this);
    for (auto &f : node->functions)
      f->accept(this);
  }
  void visit(utopia::ModuleNode *node) override {
    for (auto &s : node->structs)
      s->accept(this);
    for (auto &f : node->functions)
      f->accept(this);
  }
  void visit(utopia::ThisNode *) override {}
  void visit(utopia::SuperNode *) override {}
  void visit(utopia::MemberAccessNode *) override {}
  void visit(utopia::NullLiteralNode *) override {}
  void visit(utopia::BreakNode *) override {}
  void visit(utopia::ContinueNode *) override {}
  void visit(utopia::NullAssertNode *) override {}
  void visit(utopia::LogicalNotNode *) override {}
  void visit(utopia::NumberNode *) override {}
  void visit(utopia::FloatNode *) override {}
  void visit(utopia::BoolNode *) override {}
  void visit(utopia::StringNode *) override {}
  void visit(utopia::UnaryMinusNode *) override {}
  void visit(utopia::SubscriptNode *) override {}
  void visit(utopia::VariableNode *) override {}
  void visit(utopia::AddressOfNode *) override {}
  void visit(utopia::DerefNode *) override {}
  void visit(utopia::NewNode *) override {}
  void visit(utopia::DeleteNode *) override {}
  void visit(utopia::MoveNode *) override {}
  void visit(utopia::BinaryOpNode *) override {}
  void visit(utopia::CallNode *) override {}
  void visit(utopia::CastNode *) override {}
  void visit(utopia::AssignNode *) override {}
  void visit(utopia::ReturnNode *) override {}
  void visit(utopia::ExtensionNode *) override {}
};

// --- SERVIDOR PRINCIPAL ---

int main() {
  std::string line;
  while (std::getline(std::cin, line)) {
    if (line.rfind("Content-Length:", 0) != 0)
      continue;
    int len = std::stoi(line.substr(15));
    std::getline(std::cin, line); // saltar \r\n

    std::vector<char> buf(len);
    std::cin.read(buf.data(), len);
    json req = json::parse(std::string(buf.begin(), buf.end()));
    std::string method = req["method"];

    if (method == "initialize") {
      sendResponse({{"jsonrpc", "2.0"},
                    {"id", req["id"]},
                    {"result",
                     {{"capabilities",
                       {
                           {"textDocumentSync", 1},
                           {"completionProvider",
                            {{"triggerCharacters", {".", ":", "@"}}}},
                           {"definitionProvider", true},
                           {"hoverProvider", true},
                           {"documentSymbolProvider", true},
                           {"documentFormattingProvider",
                            true} // Tell the client we can handle the mess
                       }}}}});
    } else if (method == "textDocument/didOpen" ||
               method == "textDocument/didChange") {
      std::string uri = req["params"]["textDocument"]["uri"];
      std::string text = (method == "textDocument/didOpen")
                             ? req["params"]["textDocument"]["text"]
                             : req["params"]["contentChanges"][0]["text"];

      auto &state = documentStates[uri];
      state.updateContent(text);

      json diags = json::array();
      try {
        utopia::Lexer lexer(text);
        auto tokens = lexer.tokenize();
        utopia::Parser parser(tokens);
        state.ast = parser.parseModule(uriToPath(uri));

        state.loader = utopia::ModuleLoader();
        state.loader.addSearchPath(fs::path(uriToPath(uri)).parent_path());

        // Cargar preludios e imports
        for (auto &imp : state.ast->imports)
          state.loader.loadModule(imp, fs::path(uriToPath(uri)).parent_path());

        std::vector<utopia::ModuleNode *> modules =
            state.loader.getAllModules();
        modules.push_back(state.ast.get());

        if (!state.sema.analyzeModules(modules)) {
          for (auto &err : state.sema.getErrors()) {
            diags.push_back({{"range", createRange(err.line, err.col,
                                                   err.endLine, err.endCol)},
                             {"severity", 1},
                             {"message", err.message},
                             {"source", "Utopia Sema"}});
          }
        }
      } catch (const std::exception &e) {
        // Error de Parser simplificado para diagnóstico
        diags.push_back({{"range", createRange(1, 1, 1, 1)},
                         {"severity", 1},
                         {"message", e.what()}});
      }
      sendResponse({{"jsonrpc", "2.0"},
                    {"method", "textDocument/publishDiagnostics"},
                    {"params", {{"uri", uri}, {"diagnostics", diags}}}});
    } else if (method == "textDocument/completion") {
      std::string uri = req["params"]["textDocument"]["uri"];
      int lspLine = req["params"]["position"]["line"].get<int>();
      int col = req["params"]["position"]["character"];
      auto &state = documentStates[uri];

      json items = json::array();

      if (lspLine >= static_cast<int>(state.lines.size())) {
        sendResponse(
            {{"jsonrpc", "2.0"}, {"id", req["id"]}, {"result", items}});
        continue;
      }

      const std::string &currentLineText = state.lines[lspLine];
      std::string prefix = getWordBeforeDot(state.lines, lspLine, col);

      bool isDecorator = false;
      if (col > 0 && col <= currentLineText.length()) {
        int checkPos = col - 1;
        while (checkPos >= 0 && std::isalnum(static_cast<unsigned char>(
                                    currentLineText[checkPos])))
          checkPos--;

        if (checkPos >= 0 && currentLineText[checkPos] == '@')
          isDecorator = true;
      }

      if (isDecorator) {
        static const char *const decorators[] = {"virtual", "override", "deprecated",
                                                 "noinline", "naked"};
        for (const auto &dec : decorators) {
          items.push_back(
              {{"label", dec}, {"kind", 14}, {"detail", "Decorator"}});
        }
      } else if (!prefix.empty()) {
        // --- COMPLETADO DE MIEMBROS (OBJETO.X) ---
        SymbolResolver res(lspLine + 1, state.sema);
        if (state.ast)
          state.ast->accept(&res);

        std::string typeName =
            res.locals.count(prefix) ? res.locals[prefix] : prefix;
        // Limpiar punteros de la cadena de tipo
        typeName.erase(std::remove(typeName.begin(), typeName.end(), '*'),
                       typeName.end());

        if (state.sema.getCustomStructs().count(typeName)) {
          auto &def = state.sema.getCustomStructs().at(typeName);
          for (auto const &[fName, field] : def.fields) {
            items.push_back(
                {{"label", fName}, {"kind", 5}, {"detail", field.type.base}});
          }
          std::string mPrefix = typeName + "_";
          for (auto const &[baseName, overloads] :
               state.sema.getOverloadTable()) {
            if (baseName.find(mPrefix) == 0) {
              items.push_back({{"label", baseName.substr(mPrefix.length())},
                               {"kind", 2},
                               {"detail", "Method"}});
            }
          }
        }
      } else {
        // --- GLOBAL COMPLETION ---

        /*
         * Dump the lexicon.
         * The parser is likely bleeding out from a syntax error while the
         * user is typing. We blindly feed the client all keywords and let the
         * editor sort out the mess.
         */
        static const char *const keywords[] = {
            "import",   "int",      "uint",    "float",        "double",
            "char",     "uchar",    "short",   "ushort",       "long",
            "ulong",    "bool",     "void",    "const",        "true",
            "false",    "return",   "new",     "delete",       "move",
            "null",     "if",       "else",    "while",        "for",
            "break",    "continue", "inline",  "force_inline", "struct",
            "class",    "public",   "private", "this",         "super",
            "required", "static",   "extends", "implements",   "extension",
            "on",       "as"};

        // 14 = LSP CompletionItemKind::Keyword
        for (const auto &kw : keywords) {
          items.push_back({{"label", kw}, {"kind", 14}});
        }

        for (auto const &[name, _] : state.sema.getCustomStructs())
          items.push_back({{"label", name}, {"kind", 7}});

        for (auto const &[name, _] : state.sema.getOverloadTable())
          if (name.find('_') == std::string::npos && name != "main")
            items.push_back({{"label", name}, {"kind", 3}});

        if (state.ast) {
          /* * Global vars are anchored to the AST root, not Sema's overload
           * table. Harvest them directly before descending into the local
           * scopes.
           */
          for (const auto &gvar : state.ast->globalVars) {
            items.push_back({{"label", gvar->name},
                             {"kind", 6}, // 6 = Variable
                             {"detail", gvar->typeName}});
          }
        }

        SymbolResolver res(lspLine, state.sema);
        if (state.ast)
          state.ast->accept(&res);

        for (auto const &[name, type] : res.locals)
          items.push_back({{"label", name}, {"kind", 6}, {"detail", type}});
      }
      sendResponse({{"jsonrpc", "2.0"}, {"id", req["id"]}, {"result", items}});
    } else if (method == "textDocument/definition") {
      std::string uri = req["params"]["textDocument"]["uri"];
      auto &state = documentStates[uri];
      std::string word =
          getWordAt(state.lines, req["params"]["position"]["line"],
                    req["params"]["position"]["character"]);

      json loc = nullptr;
      for (auto *mod : state.loader.getAllModules()) {
        for (auto &st : mod->structs)
          if (st->name == word)
            loc =
                createLocation("file://" + mod->filename, st->line, st->column,
                               st->line, st->column + word.length());
        for (auto &f : mod->functions)
          if (f->name == word)
            loc = createLocation("file://" + mod->filename, f->line, f->column,
                                 f->line, f->column + word.length());
      }
      if (loc == nullptr && state.ast) {
        for (auto &st : state.ast->structs)
          if (st->name == word)
            loc = createLocation(uri, st->line, st->column, st->line,
                                 st->column + word.length());
        for (auto &f : state.ast->functions)
          if (f->name == word)
            loc = createLocation(uri, f->line, f->column, f->line,
                                 f->column + word.length());
      }
      sendResponse({{"jsonrpc", "2.0"}, {"id", req["id"]}, {"result", loc}});
    } else if (method == "textDocument/hover") {
      std::string uri = req["params"]["textDocument"]["uri"];
      int line = req["params"]["position"]["line"].get<int>() + 1;
      int col = req["params"]["position"]["character"].get<int>() + 1;

      auto &state = documentStates[uri];
      if (!state.ast) {
        sendResponse(
            {{"jsonrpc", "2.0"}, {"id", req["id"]}, {"result", nullptr}});
        continue;
      }

      NodeFinder finder(line, col);
      state.ast->accept(&finder);

      if (!finder.bestMatch) {
        sendResponse(
            {{"jsonrpc", "2.0"}, {"id", req["id"]}, {"result", nullptr}});
        continue;
      }

      utopia::ASTNode *target = finder.bestMatch;
      utopia::ASTNode *declNode = target; // Fallback to itself
      std::string markdown = "";
      std::string typeInfo = "";
      std::string symbolName = "";

      // Resolve the actual name for lookup
      if (auto v = dynamic_cast<utopia::VariableNode *>(target))
        symbolName = v->name;
      else if (auto c = dynamic_cast<utopia::CallNode *>(target))
        symbolName = c->callee;
      else if (auto n = dynamic_cast<utopia::NewNode *>(target))
        symbolName = n->typeName;
      else if (auto s = dynamic_cast<utopia::StructDeclNode *>(target))
        symbolName = s->name;
      else if (auto f = dynamic_cast<utopia::FunctionNode *>(target))
        symbolName = f->name;
      else if (auto vd = dynamic_cast<utopia::VarDeclNode *>(target))
        symbolName = vd->name;

      // SYMBOL RESOLUTION
      // We hunt for the declaration to steal its soul and its documentation.
      std::vector<utopia::ModuleNode *> searchScope =
          state.loader.getAllModules();
      if (state.ast)
        searchScope.push_back(state.ast.get());

      bool foundDecl = false;
      for (auto *mod : searchScope) {
        for (auto &st : mod->structs)
          if (st->name == symbolName) {
            declNode = st.get();
            foundDecl = true;
            break;
          }
        if (foundDecl)
          break;
        for (auto &f : mod->functions)
          if (f->name == symbolName) {
            declNode = f.get();
            foundDecl = true;
            break;
          }
        if (foundDecl)
          break;
        for (auto &g : mod->globalVars)
          if (g->name == symbolName) {
            declNode = g.get();
            foundDecl = true;
            break;
          }
        if (foundDecl)
          break;
      }

      // Local variable type resolution via Sema or local SymbolResolver
      if (state.sema.nodeTypes.count(target)) {
        typeInfo = "Type: `" +
                   utopia::typeToString(state.sema.nodeTypes[target]) + "`";
      } else {
        // Last ditch effort: crawl the local scope
        SymbolResolver res(line, state.sema);
        state.ast->accept(&res);
        if (res.locals.count(symbolName)) {
          typeInfo = "Local Variable: `" + res.locals[symbolName] + "`";
        }
      }

      // MARKDOWN CONSTRUCTION
      std::string kind = "Symbol";
      if (dynamic_cast<utopia::FunctionNode *>(declNode))
        kind = "Function";
      else if (dynamic_cast<utopia::StructDeclNode *>(declNode))
        kind = "Type";
      else if (dynamic_cast<utopia::VarDeclNode *>(declNode) ||
               dynamic_cast<utopia::VariableNode *>(target))
        kind = "Variable";

      markdown += "### " + kind + " " +
                  (symbolName.empty() ? "" : "`" + symbolName + "`") + "\n\n";

      // Only show signature if it's something meaningful
      std::string sig = getSignatureMarkup(declNode);
      if (sig != "```utp\n\n```") {
        markdown += sig + "\n\n";
      }

      if (!typeInfo.empty()) {
        markdown += "---\n" + typeInfo + "\n\n";
      }

      if (!declNode->doc.empty()) {
        markdown += "---\n" + declNode->doc + "\n\n";
      }

      json result = {{"contents", {{"kind", "markdown"}, {"value", markdown}}}};
      sendResponse({{"jsonrpc", "2.0"}, {"id", req["id"]}, {"result", result}});
    } else if (method == "textDocument/formatting") {
      std::string uri = req["params"]["textDocument"]["uri"];
      auto &state = documentStates[uri];
      int tabSize = req["params"]["options"]["tabSize"];

      utopia::Lexer lexer(state.content);
      auto tokens = lexer.tokenize();

      std::string formattedCode;
      int indentLevel = 0;
      bool startOfLine = true;
      int parenDepth = 0;
      int lastTokenLine = -1;

      for (size_t i = 0; i < tokens.size(); ++i) {
        const auto &tok = tokens[i];

        // Vertical space sync.
        // Evil line-delta hacking to preserve user's original empty lines.
        if (lastTokenLine != -1 && tok.leadingDoc.empty()) {
          int linesToAdd = tok.line - lastTokenLine;
          if (startOfLine && linesToAdd > 0)
            linesToAdd -= 1;
          linesToAdd = std::max(0, linesToAdd);
          for (int l = 0; l < linesToAdd; ++l) {
            formattedCode += "\n";
            startOfLine = true;
          }
        }

        if (!tok.leadingDoc.empty()) {
          if (!startOfLine) {
            formattedCode += "\n";
            startOfLine = true;
          }

          // Force a visual gap if the comment block was deliberately detached.
          if (lastTokenLine != -1 && tok.line > lastTokenLine + 2) {
            formattedCode += "\n";
          }

          std::istringstream docStream(tok.leadingDoc);
          std::string docLine;
          while (std::getline(docStream, docLine)) {
            bool isEmpty = true;
            for (char c : docLine) {
              if (!std::isspace(c)) {
                isEmpty = false;
                break;
              }
            }

            if (isEmpty) {
              formattedCode += "\n";
              continue;
            }

            for (int j = 0; j < indentLevel * tabSize; ++j)
              formattedCode += " ";

            // Fast slash detection to prevent double commenting.
            // what the fuck? the lexer sometimes eats the slashes and sometimes
            // doesn't.
            bool hasSlashes = false;
            int k = 0;
            while (k < docLine.length() &&
                   std::isspace(static_cast<unsigned char>(docLine[k])))
              k++;
            if (k + 1 < docLine.length() && docLine[k] == '/' &&
                (docLine[k + 1] == '/' || docLine[k + 1] == '*')) {
              hasSlashes = true;
            }

            if (hasSlashes) {
              formattedCode += docLine + "\n";
            } else {
              formattedCode += "// " + docLine + "\n";
            }
          }
          startOfLine = true;
        }

        if (tok.type == utopia::TokenType::RBRACE) {
          indentLevel = std::max(0, indentLevel - 1);
          if (!startOfLine) {
            formattedCode += "\n";
            startOfLine = true;
          }
        }

        if (startOfLine && tok.type != utopia::TokenType::EOF_TOK) {
          for (int j = 0; j < indentLevel * tabSize; ++j)
            formattedCode += " ";
          startOfLine = false;
        }

        if (tok.type == utopia::TokenType::EOF_TOK)
          break;

        if (tok.type == utopia::TokenType::STRING) {
          formattedCode += "\"" + tok.value + "\"";
        } else {
          formattedCode += tok.value;
        }

        // Descent into parenthesis hell.
        if (tok.type == utopia::TokenType::LPAREN) {
          parenDepth++;
        } else if (tok.type == utopia::TokenType::RPAREN) {
          parenDepth = std::max(0, parenDepth - 1);
        }

        if (tok.type == utopia::TokenType::LBRACE) {
          indentLevel++;
          formattedCode += "\n";
          startOfLine = true;
        } else if (tok.type == utopia::TokenType::SEMICOLON) {
          if (parenDepth == 0) {
            formattedCode += "\n";
            startOfLine = true;
          } else {
            formattedCode += " ";
          }
        } else if (tok.type == utopia::TokenType::RBRACE) {
          formattedCode += "\n";
          startOfLine = true;
        } else {
          if (i + 1 < tokens.size()) {
            auto nextT = tokens[i + 1].type;
            bool addSpace = true;

            if (tok.type == utopia::TokenType::LPAREN ||
                tok.type == utopia::TokenType::LBRACKET ||
                tok.type == utopia::TokenType::DOT ||
                tok.type == utopia::TokenType::AT ||
                tok.type == utopia::TokenType::TILDE ||
                tok.type == utopia::TokenType::BANG) {
              addSpace = false;
            }

            if (nextT == utopia::TokenType::RPAREN ||
                nextT == utopia::TokenType::RBRACKET ||
                nextT == utopia::TokenType::LBRACKET ||
                nextT == utopia::TokenType::DOT ||
                nextT == utopia::TokenType::SEMICOLON ||
                nextT == utopia::TokenType::COMMA) {
              addSpace = false;
            }

            if (nextT == utopia::TokenType::LPAREN &&
                (tok.type == utopia::TokenType::IDENTIFIER ||
                 tok.type == utopia::TokenType::KW_SUPER ||
                 tok.type == utopia::TokenType::KW_THIS)) {
              addSpace = false;
            }

            if (nextT == utopia::TokenType::STAR &&
                (tok.type == utopia::TokenType::IDENTIFIER ||
                 tok.type == utopia::TokenType::KW_CHAR ||
                 tok.type == utopia::TokenType::KW_INT ||
                 tok.type == utopia::TokenType::KW_FLOAT)) {
              addSpace = false;
            }

            if (nextT == utopia::TokenType::PLUS_PLUS ||
                nextT == utopia::TokenType::MINUS_MINUS) {
              addSpace = false;
            }
            if ((tok.type == utopia::TokenType::PLUS_PLUS ||
                 tok.type == utopia::TokenType::MINUS_MINUS) &&
                nextT == utopia::TokenType::IDENTIFIER) {
              addSpace = false;
            }

            if (tok.type == utopia::TokenType::KW_AS ||
                nextT == utopia::TokenType::KW_AS) {
              addSpace = true;
            }

            if (addSpace) {
              formattedCode += " ";
            }
          }
        }

        lastTokenLine = tok.line;
      }

      json result = json::array(
          {{{"range", createRange(1, 1, (int)state.lines.size() + 1, 1)},
            {"newText", formattedCode}}});

      sendResponse({{"jsonrpc", "2.0"}, {"id", req["id"]}, {"result", result}});
    } else if (method == "shutdown") {
      sendResponse(
          {{"jsonrpc", "2.0"}, {"id", req["id"]}, {"result", nullptr}});
      break;
    }
  }
  return 0;
}