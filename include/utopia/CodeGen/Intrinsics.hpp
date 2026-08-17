#pragma once
#include "utopia/AST/AST.hpp"
#include <llvm/IR/Constant.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Value.h>
#include <memory>
#include <string_view>
#include <unordered_map>

namespace utopia {

class CodeGen;

/**
 * Base class for all compiler intrinsics.
 * Acts as a proxy to access private CodeGen state securely.
 */
class Intrinsic {
protected:
  llvm::Type *getLLVMType(CodeGen &cg, const Type *type) const;
  llvm::Constant *
  createTypeReflectionConstant(CodeGen &cg, const Type *t,
                               llvm::StructType *structTy) const;
  llvm::IRBuilder<> &getBuilder(CodeGen &cg) const;
  llvm::Module &getModule(CodeGen &cg) const;
  void emitCleanupCall(CodeGen &cg, llvm::Value *ptr,
                       const FunctionDeclNode *dtor) const;
  llvm::AllocaInst *createEntryBlockAlloca(CodeGen &cg, llvm::Type *type,
                                           const std::string &varName) const;

public:
  virtual ~Intrinsic() = default;

  /**
   * Evaluates the intrinsic at runtime.
   */
  virtual llvm::Value *evaluateRuntime(CodeGen &cg,
                                       const FunctionCallNode *node) const = 0;

  /**
   * Evaluates the intrinsic as a compile-time constant.
   */
  virtual llvm::Constant *
  evaluateConstant(CodeGen &cg, const FunctionCallNode *node) const = 0;
};

/**
 * Centralized registry for dynamic intrinsic dispatch.
 */
class IntrinsicRegistry {
public:
  static const IntrinsicRegistry &instance();
  const Intrinsic *get(std::string_view name) const;

private:
  IntrinsicRegistry();
  std::unordered_map<std::string_view, std::unique_ptr<Intrinsic>> registry;

  void registerIntrinsic(std::string_view name,
                         std::unique_ptr<Intrinsic> intrinsic);
};

} // namespace utopia