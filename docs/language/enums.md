# Enums

Enums are integer-backed named constants with an explicit underlying type.

## Declaration

```utp
enum Color : int8 {
  Red,
  Green,
  Blue
}
```

The underlying type is optional and defaults to `int32`. Members can carry explicit constant initializers, and trailing commas are allowed:

```utp
enum HttpStatus : int16 {
  OK = 200,
  NotFound = 404,
  InternalServerError = 500,
}
```

## Usage

```utp
enum AppState { Loading, Ready, Error }

int main() {
  AppState state = AppState.Ready;

  switch (state) {
    case AppState.Loading:
      print("Loading...\n");
      break;
    case AppState.Ready:
      print("Ready!\n");
      break;
    default:
      print("Unknown\n");
  }
  return 0;
}
```

## Properties

- Members are addressed with the enum type as qualifier: `Color.Red`.
- Enums convert to their underlying integer type and participate in integer arithmetic.
- `switch` over enums is fully supported with `case`/`default`.
- Enum values are constants — they are evaluated at compile time and usable in constant expressions.

## `typedef` aliases

Related to enums, `typedef` provides compile-time type aliases:

```utp
typedef Byte = uint8;
typedef Status = int16;
```
