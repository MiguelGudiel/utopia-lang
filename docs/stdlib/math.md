# Math

The stdlib's `math.utp` module (imported with `import "utopia:math";`)
provides the `Math` class and re-exports the numeric limits constants from
`limits.utp`.

## Integer limits

| Constant | Value |
| --- | --- |
| `INT8_MAX` / `INT8_MIN` | 127 / -128 |
| `UINT8_MAX` | 255 |
| `INT16_MAX` / `INT16_MIN` | 32767 / -32768 |
| `UINT16_MAX` | 65535 |
| `INT32_MAX` / `INT32_MIN` | 2147483647 / -2147483648 |
| `UINT32_MAX` | 4294967295 |
| `INT64_MAX` / `INT64_MIN` | 9223372036854775807 / -9223372036854775808 |
| `UINT64_MAX` | 18446744073709551615 |

## Floating point limits

| Constant | Value |
| --- | --- |
| `FLOAT32_MAX` / `FLOAT32_MIN` | 3.4028235e+38 / 1.17549435e-38 |
| `FLOAT64_MAX` / `FLOAT64_MIN` | 1.7976931348623157e+308 / 2.2250738585072014e-308 |

## Usage

```utp
import "utopia:math";

int main() {
  print("INT32_MAX = %d\n", INT32_MAX);
  print("UINT64_MAX = %lu\n", UINT64_MAX);
  if (value > INT32_MAX) {
    print("overflow!\n");
  }
  return 0;
}
```
