#pragma once
#include "Core/GlobalOptions.hpp"
#include "ProjectManager.hpp"
#include "utopia/Driver/CompilerDriver.hpp"
#include <filesystem>
#include <string>

namespace utopia {

/* 'outJitExitCode' receives the exit code of the user program after a JIT
 * run (the main project's driver); it is left untouched otherwise. */
bool buildProject(const std::filesystem::path &projRoot,
                  CompileOptions &parentOptions, bool isSubproject,
                  const std::string &linkType, const GlobalOptions &globalOpts,
                  int *outJitExitCode = nullptr);

} // namespace utopia