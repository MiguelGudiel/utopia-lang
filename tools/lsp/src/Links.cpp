#include "LspCore.hpp"

namespace utopia::lsp {

namespace {

/* Links for the import/export statements of one module. */
void collectLinks(const ModuleNode *mod, const DocumentState &doc,
                  const ModuleLoaderConfig &config, json &out) {
  if (!mod)
    return;

  const std::string &text = doc.text;
  auto findStatement = [&](std::string_view pathStr) -> int {
    /* Find the 'import'/'export' line whose quoted path matches. */
    std::string needle = "\"" + std::string(pathStr) + "\"";
    size_t pos = text.find(needle);
    if (pos == std::string::npos)
      return -1;
    int line = 1;
    for (size_t i = 0; i < pos; ++i) {
      if (text[i] == '\n')
        line++;
    }
    return line;
  };

  auto addLink = [&](std::string_view pathStr) {
    int line = findStatement(pathStr);
    if (line < 0)
      return;

    /* Resolve the import the same way the compiler does, then verify
     * against the loaded modules (the loader prepends the prelude, so
     * index-based matching is unreliable). */
    std::string resolvedPath = resolveModuleUriLsp(
        config, std::string(pathStr),
        std::filesystem::path(mod->filePath).parent_path());
    if (resolvedPath.empty())
      return;

    std::string targetPath = resolvedPath;
    const llvm::ArrayRef<ModuleNode *> importLists[] = {doc.ast->importedModules,
                                                        doc.ast->exportedModules};
    for (const auto &list : importLists) {
      for (const auto *m : list) {
        if (m && !m->filePath.empty() &&
            std::filesystem::weakly_canonical(m->filePath) ==
                std::filesystem::weakly_canonical(resolvedPath)) {
          targetPath = std::string(m->filePath);
          break;
        }
      }
      if (!targetPath.empty())
        break;
    }
    if (targetPath.empty())
      return;

    size_t quotePos = text.find('"', text.find(std::string(pathStr)));
    if (quotePos == std::string::npos)
      return;
    int startCol = 0;
    size_t lineStart = 0;
    int currentLine = 1;
    for (size_t i = 0; i < text.length(); ++i) {
      if (currentLine == line) {
        lineStart = i;
        break;
      }
      if (text[i] == '\n')
        currentLine++;
    }
    startCol = (int)(quotePos - lineStart);

    out.push_back({{"range",
                    {{"start", {{"line", line - 1}, {"character", startCol}}},
                     {"end", {{"line", line - 1},
                              {"character", startCol +
                                            (int)pathStr.length() + 2}}}}},
                   {"target", pathToUri(targetPath)}});
  };

  for (std::string_view path : mod->rawImports) {
    addLink(path);
  }
  for (std::string_view path : mod->rawExports) {
    addLink(path);
  }
}

} // namespace

void handleDocumentLinks(const json &req) {
  syncWorker();
  std::string uri = req["params"]["textDocument"]["uri"];

  json res = json::array();

  DocumentState doc;
  if (documents.get(uri, doc) && doc.ast) {
    std::string filePath = uriToPath(uri);
    ModuleLoaderConfig config = documents.configFor(uri, filePath);
    collectLinks(doc.ast, doc, config, res);
  }

  sendResponse({{"jsonrpc", "2.0"}, {"id", requestId(req)}, {"result", res}});
}

} // namespace utopia::lsp
