# System

The stdlib's `system.utp` module (imported with `import "utopia:system";`)
binds basic operating-system functions: process control (`exit`, `abort`,
`Process`), environment access (`Env`) and the host name.

## Functions

| Function | Signature | Description |
| --- | --- | --- |
| `sleep` | `uint32 sleep(uint32 milliseconds)` | Suspend the calling thread |
| `system` | `int32 system(const uint8* command)` | Execute a shell command |
| `exit` | `void exit(int32 status)` | Terminate the process with the given status |
| `abort` | `void abort()` | Abort the process abnormally (SIGABRT) |

## `Env`

| Method | Description |
| --- | --- |
| `get` / `set` / `unset` | Environment variable access |
| `args()` | The program's command-line arguments as `List<String>` (the first element is the program name). Works regardless of whether `main` declares `(int32 argc, uint8** argv)`; empty under JIT execution |
| `hostname()` | The machine's network host name |

## `Process`

Spawns and controls child processes.

| Member | Description |
| --- | --- |
| `spawn(String executable, List<String> args)` | Launch a child process (POSIX `fork`+`execvp`, Windows `_spawnvp`) |
| `wait()` | Block until the child exits; returns its exit code (128 + signal for signal-killed children) |
| `kill(int32 signal)` | Send a signal (`Process.SIGINT`, `SIGTERM`, `SIGKILL`); on Windows the process is terminated |
| `pid()` / `isRunning()` | Process identifier and running state |

A child whose executable cannot be found exits with status 127 (the shell
convention); `spawn` only fails (pid `-1`) when the fork itself fails.

## Usage

```utp
import "utopia:system";

int main() {
  print("Waiting 500ms...\n");
  sleep(500);

  int32 result = system("echo hello from the shell");
  print("exit code: %d\n", result);

  print("hostname: %s\n", Env.hostname().c_str());

  Process child = Process.spawn("sh", ["-c", "echo hi; exit 3"]);
  print("child exit: %d\n", child.wait());

  return 0;
}
```

## Platform notes

- `sleep` maps to the platform's millisecond sleep (POSIX `usleep`-style behavior on Unix, `Sleep` on Windows).
- `system` returns the command's exit status.
- `Process.kill` uses `kill(2)` on POSIX and `TerminateProcess` on Windows.
- The `io` module (see [I/O](io.md)) provides higher-level filesystem utilities.
