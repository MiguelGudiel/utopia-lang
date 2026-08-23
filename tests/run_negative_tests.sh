#!/usr/bin/env bash
# Runs the negative language tests: each case must fail compilation with the
# expected diagnostic. Requires the utopia CLI on PATH or passed as $1.
#
# Usage: tests/run_negative_tests.sh [path/to/utopia]
set -u

UT="${1:-utopia}"
ROOT="$(cd "$(dirname "$0")" && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

mkdir -p "$WORK/proj/src"
cat > "$WORK/proj/build.yaml" <<'EOF'
project:
  name: "neg"
build:
  target: executable
  source_dirs:
    - src/
EOF

PASS=0
FAIL=0

run_case() {
  local name="$1" expect="$2" src="$3"
  printf '%s\n' "$src" > "$WORK/proj/src/main.utp"
  local out
  out=$("$UT" run "$WORK/proj" 2>&1)
  if printf '%s' "$out" | grep -q "$expect"; then
    PASS=$((PASS + 1))
  else
    FAIL=$((FAIL + 1))
    printf '[FAIL] %s, expected: %s\n' "$name" "$expect"
    printf '       got: %s\n' "$(printf '%s' "$out" | grep -m1 error)"
  fi
}

# 'final' variables: single assignment
run_case "assign to final local" \
  "Cannot assign to the final variable 'x'" \
  'int main() { final int32 x = 1; x = 2; return 0; }'
run_case "final local without initializer" \
  "must be initialized at its declaration" \
  'int main() { final int32 x; return 0; }'
run_case "final global without initializer" \
  "must be initialized at its declaration" \
  'final int32 g; int main() { return 0; }'
run_case "final compound assignment" \
  "Cannot assign to the final variable 'x'" \
  'int main() { final int32 x = 5; x += 1; return 0; }'
run_case "final increment" \
  "Cannot modify the final variable 'x'" \
  'int main() { final int32 x = 5; x++; return 0; }'
run_case "final decrement" \
  "Cannot modify the final variable 'x'" \
  'int main() { final int32 x = 5; --x; return 0; }'
run_case "final field increment via member" \
  "Cannot modify the final variable 'x'" \
  'class A { public final int32 x; A(this.x) {} } int main() { A* a = new A(1); a.x++; return 0; }'
run_case "final combined with var" \
  "The 'var' and 'final' modifiers cannot be combined" \
  'int main() { final var x = 5; return 0; }'
run_case "final combined with const" \
  "The 'const' and 'final' modifiers cannot be combined" \
  'int main() { final const x = 5; return 0; }'
run_case "static final without initializer" \
  "must be initialized at its declaration" \
  'class A { public static final int32 x; } int main() { return 0; }'

# 'final' fields: every constructor must initialize them
run_case "final field not initialized by a constructor" \
  "The final field 'x' must be initialized in every constructor" \
  'class A { public final int32 x; A() {} } int main() { return 0; }'
run_case "final field double init (declaration + this.x)" \
  "already initialized at its declaration" \
  'class A { public final int32 x = 1; A(this.x) {} } int main() { return 0; }'
run_case "final field double init (this.x + initializer list)" \
  "initialized twice in the initializer list" \
  'class A { public final int32 x; A(this.x) : this.x = 5 {} } int main() { return 0; }'
run_case "final field double init (declaration + initializer list)" \
  "already initialized at its declaration" \
  'class A { public final int32 x = 1; A() : this.x = 5 {} } int main() { return 0; }'
run_case "final field assigned in method body" \
  "Cannot assign to the final variable 'x'" \
  'class A { public final int32 x; A(this.x) {} public void set() { this.x = 3; } } int main() { return 0; }'
run_case "final field assigned through another object" \
  "Cannot assign to the final variable 'x'" \
  'class A { public final int32 x; A(this.x) {} } int main() { A* a = new A(1); a.x = 5; return 0; }'
run_case "inherited final field not initialized by subclass constructor" \
  "final field 'y' must be initialized" \
  'class A { public final int32 x; A(this.x) {} } class B extends A { public final int32 y; B(int32 v) : super(v) {} } int main() { return 0; }'

# 'final' classes: cannot be extended or implemented
run_case "extend final class" \
  "cannot extend the final class 'A'" \
  'final class A {} class B extends A {} int main() { return 0; }'
run_case "implement final class" \
  "cannot implement the final class 'A'" \
  'final class A { public void f() {} } class B implements A { public void f() {} } int main() { return 0; }'

# 'final' placement errors
run_case "final on struct" \
  "The 'final' modifier is strictly permitted on class declarations and variables only" \
  'final struct S { int32 x; } int main() { return 0; }'
run_case "final on method" \
  "The 'final' modifier cannot be applied to a method" \
  'class A { public final void m() {} } int main() { return 0; }'
run_case "final on function" \
  "The 'final' modifier is strictly permitted on class declarations and variables only" \
  'final void f() {} int main() { return 0; }'

# 'this.x' initializing formals
run_case "this.x in a method" \
  "can only be used in constructors" \
  'class A { public int32 x; public void m(this.x) {} } int main() { return 0; }'
run_case "this.x for unknown field" \
  "No field named 'nope'" \
  'class A { public int32 x; A(this.nope) {} } int main() { return 0; }'
run_case "this.x for static field" \
  "cannot initialize a static field" \
  'class A { public static int32 x; A(this.x) {} } int main() { return 0; }'

# constructor initializer list
run_case "super call not last in initializer list" \
  "must be the last entry in the initializer list" \
  'class A { public int32 x; A() : super(), this.x = 5 {} } int main() { return 0; }'

# for-in: protocol and header validation
run_case "for-in over a non-iterable type" \
  "Cannot iterate a value of type 'int32'" \
  'int main() { int32 x = 5; for (var v in x) { print("%lld", v); } return 0; }'
run_case "for-in over a String" \
  "No member named 'iterator'" \
  'int main() { String s = "hi"; for (var v in s) { print("%s", v.c_str()); } return 0; }'
run_case "for-in missing 'in'" \
  "Expected ';' after variable declaration" \
  'int main() { List<int32> l; for (var x of l) {} return 0; }'
run_case "for-in over an untyped literal" \
  "Cannot iterate an untyped (empty) array literal" \
  'int main() { for (var x in []) { print("%lld", x); } return 0; }'

# templates: constraints ('T extends X')
run_case "constraint violation on a record" \
  "does not satisfy the constraint 'T extends Number'" \
  'class Box<T extends Number> { T v; Box(T x) { this.v = x; } } int main() { Box<String> b = Box<String>("x"); return 0; }'
run_case "class constraint with a non-class argument" \
  "does not satisfy the constraint 'T extends Animal'" \
  'class Animal {} class Box<T extends Animal> { T v; Box(T x) { this.v = x; } } int main() { Box<int32> b = Box<int32>(1); return 0; }'
run_case "constraint violation on a function" \
  "does not satisfy the constraint 'T extends Number'" \
  'T maxOf<T extends Number>(T a, T b) { return a > b ? a : b; } int main() { print("%s", maxOf("a", "b").c_str()); return 0; }'
run_case "member access on unconstrained template parameter" \
  "Member access on non-record type" \
  'class Box<T> { T v; Box(T x) { this.v = x; } void m() { this.v.speak(); } } int main() { Box<int32> b = Box<int32>(1); b.m(); return 0; }'

# templates: specializations
run_case "specialization before its primary" \
  "its primary template has not been declared" \
  'class Foo<int32> { int32 v; } int main() { return 0; }'
run_case "specialization with wrong arity" \
  "but the primary template has" \
  'class Foo<T> { T v; } class Foo<int32, int64> { int32 v; } int main() { return 0; }'
run_case "ambiguous partial specialization" \
  "Ambiguous template specialization" \
  'class X<A, B> { A f; B s; } class X<int32, B> { A f; B s; } class X<A, int64> { A f; B s; } int main() { X<int32, int64> x = X<int32, int64>(1, 2); return 0; }'

# const expressions: rules for const constructors and const contexts
run_case "const constructor with a body" \
  "A const constructor must have an empty body" \
  'class A { final int32 x; const A(this.x) { print("hi"); } } int main() { return 0; }'
run_case "const constructor with a non-final field" \
  "all instance fields must be 'final'" \
  'class A { int32 x; const A(this.x) {} } int main() { return 0; }'
run_case "const variable with a non-constant initializer" \
  "must be a constant expression" \
  'int32 compute() { return 42; } int main() { const int32 x = compute(); return 0; }'
run_case "final variable is not a constant expression" \
  "must be a constant expression" \
  'class A { final int32 x; const A(this.x) {} } int main() { final A a = const A(1); const int32 y = a.x; return 0; }'
run_case "const expression over a non-const variable" \
  "Not a constant expression" \
  'int main() { int32 x = 5; const int32 y = const (x + 1); return 0; }'

# closures: capturing lambdas only flow through the closure-aware async APIs
run_case "capturing lambda passed to a plain function-pointer parameter" \
  "cannot be passed to a plain function-pointer parameter" \
  'int apply(int Function(int) fn, int v) => fn(v); int main() { int32 f = 3; return apply((x) => x * f, 5); }'
run_case "closure variable passed to a plain function-pointer parameter" \
  "cannot be passed to a plain function-pointer parameter" \
  'int apply(int Function(int) fn, int v) => fn(v); int main() { int32 f = 3; int Function(int) c = (x) => x * f; return apply(c, 5); }'
run_case "plain function assigned to a closure variable" \
  "Cannot assign a plain function to a variable that holds a capturing lambda" \
  'int add1(int32 x) { return x + 1; } int main() { int32 f = 3; int Function(int32) c = (x) => x * f; c = add1; return 0; }'

printf 'PASS=%d FAIL=%d\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ]
