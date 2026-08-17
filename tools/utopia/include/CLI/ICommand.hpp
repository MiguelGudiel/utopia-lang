#pragma once
#include <string>
#include <vector>

namespace utopia {

class ICommand {
public:
  virtual ~ICommand() = default;
  virtual std::string getName() const = 0;
  virtual std::string getDescription() const = 0;
  virtual int execute(const std::vector<std::string> &args,
                      const std::string &executablePath) = 0;
};

} // namespace utopia