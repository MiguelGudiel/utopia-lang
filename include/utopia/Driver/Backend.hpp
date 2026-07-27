#pragma once
#include "utopia/Driver/CompilerDriver.hpp"
#include "utopia/CodeGen/BackendContext.hpp"
#include <llvm/IR/Module.h>
#include <string>

namespace utopia {

class Backend {
public:
  static bool process(llvm::Module *mod, BackendContext &backendCtx,
                      const CompileOptions &options, const std::string &outBasePath);
};

} // namespace utopia