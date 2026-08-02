#pragma once
#include "utopia/AST/AST.hpp"
#include <llvm/IR/MDBuilder.h>
#include <llvm/IR/Module.h>
#include <unordered_map>

namespace utopia {
class CodeGen;

/*
 * Manages Type-Based Alias Analysis (TBAA) nodes for strict memory
 * disambiguation.
 */
class TBAAManager {
public:
  TBAAManager(llvm::LLVMContext &ctx);

  llvm::MDNode *getTBAATypeNode(CodeGen &cg, const Type *type);
  llvm::MDNode *getTBAAAccessTag(CodeGen &cg, const Type *type);
  llvm::MDNode *getTBAAStructAccessTag(CodeGen &cg, const Type *baseType,
                                       const Type *accessType, uint64_t offset);
  llvm::MDNode *getTBAATagForExpr(CodeGen &cg, const ExprNode *node);

private:
  llvm::MDBuilder mdBuilder;
  llvm::MDNode *tbaaRoot = nullptr;
  std::unordered_map<const Type *, llvm::MDNode *> tbaaTypes;
};

} // namespace utopia