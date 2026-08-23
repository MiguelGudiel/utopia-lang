# String

`String` is the prelude's dynamic string type. It owns its buffer and releases it on destruction. It stores UTF-8 bytes; the case conversion, trimming, reversal and rune operations decode codepoints, while indexing, `length()` and `substring` work on byte offsets.

## Construction

```utp
String empty;                    // default
String fromC = "hello";          // from a C string literal
String fromInt = String(42);     // numeric conversion ("42")
String fromFloat = String(3.14); // "3.14"
String fromBool = String(true);  // "true"
String fromChar = String.fromCharCode(0x1F600);  // one codepoint as UTF-8
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

## Formatting

`String.format` is printf-style; assign the result to a `String` to take an owning copy (the runtime reuses an internal last-result buffer):

```utp
String msg = String.format("%d and %s and %.2f", 42, "text", 3.14159);
```

For formatted output without keeping the string, use the global `print`.

## Methods

| Method | Signature | Description |
| --- | --- | --- |
| `length()` | `usize length()` | Length in bytes |
| `capacity()` | `usize capacity()` | Allocated capacity |
| `c_str()` | `const uint8* c_str()` | NUL-terminated C buffer (for `%s` printing / FFI) |
| `format` | `static const uint8* format(const uint8* fmt, ...)` | printf-style formatting |
| `padLeft` / `padRight` | `String padLeft(usize width, const uint8* padding)` | Pad with a repeated string |
| `repeat` | `String repeat(usize times)` | Concatenate this string with itself |
| `reversed` | `String reversed()` | Codepoints in reverse order (UTF-8 safe) |
| `parse` | `int64 parse(int32 radix = 0)` | Parse as integer (2..36; 0 auto-detects `0x`/`0o`/`0b`) |
| `tryParse` | `bool tryParse(int32 radix, int64& out)` | Parse without throwing; false when no digits parse |
| `toInt()` | `int32 toInt()` | Parse as integer (`atoi`) |
| `toFloat()` | `float64 toFloat()` | Parse as float (`atof`) |
| `fromCharCode` | `static String fromCharCode(int32 cp)` | One Unicode codepoint as UTF-8 |
| `runes()` | `List<int32> runes()` | All codepoints |
| `runeCount()` | `usize runeCount()` | Number of codepoints |
| `codePointAt()` | `int32 codePointAt(usize index)` | Codepoint starting at a byte offset |
| `toUpperCase()` | `String toUpperCase()` | Unicode-aware uppercase (Latin, Greek, Cyrillic; `ß` -> `SS`) |
| `toLowerCase()` | `String toLowerCase()` | Unicode-aware lowercase (`İ` -> `i` + combining dot) |
| `trim()` / `trimStart()` / `trimEnd()` | `String` | Unicode whitespace trimming |
| `clear()` | `void clear()` | Empty the string |
| `push_back()` | `void push_back(uint8 c)` | Append one byte |

## StringBuilder

`StringBuilder` builds a string from many pieces in O(n) total: it appends into a single buffer that grows geometrically, where repeated `+` concatenation reallocates on every operation.

```utp
StringBuilder sb;
sb.append("score: ");
sb.append(42);
sb.append(" / ");
sb.append(100);
String report = sb.toString();    // owning copy; sb can be cleared and reused
```

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

## Notes

- `String` stores bytes (UTF-8); `rune` literals are 32-bit Unicode code points.
- `operator[]` returns the byte at an index (no bounds check).
- Comparisons against C strings work in both directions (`"abc" == string`).
- Case mapping covers ASCII, Latin-1/Extended, Greek and Cyrillic; other scripts pass through unchanged.
