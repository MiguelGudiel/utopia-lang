#include "Core/ProjectBuilder.hpp"
#include "BuildScriptRunner.hpp"
#include "Core/EnvLoader.hpp"
#include "utopia/Common/Logger.hpp"
#include <algorithm>
#include <iostream>

namespace utopia {

namespace fs = std::filesystem;

bool buildProject(const fs::path &projRoot, CompileOptions &parentOptions,
                  bool isSubproject, const std::string &linkType,
                  const GlobalOptions &globalOpts) {
  fs::path manifestPath = projRoot / "build.yaml";
  if (!fs::exists(manifestPath)) {
    std::cerr << "Fatal: build.yaml not found at " << projRoot << ".\n";
    if (isSubproject) {
      std::cerr << "Hint: A dependency seems to be missing. Did you forget to "
                   "run 'utopia yip get'?\n";
    }
    return false;
  }

  ProjectConfig config;
  try {
    config = parseBuildManifest(manifestPath);
  } catch (const std::exception &e) {
    std::cerr << "Manifest Error in " << projRoot << ": " << e.what() << "\n";
    return false;
  }

  CompileOptions options;
  options.projectName = config.name;
  options.outputName = config.outputName;
  options.currentProjectRoot = projRoot.string();

  if (isSubproject) {
    options.target =
        (linkType == "shared") ? "shared_library" : "static_library";

    options.mainProjectRoot = parentOptions.mainProjectRoot;
    options.mainOutputDir = parentOptions.mainOutputDir;
    if (config.outputDir.has_value()) {
      options.outputDir = (projRoot / config.outputDir.value()).string();
    } else {
      options.outputDir = parentOptions.outputDir;
    }

    options.optLevel = parentOptions.optLevel;
    options.includeDirs = parentOptions.includeDirs;
    options.linkerFlags = parentOptions.linkerFlags;
    options.publicMacros = parentOptions.publicMacros;
  } else {
    options.target = config.target;
    options.mainProjectRoot = projRoot.string();
    options.outputDir =
        (projRoot / config.outputDir.value_or("build")).string();
    options.mainOutputDir = options.outputDir;

    for (const auto &m : globalOpts.cliMacros) {
      options.publicMacros.insert(m);
    }
  }

  options.projectRoot = projRoot.string();
  options.stdlibRoot = globalOpts.stdlibRoot;
  options.preludeRoot = globalOpts.preludeRoot;
  options.buildLibRoot = globalOpts.buildLibRoot;

  options.emitLLVM = globalOpts.emitLLVM;
  options.emitAsm = globalOpts.emitAsm;
  options.isJIT = globalOpts.isJIT;
  options.isDebug = globalOpts.isDebug;
  options.doFormat = globalOpts.doFormat;
  options.targetTriple = globalOpts.targetTriple;
  options.sysroot = globalOpts.sysroot;

  if (config.optLevel.has_value()) {
    options.optLevel = config.optLevel.value();
  }

  if (globalOpts.cliOptLevel.has_value()) {
    options.optLevel = globalOpts.cliOptLevel.value();
  }

  for (const auto &flag : config.linkerFlags) {
    options.linkerFlags.push_back(flag);
  }

  if (!globalOpts.targetTriple.empty()) {
    options.linkerFlags.push_back("--target=" + globalOpts.targetTriple);
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

  fs::path buildUtpPath = projRoot / "build.utp";
  if (fs::exists(buildUtpPath)) {
    Logger::debug("[Utopia] Executing project build script (build.utp)...");
    if (!BuildScriptRunner::run(buildUtpPath, options, projRoot)) {
      std::cerr << "Fatal: Failed to execute build.utp successfully.\n";
      return false;
    }
  }

  for (const auto &dep : config.dependencies) {
    fs::path depPath;
    if (!dep.path.empty()) {
      depPath = projRoot / dep.path;
    } else if (!dep.name.empty()) {
      std::string homeDir = EnvLoader::get("HOME");
      if (homeDir.empty()) {
        homeDir = EnvLoader::get("USERPROFILE");
      }

      fs::path cacheRoot =
          fs::path(homeDir) / ".utopia" / "cache" / "yip" / "packages";
      fs::path pkgCacheDir = cacheRoot / dep.name;
      std::string targetVersion = dep.version;
      fs::path resolvedPath = pkgCacheDir / targetVersion;

      if (targetVersion.empty() || targetVersion == "latest" ||
          targetVersion == "any" || !fs::exists(resolvedPath)) {
        if (fs::exists(pkgCacheDir) && fs::is_directory(pkgCacheDir)) {
          for (const auto &entry : fs::directory_iterator(pkgCacheDir)) {
            if (entry.is_directory()) {
              targetVersion = entry.path().filename().string();
              resolvedPath = entry.path();
              break;
            }
          }
        }
      }

      depPath = resolvedPath;
    } else {
      continue;
    }

    if (!buildProject(depPath, options, true, dep.linkType, globalOpts)) {
      return false;
    }
  }

  if (config.resolvedSources.empty()) {
    std::cerr << "Fatal: No sources found in build.yaml at " << projRoot
              << "\n";
    return false;
  }

  options.sourcePath = config.resolvedSources.front().path;
  options.outputPath =
      (fs::path(options.outputDir) / config.outputName).string();

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
          "\"" +
          (outDir / "lib" / ("lib" + options.outputName + ext)).string() +
          "\"");
    } else if (options.target == "shared_library") {
      std::string ext = ".so";
#if defined(_WIN32)
      ext = ".dll";
#elif defined(__APPLE__)
      ext = ".dylib";
#endif
      parentOptions.linkerFlags.push_back(
          "\"" +
          (outDir / "bin" / ("lib" + options.outputName + ext)).string() +
          "\"");

#ifndef _WIN32
#ifdef __APPLE__
      parentOptions.linkerFlags.push_back("-Wl,-rpath,@executable_path");
#else
      parentOptions.linkerFlags.push_back("-Wl,-rpath,'$ORIGIN'");
#endif
#endif
    }

    for (const auto &inc : options.includeDirs) {
      if (std::find(parentOptions.includeDirs.begin(),
                    parentOptions.includeDirs.end(),
                    inc) == parentOptions.includeDirs.end()) {
        parentOptions.includeDirs.push_back(inc);
      }
    }

    for (const auto &flag : options.linkerFlags) {
      std::string processedFlag = flag;

      if (flag.starts_with("-L") && flag.length() > 2) {
        std::string pathStr = flag.substr(2);
        fs::path p(pathStr);
        if (!p.is_absolute()) {
          processedFlag = "-L" + (fs::path(options.projectRoot) / p).string();
        }
      }

      if (std::find(parentOptions.linkerFlags.begin(),
                    parentOptions.linkerFlags.end(),
                    processedFlag) == parentOptions.linkerFlags.end()) {
        parentOptions.linkerFlags.push_back(processedFlag);
      }
    }

    for (const auto &m : options.publicMacros) {
      parentOptions.publicMacros.insert(m);
    }

    for (const auto &pkg : options.packages) {
      parentOptions.packages[pkg.first] = pkg.second;
    }
  }

  return true;
}

} // namespace utopia