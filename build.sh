#!/data/data/com.termux/files/usr/bin/bash
set -euo pipefail
root="$(dirname "$0")"
x86_64-w64-mingw32-windres -I "$root" "$root/resources.rc" -O coff -o "$root/resources.o"
x86_64-w64-mingw32-clang -Os -DSQLITE_THREADSAFE=1 -DSQLITE_OMIT_LOAD_EXTENSION -c "$root/sqlite3.c" -o "$root/sqlite3-win.o"
x86_64-w64-mingw32-clang++ "$root/main.cpp" "$root/sqlite3-win.o" "$root/resources.o" -o "$root/TagFinder.exe" \
  -std=c++17 -Os -Wall -Wextra -municode -mwindows -static -s \
  -lcomctl32 -lshell32 -lole32 -luuid -luser32 -lgdi32
