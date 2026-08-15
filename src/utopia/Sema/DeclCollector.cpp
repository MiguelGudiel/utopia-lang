#include "utopia/CodeGen/Mangler.hpp"
#include "utopia/Sema/Sema.hpp"

namespace utopia {

/* Processes a method's annotations for the pieces the template early-return
 * paths skip: 'intrinsic', 'extern', 'weak' and 'export' influence how the
 * method is compiled, so they must be recorded even while the method's body
 * is not type-checked eagerly. */
static void collectMethodAnnotations(FunctionDeclNode *method,
                                     SemaContext *ctx) {
  bool isExport = false;

  for (const auto *ann : method->annotations) {
    if (ann->name == "export") {
      isExport = true;
    }

    if (ann->name == "intrinsic") {
      method->isIntrinsic = true;
      if (!ann->args.empty() && ann->args[0]->kind == NodeKind::String) {
        method->intrinsicName =
            static_cast<const StringNode *>(ann->args[0])->value;
      } else {
        method->intrinsicName = method->name;
      }
    }

    if (ann->name == "weak") {
      method->isWeak = true;
    }

    if (ann->name == "extern") {
      if (ann->args.empty()) {
        method->externAlias = method->name;
        method->callingConv = "cdecl";
      } else if (ann->args.size() <= 2) {
        if (ann->args[0]->kind == NodeKind::String) {
          method->externAlias =
              static_cast<const StringNode *>(ann->args[0])->value;
        } else {
          ctx->reportError(ann->args[0]->line, ann->args[0]->column,
                           ann->args[0]->length,
                           "First argument of @extern must be a string "
                           "literal.");
        }

        if (ann->args.size() == 2) {
          if (ann->args[1]->kind == NodeKind::String) {
            std::string_view cc =
                static_cast<const StringNode *>(ann->args[1])->value;
            if (cc == "cdecl" || cc == "stdcall" || cc == "fastcall") {
              method->callingConv = cc;
            } else {
              ctx->reportError(ann->args[1]->line, ann->args[1]->column,
                               ann->args[1]->length,
                               "Calling convention must be 'cdecl', "
                               "'stdcall', or 'fastcall'.");
            }
          } else {
            ctx->reportError(ann->args[1]->line, ann->args[1]->column,
                             ann->args[1]->length,
                             "Second argument of @extern must be a string "
                             "literal.");
          }
        } else {
          method->callingConv = "cdecl";
        }
      } else {
        ctx->reportError(ann->line, ann->column, ann->length,
                         "The @extern annotation accepts at most two string "
                         "literal arguments.");
      }
    }
  }

  if (method->isExtern) {
    if (method->externAlias.empty())
      method->externAlias = method->name;
    method->mangledName = std::string(method->externAlias);
  } else if (isExport) {
    method->mangledName = std::string(method->name);
  }
}

void DeclCollectorPass::visit(const NamespaceDeclNode *node) {
  std::string currentNs = ctx->getCurrentNamespace();
  std::string fullNs = currentNs.empty()
                           ? std::string(node->name)
                           : currentNs + "." + std::string(node->name);

  size_t start = 0;
  while (true) {
    size_t dot = fullNs.find('.', start);
    std::string part =
        (dot == std::string::npos) ? fullNs : fullNs.substr(0, dot);

    auto *nsNode =
        ctx->astCtx.getOrCreateNamespace(ctx->astCtx.copyString(part));
    ctx->addDecl(part, nsNode);

    if (dot == std::string::npos)
      break;
    start = dot + 1;
  }

  ctx->pushNamespace(node->name);

  /* Snapshot using directives to prevent leaking from this namespace block */
  size_t prevUsings = ctx->getUsingsCount();

  for (auto *stmt : node->statements) {
    dispatch(stmt);
  }

  /* Restore previous using directives count */
  ctx->resizeUsings(prevUsings);
  ctx->popNamespace();
}

void DeclCollectorPass::visit(const UsingNode *node) {
  ctx->addUsing(node->name);
}

bool DeclCollectorPass::run(const ModuleNode *module, SemaContext &context) {
  ctx = &context;
  dispatch(module);
  return !ctx->hasErrors();
}

void DeclCollectorPass::visit(const ModuleNode *node) {
  if (visitedModules.contains(node))
    return;
  visitedModules.insert(node);

  /* Snapshot using directives to prevent leaking imports from this module */
  size_t prevUsings = ctx->getUsingsCount();

  for (const auto *imp : node->importedModules) {
    dispatch(imp);
  }
  for (const auto *exp : node->exportedModules) {
    dispatch(exp);
  }

  auto prevFile = ctx->currentFile;
  auto prevMod = ctx->currentModule;
  ctx->setCurrentFile(node->filePath);
  ctx->currentModule = node;

  for (const auto &stmt : node->statements) {
    if (stmt->kind == NodeKind::FunctionDecl ||
        stmt->kind == NodeKind::VarDecl || stmt->kind == NodeKind::StructDecl ||
        stmt->kind == NodeKind::UnionDecl ||
        stmt->kind == NodeKind::ClassDecl ||
        stmt->kind == NodeKind::AnnotationDecl ||
        stmt->kind == NodeKind::TypedefDecl ||
        stmt->kind == NodeKind::EnumDecl ||
        stmt->kind == NodeKind::NamespaceDecl ||
        stmt->kind == NodeKind::Using) {
      dispatch(stmt);
    }
  }

  ctx->setCurrentFile(prevFile);
  ctx->currentModule = prevMod;

  /* Restore using directives to avoid polluting the global context */
  ctx->resizeUsings(prevUsings);
}

void DeclCollectorPass::visit(const TypedefDeclNode *node) {
  ctx->addDecl(node->fqName, node);
  if (node->aliasType) {
    node->aliasType->setDeclaration(node);
  }
}

void DeclCollectorPass::visit(const AnnotationDeclNode *node) {
  if (node->declFilePath.empty()) {
    const_cast<AnnotationDeclNode *>(node)->declFilePath = ctx->currentFile;
  }
  ctx->addDecl(node->fqName, node);

  auto *recTy = ctx->astCtx.getRecordType(node->fqName);
  const_cast<AnnotationDeclNode *>(node)->recordType = recTy;
  recTy->setDeclaration(node);

  for (auto *field : node->fields) {
    if (field->declFilePath.empty()) {
      const_cast<VarDeclNode *>(field)->declFilePath = ctx->currentFile;
    }
  }

  if (node->constructor) {
    if (node->constructor->declFilePath.empty()) {
      const_cast<FunctionDeclNode *>(node->constructor)->declFilePath =
          ctx->currentFile;
    }
    const_cast<FunctionDeclNode *>(node->constructor)->mangledName =
        Mangler::mangle(node->constructor, std::string(node->fqName));
    ctx->addDecl(node->fqName, node->constructor);
  }
}

void DeclCollectorPass::visit(const FunctionDeclNode *node) {
  if (node->isTemplate) {
    if (node->declFilePath.empty()) {
      const_cast<FunctionDeclNode *>(node)->declFilePath = ctx->currentFile;
    }
    collectMethodAnnotations(const_cast<FunctionDeclNode *>(node), ctx);
    ctx->templateRegistry[node->name] = node;
    ctx->templateRegistry[node->fqName] = node;
    return;
  }

  bool isExport = false;

  for (const auto *ann : node->annotations) {
    if (ann->name == "intrinsic") {
      const_cast<FunctionDeclNode *>(node)->isIntrinsic = true;
      if (!ann->args.empty() && ann->args[0]->kind == NodeKind::String) {
        const_cast<FunctionDeclNode *>(node)->intrinsicName =
            static_cast<const StringNode *>(ann->args[0])->value;
      } else {
        const_cast<FunctionDeclNode *>(node)->intrinsicName = node->name;
      }
    }

    if (ann->name == "export") {
      isExport = true;
    }

    if (ann->name == "weak") {
      const_cast<FunctionDeclNode *>(node)->isWeak = true;
    }

    if (ann->name == "extern") {
      if (ann->args.empty()) {
        const_cast<FunctionDeclNode *>(node)->externAlias = node->name;
        const_cast<FunctionDeclNode *>(node)->callingConv = "cdecl";
      } else if (ann->args.size() <= 2) {
        if (ann->args[0]->kind == NodeKind::String) {
          const_cast<FunctionDeclNode *>(node)->externAlias =
              static_cast<const StringNode *>(ann->args[0])->value;
        } else {
          ctx->reportError(
              ann->args[0]->line, ann->args[0]->column, ann->args[0]->length,
              "First argument of @extern must be a string literal.");
        }

        if (ann->args.size() == 2) {
          if (ann->args[1]->kind == NodeKind::String) {
            std::string_view cc =
                static_cast<const StringNode *>(ann->args[1])->value;
            if (cc == "cdecl" || cc == "stdcall" || cc == "fastcall") {
              const_cast<FunctionDeclNode *>(node)->callingConv = cc;
            } else {
              ctx->reportError(ann->args[1]->line, ann->args[1]->column,
                               ann->args[1]->length,
                               "Calling convention must be 'cdecl', 'stdcall', "
                               "or 'fastcall'.");
            }
          } else {
            ctx->reportError(
                ann->args[1]->line, ann->args[1]->column, ann->args[1]->length,
                "Second argument of @extern must be a string literal.");
          }
        } else {
          const_cast<FunctionDeclNode *>(node)->callingConv = "cdecl";
        }
      } else {
        ctx->reportError(ann->line, ann->column, ann->length,
                         "The @extern annotation accepts at most two string "
                         "literal arguments.");
      }
    }
  }

  if (node->isExtern) {
    if (node->externAlias.empty()) {
      const_cast<FunctionDeclNode *>(node)->externAlias = node->name;
    }
    const_cast<FunctionDeclNode *>(node)->mangledName =
        std::string(node->externAlias);
  } else if (isExport) {
    const_cast<FunctionDeclNode *>(node)->mangledName = std::string(node->name);
  } else if (node->mangledName.empty()) {
    const_cast<FunctionDeclNode *>(node)->mangledName = Mangler::mangle(node);
  }

  ctx->addDecl(node->fqName, node);
}

void DeclCollectorPass::visit(const IfNode *node) {
  dispatch(node->condition);
  dispatch(node->thenBlock);
  if (node->elseBlock)
    dispatch(node->elseBlock);
}

void DeclCollectorPass::visit(const ForNode *node) {
  if (node->initStatement)
    dispatch(node->initStatement);
  if (node->condition)
    dispatch(node->condition);
  if (node->increment)
    dispatch(node->increment);
  dispatch(node->body);
}

void DeclCollectorPass::visit(const WhileNode *node) {
  dispatch(node->condition);
  dispatch(node->body);
}

void DeclCollectorPass::visit(const SwitchNode *node) {
  dispatch(node->condition);
  for (const auto *c : node->cases) {
    if (c->value) {
      dispatch(c->value);
    }
    for (const auto *stmt : c->statements) {
      dispatch(stmt);
    }
  }
}

void DeclCollectorPass::visit(const VarDeclNode *node) {
  bool isExtern = false;
  for (const auto *ann : node->annotations) {
    if (ann->name == "extern") {
      isExtern = true;
      if (ann->args.empty()) {
        const_cast<VarDeclNode *>(node)->externAlias = node->varName;
      } else if (ann->args.size() == 1 &&
                 ann->args[0]->kind == NodeKind::String) {
        const_cast<VarDeclNode *>(node)->externAlias =
            static_cast<const StringNode *>(ann->args[0])->value;
      } else {
        ctx->reportError(ann->line, ann->column, ann->length,
                         "The @extern annotation for variables accepts at most "
                         "one string literal argument.");
      }
    }
  }

  if (isExtern) {
    const_cast<VarDeclNode *>(node)->isExtern = true;
    const_cast<VarDeclNode *>(node)->mangledName =
        std::string(node->externAlias);
  }

  ctx->addDecl(node->fqName, node);
}

void DeclCollectorPass::visit(const UnionDeclNode *node) {
  if (node->isTemplate) {
    if (node->declFilePath.empty()) {
      const_cast<UnionDeclNode *>(node)->declFilePath = ctx->currentFile;
    }
    for (auto *method : node->methods) {
      if (method->declFilePath.empty()) {
        const_cast<FunctionDeclNode *>(method)->declFilePath =
            ctx->currentFile;
      }
      collectMethodAnnotations(const_cast<FunctionDeclNode *>(method), ctx);
    }
    ctx->templateRegistry[node->name] = node;
    ctx->templateRegistry[node->fqName] = node;
    /* Template records are also visible as identifiers so that static
     * member access on the type ('Future<int>.value(...)') resolves. */
    ctx->addDecl(node->name, node);
    if (node->fqName != node->name)
      ctx->addDecl(node->fqName, node);
    return;
  }

  if (node->declFilePath.empty()) {
    const_cast<UnionDeclNode *>(node)->declFilePath = ctx->currentFile;
  }

  ctx->addDecl(node->fqName, node);

  auto *recTy = ctx->astCtx.getRecordType(node->fqName);
  const_cast<UnionDeclNode *>(node)->recordType = recTy;
  recTy->setDeclaration(node);

  if (node->isOpaque)
    return;

  for (auto *field : node->fields) {
    if (field->declFilePath.empty()) {
      const_cast<VarDeclNode *>(field)->declFilePath = ctx->currentFile;
    }
    if (field->isStatic) {
      const_cast<VarDeclNode *>(field)->mangledName =
          Mangler::mangle(field, std::string(node->fqName));
    }
  }

  if (node->destructor && node->destructor->isImplicit) {
    const_cast<FunctionDeclNode *>(node->destructor)->hasPublicMod =
        node->hasPublicMod;
    const_cast<FunctionDeclNode *>(node->destructor)->hasPrivateMod =
        node->hasPrivateMod;
  }

  for (auto *ctor : node->constructors) {
    if (ctor->declFilePath.empty()) {
      const_cast<FunctionDeclNode *>(ctor)->declFilePath = ctx->currentFile;
    }
    if (ctor->isImplicit) {
      const_cast<FunctionDeclNode *>(ctor)->hasPublicMod = node->hasPublicMod;
      const_cast<FunctionDeclNode *>(ctor)->hasPrivateMod = node->hasPrivateMod;
    }
    const_cast<FunctionDeclNode *>(ctor)->mangledName =
        Mangler::mangle(ctor, std::string(node->fqName));
    ctx->addDecl(node->fqName, ctor);
  }

  if (node->destructor) {
    if (node->destructor->declFilePath.empty()) {
      const_cast<FunctionDeclNode *>(node->destructor)->declFilePath =
          ctx->currentFile;
    }
    const_cast<FunctionDeclNode *>(node->destructor)->mangledName =
        Mangler::mangle(node->destructor, std::string(node->fqName));
  }

  for (auto *method : node->methods) {
    if (method->declFilePath.empty()) {
      const_cast<FunctionDeclNode *>(method)->declFilePath = ctx->currentFile;
    }
    bool isExport = false;

    for (const auto *ann : method->annotations) {
      if (ann->name == "export") {
        isExport = true;
      }

      if (ann->name == "intrinsic") {
        const_cast<FunctionDeclNode *>(method)->isIntrinsic = true;
        if (!ann->args.empty() && ann->args[0]->kind == NodeKind::String) {
          const_cast<FunctionDeclNode *>(method)->intrinsicName =
              static_cast<const StringNode *>(ann->args[0])->value;
        } else {
          const_cast<FunctionDeclNode *>(method)->intrinsicName =
              method->name;
        }
      }

      if (ann->name == "extern") {
        if (ann->args.empty()) {
          const_cast<FunctionDeclNode *>(method)->externAlias = method->name;
          const_cast<FunctionDeclNode *>(method)->callingConv = "cdecl";
        } else if (ann->args.size() <= 2) {
          if (ann->args[0]->kind == NodeKind::String) {
            const_cast<FunctionDeclNode *>(method)->externAlias =
                static_cast<const StringNode *>(ann->args[0])->value;
          } else {
            ctx->reportError(
                ann->args[0]->line, ann->args[0]->column, ann->args[0]->length,
                "First argument of @extern must be a string literal.");
          }

          if (ann->args.size() == 2) {
            if (ann->args[1]->kind == NodeKind::String) {
              std::string_view cc =
                  static_cast<const StringNode *>(ann->args[1])->value;
              if (cc == "cdecl" || cc == "stdcall" || cc == "fastcall") {
                const_cast<FunctionDeclNode *>(method)->callingConv = cc;
              } else {
                ctx->reportError(ann->args[1]->line, ann->args[1]->column,
                                 ann->args[1]->length,
                                 "Calling convention must be 'cdecl', "
                                 "'stdcall', or 'fastcall'.");
              }
            } else {
              ctx->reportError(
                  ann->args[1]->line, ann->args[1]->column,
                  ann->args[1]->length,
                  "Second argument of @extern must be a string literal.");
            }
          } else {
            const_cast<FunctionDeclNode *>(method)->callingConv = "cdecl";
          }
        } else {
          ctx->reportError(ann->line, ann->column, ann->length,
                           "The @extern annotation accepts at most two string "
                           "literal arguments.");
        }
      }
    }

    if (method->isExtern) {
      if (method->externAlias.empty()) {
        const_cast<FunctionDeclNode *>(method)->externAlias = method->name;
      }
      const_cast<FunctionDeclNode *>(method)->mangledName =
          std::string(method->externAlias);
    } else if (isExport) {
      const_cast<FunctionDeclNode *>(method)->mangledName =
          std::string(method->name);
    } else {
      const_cast<FunctionDeclNode *>(method)->mangledName =
          Mangler::mangle(method, std::string(node->fqName));
    }
  }
}

void DeclCollectorPass::visit(const StructDeclNode *node) {
  if (node->isTemplate) {
    if (node->declFilePath.empty()) {
      const_cast<StructDeclNode *>(node)->declFilePath = ctx->currentFile;
    }
    for (auto *method : node->methods) {
      if (method->declFilePath.empty()) {
        const_cast<FunctionDeclNode *>(method)->declFilePath =
            ctx->currentFile;
      }
      collectMethodAnnotations(const_cast<FunctionDeclNode *>(method), ctx);
    }
    ctx->templateRegistry[node->name] = node;
    ctx->templateRegistry[node->fqName] = node;
    /* Template records are also visible as identifiers so that static
     * member access on the type ('Future<int>.value(...)') resolves. */
    ctx->addDecl(node->name, node);
    if (node->fqName != node->name)
      ctx->addDecl(node->fqName, node);
    return;
  }

  if (node->declFilePath.empty()) {
    const_cast<StructDeclNode *>(node)->declFilePath = ctx->currentFile;
  }

  ctx->addDecl(node->fqName, node);

  auto *recTy = ctx->astCtx.getRecordType(node->fqName);
  const_cast<StructDeclNode *>(node)->recordType = recTy;
  recTy->setDeclaration(node);

  if (node->isOpaque)
    return;

  for (auto *field : node->fields) {
    if (field->declFilePath.empty()) {
      const_cast<VarDeclNode *>(field)->declFilePath = ctx->currentFile;
    }
    if (field->isStatic) {
      const_cast<VarDeclNode *>(field)->mangledName =
          Mangler::mangle(field, std::string(node->fqName));
    }
  }

  if (node->destructor && node->destructor->isImplicit) {
    const_cast<FunctionDeclNode *>(node->destructor)->hasPublicMod =
        node->hasPublicMod;
    const_cast<FunctionDeclNode *>(node->destructor)->hasPrivateMod =
        node->hasPrivateMod;
  }

  for (auto *ctor : node->constructors) {
    if (ctor->declFilePath.empty()) {
      const_cast<FunctionDeclNode *>(ctor)->declFilePath = ctx->currentFile;
    }
    if (ctor->isImplicit) {
      const_cast<FunctionDeclNode *>(ctor)->hasPublicMod = node->hasPublicMod;
      const_cast<FunctionDeclNode *>(ctor)->hasPrivateMod = node->hasPrivateMod;
    }
    const_cast<FunctionDeclNode *>(ctor)->mangledName =
        Mangler::mangle(ctor, std::string(node->fqName));
    ctx->addDecl(node->fqName, ctor);
  }

  if (node->destructor) {
    if (node->destructor->declFilePath.empty()) {
      const_cast<FunctionDeclNode *>(node->destructor)->declFilePath =
          ctx->currentFile;
    }
    const_cast<FunctionDeclNode *>(node->destructor)->mangledName =
        Mangler::mangle(node->destructor, std::string(node->fqName));
  }

  for (auto *method : node->methods) {
    if (method->declFilePath.empty()) {
      const_cast<FunctionDeclNode *>(method)->declFilePath = ctx->currentFile;
    }
    bool isExport = false;

    for (const auto *ann : method->annotations) {
      if (ann->name == "export") {
        isExport = true;
      }

      if (ann->name == "intrinsic") {
        const_cast<FunctionDeclNode *>(method)->isIntrinsic = true;
        if (!ann->args.empty() && ann->args[0]->kind == NodeKind::String) {
          const_cast<FunctionDeclNode *>(method)->intrinsicName =
              static_cast<const StringNode *>(ann->args[0])->value;
        } else {
          const_cast<FunctionDeclNode *>(method)->intrinsicName =
              method->name;
        }
      }

      if (ann->name == "extern") {
        if (ann->args.empty()) {
          const_cast<FunctionDeclNode *>(method)->externAlias = method->name;
          const_cast<FunctionDeclNode *>(method)->callingConv = "cdecl";
        } else if (ann->args.size() <= 2) {
          if (ann->args[0]->kind == NodeKind::String) {
            const_cast<FunctionDeclNode *>(method)->externAlias =
                static_cast<const StringNode *>(ann->args[0])->value;
          } else {
            ctx->reportError(
                ann->args[0]->line, ann->args[0]->column, ann->args[0]->length,
                "First argument of @extern must be a string literal.");
          }

          if (ann->args.size() == 2) {
            if (ann->args[1]->kind == NodeKind::String) {
              std::string_view cc =
                  static_cast<const StringNode *>(ann->args[1])->value;
              if (cc == "cdecl" || cc == "stdcall" || cc == "fastcall") {
                const_cast<FunctionDeclNode *>(method)->callingConv = cc;
              } else {
                ctx->reportError(ann->args[1]->line, ann->args[1]->column,
                                 ann->args[1]->length,
                                 "Calling convention must be 'cdecl', "
                                 "'stdcall', or 'fastcall'.");
              }
            } else {
              ctx->reportError(
                  ann->args[1]->line, ann->args[1]->column,
                  ann->args[1]->length,
                  "Second argument of @extern must be a string literal.");
            }
          } else {
            const_cast<FunctionDeclNode *>(method)->callingConv = "cdecl";
          }
        } else {
          ctx->reportError(ann->line, ann->column, ann->length,
                           "The @extern annotation accepts at most two string "
                           "literal arguments.");
        }
      }
    }

    if (method->isExtern) {
      if (method->externAlias.empty()) {
        const_cast<FunctionDeclNode *>(method)->externAlias = method->name;
      }
      const_cast<FunctionDeclNode *>(method)->mangledName =
          std::string(method->externAlias);
    } else if (isExport) {
      const_cast<FunctionDeclNode *>(method)->mangledName =
          std::string(method->name);
    } else {
      const_cast<FunctionDeclNode *>(method)->mangledName =
          Mangler::mangle(method, std::string(node->fqName));
    }
  }
}

void DeclCollectorPass::visit(const ClassDeclNode *node) {
  if (node->isTemplate) {
    if (node->declFilePath.empty()) {
      const_cast<ClassDeclNode *>(node)->declFilePath = ctx->currentFile;
    }
    for (auto *method : node->methods) {
      if (method->declFilePath.empty()) {
        const_cast<FunctionDeclNode *>(method)->declFilePath =
            ctx->currentFile;
      }
      collectMethodAnnotations(const_cast<FunctionDeclNode *>(method), ctx);
    }
    ctx->templateRegistry[node->name] = node;
    ctx->templateRegistry[node->fqName] = node;
    /* Template records are also visible as identifiers so that static
     * member access on the type ('Future<int>.value(...)') resolves. */
    ctx->addDecl(node->name, node);
    if (node->fqName != node->name)
      ctx->addDecl(node->fqName, node);
    return;
  }

  if (node->declFilePath.empty()) {
    const_cast<ClassDeclNode *>(node)->declFilePath = ctx->currentFile;
  }

  ctx->addDecl(node->fqName, node);

  auto *recTy = ctx->astCtx.getRecordType(node->fqName);
  const_cast<ClassDeclNode *>(node)->recordType = recTy;
  recTy->setDeclaration(node);

  if (node->isOpaque)
    return;

  for (auto *field : node->fields) {
    if (field->declFilePath.empty()) {
      const_cast<VarDeclNode *>(field)->declFilePath = ctx->currentFile;
    }
    if (field->isStatic) {
      const_cast<VarDeclNode *>(field)->mangledName =
          Mangler::mangle(field, std::string(node->fqName));
    }
  }

  if (node->destructor && node->destructor->isImplicit) {
    const_cast<FunctionDeclNode *>(node->destructor)->hasPublicMod =
        node->hasPublicMod;
    const_cast<FunctionDeclNode *>(node->destructor)->hasPrivateMod =
        node->hasPrivateMod;
  }

  for (auto *ctor : node->constructors) {
    if (ctor->declFilePath.empty()) {
      const_cast<FunctionDeclNode *>(ctor)->declFilePath = ctx->currentFile;
    }
    if (ctor->isImplicit) {
      const_cast<FunctionDeclNode *>(ctor)->hasPublicMod = node->hasPublicMod;
      const_cast<FunctionDeclNode *>(ctor)->hasPrivateMod = node->hasPrivateMod;
    }
    const_cast<FunctionDeclNode *>(ctor)->mangledName =
        Mangler::mangle(ctor, std::string(node->fqName));
    ctx->addDecl(node->fqName, ctor);
  }
  if (node->destructor) {
    if (node->destructor->declFilePath.empty()) {
      const_cast<FunctionDeclNode *>(node->destructor)->declFilePath =
          ctx->currentFile;
    }
    const_cast<FunctionDeclNode *>(node->destructor)->mangledName =
        Mangler::mangle(node->destructor, std::string(node->fqName));
  }
  for (auto *method : node->methods) {
    if (method->declFilePath.empty()) {
      const_cast<FunctionDeclNode *>(method)->declFilePath = ctx->currentFile;
    }
    bool isExport = false;

    for (const auto *ann : method->annotations) {
      if (ann->name == "export") {
        isExport = true;
      }

      if (ann->name == "intrinsic") {
        const_cast<FunctionDeclNode *>(method)->isIntrinsic = true;
        if (!ann->args.empty() && ann->args[0]->kind == NodeKind::String) {
          const_cast<FunctionDeclNode *>(method)->intrinsicName =
              static_cast<const StringNode *>(ann->args[0])->value;
        } else {
          const_cast<FunctionDeclNode *>(method)->intrinsicName =
              method->name;
        }
      }

      if (ann->name == "extern") {
        if (ann->args.empty()) {
          const_cast<FunctionDeclNode *>(method)->externAlias = method->name;
          const_cast<FunctionDeclNode *>(method)->callingConv = "cdecl";
        } else if (ann->args.size() <= 2) {
          if (ann->args[0]->kind == NodeKind::String) {
            const_cast<FunctionDeclNode *>(method)->externAlias =
                static_cast<const StringNode *>(ann->args[0])->value;
          } else {
            ctx->reportError(
                ann->args[0]->line, ann->args[0]->column, ann->args[0]->length,
                "First argument of @extern must be a string literal.");
          }

          if (ann->args.size() == 2) {
            if (ann->args[1]->kind == NodeKind::String) {
              std::string_view cc =
                  static_cast<const StringNode *>(ann->args[1])->value;
              if (cc == "cdecl" || cc == "stdcall" || cc == "fastcall") {
                const_cast<FunctionDeclNode *>(method)->callingConv = cc;
              } else {
                ctx->reportError(ann->args[1]->line, ann->args[1]->column,
                                 ann->args[1]->length,
                                 "Calling convention must be 'cdecl', "
                                 "'stdcall', or 'fastcall'.");
              }
            } else {
              ctx->reportError(
                  ann->args[1]->line, ann->args[1]->column,
                  ann->args[1]->length,
                  "Second argument of @extern must be a string literal.");
            }
          } else {
            const_cast<FunctionDeclNode *>(method)->callingConv = "cdecl";
          }
        } else {
          ctx->reportError(ann->line, ann->column, ann->length,
                           "The @extern annotation accepts at most two string "
                           "literal arguments.");
        }
      }
    }

    if (method->isExtern) {
      if (method->externAlias.empty()) {
        const_cast<FunctionDeclNode *>(method)->externAlias = method->name;
      }
      const_cast<FunctionDeclNode *>(method)->mangledName =
          std::string(method->externAlias);
    } else if (isExport) {
      const_cast<FunctionDeclNode *>(method)->mangledName =
          std::string(method->name);
    } else {
      const_cast<FunctionDeclNode *>(method)->mangledName =
          Mangler::mangle(method, std::string(node->fqName));
    }
  }
}

void DeclCollectorPass::visit(const EnumDeclNode *node) {
  if (node->declFilePath.empty()) {
    const_cast<EnumDeclNode *>(node)->declFilePath = ctx->currentFile;
  }
  ctx->addDecl(node->fqName, node);

  const_cast<EnumDeclNode *>(node)->enumType =
      ctx->astCtx.getEnumType(node->fqName, node->underlyingType);
  const_cast<EnumType *>(node->enumType)->setDeclaration(node);

  for (auto *mem : node->members) {
    if (mem->declFilePath.empty()) {
      const_cast<EnumMemberNode *>(mem)->declFilePath = ctx->currentFile;
    }
  }
}

} // namespace utopia