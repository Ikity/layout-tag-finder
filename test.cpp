#include "extract.h"
#include "sqlite3.h"
#include <cassert>
#include <iostream>
#include <filesystem>
#include <chrono>

// Native command-line checks, independent of Win32. Keep assertions enabled:
// some assert expressions execute SQLite calls as well as verify their results.
int main() {
    // Synthetic binary strings cover photo-style names, duplicates and UTF-8.
    std::string input = "\x01Me.UPSStatus_BF\x00";
    std::vector<unsigned char> bytes(input.begin(), input.end());
    auto append = [&](const std::string& s) { bytes.push_back(0); bytes.insert(bytes.end(), s.begin(), s.end()); bytes.push_back(0); };
    append("Me.Battery_LifeTimeStatus_BF"); append("$Pump-01.Status"); append("Me.UPSStatus_BF");
    append("12345"); append("Pump\xc3\xa9.Status"); append("\xd0\x90Motor");
    auto tags = extractTags(bytes);
    assert(tags.size() == 3);
    assert(tags.count("Me.UPSStatus_BF")); assert(tags.count("Me.Battery_LifeTimeStatus_BF")); assert(tags.count("$Pump-01.Status"));
    // Test both UTF-16 byte orders and a mixed-script continuation of a name.
    for (bool be : {false, true}) {
        std::vector<unsigned char> data{0, 0};
        for (unsigned char c : std::string("$Motor_1.Run")) { data.push_back(be ? 0 : c); data.push_back(be ? c : 0); }
        data.push_back(0); data.push_back(0);
        auto extracted = extractTags(data);
        assert(extracted.size() == 1 && extracted.count("$Motor_1.Run"));
        data.resize(data.size() - 2); data.push_back(be ? 4 : 0x10); data.push_back(be ? 0x10 : 4); data.push_back(0); data.push_back(0);
        auto mixed = extractTags(data);
        if (!mixed.empty()) { for (const auto& value : mixed) std::cerr << "Mixed UTF-16 (BE=" << be << "): " << value << '\n'; return 1; }
    }
    assert(extractTags({}).empty());
    assert(extractTags(std::vector<unsigned char>(1048576, 0)).empty());
    // In-memory tests check the schema/query contract, not the Windows UI.
    sqlite3* db = nullptr; assert(sqlite3_open(":memory:", &db) == SQLITE_OK);
    auto exec = [&](const char* sql) { assert(sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK); };
    exec("CREATE TABLE uses(tag TEXT NOT NULL,path TEXT NOT NULL,PRIMARY KEY(tag,path));"
         "INSERT OR IGNORE INTO uses VALUES('$Pump-01.Status','C:/one/layout.$_');"
         "INSERT OR IGNORE INTO uses VALUES('$Pump-01.Status','C:/one/layout.$_');"
         "INSERT OR IGNORE INTO uses VALUES('$Pump-01.Status','C:/two/layout.$_');");
    sqlite3_stmt* s = nullptr;
    assert(sqlite3_prepare_v2(db, "SELECT count(*) FROM uses WHERE tag=? COLLATE NOCASE", -1, &s, nullptr) == SQLITE_OK);
    sqlite3_bind_text(s, 1, "$pump-01.status", -1, SQLITE_STATIC);
    assert(sqlite3_step(s) == SQLITE_ROW && sqlite3_column_int(s, 0) == 2); sqlite3_finalize(s);
    exec("BEGIN; DELETE FROM uses; ROLLBACK;");
    assert(sqlite3_prepare_v2(db, "SELECT count(*) FROM uses WHERE instr(lower(tag),lower(?))>0", -1, &s, nullptr) == SQLITE_OK);
    sqlite3_bind_text(s, 1, "-01.", -1, SQLITE_STATIC);
    assert(sqlite3_step(s) == SQLITE_ROW && sqlite3_column_int(s, 0) == 2); sqlite3_finalize(s);
    sqlite3_close(db);
    // Close/reopen and copy a real database, as a user would when carrying
    // the saved index to another computer. No original source files are needed.
    namespace fs = std::filesystem;
    auto directory = fs::temp_directory_path() / ("tag-finder-test-" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directory(directory);
    struct Cleanup { fs::path path; ~Cleanup() { std::error_code ec; fs::remove_all(path, ec); } } cleanup{directory};
    auto original = (directory / "tags.sqlite3").string();
    auto portable = (directory / "copied.sqlite3").string();
    assert(sqlite3_open(original.c_str(), &db) == SQLITE_OK);
    exec("PRAGMA journal_mode=DELETE; PRAGMA synchronous=FULL;"
         "CREATE TABLE uses(tag TEXT NOT NULL,path TEXT NOT NULL,PRIMARY KEY(tag,path));"
         "CREATE TABLE settings(key TEXT PRIMARY KEY,value TEXT NOT NULL);"
         "BEGIN IMMEDIATE;"
         "INSERT INTO uses VALUES('Me.UPSStatus_BF','C:/layout.$_');"
         "INSERT INTO settings VALUES('scan_folder','C:/Layouts');"
         "INSERT INTO settings VALUES('scan_recursive','0'); COMMIT;");
    assert(sqlite3_close(db) == SQLITE_OK);
    fs::copy_file(original, portable);
    fs::remove(original);
    assert(sqlite3_open(portable.c_str(), &db) == SQLITE_OK);
    auto scalar = [&](const char* sql) {
        sqlite3_stmt* statement = nullptr;
        assert(sqlite3_prepare_v2(db, sql, -1, &statement, nullptr) == SQLITE_OK);
        assert(sqlite3_step(statement) == SQLITE_ROW);
        std::string result = reinterpret_cast<const char*>(sqlite3_column_text(statement, 0));
        sqlite3_finalize(statement); return result;
    };
    assert(scalar("SELECT tag FROM uses") == "Me.UPSStatus_BF");
    assert(scalar("SELECT value FROM settings WHERE key='scan_folder'") == "C:/Layouts");
    assert(scalar("SELECT value FROM settings WHERE key='scan_recursive'") == "0");
    // A failed refresh keeps both the prior relations and prior scan preferences.
    exec("BEGIN IMMEDIATE; DELETE FROM uses; UPDATE settings SET value='broken'; ROLLBACK;");
    assert(scalar("SELECT tag FROM uses") == "Me.UPSStatus_BF");
    assert(scalar("SELECT value FROM settings WHERE key='scan_folder'") == "C:/Layouts");
    exec("BEGIN IMMEDIATE; DELETE FROM uses;"
         "INSERT INTO uses VALUES('$New.Tag','D:/new-file');"
         "INSERT OR REPLACE INTO settings VALUES('scan_folder','D:/New'); COMMIT;");
    assert(sqlite3_close(db) == SQLITE_OK);
    assert(sqlite3_open(portable.c_str(), &db) == SQLITE_OK);
    assert(scalar("SELECT count(*) FROM uses") == "1");
    assert(scalar("SELECT tag FROM uses") == "$New.Tag");
    assert(scalar("SELECT value FROM settings WHERE key='scan_folder'") == "D:/New");
    assert(sqlite3_close(db) == SQLITE_OK);
    assert(!fs::exists(portable + "-journal") && !fs::exists(portable + "-wal"));
    std::cout << "Extraction, SQLite relationships, portable persistence, and refresh tests passed.\n";
}
