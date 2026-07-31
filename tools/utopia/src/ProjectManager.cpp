#include "ProjectManager.hpp"
#include <chrono>
#include <stdexcept>
#include <yaml-cpp/yaml.h>

namespace utopia {

uint64_t getFileTimestamp(const fs::path &path) {
  auto ftime = fs::last_write_time(path);
  auto sysTime = std::chrono::clock_cast<std::chrono::system_clock>(ftime);

  return std::chrono::duration_cast<std::chrono::seconds>(
             sysTime.time_since_epoch())
      .count();
}

fs::path findProjectRoot(fs::path startPath) {
  fs::path current = fs::absolute(startPath);
  if (!fs::is_directory(current))
    current = current.parent_path();

  while (current.has_parent_path()) {
    if (fs::exists(current / "build.yaml"))
      return current;
    current = current.parent_path();
  }
  return "";
}

ProjectConfig parseBuildManifest(const fs::path &manifestPath) {
  ProjectConfig config;
  try {
    YAML::Node root = YAML::LoadFile(manifestPath.string());

    fs::path baseDir = manifestPath.parent_path();
    config.projectRoot = baseDir;

    if (root["project"]) {
      config.name = root["project"]["name"].as<std::string>("utopia_out");
    }

    if (root["build"]) {
      auto b = root["build"];
      config.target = b["target"].as<std::string>("executable");
      config.outputDir =
          b["output_dir"] ? b["output_dir"].as<std::string>() : "build";

      if (b["optimization"]) {
        config.optLevel = b["optimization"].as<int>();
      }

      if (b["linker_flags"] && b["linker_flags"].IsSequence()) {
        for (const auto &flag : b["linker_flags"]) {
          config.linkerFlags.push_back(flag.as<std::string>());
        }
      }

      if (b["source_dirs"] && b["source_dirs"].IsSequence()) {
        for (const auto &dir : b["source_dirs"]) {
          fs::path dirPath = baseDir / dir.as<std::string>();
          if (!fs::exists(dirPath) || !fs::is_directory(dirPath))
            continue;

          config.includeDirs.push_back(dirPath.string());
          for (const auto &entry : fs::recursive_directory_iterator(dirPath)) {
            if (entry.is_regular_file() && entry.path().extension() == ".utp") {
              config.resolvedSources.push_back(
                  {entry.path().string(), getFileTimestamp(entry.path())});
            }
          }
        }
      }

      if (b["sources"] && b["sources"].IsSequence()) {
        for (const auto &src : b["sources"]) {
          fs::path srcPath = baseDir / src.as<std::string>();
          if (fs::exists(srcPath) && srcPath.extension() == ".utp") {
            config.resolvedSources.push_back(
                {srcPath.string(), getFileTimestamp(srcPath)});
          }
        }
      }

      if (b["include_dirs"] && b["include_dirs"].IsSequence()) {
        for (const auto &inc : b["include_dirs"]) {
          config.includeDirs.push_back(
              (baseDir / inc.as<std::string>()).string());
        }
      }
    }

    if (root["dependencies"] && root["dependencies"].IsSequence()) {
      for (const auto &dep : root["dependencies"]) {
        SubprojectConfig sub;
        sub.path = dep["path"].as<std::string>();
        sub.linkType = dep["link"] ? dep["link"].as<std::string>() : "static";
        config.dependencies.push_back(sub);
      }
    }

  } catch (const YAML::Exception &e) {
    throw std::runtime_error("YAML parse error: " + std::string(e.what()));
  }
  return config;
}

} // namespace utopia