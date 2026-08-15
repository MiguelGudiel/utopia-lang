# String

`String` is the prelude's dynamic string type. It owns its buffer and releases it on destruction.

## Construction

```utp
String empty;                    // default
String fromC = "hello";          // from a C string literal
String fromInt = String(42);     // numeric conversion ("42")
String fromFloat = String(3.14); // "3.14"
String fromBool = String(true);  // "true"
String copy = fromC;             // deep copy
```

## Operations

```utp
String a = "Hello";
String b = " World";

String joined = a + b;              // "Hello World"
String withChar = a + '!';          // "Hello!"
String fromLeft = "pre" + a;        // free-function operator

bool equal = a == "Hello";          // operator==
bool notEqual = a != b;

uint8 ch = a[0];                    // indexing returns a byte
```

## Methods

| Method | Signature | Description |
| --- | --- | --- |
| `length()` | `usize length()` | Length in bytes |
| `capacity()` | `usize capacity()` | Allocated capacity |
| `c_str()` | `const uint8* c_str()` | NUL-terminated C buffer (for `%s` printing / FFI) |
| `toInt()` | `int32 toInt()` | Parse as integer (`atoi`) |
| `toFloat()` | `float64 toFloat()` | Parse as float (`atof`) |
| `clear()` | `void clear()` | Empty the string |
| `push_back()` | `void push_back(uint8 c)` | Append one byte |

## Example

```utp
int main() {
  String message = "Welcome to ";
  message += "Utopia Language!";        // operator+= via operator=(String) + operator+

  print("%s\n", message.c_str());
  print("Length: %d\n", message.length());
  print("First char: %c\n", message[0]);

  String input = Console.readLine();
  int number = input.toInt();
  print("Parsed: %d\n", number);
  return 0;
}
```

## Formatting

For formatted output use the global `print`:

```utp
print("Value: %d, name: %s, pi: %.3f\n", 42, name.c_str(), 3.14159);
```

## Notes

- `String` stores bytes (UTF-8); `rune` literals are 32-bit Unicode code points.
- `operator[]` returns the byte at an index (no bounds check).
- Comparisons against C strings work in both directions (`"abc" == string`).
