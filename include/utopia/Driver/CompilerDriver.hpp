#pragma once
#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;

namespace utopia {

struct CompileOptions {
  std::string projectName;
  std::string outputName;
  std::string target;
  std::string sourcePath;
  std::string outputPath;
  std::string projectRoot;
  std::string mainProjectRoot;
  std::string currentProjectRoot;
  std::string outputDir;
  std::string mainOutputDir;
  std::string stdlibRoot;
  std::string preludeRoot;
  std::string buildLibRoot;
  std::string targetTriple;
  std::string sysroot;
  std::vector<std::string> includeDirs;
  std::vector<std::string> linkerFlags;

  std::unordered_set<std::string> publicMacros;
  std::unordered_set<std::string> privateMacros;

  std::unordered_map<std::string, std::string> packages;

  bool emitLLVM = false;
  bool emitAsm = false;
  bool isJIT = false;
  bool doFormat = false;

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