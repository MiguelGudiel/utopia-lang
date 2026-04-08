#include "utopia/Driver/CompilerDriver.hpp"
#include "Linker.hpp"
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
  return std::chrono::duration_cast<std::chrono::seconds>(
             ftime.time_since_epoch())
      .count();
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
  ModuleNode *prelude = nullptr;

  if (fs::exists(preludePath)) {
    prelude = loader.loadModule("std:prelude", internalPath);
  } else {
    // If emptiness doesn't exist, we can't shout at it
    // Is the installation corrupted, or is the developer playing God?
    std::cerr << "Warning: Prelude not found at " << preludePath
              << ". Global scope will be empty.\n";
  }

  ModuleNode *root = loader.loadModule(options.sourcePath, sourceDir);
  if (!root) {
    std::cerr << "Failed to load main module: " << options.sourcePath << "\n";
    return false;
  }
  loader.setRootModule(root);

  std::cerr << "All modules (" << loader.getAllModules().size() << "):\n";
  for (auto m : loader.getAllModules())
    std::cerr << "  " << m->filename << "\n";

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
    fs::path modPath(mod->filename);
    std::error_code ec;
    fs::path relPath;

    // Magia negra para resolver rutas y evitar que los objetos se maten entre
    // sí. Si el módulo está dentro de la librería estándar, lo movemos a su
    // propia subestructura.
    if (!internalPath.empty() &&
        modPath.string().find(internalPath.string()) == 0) {
      relPath = "std" / fs::relative(modPath, internalPath, ec);
    }
    // Si está dentro del proyecto, replicamos su estructura relativa al root.
    else if (!projRoot.empty()) {
      relPath = fs::relative(modPath, projRoot, ec);

      // Si por algún milagro la ruta se escapa del árbol del proyecto (..),
      // la mandamos al exilio en 'ext' para no contaminar el namespace local.
      if (ec || relPath.string().find("..") != std::string::npos) {
        relPath = "ext" / modPath.filename();
      }
    } else {
      relPath = modPath.filename();
    }

    relPath.replace_extension(".o");
    fs::path objPath = objDir / relPath;

    // Nos aseguramos de que el nido para el .o exista, o LLVM se pondrá a
    // llorar.
    fs::create_directories(objPath.parent_path());
    objFiles.push_back(objPath.string());

    uint64_t currentTime = getLocalFileTimestamp(modPath);

    if (cache.isUpToDate(mod->filename, currentTime, mod->imports)) {
      std::cout << "[Cache] " << mod->filename << " up to date, skipping.\n";
    } else {
      modulesToCompile.push_back(mod);
      cache.update(mod->filename, currentTime, mod->imports);
    }
  }

  std::cerr << "Modules to compile: " << modulesToCompile.size() << "\n";
  for (auto m : modulesToCompile)
    std::cerr << "  " << m->filename << "\n";

  for (ModuleNode *mod : modulesToCompile) {
    fs::path modPath(mod->filename);
    std::error_code ec;
    fs::path relPath;

    if (!internalPath.empty() &&
        modPath.string().find(internalPath.string()) == 0) {
      relPath = "std" / fs::relative(modPath, internalPath, ec);
    } else if (!projRoot.empty()) {
      relPath = fs::relative(modPath, projRoot, ec);
      if (ec || relPath.string().find("..") != std::string::npos) {
        relPath = "ext" / modPath.filename();
      }
    } else {
      relPath = modPath.filename();
    }

    relPath.replace_extension(".o");
    fs::path objPath = objDir / relPath;

    CodeGen codegen(mod->filename, options.isDebug);
    try {
      codegen.generate(mod, objPath.string(), loader.getAllModules());

      if (options.optLevel > 0) {
        codegen.optimize(options.optLevel);
      }
    } catch (const std::exception &e) {
      std::cerr << "Exception: " << e.what() << "\n";
      return false;
    } catch (...) {
      std::cerr << "Unknown exception during codegen for " << mod->filename
                << "\n";
      return false;
    }

    fs::path basePath = objPath;
    basePath.replace_extension(); // remove .o

    if (options.emitLLVM) {
      fs::path llPath = basePath;
      llPath.replace_extension(".ll");
      codegen.saveToFile(llPath.string());
    }
    if (options.emitAsm) {
      fs::path asmPath = basePath;
      asmPath.replace_extension(".s");
      codegen.emitAssemblyFile(asmPath.string());
    }
  }

  // Delegamos el linkeo directamente desde aquí.
  if (!Linker::link(objFiles, options.outputPath, options.isDebug,
                    options.linkerFlags)) {
    return false;
  }

  cache.save();
  return true;
}

} // namespace utopia