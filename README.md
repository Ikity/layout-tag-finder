# Layout Tag Finder

[English](README.md) | [Русский](README.ru.md)

**Technical documentation:** [Architecture and functions](ARCHITECTURE.md) ·
[SQLite schema](DATABASE.md) · [Архитектура и функции](ARCHITECTURE.ru.md) ·
[Схема SQLite](DATABASE.ru.md)

A small native Windows x64 GUI for finding which layout files contain a tag name.

[Download the Windows executable](https://github.com/Ikity/layout-tag-finder/releases/latest)

## Run

Copy **TagFinder.exe** to a Windows 10/11 64-bit computer and double-click it.
SQLite and the C++ runtime are linked into the executable; Python and a separate
database installation are not needed.

1. Click **Choose folder...** and select the folder containing your layout files.
2. Leave **Include subfolders** checked if needed.
3. Click **Scan / replace index** and wait for the completion message.
4. Enter a name such as `Me.UPSStatus_BF` or `$Pump-01.Status`, then click
   **Search** or press Enter in the search field.
5. Check **Exact name** for a whole-name match. Otherwise, search finds literal
   substrings. Both modes are case-insensitive; `_`, `$`, `-`, and `.` are literal
   characters, not search wildcards.
6. Results show the tag, file name, and full path. Double-click a row to select
   its file in Windows Explorer.

### Copy result fields

Right-click a result and choose **Copy tag name**, **Copy file name**, or
**Copy full path**. **Copy clicked field** copies just the column you clicked;
you can also click a cell and press **Ctrl+C**. **Copy entire row** copies all
three fields separated by tabs, ready to paste into a spreadsheet. Unicode file
names and special characters are preserved.

For keyboard use, select a row with the arrow keys and press **Shift+F10** or
the Menu key to choose which field to copy. Ctrl+C uses the last clicked column
(the tag-name column by default).

The executable includes a paper-sheet-and-atoms icon for Explorer, the taskbar,
and the application window; no external icon file is needed to run it.

**Scans are saved automatically** to `tags.sqlite3` beside `TagFinder.exe`.
On later launches you can search immediately without scanning again. The last
successfully scanned folder and subfolder option are remembered in the database.
Click **Scan / replace index** whenever files are added, changed, or deleted;
the app performs a fresh scan only when you click that button.

Each successful scan
**replaces the entire previous index** with the selected folder's current contents.
Choose a common parent folder to index several folders together. The replacement
is transactional: if traversal or a database operation fails, the previous index
is retained. Individual unreadable files are skipped and counted in the summary.
Directory access failures stop the scan, rather than silently saving an incomplete
folder tree.

## Scanning rules

- Any file extension, including no extension, is accepted.
- Files larger than **1,048,576 bytes (1 MiB)** are skipped. Files exactly that
  size are accepted. Reads are capped even if a file grows during scanning.
- Names use `A–Z`, `a–z`, `0–9`, `$`, `_`, `-`, and `.` and must include at least
  one letter. Original capitalization is preserved.
- The scanner extracts ASCII-compatible strings from text or binary files.
  It also recognizes heuristic UTF-16LE/BE ASCII runs of at least three characters.
- Non-ASCII characters invalidate a candidate instead of producing partial
  Latin fragments within that candidate. UTF-16 detection is conservative near
  adjacent non-ASCII data. Unknown encodings and arbitrary binary bytes cannot be
  identified perfectly without a format specification.
- Other ASCII punctuation, whitespace, and binary control bytes separate names.
  The `NUL`, `SOH`, and similar boxes in a binary editor are not actual names in
  the original binary file. Scan the original layout files, not screenshots or
  text dumps containing those labels.
- One tag/file pair is stored once, even when the tag appears repeatedly in a
  file. The same tag can point to any number of different full file paths.
- File paths are stored as Unicode, including file names containing `$`, `_`,
  `-`, and `.`. The Latin-only restriction applies to tags, not file paths.
- Symbolic links are skipped. The index database and its sidecar files are excluded.
- A search displays up to 5,000 matches and indicates when to refine the query.

This is a **readable-string index**, not a decoder of the proprietary layout
format. The photo shows both tag-like strings (`Me.UPSStatus_BF`) and layout
object names (`EmbeddedSymbol1`, `AnalogItem1`). Both kinds may be indexed;
random printable binary fragments may also occur. Compressed/encrypted names
cannot be extracted. An original sample file and confirmed tag names would allow
format-specific filtering and more precise extraction.

## Database

The portable app consists of:

```text
TagFinder.exe
tags.sqlite3
```

The database is created automatically beside the executable on first launch,
regardless of the working directory or shortcut used to open the app. Keep the
app in a writable folder such as Documents or a USB drive. **Close the app, then
copy both files together** to move the app and its saved index to another computer.
No SQLite installation, DLLs, service, or network connection is required.
SQLite may create a temporary `tags.sqlite3-journal` during writes; after a
successful scan and normal exit only the database is needed.

**Upgrading from the first version:** close the old app and copy its existing
`%LOCALAPPDATA%\LayoutTagFinder\tags.sqlite3` next to the new `TagFinder.exe`
before launching. Your saved tags remain searchable without rescanning. The old
version did not save the scan folder, so select it once when you next refresh.

Saved file paths refer to the original source locations. You can search the
database even when those locations are unavailable. To locate files in Explorer
or refresh an index after moving the source files, select their new folder and
scan again.

Schema:

```sql
CREATE TABLE uses (
    tag TEXT NOT NULL,
    path TEXT NOT NULL,
    PRIMARY KEY (tag, path)
);
CREATE INDEX tag_lookup ON uses(tag COLLATE NOCASE);
CREATE TABLE settings (
    key TEXT PRIMARY KEY,
    value TEXT NOT NULL
);
```

`uses` stores one row per tag/full-path pair. A tag can belong to many files,
and a file can contain many tags. The composite primary key removes repeated
occurrences within one file. `tag_lookup` speeds up case-insensitive exact search.
`settings` stores `scan_folder` and `scan_recursive` (`"1"` or `"0"`) from the
last successful scan. Both tables are updated in one transaction when rescanning.

See [DATABASE.md](DATABASE.md) for examples, queries, case rules and rollback behavior.

Close the app before copying or deleting the database. Source layout files are
opened only for reading.

## Build and verification

`build.sh` builds using the installed LLVM MinGW cross-compiler in the development
environment:

```sh
bash build.sh
```

The build embeds `icon.ico` using LLVM MinGW's `windres`. To regenerate the
multi-resolution icon from its editable SVG source with ImageMagick:

```sh
magick -background none icon.svg -define icon:auto-resize=256,128,64,48,32,24,16 icon.ico
```

The SQLite 3.38.2 amalgamation is vendored as `sqlite3.c` and `sqlite3.h`, obtained
from <https://github.com/azadkuh/sqlite-amalgamation>. SQLite is public domain;
its notice is included in the source files.

Run the portable extraction and SQLite tests with a native Clang toolchain:

```sh
clang -O1 -DSQLITE_OMIT_LOAD_EXTENSION -c sqlite3.c -o sqlite3-test.o
clang++ -std=c++17 -Wall -Wextra test.cpp sqlite3-test.o -pthread -lm -o test-extract
./test-extract
```

Verified here: extraction of photo-style names, allowed symbols, non-Latin UTF-8
candidate rejection, UTF-16LE/BE extraction and mixed-script rejection, duplicate
relationships, case-insensitive exact search, literal substring search, transaction
rollback, disk persistence across closing/reopening, copying a standalone database,
restoring scan preferences, replacing outdated results on refresh, and successful
Windows compilation. PE headers confirm an AMD64 Windows
GUI executable. **The Windows GUI has not been runtime-tested in this Linux/Android
environment, and no original compiled layout file was available for validation.**
