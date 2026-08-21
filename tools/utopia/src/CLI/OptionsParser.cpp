#include "CLI/OptionsParser.hpp"
#include <cctype>
#include <iostream>

namespace utopia {

bool OptionsParser::parseCommonOptions(const std::vector<std::string> &args,
                                       GlobalOptions &opts,
                                       std::filesystem::path &startPath) {
  bool ok = true;
  for (size_t i = 0; i < args.size(); ++i) {
    const auto &arg = args[i];
    if (arg == "--emit-llvm") {
      opts.emitLLVM = true;
    } else if (arg == "--emit-asm") {
      opts.emitAsm = true;
    } else if (arg == "--jit") {
      opts.isJIT = true;
    } else if (arg == "--format") {
      opts.doFormat = true;
    } else if (arg == "--no-async") {
      opts.asyncEnabled = false;
    } else if (arg == "-g" || arg == "--debug") {
      opts.isDebug = true;
    } else if (arg == "--target" || arg == "--mcpu" || arg == "--mattr" ||
               arg == "--sysroot") {
      if (i + 1 >= args.size()) {
        std::cerr << "[Error] Option '" << arg << "' requires a value.\n";
        ok = false;
        continue;
      }
      if (arg == "--target") {
        opts.targetTriple = args[++i];
      } else if (arg == "--mcpu") {
        opts.targetCpu = args[++i];
      } else if (arg == "--mattr") {
        opts.targetFeatures = args[++i];
      } else {
        opts.sysroot = args[++i];
      }
    } else if (arg.starts_with("--target=")) {
      opts.targetTriple = arg.substr(9);
    } else if (arg.starts_with("--mcpu=")) {
      opts.targetCpu = arg.substr(7);
    } else if (arg.starts_with("--mattr=")) {
      opts.targetFeatures = arg.substr(8);
    } else if (arg.starts_with("--sysroot=")) {
      opts.sysroot = arg.substr(10);
    } else if (arg.starts_with("-O")) {
      if (arg.length() > 2 && std::isdigit(static_cast<unsigned char>(arg[2]))) {
        opts.cliOptLevel = std::stoi(arg.substr(2));
      } else {
        std::cerr << "[Error] Invalid optimization level: '" << arg
                  << "'. Use -O0 through -O3.\n";
        ok = false;
      }
    } else if (arg.starts_with("-D")) {
      if (arg.length() > 2) {
        opts.cliMacros.push_back(arg.substr(2));
      } else {
        std::cerr << "[Error] Option '-D' requires a macro name.\n";
        ok = false;
      }
    } else if (!arg.empty() && arg[0] != '-') {
      startPath = std::filesystem::absolute(arg);
    } else {
      std::cerr << "[Error] Unknown option: " << arg << "\n";
      ok = false;
    }
  }
  return ok;
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