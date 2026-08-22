# Language Server (LSP)

Utopia ships a language server (`utopia_lsp`) speaking the Language Server Protocol over stdio. It powers editor features in VS Code and other LSP clients.

## Features

| Feature | Details |
| --- | --- |
| **Completion** | Keywords, primitive types, preprocessor macros, built-in annotations, user-defined annotations, global symbols, namespace members, and record members — including **auto-deref through smart pointers** (`up.` suggests both `get()` and the pointee's members) |
| **Hover** | Signatures (return type, parameters, named/`required` info, `const`), doc comments, and type information |
| **Go to definition** | Variables, functions, methods, members, enums, types, and namespaces — including members reached through smart pointers |
| **Signature help** | Parameter list with the active parameter highlighted (triggers on `(` and `,`) |
| **Diagnostics** | Parser and semantic errors streamed as `textDocument/publishDiagnostics` |
| **Semantic tokens** | Full token classification: class, struct, enum, type, function, method, property, variable, parameter, enumMember, macro, keyword, namespace; modifiers: declaration, static, readonly |
| **Formatting** | Document formatting through the built-in formatter |
| **Quick fixes** | Code actions for every fixable warning: fix one, fix all of a kind, fix all fixable, disable on the line / in the file / in the project (see the [warnings guide](../language/warnings.md)) |

## Configuration

- Documents are synchronized as full text.
- Changes are processed on a worker thread with a 200 ms debounce.
- The server loads the real project configuration (`build.yaml`, `yip` packages, host platform macros) so features match actual compilation.

## Running

```sh
utopia_lsp
```

The server communicates over stdin/stdout; no flags are required. Configure your editor to launch it as the Utopia language server (the VS Code extension in the repo wires this up).
