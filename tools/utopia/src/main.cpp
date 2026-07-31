#include "BuildScriptRunner.hpp"
#include "ProjectManager.hpp"
#include "utopia/Common/Logger.hpp"
#include "utopia/Driver/CompilerDriver.hpp"
#include <iostream>
#include <llvm/ExecutionEngine/Orc/ExecutionUtils.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/TargetSelect.h>
#include <optional>
#include <string>
#include <vector>

using namespace utopia;

struct GlobalOptions {
  bool emitLLVM = false;
  bool emitAsm = false;
  bool isJIT = false;
  bool isDebug = false;
  std::optional<int> cliOptLevel;
  std::string stdlibRoot;
  std::string preludeRoot;
  std::string buildLibRoot;
};

void utopiaFatalErrorHandler(void *user_data, const char *reason,
                             bool gen_crash_diag) {
  std::cerr << "\033[1;31m[LLVM Fatal Error]\033[0m " << reason << std::endl;
  exit(1);
}

bool buildProject(const fs::path &projRoot, CompileOptions &parentOptions,
                  bool isSubproject, const std::string &linkType,
                  const GlobalOptions &globalOpts) {
  ProjectConfig config;
  try {
    config = parseBuildManifest(projRoot / "build.yaml");
  } catch (const std::exception &e) {
    std::cerr << "Manifest Error in " << projRoot << ": " << e.what() << "\n";
    return false;
  }

  CompileOptions options;
  options.projectName = config.name;

  if (isSubproject) {
    options.target =
        (linkType == "shared") ? "shared_library" : "static_library";
    options.outputDir = parentOptions.outputDir;

    /* Inherit parent configurations */
    options.optLevel = parentOptions.optLevel;
    options.includeDirs = parentOptions.includeDirs;
    options.linkerFlags = parentOptions.linkerFlags;
  } else {
    options.target = config.target;
    options.outputDir = (projRoot / config.outputDir).string();
  }

  options.projectRoot = projRoot.string();
  options.stdlibRoot = globalOpts.stdlibRoot;
  options.preludeRoot = globalOpts.preludeRoot;
  options.buildLibRoot = globalOpts.buildLibRoot;

  options.emitLLVM = globalOpts.emitLLVM;
  options.emitAsm = globalOpts.emitAsm;
  options.isJIT = globalOpts.isJIT;
  options.isDebug = globalOpts.isDebug;

  /* Override inherited configurations with explicit YAML values */
  if (config.optLevel.has_value()) {
    options.optLevel = config.optLevel.value();
  }

  if (globalOpts.cliOptLevel.has_value()) {
    options.optLevel = globalOpts.cliOptLevel.value();
  }

  for (const auto &flag : config.linkerFlags) {
    options.linkerFlags.push_back(flag);
  }
  for (const auto &inc : config.includeDirs) {
    options.includeDirs.push_back(inc);
  }
  options.includeDirs.push_back(projRoot.string());

  for (const auto &pkg : parentOptions.packages) {
    options.packages[pkg.first] = pkg.second;
  }

  std::string selfPkgRoot = config.includeDirs.empty()
                                ? projRoot.string()
                                : config.includeDirs.front();
  options.packages[config.name] = selfPkgRoot;

  for (const auto &dep : config.dependencies) {
    fs::path depPath = projRoot / dep.path;
    if (!buildProject(depPath, options, true, dep.linkType, globalOpts)) {
      return false;
    }
  }

  fs::path buildUtpPath = projRoot / "build.utp";
  if (fs::exists(buildUtpPath)) {
    Logger::debug("[Utopia] Executing project build script (build.utp)...");
    if (!BuildScriptRunner::run(buildUtpPath, options, projRoot)) {
      std::cerr << "Fatal: Failed to execute build.utp successfully.\n";
      return false;
    }
  }

  if (config.resolvedSources.empty()) {
    std::cerr << "Fatal: No sources found in build.yaml at " << projRoot
              << "\n";
    return false;
  }

  options.sourcePath = config.resolvedSources.front().path;
  options.outputPath = (fs::path(options.outputDir) / config.name).string();

  CompilerDriver driver(options);
  if (!driver.run()) {
    return false;
  }

  if (isSubproject) {
    fs::path outDir(options.outputDir);
    if (options.target == "static_library") {
      std::string ext = ".a";
#if defined(_WIN32)
      ext = ".lib";
#endif
      parentOptions.linkerFlags.push_back(
          (outDir / "lib" / ("lib" + options.projectName + ext)).string());
    } else if (options.target == "shared_library") {
      std::string ext = ".so";
#if defined(_WIN32)
      ext = ".dll";
#elif defined(__APPLE__)
      ext = ".dylib";
#endif
      parentOptions.linkerFlags.push_back(
          (outDir / "bin" / ("lib" + options.projectName + ext)).string());

#ifndef _WIN32
#ifdef __APPLE__
      parentOptions.linkerFlags.push_back("-Wl,-rpath,@executable_path");
#else
      parentOptions.linkerFlags.push_back("-Wl,-rpath,'$ORIGIN'");
#endif
#endif
    }

    /* Propagate unique includes and linker flags back to the parent */
    for (const auto &inc : options.includeDirs) {
      if (std::find(parentOptions.includeDirs.begin(),
                    parentOptions.includeDirs.end(),
                    inc) == parentOptions.includeDirs.end()) {
        parentOptions.includeDirs.push_back(inc);
      }
    }

    for (const auto &flag : options.linkerFlags) {
      if (std::find(parentOptions.linkerFlags.begin(),
                    parentOptions.linkerFlags.end(),
                    flag) == parentOptions.linkerFlags.end()) {
        parentOptions.linkerFlags.push_back(flag);
      }
    }

    for (const auto &pkg : options.packages) {
      parentOptions.packages[pkg.first] = pkg.second;
    }
  }

  return true;
}

int main(int argc, char **argv) {
  llvm::install_fatal_error_handler(utopiaFatalErrorHandler, nullptr);

  try {
    std::vector<std::string> args(argv + 1, argv + argc);
    GlobalOptions globalOpts;
    fs::path startPath = fs::current_path();

    for (auto it = args.begin(); it != args.end(); ++it) {
      std::string_view arg = *it;
      if (arg == "--emit-llvm") {
        globalOpts.emitLLVM = true;
      } else if (arg == "--emit-asm") {
        globalOpts.emitAsm = true;
      } else if (arg == "--jit") {
        globalOpts.isJIT = true;
      } else if (arg == "-g" || arg == "--debug") {
        globalOpts.isDebug = true;
      } else if (arg.starts_with("-O")) {
        if (arg.length() > 2 && std::isdigit(arg[2])) {
          globalOpts.cliOptLevel = std::stoi(std::string(arg.substr(2)));
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

#if defined(UTOPIA_RELEASE_BUILD)
    std::string exePathStr =
        llvm::sys::fs::getMainExecutable(argv[0], (void *)(intptr_t)&main);
    fs::path exePath(exePathStr);

    fs::path installRoot = exePath.parent_path().parent_path();

    fs::path stdlibPath = installRoot / "lib" / "utopia" / "stdlib" / "lib";
    fs::path preludePath = installRoot / "lib" / "utopia" / "prelude" / "lib";
    fs::path buildLibPath = installRoot / "lib" / "utopia" / "builder" / "lib";
#elif defined(UTOPIA_SOURCE_DIR)
    /* Resolve from the source tree during active development */
    fs::path stdlibPath =
        fs::path(UTOPIA_SOURCE_DIR) / "libs" / "stdlib" / "lib";
    fs::path preludePath =
        fs::path(UTOPIA_SOURCE_DIR) / "libs" / "prelude" / "lib";
    fs::path buildLibPath =
        fs::path(UTOPIA_SOURCE_DIR) / "libs" / "builder" / "lib";
#else
    /* Fallback relative resolution */
    fs::path stdlibPath =
        projRoot.parent_path().parent_path() / "libs" / "stdlib" / "lib";
    fs::path preludePath =
        projRoot.parent_path().parent_path() / "libs" / "prelude" / "lib";
    fs::path buildLibPath =
        projRoot.parent_path().parent_path() / "libs" / "builder" / "lib";
#endif

    globalOpts.stdlibRoot = stdlibPath.string();
    globalOpts.preludeRoot = preludePath.string();
    globalOpts.buildLibRoot = buildLibPath.string();

    CompileOptions dummyOptions;
    if (!buildProject(projRoot, dummyOptions, false, "", globalOpts)) {
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