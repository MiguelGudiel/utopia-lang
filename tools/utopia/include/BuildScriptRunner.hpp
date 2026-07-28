#pragma once
#include "utopia/Driver/CompilerDriver.hpp"
#include <filesystem>

namespace utopia {

class BuildScriptRunner {
public:
  static bool run(const std::filesystem::path &scriptPath,
                  CompileOptions &options,
                  const std::filesystem::path &projRoot);
};

} // namespace utopia