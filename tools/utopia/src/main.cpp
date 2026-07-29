#include "BuildScriptRunner.hpp"
#include "ProjectManager.hpp"
#include "utopia/Driver/CompilerDriver.hpp"
#include "utopia/Common/Logger.hpp"
#include <iostream>
#include <llvm/ExecutionEngine/Orc/ExecutionUtils.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/TargetSelect.h>
#include <optional>
#include <string>
#include <vector>

using namespace utopia;

void utopiaFatalErrorHandler(void *user_data, const char *reason,
                             bool gen_crash_diag) {
  std::cerr << "\033[1;31m[LLVM Fatal Error]\033[0m " << reason << std::endl;
  exit(1);
}

int main(int argc, char **argv) {
  llvm::install_fatal_error_handler(utopiaFatalErrorHandler, nullptr);

  try {
    std::vector<std::string> args(argv + 1, argv + argc);
    CompileOptions options;
    std::optional<int> cliOptLevel;
    fs::path startPath = fs::current_path();

    for (auto it = args.begin(); it != args.end(); ++it) {
      std::string_view arg = *it;
      if (arg == "--emit-llvm") {
        options.emitLLVM = true;
      } else if (arg == "--emit-asm") {
        options.emitAsm = true;
      } else if (arg == "--jit") {
        options.isJIT = true;
      } else if (arg.starts_with("-O")) {
        if (arg.length() > 2 && std::isdigit(arg[2])) {
          cliOptLevel = std::stoi(std::string(arg.substr(2)));
        }
      } else if (!arg.empty() && arg[0] != '-') {
        startPath = fs::absolute(std::string(arg));
      }
    }

    fs::path projRoot = findProjectRoot(startPath);
    if (projRoot.empty()) {
      std::cerr << "Fatal: build.yaml not found in path chain.\n";
      return 1;
    }

    ProjectConfig config;
    try {
      config = parseBuildManifest(projRoot / "build.yaml");
    } catch (const std::exception &e) {
      std::cerr << "Manifest Error: " << e.what() << "\n";
      return 1;
    }

    options.projectName = config.name;
    options.projectRoot = projRoot.string();
    options.outputDir = (projRoot / config.outputDir).string();

    /* Resolution of stdlib and prelude paths */
#ifdef UTOPIA_SOURCE_DIR
    fs::path stdlibPath =
        fs::path(UTOPIA_SOURCE_DIR) / "libs" / "stdlib" / "lib";
    fs::path preludePath =
        fs::path(UTOPIA_SOURCE_DIR) / "libs" / "prelude" / "lib";
    fs::path buildLibPath =
        fs::path(UTOPIA_SOURCE_DIR) / "libs" / "build" / "lib";
#else
    fs::path stdlibPath =
        projRoot.parent_path().parent_path() / "libs" / "stdlib" / "lib";
    fs::path preludePath =
        projRoot.parent_path().parent_path() / "libs" / "prelude" / "lib";
    fs::path buildLibPath =
        projRoot.parent_path().parent_path() / "libs" / "build" / "lib";
#endif

    options.stdlibRoot = stdlibPath.string();
    options.preludeRoot = preludePath.string();
    options.buildLibRoot = buildLibPath.string();

    options.linkerFlags = config.linkerFlags;
    options.includeDirs = config.includeDirs;
    options.optLevel = cliOptLevel.value_or(config.optLevel);

    fs::path buildUtpPath = projRoot / "build.utp";
    if (fs::exists(buildUtpPath)) {
      Logger::debug("[Utopia] Executing project build script (build.utp)...");
      if (!BuildScriptRunner::run(buildUtpPath, options, projRoot)) {
        std::cerr << "Fatal: Failed to execute build.utp successfully.\n";
        return 1;
      }
    }

    if (config.resolvedSources.empty()) {
      std::cerr << "Fatal: No sources found in build.yaml\n";
      return 1;
    }

    options.sourcePath = config.resolvedSources.front().path;
    options.outputPath = (fs::path(options.outputDir) / config.name).string();

    CompilerDriver driver(options);
    if (!driver.run()) {
      return 1;
    }
  } catch (const std::exception &e) {
    std::cerr << "\033[1;31m[Utopia Runtime Error]\033[0m " << e.what()
              << std::endl;
    return 1;
  } catch (...) {
    std::cerr << "[Unknown Fatal Error] Utopia se cerró inesperadamente."
              << std::endl;
    return 1;
  }

  llvm::remove_fatal_error_handler();
  return 0;
}