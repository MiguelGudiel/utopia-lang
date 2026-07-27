#pragma once
#include "utopia/AST/AST.hpp"
#include "utopia/CodeGen/BackendContext.hpp"
#include "utopia/Common/Diagnostics.hpp"
#include <llvm/IR/Module.h>
#include <string>

namespace utopia {

class Compiler {
public:
  static llvm::Module *compileToIR(ModuleNode *root, BackendContext &backendCtx,
                                   const std::string &moduleName,
                                   DiagnosticsEngine &diags);
};

} // namespace utopia