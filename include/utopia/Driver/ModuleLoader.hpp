#pragma once
#include "utopia/AST/AST.hpp"
#include "utopia/AST/ASTContext.hpp"
#include "utopia/Common/Diagnostics.hpp"
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>

namespace utopia {

struct ModuleLoaderConfig {
  std::filesystem::path projectRoot;
  std::filesystem::path stdlibRoot;
};

class ModuleLoader {
public:
  explicit ModuleLoader(ASTContext &context, const ModuleLoaderConfig &cfg,
                        DiagnosticsEngine &de)
      : astCtx(context), config(cfg), diags(de) {}

  /**
   * Resolves, parses, and recursively loads a module and its dependencies.
   * Returns a cached AST root pointer. Returns nullptr if a compilation error
   * occurs.
   */
  ModuleNode *loadModule(const std::string &importURI,
                         const std::filesystem::path &currentFileDir = "");

private:
  ASTContext &astCtx;
  ModuleLoaderConfig config;
  DiagnosticsEngine &diags;

  std::unordered_map<std::string, std::string> sourceCache;
  std::unordered_map<std::string, ModuleNode *> moduleCache;

  std::expected<std::filesystem::path, std::string>
  resolveImportURI(std::string_view uri,
                   const std::filesystem::path &currentDir);
};

} // namespace utopia