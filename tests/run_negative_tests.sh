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
    printf '[FAIL] %s — expected: %s\n' "$name" "$expect"
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

printf 'PASS=%d FAIL=%d\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ]
