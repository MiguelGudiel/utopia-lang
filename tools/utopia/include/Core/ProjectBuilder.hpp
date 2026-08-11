#pragma once
#include "Core/GlobalOptions.hpp"
#include "ProjectManager.hpp"
#include "utopia/Driver/CompilerDriver.hpp"
#include <filesystem>
#include <string>

namespace utopia {

bool buildProject(const std::filesystem::path &projRoot,
                  CompileOptions &parentOptions, bool isSubproject,
                  const std::string &linkType, const GlobalOptions &globalOpts);

} // namespace utopia