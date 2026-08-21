#include <llvm/Support/Compiler.h>

#include "utopia/Common/Timer.hpp"
#include "utopia/Driver/Backend.hpp"
#include "llvm/Transforms/Coroutines/CoroCleanup.h"
#include "llvm/Transforms/Coroutines/CoroConditionalWrapper.h"
#include "llvm/Transforms/Coroutines/CoroEarly.h"
#include "llvm/Transforms/Coroutines/CoroSplit.h"
#include <filesystem>
#include <iostream>
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/Analysis/TargetTransformInfo.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/PassManager.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/TargetParser/Triple.h>
#include <llvm/Transforms/Utils/Cloning.h>
#include <memory>

namespace fs = std::filesystem;

namespace utopia {

bool Backend::process(llvm::Module *mod, BackendContext &backendCtx,
                      const CompileOptions &options,
                      const std::string &outBasePath) {
  llvm::InitializeAllTargetInfos();
  llvm::InitializeAllTargets();
  llvm::InitializeAllTargetMCs();
  llvm::InitializeAllAsmParsers();
  llvm::InitializeAllAsmPrinters();

  std::string targetTripleStr = options.targetTriple;
  if (targetTripleStr.empty()) {
    targetTripleStr = llvm::sys::getDefaultTargetTriple();
  }

  llvm::Triple triple(targetTripleStr);
  mod->setTargetTriple(triple);

  std::string error;
  auto target = llvm::TargetRegistry::lookupTarget(triple.getTriple(), error);
  if (!target) {
    std::cerr << "\033[1;31m[Backend Error]\033[0m Target lookup failed: "
              << error << "\n";
    return false;
  }

  llvm::TargetOptions opt;
  auto rm = std::optional<llvm::Reloc::Model>(llvm::Reloc::PIC_);

  llvm::CodeGenOptLevel cgLevel = llvm::CodeGenOptLevel::None;
  switch (options.optLevel) {
  case 1:
    cgLevel = llvm::CodeGenOptLevel::Less;
    break;
  case 2:
    cgLevel = llvm::CodeGenOptLevel::Default;
    break;
  case 3:
    cgLevel = llvm::CodeGenOptLevel::Aggressive;
    break;
  default:
    cgLevel = llvm::CodeGenOptLevel::None;
    break;
  }

  std::unique_ptr<llvm::TargetMachine> targetMachine(
      target->createTargetMachine(triple, options.targetCpu,
                                  options.targetFeatures, opt, rm,
                                  std::nullopt, cgLevel));

  if (!targetMachine) {
    std::cerr << "\033[1;31m[Backend Error]\033[0m Failed to create target "
                 "machine for triple: "
              << triple.getTriple() << " (cpu: '" << options.targetCpu
              << "', features: '" << options.targetFeatures << "').\n";
    return false;
  }

  mod->setDataLayout(targetMachine->createDataLayout());

  llvm::PassBuilder pb(targetMachine.get());
  llvm::LoopAnalysisManager lam;
  llvm::FunctionAnalysisManager fam;
  llvm::CGSCCAnalysisManager cgam;
  llvm::ModuleAnalysisManager mam;

  llvm::TargetLibraryInfoImpl tlii(triple);
  fam.registerPass([&] { return llvm::TargetLibraryAnalysis(tlii); });

  pb.registerModuleAnalyses(mam);
  pb.registerCGSCCAnalyses(cgam);
  pb.registerFunctionAnalyses(fam);
  pb.registerLoopAnalyses(lam);
  pb.crossRegisterProxies(lam, fam, cgam, mam);

  llvm::OptimizationLevel level;
  switch (options.optLevel) {
  case 1:
    level = llvm::OptimizationLevel::O1;
    break;
  case 2:
    level = llvm::OptimizationLevel::O2;
    break;
  case 3:
    level = llvm::OptimizationLevel::O3;
    break;
  default:
    level = llvm::OptimizationLevel::O0;
    break;
  }

  llvm::ModulePassManager mpm;

  /* Coroutines (async support): CoroEarly must run before the rest of the
   * pipeline and CoroSplit/CoroCleanup after it. The default pipelines do
   * not include the coroutine passes, so they are added explicitly here. */
  {
    llvm::ModulePassManager coroEarlyPM;
    coroEarlyPM.addPass(llvm::CoroEarlyPass());
    mpm.addPass(llvm::CoroConditionalWrapper(std::move(coroEarlyPM)));
  }

  if (level == llvm::OptimizationLevel::O0) {
    mpm = pb.buildO0DefaultPipeline(level);
  } else {
    mpm = pb.buildPerModuleDefaultPipeline(level);
  }

  {
    llvm::ModulePassManager coroLowerPM;
    coroLowerPM.addPass(createModuleToPostOrderCGSCCPassAdaptor(
        llvm::CoroSplitPass(level != llvm::OptimizationLevel::O0)));
    coroLowerPM.addPass(llvm::CoroCleanupPass());
    mpm.addPass(llvm::CoroConditionalWrapper(std::move(coroLowerPM)));
  }

  {
    ScopedTimer timer("LLVM Optimization");
    mpm.run(*mod, mam);
  }

  /* The coro.end lowering leaves resume functions ending with
   * 'unreachable'; at O0 some backends lower that to a fall-through into
   * the destroy function (running the frame free twice). Replace every
   * unreachable with a clean return in the resume functions. */
  for (auto &fn : *mod) {
    if (fn.empty() || !fn.getName().ends_with(".resume"))
      continue;
    for (auto &bb : fn) {
      auto *unr = llvm::dyn_cast<llvm::UnreachableInst>(bb.getTerminator());
      if (!unr)
        continue;
      llvm::IRBuilder<> b(unr);
      b.CreateRetVoid();
      unr->eraseFromParent();
    }
  }

  if (options.isJIT) {
    return true;
  }

  {
    ScopedTimer timer("Code Emission (LL/ASM/Object)");

    if (options.emitLLVM) {
      std::error_code ec;
      std::string llFile = outBasePath + ".ll";
      llvm::raw_fd_ostream destLL(llFile, ec, llvm::sys::fs::OF_None);
      if (ec) {
        std::cerr
            << "\033[1;31m[Backend Error]\033[0m Could not open LLVM IR file: "
            << ec.message() << "\n";
        return false;
      }
      mod->print(destLL, nullptr);
      destLL.flush();
    }

    if (options.emitAsm) {
      auto clonedMod = llvm::CloneModule(*mod);

      std::error_code ec;
      std::string asmFile = outBasePath + ".s";
      llvm::raw_fd_ostream destAsm(asmFile, ec, llvm::sys::fs::OF_None);
      if (ec) {
        std::cerr
            << "\033[1;31m[Backend Error]\033[0m Could not open Assembly file: "
            << ec.message() << "\n";
        return false;
      }

      llvm::legacy::PassManager passAsm;

      std::unique_ptr<llvm::TargetMachine> tmAsm(
          target->createTargetMachine(triple, options.targetCpu,
                                      options.targetFeatures, opt, rm,
                                      std::nullopt, cgLevel));

      if (!tmAsm) {
        std::cerr << "\033[1;31m[Backend Error]\033[0m Failed to create target "
                     "machine for assembly emission.\n";
        return false;
      }

      if (tmAsm->addPassesToEmitFile(passAsm, destAsm, nullptr,
                                     llvm::CodeGenFileType::AssemblyFile)) {
        std::cerr << "\033[1;31m[Backend Error]\033[0m TargetMachine cannot "
                     "emit assembly.\n";
        return false;
      }

      passAsm.run(*clonedMod);
      destAsm.flush();
    }

    std::string objFile = outBasePath + ".o";
    std::error_code ecObj;
    llvm::raw_fd_ostream destObj(objFile, ecObj, llvm::sys::fs::OF_None);
    if (ecObj) {
      std::cerr
          << "\033[1;31m[Backend Error]\033[0m Could not open Object file: "
          << ecObj.message() << "\n";
      return false;
    }

    llvm::legacy::PassManager passObj;

    if (targetMachine->addPassesToEmitFile(passObj, destObj, nullptr,
                                           llvm::CodeGenFileType::ObjectFile)) {
      std::cerr << "\033[1;31m[Backend Error]\033[0m TargetMachine cannot emit "
                   "object file.\n";
      return false;
    }

    passObj.run(*mod);
    destObj.flush();
  }

  return true;
}

} // namespace utopia