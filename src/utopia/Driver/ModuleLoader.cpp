#include "utopia/Driver/ModuleLoader.hpp"
#include "utopia/Common/Logger.hpp"
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

  if (uriStr == "utopia:build") {
    if (!config.isBuildScript) {
      return std::unexpected("The 'utopia:build' module can only be imported "
                             "from build.utp scripts.");
    }
    fs::path target = config.buildLibRoot / "build.utp";
    if (fs::exists(target)) {
      return fs::weakly_canonical(target);
    }
    return std::unexpected("Build library module not found at: " +
                           target.string());
  }

  if (uriStr == "prelude") {
    fs::path target = config.preludeRoot / "prelude.utp";
    if (fs::exists(target)) {
      return fs::weakly_canonical(target);
    }
    return std::unexpected("Prelude module not found at: " + target.string());
  }

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

  /* Prevent infinite recursion resulting from circular dependencies */
  if (sourceCache.contains(key)) {
    diags.report({DiagLevel::Error, 0, 0, 0,
                  "Circular import dependency detected: " + key, ""});
    return nullptr;
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

  std::string_view persistentFilePath = astCtx.copyString(key);

  Logger::debug("[ModuleLoader Debug] Persisted module path: " +
                std::string(persistentFilePath));

  std::vector<ModuleNode *> resolvedImports;

  /*
   * Initialize prelude strictly prior to parsing the target AST block.
   * Pre-populates primitive core abstractions to satisfy early semantic
   * lookups.
   */
  if (importURI != "prelude" && key.find("prelude.utp") == std::string::npos) {
    ModuleNode *preludeMod = loadModule("prelude", currentFileDir);
    if (preludeMod) {
      resolvedImports.push_back(preludeMod);
    }
  }

  Lexer lexer(sourceView);
  auto tokens = lexer.tokenize();

  Parser parser(astCtx, tokens, diags, persistentFilePath, this);
  ModuleNode *module = parser.parseModule(persistentFilePath);

  if (diags.hasErrors()) {
    return nullptr;
  }

  moduleCache[key] = module;

  for (std::string_view imp : module->rawImports) {
    /* Cache hits are guaranteed here, fulfilling local module linkages. */
    ModuleNode *loaded = loadModule(std::string(imp), absPath.parent_path());
    if (loaded) {
      if (std::find(resolvedImports.begin(), resolvedImports.end(), loaded) ==
          resolvedImports.end()) {
        resolvedImports.push_back(loaded);
      }
    }
  }

  module->importedModules = astCtx.copyArray<ModuleNode *>(resolvedImports);
  return module;
}

} // namespace utopia