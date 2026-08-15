#include "BuildScriptRunner.hpp"
#include "utopia/Driver/Compiler.hpp"
#include "utopia/Driver/ModuleLoader.hpp"
#include "utopia/Sema/Sema.hpp"
#include <iostream>
#include <llvm/ExecutionEngine/Orc/ExecutionUtils.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/TargetParser/Host.h>
#include <unordered_set>

namespace utopia {

static CompileOptions *g_CurrentBuildOptions = nullptr;

extern "C" {

void UtopiaBuild_addLinkerFlag(const char *flag) {
  if (g_CurrentBuildOptions && flag) {
    g_CurrentBuildOptions->linkerFlags.push_back(flag);
  }
}

void UtopiaBuild_addIncludeDir(const char *dir) {
  if (g_CurrentBuildOptions && dir) {
    g_CurrentBuildOptions->includeDirs.push_back(dir);
  }
}

void UtopiaBuild_setOptLevel(int level) {
  if (g_CurrentBuildOptions) {
    g_CurrentBuildOptions->optLevel = level;
  }
}

void UtopiaBuild_addDefine(const char *name, bool isPublic) {
  if (g_CurrentBuildOptions && name) {
    if (isPublic) {
      g_CurrentBuildOptions->publicMacros.insert(name);
    } else {
      g_CurrentBuildOptions->privateMacros.insert(name);
    }
  }
}

void UtopiaBuild_removeDefine(const char *name) {
  if (g_CurrentBuildOptions && name) {
    g_CurrentBuildOptions->publicMacros.erase(name);
    g_CurrentBuildOptions->privateMacros.erase(name);
  }
}

bool UtopiaBuild_isDefined(const char *name) {
  if (g_CurrentBuildOptions && name) {
    return g_CurrentBuildOptions->publicMacros.contains(name) ||
           g_CurrentBuildOptions->privateMacros.contains(name);
  }
  return false;
}

void UtopiaBuild_addCacheDefine(const char *name, bool defaultValue,
                                bool isPublic) {
  if (!g_CurrentBuildOptions || !name)
    return;
  if (UtopiaBuild_isDefined(name))
    return;
  if (defaultValue) {
    UtopiaBuild_addDefine(name, isPublic);
  }
}

void UtopiaBuild_setSysroot(const char *sysroot) {
  if (g_CurrentBuildOptions && sysroot) {
    g_CurrentBuildOptions->sysroot = sysroot;
  }
}

const char *UtopiaBuild_getMainProjectRoot() {
  return g_CurrentBuildOptions ? g_CurrentBuildOptions->mainProjectRoot.c_str()
                               : "";
}

const char *UtopiaBuild_getCurrentProjectRoot() {
  return g_CurrentBuildOptions
             ? g_CurrentBuildOptions->currentProjectRoot.c_str()
             : "";
}

const char *UtopiaBuild_getOutputDir() {
  return g_CurrentBuildOptions ? g_CurrentBuildOptions->outputDir.c_str() : "";
}

const char *UtopiaBuild_getMainOutputDir() {
  return g_CurrentBuildOptions ? g_CurrentBuildOptions->mainOutputDir.c_str()
                               : "";
}

const char *UtopiaBuild_getTargetTriple() {
  return g_CurrentBuildOptions ? g_CurrentBuildOptions->targetTriple.c_str()
                               : "";
}

const char *UtopiaBuild_getBuildType() {
  return (g_CurrentBuildOptions && g_CurrentBuildOptions->isDebug) ? "Debug"
                                                                   : "Release";
}

const char *UtopiaBuild_getTargetOS() {
  if (!g_CurrentBuildOptions)
    return "unknown";
  std::string t = g_CurrentBuildOptions->targetTriple;
  if (t.empty())
    t = llvm::sys::getDefaultTargetTriple();
  llvm::Triple triple(t);

  if (triple.isOSWindows())
    return "windows";
  if (triple.isMacOSX())
    return "macos";
  if (triple.isAndroid() || t.find("android") != std::string::npos)
    return "android";
  if (triple.isOSLinux())
    return "linux";
  return "unknown";
}

const char *UtopiaBuild_getTargetArch() {
  if (!g_CurrentBuildOptions)
    return "unknown";
  std::string t = g_CurrentBuildOptions->targetTriple;
  if (t.empty())
    t = llvm::sys::getDefaultTargetTriple();
  llvm::Triple triple(t);

  static std::string archStr;
  archStr = triple.getArchName().str();
  return archStr.c_str();
}
} // extern "C"

bool BuildScriptRunner::run(const std::filesystem::path &scriptPath,
                            CompileOptions &options,
                            const std::filesystem::path &projRoot) {
  g_CurrentBuildOptions = &options;

  ModuleLoaderConfig modConfig;
  modConfig.projectRoot = projRoot;
  modConfig.stdlibRoot = options.stdlibRoot;
  modConfig.preludeRoot = options.preludeRoot;
  modConfig.buildLibRoot = options.buildLibRoot;
  modConfig.includeDirs = options.includeDirs;
  modConfig.packages = options.packages;
  modConfig.definedMacros = options.publicMacros;
  modConfig.definedMacros.insert(options.privateMacros.begin(),
                                 options.privateMacros.end());

  std::string targetTripleStr = options.targetTriple.empty()
                                    ? llvm::sys::getDefaultTargetTriple()
                                    : options.targetTriple;
  llvm::Triple triple(targetTripleStr);

  if (triple.isOSWindows()) {
    modConfig.definedMacros.insert("_WIN32");
  } else if (triple.isMacOSX()) {
    modConfig.definedMacros.insert("__APPLE__");
  } else if (triple.isAndroid() ||
             targetTripleStr.find("android") != std::string::npos) {
    modConfig.definedMacros.insert("__ANDROID__");
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

  modConfig.isBuildScript = true;

  DiagnosticsEngine diagEngine;
  ASTContext astCtx;
  ModuleLoader loader(astCtx, modConfig, diagEngine);

  ModuleNode *root = loader.loadModule(scriptPath.string());
  if (!root || diagEngine.hasErrors()) {
    g_CurrentBuildOptions = nullptr;
    return false;
  }

  SemaContext semaCtx(astCtx, diagEngine, scriptPath.string());
  SemaPipeline pipeline;
  if (!pipeline.run(root, semaCtx) || diagEngine.hasErrors()) {
    g_CurrentBuildOptions = nullptr;
    return false;
  }

  BackendContext backendCtx;
  std::unordered_set<const ModuleNode *> compiledModules;

  auto compileTU = [&](const ModuleNode *modNode, auto &self) -> bool {
    if (compiledModules.contains(modNode))
      return true;
    compiledModules.insert(modNode);

    for (const auto *imp : modNode->importedModules) {
      if (!self(imp, self))
        return false;
    }
    for (const auto *exp : modNode->exportedModules) {
      if (!self(exp, self))
        return false;
    }

    std::string unitStr =
        std::filesystem::absolute(std::string(modNode->filePath)).string();

    llvm::Module *llvmMod =
        Compiler::compileToIR(const_cast<ModuleNode *>(modNode), backendCtx,
                              unitStr, diagEngine, options.isDebug);

    if (!llvmMod || diagEngine.hasErrors()) {
      return false;
    }
    return true;
  };

  if (!compileTU(root, compileTU)) {
    g_CurrentBuildOptions = nullptr;
    return false;
  }

  llvm::InitializeNativeTarget();
  llvm::InitializeNativeTargetAsmPrinter();
  llvm::InitializeNativeTargetAsmParser();

  auto jitEx = llvm::orc::LLJITBuilder().create();
  if (!jitEx) {
    g_CurrentBuildOptions = nullptr;
    return false;
  }
  auto jit = std::move(*jitEx);

  auto &jd = jit->getMainJITDylib();
  auto libSym = llvm::orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
      jit->getDataLayout().getGlobalPrefix());
  if (libSym) {
    jd.addGenerator(std::move(*libSym));
  }

  llvm::orc::SymbolMap symbols;
  auto addSym = [&](llvm::StringRef name, void *ptr) {
    symbols[jit->getExecutionSession().intern(name)] = {
        llvm::orc::ExecutorAddr::fromPtr(ptr), llvm::JITSymbolFlags::Exported};
  };

  addSym("UtopiaBuild_addLinkerFlag", (void *)UtopiaBuild_addLinkerFlag);
  addSym("UtopiaBuild_addIncludeDir", (void *)UtopiaBuild_addIncludeDir);
  addSym("UtopiaBuild_setOptLevel", (void *)UtopiaBuild_setOptLevel);
  addSym("UtopiaBuild_addDefine", (void *)UtopiaBuild_addDefine);
  addSym("UtopiaBuild_removeDefine", (void *)UtopiaBuild_removeDefine);
  addSym("UtopiaBuild_isDefined", (void *)UtopiaBuild_isDefined);
  addSym("UtopiaBuild_addCacheDefine", (void *)UtopiaBuild_addCacheDefine);
  addSym("UtopiaBuild_setSysroot", (void *)UtopiaBuild_setSysroot);

  addSym("UtopiaBuild_getMainProjectRoot",
         (void *)UtopiaBuild_getMainProjectRoot);
  addSym("UtopiaBuild_getCurrentProjectRoot",
         (void *)UtopiaBuild_getCurrentProjectRoot);
  addSym("UtopiaBuild_getOutputDir", (void *)UtopiaBuild_getOutputDir);
  addSym("UtopiaBuild_getMainOutputDir",
         (void *)UtopiaBuild_getMainOutputDir);
  addSym("UtopiaBuild_getTargetTriple", (void *)UtopiaBuild_getTargetTriple);
  addSym("UtopiaBuild_getBuildType", (void *)UtopiaBuild_getBuildType);
  addSym("UtopiaBuild_getTargetOS", (void *)UtopiaBuild_getTargetOS);
  addSym("UtopiaBuild_getTargetArch", (void *)UtopiaBuild_getTargetArch);

  if (auto err = jd.define(llvm::orc::absoluteSymbols(symbols))) {
    llvm::consumeError(std::move(err));
    g_CurrentBuildOptions = nullptr;
    return false;
  }

  auto uniqueCtx = backendCtx.takeContext();
  llvm::orc::ThreadSafeContext tsc(std::move(uniqueCtx));

  for (const auto *modNode : compiledModules) {
    std::string unitStr =
        std::filesystem::absolute(std::string(modNode->filePath)).string();
    auto uniqueMod = backendCtx.takeModule(unitStr);

    if (uniqueMod) {
      auto tsm = llvm::orc::ThreadSafeModule(std::move(uniqueMod), tsc);
      if (auto err = jit->addIRModule(std::move(tsm))) {
        llvm::handleAllErrors(
            std::move(err), [&](const llvm::ErrorInfoBase &EI) {
              std::cerr << "[JIT Add Module Error] " << EI.message() << "\n";
            });
        g_CurrentBuildOptions = nullptr;
        return false;
      }
    }
  }

  auto mainSym = jit->lookup("main");
  if (!mainSym) {
    llvm::handleAllErrors(
        mainSym.takeError(), [&](const llvm::ErrorInfoBase &EI) {
          std::cerr << "[Build Script JIT Error] " << EI.message() << "\n";
        });
    std::cerr << "Fatal: Entry point 'main' not found or failed to compile in "
                 "build.utp.\n";
    g_CurrentBuildOptions = nullptr;
    return false;
  }

  /* Initialize global constructors (e.g. llvm.global_ctors) */
  if (auto initErr = jit->initialize(jit->getMainJITDylib())) {
    llvm::handleAllErrors(
        std::move(initErr), [&](const llvm::ErrorInfoBase &EI) {
          std::cerr << "[Build Script JIT Error] Failed to initialize globals: "
                    << EI.message() << "\n";
        });
    g_CurrentBuildOptions = nullptr;
    return false;
  }

  int (*mainFn)() = mainSym->toPtr<int (*)()>();

  /* Temporarily shift the working directory to the project root to ensure
     any relative paths within the build script execute predictably. */
  std::filesystem::path previousPath = std::filesystem::current_path();
  std::filesystem::current_path(projRoot);

  int exitCode = mainFn();

  /* Deinitialize globals */
  if (auto deinitErr = jit->deinitialize(jit->getMainJITDylib())) {
    llvm::consumeError(std::move(deinitErr));
  }

  std::filesystem::current_path(previousPath);

  g_CurrentBuildOptions = nullptr;

  if (exitCode != 0) {
    std::cerr << "Fatal: build script exited with code " << exitCode << ".\n";
    return false;
  }

  return true;
}

} // namespace utopia