#pragma once
#include <optional>
#include <string>
#include <vector>

namespace utopia {

struct GlobalOptions {
  bool emitLLVM = false;
  bool emitAsm = false;
  bool isJIT = false;
  bool isDebug = false;
  bool doFormat = false;
  std::optional<int> cliOptLevel;
  std::string targetTriple;
  std::string sysroot;
  std::string stdlibRoot;
  std::string preludeRoot;
  std::string buildLibRoot;
  std::vector<std::string> cliMacros;
};

} // namespace utopia