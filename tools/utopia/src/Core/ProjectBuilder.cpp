#include "Core/ProjectBuilder.hpp"
#include "BuildScriptRunner.hpp"
#include "Core/ArtifactCache.hpp"
#include "Core/EnvLoader.hpp"
#include "utopia/Common/Logger.hpp"
#include <algorithm>
#include <iostream>
#include <nlohmann/json.hpp>
#include <unordered_map>
#include <unordered_set>

namespace utopia {

namespace fs = std::filesystem;

namespace {

/* Options mutations produced by a build.utp script. The same project is
 * visited several times per build (diamond dependency graphs), so the
 * script is executed at most once per process and its mutations are
 * replayed on the remaining visits. */
struct BuildScriptDelta {
  std::vector<std::string> addedLinkerFlags;
  std::vector<std::string> addedIncludeDirs;
  std::vector<std::string> addedPublicMacros;
  std::vector<std::string> addedPrivateMacros;
  bool touchedOptLevel = false;
  int optLevel = 0;
  bool touchedAsync = false;
  bool asyncEnabled = true;
  bool touchedSysroot = false;
  std::string sysroot;
};

std::unordered_map<std::string, BuildScriptDelta> g_buildScriptSession;

void applyScriptDelta(CompileOptions &options, const BuildScriptDelta &delta) {
  options.linkerFlags.insert(options.linkerFlags.end(),
                             delta.addedLinkerFlags.begin(),
                             delta.addedLinkerFlags.end());
  options.includeDirs.insert(options.includeDirs.end(),
                             delta.addedIncludeDirs.begin(),
                             delta.addedIncludeDirs.end());
  options.publicMacros.insert(delta.addedPublicMacros.begin(),
                              delta.addedPublicMacros.end());
  options.privateMacros.insert(delta.addedPrivateMacros.begin(),
                               delta.addedPrivateMacros.end());
  if (delta.touchedOptLevel) {
    options.optLevel = delta.optLevel;
  }
  if (delta.touchedAsync) {
    options.asyncEnabled = delta.asyncEnabled;
  }
  if (delta.touchedSysroot) {
    options.sysroot = delta.sysroot;
  }
}

bool runBuildScriptOnce(const fs::path &buildUtpPath, CompileOptions &options,
                        const fs::path &projRoot) {
  std::string sessionKey = fs::weakly_canonical(buildUtpPath).string();
  auto sessionIt = g_buildScriptSession.find(sessionKey);
  if (sessionIt != g_buildScriptSession.end()) {
    applyScriptDelta(options, sessionIt->second);
    return true;
  }

  Logger::debug("[Utopia] Executing project build script (build.utp)...");

  size_t flagsBase = options.linkerFlags.size();
  size_t incsBase = options.includeDirs.size();
  std::unordered_set<std::string> pubBefore(options.publicMacros.begin(),
                                            options.publicMacros.end());
  std::unordered_set<std::string> privBefore(options.privateMacros.begin(),
                                             options.privateMacros.end());
  int optBefore = options.optLevel;
  bool asyncBefore = options.asyncEnabled;
  std::string sysrootBefore = options.sysroot;

  if (!BuildScriptRunner::run(buildUtpPath, options, projRoot)) {
    std::cerr << "Fatal: Failed to execute build.utp successfully.\n";
    return false;
  }

  BuildScriptDelta delta;
  delta.addedLinkerFlags.assign(options.linkerFlags.begin() + flagsBase,
                                options.linkerFlags.end());
  delta.addedIncludeDirs.assign(options.includeDirs.begin() + incsBase,
                                options.includeDirs.end());
  for (const auto &m : options.publicMacros) {
    if (!pubBefore.contains(m)) {
      delta.addedPublicMacros.push_back(m);
    }
  }
  for (const auto &m : options.privateMacros) {
    if (!privBefore.contains(m)) {
      delta.addedPrivateMacros.push_back(m);
    }
  }
  delta.touchedOptLevel = options.optLevel != optBefore;
  delta.optLevel = options.optLevel;
  delta.touchedAsync = options.asyncEnabled != asyncBefore;
  delta.asyncEnabled = options.asyncEnabled;
  delta.touchedSysroot = options.sysroot != sysrootBefore;
  delta.sysroot = options.sysroot;

  g_buildScriptSession[sessionKey] = std::move(delta);
  return true;
}

/* Final artifact produced by a project's CompilerDriver run, mirrored from
 * CompilerDriver::run() so the cache can verify and reuse it. */
fs::path expectedArtifactPath(const CompileOptions &options) {
  fs::path outDir(options.outputDir);
  if (options.target == "shared_library" || options.target == "shared") {
    std::string ext = ".so";
#if defined(_WIN32)
    ext = ".dll";
#elif defined(__APPLE__)
    ext = ".dylib";
#endif
    return outDir / "bin" / ("lib" + options.outputName + ext);
  }
  if (options.target == "library" || options.target == "static_library" ||
      options.target == "static") {
    std::string ext = ".a";
#if defined(_WIN32)
    ext = ".lib";
#endif
    return outDir / "lib" / ("lib" + options.outputName + ext);
  }
  return outDir / "bin" / options.outputName;
}

bool buildProjectRecursive(const fs::path &projRoot,
                           CompileOptions &parentOptions, bool isSubproject,
                           const std::string &linkType,
                           const GlobalOptions &globalOpts,
                           std::string *outFingerprint) {
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
  options.asyncEnabled = globalOpts.asyncEnabled;

  if (config.asyncEnabled.has_value()) {
    options.asyncEnabled = config.asyncEnabled.value();
  }

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
    std::string linkTriple = globalOpts.targetTriple;
    /* The NDK clang requires the API level in the target triple
     * ('aarch64-linux-android21') to locate the sysroot crt and platform
     * libraries. */
    if (linkTriple.find("android") != std::string::npos &&
        linkTriple.find("android") + 7 == linkTriple.size()) {
      linkTriple += "21";
    }
    options.linkerFlags.push_back("--target=" + linkTriple);
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
    if (!runBuildScriptOnce(buildUtpPath, options, projRoot)) {
      return false;
    }
  }

  /* Build every dependency first: the parent's fingerprint embeds each
   * dependency's fingerprint, so a change anywhere in the graph invalidates
   * every consumer. */
  std::vector<std::string> depFingerprints;
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

    std::string depFingerprint;
    if (!buildProjectRecursive(depPath, options, true, dep.linkType,
                               globalOpts, &depFingerprint)) {
      return false;
    }

    /* Record: name | requestedVersion | resolvedPath | linkType |
     * dependencyFingerprint. The path is canonicalized so the same
     * directory reached through different relative routes produces the
     * same record. */
    depFingerprints.push_back(dep.name + "|" + dep.version + "|" +
                              fs::weakly_canonical(depPath).string() + "|" +
                              dep.linkType + "|" + depFingerprint);
  }

  if (config.resolvedSources.empty()) {
    std::cerr << "Fatal: No sources found in build.yaml at " << projRoot
              << "\n";
    return false;
  }

  options.sourcePath = config.resolvedSources.front().path;
  /* Every resolved source becomes a root translation unit. The driver
   * compiles each one (deduplicating shared imports), so files listed
   * in `sources`/`source_dirs` that are not reachable from the first
   * source are no longer silently dropped. */
  for (const auto &src : config.resolvedSources) {
    options.resolvedSources.push_back(src.path);
  }
  options.outputPath =
      (fs::path(options.outputDir) / config.outputName).string();

  /* Content-addressed fingerprint: anything that affects the output, plus
   * the fingerprints of every dependency (recursively). */
  CacheInputs inputs;
  inputs.compilerId = globalOpts.compilerId;
  inputs.projectName = config.name;
  inputs.version = config.version;
  inputs.projectRoot = fs::weakly_canonical(projRoot).string();
  inputs.target = options.target;
  inputs.optLevel = options.optLevel;
  inputs.isDebug = options.isDebug;
  inputs.asyncEnabled = options.asyncEnabled;
  inputs.emitLLVM = options.emitLLVM;
  inputs.emitAsm = options.emitAsm;
  inputs.targetTriple = options.targetTriple;
  inputs.sysroot = options.sysroot;
  inputs.buildScriptHash =
      fs::exists(buildUtpPath) ? ArtifactCache::hashFile(buildUtpPath) : "";
  inputs.macros.assign(options.publicMacros.begin(), options.publicMacros.end());
  inputs.macros.insert(inputs.macros.end(), options.privateMacros.begin(),
                       options.privateMacros.end());
  /* Static archives are unaffected by linker flags, and the flags are
   * re-propagated to consumers on every run anyway. Only link steps
   * (shared library / executable) embed them in their output. */
  if (options.target == "shared_library" || options.target == "shared" ||
      options.target == "executable") {
    inputs.linkerFlags = options.linkerFlags;
  }
  for (const auto &src : config.resolvedSources) {
    inputs.sources.push_back(fs::weakly_canonical(src.path).string());
  }
  inputs.dependencies = depFingerprints;

  std::string fingerprint = ArtifactCache::computeFingerprint(inputs);
  if (outFingerprint) {
    *outFingerprint = fingerprint;
  }

  /* Formatting rewrites the sources and JIT runs produce no artifacts, so
   * neither participates in the artifact cache. */
  bool cacheEnabled = !options.doFormat && !options.isJIT;
  fs::path artifactPath = expectedArtifactPath(options);
  bool restored = false;

  if (cacheEnabled) {
    restored = ArtifactCache::restore(
        config.name, config.version, fingerprint, options.outputDir,
        artifactPath, options.emitLLVM || options.emitAsm);
    if (restored) {
      Logger::info("\033[1;32m[Cache Hit]\033[0m " + config.name +
                   " artifacts reused from cache.");
    }
  }

  if (!restored) {
    CompilerDriver driver(options);
    if (!driver.run()) {
      return false;
    }

    if (cacheEnabled) {
      nlohmann::json manifest;
      manifest["schema"] = 1;
      manifest["project"] = config.name;
      manifest["version"] = config.version;
      manifest["fingerprint"] = fingerprint;
      manifest["compiler"] = inputs.compilerId;
      manifest["dependencies"] = depFingerprints;
      ArtifactCache::store(config.name, config.version, fingerprint,
                           options.outputDir, manifest.dump(2),
                           options.emitLLVM || options.emitAsm);
    }
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

} // namespace

bool buildProject(const fs::path &projRoot, CompileOptions &parentOptions,
                  bool isSubproject, const std::string &linkType,
                  const GlobalOptions &globalOpts) {
  return buildProjectRecursive(projRoot, parentOptions, isSubproject, linkType,
                               globalOpts, nullptr);
}

} // namespace utopia
