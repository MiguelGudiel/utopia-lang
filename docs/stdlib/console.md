# Console

`Console` provides terminal input and output, backed by the C stdio functions.

## Global `print`

The prelude declares `print` as a variadic `printf` binding, available in every module:

```utp
print("Hello, World!\n");
print("%d %s %.2f\n", 42, "text", 3.14159);
```

## Console class

| Method | Signature | Description |
| --- | --- | --- |
| `print` | `static void print(const String& text)` | Print text without a newline |
| `printLine` | `static void printLine(const String& text)` | Print text with a newline |
| `error` | `static void error(const String& text)` | Print text to stderr without a newline |
| `errorLine` | `static void errorLine(const String& text)` | Print text and a newline to stderr |
| `readLine` | `static String readLine()` | Read a line from stdin (strips `\r\n`) |
| `clear` | `static void clear()` | Clear the terminal (ANSI escape) |

## Example

```utp
int main() {
  Console.printLine("What is your name?");
  String name = Console.readLine();

  Console.print("Hello, ");
  Console.printLine(name);
  return 0;
}
```

## Interactive example (Fibonacci)

```utp
int main() {
  print("How many Fibonacci numbers? ");
  String input = Console.readLine();
  int n = input.toInt();

  int a = 0, b = 1;
  for (int i = 0; i < n; i++) {
    print("%d ", a);
    int next = a + b;
    a = b;
    b = next;
  }
  print("\n");
  return 0;
}
```
