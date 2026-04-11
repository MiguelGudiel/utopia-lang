#include "utopia/Driver/CompilerDriver.hpp"
#include "Linker.hpp"
#include "utopia/CodeGen/CodeGen.hpp"
#include "utopia/Common/Types.hpp"
#include "utopia/Driver/BuildCache.hpp"
#include "utopia/Driver/ModuleLoader.hpp"
#include "utopia/Sema/Sema.hpp"
#include <chrono>
#include <fstream>
#include <iostream>
#include <sstream>

namespace utopia {

CompilerDriver::CompilerDriver(const CompileOptions &options)
    : options(options) {}

std::string CompilerDriver::readFile(const std::string &path) {
  std::ifstream file(path);
  std::stringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

static uint64_t getLocalFileTimestamp(const fs::path &path) {
  auto ftime = fs::last_write_time(path);
  auto sysTime = std::chrono::clock_cast<std::chrono::system_clock>(ftime);

  return std::chrono::duration_cast<std::chrono::seconds>(
             sysTime.time_since_epoch())
      .count();
}

fs::path CompilerDriver::getObjPath(const ModuleNode *mod,
                                    const fs::path &internalPath,
                                    const fs::path &projRoot,
                                    const fs::path &objDir) {
  fs::path modPath(mod->filename);
  std::error_code ec;
  fs::path relPath;

  // Standard library modules are exiled to their own 'std' subtree
  // to prevent namespace collisions with local project files.
  if (!internalPath.empty() &&
      modPath.string().find(internalPath.string()) == 0) {
    relPath = "std" / fs::relative(modPath, internalPath, ec);
  } else if (!projRoot.empty()) {
    relPath = fs::relative(modPath, projRoot, ec);

    // Modules escaping the project tree via '../' are sequestered
    // into the 'ext' directory to maintain a clean build structure.
    if (ec || relPath.string().find("..") != std::string::npos) {
      relPath = "ext" / modPath.filename();
    }
  } else {
    relPath = modPath.filename();
  }

  relPath.replace_extension(".o");
  return objDir / relPath;
}

bool CompilerDriver::run() {
  ModuleLoader loader;
  fs::path sourceDir = fs::path(options.sourcePath).parent_path();
  loader.addSearchPath(sourceDir);

  for (const auto &dir : options.includeDirs)
    loader.addSearchPath(dir);

#ifdef UTOPIA_DEBUG_BUILD
  fs::path internalPath = fs::path(UTOPIA_SOURCE_DIR) / "libs";
#else
  fs::path internalPath = UTOPIA_INTERNAL_LIB_PATH;
#endif

  if (fs::exists(internalPath)) {
    loader.setSystemPath(internalPath);
  } else {
    std::cerr << "error: standard library not found at " << internalPath
              << "\n";
    return false;
  }

  fs::path preludePath = internalPath / "std" / "prelude.utp";
  if (!fs::exists(preludePath)) {
    std::cerr << "Warning: Prelude not found at " << preludePath
              << ". Global scope will be empty.\n";
  } else {
    loader.loadModule("std:prelude", internalPath);
  }

  ModuleNode *root = loader.loadModule(options.sourcePath, sourceDir);
  if (!root) {
    std::cerr << "Failed to load main module: " << options.sourcePath << "\n";
    return false;
  }
  loader.setRootModule(root);

  Sema sema;
  if (!sema.analyzeModules(loader.getAllModules())) {
    for (auto &err : sema.getErrors()) {
      std::cerr << err.message << "\n";
    }
    return false;
  }

  fs::path buildDir = fs::path(options.outputPath).parent_path();
  fs::path objDir = buildDir / "obj";
  fs::path cacheDir = buildDir / "cache";

  fs::create_directories(objDir);
  fs::create_directories(cacheDir);

  BuildCache cache(cacheDir / "build_cache.json");
  cache.load();

  std::vector<std::string> objFiles;
  std::vector<ModuleNode *> modulesToCompile;

  fs::path projRoot = options.projectRoot.empty()
                          ? fs::current_path()
                          : fs::path(options.projectRoot);

  for (ModuleNode *mod : loader.getAllModules()) {
    fs::path objPath = getObjPath(mod, internalPath, projRoot, objDir);
    objFiles.push_back(objPath.string());

    // Ensure the nesting exists or LLVM will choke on the missing directory.
    fs::create_directories(objPath.parent_path());

    uint64_t currentTime = getLocalFileTimestamp(mod->filename);
    if (cache.isUpToDate(mod->filename, currentTime, mod->imports)) {
      std::cout << "[Cache] " << mod->filename << " up to date, skipping.\n";
    } else {
      modulesToCompile.push_back(mod);
      cache.update(mod->filename, currentTime, mod->imports);
    }
  }

  for (ModuleNode *mod : modulesToCompile) {
    fs::path objPath = getObjPath(mod, internalPath, projRoot, objDir);

    fs::path llPath = objPath;
    llPath.replace_extension(".ll");
    fs::path asmPath = objPath;
    asmPath.replace_extension(".s");

    try {
      CodeGen codegen(mod->filename, options.isDebug);
      codegen.generate(mod, objPath.string(), loader.getAllModules());

      if (options.optLevel > 0) {
        codegen.optimize(options.optLevel);
      }
      if (options.emitLLVM) {
        codegen.saveToFile(llPath.string());
      }
      if (options.emitAsm) {
        codegen.emitAssemblyFile(asmPath.string());
      }
    } catch (const std::exception &e) {
      std::cerr << "\n[Build Error] " << mod->filename << "\n"
                << "Details: " << e.what() << "\n";

      /* * CLEANUP PHASE:
       * Nuke any partial artifacts. A corrupted .o is worse than no .o at all.
       */
      auto cleanup = [](const fs::path &p) {
        if (fs::exists(p))
          fs::remove(p);
      };

      cleanup(objPath);
      if (options.emitLLVM)
        cleanup(llPath);
      if (options.emitAsm)
        cleanup(asmPath);
      return false;
    }
  }

  if (!Linker::link(objFiles, options.outputPath, options.isDebug,
                    options.linkerFlags)) {
    return false;
  }

  cache.save();
  return true;
}

} // namespace utopia