#include <llvm/Support/Compiler.h>

#include "utopia/Common/Timer.hpp"
#include "utopia/Driver/Backend.hpp"
#include <filesystem>
#include <iostream>
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/Analysis/TargetTransformInfo.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/Transforms/Utils/Cloning.h>
#include <memory>

namespace fs = std::filesystem;

namespace utopia {

bool Backend::process(llvm::Module *mod, BackendContext &backendCtx,
                      const CompileOptions &options,
                      const std::string &outBasePath) {
  llvm::InitializeNativeTarget();
  llvm::InitializeNativeTargetAsmPrinter();
  llvm::InitializeNativeTargetAsmParser();

  std::string targetTripleStr = llvm::sys::getDefaultTargetTriple();
  llvm::Triple triple(targetTripleStr);
  mod->setTargetTriple(triple);

  std::string error;
  auto target = llvm::TargetRegistry::lookupTarget(targetTripleStr, error);
  if (!target) {
    std::cerr << "\033[1;31m[Backend Error]\033[0m Target lookup failed: "
              << error << "\n";
    return false;
  }

  llvm::TargetOptions opt;
  auto rm = std::optional<llvm::Reloc::Model>(llvm::Reloc::PIC_);

  std::unique_ptr<llvm::TargetMachine> targetMachine(
      target->createTargetMachine(triple, "generic", "", opt, rm));

  mod->setDataLayout(targetMachine->createDataLayout());

  llvm::PassBuilder pb(targetMachine.get());
  llvm::LoopAnalysisManager lam;
  llvm::FunctionAnalysisManager fam;
  llvm::CGSCCAnalysisManager cgam;
  llvm::ModuleAnalysisManager mam;

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

  if (level == llvm::OptimizationLevel::O0) {
    mpm = pb.buildO0DefaultPipeline(level);
  } else {
    mpm = pb.buildPerModuleDefaultPipeline(level);
  }

  {
    ScopedTimer timer("LLVM Optimization");
    mpm.run(*mod, mam);
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
      /*
       * Cloned module MUST be declared before the PassManager to ensure it
       * outlives the PassManager during scope destruction, preventing
       * use-after-free segfaults when the PassManager cleans up cached machine
       * functions.
       */
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
          target->createTargetMachine(triple, "generic", "", opt, rm));

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