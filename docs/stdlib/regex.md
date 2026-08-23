# Regex

`Regex` is the prelude's regular expression engine (imported
automatically, like `String`, because `String.split`/`replace` accept it).
It is a backtracking engine with a PCRE-flavored syntax, operating on
UTF-8 codepoints (`.` and character classes never split a multi-byte
rune).

## Syntax

| Construct | Meaning |
| --- | --- |
| `abc` | literals (escape special chars: `\.` `\*` `\(` ...) |
| `.` | any codepoint except `\n` |
| `[abc]` `[a-z]` `[^...]` | character classes with ranges and `\d\w\s` |
| `\d` `\D` `\w` `\W` `\s` `\S` | ASCII digit / word / whitespace (and negations) |
| `\n` `\t` `\r` `\f` `\v` `\0` | common escapes |
| `^` `$` | start / end of the string |
| `*` `+` `?` `{n}` `{n,}` `{n,m}` | greedy quantifiers (`*?` etc. are lazy) |
| `( ... )` | capturing group (1..N; group 0 = whole match) |
| `a\|b` | alternation |

Matching is leftmost-first with full backtracking, like the common
engines.

## API

```utp
Regex re = Regex("(\\w+)@(\\w+)");

bool ok = re.hasMatch("user@host");
Match m = re.firstMatch("mail user@host now");
m.start;  m.end;                 // byte range of the match
m.group(0);  m.group(1);  m.group(2);   // "user@host", "user", "host"
List<Match> all = re.allMatches("a@b c@d");
List<String> parts = re.split("a,b,c");       // ["a", "b", "c"]
String s = re.replaceAll("user@host", "$2@$1"); // "host@user"
String s2 = re.replaceFirst("user@host x@y", "$1"); // "user x@y"
```

`Match.group(i)` returns the captured substring (empty for groups that
did not participate). A `firstMatch` with no match returns `start == -1`.

## String integration

`String.split`, `replaceAll` and `replaceFirst` accept a `Regex`
(alongside their literal overloads); replacements support `$&` (whole
match), `$1`..`$9` (capture groups) and `$$` (literal `$`):

```utp
String csv = "a,b,,c";
List<String> fields = csv.split(Regex(","));

String clean = "one  two   three".replaceAll(Regex("\\s+"), " ");
String date = "2024-01-15".replaceAll(Regex("(\\d{4})-(\\d{2})-(\\d{2})"),
                                     "$3/$2/$1");  // "15/01/2024"
```

## Notes

- `^`/`$` anchor to the string boundaries, not line boundaries.
- Quantifiers are greedy by default; append `?` for lazy matching.
- Character classes are ASCII-oriented (`\w` = `[0-9A-Za-z_]`).
