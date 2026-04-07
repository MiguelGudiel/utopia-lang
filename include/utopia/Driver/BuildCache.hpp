#pragma once
#include <filesystem>
#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace utopia {

class BuildCache {
public:
  explicit BuildCache(const std::filesystem::path &cacheFile);
  void load();
  void save();
  bool isUpToDate(const std::string &modulePath, uint64_t currentTime,
                  const std::vector<std::string> &imports);
  void update(const std::string &modulePath, uint64_t currentTime,
              const std::vector<std::string> &imports);

private:
  std::filesystem::path cacheFilePath;
  struct ModuleInfo {
    uint64_t timestamp;
    std::vector<std::string> imports;
  };
  std::map<std::string, ModuleInfo> modules;
};

} // namespace utopia