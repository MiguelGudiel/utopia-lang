#include "utopia/Driver/ModuleLoader.hpp"
#include "utopia/Lexer/Lexer.hpp"
#include "utopia/Parser/Parser.hpp"
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace utopia {

std::expected<fs::path, std::string>
ModuleLoader::resolveImportURI(std::string_view uri,
                               const fs::path &currentDir) {
  std::string uriStr(uri);

  if (uriStr.starts_with("utopia:")) {
    std::string libName = uriStr.substr(7);
    fs::path target = config.stdlibRoot / libName;
    if (target.extension() != ".utp")
      target += ".utp";

    if (fs::exists(target)) {
      return fs::weakly_canonical(target);
    }
    return std::unexpected("Standard library module not found: " + libName);
  }

  if (uriStr.starts_with("package:")) {
    std::string pkgPath = uriStr.substr(8);
    fs::path target = config.projectRoot / "utopia_modules" / pkgPath;
    if (target.extension() != ".utp")
      target += ".utp";

    if (fs::exists(target)) {
      return fs::weakly_canonical(target);
    }
    return std::unexpected("Package module not found: " + pkgPath);
  }

  fs::path target(uriStr);
  if (target.extension() != ".utp")
    target += ".utp";

  fs::path candidate = fs::weakly_canonical(currentDir / target);
  if (fs::exists(candidate)) {
    return candidate;
  }

  if (fs::exists(target)) {
    return fs::weakly_canonical(target);
  }

  return std::unexpected("Local module not found: " + uriStr);
}

ModuleNode *ModuleLoader::loadModule(const std::string &importURI,
                                     const fs::path &currentFileDir) {
  auto resolvedPathResult = resolveImportURI(importURI, currentFileDir);
  if (!resolvedPathResult) {
    diags.report({DiagLevel::Error, 0, 0, 0, resolvedPathResult.error(), ""});
    return nullptr;
  }

  fs::path absPath = *resolvedPathResult;
  std::string key = absPath.string();

  if (auto it = moduleCache.find(key); it != moduleCache.end()) {
    return it->second;
  }

  std::ifstream file(absPath);
  if (!file) {
    diags.report(
        {DiagLevel::Error, 0, 0, 0, "Could not open file: " + key, ""});
    return nullptr;
  }

  std::stringstream buffer;
  buffer << file.rdbuf();

  auto [sourceIt, inserted] = sourceCache.insert({key, buffer.str()});
  std::string_view sourceView = sourceIt->second;

  // Persist the file path in the AST context allocator to guarantee it outlives
  // the parsing phase and safely feeds the zero-copy string_view in ModuleNode.
  std::string_view persistentFilePath = astCtx.copyString(key);

  std::cerr << "[ModuleLoader Debug] Persisted module path: "
            << persistentFilePath << "\n";

  Lexer lexer(sourceView);
  auto tokens = lexer.tokenize();

  Parser parser(astCtx, tokens, diags, persistentFilePath);
  ModuleNode *module = parser.parseModule(persistentFilePath);

  if (diags.hasErrors()) {
    return nullptr;
  }

  moduleCache[key] = module;
  std::vector<ModuleNode *> resolvedImports;

  for (std::string_view imp : module->rawImports) {
    ModuleNode *loaded = loadModule(std::string(imp), absPath.parent_path());
    if (loaded) {
      resolvedImports.push_back(loaded);
    }
  }

  module->importedModules = astCtx.copyArray<ModuleNode *>(resolvedImports);
  return module;
}

} // namespace utopia