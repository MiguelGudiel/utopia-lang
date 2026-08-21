#include "Core/EnvLoader.hpp"
#include <cstdlib>
#include <fstream>
#include <iostream>

#ifdef _WIN32
#include <windows.h>
#endif

namespace utopia {

static void trim(std::string &s) {
  s.erase(0, s.find_first_not_of(" \t\r\n"));
  s.erase(s.find_last_not_of(" \t\r\n") + 1);
}

void EnvLoader::load(const std::filesystem::path &envPath) {
  std::ifstream file(envPath);
  if (!file.is_open()) {
    /* A missing file is a normal configuration state (debug.env is
     * optional), but an existing file that cannot be read hides variables
     * the build may depend on (e.g. YIP_BASE_URL). */
    if (std::filesystem::exists(envPath)) {
      std::cerr << "[Warning] Environment file " << envPath
                << " exists but cannot be read.\n";
    }
    return;
  }

  std::string line;
  while (std::getline(file, line)) {
    trim(line);
    if (line.empty() || line[0] == '#') {
      continue;
    }

    auto delimiterPos = line.find('=');
    if (delimiterPos == std::string::npos) {
      std::cerr << "[Warning] Ignoring malformed line in " << envPath
                << ": missing '='.\n";
      continue;
    }

    std::string key = line.substr(0, delimiterPos);
    std::string value = line.substr(delimiterPos + 1);

    trim(key);
    trim(value);

    if (!value.empty() && value.front() == '"' && value.back() == '"') {
      value = value.substr(1, value.length() - 2);
    }

    if (std::getenv(key.c_str()) == nullptr) {
#ifdef _WIN32
      _putenv_s(key.c_str(), value.c_str());
#else
      setenv(key.c_str(), value.c_str(), 0);
#endif
    }
  }
}

std::string EnvLoader::get(const std::string &key,
                           const std::string &defaultValue) {
  const char *val = std::getenv(key.c_str());
  return val ? std::string(val) : defaultValue;
}

} // namespace utopia