#pragma once
#include "utopia/AST/AST.hpp"
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace utopia {

struct CompileOptions {
  std::string sourcePath;
  std::string outputPath;
  std::string projectRoot;
  std::vector<std::string> includeDirs;
  std::vector<std::string> linkerFlags;
  bool emitLLVM = false;
  bool emitAsm = false;

  int optLevel = 0;
  bool isDebug = false;
};

class CompilerDriver {
public:
  explicit CompilerDriver(const CompileOptions &options);
  bool run();

private:
  CompileOptions options;
  std::string readFile(const std::string &path);

  // Resolve the destination object path for a given module,
  // handling standard library exiles and project-relative nesting.
  fs::path getObjPath(const ModuleNode *mod, const fs::path &internalPath,
                      const fs::path &projRoot, const fs::path &objDir);
};

} // namespace utopia