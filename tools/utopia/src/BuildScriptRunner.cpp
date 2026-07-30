#include "BuildScriptRunner.hpp"
#include "utopia/Driver/Compiler.hpp"
#include "utopia/Driver/ModuleLoader.hpp"
#include "utopia/Sema/Sema.hpp"
#include <iostream>
#include <llvm/ExecutionEngine/Orc/ExecutionUtils.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/Support/TargetSelect.h>

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
  llvm::Module *llvmMod = Compiler::compileToIR(
      root, backendCtx, "build_script", diagEngine, options.isDebug);
  if (!llvmMod || diagEngine.hasErrors()) {
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

  if (auto err = jd.define(llvm::orc::absoluteSymbols(symbols))) {
    g_CurrentBuildOptions = nullptr;
    return false;
  }

  auto uniqueCtx = backendCtx.takeContext();
  llvm::orc::ThreadSafeContext tsc(std::move(uniqueCtx));
  auto uniqueMod = backendCtx.takeModule("build_script");
  auto tsm = llvm::orc::ThreadSafeModule(std::move(uniqueMod), tsc);

  if (auto err = jit->addIRModule(std::move(tsm))) {
    g_CurrentBuildOptions = nullptr;
    return false;
  }

  auto mainSym = jit->lookup("main");
  if (!mainSym) {
    std::cerr << "[Build Script] Entry point 'main' not found in build.utp.\n";
    g_CurrentBuildOptions = nullptr;
    return false;
  }

  int (*mainFn)() = mainSym->toPtr<int (*)()>();
  mainFn();

  g_CurrentBuildOptions = nullptr;
  return true;
}

} // namespace utopia