#include "utopia/AST/ASTVisitor.hpp"
#include "utopia/Driver/ModuleLoader.hpp"
#include "utopia/Lexer/Lexer.hpp"
#include "utopia/Parser/Parser.hpp"
#include "utopia/Sema/Sema.hpp"
#include <filesystem>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

using json = nlohmann::json;

struct FileState {
  std::unique_ptr<utopia::ModuleNode> ast;
  utopia::Sema sema;
  std::string content;
  utopia::ModuleLoader loader;
};

std::map<std::string, FileState> documentStates;

void sendResponse(const json &response) {
  std::string content = response.dump();
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

void publishDiagnostics(const std::string &uri, const json &diagnostics) {
  json notification = {
      {"jsonrpc", "2.0"},
      {"method", "textDocument/publishDiagnostics"},
      {"params", {{"uri", uri}, {"diagnostics", diagnostics}}}};
  sendResponse(notification);
}

json createRange(int sl, int sc, int el, int ec) {
  return {{"start",
           {{"line", sl - 1 < 0 ? 0 : sl - 1},
            {"character", sc - 1 < 0 ? 0 : sc - 1}}},
          {"end",
           {{"line", el - 1 < 0 ? 0 : el - 1},
            {"character", ec - 1 < 0 ? 0 : ec - 1}}}};
}

json createLocation(const std::string &uri, int sl, int sc, int el, int ec) {
  return {{"uri", uri}, {"range", createRange(sl, sc, el, ec)}};
}

std::string getWordAt(const std::string &text, int targetLine, int targetCol) {
  int currentLine = 0;
  size_t pos = 0;

  while (currentLine < targetLine && pos < text.length()) {
    if (text[pos] == '\n')
      currentLine++;
    pos++;
  }

  if (pos >= text.length())
    return "";

  size_t lineStart = pos;
  size_t cursorAbs = lineStart + targetCol;

  if (cursorAbs >= text.length())
    return "";

  long left = cursorAbs;
  long right = cursorAbs;

  auto isIdent = [](char c) { return std::isalnum(c) || c == '_'; };

  while (left >= (long)lineStart && isIdent(text[left]))
    left--;
  while (right < (long)text.length() && text[right] != '\n' &&
         isIdent(text[right]))
    right++;

  left++;
  if (left < right)
    return text.substr(left, right - left);
  return "";
}

// Memory block traversal to fetch l-value before a dot.
std::string getWordBeforeDot(const std::string &text, int targetLine,
                             int targetCol) {
  size_t absPos = 0;
  int curLine = 0;
  while (curLine < targetLine && absPos < text.length()) {
    if (text[absPos] == '\n')
      curLine++;
    absPos++;
  }
  if (absPos >= text.length())
    return "";

  long cursorAbs = absPos + targetCol - 1;
  if (cursorAbs >= (long)text.length())
    cursorAbs = text.length() - 1;

  auto isIdent = [](char c) { return std::isalnum(c) || c == '_'; };
  long tempCursor = cursorAbs;

  // Retroceder por encima del ruido actual de tipado (ej. 'p1.st|')
  while (tempCursor >= 0 && isIdent(text[tempCursor]))
    tempCursor--;
  while (tempCursor >= 0 && std::isspace(text[tempCursor]))
    tempCursor--;

  if (tempCursor >= 0 && text[tempCursor] == '.') {
    tempCursor--;
    while (tempCursor >= 0 && std::isspace(text[tempCursor]))
      tempCursor--;

    long left = tempCursor;
    while (left >= 0 && isIdent(text[left]))
      left--;
    left++;

    if (left <= tempCursor)
      return text.substr(left, tempCursor - left + 1);
  }
  return "";
}

json findDefinition(const std::string &word, const std::string &uri) {
  if (documentStates.find(uri) == documentStates.end())
    return nullptr;

  auto &state = documentStates[uri];
  std::vector<utopia::ModuleNode *> modulesToSearch =
      state.loader.getAllModules();

  if (state.ast) {
    modulesToSearch.push_back(state.ast.get());
  }

  for (auto *mod : modulesToSearch) {
    std::string modUri = "file://" + mod->filename;

    for (const auto &st : mod->structs) {
      if (st->name == word) {
        return createLocation(modUri, st->line, st->column, st->endLine,
                              st->endColumn);
      }
      for (const auto &m : st->methods) {
        if (m->name == word) {
          return createLocation(modUri, m->line, m->column, m->endLine,
                                m->endColumn);
        }
      }
      for (const auto &f : st->fields) {
        if (f.name == word) {
          return createLocation(modUri, st->line, st->column, st->endLine,
                                st->endColumn);
        }
      }
    }
    for (const auto &f : mod->functions) {
      if (f->name == word) {
        return createLocation(modUri, f->line, f->column, f->endLine,
                              f->endColumn);
      }
    }
  }
  return nullptr;
}

class LocalSymbolCollector : public utopia::ASTVisitor {
public:
  int targetLine;
  json &completions;
  std::map<std::string, std::string> localVars;

  LocalSymbolCollector(int line, json &comp)
      : targetLine(line), completions(comp) {}

  void visit(utopia::ModuleNode *node) override {
    for (auto &f : node->functions)
      f->accept(this);
    for (auto &s : node->structs)
      s->accept(this);
  }

  void visit(utopia::StructDeclNode *node) override {
    if (targetLine >= node->line && targetLine <= node->endLine) {
      for (auto &m : node->methods)
        m->accept(this);
    }
  }

  void visit(utopia::FunctionNode *node) override {
    if (targetLine >= node->line && targetLine <= node->endLine + 5) {
      // Fast-path para instanciar 'this'. El contexto no tiene AST scope,
      // forzamos el símbolo.
      if (!node->isStatic &&
          (node->isMethod || node->isConstructor || node->isDestructor)) {
        localVars["this"] = node->className;
        completions.push_back({{"label", "this"},
                               {"kind", 14},
                               {"detail", node->className + " (Instance)"}});
      }

      for (auto &arg : node->args) {
        localVars[arg.name] = arg.type;
        completions.push_back({{"label", arg.name},
                               {"kind", 6},
                               {"detail", arg.type + " (Parameter)"}});
      }
      for (auto &stmt : node->body)
        stmt->accept(this);
    }
  }

  void visit(utopia::BlockNode *node) override {
    for (auto &stmt : node->statements) {
      if (stmt->line <= targetLine && stmt->endLine >= targetLine) {
        stmt->accept(this);
      } else if (stmt->line <= targetLine) {
        if (auto v = dynamic_cast<utopia::VarDeclNode *>(stmt.get())) {
          v->accept(this);
        }
      }
    }
  }

  void visit(utopia::VarDeclNode *node) override {
    if (node->line <= targetLine) {
      localVars[node->name] = node->typeName;
      completions.push_back({{"label", node->name},
                             {"kind", 6},
                             {"detail", node->typeName + " (Local)"}});
    }
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

  void visit(utopia::ThisNode *) override {}
  void visit(utopia::ExtensionNode *) override {}
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
  void visit(utopia::AssignNode *) override {}
  void visit(utopia::ReturnNode *) override {}
  void visit(utopia::ProgramNode *) override {}
};

json generateDocumentSymbols(utopia::ModuleNode *ast) {
  json symbols = json::array();

  for (const auto &st : ast->structs) {
    json structSym = {
        {"name", st->name},
        {"kind", st->isClass ? 5 : 23},
        {"range",
         createRange(st->line, st->column, st->endLine, st->endColumn)},
        {"selectionRange", createRange(st->line, st->column, st->line,
                                       st->column + st->name.length())}};

    json children = json::array();
    for (const auto &f : st->fields) {
      children.push_back({{"name", f.name},
                          {"kind", 13},
                          {"range", createRange(0, 0, 0, 0)},
                          {"selectionRange", createRange(0, 0, 0, 0)}});
    }
    for (const auto &m : st->methods) {
      children.push_back(
          {{"name", m->name},
           {"kind", 6},
           {"range", createRange(m->line, m->column, m->endLine, m->endColumn)},
           {"selectionRange", createRange(m->line, m->column, m->line,
                                          m->column + m->name.length())}});
    }
    structSym["children"] = children;
    symbols.push_back(structSym);
  }

  for (const auto &fn : ast->functions) {
    if (fn->name == "main")
      continue;
    symbols.push_back(
        {{"name", fn->name},
         {"kind", 12},
         {"range",
          createRange(fn->line, fn->column, fn->endLine, fn->endColumn)},
         {"selectionRange", createRange(fn->line, fn->column, fn->line,
                                        fn->column + fn->name.length())}});
  }

  return symbols;
}

int main() {
  std::cin.tie(NULL);
  std::cout.tie(NULL);
  std::ios_base::sync_with_stdio(false);

  while (true) {
    std::string line;
    int contentLength = 0;

    while (std::getline(std::cin, line)) {
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      if (line.empty()) {
        break;
      }
      if (line.rfind("Content-Length:", 0) == 0) {
        contentLength = std::stoi(line.substr(15));
      }
    }

    if (std::cin.eof() || std::cin.fail()) {
      break;
    }

    if (contentLength == 0)
      continue;

    std::vector<char> buffer(contentLength);
    std::cin.read(buffer.data(), contentLength);
    std::string body(buffer.begin(), buffer.end());

    try {
      json request = json::parse(body);

      if (!request.contains("method"))
        continue;
      std::string method = request["method"];

      if (method == "initialize") {
        json response = {
            {"jsonrpc", "2.0"},
            {"id", request["id"]},
            {"result",
             {{"capabilities",
               {{"textDocumentSync", 1},
                {"completionProvider",
                 {{"resolveProvider", false},
                  {"triggerCharacters", {".", " ", ":"}}}},
                {"documentSymbolProvider", true},
                {"hoverProvider", true},
                {"definitionProvider", true},
                {"signatureHelpProvider",
                 {{"triggerCharacters", {"(", ","}}}}}},
              {"serverInfo", {{"name", "UtopiaLSP"}, {"version", "0.2.0"}}}}}};
        sendResponse(response);
        logMessage("Servidor UtopiaLSP inicializado con soporte extendido.");
      } else if (method == "textDocument/didOpen" ||
                 method == "textDocument/didChange") {
        std::string uri = request["params"]["textDocument"]["uri"];
        std::string text = (method == "textDocument/didOpen")
                               ? request["params"]["textDocument"]["text"]
                               : request["params"]["contentChanges"][0]["text"];

        // 1. Declarar filePath antes de usarlo
        std::string filePath = uri;
        if (filePath.find("file://") == 0) {
          filePath = filePath.substr(7);
        }

        if (documentStates.find(uri) == documentStates.end()) {
          documentStates[uri] = FileState();
        }
        documentStates[uri].content = text;

        json diagnostics = json::array();

        try {
          utopia::Lexer lexer(text);
          auto tokens = lexer.tokenize();
          utopia::Parser parser(tokens);
          auto ast = parser.parseModule(uri);

          // 2. Usar el estado persistente del documento
          auto &state = documentStates[uri];
          state.content = text;
          state.ast = std::move(ast);

          // Limpiamos rutas previas para evitar duplicados en cada cambio
          // state.loader.clearSearchPaths();

          std::filesystem::path currentDir =
              std::filesystem::path(filePath).parent_path();
          state.loader.addSearchPath(currentDir);

          std::filesystem::path current = currentDir;
          while (current.has_parent_path()) {
            if (std::filesystem::exists(current / "build.yaml")) {
              state.loader.addSearchPath(current / "src");
              state.loader.setSystemPath(current / "libs");
              break;
            }
            current = current.parent_path();
          }

#ifdef UTOPIA_DEBUG_BUILD
          std::filesystem::path internalPath =
              std::filesystem::path(UTOPIA_SOURCE_DIR) / "libs";
#else
#ifdef UTOPIA_INTERNAL_LIB_PATH
          std::filesystem::path internalPath = UTOPIA_INTERNAL_LIB_PATH;
#else
          std::filesystem::path internalPath = "/usr/local/lib/utopia";
#endif
#endif
          if (std::filesystem::exists(internalPath)) {
            state.loader.setSystemPath(internalPath);
          }

          for (const auto &imp : state.ast->imports) {
            state.loader.loadModule(imp, currentDir);
          }

          std::vector<utopia::ModuleNode *> modules =
              state.loader.getAllModules();
          modules.push_back(state.ast.get());
          state.sema.analyzeModules(modules);

        } catch (const std::exception &e) {
          std::string errStr = e.what();
          int sLine = 0, sCol = 0, eLine = 0, eCol = 0;
          std::string msg = errStr;

          std::vector<size_t> seps;
          size_t pos = errStr.find(':');
          while (pos != std::string::npos && seps.size() < 3) {
            seps.push_back(pos);
            pos = errStr.find(':', pos + 1);
          }
          size_t pipe = errStr.find('|');

          if (seps.size() == 3 && pipe != std::string::npos) {
            sLine = std::stoi(errStr.substr(0, seps[0])) - 1;
            sCol =
                std::stoi(errStr.substr(seps[0] + 1, seps[1] - seps[0] - 1)) -
                1;
            eLine =
                std::stoi(errStr.substr(seps[1] + 1, seps[2] - seps[1] - 1)) -
                1;
            eCol =
                std::stoi(errStr.substr(seps[2] + 1, pipe - seps[2] - 1)) - 1;
            msg = errStr.substr(pipe + 1);
          }

          diagnostics.push_back(
              {{"range",
                {{"start", {{"line", sLine}, {"character", sCol}}},
                 {"end", {{"line", eLine}, {"character", eCol}}}}},
               {"severity", 1},
               {"message", msg},
               {"source", "Utopia"}});
        }
        publishDiagnostics(uri, diagnostics);
      } else if (method == "textDocument/documentSymbol") {
        std::string uri = request["params"]["textDocument"]["uri"];
        json symbols = json::array();

        if (documentStates.count(uri) && documentStates[uri].ast) {
          symbols = generateDocumentSymbols(documentStates[uri].ast.get());
        }

        json response = {
            {"jsonrpc", "2.0"}, {"id", request["id"]}, {"result", symbols}};
        sendResponse(response);
      } else if (method == "textDocument/completion") {
        std::string uri = request["params"]["textDocument"]["uri"];
        int line = request["params"]["position"]["line"];
        int col = request["params"]["position"]["character"];

        json completions = json::array();

        if (documentStates.count(uri)) {
          auto &state = documentStates[uri];
          std::string prefixWord = getWordBeforeDot(state.content, line, col);

          LocalSymbolCollector collector(line, completions);
          if (state.ast) {
            state.ast->accept(&collector);
          }

          if (!prefixWord.empty()) {
            // Drop locals. We are drilling into a member access.
            completions.clear();

            std::string typeName = "";
            bool isStatic = false;

            if (collector.localVars.count(prefixWord)) {
              typeName = collector.localVars[prefixWord];

              // 0xDEADBEEF: Pointer stripping. El autocompletado solo necesita
              // la estructura base.
              size_t cleanEnd = typeName.find_first_of("*?&[");
              if (cleanEnd != std::string::npos) {
                typeName = typeName.substr(0, cleanEnd);
              }
            } else if (state.sema.getCustomStructs().count(prefixWord)) {
              typeName = prefixWord;
              isStatic = true;
            } else {
              // Fallback para tipos primitivos estáticos (ej. String.)
              typeName = prefixWord;
            }

            if (!typeName.empty()) {
              if (state.sema.getCustomStructs().count(typeName)) {
                const auto &structDef =
                    state.sema.getCustomStructs().at(typeName);
                for (const auto &[fName, field] : structDef.fields) {
                  if (field.isStatic == isStatic) {
                    completions.push_back(
                        {{"label", fName},
                         {"kind", 5},
                         {"detail", field.type.base + " (Field)"}});
                  }
                }
              }

              // Scrape the vtable para encontrar métodos e inyecciones de
              // extensiones
              std::string methodPrefix = typeName + "_";
              std::string extPrefix = "ext_" + typeName + "_";

              for (const auto &[baseName, overloads] :
                   state.sema.getOverloadTable()) {
                if (overloads.empty())
                  continue;

                std::string methodName = "";
                if (baseName.find(methodPrefix) == 0) {
                  methodName = baseName.substr(methodPrefix.length());
                } else if (!isStatic && baseName.find(extPrefix) == 0) {
                  methodName = baseName.substr(extPrefix.length());
                }

                if (!methodName.empty()) {
                  // Hide constructors.
                  if (methodName == typeName)
                    continue;

                  // Static/Instance VTable Routing
                  // It's a dirty heuristic, but it works. If the first
                  // parameter of the mangled signature matches the class name,
                  // we assume it's the injected 'this' pointer.
                  bool methodIsStatic = true;
                  if (baseName.find(extPrefix) == 0) {
                    methodIsStatic = false;
                  } else if (!overloads[0].paramTypes.empty() &&
                             overloads[0].paramTypes[0].base == typeName) {
                    methodIsStatic = false;
                  }

                  if (isStatic != methodIsStatic)
                    continue;

                  bool exists = false;
                  for (auto &c : completions) {
                    if (c["label"] == methodName) {
                      exists = true;
                      break;
                    }
                  }
                  if (!exists) {
                    completions.push_back(
                        {{"label", methodName},
                         {"kind", 2},
                         {"detail",
                          overloads[0].returnType.base + " (Method)"}});
                  }
                }
              }
            }
          } else {
            // Contexto global. Se inyecta Sema para asegurar que los módulos
            // importados sean sugeridos.
            for (const auto &[structName, def] :
                 state.sema.getCustomStructs()) {
              completions.push_back(
                  {{"label", structName},
                   {"kind", def.isClass ? 7 : 22},
                   {"detail", def.isClass ? "Class" : "Struct"}});
            }

            for (const auto &[baseName, overloads] :
                 state.sema.getOverloadTable()) {
              if (baseName.find('_') == std::string::npos &&
                  !overloads.empty()) {
                completions.push_back(
                    {{"label", baseName},
                     {"kind", 3},
                     {"detail", overloads[0].returnType.base + " (Function)"}});
              }
            }
          }
        }

        json response = {
            {"jsonrpc", "2.0"}, {"id", request["id"]}, {"result", completions}};
        sendResponse(response);
      } else if (method == "textDocument/definition") {
        std::string uri = request["params"]["textDocument"]["uri"];
        int line = request["params"]["position"]["line"];
        int col = request["params"]["position"]["character"];

        json result = nullptr;
        if (documentStates.count(uri)) {
          std::string targetWord =
              getWordAt(documentStates[uri].content, line, col);
          if (!targetWord.empty()) {
            result = findDefinition(targetWord, uri);
          }
        }

        json response = {
            {"jsonrpc", "2.0"}, {"id", request["id"]}, {"result", result}};
        sendResponse(response);
      } else if (method == "textDocument/hover") {
        std::string uri = request["params"]["textDocument"]["uri"];
        int line = request["params"]["position"]["line"];
        int col = request["params"]["position"]["character"];

        json result = nullptr;
        if (documentStates.count(uri)) {
          auto &state = documentStates[uri];
          std::string targetWord = getWordAt(state.content, line, col);

          if (!targetWord.empty()) {
            std::string markdown = "";
            const auto &overloads = state.sema.getOverloadTable();

            if (overloads.count(targetWord)) {
              for (const auto &cand : overloads.at(targetWord)) {
                markdown += "```utopia\n" + targetWord + "(";
                for (size_t i = 0; i < cand.paramTypes.size(); ++i) {
                  markdown += cand.paramTypes[i].base;
                  if (cand.paramTypes[i].ptrDepth > 0)
                    markdown += "*";
                  if (i < cand.paramTypes.size() - 1)
                    markdown += ", ";
                }
                markdown += ") -> " + cand.returnType.base + "\n```\n";
              }
            }

            if (!markdown.empty()) {
              result = {
                  {"contents", {{"kind", "markdown"}, {"value", markdown}}}};
            }
          }
        }

        json response = {
            {"jsonrpc", "2.0"}, {"id", request["id"]}, {"result", result}};
        sendResponse(response);
      } else if (method == "textDocument/signatureHelp") {
        std::string uri = request["params"]["textDocument"]["uri"];
        int line = request["params"]["position"]["line"];
        int col = request["params"]["position"]["character"];

        json result = {{"signatures", json::array()},
                       {"activeSignature", 0},
                       {"activeParameter", 0}};

        if (documentStates.count(uri)) {
          auto &state = documentStates[uri];

          int tempCol = col - 1;
          size_t lineStart = 0;
          int currentLine = 0;

          while (currentLine < line && lineStart < state.content.length()) {
            if (state.content[lineStart] == '\n')
              currentLine++;
            lineStart++;
          }

          /* Evil buffer offset hack. We walk the memory like it's 1999. Do not
           * trust line * 1000. */
          long absPos = lineStart + tempCol;
          if (absPos >= (long)state.content.length())
            absPos = state.content.length() - 1;

          while (absPos >= (long)lineStart && (state.content[absPos] == ' ' ||
                                               state.content[absPos] == '(')) {
            absPos--;
          }

          int relativeCol = absPos - lineStart;
          std::string targetWord = getWordAt(state.content, line, relativeCol);

          const auto &overloads = state.sema.getOverloadTable();
          if (overloads.count(targetWord)) {
            for (const auto &cand : overloads.at(targetWord)) {
              std::string sigLabel = targetWord + "(";
              json paramsArray = json::array();

              for (size_t i = 0; i < cand.paramTypes.size(); ++i) {
                std::string paramStr = cand.paramTypes[i].base;
                if (cand.paramTypes[i].ptrDepth > 0)
                  paramStr += "*";

                paramsArray.push_back({{"label", paramStr}});
                sigLabel += paramStr;
                if (i < cand.paramTypes.size() - 1)
                  sigLabel += ", ";
              }
              sigLabel += ") -> " + cand.returnType.base;

              json sig = {{"label", sigLabel},
                          {"documentation", "Utopia Function (Overload)"},
                          {"parameters", paramsArray}};
              result["signatures"].push_back(sig);
            }
          }
        }

        json response = {
            {"jsonrpc", "2.0"}, {"id", request["id"]}, {"result", result}};
        sendResponse(response);
      } else if (method == "exit") {
        break;
      }
    } catch (const std::exception &e) {
      // Ignorado para no romper el loop del servidor
    }
  }

  return 0;
}