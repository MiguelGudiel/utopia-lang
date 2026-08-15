#include "utopia/Driver/Compiler.hpp"
#include "utopia/CodeGen/CodeGen.hpp"
#include "utopia/Common/Timer.hpp"
#include <iostream>
#include <llvm/IR/Verifier.h>

namespace utopia {

llvm::Module *Compiler::compileToIR(ModuleNode *root,
                                    BackendContext &backendCtx,
                                    const std::string &moduleName,
                                    DiagnosticsEngine &diags,
                                    bool emitDebugInfo,
                                    bool asyncEnabled) {
  llvm::Module *llvmMod = backendCtx.createModule(moduleName);
  CodeGen codegen(backendCtx, *llvmMod, diags, emitDebugInfo,
                  std::string(root->filePath), asyncEnabled);
  codegen.dispatch(root);

  if (diags.hasErrors()) {
    return nullptr;
  }

  if (const char *dbg = std::getenv("UTOPIA_DUMP_IR")) {
    std::string dbgPath = std::string(dbg) + "/" + std::string(moduleName);
    for (char &ch : dbgPath)
      if (ch == '/') ch = '_';
    std::error_code ec;
    llvm::raw_fd_ostream out(dbgPath, ec);
    if (!ec)
      llvmMod->print(out, nullptr);
  }

  std::string IRTypeErrors;
  llvm::raw_string_ostream os(IRTypeErrors);

  if (llvm::verifyModule(*llvmMod, &os)) {
    std::cerr << "\033[1;31m[IR Verification Failure in " << moduleName
              << "]\033[0m\n"
              << os.str() << std::endl;
    return nullptr;
  }

  return llvmMod;
}

} // namespace utopia