#include "Commands/Run/RunCommand.hpp"
#include "CLI/OptionsParser.hpp"
#include "Core/ProjectBuilder.hpp"
#include "ProjectManager.hpp"
#include <iostream>

namespace utopia {

std::string RunCommand::getName() const { return "run"; }

std::string RunCommand::getDescription() const {
  return "Compile and execute the project using JIT.";
}

int RunCommand::execute(const std::vector<std::string> &args,
                        const std::string &executablePath) {
  GlobalOptions globalOpts;
  std::filesystem::path startPath = std::filesystem::current_path();

  OptionsParser::parseCommonOptions(args, globalOpts, startPath);
  globalOpts.isJIT = true;

  std::filesystem::path projRoot = findProjectRoot(startPath);
  if (projRoot.empty()) {
    std::cerr << "Fatal: build.yaml not found in path chain.\n";
    return 1;
  }

  OptionsParser::resolveStandardPaths(executablePath, projRoot, globalOpts);

  CompileOptions dummyOptions;
  if (!buildProject(projRoot, dummyOptions, false, "", globalOpts)) {
    return 1;
  }

  return 0;
}

} // namespace utopia