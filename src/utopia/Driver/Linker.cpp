#include "utopia/Driver/Linker.hpp"
#include <array>
#include <iostream>

#ifndef _WIN32
#include <sys/wait.h>
#endif

namespace utopia {

bool Linker::link(const std::vector<std::string> &objPaths,
                  const std::string &outPath, bool debug,
                  const std::vector<std::string> &linkerFlags,
                  const std::string &targetType,
                  const std::string &compilerPath, const std::string &arPath) {
  std::string cmd;

  if (targetType == "static_library") {
    cmd = "\"" + arPath + "\" rcs \"" + outPath + "\" ";
    for (const auto &obj : objPaths) {
      cmd += "\"" + obj + "\" ";
    }
  } else {
    cmd = "\"" + compilerPath + "\" ";
    if (debug)
      cmd += "-g ";

    if (targetType == "shared_library" || targetType == "library") {
      cmd += "-shared ";
    }

    /* Object files must precede linker flags and static libraries
     * to guarantee proper symbol resolution in single-pass linkers. */
    for (const auto &obj : objPaths) {
      cmd += "\"" + obj + "\" ";
    }

    for (const auto &flag : linkerFlags) {
      cmd += flag + " ";
    }
    cmd += "-o \"" + outPath + "\"";
  }

  std::array<char, 128> buffer;
  std::string output;

  FILE *pipe = popen((cmd + " 2>&1").c_str(), "r");
  if (!pipe) {
    std::cerr << "[Linker Error] Failed to invoke linker process.\n";
    return false;
  }

  while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
    output += buffer.data();
  }

  int status = pclose(pipe);

  /* Termination status resolution.
   * On POSIX-compliant systems, pclose() returns the full termination status
   * as defined by wait4() rather than a raw exit code. Direct comparison
   * against zero is unreliable as it may fail to detect abnormal termination
   * or signal interference. We use WEXITSTATUS to guarantee we are evaluating
   * the actual return code from the linker process.
   */
#ifdef _WIN32
  int exitCode = status;
#else
  int exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : status;
#endif

  if (exitCode != 0 || output.find("error:") != std::string::npos) {
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

  int status = pclose(pipe);
#ifndef _WIN32
  (void)WIFEXITED(status);
#endif

  return result;
}

} // namespace utopia