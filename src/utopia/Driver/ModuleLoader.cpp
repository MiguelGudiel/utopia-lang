#include "utopia/Driver/ModuleLoader.hpp"
#include "utopia/Common/Logger.hpp"
#include "utopia/Lexer/Lexer.hpp"
#include "utopia/Parser/Parser.hpp"
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace utopia {

std::expected<std::filesystem::path, std::string>
ModuleLoader::resolveImportURI(std::string_view uri,
                               const std::filesystem::path &currentDir) {
  std::string uriStr(uri);

  if (uriStr == "utopia:builder") {
    if (!config.isBuildScript) {
      return std::unexpected("The 'utopia:builder' module can only be imported "
                             "from build.utp scripts.");
    }
    std::filesystem::path target = config.buildLibRoot / "builder.utp";
    if (std::filesystem::exists(target)) {
      return std::filesystem::weakly_canonical(target);
    }
    return std::unexpected("Builder library module not found at: " +
                           target.string());
  }

  if (uriStr == "prelude") {
    std::filesystem::path target = config.preludeRoot / "prelude.utp";
    if (std::filesystem::exists(target)) {
      return std::filesystem::weakly_canonical(target);
    }
    return std::unexpected("Prelude module not found at: " + target.string());
  }

  if (uriStr.starts_with("utopia:")) {
    std::string libName = uriStr.substr(7);
    std::filesystem::path target = config.stdlibRoot / libName;
    if (target.extension() != ".utp")
      target += ".utp";

    if (std::filesystem::exists(target)) {
      return std::filesystem::weakly_canonical(target);
    }
    return std::unexpected("Standard library module not found: " + libName);
  }

  if (uriStr.starts_with("package:")) {
    std::string pkgPath = uriStr.substr(8);
    size_t slashPos = pkgPath.find('/');

    std::string pkgName = pkgPath.substr(0, slashPos);
    std::string subPath = (slashPos != std::string::npos)
                              ? pkgPath.substr(slashPos + 1)
                              : pkgName;

    if (config.packages.contains(pkgName)) {
      std::filesystem::path target =
          std::filesystem::path(config.packages.at(pkgName)) / subPath;
      if (target.extension() != ".utp")
        target += ".utp";

      if (std::filesystem::exists(target)) {
        return std::filesystem::weakly_canonical(target);
      }
    }

    std::filesystem::path target =
        config.projectRoot / "utopia_modules" / pkgPath;
    if (target.extension() != ".utp")
      target += ".utp";

    if (std::filesystem::exists(target)) {
      return std::filesystem::weakly_canonical(target);
    }
    return std::unexpected("Package module not found: " + pkgPath);
  }

  std::filesystem::path target(uriStr);
  if (target.extension() != ".utp")
    target += ".utp";

  std::filesystem::path candidate =
      std::filesystem::weakly_canonical(currentDir / target);
  if (std::filesystem::exists(candidate)) {
    return candidate;
  }

  if (std::filesystem::exists(target)) {
    return std::filesystem::weakly_canonical(target);
  }

  return std::unexpected("Local module not found: " + uriStr);
}

ModuleNode *ModuleLoader::loadModule(const std::string &importURI,
                                     const fs::path &currentFileDir, int line,
                                     int col, int len,
                                     std::string_view sourceFile) {
  auto resolvedPathResult = resolveImportURI(importURI, currentFileDir);

  std::string diagFile = sourceFile.empty() ? "" : std::string(sourceFile);

  if (!resolvedPathResult) {
    diags.report({DiagLevel::Error, line, col, len, resolvedPathResult.error(),
                  diagFile});
    return nullptr;
  }

  fs::path absPath = *resolvedPathResult;
  std::string key = absPath.string();

  if (auto it = moduleCache.find(key); it != moduleCache.end()) {
    return it->second;
  }

  /* Prevent infinite recursion resulting from circular dependencies */
  if (sourceCache.contains(key)) {
    diags.report({DiagLevel::Error, line, col, len,
                  "Circular import dependency detected: " + key, diagFile});
    return nullptr;
  }

  std::ifstream file(absPath);
  if (!file) {
    diags.report({DiagLevel::Error, line, col, len,
                  "Could not open file: " + key, diagFile});
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
   * lookups. We skip injection if the module being compiled is part of
   * the prelude itself to avoid circular dependency.
   */
  std::string preludeDir =
      std::filesystem::absolute(config.preludeRoot).string();
  if (importURI != "prelude" && key.find(preludeDir) == std::string::npos) {
    ModuleNode *preludeMod = loadModule("prelude", currentFileDir);
    if (preludeMod) {
      resolvedImports.push_back(preludeMod);
    }
  }

  Lexer lexer(sourceView, config.definedMacros);
  auto tokensVec = lexer.tokenize();

  auto tokens = astCtx.copyArray<Token>(tokensVec);

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