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
