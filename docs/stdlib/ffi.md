# FFI: Dynamic Libraries

The `utopia:ffi` module provides runtime loading of shared libraries and symbol resolution — the Utopia equivalent of `dlopen`/`dlsym`. The `DynamicLibrary` type lives in the `FFI` namespace:

```utp
import "utopia:ffi";
using FFI;
```

## DynamicLibrary

```utp
class DynamicLibrary {
  public static DynamicLibrary* open(const uint8* path);
  public T resolve<T>(const uint8* symName);
  public void unload();
}
```

- `open` with an absolute path loads the library directly; a relative path is resolved against the executable's directory.
- `resolve<T>` looks up a symbol and reinterprets it as a function (or data) pointer of type `T`.
- `unload` releases the library handle.

## Example

```utp
import "utopia:ffi";
using FFI;

int main() {
  DynamicLibrary* lib = DynamicLibrary.open("libmylib.so");
  if (lib == null) {
    print("failed to load library\n");
    return 1;
  }

  int32 Function(int32) addOne = lib.resolve<int32 Function(int32)>("add_one");
  print("add_one(41) = %d\n", addOne(41));

  lib.unload();
  return 0;
}
```

## Low-level bindings

The module also exposes the raw OS functions for direct use:

- `os_dlopen(path)` / `os_dlsym(handle, symbol)` / `os_dlclose(handle)` on POSIX
- `LoadLibraryA` / `GetProcAddress` / `FreeLibrary` on Windows (selected with `#if _WIN32`)

## Use cases

- Plugin systems and hot-reloadable modules
- Loading system libraries (e.g. SDL, OpenGL) at run time
- Game engines that resolve platform backends dynamically

The SpikesEngine + UtpSDL stack uses this mechanism to load SDL.
