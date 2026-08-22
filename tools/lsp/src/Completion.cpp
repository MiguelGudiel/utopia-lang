#include "LspCore.hpp"
#include <cctype>
#include <sstream>

namespace utopia::lsp {

namespace {

/* The record whose members the completion site may access: the enclosing
 * function's record when the cursor is inside a method (its private members
 * are visible), null at global scope. */
const RecordType *completionAccessContext(const LocalVarCollector &collector) {
  if (collector.closestFunc && collector.closestFunc->parentRecord)
    return collector.closestFunc->parentRecord;
  return nullptr;
}

/* Record fields/methods are filtered by visibility from the access site;
 * operator overloads are hidden (they are not usable as names). */
void addRecordMembers(const DeclNode *recDecl, const RecordType *recTy,
                      const RecordType *accessContext,
                      const std::function<void(const std::string &, int,
                                               const std::string &,
                                               const std::string &)> &add) {
  if (!recDecl || !recTy)
    return;
  llvm::ArrayRef<VarDeclNode *> fields;
  llvm::ArrayRef<FunctionDeclNode *> methods;
  if (recDecl->kind == NodeKind::ClassDecl) {
    fields = static_cast<const ClassDeclNode *>(recDecl)->fields;
    methods = static_cast<const ClassDeclNode *>(recDecl)->methods;
  } else if (recDecl->kind == NodeKind::StructDecl) {
    fields = static_cast<const StructDeclNode *>(recDecl)->fields;
    methods = static_cast<const StructDeclNode *>(recDecl)->methods;
  } else if (recDecl->kind == NodeKind::UnionDecl) {
    fields = static_cast<const UnionDeclNode *>(recDecl)->fields;
    methods = static_cast<const UnionDeclNode *>(recDecl)->methods;
  } else {
    return;
  }

  for (const auto *f : fields) {
    if (!f->isStatic && isMemberVisible(f, recTy, accessContext)) {
      std::string detail = f->type ? f->type->toString() : "auto";
      add(std::string(f->varName), 5, detail, std::string(f->docString));
    }
  }
  for (const auto *m : methods) {
    if (!m->isStatic && !m->name.starts_with("operator") &&
        isMemberVisible(m, recTy, accessContext)) {
      std::string detail = m->returnType ? m->returnType->toString() : "auto";
      add(std::string(m->name), 2, detail, std::string(m->docString));
    }
  }
}

/* The record type behind a record declaration, or null. */
const RecordType *recordTypeOf(const DeclNode *decl) {
  if (!decl)
    return nullptr;
  if (decl->kind == NodeKind::ClassDecl)
    return static_cast<const ClassDeclNode *>(decl)->recordType;
  if (decl->kind == NodeKind::StructDecl)
    return static_cast<const StructDeclNode *>(decl)->recordType;
  if (decl->kind == NodeKind::UnionDecl)
    return static_cast<const UnionDeclNode *>(decl)->recordType;
  return nullptr;
}

} // namespace

void handleCompletion(const json &req) {
  syncWorker();
  std::string uri = req["params"]["textDocument"]["uri"];
  int line = req["params"]["position"]["line"].get<int>() + 1;
  int reqCol = req["params"]["position"]["character"].get<int>();

  json items = json::array();
  std::unordered_set<std::string> addedLabels;

  auto addCompletion = [&](const std::string &label, int kind,
                           const std::string &detail,
                           const std::string &docString = "") {
    if (addedLabels.contains(label))
      return;
    addedLabels.insert(label);
    json item = {{"label", label}, {"kind", kind}, {"detail", detail}};
    if (!docString.empty())
      item["documentation"] = docString;
    items.push_back(item);
  };

  auto addBuiltInAnnotations = [&]() {
    std::vector<std::string> builtInAnnotations = {
        "extern",          "export",       "intrinsic", "align",
        "packed",          "nodiscard",    "deprecated", "inline",
        "forceInline",     "readnone",     "readonly",  "nosync",
        "nofree",          "willreturn",   "mustprogress", "nocapture",
        "nonnull",         "dereferenceable", "weak"};
    for (const auto &ann : builtInAnnotations) {
      addCompletion(ann, 8, "Built-in Annotation");
    }
  };

  std::string targetLineStr;

  DocumentState doc;
  if (documents.get(uri, doc)) {
    const std::string &text = doc.text;
    int currentLine = 0;
    size_t lineStart = 0;
    for (size_t i = 0; i < text.length(); ++i) {
      if (currentLine == line - 1) {
        lineStart = i;
        break;
      }
      if (text[i] == '\n')
        currentLine++;
    }
    for (size_t i = lineStart;
         i < text.length() && text[i] != '\n' && text[i] != '\r'; ++i) {
      targetLineStr += text[i];
    }
  }

  bool isDotCompletion = false;
  bool isAtCompletion = false;
  std::string triggerWord;

  int cursorIdx = reqCol - 1;
  if (cursorIdx >= (int)targetLineStr.length()) {
    cursorIdx = (int)targetLineStr.length() - 1;
  }

  int tokenStart = cursorIdx;
  while (tokenStart >= 0 && (std::isalnum(targetLineStr[tokenStart]) ||
                             targetLineStr[tokenStart] == '_')) {
    tokenStart--;
  }

  int triggerSymbolIdx = tokenStart;
  while (triggerSymbolIdx >= 0 &&
         std::isspace(targetLineStr[triggerSymbolIdx])) {
    triggerSymbolIdx--;
  }

  if (triggerSymbolIdx >= 0) {
    if (targetLineStr[triggerSymbolIdx] == '.') {
      isDotCompletion = true;
    } else if (targetLineStr[triggerSymbolIdx] == '@') {
      isAtCompletion = true;
    }
  }

  if (isDotCompletion) {
    int idEnd = triggerSymbolIdx - 1;
    while (idEnd >= 0 && std::isspace(targetLineStr[idEnd])) {
      idEnd--;
    }
    int idStart = idEnd;
    while (idStart >= 0 &&
           (std::isalnum(targetLineStr[idStart]) ||
            targetLineStr[idStart] == '_' || targetLineStr[idStart] == '.')) {
      idStart--;
    }
    idStart++;
    if (idStart <= idEnd) {
      triggerWord = targetLineStr.substr(idStart, idEnd - idStart + 1);
    }
  }

  if (isAtCompletion) {
    addBuiltInAnnotations();
  } else if (!isDotCompletion) {
    std::vector<std::string> keywords = {
        "if",        "else",      "while",      "for",      "switch",
        "case",      "default",   "break",      "continue", "return",
        "import",    "export",    "as",         "new",      "delete",
        "struct",    "union",     "class",      "extends",  "implements",
        "enum",      "typedef",   "annotation", "Function", "this",
        "null",      "true",      "false",      "public",   "private",
        "protected", "const",     "static",     "extern",   "required",
        "operator",  "namespace", "using",      "abstract", "final",
        "async",     "await",     "try",        "catch",    "throw",
        "assert",    "super"};

    std::vector<std::string> primitives = {
        "int8",   "int16",   "int32",   "int64",  "uint8", "uint16", "uint32",
        "uint64", "float32", "float64", "bool",   "void",  "String", "char",
        "rune",   "int",     "uint",    "double", "usize"};

    for (const auto &kw : keywords) {
      addCompletion(kw, 14, "Keyword");
    }
    for (const auto &pr : primitives) {
      addCompletion(pr, 7, "Primitive Type");
    }

    std::vector<std::string> macros = {
        "_WIN32", "__APPLE__", "__linux__", "__gnu_linux__", "__ANDROID__",
        "x64",    "x86_64",    "x86",       "arm64",         "arm",
        "__FILE__", "__LINE__"};

    for (const auto &mc : macros) {
      addCompletion(mc, 21, "Preprocessor Macro");
    }

    addBuiltInAnnotations();
  }

  if (doc.ast) {
    GlobalSymbols globals = collectGlobals(doc.ast);

    LocalVarCollector collector(line);
    collector.dispatch(doc.ast);
    const RecordType *accessContext = completionAccessContext(collector);

    if (isDotCompletion && !triggerWord.empty()) {
      const Type *instanceType = nullptr;
      const DeclNode *staticTypeDecl = nullptr;
      std::string currentStaticNs;

      std::vector<std::string> chain;
      std::stringstream ss(triggerWord);
      std::string item;
      while (std::getline(ss, item, '.')) {
        if (!item.empty())
          chain.push_back(item);
      }

      if (!chain.empty()) {
        std::string first = chain[0];

        if (collector.closestFunc) {
          if (first == "this" && collector.closestFunc->parentRecord) {
            instanceType = collector.closestFunc->parentRecord;
          } else {
            for (const auto *p : collector.closestFunc->params) {
              if (p->name == first) {
                instanceType = p->type;
                break;
              }
            }
            if (!instanceType) {
              for (const auto *l : collector.locals) {
                if (l->varName == first) {
                  instanceType = l->type;
                  break;
                }
              }
            }
            /* A bare field of the enclosing record ('_window.') is a valid
             * receiver inside instance methods. */
            if (!instanceType && collector.closestFunc->parentRecord) {
              const DeclNode *recDecl =
                  collector.closestFunc->parentRecord->getDeclaration();
              if (recDecl) {
                llvm::ArrayRef<VarDeclNode *> fields;
                if (recDecl->kind == NodeKind::ClassDecl)
                  fields = static_cast<const ClassDeclNode *>(recDecl)->fields;
                else if (recDecl->kind == NodeKind::StructDecl)
                  fields = static_cast<const StructDeclNode *>(recDecl)->fields;
                else if (recDecl->kind == NodeKind::UnionDecl)
                  fields = static_cast<const UnionDeclNode *>(recDecl)->fields;
                for (const auto *f : fields) {
                  if (f->varName == first) {
                    instanceType = f->type;
                    break;
                  }
                }
              }
            }
          }
        }

        if (!instanceType) {
          auto checkDecls = [&](const std::vector<const DeclNode *> &decls) {
            for (const auto *decl : decls) {
              if (decl->kind == NodeKind::VarDecl &&
                  static_cast<const VarDeclNode *>(decl)->varName == first) {
                instanceType = static_cast<const VarDeclNode *>(decl)->type;
                return true;
              } else if (decl->kind == NodeKind::ClassDecl &&
                         static_cast<const ClassDeclNode *>(decl)->name ==
                             first) {
                staticTypeDecl = decl;
                return true;
              } else if (decl->kind == NodeKind::StructDecl &&
                         static_cast<const StructDeclNode *>(decl)->name ==
                             first) {
                staticTypeDecl = decl;
                return true;
              } else if (decl->kind == NodeKind::UnionDecl &&
                         static_cast<const UnionDeclNode *>(decl)->name ==
                             first) {
                staticTypeDecl = decl;
                return true;
              } else if (decl->kind == NodeKind::EnumDecl &&
                         static_cast<const EnumDeclNode *>(decl)->name ==
                             first) {
                staticTypeDecl = decl;
                return true;
              } else if (decl->kind == NodeKind::NamespaceDecl &&
                         static_cast<const NamespaceDeclNode *>(decl)->name ==
                             first) {
                currentStaticNs =
                    static_cast<const NamespaceDeclNode *>(decl)->fqName;
                staticTypeDecl = decl;
                return true;
              }
            }
            return false;
          };

          if (!checkDecls(globals.rootGlobals)) {
            if (!collector.currentNamespace.empty()) {
              checkDecls(globals.namespaceMembers[collector.currentNamespace]);
            }
            if (!instanceType && !staticTypeDecl) {
              for (const auto &u : collector.activeUsings) {
                if (checkDecls(globals.namespaceMembers[u]))
                  break;
              }
            }
          }
        }

        for (size_t i = 1; i < chain.size(); ++i) {
          std::string part = chain[i];
          const Type *nextInstanceType = nullptr;
          const DeclNode *nextStaticDecl = nullptr;
          std::string nextStaticNs;

          if (instanceType) {
            const Type *unqual = instanceType->getUnqualifiedType();
            while (unqual->isPointerType())
              unqual = static_cast<const PointerType *>(unqual)
                           ->getPointeeType()
                           ->getUnqualifiedType();
            while (unqual->isReferenceType() ||
                   unqual->getKind() == TypeKind::RValueReference) {
              if (unqual->isReferenceType())
                unqual = static_cast<const ReferenceType *>(unqual)
                             ->getPointeeType()
                             ->getUnqualifiedType();
              else
                unqual = static_cast<const RValueReferenceType *>(unqual)
                             ->getPointeeType()
                             ->getUnqualifiedType();
            }
            if (unqual->getKind() == TypeKind::Class ||
                unqual->getKind() == TypeKind::Struct ||
                unqual->getKind() == TypeKind::Union) {
              auto *recTy = static_cast<const RecordType *>(unqual);
              const DeclNode *rDecl = recTy->getDeclaration();
              if (!rDecl) {
                auto findRec =
                    [&](const std::vector<const DeclNode *> &decls) {
                      for (const auto *decl : decls) {
                        if ((decl->kind == NodeKind::ClassDecl &&
                             static_cast<const ClassDeclNode *>(decl)->name ==
                                 recTy->getName()) ||
                            (decl->kind == NodeKind::StructDecl &&
                             static_cast<const StructDeclNode *>(decl)->name ==
                                 recTy->getName()) ||
                            (decl->kind == NodeKind::UnionDecl &&
                             static_cast<const UnionDeclNode *>(decl)->name ==
                                 recTy->getName())) {
                          return decl;
                        }
                      }
                      return (const DeclNode *)nullptr;
                    };
                rDecl = findRec(globals.rootGlobals);
                if (!rDecl) {
                  for (const auto &pair : globals.namespaceMembers) {
                    rDecl = findRec(pair.second);
                    if (rDecl)
                      break;
                  }
                }
              }

              if (rDecl) {
                llvm::ArrayRef<VarDeclNode *> fields;
                if (rDecl->kind == NodeKind::ClassDecl)
                  fields = static_cast<const ClassDeclNode *>(rDecl)->fields;
                else if (rDecl->kind == NodeKind::StructDecl)
                  fields = static_cast<const StructDeclNode *>(rDecl)->fields;
                else if (rDecl->kind == NodeKind::UnionDecl)
                  fields = static_cast<const UnionDeclNode *>(rDecl)->fields;

                for (const auto *f : fields) {
                  if (f->varName == part &&
                      isMemberVisible(f, recTy, accessContext)) {
                    nextInstanceType = f->type;
                    break;
                  }
                }

                /* Rust-style auto-deref for smart pointers: if the member is
                 * not on the record itself, follow operator* chains. */
                if (!nextInstanceType) {
                  const DeclNode *derefDecl = getAutoDerefTarget(rDecl);
                  int derefDepth = 0;
                  while (derefDecl && derefDepth < 8) {
                    llvm::ArrayRef<VarDeclNode *> dFields;
                    if (derefDecl->kind == NodeKind::ClassDecl)
                      dFields =
                          static_cast<const ClassDeclNode *>(derefDecl)->fields;
                    else if (derefDecl->kind == NodeKind::StructDecl)
                      dFields =
                          static_cast<const StructDeclNode *>(derefDecl)->fields;
                    else if (derefDecl->kind == NodeKind::UnionDecl)
                      dFields =
                          static_cast<const UnionDeclNode *>(derefDecl)->fields;

                    for (const auto *f : dFields) {
                      if (f->varName == part) {
                        nextInstanceType = f->type;
                        break;
                      }
                    }
                    if (nextInstanceType)
                      break;
                    derefDecl = getAutoDerefTarget(derefDecl);
                    derefDepth++;
                  }
                }
              }
            }
          } else if (staticTypeDecl) {
            if (staticTypeDecl->kind == NodeKind::NamespaceDecl) {
              for (const auto *decl : globals.namespaceMembers[currentStaticNs]) {
                if (decl->kind == NodeKind::VarDecl &&
                    static_cast<const VarDeclNode *>(decl)->varName == part) {
                  nextInstanceType =
                      static_cast<const VarDeclNode *>(decl)->type;
                  break;
                } else if (decl->kind == NodeKind::ClassDecl &&
                           static_cast<const ClassDeclNode *>(decl)->name ==
                               part) {
                  nextStaticDecl = decl;
                  break;
                } else if (decl->kind == NodeKind::StructDecl &&
                           static_cast<const StructDeclNode *>(decl)->name ==
                               part) {
                  nextStaticDecl = decl;
                  break;
                } else if (decl->kind == NodeKind::UnionDecl &&
                           static_cast<const UnionDeclNode *>(decl)->name ==
                               part) {
                  nextStaticDecl = decl;
                  break;
                } else if (decl->kind == NodeKind::EnumDecl &&
                           static_cast<const EnumDeclNode *>(decl)->name ==
                               part) {
                  nextStaticDecl = decl;
                  break;
                } else if (decl->kind == NodeKind::NamespaceDecl &&
                           static_cast<const NamespaceDeclNode *>(decl)->name ==
                               part) {
                  nextStaticDecl = decl;
                  nextStaticNs =
                      static_cast<const NamespaceDeclNode *>(decl)->fqName;
                  break;
                }
              }
            } else if (staticTypeDecl->kind == NodeKind::ClassDecl ||
                       staticTypeDecl->kind == NodeKind::StructDecl ||
                       staticTypeDecl->kind == NodeKind::UnionDecl) {
              llvm::ArrayRef<VarDeclNode *> fields;
              if (staticTypeDecl->kind == NodeKind::ClassDecl)
                fields = static_cast<const ClassDeclNode *>(staticTypeDecl)
                             ->fields;
              else if (staticTypeDecl->kind == NodeKind::StructDecl)
                fields = static_cast<const StructDeclNode *>(staticTypeDecl)
                             ->fields;
              else if (staticTypeDecl->kind == NodeKind::UnionDecl)
                fields = static_cast<const UnionDeclNode *>(staticTypeDecl)
                             ->fields;
              for (const auto *f : fields) {
                if (f->isStatic && f->varName == part) {
                  nextInstanceType = f->type;
                  break;
                }
              }
            }
          }

          instanceType = nextInstanceType;
          staticTypeDecl = nextStaticDecl;
          currentStaticNs = nextStaticNs;
          if (!instanceType && !staticTypeDecl)
            break;
        }
      }

      if (instanceType) {
        const Type *unqual = instanceType->getUnqualifiedType();
        while (unqual->isPointerType())
          unqual = static_cast<const PointerType *>(unqual)
                       ->getPointeeType()
                       ->getUnqualifiedType();
        while (unqual->isReferenceType() ||
               unqual->getKind() == TypeKind::RValueReference) {
          if (unqual->isReferenceType())
            unqual = static_cast<const ReferenceType *>(unqual)
                         ->getPointeeType()
                         ->getUnqualifiedType();
          else
            unqual = static_cast<const RValueReferenceType *>(unqual)
                         ->getPointeeType()
                         ->getUnqualifiedType();
        }

        if (unqual->getKind() == TypeKind::Class ||
            unqual->getKind() == TypeKind::Struct ||
            unqual->getKind() == TypeKind::Union) {
          auto *recTy = static_cast<const RecordType *>(unqual);
          const DeclNode *rDecl = recTy->getDeclaration();
          if (!rDecl) {
            auto findRec = [&](const std::vector<const DeclNode *> &decls) {
              for (const auto *decl : decls) {
                if ((decl->kind == NodeKind::ClassDecl &&
                     static_cast<const ClassDeclNode *>(decl)->name ==
                         recTy->getName()) ||
                    (decl->kind == NodeKind::StructDecl &&
                     static_cast<const StructDeclNode *>(decl)->name ==
                         recTy->getName()) ||
                    (decl->kind == NodeKind::UnionDecl &&
                     static_cast<const UnionDeclNode *>(decl)->name ==
                         recTy->getName())) {
                  return decl;
                }
              }
              return (const DeclNode *)nullptr;
            };
            rDecl = findRec(globals.rootGlobals);
            if (!rDecl) {
              for (const auto &pair : globals.namespaceMembers) {
                rDecl = findRec(pair.second);
                if (rDecl)
                  break;
              }
            }
          }

          if (rDecl) {
            addRecordMembers(rDecl, recTy, accessContext, addCompletion);

            /* Rust-style auto-deref: also suggest the pointee's members for
             * smart pointers (unique_ptr/shared_ptr/etc.). */
            const DeclNode *derefDecl = getAutoDerefTarget(rDecl);
            int derefDepth = 0;
            while (derefDecl && derefDepth < 8) {
              addRecordMembers(derefDecl, recordTypeOf(derefDecl),
                               accessContext, addCompletion);
              derefDecl = getAutoDerefTarget(derefDecl);
              derefDepth++;
            }
          }
        }
      } else if (staticTypeDecl) {
        if (staticTypeDecl->kind == NodeKind::NamespaceDecl) {
          for (const auto *decl : globals.namespaceMembers[currentStaticNs]) {
            std::string_view declName = decl->fqName;
            size_t dotPos = declName.find_last_of('.');
            if (dotPos != std::string_view::npos)
              declName = declName.substr(dotPos + 1);
            if (declName.starts_with("_"))
              continue;

            if (decl->kind == NodeKind::FunctionDecl) {
              auto *f = static_cast<const FunctionDeclNode *>(decl);
              if (f->name.starts_with("operator"))
                continue;
              std::string detail =
                  f->returnType ? f->returnType->toString() : "auto";
              addCompletion(std::string(f->name), 3, detail,
                            std::string(f->docString));
            } else if (decl->kind == NodeKind::VarDecl) {
              auto *v = static_cast<const VarDeclNode *>(decl);
              std::string detail = v->type ? v->type->toString() : "auto";
              addCompletion(std::string(v->varName), 6, detail,
                            std::string(v->docString));
            } else if (decl->kind == NodeKind::ClassDecl) {
              auto *c = static_cast<const ClassDeclNode *>(decl);
              addCompletion(std::string(c->name), 7, "class",
                            std::string(c->docString));
            } else if (decl->kind == NodeKind::StructDecl) {
              auto *s = static_cast<const StructDeclNode *>(decl);
              addCompletion(std::string(s->name), 22, "struct",
                            std::string(s->docString));
            } else if (decl->kind == NodeKind::UnionDecl) {
              auto *u = static_cast<const UnionDeclNode *>(decl);
              addCompletion(std::string(u->name), 22, "union",
                            std::string(u->docString));
            } else if (decl->kind == NodeKind::EnumDecl) {
              auto *e = static_cast<const EnumDeclNode *>(decl);
              addCompletion(std::string(e->name), 13, "enum",
                            std::string(e->docString));
            } else if (decl->kind == NodeKind::NamespaceDecl) {
              auto *n = static_cast<const NamespaceDeclNode *>(decl);
              addCompletion(std::string(n->name), 9, "namespace",
                            std::string(n->docString));
            }
          }
        } else if (staticTypeDecl->kind == NodeKind::ClassDecl ||
                   staticTypeDecl->kind == NodeKind::StructDecl ||
                   staticTypeDecl->kind == NodeKind::UnionDecl) {
          llvm::ArrayRef<VarDeclNode *> fields;
          llvm::ArrayRef<FunctionDeclNode *> methods;
          if (staticTypeDecl->kind == NodeKind::ClassDecl) {
            fields = static_cast<const ClassDeclNode *>(staticTypeDecl)->fields;
            methods =
                static_cast<const ClassDeclNode *>(staticTypeDecl)->methods;
          } else if (staticTypeDecl->kind == NodeKind::StructDecl) {
            fields = static_cast<const StructDeclNode *>(staticTypeDecl)->fields;
            methods =
                static_cast<const StructDeclNode *>(staticTypeDecl)->methods;
          } else if (staticTypeDecl->kind == NodeKind::UnionDecl) {
            fields = static_cast<const UnionDeclNode *>(staticTypeDecl)->fields;
            methods =
                static_cast<const UnionDeclNode *>(staticTypeDecl)->methods;
          }
          const RecordType *staticRecTy = recordTypeOf(staticTypeDecl);
          for (const auto *f : fields) {
            /* Static members follow the same visibility rules as instance
             * members: private statics (e.g. Console.c_printf) must not be
             * offered to code outside the declaring class. */
            if (f->isStatic &&
                isMemberVisible(f, staticRecTy, accessContext)) {
              std::string detail = f->type ? f->type->toString() : "auto";
              addCompletion(std::string(f->varName), 5, detail,
                            std::string(f->docString));
            }
          }
          for (const auto *m : methods) {
            if (m->isStatic && !m->name.starts_with("operator") &&
                isMemberVisible(m, staticRecTy, accessContext)) {
              std::string detail =
                  m->returnType ? m->returnType->toString() : "auto";
              addCompletion(std::string(m->name), 2, detail,
                            std::string(m->docString));
            }
          }
        } else if (staticTypeDecl->kind == NodeKind::EnumDecl) {
          auto *e = static_cast<const EnumDeclNode *>(staticTypeDecl);
          for (const auto *em : e->members) {
            addCompletion(std::string(em->name), 20, "enum member",
                          std::string(em->docString));
          }
        }
      }
    } else if (!isDotCompletion) {
      if (collector.closestFunc) {
        for (const auto *p : collector.closestFunc->params) {
          if (p->name == "this")
            continue;
          std::string detail = p->type ? p->type->toString() : "auto";
          addCompletion(std::string(p->name), 6, detail,
                        std::string(p->docString));
        }
      }
      for (const auto *l : collector.locals) {
        std::string detail = l->type ? l->type->toString() : "auto";
        addCompletion(std::string(l->varName), 6, detail,
                      std::string(l->docString));
      }

      auto addDeclItems = [&](const std::vector<const DeclNode *> &decls) {
        for (const auto *decl : decls) {
          /* Underscore-prefixed declarations are private by convention
           * (DeclNode::isPublic); the prelude alone contributes hundreds of
           * internal helpers that would drown real suggestions. */
          std::string_view declName = decl->fqName;
          size_t dotPos = declName.find_last_of('.');
          if (dotPos != std::string_view::npos)
            declName = declName.substr(dotPos + 1);
          if (declName.starts_with("_"))
            continue;

          if (decl->kind == NodeKind::FunctionDecl) {
            auto *f = static_cast<const FunctionDeclNode *>(decl);
            if (f->name.starts_with("operator"))
              continue;
            std::string detail =
                f->returnType ? f->returnType->toString() : "auto";
            addCompletion(std::string(f->name), 3, detail,
                          std::string(f->docString));
          } else if (decl->kind == NodeKind::VarDecl) {
            auto *v = static_cast<const VarDeclNode *>(decl);
            std::string detail = v->type ? v->type->toString() : "auto";
            addCompletion(std::string(v->varName), 6, detail,
                          std::string(v->docString));
          } else if (decl->kind == NodeKind::ClassDecl) {
            auto *c = static_cast<const ClassDeclNode *>(decl);
            addCompletion(std::string(c->name), 7, "class",
                          std::string(c->docString));
          } else if (decl->kind == NodeKind::StructDecl) {
            auto *s = static_cast<const StructDeclNode *>(decl);
            addCompletion(std::string(s->name), 22, "struct",
                          std::string(s->docString));
          } else if (decl->kind == NodeKind::UnionDecl) {
            auto *u = static_cast<const UnionDeclNode *>(decl);
            addCompletion(std::string(u->name), 22, "union",
                          std::string(u->docString));
          } else if (decl->kind == NodeKind::EnumDecl) {
            auto *e = static_cast<const EnumDeclNode *>(decl);
            addCompletion(std::string(e->name), 13, "enum",
                          std::string(e->docString));
          } else if (decl->kind == NodeKind::TypedefDecl) {
            auto *t = static_cast<const TypedefDeclNode *>(decl);
            addCompletion(std::string(t->aliasName), 8, "typedef",
                          std::string(t->docString));
          } else if (decl->kind == NodeKind::AnnotationDecl) {
            auto *a = static_cast<const AnnotationDeclNode *>(decl);
            addCompletion(std::string(a->name), 8, "annotation",
                          std::string(a->docString));
          } else if (decl->kind == NodeKind::NamespaceDecl) {
            auto *n = static_cast<const NamespaceDeclNode *>(decl);
            addCompletion(std::string(n->name), 9, "namespace",
                          std::string(n->docString));
          }
        }
      };

      addDeclItems(globals.rootGlobals);
      if (!collector.currentNamespace.empty()) {
        addDeclItems(globals.namespaceMembers[collector.currentNamespace]);
      }
      for (const auto &u : collector.activeUsings) {
        addDeclItems(globals.namespaceMembers[u]);
      }
    }
  }

  sendResponse({{"jsonrpc", "2.0"}, {"id", requestId(req)}, {"result", items}});
}

} // namespace utopia::lsp
