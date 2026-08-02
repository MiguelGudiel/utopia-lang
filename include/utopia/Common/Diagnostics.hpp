#pragma once
#include <algorithm>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace utopia {

enum class DiagLevel { Error, Warning, Note, Inactive };

struct Diagnostic {
  DiagLevel level;
  int line;
  int column;
  int length;
  std::string message;
  std::string filePath;
  int endLine = 0;
};

class DiagnosticsEngine {
public:
  bool printToConsole = true;

  void report(const Diagnostic &diag) {
    if (diag.level == DiagLevel::Error) {
      errorCount++;
    }
    diagnostics.push_back(diag);
    if (printToConsole) {
      renderToConsole(diag);
    }
  }

  bool hasErrors() const { return errorCount > 0; }
  const std::vector<Diagnostic> &getDiagnostics() const { return diagnostics; }

  // Generates a JSON array compatible with LSP publishDiagnostics
  nlohmann::json toJSON() const {
    auto j = nlohmann::json::array();
    for (const auto &d : diagnostics) {
      int lspLine = d.line > 0 ? d.line - 1 : 0;
      int lspCol = d.column > 0 ? d.column - 1 : 0;
      int lspEndLine = (d.endLine > 0) ? d.endLine - 1 : lspLine;

      int lspEndCol;
      if (lspEndLine > lspLine) {
        lspEndCol = std::max(0, lspCol + d.length);
      } else {
        // Prevent negative lengths from multi-line spanning nodes to avoid VS
        // Code rejecting the payload
        int safeLen = std::max(1, d.length);
        lspEndCol = lspCol + safeLen;
      }

      auto diagObj = nlohmann::json{
          {"range",
           {{"start", {{"line", lspLine}, {"character", lspCol}}},
            {"end", {{"line", lspEndLine}, {"character", lspEndCol}}}}},
          {"message", d.message},
          {"source", "utopia"},
          {"file", d.filePath}};

      if (d.level == DiagLevel::Error) {
        diagObj["severity"] = 1;
      } else if (d.level == DiagLevel::Warning) {
        diagObj["severity"] = 2;
      } else if (d.level == DiagLevel::Note) {
        diagObj["severity"] = 3;
      } else if (d.level == DiagLevel::Inactive) {
        diagObj["severity"] = 4; // Hint level in LSP
        diagObj["tags"] = {
            1}; // 1 = DiagnosticTag::Unnecessary (Greys out code in IDE)
      }

      j.push_back(diagObj);
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
    case DiagLevel::Inactive:
      color = "\033[1;30m"; // Dark grey
      label = "inactive";
      break;
    }

    std::cerr << "\033[1m" << diag.filePath << ":" << diag.line << ":"
              << diag.column << ": " << color << label << ":\033[0m\033[1m "
              << diag.message << "\033[0m\n";
  }
};

} // namespace utopia