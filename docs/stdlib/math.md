# Math

The stdlib's `math.utp` module (imported with `import "utopia:math";`)
provides the `Math` class and re-exports the numeric limits constants from
`limits.utp`.

## Limits (`limits.utp`)

### Integer limits

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
| `SIZE_MAX` | 18446744073709551615 (largest `usize`) |

### Floating point limits

| Constant | Value |
| --- | --- |
| `FLOAT32_MAX` / `FLOAT32_MIN` | 3.4028235e+38 / 1.17549435e-38 |
| `FLOAT64_MAX` / `FLOAT64_MIN` | 1.7976931348623157e+308 / 2.2250738585072014e-308 |
| `FLOAT32_EPSILON` | 1.1920928955078125e-07 (2^-23) |
| `FLOAT64_EPSILON` | 2.2204460492503131e-16 (2^-52) |
| `EPSILON` | alias of `FLOAT64_EPSILON` |
| `NAN` | IEEE-754 quiet NaN |
| `INFINITY` | +infinity |
| `NEG_INFINITY` | -infinity |

`NAN`, `INFINITY` and `NEG_INFINITY` are compile-time constants built by
the const-evaluable `float_special` intrinsic.

## `Math` methods

Elementary functions: `sqrt`, `pow`, `exp`, `log`, `log10`, `log2`, `sin`,
`cos`, `tan`, `asin`, `acos`, `atan`, `atan2`, `sinh`, `cosh`, `tanh`,
`floor`, `ceil`, `round`, `trunc`, `fmod`, `hypot`, `abs`, `min`, `max`,
`expm1`, `log1p`, `cbrt`, `erf`, `tgamma`, `copysign`, `frexp`.

Statistics (population semantics, template element type `T`):

| Method | Description |
| --- | --- |
| `mean(List<T> data)` | Arithmetic mean (0.0 for an empty list) |
| `variance(List<T> data)` | Population variance (mean of squared deviations, /n) |
| `stddev(List<T> data)` | Population standard deviation (sqrt of variance) |
| `median(List<T> data)` | Middle of a sorted copy (mean of the two middle values for even lengths) |

## Usage

```utp
import "utopia:math";

int main() {
  print("INT32_MAX = %d\n", INT32_MAX);
  print("UINT64_MAX = %lu\n", UINT64_MAX);
  if (value > INT32_MAX) {
    print("overflow!\n");
  }

  List<int32> values = [1, 2, 3, 4];
  print("mean: %g\n", Math.mean(values));

  int32 exp;
  float64 mantissa = Math.frexp(12.5, exp);
  print("12.5 = %g * 2^%d\n", mantissa, exp);

  return 0;
}
```
