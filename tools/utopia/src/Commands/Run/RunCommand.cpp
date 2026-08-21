#include "Commands/Run/RunCommand.hpp"
#include "CLI/OptionsParser.hpp"
#include "Core/ArtifactCache.hpp"
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

  if (!OptionsParser::parseCommonOptions(args, globalOpts, startPath)) {
    return 1;
  }
  globalOpts.isJIT = true;

  std::filesystem::path projRoot = findProjectRoot(startPath);
  if (projRoot.empty()) {
    std::cerr << "Fatal: build.yaml not found in path chain.\n";
    return 1;
  }

  OptionsParser::resolveStandardPaths(executablePath, projRoot, globalOpts);
  globalOpts.compilerId = ArtifactCache::computeCompilerId(executablePath);

  CompileOptions dummyOptions;
  int jitExitCode = 0;
  if (!buildProject(projRoot, dummyOptions, false, "", globalOpts,
                    &jitExitCode)) {
    return 1;
  }

  /* The user program's exit code is part of the command's result: 'run'
   * returning 0 for a program that failed would hide the failure from
   * scripts and CI. */
  return jitExitCode;
}

} // namespace utopia