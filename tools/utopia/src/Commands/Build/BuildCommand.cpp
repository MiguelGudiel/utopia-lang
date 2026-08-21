#include "Commands/Build/BuildCommand.hpp"
#include "CLI/OptionsParser.hpp"
#include "Core/ArtifactCache.hpp"
#include "Core/ProjectBuilder.hpp"
#include "ProjectManager.hpp"
#include <iostream>

namespace utopia {

std::string BuildCommand::getName() const { return "build"; }

std::string BuildCommand::getDescription() const {
  return "Compile the project or file.";
}

int BuildCommand::execute(const std::vector<std::string> &args,
                          const std::string &executablePath) {
  GlobalOptions globalOpts;
  std::filesystem::path startPath = std::filesystem::current_path();

  if (!OptionsParser::parseCommonOptions(args, globalOpts, startPath)) {
    return 1;
  }

  std::filesystem::path projRoot = findProjectRoot(startPath);
  if (projRoot.empty()) {
    std::cerr << "Fatal: build.yaml not found in path chain.\n";
    return 1;
  }

  OptionsParser::resolveStandardPaths(executablePath, projRoot, globalOpts);
  globalOpts.compilerId = ArtifactCache::computeCompilerId(executablePath);

  CompileOptions dummyOptions;
  if (!buildProject(projRoot, dummyOptions, false, "", globalOpts)) {
    return 1;
  }

  return 0;
}

} // namespace utopia