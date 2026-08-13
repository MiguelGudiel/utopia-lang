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

  int (*mainFn)() = mainSym->toPtr<int (*)()>();

  /* Temporarily shift the working directory to the project root to ensure
     any relative paths within the build script execute predictably. */
  std::filesystem::path previousPath = std::filesystem::current_path();
  std::filesystem::current_path(projRoot);

  int exitCode = mainFn();

  std::filesystem::current_path(previousPath);

  g_CurrentBuildOptions = nullptr;

  if (exitCode != 0) {
    std::cerr << "Fatal: build script exited with code " << exitCode << ".\n";
    return false;
  }

  return true;
}

} // namespace utopia