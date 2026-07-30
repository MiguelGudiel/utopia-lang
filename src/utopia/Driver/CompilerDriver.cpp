#include "utopia/Driver/CompilerDriver.hpp"
#include "utopia/CodeGen/BackendContext.hpp"
#include "utopia/Common/Logger.hpp"
#include "utopia/Common/Timer.hpp"
#include "utopia/Driver/Backend.hpp"
#include "utopia/Driver/Compiler.hpp"
#include "utopia/Driver/Linker.hpp"
#include "utopia/Driver/ModuleLoader.hpp"
#include "utopia/Sema/Sema.hpp"

#include <iostream>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/Support/TargetSelect.h>
#include <unordered_set>

namespace utopia {

CompilerDriver::CompilerDriver(const CompileOptions &options)
    : options(options) {}

fs::path CompilerDriver::getObjPath(const std::string &filename,
                                    const fs::path &internalPath,
                                    const fs::path &projRoot,
                                    const fs::path &objDir) {
  fs::path relPath;
  std::string targetProjectName;

  fs::path absInternal = fs::absolute(internalPath);
  fs::path absProj = fs::absolute(projRoot);
  fs::path absStdlib = fs::absolute(options.stdlibRoot);
  fs::path absPrelude = fs::absolute(options.preludeRoot);

  std::string internalStr = absInternal.string();
  std::string projStr = absProj.string();
  std::string stdlibStr = absStdlib.string();
  std::string preludeStr = absPrelude.string();

  if (internalStr.find(projStr) == 0) {
    relPath = fs::relative(absInternal, absProj);
    targetProjectName = options.projectName;
  } else if (internalStr.find(stdlibStr) == 0) {
    relPath = fs::relative(absInternal, absStdlib);
    targetProjectName = "stdlib";
  } else if (internalStr.find(preludeStr) == 0) {
    relPath = fs::relative(absInternal, absPrelude);
    targetProjectName = "prelude";
  } else {
    relPath = absInternal.lexically_relative(fs::current_path());
    targetProjectName = "external";

    std::string sanitized = relPath.string();
    size_t pos;
    while ((pos = sanitized.find("../")) != std::string::npos) {
      sanitized.replace(pos, 3, "__/");
    }
    relPath = fs::path(sanitized);
  }

  fs::path targetDir =
      objDir / "obj" / targetProjectName / relPath.parent_path();
  if (!fs::exists(targetDir)) {
    fs::create_directories(targetDir);
  }

  fs::path finalPath = targetDir / filename;
  Logger::debug("[Driver Debug] Mapped artifact base path: " +
                finalPath.string());
  return finalPath;
}

bool CompilerDriver::run() {
  fs::path outDir(options.outputDir);
  if (!fs::exists(outDir)) {
    fs::create_directories(outDir);
  }

  Logger::debug("[Driver] Starting compilation for: " + options.sourcePath);

  fs::path entryPath = fs::absolute(options.sourcePath);
  std::string entryStr = entryPath.string();

  DiagnosticsEngine diagEngine;
  ASTContext astCtx;
  ModuleNode *root = nullptr;

  ModuleLoaderConfig modConfig;
  modConfig.projectRoot = options.projectRoot;
  modConfig.stdlibRoot = options.stdlibRoot;
  modConfig.preludeRoot = options.preludeRoot;

  ModuleLoader loader(astCtx, modConfig, diagEngine);

  {
    ScopedTimer timer("Lexer + Parser + Module Loading");
    root = loader.loadModule(entryStr);
  }

  if (!root || diagEngine.hasErrors()) {
    std::cerr << "[Fatal] Syntax or import errors found." << std::endl;
    return false;
  }

  Logger::debug("[Driver] AST generated successfully.");

  {
    ScopedTimer timer("Semantic Analysis Pipeline");
    SemaContext semaCtx(astCtx, diagEngine, entryStr);
    SemaPipeline pipeline;

    if (!pipeline.run(root, semaCtx) || diagEngine.hasErrors()) {
      std::cerr << "[Fatal] Semantic errors found." << std::endl;
      return false;
    }
    Logger::debug("[Driver] Semantic Analysis passed.");
  }

  BackendContext backendCtx;
  std::vector<std::string> compiledObjects;
  std::unordered_set<const ModuleNode *> compiledModules;
  fs::path projRootFs(options.projectRoot);

  auto compileTranslationUnit = [&](const ModuleNode *modNode,
                                    auto &self) -> bool {
    if (compiledModules.contains(modNode))
      return true;
    compiledModules.insert(modNode);

    for (const auto *imp : modNode->importedModules) {
      if (!self(imp, self))
        return false;
    }

    fs::path unitPath = fs::absolute(std::string(modNode->filePath));
    std::string baseName = unitPath.stem().string();

    Logger::debug("[Driver Debug] Starting IR generation for unit: " +
                  baseName);

    llvm::Module *llvmMod =
        Compiler::compileToIR(const_cast<ModuleNode *>(modNode), backendCtx,
                              unitPath.string(), diagEngine, options.isDebug);

    if (!llvmMod || diagEngine.hasErrors()) {
      std::cerr << "\033[1;31m[Fatal]\033[0m Compilation aborted for "
                   "translation unit: "
                << baseName << "." << std::endl;
      return false;
    }

    fs::path targetBasePath =
        getObjPath(baseName, unitPath, projRootFs, outDir);

    Logger::debug("[Driver Debug] Executing backend passes for unit: " +
                  baseName);

    if (!Backend::process(llvmMod, backendCtx, options,
                          targetBasePath.string())) {
      std::cerr << "\033[1;31m[Fatal]\033[0m Backend processing failed for "
                   "translation unit: "
                << baseName << "." << std::endl;
      return false;
    }

    compiledObjects.push_back(targetBasePath.string() + ".o");
    return true;
  };

  {
    ScopedTimer timer("IR Generation & Code Emission");
    if (!compileTranslationUnit(root, compileTranslationUnit)) {
      return false;
    }
  }

  if (options.isJIT) {
    ScopedTimer timer("JIT Execution Engine");
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    llvm::InitializeNativeTargetAsmParser();

    auto jitEx = llvm::orc::LLJITBuilder().create();
    if (!jitEx) {
      std::cerr
          << "\033[1;31m[JIT Error]\033[0m Failed to spin up execution engine."
          << std::endl;
      return false;
    }
    auto jit = std::move(*jitEx);

    /* Transfer global LLVMContext ownership to thread-safe architecture */
    auto uniqueCtx = backendCtx.takeContext();
    llvm::orc::ThreadSafeContext tsc(std::move(uniqueCtx));

    for (const auto *modNode : compiledModules) {
      fs::path unitPath = fs::absolute(std::string(modNode->filePath));
      auto uniqueMod = backendCtx.takeModule(unitPath.string());

      if (uniqueMod) {
        auto tsm = llvm::orc::ThreadSafeModule(std::move(uniqueMod), tsc);
        if (auto err = jit->addIRModule(std::move(tsm))) {
          std::cerr
              << "\033[1;31m[JIT Error]\033[0m IR linkage failure for module: "
              << unitPath.string() << std::endl;
          return false;
        }
      }
    }

    auto mainSym = jit->lookup("main");
    if (!mainSym) {
      std::cerr << "\033[1;31m[JIT Error]\033[0m Entry point symbol 'main' not "
                   "resolved."
                << std::endl;
      return false;
    }

    int (*mainFn)() = mainSym->toPtr<int (*)()>();
    int result = mainFn();

    Logger::debug("\033[1;32m[JIT Execution Finished]\033[0m Exit code: " +
                  std::to_string(result));
  } else {
    ScopedTimer timer("Linking");

    if (options.target == "library") {
      if (!fs::exists(outDir))
        fs::create_directories(outDir);

#if defined(_WIN32)
      std::string ext = ".dll";
#elif defined(__APPLE__)
      std::string ext = ".dylib";
#else
      std::string ext = ".so";
#endif

      std::string libPath =
          (outDir / ("lib" + options.projectName + ext)).string();
      std::vector<std::string> libFlags = options.linkerFlags;
      libFlags.push_back("-shared");

      if (!Linker::link(compiledObjects, libPath, options.isDebug, libFlags)) {
        std::cerr
            << "\033[1;31m[Fatal]\033[0m Linker step failed for library.\n";
        return false;
      }
      Logger::info("\033[1;32m[Build Success]\033[0m " + libPath);
    } else {
      fs::path binOut = outDir / "bin";
      if (!fs::exists(binOut))
        fs::create_directories(binOut);

      std::string executablePath = (binOut / options.projectName).string();

      if (!Linker::link(compiledObjects, executablePath, options.isDebug,
                        options.linkerFlags)) {
        std::cerr << "\033[1;31m[Fatal]\033[0m Linker step failed.\n";
        return false;
      }
      Logger::info("\033[1;32m[Build Success]\033[0m " + executablePath);
    }
  }

  return true;
}

} // namespace utopia