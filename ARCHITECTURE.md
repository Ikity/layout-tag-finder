# Project structure and implementation

[English](ARCHITECTURE.md) | [Русский](ARCHITECTURE.ru.md) | [Main README](README.md) | [Database](DATABASE.md)

## Overview

Layout Tag Finder is a C++17 Windows desktop application. It reads small layout
files, extracts possible tag names, and stores their full file paths in SQLite.
Search uses the saved database, so the source files need not be available until
you rescan or open a result in Explorer. No application server is involved.

The dedicated repository places the following files at its root. In the original
multi-project workspace they are under `tag-finder/`.

| File | Responsibility |
| --- | --- |
| `main.cpp` | Win32 controls/events, scan worker, SQLite access, search, clipboard and Explorer integration. |
| `extract.h` | Platform-independent byte/string scanner, shared by the app and tests. |
| `test.cpp` | Native assertion-based extraction and SQLite behavior checks. |
| `sqlite3.c`, `sqlite3.h` | Vendored SQLite 3.38.2 amalgamation and public API. |
| `build.sh` | Cross-compiles the application for Windows x64. |
| `resources.rc` | Maps icon resource ID `101` to `icon.ico`. |
| `icon.svg` | Editable paper-sheet-and-atoms artwork. |
| `icon.ico` | Generated icon with 16, 24, 32, 48, 64, 128 and 256 pixel images. |
| `TagFinder.exe` | Ready-to-run Windows GUI executable, also distributed in Releases. |
| `.gitignore` | Excludes temporary objects, native test binaries, crash dumps and user databases. |
| `README.md`, `README.ru.md` | English/Russian user instructions and build commands. |
| `ARCHITECTURE.md`, `ARCHITECTURE.ru.md` | This implementation guide in both languages. |
| `DATABASE.md`, `DATABASE.ru.md` | Tables, index, queries and persistence rules in both languages. |

Generated runtime file: `tags.sqlite3` beside the executable. Generated build
files: `resources.o`, `sqlite3-win.o`, `sqlite3-test.o`, `test-extract`.

## Libraries and tools

| Component | Used for | Distributed separately? |
| --- | --- | --- |
| C++17 standard library | Strings, sets/vectors, files, paths, threads, atomics, RAII and exceptions. | C++ runtime is statically linked. |
| SQLite 3.38.2 | Embedded transactional storage, prepared statements and searching. | Compiled directly from `sqlite3.c`; no SQLite DLL/service. |
| Win32 User32 | Windows, controls, messages, menus and Unicode clipboard. | Part of Windows. |
| Common Controls (`comctl32`) | Report-mode list view and common-control initialization. | Part of Windows. |
| Windows Shell (`shell32`) | Folder picker interfaces and selecting a file in Explorer. | Part of Windows. |
| COM (`ole32`, `uuid`) | `IFileDialog` creation, COM lifetime, interface/class identifiers. | Windows APIs; `uuid` supplies link-time identifiers. |
| GDI (`gdi32`) | Default GUI font handle. | Part of Windows. |
| LLVM MinGW + `windres` | Windows headers/import libraries, compiler/linker and resource compilation. | Build-time only. |
| Native Clang | Builds the portable test executable on Linux/Termux. | Test-time only. |
| ImageMagick | Regenerates `icon.ico` from `icon.svg`. | Artwork-generation only; normal build uses the existing ICO. |

SQLite is public domain; its original notices remain in the vendored source.
There is no third-party GUI framework. Windows system DLLs and its C runtime
services are still required: “standalone” means no additional app-specific DLLs,
Python installation, database server, or network connection.

## Startup and event flow

```text
wWinMain
  -> initialize COM, DPI awareness and common controls
  -> locate EXE directory -> open tags.sqlite3 -> create missing schema
  -> restore scan_folder and scan_recursive
  -> register window class + embedded icon -> CreateWindow -> proc(WM_CREATE)
  -> message loop -> shortcuts / proc(...)

proc(WM_COMMAND: Scan)
  -> validate folder -> enabled(false) -> start scan(root, recursive) worker
       -> DB + BEGIN IMMEDIATE -> delete previous uses rows (uncommitted)
       -> visit files -> extractTags(bytes) -> insert tag/path pairs
       -> save scan preferences -> COMMIT
       -> post Done -> proc(Done) -> enabled(true), show summary

Search button / Enter -> search -> SELECT from SQLite -> result list
Right click -> copyMenu -> copyResult -> resultField -> Windows clipboard
Ctrl+C -> copyResult for last clicked column
Double click -> proc(WM_NOTIFY) -> select full path in Explorer
```

## Functions and their relationships

### Text and storage helpers (`main.cpp`)

| Function/type | Meaning and callers |
| --- | --- |
| `text(HWND)` | Reads a control's complete UTF-16 text. Used for folder input and search text. |
| `utf8(wstring)` | Converts UTF-16 to UTF-8 for SQLite paths, tag queries and saved preferences. |
| `wide(string)` | Converts UTF-8 database values/errors back to Win32 text. |
| `DB::DB()` | Opens `databasePath` and sets a 5-second busy timeout. Used at startup, by `scan`, and by `search`. |
| `DB::~DB()` | Closes the owned connection when the scope ends, including exception unwinding. |
| `DB::exec(sql)` | Executes fixed SQL, throwing on error; used for schema and transactions. |
| `Statement::Statement(DB&, sql)` | Compiles a prepared SQL statement tied to that connection. |
| `Statement::~Statement()` | Finalizes the statement before the enclosing connection is destroyed. |
| `Statement::bind(index, value)` | Copies UTF-8 data into a numbered SQL parameter. Prevents input from becoming SQL syntax. |
| `Statement::insert()` | Runs a write to completion, then resets the statement and clears bindings for reuse. |

### Indexing and UI (`main.cpp`)

| Function | Meaning and relationships |
| --- | --- |
| `enabled(bool)` | Enables/disables folder, scan and search controls; called before starting a worker and when `Done` arrives. |
| `scan(root, recursive)` | Background entry point: enumerate files, extract tags, replace index and settings transactionally, report progress. |
| `scan`'s `visit` lambda | Shared per-file pipeline: reject links/non-files/oversized files/database sidecars, bounded read, `extractTags`, SQL inserts. |
| `search()` | Reads the query controls, runs exact or literal-substring SQL, fills three list columns and displays up to 5,000 rows. |
| `resultField(row, column)` | Grows a UTF-16 buffer until a whole list-view cell fits; used by copying. |
| `copyResult(command)` | Reads a chosen field or tab-separated row and transfers it to the Unicode clipboard. |
| `copyMenu(location)` | Resolves mouse row/column or keyboard selection, shows the copy menu, calls `copyResult`. |
| `control(class, title, style, id)` | Creates a child window with the default GUI font; used during `WM_CREATE`. |
| `proc(hwnd, message, wParam, lParam)` | Window procedure: dispatches creation, resizing, commands, list notifications, worker messages and shutdown. |
| `wWinMain(...)` | Windows entry point: initializes services/storage/UI, restores settings, intercepts shortcuts and runs event dispatch. |

### Extraction (`extract.h`)

`extractTags(bytes)` returns a `set<string>` of unique candidates from one file.
Its nested `inspect(units)` tokenizer is reused for both byte strings and detected
UTF-16 runs. `flush()` accepts a completed candidate only if it contains a Latin
letter and no non-ASCII unit. Allowed remaining characters are digits, `$`, `_`,
`-`, and `.`; other ASCII punctuation and controls delimit candidates.

The wide-string pass tries both byte orders and all possible starting positions.
A printable ASCII UTF-16 run must be at least three units long. `unit(position)`
decodes an adjacent unit to detect possible mixed-script continuations. `Run`
records the byte interval, units and rejection flag. Overlapping interpretations
are rejected if one looks mixed-script, and the `wide` mask prevents their low
bytes from being indexed again as isolated Latin letters. Finally, `inspect`
handles the unmasked byte stream.

This heuristic can miss valid strings near binary data and can admit random
printable strings or object names. It does not parse proprietary records or
decompress/decrypt content. See the README's scanning rules.

### Tests (`test.cpp`)

`main()` runs extraction fixtures, in-memory SQL checks and a real on-disk
copy/reopen/refresh scenario. `append` creates separated byte fixtures; `exec`
runs test SQL; `scalar` reads a single result; `Cleanup` removes the temporary
test directory. These SQL tests check storage behavior rather than exercising
the Windows `DB` wrapper or GUI. Keep assertions enabled (do not add `-DNDEBUG`).

## Threads, ownership and failure behavior

- UI globals hold window handles, immutable database location, restored options,
  selected column and the scan state. `busy` prevents a second scan and blocks
  normal window closing until the worker finishes.
- The scan worker uses its own SQLite connection and input copies. Search and UI
  updates happen on the main thread; search inputs are disabled during scanning.
- Every 25 accepted files the worker posts `Progress` with a count in `WPARAM`.
  After success or a caught exception it posts `Done` with an allocated message;
  the UI takes ownership with `unique_ptr` and frees it after display.
- `Statement` objects are destroyed before their `DB`. If scanning throws before
  commit, closing the connection rolls back the whole replacement. An unreadable
  file is counted/skipped; a directory traversal error aborts the scan.
- A clipboard allocation belongs to the app until `SetClipboardData` succeeds,
  then Windows owns it. COM dialog/item interfaces are released after use.
- Only source reads occur; no source layout is modified. Refresh is a full manual
  rescan, with no file watcher, content hashes, or incremental-update table.

## Build and validation

`build.sh` compiles resource `101`, compiles SQLite as C with thread support and
loadable extensions disabled, then links the C++17 GUI with `-municode`,
`-mwindows`, static C++/SQLite code and Windows import libraries. Paths are based
on the script's location. Run commands from [README.md](README.md).

Native tests cover extraction, deduplication, query semantics, persistence,
portable copying and transaction replacement/rollback. The Windows build and PE
resource/header checks verify compilation, x64 GUI format and embedded icon.
Actual GUI, clipboard and Explorer interactions require a Windows runtime test.
