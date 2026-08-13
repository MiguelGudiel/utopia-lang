#pragma once
#include <filesystem>
#include <string>

namespace utopia {

class EnvLoader {
public:
  static void load(const std::filesystem::path &envPath);
  static std::string get(const std::string &key,
                         const std::string &defaultValue = "");
};

} // namespace utopia