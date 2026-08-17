# Formatter

Utopia includes a built-in code formatter with a Wadler-style pretty-printing engine.

## Usage

```sh
utopia fmt src/main.utp          # format one file in place
utopia fmt path/to/project       # format all project sources
```

## Style

- 80-column page width.
- Consistent indentation, spacing, and line breaking for declarations, expressions, and call arguments.
- Preserves **comments**, including trailing comments and doc comments.
- Line-breaks are solved optimally across the whole line (dynamic programming over the piece tree), so formatting is deterministic and stable.

## Example

```utp
// Before
class Point{public int x;public int y;
public Point(int x,int y){this.x=x;this.y=y;}}

// After
class Point {
  public int x;
  public int y;
  public Point(int x, int y) {
    this.x = x;
    this.y = y;
  }
}
```

## Integration

- The LSP exposes `textDocument/formatting` using the same engine, so editors format identically to the CLI.
- Formatter output is idempotent: formatting an already formatted file produces no changes.
