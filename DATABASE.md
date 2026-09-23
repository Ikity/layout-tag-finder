# SQLite database schema

[English](DATABASE.md) | [Русский](DATABASE.ru.md) | [Main README](README.md) | [Architecture](ARCHITECTURE.md)

## Location and lifecycle

`tags.sqlite3` is created beside `TagFinder.exe`, using the executable's absolute
location, not the shell's working directory. SQLite is embedded in the app.
Close the application before copying the EXE and database to another folder or
computer. No external database service is needed. Paths inside the index still
refer to the source locations used by the last scan.

Startup creates missing tables/indexes with `IF NOT EXISTS` and restores scan
preferences. It does not discard existing relationships or start a scan.

## Complete application schema

```sql
CREATE TABLE IF NOT EXISTS uses (
    tag  TEXT NOT NULL,
    path TEXT NOT NULL,
    PRIMARY KEY (tag, path)
);

CREATE INDEX IF NOT EXISTS tag_lookup
    ON uses(tag COLLATE NOCASE);

CREATE TABLE IF NOT EXISTS settings (
    key   TEXT PRIMARY KEY,
    value TEXT NOT NULL
);
```

### `uses`: where a tag occurs

| Field | Meaning | Example |
| --- | --- | --- |
| `tag` | Extracted name, preserving original capitalization and permitted symbols. | `Me.UPSStatus_BF` |
| `path` | Full source-file path, passed to SQLite as UTF-8. | `C:\Layouts\Screen_01.$view` |

This is a direct many-to-many relationship table: each tag may occur in several
files and each file may contain several tags. There are no separate `tags` or
`files` tables, integer IDs, or foreign keys. The `(tag, path)` primary key
prevents storing the same pair twice. Occurrence counts and byte offsets are not
stored. A file with no accepted names has no row, even if counted in the scan
summary. The displayed file name is derived from `path`, not stored separately.

For example:

| tag | path |
| --- | --- |
| `Me.UPSStatus_BF` | `C:\Layouts\Overview.bin` |
| `Me.UPSStatus_BF` | `C:\Layouts\Power.bin` |
| `$Pump-01.Status` | `C:\Layouts\Overview.bin` |

The first two rows explain where `Me.UPSStatus_BF` is used. The first and third
explain that `Overview.bin` contains more than one name.

**Case rules:** primary-key comparison uses SQLite's default `BINARY` collation,
so `Pump.Run` and `pump.run` are distinct stored names. Searches ignore case and
can return both. `tag_lookup` uses `NOCASE` to accelerate exact-name searching;
it does not change uniqueness. SQLite's built-in case folding is ASCII-oriented,
which matches the accepted tag alphabet. File-path spelling is not globally
normalized by the database; the same path with different case is a distinct key.

### `settings`: the last successful scan

| `key` | Stored `value` | Use |
| --- | --- | --- |
| `scan_folder` | Absolute folder path in UTF-8. | Prefills the folder input at next launch. |
| `scan_recursive` | Text `"1"` or `"0"`. | Restores the Include subfolders checkbox. |

Before the first scan these rows may be absent. The app defaults to an empty
folder and recursion enabled. Both preferences are written only when the scan
succeeds. The app always supplies non-null fixed keys; the schema's declaration
above is the actual SQLite schema, without extra constraints or validation.

No last-scan timestamp, search text, source modification times or per-file hashes
are stored. Consequently refresh scans the whole selected scope each time.

## Replacement transaction

`scan()` runs this sequence using one connection:

```sql
BEGIN IMMEDIATE;
DELETE FROM uses;
-- Repeated prepared statements, with actual values bound to ? parameters:
INSERT OR IGNORE INTO uses(tag, path) VALUES (?, ?);
INSERT OR REPLACE INTO settings(key, value) VALUES (?, ?);
COMMIT;
```

The preference write runs once per known key. Deletion, new relationships and
preferences are committed together. If traversal or SQL fails, statement and
connection cleanup rolls back the uncommitted transaction and preserves the
previous saved snapshot. Individual unreadable files are skipped rather than
causing rollback; their old relationships disappear if that scan commits.

Each successful scan replaces all `uses` rows, including rows from any previously
selected folder. Use a common parent folder to keep several folders in one index.
Deleted files and removed names disappear on the next successful refresh.

Startup selects `journal_mode=DELETE` and `synchronous=FULL`; connections use a
5-second busy timeout. Writes may create a temporary `tags.sqlite3-journal` file.
A completed transaction removes it, leaving a standalone database after normal
exit. After an abnormal termination, let SQLite recover by reopening the app
before copying files; do not manually remove a recovery journal.

## Search queries

Exact (case-insensitive):

```sql
SELECT tag, path FROM uses
WHERE tag = ? COLLATE NOCASE
ORDER BY tag, path
LIMIT 5001;
```

Literal substring (case-insensitive):

```sql
SELECT tag, path FROM uses
WHERE instr(lower(tag), lower(?)) > 0
ORDER BY tag, path
LIMIT 5001;
```

`?` receives a bound UTF-8 parameter, not SQL string concatenation. The UI rejects
an empty search. `instr` makes punctuation literal rather than LIKE wildcards;
substring search generally scans the table instead of doing an indexed equality
lookup. At most 5,000 results are displayed; the extra row detects truncation.

For manual inspection with a SQLite client (not required to run the app):

```sql
SELECT COUNT(*) AS relationships FROM uses;
SELECT COUNT(DISTINCT tag) AS distinct_names FROM uses;
SELECT COUNT(DISTINCT path) AS files_with_tags FROM uses;
SELECT tag FROM uses WHERE path = 'C:\Layouts\Overview.bin' ORDER BY tag;
SELECT key, value FROM settings ORDER BY key;
```

`files_with_tags` is not the total visited-file count because zero-tag files are
not represented. The app opens layouts read-only; only its own SQLite index changes.
