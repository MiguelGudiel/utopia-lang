#include "utopia/CodeGen/BackendContext.hpp"
#include <llvm/IR/DiagnosticHandler.h>
#include <llvm/IR/DiagnosticInfo.h>
#include <llvm/IR/DiagnosticPrinter.h>
#include <iostream>

namespace utopia {

struct UtopiaDiagnosticHandler : public llvm::DiagnosticHandler {
  bool handleDiagnostics(const llvm::DiagnosticInfo &DI) override {
    unsigned severity = DI.getSeverity();
    switch (severity) {
    case llvm::DS_Error:
      std::cerr << "\033[1;31m[LLVM Error]\033[0m ";
      break;
    case llvm::DS_Warning:
      std::cerr << "\033[1;33m[LLVM Warning]\033[0m ";
      break;
    default:
      return true;
    }

    llvm::DiagnosticPrinterRawOStream DP(llvm::errs());
    DI.print(DP);
    std::cerr << std::endl;
    return true;
  }
};

BackendContext::BackendContext() {
  llvmCtx = std::make_unique<llvm::LLVMContext>();
  llvmCtx->setDiagnosticHandler(std::make_unique<UtopiaDiagnosticHandler>(), true);
}

llvm::Module *BackendContext::createModule(std::string_view moduleName) {
  std::string name(moduleName);
  auto mod = std::make_unique<llvm::Module>(name, *llvmCtx);
  llvm::Module *modPtr = mod.get();
  modules[name] = std::move(mod);
  return modPtr;
}

llvm::Module *BackendContext::getModule(std::string_view moduleName) {
  auto it = modules.find(std::string(moduleName));
  if (it != modules.end()) {
    return it->second.get();
  }
  return nullptr;
}

std::unique_ptr<llvm::Module> BackendContext::takeModule(std::string_view moduleName) {
  std::string name(moduleName);
  auto it = modules.find(name);
  if (it != modules.end()) {
    auto mod = std::move(it->second);
    modules.erase(it);
    return mod;
  }
  return nullptr;
}

} // namespace utopia