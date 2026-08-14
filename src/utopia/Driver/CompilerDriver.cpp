#include "utopia/Driver/CompilerDriver.hpp"
#include "utopia/CodeGen/BackendContext.hpp"
#include "utopia/Common/Logger.hpp"
#include "utopia/Common/Timer.hpp"
#include "utopia/Driver/Backend.hpp"
#include "utopia/Driver/Compiler.hpp"
#include "utopia/Driver/Linker.hpp"
#include "utopia/Driver/ModuleLoader.hpp"
#include "utopia/Format/Formatter.hpp"
#include "utopia/Sema/Sema.hpp"

#include <fstream>
#include <iostream>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/TargetParser/Triple.h>
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
  modConfig.includeDirs = options.includeDirs;
  modConfig.packages = options.packages;
  modConfig.definedMacros = options.publicMacros;
  modConfig.definedMacros.insert(options.privateMacros.begin(),
                                 options.privateMacros.end());

  std::string targetTripleStr = options.targetTriple;
  if (targetTripleStr.empty()) {
    targetTripleStr = llvm::sys::getDefaultTargetTriple();
  }
  llvm::Triple triple(targetTripleStr);

  if (triple.isOSWindows()) {
    modConfig.definedMacros.insert("_WIN32");
  } else if (triple.isMacOSX()) {
    modConfig.definedMacros.insert("__APPLE__");
  } else if (triple.isAndroid() ||
             targetTripleStr.find("android") != std::string::npos) {
    modConfig.definedMacros.insert("__ANDROID__");

    if (options.sysroot.empty()) {
      const char *ndkHome = std::getenv("ANDROID_NDK_HOME");
      if (ndkHome) {
        std::string hostOS;
#if defined(_WIN32)
        hostOS = "windows-x86_64";
#elif defined(__APPLE__)
        hostOS = "darwin-x86_64";
#else
        hostOS = "linux-x86_64";
#endif
        fs::path sysrootPath = fs::path(ndkHome) / "toolchains" / "llvm" /
                               "prebuilt" / hostOS / "sysroot";
        if (fs::exists(sysrootPath)) {
          options.sysroot = sysrootPath.string();
          Logger::info("[Driver] Auto-detected Android NDK sysroot: " +
                       options.sysroot);
        }
      } else {
        Logger::warning("[Driver] Target is Android but --sysroot is missing "
                        "and ANDROID_NDK_HOME is not set.");
      }
    }
  } else if (triple.isOSLinux()) {
    modConfig.definedMacros.insert("__linux__");
    modConfig.definedMacros.insert("__gnu_linux__");
  } else if (triple.isOSFreeBSD() || triple.isOSNetBSD() ||
             triple.isOSOpenBSD()) {
    modConfig.definedMacros.insert("__BSD__");
  }

  if (triple.getArch() == llvm::Triple::x86_64) {
    modConfig.definedMacros.insert("x64");
    modConfig.definedMacros.insert("x86_64");
    modConfig.definedMacros.insert("__x86_64__");
  } else if (triple.getArch() == llvm::Triple::x86) {
    modConfig.definedMacros.insert("x86");
    modConfig.definedMacros.insert("__i386__");
  } else if (triple.getArch() == llvm::Triple::aarch64) {
    modConfig.definedMacros.insert("arm64");
    modConfig.definedMacros.insert("__aarch64__");
  } else if (triple.getArch() == llvm::Triple::arm) {
    modConfig.definedMacros.insert("arm");
    modConfig.definedMacros.insert("__arm__");
  }

  modConfig.isFormatting = options.doFormat;

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

  if (options.doFormat) {
    std::string formatted = Formatter::format(root);
    std::ofstream outFile(options.sourcePath);
    outFile << formatted;
    std::cout << "\033[1;32m[Format Success]\033[0m Formatted "
              << options.sourcePath << std::endl;
    return true;
  }

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

    /* Traverse standard explicit imports */
    for (const auto *imp : modNode->importedModules) {
      if (!self(imp, self))
        return false;
    }

    /* Traverse re-exported dependencies to guarantee backend object generation
     */
    for (const auto *exp : modNode->exportedModules) {
      if (!self(exp, self))
        return false;
    }

    fs::path unitPath = fs::absolute(std::string(modNode->filePath));
    std::string baseName = unitPath.stem().string();
    std::string unitStr = unitPath.string();

    /* Filter out external package dependencies to prevent redundant IR
     * generation. Dependencies are independently compiled and linked by the
     * build system.
     */
    bool isPackageDependency = false;
    for (const auto &[pkgName, pkgRoot] : options.packages) {
      if (pkgName == options.projectName)
        continue;

      std::string pkgStr = fs::absolute(pkgRoot).string();
      if (unitStr.find(pkgStr) == 0) {
        isPackageDependency = true;
        break;
      }
    }

    if (isPackageDependency) {
      return true;
    }

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
    if (!options.targetTriple.empty() &&
        options.targetTriple != llvm::sys::getDefaultTargetTriple()) {
      std::cerr << "\033[1;31m[Fatal]\033[0m Cannot run JIT execution for "
                   "cross-compiled targets.\n";
      return false;
    }

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

    /* Changed linker from clang++ to clang to avoid implicitly linking C++
     * standard libraries for a C-compatible language. */
    std::string compilerPath = "clang";
    std::string arPath = "llvm-ar";

    bool isAndroidTarget =
        triple.isAndroid() ||
        options.targetTriple.find("android") != std::string::npos;

    if (isAndroidTarget && !options.sysroot.empty()) {
      fs::path sysrootPath(options.sysroot);
      fs::path binDir = sysrootPath.parent_path() / "bin";

      /* Changed wrapper resolution to use clang */
      fs::path wrapperBin = binDir / (options.targetTriple + "-clang");
      if (fs::exists(wrapperBin)) {
        compilerPath = wrapperBin.string();
      } else {
        /* Changed fallback resolution to use clang */
        fs::path clangBin = binDir / "clang";
        if (fs::exists(clangBin)) {
          compilerPath = clangBin.string();
        }
      }

      fs::path arBin = binDir / "llvm-ar";
      if (fs::exists(arBin)) {
        arPath = arBin.string();
      }
    }

    std::vector<std::string> activeLinkerFlags;
    for (const auto &flag : options.linkerFlags) {
      if (isAndroidTarget && (flag == "-lpthread" || flag == "-lrt")) {
        continue;
      }
      activeLinkerFlags.push_back(flag);
    }

    if (!options.sysroot.empty()) {
      activeLinkerFlags.push_back("\"--sysroot=" + options.sysroot + "\"");

      if (isAndroidTarget) {
        fs::path sysrootPath(options.sysroot);
        fs::path binDir = sysrootPath.parent_path() / "bin";
        fs::path lldPath = binDir / "ld.lld";

        activeLinkerFlags.push_back("\"-fuse-ld=" + lldPath.string() + "\"");
        activeLinkerFlags.push_back("\"--ld-path=" + lldPath.string() + "\"");

        activeLinkerFlags.push_back("\"-B" + binDir.string() + "\"");
      }
    }

    if (options.target == "shared_library" || options.target == "shared") {
      fs::path binOut = outDir / "bin";
      if (!fs::exists(binOut))
        fs::create_directories(binOut);

#if defined(_WIN32)
      std::string ext = ".dll";
#elif defined(__APPLE__)
      std::string ext = ".dylib";
#else
      std::string ext = ".so";
#endif

      std::string libPath =
          (binOut / ("lib" + options.outputName + ext)).string();

      if (!Linker::link(compiledObjects, libPath, options.isDebug,
                        activeLinkerFlags, "shared_library", compilerPath,
                        arPath)) {
        std::cerr << "\033[1;31m[Fatal]\033[0m Linker step failed for shared "
                     "library.\n";
        return false;
      }
      Logger::info("\033[1;32m[Build Success]\033[0m " + libPath);
    } else if (options.target == "library" ||
               options.target == "static_library" ||
               options.target == "static") {
      fs::path libOut = outDir / "lib";
      if (!fs::exists(libOut))
        fs::create_directories(libOut);

      std::string ext = ".a";
#if defined(_WIN32)
      std::string ext = ".lib";
#endif

      std::string libPath =
          (libOut / ("lib" + options.outputName + ext)).string();

      if (!Linker::link(compiledObjects, libPath, options.isDebug,
                        activeLinkerFlags, "static_library", compilerPath,
                        arPath)) {
        std::cerr << "\033[1;31m[Fatal]\033[0m Linker step failed for static "
                     "library.\n";
        return false;
      }
      Logger::info("\033[1;32m[Build Success]\033[0m " + libPath);
    } else {
      fs::path binOut = outDir / "bin";
      if (!fs::exists(binOut))
        fs::create_directories(binOut);

      std::string executablePath = (binOut / options.outputName).string();

      if (!Linker::link(compiledObjects, executablePath, options.isDebug,
                        activeLinkerFlags, "executable", compilerPath,
                        arPath)) {
        std::cerr << "\033[1;31m[Fatal]\033[0m Linker step failed.\n";
        return false;
      }
      Logger::info("\033[1;32m[Build Success]\033[0m " + executablePath);
    }
  }

  return true;
}

} // namespace utopia