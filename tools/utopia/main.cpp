#include "utopia/CodeGen/CodeGen.hpp"
#include "utopia/Lexer/Lexer.hpp"
#include "utopia/Parser/Parser.hpp"
#include "utopia/Sema/Sema.hpp"
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

const std::string PROJECT_MANIFEST = "build.yaml";

std::string readFile(const std::string &filepath) {
  std::ifstream file(filepath);
  if (!file.is_open()) {
    std::cerr << "Error: No se pudo abrir el archivo '" << filepath << "'\n";
    exit(1);
  }
  std::stringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

fs::path findProjectRoot(fs::path startDir) {
  fs::path current = fs::absolute(startDir);

  while (true) {
    if (fs::exists(current / PROJECT_MANIFEST)) {
      return current;
    }
    if (!current.has_parent_path() || current == current.root_path()) {
      break;
    }
    current = current.parent_path();
  }
  return "";
}

void printHelp() {
  std::cout << "Utopia Compiler v0.1.0\n"
            << "Usage: utopia <file.utp> [options]\n\n"
            << "Options:\n"
            << "  -o <path>      Specify the output executable name and path.\n"
            << "                 If not specified, it will be created in a "
               "'build' folder.\n"
            << "  --help         Show this help message.\n";
}

std::string executeAndCapture(const std::string &cmd) {
  std::array<char, 256> buffer;
  std::string result;

  std::string fullCmd = cmd + " 2>&1";
  FILE *pipe = popen(fullCmd.c_str(), "r");

  if (!pipe) {
    return "Error: Failed to run command";
  }

  while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
    result += buffer.data();
  }

  int exitCode = pclose(pipe);

  // Si el comando falló, agregar el código de salida al resultado
  if (exitCode != 0) {
    result += "\n[Command exited with code: " + std::to_string(exitCode) + "]";
  }

  return result;
}

int main(int argc, char **argv) {
  if (argc < 2) {
    std::cout << "Utopia Compiler v0.1.0\nUso: utopia <file.utp> [-o output]\n";
    return 1;
  }

  std::string sourceFilePath;
  std::string outputPath;

  bool shouldRun = false;
  int optLevel = 0;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "-o" && i + 1 < argc) {
      outputPath = argv[++i];
    } else if (arg == "--run") {
      shouldRun = true;
    } else if (sourceFilePath.empty()) {
      sourceFilePath = arg;
    } else if (arg == "-O0") {
      optLevel = 0;
    } else if (arg == "-O1") {
      optLevel = 1;
    } else if (arg == "-O2") {
      optLevel = 2;
    } else if (arg == "-O3") {
      optLevel = 3;
    }
  }

  fs::path srcPath = fs::absolute(sourceFilePath);
  if (!fs::exists(srcPath)) {
    std::cerr << "Error: Source file does not exist.\n";
    return 1;
  }

  // Project Logic
  fs::path projectRoot = findProjectRoot(srcPath.parent_path());

  if (outputPath.empty()) {
    std::string baseName = srcPath.stem().string();
    fs::path baseDir;

    if (!projectRoot.empty()) {
      baseDir = projectRoot / "build";
      std::cout << "[Project] Root detected at: " << projectRoot << "\n";
    } else {
      baseDir = srcPath.parent_path() / "build";
    }
    outputPath = (baseDir / baseName).string();
  }

  fs::path finalOutPath(outputPath);
  fs::create_directories(finalOutPath.parent_path());

  fs::path objPath = finalOutPath;
  objPath.replace_extension(".o");
  fs::path llPath = finalOutPath;
  llPath.replace_extension(".ll");

  std::string sourceCode = readFile(srcPath.string());
  std::cout << "--- Utopia Compiler v0.1.0 ---\n";

  auto startTotal = std::chrono::high_resolution_clock::now();

  auto startCompile = std::chrono::high_resolution_clock::now();
  try {
    utopia::Lexer lexer(sourceCode);
    auto tokens = lexer.tokenize();

    utopia::Parser parser(tokens);
    auto mainAst = parser.parseProgram();

    utopia::Sema sema;

    if (!sema.analyze(mainAst.get())) {
      std::cerr << "\nCompilation failed due to semantic errors:\n";
      for (const auto &err : sema.getErrors()) {
        std::cerr << sourceFilePath << ":" << err.line << ":" << err.col << ": "
                  << err.message << "\n";
      }
      return 1;
    }

    std::cout << "[CodeGen] Generating LLVM IR...\n";
    bool isDebug = (optLevel == 0);
    utopia::CodeGen codegen(srcPath.string(), isDebug);
    codegen.generate(mainAst.get());
    codegen.optimize(optLevel);
    codegen.saveToFile(llPath.string());
    codegen.emitObjectFile(objPath.string());
  } catch (const std::exception &e) {
    std::cerr << "\nfatal error: " << e.what() << "\ncompilation terminated.\n";
    return 1;
  }
  auto endCompile = std::chrono::high_resolution_clock::now();

  std::cout << "[CodeGen] Linking with Clang...\n";
  auto startLink = std::chrono::high_resolution_clock::now();

  std::string linkCommand =
      "clang -g " + objPath.string() + " -o " + finalOutPath.string();
  std::cout << "[CodeGen] Linking command: " << linkCommand << "\n";
  std::string linkOutput = executeAndCapture(linkCommand);

  // Verificar si el ejecutable existe Y si el comando tuvo éxito
  bool linkSuccess = std::filesystem::exists(finalOutPath);

  // También verificar si la salida contiene errores típicos del linker
  bool hasLinkError =
      linkOutput.find("error:") != std::string::npos ||
      linkOutput.find("undefined reference") != std::string::npos ||
      linkOutput.find("duplicate symbol") != std::string::npos;

  if (!linkSuccess || hasLinkError) {
    std::cerr << "\n========================================\n";
    std::cerr << "[Linker Error] Failed to create executable\n";
    std::cerr << "========================================\n";
    std::cerr << "Command: " << linkCommand << "\n\n";

    if (!linkOutput.empty()) {
      std::cerr << "Linker output:\n";
      std::cerr << "----------------------------------------\n";
      std::cerr << linkOutput;
      std::cerr << "----------------------------------------\n";
    }

    auto endLink = std::chrono::high_resolution_clock::now();
    auto dLink =
        std::chrono::duration<double, std::milli>(endLink - startLink).count();
    std::cerr << "\n[Timer] Linking failed after: " << dLink << " ms\n";

    return 1; // ← DETENER LA EJECUCIÓN AQUÍ
  }

  auto endLink = std::chrono::high_resolution_clock::now();
  auto endTotal = std::chrono::high_resolution_clock::now();

  auto dComp =
      std::chrono::duration<double, std::milli>(endCompile - startCompile)
          .count();
  auto dLink =
      std::chrono::duration<double, std::milli>(endLink - startLink).count();
  auto dTotal =
      std::chrono::duration<double, std::milli>(endTotal - startTotal).count();

  std::cout << "-------------------------------------\n";
  std::cout << "[Timer] Utopia Compilation: " << dComp << " ms\n";
  std::cout << "[Timer] Clang Linking:      " << dLink << " ms\n";
  std::cout << "[Timer] Total Time:         " << dTotal << " ms\n";
  std::cout << "--- Executable: " << finalOutPath.string() << " ---\n";

  if (shouldRun) {
    std::cout << "\n--- Running program ---\n";
    std::string execCommand = finalOutPath.string();

    if (!finalOutPath.is_absolute()) {
      execCommand = "./" + execCommand;
    }

    auto startRun = std::chrono::high_resolution_clock::now();

    int exitCode = std::system(execCommand.c_str());

    auto endRun = std::chrono::high_resolution_clock::now();

    auto dRun =
        std::chrono::duration<double, std::milli>(endRun - startRun).count();

    std::cout << "---------------------------\n";
    std::cout << "[Utopia] Program finished (code: " << WEXITSTATUS(exitCode)
              << ")\n";
    std::cout << "[Timer] Execution time: " << dRun << " ms\n";
  }

  return 0;
}