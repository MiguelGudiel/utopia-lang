#include "utopia/CodeGen/Intrinsics.hpp"
#include "utopia/CodeGen/CodeGen.hpp"

namespace utopia {

/* Proxy implementations to access CodeGen's private members */

llvm::Type *Intrinsic::getLLVMType(CodeGen &cg, const Type *type) const {
  return cg.getLLVMType(type);
}

llvm::Constant *
Intrinsic::createTypeReflectionConstant(CodeGen &cg, const Type *t,
                                        llvm::StructType *structTy) const {
  return cg.createTypeReflectionConstant(t, structTy);
}

llvm::IRBuilder<> &Intrinsic::getBuilder(CodeGen &cg) const {
  return cg.builder;
}

llvm::Module &Intrinsic::getModule(CodeGen &cg) const { return cg.mod; }

/* Intrinsic Implementations */

class SizeofTypeIntrinsic : public Intrinsic {
public:
  llvm::Value *evaluateRuntime(CodeGen &cg,
                               const FunctionCallNode *node) const override {
    return evaluateConstant(cg, node);
  }

  llvm::Constant *
  evaluateConstant(CodeGen &cg, const FunctionCallNode *node) const override {
    if (node->args.size() == 1 && node->args[0]->representedType) {
      llvm::Type *llTy = getLLVMType(cg, node->args[0]->representedType);
      uint64_t size = getModule(cg).getDataLayout().getTypeAllocSize(llTy);
      return getBuilder(cg).getInt64(size);
    }
    return getBuilder(cg).getInt64(0);
  }
};

class SizeofExprIntrinsic : public Intrinsic {
public:
  llvm::Value *evaluateRuntime(CodeGen &cg,
                               const FunctionCallNode *node) const override {
    return evaluateConstant(cg, node);
  }

  llvm::Constant *
  evaluateConstant(CodeGen &cg, const FunctionCallNode *node) const override {
    if (node->args.size() == 1 && node->args[0]->exprType) {
      llvm::Type *llTy = getLLVMType(cg, node->args[0]->exprType);
      uint64_t size = getModule(cg).getDataLayout().getTypeAllocSize(llTy);
      return getBuilder(cg).getInt64(size);
    }
    return getBuilder(cg).getInt64(0);
  }
};

class TypeofTypeIntrinsic : public Intrinsic {
public:
  llvm::Value *evaluateRuntime(CodeGen &cg,
                               const FunctionCallNode *node) const override {
    llvm::Constant *constVal = evaluateConstant(cg, node);
    return constVal ? constVal
                    : llvm::UndefValue::get(getLLVMType(cg, node->exprType));
  }

  llvm::Constant *
  evaluateConstant(CodeGen &cg, const FunctionCallNode *node) const override {
    if (node->args.size() == 1 && node->args[0]->representedType) {
      return createTypeReflectionConstant(
          cg, node->args[0]->representedType,
          llvm::cast<llvm::StructType>(getLLVMType(cg, node->exprType)));
    }
    return nullptr;
  }
};

class TypeofExprIntrinsic : public Intrinsic {
public:
  llvm::Value *evaluateRuntime(CodeGen &cg,
                               const FunctionCallNode *node) const override {
    llvm::Constant *constVal = evaluateConstant(cg, node);
    return constVal ? constVal
                    : llvm::UndefValue::get(getLLVMType(cg, node->exprType));
  }

  llvm::Constant *
  evaluateConstant(CodeGen &cg, const FunctionCallNode *node) const override {
    if (node->args.size() == 1 && node->args[0]->exprType) {
      return createTypeReflectionConstant(
          cg, node->args[0]->exprType,
          llvm::cast<llvm::StructType>(getLLVMType(cg, node->exprType)));
    }
    return nullptr;
  }
};

/* Registry Implementation */

IntrinsicRegistry::IntrinsicRegistry() {
  registerIntrinsic("sizeof_type", std::make_unique<SizeofTypeIntrinsic>());
  registerIntrinsic("sizeof_expr", std::make_unique<SizeofExprIntrinsic>());
  registerIntrinsic("typeof_type", std::make_unique<TypeofTypeIntrinsic>());
  registerIntrinsic("typeof_expr", std::make_unique<TypeofExprIntrinsic>());
}

const IntrinsicRegistry &IntrinsicRegistry::instance() {
  static IntrinsicRegistry registry;
  return registry;
}

void IntrinsicRegistry::registerIntrinsic(
    std::string_view name, std::unique_ptr<Intrinsic> intrinsic) {
  registry[name] = std::move(intrinsic);
}

const Intrinsic *IntrinsicRegistry::get(std::string_view name) const {
  auto it = registry.find(name);
  if (it != registry.end()) {
    return it->second.get();
  }
  return nullptr;
}

} // namespace utopia