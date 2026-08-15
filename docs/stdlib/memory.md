# Memory & Type Reflection

The prelude's `Memory/Core.utp` module exposes raw allocation and compile-time type reflection.

## Raw allocation

```utp
@extern("malloc")
void* malloc(usize size);

@extern("free")
void free(void* ptr);
```

You normally use `new`/`delete` instead; these bindings exist for FFI and low-level work.

## Type reflection

Utopia provides compile-time `sizeof` and `typeof` intrinsics that produce **constants**.

### `sizeof`

```utp
usize bytes = sizeof(int32);       // 4
usize w = sizeof(Widget);          // record size
usize elem = sizeof(T);            // template parameter
```

### `typeof`

`typeof` returns a reflection constant of type `Type`:

```utp
Type t = typeof(Widget);

if (t.isClass)    { /* ... */ }
if (t.isStruct)   { /* ... */ }
if (t.isEnum)     { /* ... */ }
if (t.isArray)    { /* ... */ }
if (t.isPointer)  { /* ... */ }
if (t.isPrimitive){ /* ... */ }
```

The `Type` struct:

```utp
struct Type {
  public uint8* name;
  public bool isClass;
  public bool isStruct;
  public bool isPrimitive;
  public bool isEnum;
  public bool isArray;
  public bool isPointer;
}
```

`MethodInfo` provides method reflection for records.

## Example

```utp
class Widget {
  public int id;
}

int main() {
  print("int32 size: %d\n", sizeof(int32));
  print("Widget size: %d\n", sizeof(Widget));

  Type t = typeof(Widget);
  print("isClass: %d\n", t.isClass);
  print("name: %s\n", t.name);
  return 0;
}
```
