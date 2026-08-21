#include "Commands/Fmt/FmtCommand.hpp"
#include "CLI/OptionsParser.hpp"
#include "Core/ProjectBuilder.hpp"
#include "ProjectManager.hpp"
#include <iostream>

namespace utopia {

std::string FmtCommand::getName() const { return "fmt"; }

std::string FmtCommand::getDescription() const {
  return "Format the source code.";
}

int FmtCommand::execute(const std::vector<std::string> &args,
                        const std::string &executablePath) {
  GlobalOptions globalOpts;
  std::filesystem::path startPath = std::filesystem::current_path();

  if (!OptionsParser::parseCommonOptions(args, globalOpts, startPath)) {
    return 1;
  }
  globalOpts.doFormat = true;

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