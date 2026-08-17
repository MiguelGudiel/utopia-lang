# System

The prelude's `System/OS.utp` module binds basic operating-system functions.

## Functions

| Function | Signature | Description |
| --- | --- | --- |
| `sleep` | `uint32 sleep(uint32 milliseconds)` | Suspend the calling thread |
| `system` | `int32 system(const uint8* command)` | Execute a shell command |

## Usage

```utp
int main() {
  print("Waiting 500ms...\n");
  sleep(500);

  int32 result = system("echo hello from the shell");
  print("exit code: %d\n", result);
  return 0;
}
```

## Platform notes

- `sleep` maps to the platform's millisecond sleep (POSIX `usleep`-style behavior on Unix, `Sleep` on Windows).
- `system` returns the command's exit status.
- The `io` module (see [I/O](io.md)) provides higher-level filesystem utilities.
