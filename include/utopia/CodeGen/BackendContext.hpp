#pragma once
#include <llvm/Support/Compiler.h>

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

namespace utopia {

/*
 * Centralized manager for LLVM backend state.
 * Owns the global LLVMContext and acts as a factory/registry for LLVM Modules.
 */
class BackendContext {
public:
  BackendContext();
  ~BackendContext() = default;

  BackendContext(const BackendContext &) = delete;
  BackendContext &operator=(const BackendContext &) = delete;

  llvm::LLVMContext &getLLVMContext() { return *llvmCtx; }

  llvm::Module *createModule(std::string_view moduleName);
  llvm::Module *getModule(std::string_view moduleName);

  std::unique_ptr<llvm::Module> takeModule(std::string_view moduleName);

  /* Transfers ownership of the global LLVMContext to the caller. */
  std::unique_ptr<llvm::LLVMContext> takeContext() {
    return std::move(llvmCtx);
  }

private:
  std::unique_ptr<llvm::LLVMContext> llvmCtx;
  std::unordered_map<std::string, std::unique_ptr<llvm::Module>> modules;
};

} // namespace utopia