# Operators

## Built-in operators

Utopia implements the classic C operator set with full type checking and numeric promotion.

### Arithmetic

```utp
+  -  *  /  %
```

### Bitwise

```utp
|  &  ^  ~  <<  >>
```

### Logical

```utp
&&  ||  !
```

### Comparison

```utp
==  !=  <  >  <=  >=
```

### Assignment

```utp
=   +=  -=  *=  /=  %=  &=  |=  ^=  <<=  >>=
```

### Increment / decrement

```utp
i++;  ++i;  i--;  --i;
```

### Unary

```utp
*p       // dereference
&x       // address-of
-x, +x
```

### Ternary and cast

```utp
cond ? a : b
expr as Type
```

### Type tests: `is` and `is!`

`is` evaluates whether an expression can be treated as a given class type — the
object's dynamic type is `Type` or a subtype of it. `is!` is the negated test:

```utp
Object obj = getAnimal();       // assume Animal* or similar
if (obj is Dog) {
  // true when the object is a Dog or a subclass of Dog
}
if (obj is! Cat) {
  // true when the object is not a Cat (nor a subclass)
}
```

Semantics:

- **Compile-time results**: when the static type already decides the answer
  (`Derived*` with `is Base`, exact type matches, or unrelated types), no
  runtime code is emitted — the result is constant.
- **Runtime checks**: when the static type is a polymorphic base of the tested
  type (or the tested type is an interface the static type may implement), the
  check walks the object's vtable-based type descriptor chain.
- `null is T` is always `false`.
- A runtime check requires a polymorphic class (a class with `@virtual`
  methods or interfaces); otherwise the compiler reports an error.

#### Type promotion

Like Dart, `is` inside an `if` condition narrows the variable's type for the
rest of the block — no new variable is needed:

```utp
void describe(Animal* a) {
  if (a is Dog) {
    // Inside this block a is treated as Dog*, so Dog-only members work:
    print("barks: %s\n", a.makeNoise().c_str());
  }
  if (a is! Dog) { ... } else {
    // In the else of an `is!` test the variable is promoted as well.
  }
}
```

Promotion also flows through `&&` (`if (a is Dog && ...)`) and `||` chains
(the `else` of `a is! T || ...` promotes), and is invalidated by an assignment
to the variable inside the block.

## Operator overloading

Operators are overloaded with `operator<op>` methods on records, or as free functions for operands where neither side is the record.

### Overloadable operators

```utp
operator[]   operator+  operator-  operator*  operator/  operator%
operator==   operator!=  operator<  operator>  operator<=  operator>=
operator=    +=  -=  *=  /=  %=
operator++   operator--
|  &  ^  <<  >>   |=  &=  ^=  <<=  >>=
!  ~
```

### Example: a String-like type

```utp
class Text {
  private String data;

  public Text(String data) { this.data = data; }

  public Text operator+(const Text& other) {
    return Text(this.data + other.data);
  }

  public bool operator==(const Text& other) {
    return this.data == other.data;
  }

  public uint8 operator[](usize index) {
    return this.data[index];
  }
}
```

### Free-function operators

When neither operand is a record with the operator, a global function is used. The standard library uses this for commutative `String` operations:

```utp
bool operator==(const uint8* lhs, const String& rhs) {
  return String(lhs) == rhs;
}
```

### Assignment operators

- `operator=` for records with custom destructors is **required** to assign by value; implicit bitwise copy-assignment of such records is rejected.
- Move semantics use r-value references: `operator=(T&& other)`.

## Overload resolution

Candidates are scored by:

- exact type match (+10),
- r-value reference binding (+3),
- non-const reference + l-value (+3) / r-value (+1),
- const reference (+2),
- default/looser conversions (+1).

Binding an r-value to a non-const reference emits a warning (a stack temporary is created).

## Comparison and `null`

Records can overload `operator==`/`operator!=` to compare against raw pointers and `null`:

```utp
public bool operator==(T* other) { return this._ptr == other; }

if (u == null) { /* empty */ }
if (sp != null) { /* alive */ }
```
