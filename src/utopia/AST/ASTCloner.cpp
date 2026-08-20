#include "utopia/AST/ASTCloner.hpp"

namespace utopia {

ASTCloner::ASTCloner(
    ASTContext &ctx,
    const std::unordered_map<std::string_view, const Type *> &tMap,
    std::string_view baseName, std::string_view mangledName)
    : ctx(ctx), typeMap(tMap), baseName(baseName), mangledName(mangledName) {}

const Type *ASTCloner::cloneType(const Type *t) {
  if (!t)
    return nullptr;
  switch (t->getKind()) {
  case TypeKind::TemplateParam: {
    auto *tp = static_cast<const TemplateParamType *>(t);
    if (auto it = typeMap.find(tp->getName()); it != typeMap.end()) {
      return it->second;
    }
    return t;
  }
  case TypeKind::Pointer:
    return ctx.getPointerType(
        cloneType(static_cast<const PointerType *>(t)->getPointeeType()));
  case TypeKind::Reference:
    return ctx.getReferenceType(
        cloneType(static_cast<const ReferenceType *>(t)->getPointeeType()));
  case TypeKind::RValueReference:
    return ctx.getRValueReferenceType(cloneType(
        static_cast<const RValueReferenceType *>(t)->getPointeeType()));
  case TypeKind::Const:
    return ctx.getConstType(
        cloneType(static_cast<const ConstType *>(t)->getBaseType()));
  case TypeKind::Array: {
    auto *arr = static_cast<const ArrayType *>(t);
    return ctx.getArrayType(cloneType(arr->getElementType()), arr->getSize());
  }
  case TypeKind::MapLiteral: {
    auto *map = static_cast<const MapLiteralType *>(t);
    return ctx.getMapLiteralType(cloneType(map->getKeyType()),
                                 cloneType(map->getValueType()), map->getSize());
  }
  case TypeKind::Function: {
    auto *f = static_cast<const FunctionType *>(t);
    std::vector<const Type *> pTypes;
    for (auto *p : f->getParamTypes())
      pTypes.push_back(cloneType(p));
    return ctx.getFunctionType(cloneType(f->getReturnType()),
                               ctx.copyArray<const Type *>(pTypes));
  }
  case TypeKind::TemplateInst: {
    auto *inst = static_cast<const TemplateInstType *>(t);
    std::vector<const Type *> args;
    for (auto *a : inst->getTemplateArgs())
      args.push_back(cloneType(a));
    return ctx.getTemplateInstType(inst->getBaseName(),
                                   ctx.copyArray<const Type *>(args));
  }
  default:
    return t;
  }
}

ASTNode *ASTCloner::visit(const NumberNode *n) {
  return ctx.create<NumberNode>(n->raw, n->isFloat, n->line, n->column,
                                n->length);
}

ASTNode *ASTCloner::visit(const BoolNode *n) {
  return ctx.create<BoolNode>(n->value, n->line, n->column, n->length);
}

ASTNode *ASTCloner::visit(const CharNode *n) {
  return ctx.create<CharNode>(n->value, n->line, n->column, n->length);
}

ASTNode *ASTCloner::visit(const RuneNode *n) {
  return ctx.create<RuneNode>(n->value, n->line, n->column, n->length);
}

ASTNode *ASTCloner::visit(const StringNode *n) {
  return ctx.create<StringNode>(n->value, n->line, n->column, n->length);
}

ASTNode *ASTCloner::visit(const NullNode *n) {
  return ctx.create<NullNode>(n->line, n->column, n->length);
}

ASTNode *ASTCloner::visit(const TypeLiteralNode *n) {
  auto *node = ctx.create<TypeLiteralNode>(cloneType(n->representedType),
                                           n->line, n->column, n->length);
  return node;
}

ASTNode *ASTCloner::visit(const VariableNode *n) {
  auto *node = ctx.create<VariableNode>(n->name, n->line, n->column, n->length);
  std::vector<const Type *> tArgs;
  for (auto *ta : n->templateArgs)
    tArgs.push_back(cloneType(ta));
  node->templateArgs = ctx.copyArray<const Type *>(tArgs);
  return node;
}

ASTNode *ASTCloner::visit(const UnaryOpNode *n) {
  return ctx.create<UnaryOpNode>(n->op,
                                 static_cast<ExprNode *>(dispatch(n->expr)),
                                 n->line, n->column, n->isPostfix);
}

ASTNode *ASTCloner::visit(const BinaryOpNode *n) {
  return ctx.create<BinaryOpNode>(
      n->op, static_cast<ExprNode *>(dispatch(n->left)),
      static_cast<ExprNode *>(dispatch(n->right)), n->line, n->column);
}

ASTNode *ASTCloner::visit(const LambdaNode *n) {
  auto *node = ctx.create<LambdaNode>(n->line, n->column, n->length);
  node->params = cloneArray(n->params);
  node->isExpressionBody = n->isExpressionBody;
  node->isAsync = n->isAsync;
  if (n->exprBody)
    node->exprBody = static_cast<ExprNode *>(dispatch(n->exprBody));
  if (n->body)
    node->body = static_cast<BlockNode *>(dispatch(n->body));
  node->explicitReturnType = cloneType(n->explicitReturnType);
  node->hasParens = n->hasParens;
  node->length = n->length;
  node->endLine = n->endLine;
  return node;
}

ASTNode *ASTCloner::visit(const AwaitExprNode *n) {
  auto *node = ctx.create<AwaitExprNode>(
      static_cast<ExprNode *>(dispatch(n->expr)), n->line, n->column,
      n->length);
  return node;
}

ASTNode *ASTCloner::visit(const TernaryOpNode *n) {  auto *node =
      ctx.create<TernaryOpNode>(static_cast<ExprNode *>(dispatch(n->condition)),
                                static_cast<ExprNode *>(dispatch(n->trueExpr)),
                                static_cast<ExprNode *>(dispatch(n->falseExpr)),
                                n->line, n->column, n->length);
  node->promotedType = cloneType(n->promotedType);
  node->exprType = cloneType(n->exprType);
  node->isLValue = n->isLValue;
  node->hasParens = n->hasParens;
  node->representedType = cloneType(n->representedType);
  return node;
}

ASTNode *ASTCloner::visit(const AssignNode *n) {
  auto *node = ctx.create<AssignNode>(
      n->op, static_cast<ExprNode *>(dispatch(n->target)),
      static_cast<ExprNode *>(dispatch(n->value)), n->line, n->column,
      n->length);
  node->isFieldInit = n->isFieldInit;
  return node;
}

ASTNode *ASTCloner::visit(const ArrayLiteralNode *n) {
  auto *node = ctx.create<ArrayLiteralNode>(cloneArray(n->elements), n->line,
                                            n->column, n->length);
  node->hasTrailingComma = n->hasTrailingComma;
  return node;
}

ASTNode *ASTCloner::visit(const MapLiteralNode *n) {
  auto *node = ctx.create<MapLiteralNode>(cloneArray(n->keys),
                                          cloneArray(n->values), n->line,
                                          n->column, n->length);
  node->hasTrailingComma = n->hasTrailingComma;
  return node;
}

ASTNode *ASTCloner::visit(const ArraySubscriptNode *n) {
  return ctx.create<ArraySubscriptNode>(
      static_cast<ExprNode *>(dispatch(n->base)),
      static_cast<ExprNode *>(dispatch(n->index)), n->line, n->column,
      n->length);
}

ASTNode *ASTCloner::visit(const MemberAccessNode *n) {
  auto *node = ctx.create<MemberAccessNode>(
      static_cast<ExprNode *>(dispatch(n->object)), n->memberName, n->line,
      n->column, n->length);
  std::vector<const Type *> tArgs;
  for (auto *ta : n->templateArgs)
    tArgs.push_back(cloneType(ta));
  node->templateArgs = ctx.copyArray<const Type *>(tArgs);
  node->isSuperAccess = n->isSuperAccess;
  return node;
}

ASTNode *ASTCloner::visit(const FunctionCallNode *n) {
  auto *node = ctx.create<FunctionCallNode>(
      static_cast<ExprNode *>(dispatch(n->target)), cloneArray(n->args),
      ctx.copyArray<std::string_view>(n->argNames), n->line, n->column,
      n->length);
  node->rawArgs = cloneArray(n->rawArgs);
  node->rawArgNames = ctx.copyArray<std::string_view>(n->rawArgNames);
  node->hasRawArgs = n->hasRawArgs;
  node->hasTrailingComma = n->hasTrailingComma;
  node->isSuperCall = n->isSuperCall;
  if (n->loweredNew) {
    node->loweredNew =
        llvm::dyn_cast_or_null<NewExprNode>(dispatch(n->loweredNew));
  }
  return node;
}

ASTNode *ASTCloner::visit(const CastNode *n) {
  auto *node = ctx.create<CastNode>(static_cast<ExprNode *>(dispatch(n->expr)),
                                    cloneType(n->targetType), n->line,
                                    n->column, n->length);
  node->rawTargetTypeStr = n->rawTargetTypeStr;
  node->conversionConstructor = n->conversionConstructor;
  return node;
}

ASTNode *ASTCloner::visit(const IsExprNode *n) {
  auto *node = ctx.create<IsExprNode>(static_cast<ExprNode *>(dispatch(n->expr)),
                                      cloneType(n->targetType), n->line,
                                      n->column, n->length);
  node->isNegated = n->isNegated;
  node->rawTargetTypeStr = n->rawTargetTypeStr;
  return node;
}

ASTNode *ASTCloner::visit(const NewExprNode *n) {
  auto *node = ctx.create<NewExprNode>(
      cloneType(n->allocatedType),
      n->arraySize ? static_cast<ExprNode *>(dispatch(n->arraySize)) : nullptr,
      cloneArray(n->args), ctx.copyArray<std::string_view>(n->argNames),
      n->hasParens, n->line, n->column, n->length);
  node->rawAllocatedTypeStr = n->rawAllocatedTypeStr;
  node->rawArgs = cloneArray(n->rawArgs);
  node->rawArgNames = ctx.copyArray<std::string_view>(n->rawArgNames);
  node->hasRawArgs = n->hasRawArgs;
  node->hasTrailingComma = n->hasTrailingComma;
  node->placementExpr =
      n->placementExpr ? static_cast<ExprNode *>(dispatch(n->placementExpr))
                       : nullptr;
  node->implicitCopyInit = n->implicitCopyInit;
  node->allocator = n->allocator;
  node->deallocator = n->deallocator;
  return node;
}

ASTNode *ASTCloner::visit(const DeleteExprNode *n) {
  auto *node = ctx.create<DeleteExprNode>(
      static_cast<ExprNode *>(dispatch(n->ptr)), n->isArray, n->line,
      n->column, n->length);
  node->deallocator = n->deallocator;
  return node;
}

ASTNode *ASTCloner::visit(const ConstExprNode *n) {
  auto *node = ctx.create<ConstExprNode>(
      static_cast<ExprNode *>(dispatch(n->expr)), n->line, n->column,
      n->length);
  node->isConstExpr = n->isConstExpr;
  node->constKey = n->constKey;
  node->exprType = cloneType(n->exprType);
  node->isLValue = n->isLValue;
  return node;
}

ASTNode *ASTCloner::visit(const DestructorCallNode *n) {
  auto *node = ctx.create<DestructorCallNode>(
      static_cast<ExprNode *>(dispatch(n->object)), cloneType(n->targetType),
      n->line, n->column, n->length);
  node->destructor = n->destructor;
  return node;
}

ASTNode *ASTCloner::visit(const BlockNode *n) {
  auto *b = ctx.create<BlockNode>(n->line, n->column);
  b->statements = cloneArray(n->statements);
  b->length = n->length;
  b->endLine = n->endLine;
  b->isExpressionBody = n->isExpressionBody;
  b->hasBraces = n->hasBraces;
  return b;
}

ASTNode *ASTCloner::visit(const IfNode *n) {
  auto *node =
      ctx.create<IfNode>(static_cast<ExprNode *>(dispatch(n->condition)),
                         static_cast<BlockNode *>(dispatch(n->thenBlock)),
                         n->elseBlock ? dispatch(n->elseBlock) : nullptr,
                         n->line, n->column, n->length);
  node->endLine = n->endLine;
  return node;
}

ASTNode *ASTCloner::visit(const ForNode *n) {
  auto *node = ctx.create<ForNode>(
      n->initStatement ? dispatch(n->initStatement) : nullptr,
      n->condition ? static_cast<ExprNode *>(dispatch(n->condition)) : nullptr,
      n->increment ? static_cast<ExprNode *>(dispatch(n->increment)) : nullptr,
      static_cast<BlockNode *>(dispatch(n->body)), n->line, n->column,
      n->length);
  node->endLine = n->endLine;
  return node;
}

ASTNode *ASTCloner::visit(const WhileNode *n) {
  auto *node =
      ctx.create<WhileNode>(static_cast<ExprNode *>(dispatch(n->condition)),
                            static_cast<BlockNode *>(dispatch(n->body)),
                            n->line, n->column, n->length);
  node->endLine = n->endLine;
  return node;
}

ASTNode *ASTCloner::visit(const SwitchNode *n) {
  auto *s = ctx.create<SwitchNode>(
      static_cast<ExprNode *>(dispatch(n->condition)), cloneArray(n->cases),
      n->hasDefault, n->line, n->column, n->length);
  s->endLine = n->endLine;
  return s;
}

ASTNode *ASTCloner::visit(const CaseNode *n) {
  auto *node = ctx.create<CaseNode>(
      n->value ? static_cast<ExprNode *>(dispatch(n->value)) : nullptr,
      cloneArray(n->statements), n->line, n->column, n->length);
  node->endLine = n->endLine;
  return node;
}

ASTNode *ASTCloner::visit(const BreakNode *n) {
  return ctx.create<BreakNode>(n->line, n->column, n->length);
}

ASTNode *ASTCloner::visit(const ContinueNode *n) {
  return ctx.create<ContinueNode>(n->line, n->column, n->length);
}

ASTNode *ASTCloner::visit(const ReturnNode *n) {
  return ctx.create<ReturnNode>(
      n->value ? static_cast<ExprNode *>(dispatch(n->value)) : nullptr, n->line,
      n->column, n->length);
}

ASTNode *ASTCloner::visit(const VarDeclNode *n) {
  auto *node = ctx.create<VarDeclNode>(
      cloneType(n->type), n->varName,
      n->initializer ? static_cast<ExprNode *>(dispatch(n->initializer))
                     : nullptr,
      n->line, n->column, n->length);
  node->isStatic = n->isStatic;
  node->isWeak = n->isWeak;
  node->isExtern = n->isExtern;
  node->isFinal = n->isFinal;
  node->externAlias = n->externAlias;
  node->hasPublicMod = n->hasPublicMod;
  node->hasPrivateMod = n->hasPrivateMod;
  node->hasProtectedMod = n->hasProtectedMod;
  node->annotations = n->annotations;
  node->docString = n->docString;
  node->trailingComment = n->trailingComment;
  node->rawTypeStr = n->rawTypeStr;
  node->endLine = n->endLine;
  node->isInitialized = n->isInitialized;
  node->alignment = n->alignment;
  node->isPacked = n->isPacked;
  return node;
}

ASTNode *ASTCloner::visit(const ParamDeclNode *n) {
  auto *node = ctx.create<ParamDeclNode>(
      cloneType(n->type), n->name,
      n->defaultValue ? static_cast<ExprNode *>(dispatch(n->defaultValue))
                      : nullptr,
      n->isNamed, n->isRequired, n->line, n->column, n->length);
  node->isThisParam = n->isThisParam;
  node->hasPublicMod = n->hasPublicMod;
  node->hasPrivateMod = n->hasPrivateMod;
  node->hasProtectedMod = n->hasProtectedMod;
  node->annotations = n->annotations;
  node->docString = n->docString;
  node->trailingComment = n->trailingComment;
  node->rawTypeStr = n->rawTypeStr;
  node->alignment = n->alignment;
  node->isPacked = n->isPacked;
  return node;
}

ASTNode *ASTCloner::visit(const FunctionDeclNode *n) {
  auto *node = ctx.create<FunctionDeclNode>(
      cloneType(n->returnType), n->name, n->line, n->column, n->isConst,
      n->isMethod, n->isExtern, n->isVariadic, n->isImplicit);
  node->params = cloneArray(n->params);
  if (n->superCall)
    node->superCall = static_cast<FunctionCallNode *>(dispatch(n->superCall));
  if (!n->fieldInitializers.empty()) {
    std::vector<AssignNode *> inits;
    for (const auto *init : n->fieldInitializers)
      inits.push_back(static_cast<AssignNode *>(dispatch(init)));
    node->fieldInitializers = ctx.copyArray<AssignNode *>(inits);
  }
  if (n->body)
    node->body = static_cast<BlockNode *>(dispatch(n->body));
  node->isStatic = n->isStatic;
  node->isWeak = n->isWeak;
  node->externAlias = n->externAlias;
  node->callingConv = n->callingConv;
  node->hasPublicMod = n->hasPublicMod;
  node->hasPrivateMod = n->hasPrivateMod;
  node->hasProtectedMod = n->hasProtectedMod;
  node->annotations = n->annotations;
  node->docString = n->docString;
  node->trailingComment = n->trailingComment;
  node->declFilePath = n->declFilePath;
  node->length = n->length;
  node->templateParams = n->templateParams;
  /* A method inside a template class keeps its own template parameters
   * (e.g. 'static Future<R> value<R>(R value)') when the class-level
   * parameters are substituted; it must stay a template so its body is not
   * eagerly type-checked with unbound parameters. */
  node->isTemplate = !n->templateParams.empty();
  node->parentRecord = n->parentRecord;
  node->rawReturnTypeStr = n->rawReturnTypeStr;
  node->endLine = n->endLine;
  node->alignment = n->alignment;
  node->isPacked = n->isPacked;
  node->hasTrailingComma = n->hasTrailingComma;

  node->isIntrinsic = n->isIntrinsic;
  node->isVirtual = n->isVirtual;
  node->isOverride = n->isOverride;
  node->isAbstract = n->isAbstract;
  node->isAsync = n->isAsync;
  node->intrinsicName = n->intrinsicName;

  return node;
}

ASTNode *ASTCloner::visit(const UnionDeclNode *n) {
  auto *node =
      ctx.create<UnionDeclNode>(n->name, n->line, n->column, n->length);
  node->fields = cloneArray(n->fields);
  node->methods = cloneArray(n->methods);
  node->constructors = cloneArray(n->constructors);
  if (n->destructor)
    node->destructor = static_cast<FunctionDeclNode *>(dispatch(n->destructor));
  node->hasPublicMod = n->hasPublicMod;
  node->hasPrivateMod = n->hasPrivateMod;
  node->hasProtectedMod = n->hasProtectedMod;
  node->annotations = n->annotations;
  node->docString = n->docString;
  node->trailingComment = n->trailingComment;
  node->declFilePath = n->declFilePath;
  node->isOpaque = n->isOpaque;
  node->isTemplate = false;
  node->endLine = n->endLine;
  node->alignment = n->alignment;
  node->isPacked = n->isPacked;
  return node;
}

ASTNode *ASTCloner::visit(const ClassDeclNode *n) {
  auto *node =
      ctx.create<ClassDeclNode>(n->name, n->line, n->column, n->length);
  node->fields = cloneArray(n->fields);
  node->methods = cloneArray(n->methods);
  node->constructors = cloneArray(n->constructors);
  if (n->destructor)
    node->destructor = static_cast<FunctionDeclNode *>(dispatch(n->destructor));

  if (n->baseClass)
    node->baseClass = cloneType(n->baseClass);

  std::vector<const Type *> clonedInterfaces;
  for (auto *i : n->interfaces)
    clonedInterfaces.push_back(cloneType(i));
  node->interfaces = ctx.copyArray<const Type *>(clonedInterfaces);

  node->hasPublicMod = n->hasPublicMod;
  node->hasPrivateMod = n->hasPrivateMod;
  node->hasProtectedMod = n->hasProtectedMod;
  node->annotations = n->annotations;
  node->docString = n->docString;
  node->trailingComment = n->trailingComment;
  node->declFilePath = n->declFilePath;
  node->isOpaque = n->isOpaque;
  node->isTemplate = false;
  node->isAbstract = n->isAbstract;
  node->isFinal = n->isFinal;
  node->endLine = n->endLine;
  node->alignment = n->alignment;
  node->isPacked = n->isPacked;
  return node;
}

ASTNode *ASTCloner::visit(const StructDeclNode *n) {
  auto *node =
      ctx.create<StructDeclNode>(n->name, n->line, n->column, n->length);
  node->fields = cloneArray(n->fields);
  node->methods = cloneArray(n->methods);
  node->constructors = cloneArray(n->constructors);
  if (n->destructor)
    node->destructor = static_cast<FunctionDeclNode *>(dispatch(n->destructor));
  node->hasPublicMod = n->hasPublicMod;
  node->hasPrivateMod = n->hasPrivateMod;
  node->hasProtectedMod = n->hasProtectedMod;
  node->annotations = n->annotations;
  node->docString = n->docString;
  node->trailingComment = n->trailingComment;
  node->declFilePath = n->declFilePath;
  node->isOpaque = n->isOpaque;
  node->isTemplate = false;
  node->endLine = n->endLine;
  node->alignment = n->alignment;
  node->isPacked = n->isPacked;
  return node;
}

ASTNode *ASTCloner::visit(const NamespaceDeclNode *n) {
  auto *node =
      ctx.create<NamespaceDeclNode>(n->name, n->line, n->column, n->length);
  node->fqName = n->fqName;
  node->isFileScoped = n->isFileScoped;
  node->statements = cloneArray(n->statements);
  return node;
}

ASTNode *ASTCloner::visit(const UsingNode *n) {
  return ctx.create<UsingNode>(n->name, n->line, n->column, n->length);
}

ASTNode *ASTCloner::visit(const ModuleNode *n) { return nullptr; }
ASTNode *ASTCloner::visit(const TypedefDeclNode *n) { return nullptr; }
ASTNode *ASTCloner::visit(const EnumDeclNode *n) { return nullptr; }
ASTNode *ASTCloner::visit(const EnumMemberNode *n) { return nullptr; }
ASTNode *ASTCloner::visit(const AnnotationDeclNode *n) { return nullptr; }
ASTNode *ASTCloner::visit(const AnnotationNode *n) { return nullptr; }
ASTNode *ASTCloner::visit(const ImplicitCastNode *n) { return nullptr; }

} // namespace utopia