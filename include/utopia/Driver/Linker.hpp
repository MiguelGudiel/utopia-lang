#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace utopia {

class Linker {
public:
  static bool link(const std::vector<std::string> &objPaths,
                   const std::string &outPath, bool debug,
                   const std::vector<std::string> &linkerFlags,
                   const std::string &targetType = "executable");

private:
  static std::string executeAndCapture(const std::string &cmd);
};

} // namespace utopia