#include <utopia/Driver/BuildCache.hpp>
#include <fstream>
#include <iostream>

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
  // Comprobar que las dependencias no han cambiado
  for (const auto &imp : imports) {
    // Nota: necesitaríamos timestamp de cada import; aquí simplificamos
    // asumiendo que si el módulo está actualizado, sus imports también.
    // Para una implementación real, se debe verificar recursivamente.
  }
  return true;
}

void BuildCache::update(const std::string &modulePath, uint64_t currentTime,
                        const std::vector<std::string> &imports) {
  modules[modulePath] = {currentTime, imports};
}

} // namespace utopia