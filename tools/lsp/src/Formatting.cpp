#include "LspCore.hpp"
#include <algorithm>

namespace utopia::lsp {

void handleFormatting(const json &req) {
  syncWorker();
  std::string uri = req["params"]["textDocument"]["uri"];
  json res = nullptr;

  DocumentState doc;
  if (!documents.get(uri, doc) || doc.text.empty())
    return sendResponse(
        {{"jsonrpc", "2.0"}, {"id", requestId(req)}, {"result", res}});

  std::string filePath = uriToPath(uri);
  std::filesystem::path currentPath(filePath);

  ModuleLoaderConfig modConfig = documents.configFor(uri, currentPath);
  if (currentPath.filename() == "build.utp") {
    modConfig.isBuildScript = true;
  }
  modConfig.isFormatting = true;

  ASTContext formatAstCtx;
  DiagnosticsEngine formatDiags;
  formatDiags.printToConsole = false;

  ModuleLoader loader(formatAstCtx, modConfig, formatDiags);

  try {
    ModuleNode *formatAst = loader.loadModule(
        filePath, currentPath.parent_path(), 0, 0, 0, filePath, doc.text);
    if (formatAst) {
      std::string formatted = Formatter::format(formatAst);
      if (!formatted.empty()) {
        int lineCount =
            std::count(doc.text.begin(), doc.text.end(), '\n') + 1;
        res = json::array();
        res.push_back({{"range",
                        {{"start", {{"line", 0}, {"character", 0}}},
                         {"end", {{"line", lineCount}, {"character", 0}}}}},
                       {"newText", formatted}});
      }
    }
  } catch (...) {
    /* Formatting is best-effort; a malformed document keeps its text. */
  }

  sendResponse({{"jsonrpc", "2.0"}, {"id", requestId(req)}, {"result", res}});
}

} // namespace utopia::lsp
