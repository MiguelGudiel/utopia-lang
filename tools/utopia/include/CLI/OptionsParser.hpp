#pragma once
#include "Core/GlobalOptions.hpp"
#include <filesystem>
#include <string>
#include <vector>

namespace utopia {

class OptionsParser {
public:
  static void parseCommonOptions(const std::vector<std::string> &args,
                                 GlobalOptions &opts,
                                 std::filesystem::path &startPath);

  static void resolveStandardPaths(const std::string &exePathStr,
                                   const std::filesystem::path &projRoot,
                                   GlobalOptions &opts);
};

} // namespace utopia