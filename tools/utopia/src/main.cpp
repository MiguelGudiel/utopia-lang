#include "ProjectManager.hpp"
#include "utopia/Driver/CompilerDriver.hpp"
#include <chrono>
#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;

void printHelp() {
  std::cout << "Utopia Compiler v0.1.0\n"
            << "Uso: utopia <file.utp> [options]\n\n"
            << "Opciones:\n"
            << "  -o <path>    Especificar nombre del ejecutable.\n"
            << "  -O[0-3]      Nivel de optimización.\n"
            << "  --run        Ejecutar tras compilar.\n"
            << "  --emit-llvm   Guardar archivo LLVM IR (.ll)\n"
            << "  --emit-asm    Guardar archivo ensamblador (.s)\n";
}

int main(int argc, char **argv) {
  if (argc < 2) {
    printHelp();
    return 1;
  }

  try {
    utopia::CompileOptions opts;
    bool shouldRun = false;
    bool optOverride = false;

    for (int i = 1; i < argc; ++i) {
      std::string arg = argv[i];
      if (arg == "-o" && i + 1 < argc)
        opts.outputPath = argv[++i];
      else if (arg == "--run")
        shouldRun = true;
      else if (arg.substr(0, 2) == "-O") {
        opts.optLevel = std::stoi(arg.substr(2));
        optOverride = true;
      } else if (opts.sourcePath.empty())
        opts.sourcePath = arg;
      else if (arg == "--emit-llvm")
        opts.emitLLVM = true;
      else if (arg == "--emit-asm")
        opts.emitAsm = true;
    }

    fs::path projectRoot = utopia::findProjectRoot(opts.sourcePath);
    utopia::ProjectConfig config;

    if (!projectRoot.empty()) {
      opts.projectRoot = projectRoot.string();
      config = utopia::parseBuildManifest(projectRoot / "build.yaml");
      opts.includeDirs = config.includeDirs;
      opts.linkerFlags = config.linkerFlags;

      fs::path inputPath(opts.sourcePath);
      if (inputPath.extension() == ".yaml" || inputPath.extension() == ".yml") {
        if (!config.resolvedSources.empty()) {
          opts.sourcePath = config.resolvedSources[0].path;
        } else {
          throw std::runtime_error("Manifest does not define source files.");
        }
      }

      if (!optOverride)
        opts.optLevel = config.optLevel;
    }

    opts.isDebug = (opts.optLevel == 0);

    if (opts.outputPath.empty()) {
      fs::path base = projectRoot.empty()
                          ? fs::path(opts.sourcePath).parent_path()
                          : projectRoot;
      std::string subDir =
          config.outputDir.empty() ? "build" : config.outputDir;
      std::string binName = config.name.empty()
                                ? fs::path(opts.sourcePath).stem().string()
                                : config.name;
      opts.outputPath = (base / subDir / binName).string();
    }

    fs::path outDir = fs::path(opts.outputPath).parent_path();
    if (!outDir.empty() && !fs::exists(outDir)) {
      fs::create_directories(outDir);
    }

    auto start = std::chrono::high_resolution_clock::now();

    utopia::CompilerDriver driver(opts);
    if (!driver.run())
      return 1;

    auto end = std::chrono::high_resolution_clock::now();
    auto diff = std::chrono::duration<double, std::milli>(end - start).count();

    std::cout << "[Success] Project built in " << diff << " ms\n";

    if (shouldRun) {
      std::string cmd = opts.outputPath;
      if (!fs::path(cmd).is_absolute())
        cmd = "./" + cmd;
      cmd = "\"" + cmd + "\"";
      return std::system(cmd.c_str());
    }

  } catch (const std::exception &e) {
    std::cerr << "[Fatal Error] " << e.what() << "\n";
    return 1;
  }

  return 0;
}