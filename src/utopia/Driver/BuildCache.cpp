#include "utopia/Driver/BuildCache.hpp"
#include <fstream>
#include <iostream>
#include <algorithm>

namespace utopia {

BuildCache::BuildCache(const std::filesystem::path &cacheFile)
    : cacheFilePath(cacheFile) {}

void BuildCache::load() {
  if (!std::filesystem::exists(cacheFilePath))
    return;
  std::ifstream file(cacheFilePath);
  if (!file)
    return;
  nlohmann::json j;
  try {
    file >> j;
  } catch (...) {
    return;
  }
  for (auto &[path, info] : j.items()) {
    ModuleInfo mi;
    mi.timestamp = info["timestamp"];
    mi.imports = info["imports"].get<std::vector<std::string>>();
    modules[path] = mi;
  }
}

void BuildCache::save() {
  nlohmann::json j;
  for (auto &[path, info] : modules) {
    j[path]["timestamp"] = info.timestamp;
    j[path]["imports"] = info.imports;
  }
  std::ofstream file(cacheFilePath);
  if (file)
    file << j.dump(2);
}

bool BuildCache::isUpToDate(const std::string &modulePath, uint64_t currentTime,
                            const std::vector<std::string> &imports) {
  auto it = modules.find(modulePath);
  if (it == modules.end())
    return false;
    
  if (it->second.timestamp != currentTime)
    return false;

  std::vector<std::string> visited;
  visited.push_back(modulePath);

  /*
   * DFS through the dependency graph.
   * We compare the child's cache timestamp against the parent's DISK timestamp.
   * If a deep dependency mutates, its cache time will exceed the parent's,
   * effectively poisoning the bloodline without needing OS filesystem checks.
   * Rip and tear.
   */
  auto checkRecursively = [&](const std::string& node, auto& self) -> bool {
    auto impIt = modules.find(node);
    if (impIt == modules.end())
      return false;

    if (impIt->second.timestamp > currentTime)
      return false;

    visited.push_back(node);

    for (const auto& child : impIt->second.imports) {
      if (std::find(visited.begin(), visited.end(), child) != visited.end())
        continue;

      if (!self(child, self))
        return false;
    }
    
    return true;
  };

  for (const auto &imp : imports) {
    if (!checkRecursively(imp, checkRecursively)) {
      return false;
    }
  }

  return true;
}

void BuildCache::update(const std::string &modulePath, uint64_t currentTime,
                        const std::vector<std::string> &imports) {
  modules[modulePath] = {currentTime, imports};
}

} // namespace utopia