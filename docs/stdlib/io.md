# I/O: Path, File, Directory, FileHandle

The `utopia:io` module provides a filesystem and file I/O API with RAII resource management. All types live in the `IO` namespace:

```utp
import "utopia:io";
using IO;
```

## Path

Static path utilities:

| Method | Description |
| --- | --- |
| `getExecutablePath()` | Absolute path of the running executable |
| `getCurrentDirectory()` | Current working directory |
| `getDirectoryName(String)` | Parent directory of a path |
| `getFileName(String)` | File name component of a path |
| `combine(String, String)` | Join two path components |

## FileHandle

A low-level, RAII file handle. The underlying file is closed automatically by the destructor.

```utp
FileHandle fh;
if (fh.open("data.bin", "rb")) {
  uint8 buffer[256];
  usize n = fh.read(buffer, 1, 256);
  // ...
  fh.seek(0, FileHandle.SEEK_SET);
  int64 pos = fh.tell();
  // fh is closed automatically at scope exit
}
```

| Member | Description |
| --- | --- |
| `open(path, mode) → bool` | Open a file (`"r"`, `"w"`, `"rb"`, ...) |
| `close()` | Close the handle |
| `isOpen() → bool` | Whether the handle is open |
| `read(uint8*, usize, usize) → usize` | Read up to N items |
| `write(const uint8*, usize, usize) → usize` | Write N items |
| `seek(int64, int32) → bool` | Seek with `SEEK_SET` / `SEEK_CUR` / `SEEK_END` |
| `tell() → int64` | Current position |

## File

High-level file operations:

```utp
File f("notes.txt");
print("exists: %d\n", f.exists());
print("size: %d\n", f.length());

String content = f.readAsString();
f.writeAsString("new content");
f.appendAsString(" appended");
f.rename("notes2.txt");
f.copy("backup.txt");
f.move("archive/notes2.txt");
f.remove();
```

| Method | Description |
| --- | --- |
| `exists()` | File existence |
| `remove()` | Delete the file |
| `rename(String)` / `move(String)` | Rename / move |
| `copy(String)` | Copy (buffered) |
| `length() → usize` | Size in bytes |
| `readAsString() → String` | Read whole file |
| `writeAsString(String) → bool` | Truncate and write |
| `appendAsString(String) → bool` | Append |
| `create()` | Create empty file |

## Directory

```utp
Directory d("data");
if (!d.exists()) {
  d.create();                 // mode 0777
}
d.rename("data2");
d.deleteRecursive();          // rm -rf / rmdir /s /q
```

| Method | Description |
| --- | --- |
| `exists()` | Directory existence |
| `create()` | Create with mode 0777 |
| `remove()` | Remove (must be empty) |
| `rename(String)` / `move(String)` | Rename / move |
| `deleteRecursive()` | Recursive delete (shell-assisted) |
| `copy(String)` | Recursive copy (shell-assisted) |

## Example: reading a config file

```utp
import "utopia:io";
using IO;

int main() {
  File cfg("config.ini");
  if (cfg.exists()) {
    String contents = cfg.readAsString();
    print("%s\n", contents.c_str());
  } else {
    cfg.writeAsString("host=localhost\nport=8080\n");
  }
  return 0;
}
```
