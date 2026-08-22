#include "LspCore.hpp"
#include "utopia/Common/Warnings.hpp"
#include <cstdlib>
#include <fstream>
#include <mutex>
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

std::string
DocumentManager::manifestTextFor(const std::filesystem::path &projRoot) const {
  {
    std::lock_guard<std::mutex> lock(cacheMutex);
    auto it = manifestTexts.find(projRoot.string());
    if (it != manifestTexts.end())
      return it->second;
  }
  std::filesystem::path manifest = projRoot / "build.yaml";
  if (std::filesystem::exists(manifest)) {
    std::ifstream in(manifest);
    std::stringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
  }
  return "";
}

void DocumentManager::applyAsyncConfig(ModuleLoaderConfig &modConfig,
                                       const std::filesystem::path &projRoot) {
  /* Async support is enabled by default; build.yaml may disable it with
   * 'async: false'. The prelude's Future class is guarded by the
   * UTOPIA_ASYNC macro, so it must be defined exactly like the driver. */
  if (!projRoot.empty()) {
    std::string manifestText = manifestTextFor(projRoot);
    if (!manifestText.empty()) {
      try {
        YAML::Node root = YAML::Load(manifestText);
        if (root["build"] && root["build"]["async"]) {
          modConfig.asyncEnabled = root["build"]["async"].as<bool>();
        }

        /* 'build.warnings' disables warning kinds for this project:
         * either a map of name -> bool or a list of names. */
        if (root["build"] && root["build"]["warnings"]) {
          YAML::Node warnings = root["build"]["warnings"];
          if (warnings.IsMap()) {
            for (const auto &entry : warnings) {
              std::string name = entry.first.as<std::string>();
              if (!entry.second.as<bool>(true)) {
                modConfig.disabledWarnings.push_back(name);
              }
            }
          } else if (warnings.IsSequence()) {
            for (const auto &entry : warnings) {
              modConfig.disabledWarnings.push_back(
                  entry.as<std::string>());
            }
          }
        }
      } catch (...) {
        /* Malformed build.yaml: keep the defaults. */
      }
    }
  }
  if (modConfig.asyncEnabled) {
    modConfig.definedMacros.insert("UTOPIA_ASYNC");
  }
}

void DocumentManager::reanalyzeProject(const std::string &projRoot) {
  std::vector<std::pair<std::string, std::string>> toAnalyze;
  {
    std::shared_lock<std::shared_mutex> lock(docMutex);
    for (const auto &[uri, state] : documents) {
      std::string path = uriToPath(uri);
      if (!path.empty() && findProjectRoot(path).string() == projRoot) {
        toAnalyze.emplace_back(uri, state.text);
      }
    }
  }
  for (const auto &[uri, text] : toAnalyze) {
    requestBackgroundAnalysis(uri, text);
  }
}

void DocumentManager::refreshProject(const std::string &projRoot) {
  {
    std::lock_guard<std::mutex> lock(cacheMutex);
    projectConfigCache.erase(projRoot);
    configMtimes.erase(projRoot);
  }
  reanalyzeProject(projRoot);
}

void DocumentManager::onBuildManifestChanged(const std::string &uri,
                                             const std::string &text) {
  std::string projRoot = findProjectRoot(uriToPath(uri)).string();
  {
    std::lock_guard<std::mutex> lock(cacheMutex);
    manifestTexts[projRoot] = text;
  }
  refreshProject(projRoot);
}

void DocumentManager::onBuildManifestSaved(const std::string &uri) {
  std::filesystem::path manifestPath = uriToPath(uri);
  /* When the manifest no longer exists, the upward search could land on an
   * unrelated parent project; the manifest's own directory is the root. */
  std::filesystem::path root = manifestPath.parent_path();
  if (std::filesystem::exists(manifestPath))
    root = findProjectRoot(manifestPath);
  refreshProject(root.string());
}

void DocumentManager::onBuildManifestClosed(const std::string &uri) {
  std::filesystem::path manifestPath = uriToPath(uri);
  std::filesystem::path root = manifestPath.parent_path();
  if (std::filesystem::exists(manifestPath))
    root = findProjectRoot(manifestPath);
  std::string projRoot = root.string();
  {
    std::lock_guard<std::mutex> lock(cacheMutex);
    manifestTexts.erase(projRoot);
    /* The disk mtime is re-seeded by the next poll, so an out-of-date
     * entry cannot suppress the refresh the close triggers. */
    manifestDiskMtimes.erase(projRoot);
  }
  refreshProject(projRoot);
}

void DocumentManager::pollManifests() {
  /* Every project the server knows about: open manifests, cached configs
   * and analyzed documents. */
  std::vector<std::string> roots;
  {
    std::lock_guard<std::mutex> lock(cacheMutex);
    for (const auto &[root, _] : manifestTexts)
      roots.push_back(root);
    for (const auto &[root, _] : projectConfigCache)
      roots.push_back(root);
    for (const auto &[_, root] : uriToProjectRoot)
      roots.push_back(root.string());
  }
  std::sort(roots.begin(), roots.end());
  roots.erase(std::unique(roots.begin(), roots.end()), roots.end());

  for (const auto &root : roots) {
    {
      /* An open editor copy takes precedence; disk changes only matter once
       * the document is closed (or was never opened). */
      std::lock_guard<std::mutex> lock(cacheMutex);
      if (manifestTexts.contains(root))
        continue;
    }

    std::filesystem::path manifest = std::filesystem::path(root) / "build.yaml";
    auto mtime = std::filesystem::exists(manifest)
                     ? std::filesystem::last_write_time(manifest)
                     : std::filesystem::file_time_type::min();

    bool changed = false;
    {
      std::lock_guard<std::mutex> lock(cacheMutex);
      auto it = manifestDiskMtimes.find(root);
      if (it == manifestDiskMtimes.end()) {
        /* First sight: seed the baseline from the mtime the current
         * configuration was derived from, not from the file right now: the
         * manifest may have changed before the first poll ran, and that
         * change must still be detected. */
        auto cfgIt = configMtimes.find(root);
        it = manifestDiskMtimes
                 .emplace(root, cfgIt != configMtimes.end()
                                    ? cfgIt->second
                                    : mtime)
                 .first;
      }
      if (it->second != mtime) {
        it->second = mtime;
        changed = true;
      }
    }

    if (changed) {
      refreshProject(root);
    }
  }
}

ModuleLoaderConfig
DocumentManager::configFor(const std::string &uri,
                           const std::filesystem::path &filePath) {
  std::filesystem::path projRoot = projectRootFor(uri, filePath);
  std::string projRootStr = projRoot.string();

  /* The cached config must be discarded when build.yaml changes (e.g. after
   * the 'disable in project' quick fix edits the manifest). */
  {
    std::lock_guard<std::mutex> lock(cacheMutex);
    auto it = projectConfigCache.find(projRootStr);
    if (it != projectConfigCache.end()) {
      std::filesystem::path manifest = projRoot / "build.yaml";
      auto mtime = std::filesystem::exists(manifest)
                       ? std::filesystem::last_write_time(manifest)
                       : std::filesystem::file_time_type::min();
      auto cached = it->second;
      auto cachedIt = configMtimes.find(projRootStr);
      if (cachedIt != configMtimes.end() && cachedIt->second == mtime) {
        return cached;
      }
      projectConfigCache.erase(it);
    }
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
  configMtimes[projRootStr] = std::filesystem::exists(projRoot / "build.yaml")
                                  ? std::filesystem::last_write_time(
                                        projRoot / "build.yaml")
                                  : std::filesystem::file_time_type::min();
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

      /* Warning kinds disabled by the project manifest. */
      for (const auto &name : modConfig.disabledWarnings) {
        newState.sema->warningConfig.disable(name);
      }

      /* In-source suppression directives. The text being analyzed is
       * authoritative for this document; other files (imports) are read on
       * demand and cached. textFor() returns the previously stored version
       * (the new state is committed at the end of processFile), so the
       * current document must be seeded explicitly, otherwise the
       * suppression scan would always lag one edit behind. */
      auto suppressionCache =
          std::make_shared<std::unordered_map<std::string, WarningSuppressions>>();
      (*suppressionCache)[filePath] = collectWarningSuppressions(newState.text);
      std::mutex suppressionMutex;
      newState.sema->warningFilter =
          [this, suppressionCache, &suppressionMutex](
              WarningKind kind, std::string_view filePath, int line) {
            std::string key(filePath);
            {
              std::lock_guard<std::mutex> lock(suppressionMutex);
              auto it = suppressionCache->find(key);
              if (it == suppressionCache->end()) {
                WarningSuppressions supp =
                    collectWarningSuppressions(textFor(pathToUri(key)));
                it = suppressionCache->emplace(key, std::move(supp)).first;
              }
              return !it->second.suppresses(kind, line);
            }
          };

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
