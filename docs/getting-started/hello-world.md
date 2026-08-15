# Hello, World!

Utopia uses `int main()` as the program entry point. The global `print` function, available through the automatically loaded prelude, provides `printf`-style formatted output.

```utp
int main() {
  print("Hello, World!\n");
  return 0;
}
```

## Running the program

Every Utopia program lives in a project with a `build.yaml` manifest:

```yaml
project:
  name: "hello-world"

build:
  target: executable
  source_dirs:
    - src/
  sources:
    - "main.utp"
```

Compile and link:

```sh
utopia build path/to/project
```

The resulting executable is written to `build/bin/hello-world`.

To compile and run immediately through the in-process JIT:

```sh
utopia run path/to/project
```

## Variadic formatted printing

`print` accepts any number of arguments and applies C-style format specifiers:

```utp
int main() {
  int x = 42;
  double pi = 3.14159;
  print("x = %d, pi = %.2f\n", x, pi);
  return 0;
}
```

Supported specifiers follow the C `printf` family (`%d`, `%f`, `%s`, `%x`, ...).

## Comments

Utopia supports both line and block comments, and `///` doc comments that editors and the LSP surface:

```utp
// Line comment
/* Block
   comment */

/// Doc comment for the function below
int add(int a, int b) => a + b;
```
