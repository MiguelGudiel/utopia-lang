#include "LspCore.hpp"
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <yaml-cpp/yaml.h>

namespace utopia::lsp {

namespace {

/* Recursively parses build.yaml manifests to map subproject dependencies to
 * their include directories and package roots, so imports across the
 * workspace resolve like the compiler driver resolves them. */
void loadPackagesRecursive(const std::filesystem::path &manifestPath,
                           std::unordered_map<std::string, std::string> &packages,
                           std::vector<std::string> &includeDirs,
                           std::unordered_set<std::string> &visited) {
  if (manifestPath.empty() || !std::filesystem::exists(manifestPath))
    return;

  std::string absPath = std::filesystem::absolute(manifestPath).string();
  if (visited.contains(absPath))
    return;
  visited.insert(absPath);

  try {
    YAML::Node root = YAML::LoadFile(manifestPath.string());
    std::string projName = "unknown";
    if (root["project"] && root["project"]["name"]) {
      projName = root["project"]["name"].as<std::string>();
    }

    std::filesystem::path baseDir = manifestPath.parent_path();
    std::string selfPkgRoot = baseDir.string();

    if (root["build"]) {
      auto b = root["build"];
      std::vector<std::string> dirs;

      if (b["source_dirs"] && b["source_dirs"].IsSequence()) {
        for (const auto &dir : b["source_dirs"]) {
          dirs.push_back((baseDir / dir.as<std::string>()).string());
        }
      }

      if (b["include_dirs"] && b["include_dirs"].IsSequence()) {
        for (const auto &inc : b["include_dirs"]) {
          dirs.push_back((baseDir / inc.as<std::string>()).string());
        }
      }

      for (const auto &d : dirs) {
        if (std::find(includeDirs.begin(), includeDirs.end(), d) ==
            includeDirs.end()) {
          includeDirs.push_back(d);
        }
      }

      if (!dirs.empty()) {
        selfPkgRoot = dirs.front();
      }
    }

    if (std::find(includeDirs.begin(), includeDirs.end(), baseDir.string()) ==
        includeDirs.end()) {
      includeDirs.push_back(baseDir.string());
    }

    packages[projName] = selfPkgRoot;

    if (root["dependencies"] && root["dependencies"].IsSequence()) {
      for (const auto &dep : root["dependencies"]) {
        if (dep["path"]) {
          std::string depPath = dep["path"].as<std::string>();
          std::filesystem::path depYaml = baseDir / depPath / "build.yaml";
          loadPackagesRecursive(depYaml, packages, includeDirs, visited);
        } else if (dep["name"]) {
          /* Registry dependency (yip): resolve from the local package cache,
           * same layout the compiler driver uses. */
          std::string depName = dep["name"].as<std::string>();
          std::string depVersion =
              dep["version"] ? dep["version"].as<std::string>() : "";

          const char *homeEnv = std::getenv("HOME");
          std::string homeDir = homeEnv ? homeEnv : "";
          if (homeDir.empty()) {
            const char *userProfileEnv = std::getenv("USERPROFILE");
            homeDir = userProfileEnv ? userProfileEnv : "";
          }

          std::filesystem::path cacheRoot = std::filesystem::path(homeDir) /
                                            ".utopia" / "cache" / "yip" /
                                            "packages";
          std::filesystem::path pkgCacheDir = cacheRoot / depName;
          std::filesystem::path resolvedPath = pkgCacheDir / depVersion;

          if (depVersion.empty() || depVersion == "latest" ||
              depVersion == "any" ||
              !std::filesystem::exists(resolvedPath)) {
            if (std::filesystem::exists(pkgCacheDir) &&
                std::filesystem::is_directory(pkgCacheDir)) {
              for (const auto &entry :
                   std::filesystem::directory_iterator(pkgCacheDir)) {
                if (entry.is_directory()) {
                  resolvedPath = entry.path();
                  break;
                }
              }
            }
          }

          std::filesystem::path depYaml = resolvedPath / "build.yaml";
          loadPackagesRecursive(depYaml, packages, includeDirs, visited);
        }
      }
    }
  } catch (...) {
    /* A malformed manifest only degrades package resolution; the document
     * itself still gets analyzed. */
  }
}

} // namespace

std::filesystem::path
DocumentManager::findProjectRoot(const std::filesystem::path &current) const {
  std::filesystem::path cursor = current;
  if (!std::filesystem::is_directory(cursor))
    cursor = cursor.parent_path();
  while (cursor.has_parent_path()) {
    if (std::filesystem::exists(cursor / "build.yaml"))
      return cursor;
    cursor = cursor.parent_path();
  }
  return "";
}

std::filesystem::path
DocumentManager::projectRootFor(const std::string &uri,
                                const std::filesystem::path &filePath) {
  {
    std::lock_guard<std::mutex> lock(cacheMutex);
    auto it = uriToProjectRoot.find(uri);
    if (it != uriToProjectRoot.end())
      return it->second;
  }
  std::filesystem::path root = findProjectRoot(filePath);
  {
    std::lock_guard<std::mutex> lock(cacheMutex);
    uriToProjectRoot[uri] = root;
  }
  return root;
}

void DocumentManager::applyAsyncConfig(ModuleLoaderConfig &modConfig,
                                       const std::filesystem::path &projRoot) {
  /* Async support is enabled by default; build.yaml may disable it with
   * 'async: false'. The prelude's Future class is guarded by the
   * UTOPIA_ASYNC macro, so it must be defined exactly like the driver. */
  if (!projRoot.empty()) {
    std::filesystem::path manifest = projRoot / "build.yaml";
    if (std::filesystem::exists(manifest)) {
      try {
        YAML::Node root = YAML::LoadFile(manifest.string());
        if (root["build"] && root["build"]["async"]) {
          modConfig.asyncEnabled = root["build"]["async"].as<bool>();
        }
      } catch (...) {
        /* Malformed build.yaml: keep the default. */
      }
    }
  }
  if (modConfig.asyncEnabled) {
    modConfig.definedMacros.insert("UTOPIA_ASYNC");
  }
}

ModuleLoaderConfig
DocumentManager::configFor(const std::string &uri,
                           const std::filesystem::path &filePath) {
  std::filesystem::path projRoot = projectRootFor(uri, filePath);
  std::string projRootStr = projRoot.string();

  {
    std::lock_guard<std::mutex> lock(cacheMutex);
    auto it = projectConfigCache.find(projRootStr);
    if (it != projectConfigCache.end())
      return it->second;
  }

  ModuleLoaderConfig modConfig;
  modConfig.projectRoot = projRoot;
  std::filesystem::path stdlibPath =
      projRoot.empty()
          ? ""
          : projRoot.parent_path().parent_path() / "libs" / "stdlib" / "lib";
  std::filesystem::path preludePath =
      projRoot.empty()
          ? ""
          : projRoot.parent_path().parent_path() / "libs" / "prelude" / "lib";
  std::filesystem::path buildLibPath =
      projRoot.empty()
          ? ""
          : projRoot.parent_path().parent_path() / "libs" / "builder" / "lib";

#ifdef UTOPIA_SOURCE_DIR
  if (!std::filesystem::exists(stdlibPath)) {
    stdlibPath =
        std::filesystem::path(UTOPIA_SOURCE_DIR) / "libs" / "stdlib" / "lib";
    preludePath =
        std::filesystem::path(UTOPIA_SOURCE_DIR) / "libs" / "prelude" / "lib";
    buildLibPath =
        std::filesystem::path(UTOPIA_SOURCE_DIR) / "libs" / "builder" / "lib";
  }
#endif

  modConfig.stdlibRoot = stdlibPath;
  modConfig.preludeRoot = preludePath;
  modConfig.buildLibRoot = buildLibPath;

  if (!projRoot.empty()) {
    std::unordered_set<std::string> visited;
    loadPackagesRecursive(projRoot / "build.yaml", modConfig.packages,
                          modConfig.includeDirs, visited);
  }

#if defined(_WIN32)
  modConfig.definedMacros.insert("_WIN32");
#elif defined(__APPLE__)
  modConfig.definedMacros.insert("__APPLE__");
#elif defined(__linux__) || defined(__gnu_linux__)
  modConfig.definedMacros.insert("__gnu_linux__");
#endif
#if defined(__x86_64__) || defined(_M_X64)
  modConfig.definedMacros.insert("x64");
#elif defined(__aarch64__) || defined(_M_ARM64)
  modConfig.definedMacros.insert("arm64");
#endif

  applyAsyncConfig(modConfig, projRoot);

  std::lock_guard<std::mutex> lock(cacheMutex);
  projectConfigCache[projRootStr] = modConfig;
  return modConfig;
}

void DocumentManager::processFile(const std::string &uri, std::string text) {
  DocumentState newState;
  newState.text = std::move(text);
  newState.astCtx = std::make_shared<ASTContext>();
  newState.diags = std::make_shared<DiagnosticsEngine>();
  newState.diags->printToConsole = false;

  std::string filePath = uriToPath(uri);
  std::filesystem::path currentPath(filePath);

  ModuleLoaderConfig modConfig = configFor(uri, currentPath);

  /* build.utp files load the builder API through the module loader. */
  if (currentPath.filename() == "build.utp") {
    modConfig.isBuildScript = true;
  }

  ModuleLoader loader(*newState.astCtx, modConfig, *newState.diags);

  try {
    newState.ast = loader.loadModule(filePath, currentPath.parent_path(), 0, 0,
                                     0, filePath, newState.text);
    if (newState.ast) {
      newState.sema = std::make_shared<SemaContext>(*newState.astCtx,
                                                    *newState.diags, filePath);
      SemaPipeline pipeline;
      pipeline.run(newState.ast, *newState.sema);
    }
  } catch (const std::exception &e) {
    /* Analysis errors must never kill the server: report diagnostics from
     * whatever was parsed and keep the document state usable. */
    std::cerr << "[LSP] Analysis failed for " << uri << ": " << e.what()
              << "\n";
  } catch (...) {
    std::cerr << "[LSP] Analysis failed for " << uri
              << " (unknown error).\n";
  }

  sendResponse({{"jsonrpc", "2.0"},
                {"method", "textDocument/publishDiagnostics"},
                {"params",
                 {{"uri", uri},
                  {"diagnostics", newState.diags->toJSON(filePath)}}}});

  {
    std::unique_lock<std::shared_mutex> lock(docMutex);
    documents[uri] = std::move(newState);
  }
}

bool DocumentManager::get(const std::string &uri, DocumentState &out) const {
  std::shared_lock<std::shared_mutex> lock(docMutex);
  auto it = documents.find(uri);
  if (it == documents.end())
    return false;
  out = it->second;
  return true;
}

std::string DocumentManager::textFor(const std::string &uri) const {
  {
    std::shared_lock<std::shared_mutex> lock(docMutex);
    auto it = documents.find(uri);
    if (it != documents.end())
      return it->second.text;
  }
  std::ifstream file(uriToPath(uri));
  if (file) {
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
  }
  return "";
}

std::vector<std::pair<std::string, DocumentState>>
DocumentManager::snapshot() const {
  std::shared_lock<std::shared_mutex> lock(docMutex);
  return {documents.begin(), documents.end()};
}

} // namespace utopia::lsp
