# Intrinsics

Utopia provides compile-time intrinsics for type introspection, declared with `@intrinsic` and implemented by the compiler (`Intrinsics.cpp`).

## `sizeof`

Evaluated at compile time to a `usize` constant:

```utp
usize a = sizeof(int32);      // 4
usize b = sizeof(Widget);     // record size
usize c = sizeof(T);          // template parameter (in generics)
```

## `typeof`

Produces a **constant** `Type` reflection value (see [Memory & Reflection](../stdlib/memory.md)):

```utp
Type t = typeof(Widget);
print("%s\n", t.name);
if (t.isClass)    { ... }
if (t.isPointer)  { ... }
```

The reflected `Type` fields: `name`, `isClass`, `isStruct`, `isPrimitive`, `isEnum`, `isArray`, `isPointer`.

## `MethodInfo`

Method reflection is available for records through the same machinery, enabling runtime inspection of a record's methods.

## Declaring custom intrinsics

`@intrinsic("name")` declares a function with no body that the compiler implements:

```utp
@intrinsic("sizeof_type")
usize sizeof(Type t);
```

The prelude (`Memory/Core.utp`) declares the standard set:

```utp
usize sizeof(Type);
usize sizeof<T>(T);
Type typeof(Type);
Type typeof<T>(T);
```

## Use cases

- Generic code that needs size/alignment information per type.
- Serialization and reflection-driven tooling.
- Assertions and debug builds: `assert(sizeof(int64) == 8)`.
