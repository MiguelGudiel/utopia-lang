#pragma once
#include "Core/GlobalOptions.hpp"
#include <filesystem>
#include <string>
#include <vector>

namespace utopia {

class OptionsParser {
public:
  /* Parses the CLI arguments into 'opts'. Returns false when an option is
   * malformed or unknown; the offending argument is printed to stderr so
   * the caller can abort before doing anything (silently ignoring a
   * misspelled flag would produce a build that does not do what the user
   * asked for). */
  static bool parseCommonOptions(const std::vector<std::string> &args,
                                 GlobalOptions &opts,
                                 std::filesystem::path &startPath);

  static void resolveStandardPaths(const std::string &exePathStr,
                                   const std::filesystem::path &projRoot,
                                   GlobalOptions &opts);
};

} // namespace utopia