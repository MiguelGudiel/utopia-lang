// File: tools/lsp/main.cpp
#include "utopia/CodeGen/CodeGen.hpp"
#include "utopia/Lexer/Lexer.hpp"
#include "utopia/Parser/Parser.hpp"
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

using json = nlohmann::json;

// Helper function to send responses to VS Code using the LSP format
void sendResponse(const json &response) {
  std::string content = response.dump();
  std::cout << "Content-Length: " << content.length() << "\r\n\r\n"
            << content << std::flush;
}

// Sends log messages (visible in VS Code "Output" tab)
void logMessage(const std::string &message) {
  json notification = {{"jsonrpc", "2.0"},
                       {"method", "window/logMessage"},
                       {"params",
                        {{"type", 3}, // 3 = Info
                         {"message", "[UtopiaLSP] " + message}}}};
  sendResponse(notification);
}

void publishDiagnostics(const std::string &uri, const json &diagnostics) {
  json notification = {
      {"jsonrpc", "2.0"},
      {"method", "textDocument/publishDiagnostics"},
      {"params", {{"uri", uri}, {"diagnostics", diagnostics}}}};
  sendResponse(notification);
}

int main() {
  // Crucial configuration for binary communication with VS Code
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
               {// Full sync (sends the whole file on change)
                {"textDocumentSync", 1},
                // Ability to provide autocompletion
                {"completionProvider",
                 {{"resolveProvider", false},
                  {"triggerCharacters", {".", " "}}}}}},
              {"serverInfo", {{"name", "UtopiaLSP"}, {"version", "0.1.0"}}}}}};
        sendResponse(response);
        logMessage("Servidor inicializado correctamente.");
      } else if (method == "textDocument/didOpen" ||
                 method == "textDocument/didChange") {
        std::string uri = request["params"]["textDocument"]["uri"];
        std::string text;

        if (method == "textDocument/didOpen") {
          text = request["params"]["textDocument"]["text"];
        } else {
          text = request["params"]["contentChanges"][0]["text"];
        }

        json diagnostics = json::array();

        try {
          utopia::Lexer lexer(text);
          auto tokens = lexer.tokenize();
          utopia::Parser parser(tokens);
          auto ast = parser.parseProgram();

          utopia::CodeGen validator(uri, false);
          validator.generate(ast.get());
        } catch (const std::exception &e) {
          std::string errStr = e.what();
          int sLine = 0, sCol = 0, eLine = 0, eCol = 0;
          std::string msg = errStr;

          // Find separators for format SL:SC:EL:EC|MSG
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
      } else if (method == "textDocument/completion") {
        json response = {
            {"jsonrpc", "2.0"},
            {"id", request["id"]},
            {"result",
             {// Kind 14 is "Keyword", Kind 3 is "Function", Kind
              // 6 is "Variable"
              {{"label", "import"},
               {"kind", 14},
               {"detail", "Importar módulo"}},
              {{"label", "print"},
               {"kind", 3},
               {"detail", "Imprimir en consola"}},
              {{"label", "int"}, {"kind", 14}, {"detail", "Entero de 32 bits"}},
              {{"label", "float"}, {"kind", 14}, {"detail", "Punto flotante"}},
              {{"label", "bool"},
               {"kind", 14},
               {"detail", "Booleano (true/false)"}},
              {{"label", "String"},
               {"kind", 14},
               {"detail", "Cadena de texto"}},
              {{"label", "return"}, {"kind", 14}},
              {{"label", "new"},
               {"kind", 14},
               {"detail", "Asignar memoria en el heap"}},
              {{"label", "delete"},
               {"kind", 14},
               {"detail", "Liberar memoria del heap"}}}}};
        sendResponse(response);
        logMessage("Enviando sugerencias de autocompletado.");
      } else if (method == "shutdown") {
        json response = {
            {"jsonrpc", "2.0"}, {"id", request["id"]}, {"result", nullptr}};
        sendResponse(response);
      } else if (method == "exit") {
        break; // Cierra el proceso
      }
    } catch (const std::exception &e) {
    }
  }

  return 0;
}