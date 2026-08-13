#pragma once
#include "CLI/ICommand.hpp"
#include <memory>
#include <unordered_map>

namespace utopia {

class YipCommand : public ICommand {
public:
  YipCommand();
  std::string getName() const override;
  std::string getDescription() const override;
  int execute(const std::vector<std::string> &args,
              const std::string &executablePath) override;

private:
  void printYipHelp() const;
  int handleGet(const std::vector<std::string> &args);
  int handleAdd(const std::vector<std::string> &args);
  int handlePublish(const std::vector<std::string> &args);
  int handleLogin(const std::vector<std::string> &args);

  std::string getToken() const;

  std::unordered_map<std::string, std::string> m_subcommandsHelp;
};

} // namespace utopia