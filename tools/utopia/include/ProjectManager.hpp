#pragma once
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace utopia {

namespace fs = std::filesystem;

struct SourceInfo {
  std::string path;
  uint64_t lastWriteTime;
};

struct SubprojectConfig {
  std::string name;
  std::string version;
  std::string path;
  std::string linkType;
};

struct ProjectConfig {
  std::string name;
  std::string outputName;
  std::string version;
  std::string target;
  fs::path projectRoot;
  std::optional<std::string> outputDir;
  std::vector<SourceInfo> resolvedSources;
  std::vector<std::string> includeDirs;
  std::vector<std::string> linkerFlags;
  std::optional<int> optLevel;
  std::vector<SubprojectConfig> dependencies;
};

fs::path findProjectRoot(fs::path startPath);
ProjectConfig parseBuildManifest(const fs::path &manifestPath);

uint64_t getFileTimestamp(const fs::path &path);

} // namespace utopia