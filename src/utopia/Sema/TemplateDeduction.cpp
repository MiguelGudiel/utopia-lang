#include "utopia/Sema/Sema.hpp"
#include "utopia/Common/Types.hpp"
#include <algorithm>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace utopia {

namespace {

std::string_view simpleName(std::string_view n) {
  size_t p = n.find_last_of('.');
  return p == std::string_view::npos ? n : n.substr(p + 1);
}

std::string_view recordName(const DeclNode *d) {
  if (auto *c = llvm::dyn_cast<ClassDeclNode>(d))
    return c->name;
  if (auto *s = llvm::dyn_cast<StructDeclNode>(d))
    return s->name;
  if (auto *u = llvm::dyn_cast<UnionDeclNode>(d))
    return u->name;
  return "";
}

/* Alias types are fully transparent for deduction (typedef transparency). */
const Type *unwrapAlias(const Type *t) {
  while (t && t->getKind() == TypeKind::Alias) {
    const Type *target = static_cast<const AliasType *>(t)->getTarget();
    if (!target)
      break;
    t = target;
  }
  return t;
}

/* The name of the template record being deduced, unqualified. */
std::string_view dedRecordName(const DeclNode *d) {
  if (auto *c = llvm::dyn_cast<ClassDeclNode>(d))
    return c->name;
  if (auto *s = llvm::dyn_cast<StructDeclNode>(d))
    return s->name;
  if (auto *u = llvm::dyn_cast<UnionDeclNode>(d))
    return u->name;
  return "";
}

/* Removes the top-level const-qualifier (C++ ignores top-level cv-qualifiers
 * of the argument when the parameter is a plain 'T'). */
const Type *stripTopLevelConst(const Type *t) {
  t = unwrapAlias(t);
  while (t && t->getKind() == TypeKind::Const) {
    t = static_cast<const ConstType *>(t)->getBaseType();
    t = unwrapAlias(t);
  }
  return t;
}

/* Removes top-level reference wrappers (an lvalue of type 'int&' is seen as
 * plain 'int' when the parameter is not itself a reference). */
const Type *stripRefs(const Type *t) {
  while (t && (t->getKind() == TypeKind::Reference ||
               t->getKind() == TypeKind::RValueReference)) {
    t = static_cast<const ReferenceType *>(t)->getPointeeType();
  }
  return t;
}

/* 'Auto' appears in the signature of an unresolved lambda; deduction from
 * such arguments is not possible (the signature is re-typed later once the
 * candidate is matched). */
bool containsAuto(const Type *t) {
  if (!t)
    return false;
  switch (t->getKind()) {
  case TypeKind::Auto:
    return true;
  case TypeKind::Pointer:
    return containsAuto(
        static_cast<const PointerType *>(t)->getPointeeType());
  case TypeKind::Reference:
  case TypeKind::RValueReference:
    return containsAuto(
        static_cast<const ReferenceType *>(t)->getPointeeType());
  case TypeKind::Const:
    return containsAuto(static_cast<const ConstType *>(t)->getBaseType());
  case TypeKind::Array:
    return containsAuto(
        static_cast<const ArrayType *>(t)->getElementType());
  case TypeKind::Function: {
    auto *f = static_cast<const FunctionType *>(t);
    if (containsAuto(f->getReturnType()))
      return true;
    for (const auto *p : f->getParamTypes())
      if (containsAuto(p))
        return true;
    return false;
  }
  case TypeKind::MapLiteral: {
    auto *m = static_cast<const MapLiteralType *>(t);
    return containsAuto(m->getKeyType()) || containsAuto(m->getValueType());
  }
  case TypeKind::TemplateInst: {
    for (const auto *a :
         static_cast<const TemplateInstType *>(t)->getTemplateArgs())
      if (containsAuto(a))
        return true;
    return false;
  }
  default:
    return false;
  }
}

/* Structural type equality used for the consistency of repeated deductions.
 * It never instantiates templates: instantiations are compared by base name
 * and argument list whether they are still raw placeholders
 * (TemplateInstType) or already-resolved record types. */
bool structurallySame(const Type *a, const Type *b) {
  if (a == b)
    return true;
  if (!a || !b)
    return false;

  if (a->getKind() == TypeKind::Alias) {
    return structurallySame(static_cast<const AliasType *>(a)->getTarget(),
                            b);
  }
  if (b->getKind() == TypeKind::Alias) {
    return structurallySame(a, static_cast<const AliasType *>(b)->getTarget());
  }

  auto instKey = [](const Type *t) -> std::optional<std::string> {
    llvm::ArrayRef<const Type *> args;
    std::string_view base;
    if (auto *inst = llvm::dyn_cast<TemplateInstType>(t)) {
      base = inst->getBaseName();
      args = inst->getTemplateArgs();
    } else if (auto *rec = llvm::dyn_cast<RecordType>(t)) {
      if (!rec->isTemplateInstantiation())
        return std::nullopt;
      base = rec->getTemplateBaseName();
      args = rec->getTemplateArgs();
    } else {
      return std::nullopt;
    }
    std::string key = std::string(simpleName(base)) + "<";
    for (size_t i = 0; i < args.size(); ++i) {
      if (i)
        key += ",";
      key += args[i]->toString();
    }
    key += ">";
    return key;
  };

  auto ka = instKey(a);
  auto kb = instKey(b);
  if (ka && kb)
    return *ka == *kb;
  if (ka || kb)
    return false;

  if (a->getKind() != b->getKind())
    return false;

  switch (a->getKind()) {
  case TypeKind::Pointer:
    return structurallySame(
        static_cast<const PointerType *>(a)->getPointeeType(),
        static_cast<const PointerType *>(b)->getPointeeType());
  case TypeKind::Reference:
  case TypeKind::RValueReference:
    return structurallySame(
        static_cast<const ReferenceType *>(a)->getPointeeType(),
        static_cast<const ReferenceType *>(b)->getPointeeType());
  case TypeKind::Const:
    return structurallySame(static_cast<const ConstType *>(a)->getBaseType(),
                            static_cast<const ConstType *>(b)->getBaseType());
  case TypeKind::Array: {
    auto *aa = static_cast<const ArrayType *>(a);
    auto *bb = static_cast<const ArrayType *>(b);
    return aa->getSize() == bb->getSize() &&
           structurallySame(aa->getElementType(), bb->getElementType());
  }
  case TypeKind::Function: {
    auto *af = static_cast<const FunctionType *>(a);
    auto *bf = static_cast<const FunctionType *>(b);
    if (!structurallySame(af->getReturnType(), bf->getReturnType()))
      return false;
    auto pa = af->getParamTypes();
    auto pb = bf->getParamTypes();
    if (pa.size() != pb.size())
      return false;
    for (size_t i = 0; i < pa.size(); ++i)
      if (!structurallySame(pa[i], pb[i]))
        return false;
    return true;
  }
  case TypeKind::MapLiteral: {
    auto *am = static_cast<const MapLiteralType *>(a);
    auto *bm = static_cast<const MapLiteralType *>(b);
    return structurallySame(am->getKeyType(), bm->getKeyType()) &&
           structurallySame(am->getValueType(), bm->getValueType());
  }
  default:
    return a->toString() == b->toString();
  }
}

class TemplateDeducer {
  TypeCheckPass &pass;
  const std::vector<std::string_view> params;
  std::unordered_map<std::string_view, const Type *> deduced;

public:
  TemplateDeducer(TypeCheckPass &pass,
                  llvm::ArrayRef<std::string_view> templateParams,
                  llvm::ArrayRef<const Type *> explicitArgs)
      : pass(pass), params(templateParams.begin(), templateParams.end()) {
    /* Explicit template arguments are a leading prefix; they are fixed and
     * any later deduction must agree with them. */
    for (size_t i = 0; i < explicitArgs.size() && i < params.size(); ++i) {
      const Type *arg = pass.resolveIfTemplate(explicitArgs[i]);
      deduced[params[i]] = arg ? stripTopLevelConst(arg) : arg;
    }
  }

  bool isDeducible(std::string_view name) const {
    return std::find(params.begin(), params.end(), name) != params.end();
  }

  bool isBound(std::string_view name) const {
    return deduced.find(name) != deduced.end();
  }

  bool addDeduction(std::string_view name, const Type *type,
                    std::string &error) {
    const Type *cand = stripTopLevelConst(unwrapAlias(type));
    auto it = deduced.find(name);
    if (it != deduced.end()) {
      if (structurallySame(it->second, cand))
        return true;
      error = "conflicting deductions for template parameter '" +
              std::string(name) + "': deduced '" + it->second->toString() +
              "' and '" + cand->toString() + "'";
      return false;
    }
    deduced[name] = cand;
    return true;
  }

  bool allBound() const {
    for (const auto &p : params)
      if (!deduced.count(p))
        return false;
    return true;
  }

  std::vector<const Type *> finalArgs() const {
    std::vector<const Type *> out;
    out.reserve(params.size());
    for (const auto &p : params)
      out.push_back(deduced.at(p));
    return out;
  }

  static bool argsSame(TypeCheckPass &pass,
                       const std::vector<const Type *> &a,
                       const std::vector<const Type *> &b) {
    (void)pass;
    if (a.size() != b.size())
      return false;
    for (size_t i = 0; i < a.size(); ++i)
      if (!structurallySame(a[i], b[i]))
        return false;
    return true;
  }

  /* Returns true when 't' mentions one of the template parameters being
   * deduced (i.e. the parameter type is dependent). */
  bool containsParam(const Type *t) const {
    if (!t)
      return false;
    switch (t->getKind()) {
    case TypeKind::TemplateParam:
      return isDeducible(
          static_cast<const TemplateParamType *>(t)->getName());
    case TypeKind::Pointer:
      return containsParam(
          static_cast<const PointerType *>(t)->getPointeeType());
    case TypeKind::Reference:
    case TypeKind::RValueReference:
      return containsParam(
          static_cast<const ReferenceType *>(t)->getPointeeType());
    case TypeKind::Const:
      return containsParam(static_cast<const ConstType *>(t)->getBaseType());
    case TypeKind::Array:
      return containsParam(
          static_cast<const ArrayType *>(t)->getElementType());
    case TypeKind::Function: {
      auto *f = static_cast<const FunctionType *>(t);
      if (containsParam(f->getReturnType()))
        return true;
      for (const auto *p : f->getParamTypes())
        if (containsParam(p))
          return true;
      return false;
    }
    case TypeKind::MapLiteral: {
      auto *m = static_cast<const MapLiteralType *>(t);
      return containsParam(m->getKeyType()) || containsParam(m->getValueType());
    }
    case TypeKind::TemplateInst: {
      for (const auto *a :
           static_cast<const TemplateInstType *>(t)->getTemplateArgs())
        if (containsParam(a))
          return true;
      return false;
    }
    case TypeKind::Class:
    case TypeKind::Struct:
    case TypeKind::Union: {
      auto *r = static_cast<const RecordType *>(t);
      if (!r->isTemplateInstantiation())
        return false;
      for (const auto *a : r->getTemplateArgs())
        if (containsParam(a))
          return true;
      return false;
    }
    default:
      return false;
    }
  }

  /* Deduces from 'Box<T>' (either as a raw TemplateInstType or as an
   * already-resolved record instantiation) against the argument type. */
  bool deduceFromTemplateArgs(std::string_view baseName,
                              llvm::ArrayRef<const Type *> pArgs,
                              const Type *A, std::string &error) {
    /* A parameter type without any of our template parameters needs no
     * deduction: ordinary matching/conversion applies later. */
    bool dependent = false;
    for (const auto *a : pArgs)
      if (containsParam(a)) {
        dependent = true;
        break;
      }
    if (!dependent)
      return true;

    enum class Match { Found, Dependent, Mismatch };
    std::optional<llvm::ArrayRef<const Type *>> aArgs;
    std::function<Match(const Type *)> walk = [&](const Type *t) -> Match {
      if (!t)
        return Match::Mismatch;
      if (t->getKind() == TypeKind::TemplateParam)
        return Match::Dependent;
      if (auto *inst = llvm::dyn_cast<TemplateInstType>(t)) {
        if (simpleName(inst->getBaseName()) == simpleName(baseName)) {
          aArgs = inst->getTemplateArgs();
          return Match::Found;
        }
        return Match::Mismatch;
      }
      if (auto *rec = llvm::dyn_cast<RecordType>(t)) {
        if (rec->isTemplateInstantiation() &&
            simpleName(rec->getTemplateBaseName()) == simpleName(baseName)) {
          aArgs = rec->getTemplateArgs();
          return Match::Found;
        }
        /* A derived class satisfies a template-typed parameter ('Base<T>'
         * matches 'Derived : Base<int>'), mirroring C++ deduction. */
        if (rec->getKind() == TypeKind::Class) {
          const Type *base =
              static_cast<const ClassType *>(rec)->getBaseClass();
          if (base)
            return walk(base->getUnqualifiedType());
        }
        return Match::Mismatch;
      }
      return Match::Mismatch;
    };

    Match kind = walk(stripTopLevelConst(A));
    if (kind == Match::Dependent)
      return true; /* dependent argument type: nothing can be deduced */
    if (kind == Match::Mismatch) {
      error = "cannot deduce '" + std::string(simpleName(baseName)) +
              "<...>' from an argument of type '" + A->toString() + "'";
      return false;
    }
    if (pArgs.size() != aArgs->size()) {
      error = "template '" + std::string(simpleName(baseName)) +
              "' takes " + std::to_string(pArgs.size()) +
              " type arguments, but the argument provides " +
              std::to_string(aArgs->size());
      return false;
    }
    for (size_t i = 0; i < pArgs.size(); ++i)
      if (!deduceOne(pArgs[i], (*aArgs)[i], error))
        return false;
    return true;
  }

  /* Core deduction for one parameter/argument pair (P, A). Follows the C++
   * rules: exact matching, top-level cv-qualifiers ignored for plain 'T',
   * references stripped from the argument for non-reference parameters,
   * array/function decay, and memberwise deduction through pointers,
   * references, templates and function types. */
  bool deduceOne(const Type *P, const Type *A, std::string &error) {
    if (!P || !A)
      return false;

    if (P->getKind() == TypeKind::Alias) {
      const Type *t = static_cast<const AliasType *>(P)->getTarget();
      return t ? deduceOne(t, A, error) : true;
    }
    if (A->getKind() == TypeKind::Alias) {
      const Type *t = static_cast<const AliasType *>(A)->getTarget();
      return t ? deduceOne(P, t, error) : false;
    }

    /* An unresolved lambda signature (Auto placeholders) cannot contribute
     * type information; the pair is skipped. */
    if (containsAuto(A))
      return true;

    switch (P->getKind()) {
    case TypeKind::TemplateParam: {
      auto *tp = static_cast<const TemplateParamType *>(P);
      if (!isDeducible(tp->getName()))
        return true; /* foreign template parameter: non-deducible context */
      return addDeduction(tp->getName(), stripRefs(A), error);
    }
    case TypeKind::Const:
      return deduceOne(static_cast<const ConstType *>(P)->getBaseType(),
                       stripTopLevelConst(A), error);
    case TypeKind::Pointer: {
      const Type *A2 = stripTopLevelConst(A);
      /* Arrays and functions decay to pointers when the parameter is a
       * pointer, exactly as in C++ (P = T*, A = U[N] deduces T = U). */
      if (A2->getKind() == TypeKind::Array) {
        return deduceOne(
            static_cast<const PointerType *>(P)->getPointeeType(),
            static_cast<const ArrayType *>(A2)->getElementType(), error);
      }
      if (A2->getKind() == TypeKind::Function) {
        return deduceOne(static_cast<const PointerType *>(P)->getPointeeType(),
                         A2, error);
      }
      if (!A2->isPointerType())
        return false;
      return deduceOne(static_cast<const PointerType *>(P)->getPointeeType(),
                       static_cast<const PointerType *>(A2)->getPointeeType(),
                       error);
    }
    case TypeKind::Reference:
    case TypeKind::RValueReference:
      return deduceOne(static_cast<const ReferenceType *>(P)->getPointeeType(),
                       stripRefs(stripTopLevelConst(A)), error);
    case TypeKind::Array: {
      /* Array parameters decay to pointers, as in C++. */
      const Type *A2 = stripTopLevelConst(A);
      const Type *elemP = static_cast<const ArrayType *>(P)->getElementType();
      if (A2->getKind() == TypeKind::Array)
        return deduceOne(elemP,
                         static_cast<const ArrayType *>(A2)->getElementType(),
                         error);
      if (A2->isPointerType())
        return deduceOne(elemP,
                         static_cast<const PointerType *>(A2)->getPointeeType(),
                         error);
      return false;
    }
    case TypeKind::Function: {
      const Type *A2 = stripTopLevelConst(A);
      if (A2->getKind() != TypeKind::Function)
        return false;
      auto *pFn = static_cast<const FunctionType *>(P);
      auto *aFn = static_cast<const FunctionType *>(A2);
      if (!deduceOne(pFn->getReturnType(), aFn->getReturnType(), error))
        return false;
      auto pP = pFn->getParamTypes();
      auto aP = aFn->getParamTypes();
      if (pP.size() != aP.size())
        return false;
      for (size_t i = 0; i < pP.size(); ++i)
        if (!deduceOne(pP[i], aP[i], error))
          return false;
      return true;
    }
    case TypeKind::MapLiteral: {
      const Type *A2 = stripTopLevelConst(A);
      if (A2->getKind() != TypeKind::MapLiteral)
        return false;
      auto *pM = static_cast<const MapLiteralType *>(P);
      auto *aM = static_cast<const MapLiteralType *>(A2);
      return deduceOne(pM->getKeyType(), aM->getKeyType(), error) &&
             deduceOne(pM->getValueType(), aM->getValueType(), error);
    }
    case TypeKind::TemplateInst: {
      auto *pInst = static_cast<const TemplateInstType *>(P);
      return deduceFromTemplateArgs(pInst->getBaseName(),
                                    pInst->getTemplateArgs(), A, error);
    }
    case TypeKind::Class:
    case TypeKind::Struct:
    case TypeKind::Union: {
      auto *pRec = static_cast<const RecordType *>(P);
      if (!pRec->isTemplateInstantiation())
        return true; /* plain record: no deduction targets inside */
      return deduceFromTemplateArgs(pRec->getTemplateBaseName(),
                                    pRec->getTemplateArgs(), A, error);
    }
    default:
      return true; /* builtins, enums, auto, void: nothing to deduce */
    }
  }

  /* Returns true when 't' is (after stripping references and cv-qualifiers)
   * an instantiation of the class template named 'baseName'. */
  static bool isInstOf(std::string_view baseName, const Type *t) {
    t = unwrapAlias(t);
    while (t && t->getKind() == TypeKind::Const) {
      t = static_cast<const ConstType *>(t)->getBaseType();
      t = unwrapAlias(t);
    }
    while (t && (t->getKind() == TypeKind::Reference ||
                 t->getKind() == TypeKind::RValueReference)) {
      t = static_cast<const ReferenceType *>(t)->getPointeeType();
      t = unwrapAlias(t);
      while (t && t->getKind() == TypeKind::Const) {
        t = static_cast<const ConstType *>(t)->getBaseType();
        t = unwrapAlias(t);
      }
    }
    if (!t)
      return false;
    if (auto *inst = llvm::dyn_cast<TemplateInstType>(t))
      return simpleName(inst->getBaseName()) == simpleName(baseName);
    if (auto *rec = llvm::dyn_cast<RecordType>(t))
      return rec->isTemplateInstantiation() &&
             simpleName(rec->getTemplateBaseName()) == simpleName(baseName);
    return false;
  }

  /* C++ rule [over.match.class.deduct]: when the single initializer is an
   * lvalue or rvalue of the class being deduced, only the copy/move
   * deduction guides are considered. The guides are synthesized from the
   * copy constructor ('Box(const Box<T...>&)'), which deduces every
   * template parameter from the argument's own type arguments. Returns:
   *   1 = rule applied and deduced; 0 = rule applied but deduction failed;
   *  -1 = rule does not apply. */
  static int deduceCopyGuide(TypeCheckPass &pass, const DeclNode *tmplDecl,
                             llvm::ArrayRef<const Type *> argTypes,
                             std::vector<const Type *> &outArgs,
                             std::string &outError) {
    if (argTypes.size() != 1)
      return -1;
    std::string_view baseName = tmplDecl->fqName;
    if (baseName.empty())
      baseName = dedRecordName(tmplDecl);
    if (!isInstOf(baseName, argTypes[0]))
      return -1;

    TemplateDeducer d(pass, tmplDecl->templateParams, {});
    std::vector<const Type *> guideArgs;
    guideArgs.reserve(tmplDecl->templateParams.size());
    for (const auto &p : tmplDecl->templateParams)
      guideArgs.push_back(pass.getContext().astCtx.getTemplateParamType(p));
    if (!d.deduceFromTemplateArgs(
            simpleName(baseName),
            pass.getContext().astCtx.copyArray<const Type *>(guideArgs),
            argTypes[0], outError))
      return 0;
    if (!d.allBound())
      return 0;
    outArgs = d.finalArgs();
    return 1;
  }

  /* Pairs every call argument with the corresponding parameter and deduces.
   * Parameters that receive no argument (defaults, optional named
   * parameters) never participate. The implicit 'this' parameter, when
   * present, is skipped. */
  bool deduceFromCall(const FunctionDeclNode *fn,
                      llvm::ArrayRef<const Type *> argTypes,
                      llvm::ArrayRef<std::string_view> argNames,
                      std::string &error) {
    size_t posIdx = 0;
    for (size_t i = 0; i < argTypes.size(); ++i) {
      size_t pIdx;
      if (!argNames.empty() && !argNames[i].empty()) {
        bool found = false;
        for (size_t p = 0; p < fn->params.size(); ++p) {
          if (fn->params[p]->name == argNames[i]) {
            pIdx = p;
            found = true;
            break;
          }
        }
        if (!found)
          continue; /* invalid named argument: reported later */
      } else {
        pIdx = posIdx++;
      }
      if (pIdx >= fn->params.size())
        continue; /* variadic tail */
      const auto *param = fn->params[pIdx];
      if (!param->type || param->name == "this")
        continue;
      if (!deduceOne(param->type, argTypes[i], error))
        return false;
    }
    return true;
  }
};

} // namespace

bool TypeCheckPass::deduceTemplateArguments(
    const FunctionDeclNode *tmplDecl, llvm::ArrayRef<const Type *> argTypes,
    llvm::ArrayRef<std::string_view> argNames,
    llvm::ArrayRef<const Type *> explicitArgs,
    std::vector<const Type *> &outArgs, std::string &outError) {
  TemplateDeducer d(*this, tmplDecl->templateParams, explicitArgs);
  if (!d.deduceFromCall(tmplDecl, argTypes, argNames, outError)) {
    if (outError.empty()) {
      outError = "the call arguments do not match the template parameter "
                 "types";
    }
    return false;
  }
  if (!d.allBound()) {
    std::string missing;
    for (const auto &p : tmplDecl->templateParams) {
      if (!d.isBound(p)) {
        missing = std::string(p);
        break;
      }
    }
    outError = "couldn't deduce template parameter '" + missing +
               "': no call argument provides type information for it";
    return false;
  }
  outArgs = d.finalArgs();
  return true;
}

bool TypeCheckPass::deduceClassTemplateArguments(
    const DeclNode *tmplDecl, llvm::ArrayRef<const Type *> argTypes,
    llvm::ArrayRef<std::string_view> argNames,
    std::vector<const Type *> &outArgs, std::string &outError) {
  llvm::ArrayRef<FunctionDeclNode *> ctors;
  if (auto *c = llvm::dyn_cast<ClassDeclNode>(tmplDecl))
    ctors = c->constructors;
  else if (auto *s = llvm::dyn_cast<StructDeclNode>(tmplDecl))
    ctors = s->constructors;
  else if (auto *u = llvm::dyn_cast<UnionDeclNode>(tmplDecl))
    ctors = u->constructors;

  /* C++ [over.match.class.deduct]: when the single initializer is an lvalue
   * or rvalue of the class being deduced, only the copy/move deduction
   * guides are considered ('Box b2(b1)' deduces T from 'Box<int>' itself,
   * not from the value constructor). */
  std::vector<const Type *> copyGuideArgs;
  std::string copyGuideErr;
  int copyGuideRes = TemplateDeducer::deduceCopyGuide(
      *this, tmplDecl, argTypes, copyGuideArgs, copyGuideErr);
  if (copyGuideRes == 1) {
    outArgs = copyGuideArgs;
    return true;
  }
  if (copyGuideRes == 0) {
    outError = "could not deduce template arguments for '" +
               std::string(recordName(tmplDecl)) +
               "' from the copy constructor; pass the type arguments "
               "explicitly (e.g. '" +
               std::string(recordName(tmplDecl)) +
               "<...>(...)' or 'new " + std::string(recordName(tmplDecl)) +
               "<...>(...)')";
    return false;
  }

  /* Every constructor is a deduction source; only those that bind every
   * template parameter are viable (C++ deduction-guide semantics). */
  std::vector<std::vector<const Type *>> candidates;
  for (const auto *ctor : ctors) {
    TemplateDeducer d(*this, tmplDecl->templateParams, {});
    std::string err;
    if (d.deduceFromCall(ctor, argTypes, argNames, err) && d.allBound())
      candidates.push_back(d.finalArgs());
  }

  if (candidates.empty()) {
    std::string name(recordName(tmplDecl));
    outError = "could not deduce template arguments for '" + name +
               "' from the constructor call; pass the type arguments "
               "explicitly (e.g. '" + name + "<...>(...)' or 'new " + name +
               "<...>(...)')";
    return false;
  }

  const auto &first = candidates[0];
  for (size_t i = 1; i < candidates.size(); ++i) {
    if (!TemplateDeducer::argsSame(*this, first, candidates[i])) {
      outError = "template argument deduction for '" +
                 std::string(recordName(tmplDecl)) +
                 "' is ambiguous: the constructor arguments match multiple "
                 "instantiations; specify the type arguments explicitly";
      return false;
    }
  }
  outArgs = first;
  return true;
}

} // namespace utopia
