# Repository notes

Total Commander WCX plugin that exposes a Firefox `places.sqlite` profile as an
archive of `.url` files.

## Build

CI (`.github/workflows/build.yml`) builds both targets with MinGW-w64 from MSYS2:

```
gcc -c sqlite3.c -o sqlite3-x64.o -O2 -I. -DSQLITE_THREADSAFE=0 \
    -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_OMIT_DEPRECATED -DSQLITE_DEFAULT_MEMSTATUS=0
g++ -std=c++17 -O2 -Wall -Wextra -DUNICODE -D_UNICODE -shared \
    -o places_wcx.wcx64 places_wcx.cpp sqlite3-x64.o -I. places_wcx.def \
    -static -static-libgcc -static-libstdc++ \
    -Wl,-Bstatic -lwinpthread -Wl,-Bdynamic -Wl,-Bstatic -Wl,--kill-at
```

The 32-bit build uses `i686-w64-mingw32-g++`, `sqlite3-x86.o` and writes
`places_wcx.wcx`. `places_wcx.def` lists the 19 exports; CI diffs the real export
table against it, so a new entry point must be added there too.

`sqlite3.c`/`sqlite3.h` are downloaded at build time and are not in the repo.

## Total Commander traps

- `SetProcessDataProcW`/`SetChangeVolProcW` are called with the placeholder handle
  `-1` before packing and deleting, when no archive is open. Never dereference a
  handle without checking it against the registry in this file.
- The number in `[PackerPlugins]` of `wincmd.ini` is the mask cached from
  `GetPackerCaps` (1 new, 2 modify, 4 multiple, 8 delete, 16 options, 32 mempack,
  64 by content, 128 search, 256 hide, 512 encrypt). Total Commander only calls
  `GetPackerCaps` when the plugin is installed, so a stale mask keeps the old
  behaviour after a rebuild: remove and re-add the plugin.
- Packing into an archive requires the new bit; modifying an existing one also
  needs the modify bit. Without them TC answers "Function not supported!".
- Deleting a folder sends the mask `Folder\*.*`, which means the folder and
  everything below it.
- `tHeaderDataExW::FileTime` is a DOS timestamp with the date in the high word
  (`date << 16 | time`).

## Testing

Total Commander runs under Wine for manual checks: install the plugin, point it
at a copy of `places.sqlite`, then exercise list, extract, F5/F7 (pack, new
folder) and F8 (delete). The plugin logs to `%TEMP%\places_wcx.log`.
