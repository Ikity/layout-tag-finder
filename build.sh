#!/data/data/com.termux/files/usr/bin/bash
set -euo pipefail
# Resolve inputs relative to this script, so it can be invoked from any folder.
root="$(dirname "$0")"
# Package the multi-resolution icon into a COFF object linked into the EXE.
x86_64-w64-mingw32-windres -I "$root" "$root/resources.rc" -O coff -o "$root/resources.o"
# Compile the vendored SQLite C code separately; dynamic extensions are disabled.
x86_64-w64-mingw32-clang -Os -DSQLITE_THREADSAFE=1 -DSQLITE_OMIT_LOAD_EXTENSION -c "$root/sqlite3.c" -o "$root/sqlite3-win.o"
# Build a Unicode Windows GUI program and statically link the C++ runtime/SQLite.
# Windows' own GUI, COM and Shell DLLs are still used at runtime.
x86_64-w64-mingw32-clang++ "$root/main.cpp" "$root/sqlite3-win.o" "$root/resources.o" -o "$root/TagFinder.exe" \
  -std=c++17 -Os -Wall -Wextra -municode -mwindows -static -s \
  -lcomctl32 -lshell32 -lole32 -luuid -luser32 -lgdi32
