# Time

The stdlib `time.utp` module (imported with `import "utopia:time";`)
provides `Stopwatch` (elapsed-time measurement) and `DateTime` (calendar
dates and times).

## Stopwatch

```utp
Stopwatch sw;
sw.start();
... work ...
sw.stop();
Duration elapsed = sw.elapsed();
```

## DateTime

`DateTime` represents a point in time with microsecond precision. Values
are either local, UTC, or zoned (carrying a UTC offset).

### Construction

```utp
DateTime local = DateTime(2024, 1, 15, 10, 30);   // local zone
DateTime utc    = DateTime.utc(2024, 1, 15, 10, 30);
DateTime now    = DateTime.now();
DateTime fromUs = DateTime.fromMicrosecondsSinceEpoch(us);
```

### Parsing

`DateTime.parse` accepts ISO 8601 strings and throws `FormatException` on
malformed input; `tryParse(String, DateTime&)` returns a bool instead:

```utp
DateTime a = DateTime.parse("2024-01-15");
DateTime b = DateTime.parse("2024-01-15T10:30:00");
DateTime c = DateTime.parse("2024-01-15T10:30:00.123456");
DateTime d = DateTime.parse("2024-01-15T10:30:00Z");        // UTC
DateTime e = DateTime.parse("2024-01-15T10:30:00+02:00");   // UTC + offset
DateTime f = DateTime.parse("2024-01-15 10:30:00");         // space separator
```

A trailing `Z` or `+HH:MM`/`-HH:MM` produces a UTC-based value that keeps
the offset, so `toIso8601String()` round-trips the input.

### Time zones (IANA)

`inZone(zone)` converts the value to an IANA zone (for example
`"Europe/Madrid"`, `"America/New_York"`), keeping the same instant. The
system's zoneinfo database provides the real DST rules:

```utp
DateTime utc = DateTime.parse("2024-07-01T12:00:00Z");
DateTime madrid = utc.inZone("Europe/Madrid");
print("%s\n", madrid.toIso8601String().c_str());
// 2024-07-01T14:00:00.000000+02:00  (CEST)
```

`utcOffset()` reports the value's offset in seconds east of UTC (0 for
UTC, the recorded offset for parsed/zoned values, or the local zone's
offset for plain local values).

### Formatting

`toString()` prints `"2024-01-15 10:30:00.000000"` (with `Z` or `+HH:MM`
appended when the offset is known); `toIso8601String()` uses the `T`
separator.

### Notes

- `inZone` switches the process-wide `TZ` environment variable for the
  duration of one `localtime_r` call and restores it afterwards; it is
  not thread-safe. On Windows, `inZone` returns the value unchanged.
- `DateTime` values before the epoch (negative microseconds since epoch)
  are supported.
