#pragma once
#include "utopia/AST/AST.hpp"
#include "utopia/CodeGen/CodeGen.hpp"
#include "utopia/Common/Types.hpp"
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
};

} // namespace utopia