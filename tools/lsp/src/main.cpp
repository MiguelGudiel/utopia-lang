#include "SearchVisitor.hpp"
#include "utopia/AST/ASTVisitor.hpp"
#include "utopia/Lexer/Lexer.hpp"
#include "utopia/Parser/Parser.hpp"
#include "utopia/Sema/Sema.hpp"
#include <iostream>
#include <map>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace utopia::lsp {

struct DocumentState {
  std::string text;
  std::unique_ptr<ModuleNode> ast;
  std::shared_ptr<DiagnosticsEngine> diags;
  std::shared_ptr<SemaContext> sema;
};

std::map<std::string, DocumentState> documents;

class LSPVisitor : public ASTVisitor<LSPVisitor, void> {
public:
  void visit(const NumberNode *n) {}
  void visit(const VariableNode *n) {}
  void visit(const BinaryOpNode *n) {
    dispatch(n->left);
    dispatch(n->right);
  }
  void visit(const ModuleNode *n) {
    for (auto &s : n->statements)
      dispatch(s);
  }
  void visit(const VarDeclNode *n) {
    if (n->initializer)
      dispatch(n->initializer);
  }
  void visit(const AssignNode *n) { dispatch(n->value); }
  void visit(const BlockNode *n) {
    for (auto &s : n->statements)
      dispatch(s);
  }
  void visit(const FunctionDeclNode *n) { dispatch(n->body); }
  void visit(const ReturnNode *n) {
    if (n->value)
      dispatch(n->value);
  }
  void visit(const FunctionCallNode *n) {
    for (auto &a : n->args)
      dispatch(a);
  }
  void visit(const CastNode *n) { dispatch(n->expr); }
};

void sendResponse(const json &res) {
  std::string content = res.dump();
  std::cout << "Content-Length: " << content.length() << "\r\n\r\n"
            << content << std::flush;
}

void handleHover(const json &req) {
  std::string uri = req["params"]["textDocument"]["uri"];
  int line = req["params"]["position"]["line"].get<int>() + 1;
  int col = req["params"]["position"]["character"].get<int>() + 1;

  json res = nullptr;
  if (documents.contains(uri)) {
    auto &doc = documents[uri];
    SearchVisitor searcher(line, col);
    const ASTNode *node = searcher.find(doc.ast);

    if (node) {
      std::string hoverText;

      if (node->kind == NodeKind::Variable) {
        auto varNode = static_cast<const VariableNode *>(node);
        auto symIt = doc.sema->symbolRegistry.find(std::string(varNode->name));
        if (symIt != doc.sema->symbolRegistry.end()) {
          hoverText = "```utopia\n" + std::string(symIt->second.type.base) +
                      " " + std::string(varNode->name) + "\n```";
          if (!symIt->second.doc.empty())
            hoverText += "\n---\n" + symIt->second.doc;
        }
      } else if (node->kind == NodeKind::FunctionCall) {
        auto callNode = static_cast<const FunctionCallNode *>(node);
        auto funcRes = doc.sema->getFunction(callNode->name, 0, 0, 0);
        if (funcRes) {
          hoverText = "```utopia\n" + std::string(funcRes->retType.base) + " " +
                      std::string(callNode->name) + "(";
          for (size_t i = 0; i < funcRes->paramTypes.size(); ++i) {
            hoverText += std::string(funcRes->paramTypes[i].base);
            if (i + 1 < funcRes->paramTypes.size())
              hoverText += ", ";
          }
          hoverText += ")\n```";
          if (!funcRes->doc.empty())
            hoverText += "\n---\n" + funcRes->doc;
        }
      } else if (node->kind == NodeKind::FunctionDecl) {
        auto declNode = static_cast<const FunctionDeclNode *>(node);
        hoverText = "```utopia\n" + std::string(declNode->returnType) + " " +
                    std::string(declNode->name) + "(...)\n```";
        if (!declNode->docString.empty())
          hoverText += "\n---\n" + declNode->docString;
      } else if (node->kind == NodeKind::VarDecl) {
        auto declNode = static_cast<const VarDeclNode *>(node);
        hoverText = "```utopia\n" + std::string(declNode->typeName) + " " +
                    std::string(declNode->varName) + "\n```";
        if (!declNode->docString.empty())
          hoverText += "\n---\n" + declNode->docString;
      }

      if (!hoverText.empty()) {
        res = {{"contents", {{"kind", "markdown"}, {"value", hoverText}}},
               {"range",
                {{"start",
                  {{"line", node->line - 1}, {"character", node->column - 1}}},
                 {"end",
                  {{"line", node->line - 1},
                   {"character", node->column - 1 + node->length}}}}}};
      }
    }
  }
  sendResponse({{"jsonrpc", "2.0"}, {"id", req["id"]}, {"result", res}});
}

void handleDefinition(const json &req) {
  std::string uri = req["params"]["textDocument"]["uri"];
  int line = req["params"]["position"]["line"].get<int>() + 1;
  int col = req["params"]["position"]["character"].get<int>() + 1;

  json res = nullptr;
  if (documents.contains(uri)) {
    auto &doc = documents[uri];
    SearchVisitor searcher(line, col);
    const ASTNode *node = searcher.find(doc.ast);

    if (node) {
      if (node->kind == NodeKind::Variable) {
        auto varNode = static_cast<const VariableNode *>(node);
        auto symIt = doc.sema->symbolRegistry.find(std::string(varNode->name));
        if (symIt != doc.sema->symbolRegistry.end()) {
          int defLine = symIt->second.line - 1;
          int defCol = symIt->second.column - 1;
          res = {{"uri", uri},
                 {"range",
                  {{"start", {{"line", defLine}, {"character", defCol}}},
                   {"end",
                    {{"line", defLine},
                     {"character", defCol + (int)varNode->name.length()}}}}}};
        }
      } else if (node->kind == NodeKind::FunctionCall) {
        auto callNode = static_cast<const FunctionCallNode *>(node);
        auto funcRes = doc.sema->getFunction(callNode->name, 0, 0, 0);
        if (funcRes) {
          int defLine = funcRes->line - 1;
          int defCol = funcRes->column - 1;
          res = {{"uri", uri},
                 {"range",
                  {{"start", {{"line", defLine}, {"character", defCol}}},
                   {"end",
                    {{"line", defLine},
                     {"character", defCol + (int)callNode->name.length()}}}}}};
        }
      } else if (node->kind == NodeKind::FunctionDecl) {
        auto declNode = static_cast<const FunctionDeclNode *>(node);
        res = {{"uri", uri},
               {"range",
                {{"start",
                  {{"line", declNode->line - 1},
                   {"character", declNode->column - 1}}},
                 {"end",
                  {{"line", declNode->line - 1},
                   {"character",
                    declNode->column - 1 + (int)declNode->name.length()}}}}}};
      } else if (node->kind == NodeKind::VarDecl) {
        auto declNode = static_cast<const VarDeclNode *>(node);
        res = {{"uri", uri},
               {"range",
                {{"start",
                  {{"line", declNode->line - 1},
                   {"character", declNode->column - 1}}},
                 {"end",
                  {{"line", declNode->line - 1},
                   {"character", declNode->column - 1 +
                                     (int)declNode->varName.length()}}}}}};
      }
    }
  }
  sendResponse({{"jsonrpc", "2.0"}, {"id", req["id"]}, {"result", res}});
}

void handleCompletion(const json &req) {
  json items = json::array(
      {{{"label", "int32"}, {"kind", 7}, {"detail", "Utopia Primitive"}},
       {{"label", "float32"}, {"kind", 7}},
       {{"label", "return"}, {"kind", 14}},
       {{"label", "as"},
        {"kind", 14},
        {"documentation", "Type casting operator"}}});
  sendResponse({{"jsonrpc", "2.0"}, {"id", req["id"]}, {"result", items}});
}

void processFile(const std::string &uri, std::string text) {
  auto diags = std::make_shared<DiagnosticsEngine>();

  /* string ownership transferred to DocumentState to prevent garbage memory in
   * AST string_views */
  auto &state = documents[uri];
  state.text = std::move(text);
  state.diags = std::make_shared<DiagnosticsEngine>();

  Lexer lexer(state.text);
  auto tokens = lexer.tokenize();
  Parser parser(tokens, *state.diags, uri);

  try {
    state.ast = parser.parseModule(uri);
    state.sema = std::make_shared<SemaContext>(*state.diags, uri);
    Sema sema(*state.sema);
    sema.dispatch(state.ast);
  } catch (...) {
  }

  documents[uri] = std::move(state);

  sendResponse(
      {{"jsonrpc", "2.0"},
       {"method", "textDocument/publishDiagnostics"},
       {"params", {{"uri", uri}, {"diagnostics", state.diags->toJSON()}}}});
}

} // namespace utopia::lsp

int main() {
  std::string line;
  while (std::getline(std::cin, line)) {
    if (line.find("Content-Length:") == 0) {
      int len = std::stoi(line.substr(15));
      while (std::getline(std::cin, line) && (line != "\r" && !line.empty()))
        ;

      std::vector<char> buf(len);
      std::cin.read(buf.data(), len);
      auto req = json::parse(std::string(buf.begin(), buf.end()));
      std::string method = req["method"];

      if (method == "initialize") {
        utopia::lsp::sendResponse(
            {{"jsonrpc", "2.0"},
             {"id", req["id"]},
             {"result",
              {{"capabilities",
                {{"textDocumentSync", 1},
                 {"hoverProvider", true},
                 {"definitionProvider", true},
                 {"completionProvider", {{"triggerCharacters", {"."}}}}}}}}});
      } else if (method == "textDocument/hover") {
        utopia::lsp::handleHover(req);
      } else if (method == "textDocument/definition") {
        utopia::lsp::handleDefinition(req);
      } else if (method == "textDocument/completion") {
        utopia::lsp::handleCompletion(req);
      } else if (method == "textDocument/didOpen" ||
                 method == "textDocument/didChange") {
        std::string uri = req["params"]["textDocument"]["uri"];
        std::string text =
            (method == "textDocument/didOpen")
                ? req["params"]["textDocument"]["text"].get<std::string>()
                : req["params"]["contentChanges"][0]["text"].get<std::string>();
        utopia::lsp::processFile(uri, std::move(text));
      } else if (method == "exit") {
        return 0;
      }
    }
  }
  return 0;
}