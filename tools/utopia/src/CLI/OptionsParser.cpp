#include "CLI/OptionsParser.hpp"
#include <cctype>

namespace utopia {

void OptionsParser::parseCommonOptions(const std::vector<std::string> &args,
                                       GlobalOptions &opts,
                                       std::filesystem::path &startPath) {
  for (const auto &arg : args) {
    if (arg == "--emit-llvm") {
      opts.emitLLVM = true;
    } else if (arg == "--emit-asm") {
      opts.emitAsm = true;
    } else if (arg == "--jit") {
      opts.isJIT = true;
    } else if (arg == "--format") {
      opts.doFormat = true;
    } else if (arg == "-g" || arg == "--debug") {
      opts.isDebug = true;
    } else if (arg.starts_with("-O")) {
      if (arg.length() > 2 && std::isdigit(arg[2])) {
        opts.cliOptLevel = std::stoi(arg.substr(2));
      }
    } else if (arg.starts_with("-D")) {
      opts.cliMacros.push_back(arg.substr(2));
    } else if (!arg.empty() && arg[0] != '-') {
      startPath = std::filesystem::absolute(arg);
    }
  }
}

void OptionsParser::resolveStandardPaths(const std::string &exePathStr,
                                         const std::filesystem::path &projRoot,
                                         GlobalOptions &opts) {
#if defined(UTOPIA_RELEASE_BUILD)
  std::filesystem::path exePath(exePathStr);
  std::filesystem::path installRoot = exePath.parent_path().parent_path();

  opts.stdlibRoot =
      (installRoot / "lib" / "utopia" / "stdlib" / "lib").string();
  opts.preludeRoot =
      (installRoot / "lib" / "utopia" / "prelude" / "lib").string();
  opts.buildLibRoot =
      (installRoot / "lib" / "utopia" / "builder" / "lib").string();
#elif defined(UTOPIA_SOURCE_DIR)
  opts.stdlibRoot =
      (std::filesystem::path(UTOPIA_SOURCE_DIR) / "libs" / "stdlib" / "lib")
          .string();
  opts.preludeRoot =
      (std::filesystem::path(UTOPIA_SOURCE_DIR) / "libs" / "prelude" / "lib")
          .string();
  opts.buildLibRoot =
      (std::filesystem::path(UTOPIA_SOURCE_DIR) / "libs" / "builder" / "lib")
          .string();
#else
  opts.stdlibRoot =
      (projRoot.parent_path().parent_path() / "libs" / "stdlib" / "lib")
          .string();
  opts.preludeRoot =
      (projRoot.parent_path().parent_path() / "libs" / "prelude" / "lib")
          .string();
  opts.buildLibRoot =
      (projRoot.parent_path().parent_path() / "libs" / "builder" / "lib")
          .string();
#endif
}

} // namespace utopia