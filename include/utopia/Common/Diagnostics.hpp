#pragma once
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace utopia {

enum class DiagLevel { Error, Warning, Note };

struct Diagnostic {
  DiagLevel level;
  int line;
  int column;
  int length;
  std::string message;
  std::string filePath;
};

class DiagnosticsEngine {
public:
  void report(const Diagnostic &diag) {
    diagnostics.push_back(diag);
    renderToConsole(diag);
  }

  bool hasErrors() const { return errorCount > 0; }
  const std::vector<Diagnostic> &getDiagnostics() const { return diagnostics; }

  // Generates a JSON array compatible with LSP publishDiagnostics
  nlohmann::json toJSON() const {
    auto j = nlohmann::json::array();
    for (const auto &d : diagnostics) {
      int lspLine = d.line > 0 ? d.line - 1 : 0;
      int lspCol = d.column > 0 ? d.column - 1 : 0;

      j.push_back(
          {{"range",
            {{"start", {{"line", lspLine}, {"character", lspCol}}},
             {"end", {{"line", lspLine}, {"character", lspCol + d.length}}}}},
           {"severity", d.level == DiagLevel::Error ? 1 : 2},
           {"message", d.message},
           {"source", "utopia"}});
    }
    return j;
  }

  void clear() {
    diagnostics.clear();
    errorCount = 0;
  }

private:
  std::vector<Diagnostic> diagnostics;
  int errorCount = 0;

  void renderToConsole(const Diagnostic &diag) {
    if (diag.level == DiagLevel::Error)
      errorCount++;

    const char *color = "";
    const char *label = "";

    switch (diag.level) {
    case DiagLevel::Error:
      color = "\033[1;31m";
      label = "error";
      break;
    case DiagLevel::Warning:
      color = "\033[1;33m";
      label = "warning";
      break;
    case DiagLevel::Note:
      color = "\033[1;36m";
      label = "note";
      break;
    }

    std::cerr << "\033[1m" << diag.filePath << ":" << diag.line << ":"
              << diag.column << ": " << color << label << ":\033[0m\033[1m "
              << diag.message << "\033[0m\n";
  }
};

} // namespace utopia