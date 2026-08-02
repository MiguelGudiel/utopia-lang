#include "utopia/CodeGen/TBAAManager.hpp"
#include "utopia/CodeGen/CodeGen.hpp"

namespace utopia {

TBAAManager::TBAAManager(llvm::LLVMContext &ctx) : mdBuilder(ctx) {
  tbaaRoot = mdBuilder.createTBAARoot("Utopia TBAA");
}

llvm::MDNode *TBAAManager::getTBAATypeNode(CodeGen &cg, const Type *type) {
  if (!type || type->isVoid())
    return nullptr;

  const Type *unqual = type->getUnqualifiedType();
  if (tbaaTypes.contains(unqual))
    return tbaaTypes[unqual];

  llvm::MDNode *charNode =
      mdBuilder.createTBAAScalarTypeNode("omnipotent char", tbaaRoot);
  llvm::MDNode *node = nullptr;

  if (unqual->isPointerType() || unqual->isReferenceType() ||
      unqual->getKind() == TypeKind::RValueReference) {
    node = mdBuilder.createTBAAScalarTypeNode("any pointer", charNode);
  } else if (unqual->isBuiltinType()) {
    node = mdBuilder.createTBAAScalarTypeNode(unqual->toString(), charNode);
  } else if (unqual->getKind() == TypeKind::Class ||
             unqual->getKind() == TypeKind::Struct ||
             unqual->getKind() == TypeKind::Union) {
    auto *recTy = static_cast<const RecordType *>(unqual);

    if (unqual->getKind() == TypeKind::Union) {
      node = mdBuilder.createTBAAScalarTypeNode(recTy->getName(), charNode);
    } else {
      llvm::StructType *structTy =
          llvm::cast<llvm::StructType>(cg.getLLVMType(recTy));
      const llvm::StructLayout *layout =
          cg.mod.getDataLayout().getStructLayout(structTy);

      std::vector<std::pair<llvm::MDNode *, uint64_t>> fields;
      for (const auto &f : recTy->getFields()) {
        uint64_t offset = layout->getElementOffset(f.index);
        llvm::MDNode *fieldTypeNode = getTBAATypeNode(cg, f.type);
        fields.push_back({fieldTypeNode, offset});
      }
      node = mdBuilder.createTBAAStructTypeNode(recTy->getName(), fields);
    }
  } else {
    node = charNode;
  }

  tbaaTypes[unqual] = node;
  return node;
}

llvm::MDNode *TBAAManager::getTBAAAccessTag(CodeGen &cg, const Type *type) {
  if (!type || type->isVoid())
    return nullptr;

  const Type *unqual = type->getUnqualifiedType();
  if (unqual->getKind() == TypeKind::Struct ||
      unqual->getKind() == TypeKind::Class ||
      unqual->getKind() == TypeKind::Array) {
    return nullptr;
  }

  llvm::MDNode *typeNode = getTBAATypeNode(cg, type);
  return mdBuilder.createTBAAStructTagNode(typeNode, typeNode, 0);
}

llvm::MDNode *TBAAManager::getTBAAStructAccessTag(CodeGen &cg,
                                                  const Type *baseType,
                                                  const Type *accessType,
                                                  uint64_t offset) {
  if (!baseType || !accessType)
    return nullptr;

  const Type *unqualAccess = accessType->getUnqualifiedType();
  if (unqualAccess->getKind() == TypeKind::Struct ||
      unqualAccess->getKind() == TypeKind::Class ||
      unqualAccess->getKind() == TypeKind::Array) {
    return nullptr;
  }

  llvm::MDNode *baseNode = getTBAATypeNode(cg, baseType);
  llvm::MDNode *accessNode = getTBAATypeNode(cg, accessType);

  return mdBuilder.createTBAAStructTagNode(baseNode, accessNode, offset);
}

llvm::MDNode *TBAAManager::getTBAATagForExpr(CodeGen &cg,
                                             const ExprNode *node) {
  if (!node || !node->exprType)
    return nullptr;

  const Type *unqual = node->exprType->getUnqualifiedType();
  if (unqual->getKind() == TypeKind::Struct ||
      unqual->getKind() == TypeKind::Class ||
      unqual->getKind() == TypeKind::Union ||
      unqual->getKind() == TypeKind::Array) {
    return nullptr;
  }

  if (node->kind == NodeKind::MemberAccess) {
    auto *ma = static_cast<const MemberAccessNode *>(node);
    if (ma->isMethodRef)
      return nullptr;
    if (ma->isStaticFieldRef)
      return getTBAAAccessTag(cg, node->exprType);

    const Type *baseTy = ma->object->exprType;
    if (baseTy->isPointerType()) {
      baseTy = static_cast<const PointerType *>(baseTy)->getPointeeType();
    } else if (baseTy->isReferenceType() ||
               baseTy->getKind() == TypeKind::RValueReference) {
      baseTy = static_cast<const ReferenceType *>(baseTy)->getPointeeType();
    }

    if (baseTy->getKind() == TypeKind::Union) {
      return getTBAAAccessTag(cg, node->exprType);
    }

    llvm::StructType *llBaseTy =
        llvm::cast<llvm::StructType>(cg.getLLVMType(baseTy));
    uint64_t offset =
        cg.mod.getDataLayout().getStructLayout(llBaseTy)->getElementOffset(
            ma->fieldIndex);

    return getTBAAStructAccessTag(cg, baseTy, ma->exprType, offset);
  }

  if (node->kind == NodeKind::Variable) {
    auto *varNode = static_cast<const VariableNode *>(node);
    if (varNode->isField) {
      if (varNode->parentType->getKind() == TypeKind::Union) {
        return getTBAAAccessTag(cg, node->exprType);
      }

      llvm::StructType *llBaseTy =
          llvm::cast<llvm::StructType>(cg.getLLVMType(varNode->parentType));
      uint64_t offset =
          cg.mod.getDataLayout().getStructLayout(llBaseTy)->getElementOffset(
              varNode->fieldIndex);
      return getTBAAStructAccessTag(cg, varNode->parentType, varNode->exprType,
                                    offset);
    }
  }

  return getTBAAAccessTag(cg, node->exprType);
}

} // namespace utopia