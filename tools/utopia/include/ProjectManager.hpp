#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace utopia {

namespace fs = std::filesystem;

struct SourceInfo {
  std::string path;
  uint64_t lastWriteTime;
};

struct ProjectConfig {
  std::string name;
  std::string target;
  fs::path projectRoot;
  std::string outputDir;
  std::vector<SourceInfo> resolvedSources;
  std::vector<std::string> includeDirs;
  std::vector<std::string> linkerFlags;
  int optLevel = 0;
};

fs::path findProjectRoot(fs::path startPath);
ProjectConfig parseBuildManifest(const fs::path &manifestPath);

uint64_t getFileTimestamp(const fs::path &path);

} // namespace utopia