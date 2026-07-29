#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace utopia {

struct CompileOptions {
  std::string projectName;
  std::string sourcePath;
  std::string outputPath;
  std::string projectRoot;
  std::string outputDir;
  std::string stdlibRoot;
  std::string preludeRoot;
  std::string buildLibRoot;
  std::vector<std::string> includeDirs;
  std::vector<std::string> linkerFlags;

  bool emitLLVM = false;
  bool emitAsm = false;
  bool isJIT = false;

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

  /* Resolved destination object path based on project taxonomy and origin */
  fs::path getObjPath(const std::string &filename, const fs::path &internalPath,
                      const fs::path &projRoot, const fs::path &objDir);
};

} // namespace utopia