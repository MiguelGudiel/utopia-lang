#include "Linker.hpp"
#include <array>
#include <iostream>

namespace utopia {

bool Linker::link(const std::vector<std::string> &objPaths,
                  const std::string &outPath, bool debug,
                  const std::vector<std::string> &linkerFlags) {
  std::string cmd = "clang ";
  if (debug)
    cmd += "-g ";

  for (const auto &flag : linkerFlags) {
    cmd += flag + " ";
  }

  for (const auto &obj : objPaths) {
    cmd += obj + " ";
  }
  cmd += "-o " + outPath;

  std::string output = executeAndCapture(cmd);
  if (output.find("error:") != std::string::npos) {
    std::cerr << "[Linker Error]\n" << output << "\n";
    return false;
  }
  return true;
}

std::string Linker::executeAndCapture(const std::string &cmd) {
  std::array<char, 128> buffer;
  std::string result;
  FILE *pipe = popen((cmd + " 2>&1").c_str(), "r");
  if (!pipe)
    return "Failed to start linker process";
  while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
    result += buffer.data();
  }
  pclose(pipe);
  return result;
}

} // namespace utopia