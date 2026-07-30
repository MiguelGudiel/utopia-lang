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

ASTNode *ASTCloner::visit(const AssignNode *n) {
  return ctx.create<AssignNode>(n->op,
                                static_cast<ExprNode *>(dispatch(n->target)),
                                static_cast<ExprNode *>(dispatch(n->value)),
                                n->line, n->column, n->length);
}

ASTNode *ASTCloner::visit(const ArrayLiteralNode *n) {
  return ctx.create<ArrayLiteralNode>(cloneArray(n->elements), n->line,
                                      n->column, n->length);
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
  return node;
}

ASTNode *ASTCloner::visit(const FunctionCallNode *n) {
  return ctx.create<FunctionCallNode>(
      static_cast<ExprNode *>(dispatch(n->target)), cloneArray(n->args),
      ctx.copyArray<std::string_view>(n->argNames), n->line, n->column,
      n->length);
}

ASTNode *ASTCloner::visit(const CastNode *n) {
  return ctx.create<CastNode>(static_cast<ExprNode *>(dispatch(n->expr)),
                              cloneType(n->targetType), n->line, n->column,
                              n->length);
}

ASTNode *ASTCloner::visit(const NewExprNode *n) {
  return ctx.create<NewExprNode>(
      cloneType(n->allocatedType),
      n->arraySize ? static_cast<ExprNode *>(dispatch(n->arraySize)) : nullptr,
      cloneArray(n->args), ctx.copyArray<std::string_view>(n->argNames),
      n->hasParens, n->line, n->column, n->length);
}

ASTNode *ASTCloner::visit(const DeleteExprNode *n) {
  return ctx.create<DeleteExprNode>(static_cast<ExprNode *>(dispatch(n->ptr)),
                                    n->isArray, n->line, n->column, n->length);
}

ASTNode *ASTCloner::visit(const BlockNode *n) {
  auto *b = ctx.create<BlockNode>(n->line, n->column);
  b->statements = cloneArray(n->statements);
  b->length = n->length;
  return b;
}

ASTNode *ASTCloner::visit(const IfNode *n) {
  return ctx.create<IfNode>(static_cast<ExprNode *>(dispatch(n->condition)),
                            static_cast<BlockNode *>(dispatch(n->thenBlock)),
                            n->elseBlock ? dispatch(n->elseBlock) : nullptr,
                            n->line, n->column, n->length);
}

ASTNode *ASTCloner::visit(const ForNode *n) {
  return ctx.create<ForNode>(
      n->initStatement ? dispatch(n->initStatement) : nullptr,
      n->condition ? static_cast<ExprNode *>(dispatch(n->condition)) : nullptr,
      n->increment ? static_cast<ExprNode *>(dispatch(n->increment)) : nullptr,
      static_cast<BlockNode *>(dispatch(n->body)), n->line, n->column,
      n->length);
}

ASTNode *ASTCloner::visit(const WhileNode *n) {
  return ctx.create<WhileNode>(static_cast<ExprNode *>(dispatch(n->condition)),
                               static_cast<BlockNode *>(dispatch(n->body)),
                               n->line, n->column, n->length);
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
  node->hasPublicMod = n->hasPublicMod;
  node->hasPrivateMod = n->hasPrivateMod;
  node->annotations = n->annotations;
  node->docString = n->docString;
  return node;
}

ASTNode *ASTCloner::visit(const ParamDeclNode *n) {
  auto *node = ctx.create<ParamDeclNode>(
      cloneType(n->type), n->name,
      n->defaultValue ? static_cast<ExprNode *>(dispatch(n->defaultValue))
                      : nullptr,
      n->isNamed, n->isRequired, n->line, n->column, n->length);
  node->hasPublicMod = n->hasPublicMod;
  node->hasPrivateMod = n->hasPrivateMod;
  node->annotations = n->annotations;
  return node;
}

ASTNode *ASTCloner::visit(const FunctionDeclNode *n) {
  auto *node = ctx.create<FunctionDeclNode>(
      cloneType(n->returnType), n->name, n->line, n->column, n->isConst,
      n->isMethod, n->isExtern, n->isVariadic, n->isImplicit);
  node->params = cloneArray(n->params);
  if (n->body)
    node->body = static_cast<BlockNode *>(dispatch(n->body));
  node->isStatic = n->isStatic;
  node->externAlias = n->externAlias;
  node->hasPublicMod = n->hasPublicMod;
  node->hasPrivateMod = n->hasPrivateMod;
  node->annotations = n->annotations;
  node->docString = n->docString;
  node->declFilePath = n->declFilePath;
  node->length = n->length;
  node->isTemplate = false;
  node->parentRecord = n->parentRecord;
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
  node->hasPublicMod = n->hasPublicMod;
  node->hasPrivateMod = n->hasPrivateMod;
  node->annotations = n->annotations;
  node->docString = n->docString;
  node->declFilePath = n->declFilePath;
  node->isOpaque = n->isOpaque;
  node->isTemplate = false;
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
  node->annotations = n->annotations;
  node->docString = n->docString;
  node->declFilePath = n->declFilePath;
  node->isOpaque = n->isOpaque;
  node->isTemplate = false;
  return node;
}

ASTNode *ASTCloner::visit(const ModuleNode *n) { return nullptr; }
ASTNode *ASTCloner::visit(const TypedefDeclNode *n) { return nullptr; }
ASTNode *ASTCloner::visit(const EnumDeclNode *n) { return nullptr; }
ASTNode *ASTCloner::visit(const EnumMemberNode *n) { return nullptr; }
ASTNode *ASTCloner::visit(const AnnotationDeclNode *n) { return nullptr; }
ASTNode *ASTCloner::visit(const AnnotationNode *n) { return nullptr; }
ASTNode *ASTCloner::visit(const ImplicitCastNode *n) { return nullptr; }

} // namespace utopia