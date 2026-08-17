#pragma once
#include "CLI/ICommand.hpp"

namespace utopia {

class FmtCommand : public ICommand {
public:
  std::string getName() const override;
  std::string getDescription() const override;
  int execute(const std::vector<std::string> &args,
              const std::string &executablePath) override;
};

} // namespace utopia