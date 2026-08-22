# Compiler Warnings

Every warning the Utopia compiler emits belongs to a machine-identifiable
family (a *warning kind*). This lets you:

- see a stable code for each warning in the editor and in build output,
- disable individual warning kinds for a project from `build.yaml` (or a
  `build.utp` script),
- suppress a single occurrence in source with a comment directive,
- and, inside the VS Code extension, get quick fixes that remove the
  offending code or add the suppression comments for you.

## Warning kinds

| Kind | Description | Quick fix |
|---|---|---|
| `unused_import` | An `import` directive whose module is never referenced (re-exports are never flagged) | removes the import |
| `unused_function` | A private top-level function that is never called | removes the function |
| `unused_method` | A private method that is never called | removes the method |
| `unused_type` | A private record/enum/type alias that is never referenced | removes the declaration |
| `unused_variable` | A variable that is declared but never used (side-effecting initializers are kept) | removes the declaration or keeps the call |
| `unused_field` | A private field that is never referenced | removes the field when safe |
| `unused_parameter` | A parameter that is never used in the body (names starting with `_`, `this` parameters and override/operator parameters are exempt) | renames to `_name` (positional parameters only) |
| `unused_using` | A `using` directive that never resolves a referenced symbol | removes the directive |
| `nodiscard_ignored` | The return value of an `@nodiscard` function or type is ignored | n/a |
| `deprecated` | Use of a declaration marked `@deprecated` | n/a |
| `implicit_cast` | Binding an r-value to a non-const reference creates a temporary; implicit conversions that lose precision or change signedness | n/a |
| `uninitialized_variable` | Use of a variable before it is initialized | n/a |

The *unused* analyses only flag declarations that live in the analyzed
module tree and can never be referenced from outside it (private symbols,
file-local variables and parameters), so the warnings do not produce false
positives for public library surface.

## Disabling warnings per project (`build.yaml`)

Add a `warnings` section under `build:` with a map of `kind: enabled`:

```yaml
project:
  name: "my-app"

build:
  target: executable
  source_dirs:
    - src/
  warnings:
    unused_import: false
    unused_function: false
```

Or as a list of kinds to disable:

```yaml
build:
  warnings:
    - unused_import
    - unused_function
```

A `build.utp` script can do the same at runtime:

```utp
import "utopia:builder";

main() {
  setWarningEnabled("unused_import", false);
  if (isWarningEnabled("unused_parameter")) {
    // ...
  }
}
```

## In-source suppression directives

Two comment directives control warnings without touching the project
configuration:

```utp
// @ignore-warning unused_import
import "utopia:memory";

// @ignore-warnings unused_variable, unused_function
// (above: file scope; must appear at the very top of the file)

int main() {
  int unused = 1; // @ignore-warning unused_variable
  return 0;
}
```

- `// @ignore-warning <kind>` suppresses the kind **on the next line**
  (or on its own line when placed after code).
- `// @ignore-warnings <kind>[, <kind>...]` suppresses the listed kinds for
  the **whole file** and is only honored in the leading comment block.

## Editor integration

The VS Code extension surfaces every warning with its kind code and a
light-bulb with quick fixes for warnings that have one. Available actions:

1. **Fix this warning**: applies the automatic fix for the warning under
   the cursor.
2. **Fix all `<kind>` warnings in this document**: applies the same fix to
   every warning of that kind.
3. **Fix all warnings with fixes in this document**: applies every
   available fix in one go.
4. **Disable `<kind>` on this line**: inserts
   `// @ignore-warning <kind>` above the line.
5. **Disable `<kind>` in this file**: inserts
   `// @ignore-warnings <kind>` at the top of the file (or extends the
   existing directive).
6. **Disable `<kind>` in this project**: edits the current project's
   `build.yaml` (`warnings: { <kind>: false }`) so the kind stays enabled
   for subprojects.
