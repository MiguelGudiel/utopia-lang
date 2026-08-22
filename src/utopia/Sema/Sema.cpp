#include "utopia/Sema/Sema.hpp"
#include "utopia/Common/Logger.hpp"
#include "utopia/Sema/UnusedCode.hpp"
#include <iostream>

namespace utopia {

SemaPipeline::SemaPipeline() {
  passes.push_back(std::make_unique<DeclCollectorPass>());
  passes.push_back(std::make_unique<TypeCheckPass>());
  passes.push_back(std::make_unique<ControlFlowPass>());
  passes.push_back(std::make_unique<UnusedCodePass>());
}

bool SemaPipeline::run(const ModuleNode *module, SemaContext &ctx) {
  Logger::debug("[Sema Debug] Initiating semantic analysis pipeline...");

  for (auto &pass : passes) {
    Logger::debug("[Sema Debug] Executing pass: " +
                  std::string(pass->getName()));

    try {
      if (!pass->run(module, ctx)) {
        Logger::debug("[Sema Debug] Pass aborted due to unexpected failure: " +
                      std::string(pass->getName()));
        return false;
      }
    } catch (const std::exception &e) {
      /* Report through the diagnostics engine as well: the driver only
       * prints its generic '[Fatal] Semantic errors found.' when
       * hasErrors() is true, and the LSP consumes diags, so a raw stderr
       * line alone would leave editors with no visible error. */
      ctx.diags.report({DiagLevel::Error, 0, 0, 0,
                        "Internal compiler error in Sema pass '" +
                            std::string(pass->getName()) + "': " + e.what(),
                        std::string(ctx.currentFile)});
      std::cerr << "\033[1;31m[Fatal]\033[0m Exception caught in Sema pass '"
                << pass->getName() << "': " << e.what() << "\n"
                << std::flush;
      return false;
    } catch (...) {
      ctx.diags.report({DiagLevel::Error, 0, 0, 0,
                        "Internal compiler error: hardware/OS fault in Sema "
                        "pass '" +
                            std::string(pass->getName()) + "'",
                        std::string(ctx.currentFile)});
      std::cerr << "\033[1;31m[Fatal]\033[0m Hardware/OS fault in Sema pass '"
                << pass->getName() << "'.\n"
                << std::flush;
      return false;
    }

    if (ctx.hasErrors()) {
      Logger::debug(
          "[Sema Debug] Semantic integrity compromised during pass: " +
          std::string(pass->getName()));
      return false;
    }

    Logger::debug("[Sema Debug] Pass completed successfully: " +
                  std::string(pass->getName()));
  }
  return true;
}

} // namespace utopia