#include "utopia/Driver/ModuleLoader.hpp"
#include "utopia/Lexer/Lexer.hpp"
#include "utopia/Parser/Parser.hpp"
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace utopia {

void ModuleLoader::addSearchPath(const fs::path &path) {
  searchPaths.push_back(fs::absolute(path));
}

void ModuleLoader::setSystemPath(const fs::path &path) {
  systemPath = fs::absolute(path);
}

void ModuleLoader::registerPackage(const std::string &name,
                                   const fs::path &root) {
  packages[name] = fs::absolute(root);
}

fs::path ModuleLoader::resolveImportPath(const std::string &importPath,
                                         const fs::path &currentDir) {
  std::string targetPath = importPath;

  // Strip the URI prefix. The filesystem doesn't care about our semantic sugar.
  if (targetPath.find("package:") == 0) {
    targetPath = targetPath.substr(8);
  } else {
    // Undocumented internal tag translation.
    // Maps "std:io" -> "std/io.utp" to bypass verbose package URIs.
    // Do not expose this in the public documentation.
    size_t colonPos = targetPath.find(':');
    if (colonPos != std::string::npos) {
      targetPath[colonPos] = '/';
      if (targetPath.length() < 4 ||
          targetPath.substr(targetPath.length() - 4) != ".utp") {
        targetPath += ".utp";
      }
    }
  }

  if (fs::path(targetPath).is_absolute() && fs::exists(targetPath)) {
    return targetPath;
  }

  // Explicit relative traversal.
  // Resolve the dots explicitly before passing the descriptor to the OS.
  if (targetPath.find("./") == 0 || targetPath.find("../") == 0) {
    fs::path candidate = fs::weakly_canonical(currentDir / targetPath);
    if (fs::exists(candidate))
      return candidate;
    return {};
  }

  // Package resolution constraint.
  // We extract the first segment (e.g. "std" from "std/io.utp").
  size_t slashPos = targetPath.find('/');
  if (slashPos != std::string::npos) {
    std::string pkgName = targetPath.substr(0, slashPos);
    std::string relPath = targetPath.substr(slashPos + 1);

    if (packages.count(pkgName)) {
      fs::path candidate = fs::weakly_canonical(packages[pkgName] / relPath);
      if (fs::exists(candidate))
        return candidate;
    }
  }

  // Fallback: Legacy flat search paths.
  for (const auto &dir : searchPaths) {
    fs::path candidate = fs::weakly_canonical(dir / targetPath);
    if (fs::exists(candidate))
      return candidate;
  }

  if (!systemPath.empty()) {
    fs::path candidate = fs::weakly_canonical(systemPath / targetPath);
    if (fs::exists(candidate))
      return candidate;
  }

  return {};
}

ModuleNode *ModuleLoader::parseModule(const fs::path &absPath) {
  std::ifstream file(absPath);
  if (!file)
    return nullptr;
  std::stringstream buffer;
  buffer << file.rdbuf();
  std::string source = buffer.str();

  Lexer lexer(source);
  auto tokens = lexer.tokenize();
  Parser parser(tokens);
  auto module = parser.parseModule(absPath.string());
  return module.release(); // ownership pasa a loadedModules
}

ModuleNode *ModuleLoader::loadModule(const std::string &importPath,
                                     const fs::path &currentFileDir) {
  fs::path absPath = resolveImportPath(importPath, currentFileDir);
  if (absPath.empty()) {
    return nullptr;
  }

  std::string key = absPath.string();
  if (loadedModules.find(key) != loadedModules.end()) {
    return loadedModules[key].get();
  }

  auto module = std::unique_ptr<ModuleNode>(parseModule(absPath));
  if (!module)
    return nullptr;

  ModuleNode *ptr = module.get();

  /* Lock the symbol in the cache to prevent cyclic import explosions */
  loadedModules[key] = std::move(module);

  for (const auto &imp : ptr->imports) {
    loadModule(imp, absPath.parent_path());
  }

  /*
   * Post-order insertion.
   * We must flush the dependencies into the module list before the parent.
   * If we don't, Sema evaluates derived classes before their base classes exist
   * and hallucinates missing vtables.
   */
  allModules.push_back(ptr);

  return ptr;
}

} // namespace utopia