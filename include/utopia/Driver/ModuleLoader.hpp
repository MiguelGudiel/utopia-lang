#pragma once
#include "utopia/AST/AST.hpp"
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace utopia {

class ModuleLoader {
public:
  void addSearchPath(const std::filesystem::path &path);
  void setSystemPath(const std::filesystem::path &path);

  // Register a package alias pointing to its root directory
  void registerPackage(const std::string &name,
                       const std::filesystem::path &root);

  ModuleNode *loadModule(const std::string &importPath,
                         const std::filesystem::path &currentFileDir);

  ModuleNode *getRootModule() const { return rootModule; }
  void setRootModule(ModuleNode *module) { rootModule = module; }

  const std::vector<ModuleNode *> &getAllModules() const { return allModules; }

private:
  std::vector<std::filesystem::path> searchPaths;
  std::filesystem::path systemPath;
  std::map<std::string, std::filesystem::path> packages; // name -> root path
  std::map<std::string, std::unique_ptr<ModuleNode>> loadedModules;
  ModuleNode *rootModule = nullptr;
  std::vector<ModuleNode *> allModules;

  std::filesystem::path
  resolveImportPath(const std::string &importPath,
                    const std::filesystem::path &currentDir);
  ModuleNode *parseModule(const std::filesystem::path &absPath);
};

} // namespace utopia