#include "Commands/Build/BuildCommand.hpp"
#include "Commands/Fmt/FmtCommand.hpp"
#include "Commands/Run/RunCommand.hpp"
#include <iostream>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/FileSystem.h>
#include <memory>
#include <unordered_map>

using namespace utopia;

void utopiaFatalErrorHandler(void *user_data, const char *reason,
                             bool gen_crash_diag) {
  std::cerr << "\033[1;31m[LLVM Fatal Error]\033[0m " << reason << std::endl;
  exit(1);
}

void printHelp(const std::unordered_map<std::string, std::shared_ptr<ICommand>>
                   &commands) {
  std::cout << "Utopia Compiler CLI\n"
            << "Usage: utopia <command> [options] [path]\n\n"
            << "Commands:\n";
  for (const auto &[name, cmd] : commands) {
    std::cout << "  " << name << " \t" << cmd->getDescription() << "\n";
  }
  std::cout << "\nOptions:\n"
            << "  --emit-llvm    Emit LLVM IR (.ll) files.\n"
            << "  --emit-asm     Emit Assembly (.s) files.\n"
            << "  --jit          Force JIT execution (default for 'run').\n"
            << "  -g, --debug    Include debug symbols.\n"
            << "  -O<level>      Set optimization level (e.g., -O3).\n"
            << "  -D<macro>      Define a public macro.\n";
}

int main(int argc, char **argv) {
  llvm::install_fatal_error_handler(utopiaFatalErrorHandler, nullptr);

  std::unordered_map<std::string, std::shared_ptr<ICommand>> commands;
  commands["build"] = std::make_shared<BuildCommand>();
  commands["run"] = std::make_shared<RunCommand>();
  commands["fmt"] = std::make_shared<FmtCommand>();

  try {
    if (argc < 2) {
      printHelp(commands);
      return 1;
    }

    std::string subCmdStr = argv[1];
    if (subCmdStr == "help" || subCmdStr == "--help" || subCmdStr == "-h") {
      printHelp(commands);
      return 0;
    }

    std::shared_ptr<ICommand> selectedCmd = nullptr;
    int argOffset = 2;

    if (commands.find(subCmdStr) != commands.end()) {
      selectedCmd = commands[subCmdStr];
    } else {
      /* Allow legacy fallback to 'build' if no valid subcommand is provided */
      selectedCmd = commands["build"];
      argOffset = 1;
    }

    std::vector<std::string> args(argv + argOffset, argv + argc);
    std::string exePathStr =
        llvm::sys::fs::getMainExecutable(argv[0], (void *)(intptr_t)&main);

    int result = selectedCmd->execute(args, exePathStr);
    llvm::remove_fatal_error_handler();
    return result;

  } catch (const std::exception &e) {
    std::cerr << "\033[1;31m[Utopia Runtime Error]\033[0m " << e.what()
              << std::endl;
    return 1;
  } catch (...) {
    std::cerr << "[Unknown Fatal Error] Utopia se cerró inesperadamente."
              << std::endl;
    return 1;
  }
}