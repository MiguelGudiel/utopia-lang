#include "utopia/Driver/CompilerDriver.hpp"
#include "utopia/CodeGen/BackendContext.hpp"
#include "utopia/Common/Logger.hpp"
#include "utopia/Common/Timer.hpp"
#include "utopia/Common/Warnings.hpp"
#include "utopia/Driver/Backend.hpp"
#include "utopia/Driver/Compiler.hpp"
#include "utopia/Driver/Linker.hpp"
#include "utopia/Driver/ModuleLoader.hpp"
#include "utopia/Format/Formatter.hpp"
#include "utopia/Sema/Sema.hpp"

#include <fstream>
#include <sstream>
#include <iostream>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/TargetParser/Triple.h>
#include <mutex>
#include <regex>
#include <unordered_map>
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

  DiagnosticsEngine diagEngine;
  ASTContext astCtx;

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

  /* Async support is on by default; when enabled, the prelude's Future
   * class is compiled in and the async runtime is linked. */
  if (options.asyncEnabled) {
    modConfig.definedMacros.insert("UTOPIA_ASYNC");
  }

  ModuleLoader loader(astCtx, modConfig, diagEngine);

  /* Load every resolved source as a root translation unit. Previously
   * only the first source was compiled; additional roots were silently
   * dropped unless reachable through imports from the first one. */
  std::vector<ModuleNode *> roots;
  {
    ScopedTimer timer("Lexer + Parser + Module Loading");
    std::vector<std::string> entryFiles = options.resolvedSources;
    if (entryFiles.empty())
      entryFiles.push_back(options.sourcePath);

    std::unordered_set<std::string> seenEntries;
    for (const auto &entry : entryFiles) {
      std::string absEntry = fs::absolute(entry).string();
      if (!seenEntries.insert(absEntry).second)
        continue;

      ModuleNode *m = loader.loadModule(absEntry);
      if (!m)
        continue;
      roots.push_back(m);
    }
  }

  if (roots.empty() || diagEngine.hasErrors()) {
    std::cerr << "[Fatal] Syntax or import errors found." << std::endl;
    return false;
  }

  Logger::debug("[Driver] AST generated successfully.");

  if (options.doFormat) {
    /* Format every root translation unit: 'roots' are deduplicated by
     * absolute path, so each resolved source is written exactly once. */
    for (const ModuleNode *root : roots) {
      std::string original;
      {
        std::ifstream inFile(std::string(root->filePath), std::ios::binary);
        if (inFile) {
          std::stringstream buffer;
          buffer << inFile.rdbuf();
          original = buffer.str();
        }
      }
      std::string formatted = Formatter::format(root);

      /* An empty result for a non-empty source means the formatter hit a
       * construct it cannot render: writing it would erase the user's
       * file, so refuse and report instead. */
      if (formatted.empty() && !original.empty()) {
        std::cerr << "\033[1;31m[Format Error]\033[0m Formatter produced no "
                     "output for: "
                  << root->filePath
                  << " (unsupported construct); file was left unchanged."
                  << std::endl;
        return false;
      }

      std::ofstream outFile(std::string(root->filePath));
      if (!outFile) {
        std::cerr << "\033[1;31m[Format Error]\033[0m Cannot open file for "
                     "writing: "
                  << root->filePath << std::endl;
        return false;
      }
      outFile << formatted;
      outFile.close();
      if (!outFile) {
        std::cerr << "\033[1;31m[Format Error]\033[0m Failed to write "
                     "formatted source to: "
                  << root->filePath << std::endl;
        return false;
      }
      std::cout << "\033[1;32m[Format Success]\033[0m Formatted "
                << root->filePath << std::endl;
    }
    return true;
  }

  {
    ScopedTimer timer("Semantic Analysis Pipeline");
    /* A single SemaContext and pipeline are shared across all roots: the
     * passes track visited modules internally, so each module is type-checked
     * exactly once even when roots share imports. (Running a fresh pipeline
     * per root is not idempotent: the first run mutates the AST — e.g. it
     * reorders call arguments and clears named-argument bookkeeping — which
     * breaks overload resolution on a second pass over the same module.) */
    SemaContext semaCtx(astCtx, diagEngine, roots.front()->filePath);
    SemaPipeline pipeline;

    /* 'assert' mirrors C/C++: it compiles to a no-op when NDEBUG is among
     * the compiler-provided macros (release builds). */
    semaCtx.ndebugEnabled =
        options.publicMacros.contains("NDEBUG") ||
        options.privateMacros.contains("NDEBUG");

    /* Warning kinds disabled by the project manifest. */
    for (const auto &name : options.disabledWarnings) {
      semaCtx.warningConfig.disable(name);
    }

    /* In-source suppression directives ('// @ignore-warning ...'): the
     * driver reads each analyzed file once and caches the directives. */
    std::mutex suppressionMutex;
    std::unordered_map<std::string, WarningSuppressions> suppressionCache;
    semaCtx.warningFilter = [&](WarningKind kind, std::string_view filePath,
                                int line) {
      std::string key(filePath);
      {
        std::lock_guard<std::mutex> lock(suppressionMutex);
        auto it = suppressionCache.find(key);
        if (it == suppressionCache.end()) {
          std::ifstream file(key);
          std::stringstream buffer;
          if (file)
            buffer << file.rdbuf();
          WarningSuppressions supp =
              collectWarningSuppressions(buffer.str());
          it = suppressionCache.emplace(key, std::move(supp)).first;
        }
        return !it->second.suppresses(kind, line);
      }
    };

    for (const ModuleNode *rootMod : roots) {
      if (!pipeline.run(rootMod, semaCtx) || diagEngine.hasErrors()) {
        std::cerr << "[Fatal] Semantic errors found." << std::endl;
        return false;
      }
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

    /* Explicit imports first, then re-exports: both must compile so the
     * backend generates their objects. */
    for (const auto *imp : modNode->importedModules) {
      if (!self(imp, self))
        return false;
    }

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
                              unitPath.string(), diagEngine, options.isDebug,
                              astCtx, options.asyncEnabled);

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
    for (const ModuleNode *rootMod : roots) {
      if (!compileTranslationUnit(rootMod, compileTranslationUnit)) {
        return false;
      }
    }
  }

  if (options.isJIT) {
    if (!options.targetTriple.empty() &&
        options.targetTriple != llvm::sys::getDefaultTargetTriple()) {
      std::cerr << "\033[1;31m[Fatal]\033[0m Cannot run JIT execution for "
                   "cross-compiled targets.\n";
      return false;
    }

    /* Fail early when the program has no 'main': materializing a module
     * without an entry point through the ORC JIT corrupts the host stack
     * (the compiler's own argc/argv slot gets clobbered, crashing 'utopia'
     * with a nondeterministic SEGV). */
    bool hasMain = false;
    for (const ModuleNode *modNode : compiledModules) {
      for (const auto *stmt : modNode->statements) {
        if (stmt->kind == NodeKind::FunctionDecl &&
            static_cast<const FunctionDeclNode *>(stmt)->name == "main") {
          hasMain = true;
          break;
        }
      }
      if (hasMain)
        break;
    }
    if (!hasMain) {
      std::cerr << "\033[1;31m[JIT Error]\033[0m Entry point symbol 'main' not "
                   "resolved."
                << std::endl;
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

    /* Resolve the async runtime symbols from the compiler process itself
     * (the runtime is linked into the utopia binary). */
    if (options.asyncEnabled) {
      if (auto gen = llvm::orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
              jit->getDataLayout().getGlobalPrefix())) {
        jit->getMainJITDylib().addGenerator(std::move(*gen));
      }
    }

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

    /* Initialize global constructors (e.g. llvm.global_ctors) */
    if (auto initErr = jit->initialize(jit->getMainJITDylib())) {
      std::cerr << "\033[1;31m[JIT Error]\033[0m Failed to initialize globals."
                << std::endl;
      llvm::consumeError(std::move(initErr));
      return false;
    }

    int (*mainFn)() = mainSym->toPtr<int (*)()>();
    jitExitCode = mainFn();

    /* Deinitialize globals */
    if (auto deinitErr = jit->deinitialize(jit->getMainJITDylib())) {
      llvm::consumeError(std::move(deinitErr));
    }

    Logger::debug("\033[1;32m[JIT Execution Finished]\033[0m Exit code: " +
                  std::to_string(jitExitCode));
  } else {
    ScopedTimer timer("Linking");

    /* Link with 'clang' instead of 'clang++' so the C++ standard library is
     * not implicitly linked for a C-compatible language. */
    std::string compilerPath = "clang";
    std::string arPath = "llvm-ar";

    bool isAndroidTarget =
        triple.isAndroid() ||
        options.targetTriple.find("android") != std::string::npos;

    if (isAndroidTarget && !options.sysroot.empty()) {
      fs::path sysrootPath(options.sysroot);
      fs::path binDir = sysrootPath.parent_path() / "bin";

      /* The NDK clang needs the API level in the target triple
       * ('aarch64-linux-android21'); without it the sysroot crt files and
       * platform libraries cannot be located. */
      std::string linkTriple = options.targetTriple;
      if (linkTriple.find("android") != std::string::npos &&
          !std::regex_search(linkTriple, std::regex("android\\d+$"))) {
        linkTriple += "21";
      }

      /* Prefer the target-prefixed clang wrapper when present. */
      fs::path wrapperBin = binDir / (linkTriple + "-clang");
      if (fs::exists(wrapperBin)) {
        compilerPath = wrapperBin.string();
      } else {
        /* Fall back to the plain 'clang' binary. */
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

    /* The prelude is always compiled in, and its Math module references the
     * C math functions (acos, sqrt, ...). On glibc/BSD they live in a
     * separate libm that must be linked explicitly; without it every build
     * fails at link time with an undefined symbol. macOS and Windows keep
     * the math functions in the system libc, and Android resolves them
     * through the NDK sysroot automatically. */
    if (!isAndroidTarget && !triple.isOSWindows() && !triple.isMacOSX()) {
      activeLinkerFlags.push_back("-lm");
    }

    /* Locates a runtime library in the install/source layout. */
    auto findRuntimeLib = [&](std::string_view libName,
                              std::string_view installDir) -> fs::path {
      std::vector<fs::path> libDirs;
      /* Install layout: <prefix>/lib/utopia/<installDir> */
      if (!options.preludeRoot.empty()) {
        libDirs.push_back(fs::path(options.preludeRoot).parent_path()
                              .parent_path() /
                          installDir);
      }
      /* Official install prefix baked in at build time (handles staged
       * installs where preludeRoot lives in a local tools/ tree). */
#ifdef UTOPIA_INTERNAL_LIB_PATH
      libDirs.push_back(fs::path(UTOPIA_INTERNAL_LIB_PATH) / installDir);
#endif
      /* Source layout: <repo>/build/runtime/<libName> */
      if (!options.preludeRoot.empty()) {
        libDirs.push_back(fs::path(options.preludeRoot).parent_path()
                              .parent_path()
                              .parent_path() /
                          "build" / "runtime" / libName);
      }
      for (const auto &dir : libDirs) {
        if (fs::exists(dir / ("lib" + std::string(libName) + ".a"))) {
          return dir;
        }
      }
      return {};
    };

    /* The exception/runtime library is always required: it provides the
     * personality function, the throw/catch machinery and 'assert'. */
    fs::path runtimeDir = findRuntimeLib("utopia_runtime", "runtime");
    if (!runtimeDir.empty()) {
      activeLinkerFlags.push_back("\"-L" + runtimeDir.string() + "\"");
      activeLinkerFlags.push_back("-lutopia_runtime");
    } else {
      Logger::warning(
          "[Linker] libutopia_runtime.a not found; programs using "
          "try/catch/throw or assert may fail to link.");
    }

    /* Link the async runtime (event loop, threads, timers) when async
     * support is enabled. */
    if (options.asyncEnabled) {
      fs::path asyncDir = findRuntimeLib("utopia_async", "async");
      if (!asyncDir.empty()) {
        activeLinkerFlags.push_back("\"-L" + asyncDir.string() + "\"");
        activeLinkerFlags.push_back("-lutopia_async");
        /* The async runtime is C++; the C linker needs the C++ runtime
         * library for its exception/RTTI support. */
        activeLinkerFlags.push_back("-lstdc++");
      } else {
        Logger::warning(
            "[Linker] libutopia_async.a not found; async programs may fail "
            "to link.");
      }

      if (!isAndroidTarget) {
        activeLinkerFlags.push_back("-lpthread");
      }
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

    /* The linker subprocess can report success and still leave no output
     * (missing tool, killed output write, misconfigured wrapper). Never
     * print '[Build Success]' without the artifact actually being on disk:
     * a silent missing binary is the failure mode this guard exists for. */
    auto verifyArtifact = [&](const fs::path &artifactPath) -> bool {
      if (!fs::is_regular_file(artifactPath)) {
        std::cerr << "\033[1;31m[Fatal]\033[0m Linker reported success but "
                     "no output file was produced at: "
                  << artifactPath.string() << std::endl;
        return false;
      }
      return true;
    };

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
      if (!verifyArtifact(libPath))
        return false;
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
      if (!verifyArtifact(libPath))
        return false;
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
      if (!verifyArtifact(executablePath))
        return false;
      Logger::info("\033[1;32m[Build Success]\033[0m " + executablePath);
    }
  }

  return true;
}

} // namespace utopia