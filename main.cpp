// Layout Tag Finder: Win32 interface + background scanner + portable SQLite index.
// Start at wWinMain(), then follow proc() for UI events and scan() for indexing.
// See ARCHITECTURE.md / ARCHITECTURE.ru.md for the complete call and data flow.
#ifndef UNICODE
#define UNICODE
#endif
#define _UNICODE
#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <filesystem>
#include <fstream>
#include <thread>
#include <atomic>
#include <memory>
#include <stdexcept>
#include <cstring>
#include "sqlite3.h"
#include "extract.h"

namespace fs = std::filesystem;
// Controls belong to the main UI thread. The worker reports progress with posted
// messages; it never changes a control directly. databasePath is set at startup.
HWND window, folderBox, recursiveBox, queryBox, exactBox, results, statusBox;
HWND browseButton, scanButton, searchButton;
std::wstring databasePath;
std::wstring lastScanFolder;
bool lastScanRecursive = true;
std::atomic<bool> busy{false};
// Progress carries a file count in WPARAM. Done transfers ownership of a heap
// std::wstring in LPARAM; proc() takes it into a unique_ptr and releases it.
constexpr UINT Done = WM_APP + 1;
constexpr UINT Progress = WM_APP + 2;
int selectedColumn = 0;
constexpr UINT CopyField = 40, CopyTag = 41, CopyFileName = 42, CopyPath = 43, CopyRow = 44;
// Win32 controls use UTF-16 strings. Include space for the terminating NUL while
// reading a control, then remove that extra character from the returned string.
std::wstring text(HWND h) {
    int n = GetWindowTextLengthW(h);
    std::wstring s(n + 1, L'\0'); GetWindowTextW(h, s.data(), n + 1); s.resize(n); return s;
}
// SQLite's char-based API stores UTF-8; filesystem and GUI paths stay UTF-16.
std::string utf8(const std::wstring& s) {
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0, nullptr, nullptr);
    std::string r(n, 0); WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), r.data(), n, nullptr, nullptr); return r;
}
// Convert database results/errors back to text accepted by the Win32 W APIs.
std::wstring wide(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring r(n, 0); MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), r.data(), n); return r;
}
// Own one SQLite connection (RAII: destruction closes it, including on errors).
// Startup, scanning and searching use separate connections. An uncommitted
// scan is rolled back when its statements are finalized and this connection closes.
struct DB {
    sqlite3* p = nullptr;
    DB() {
        int rc = sqlite3_open(utf8(databasePath).c_str(), &p);
        if (rc != SQLITE_OK) { std::string e = p ? sqlite3_errmsg(p) : "Cannot open database"; sqlite3_close(p); throw std::runtime_error(e); }
        sqlite3_busy_timeout(p, 5000);
    }
    ~DB() { sqlite3_close(p); }
    void exec(const char* sql) { if (sqlite3_exec(p, sql, nullptr, nullptr, nullptr) != SQLITE_OK) throw std::runtime_error(sqlite3_errmsg(p)); }
};
// Own a compiled SQL statement. Values are bound, never concatenated into SQL.
// SQLITE_TRANSIENT makes SQLite copy the string, so temporary inputs are safe.
struct Statement {
    sqlite3_stmt* p = nullptr;
    Statement(DB& db, const char* sql) { if (sqlite3_prepare_v2(db.p, sql, -1, &p, nullptr) != SQLITE_OK) throw std::runtime_error(sqlite3_errmsg(db.p)); }
    ~Statement() { sqlite3_finalize(p); }
    void bind(int n, const std::string& s) { if (sqlite3_bind_text(p, n, s.c_str(), int(s.size()), SQLITE_TRANSIENT) != SQLITE_OK) throw std::runtime_error("Cannot bind database value"); }
    // Execute a write, then reuse the same prepared statement for the next row.
    void insert() { if (sqlite3_step(p) != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(sqlite3_db_handle(p))); sqlite3_reset(p); sqlite3_clear_bindings(p); }
};
// Freeze scan/search inputs while a worker owns the index-replacement operation.
void enabled(bool value) {
    for (HWND h : {folderBox, recursiveBox, queryBox, exactBox, browseButton, scanButton, searchButton}) EnableWindow(h, value);
}
// Worker-thread entry point. Each successful scan replaces the whole index, not
// just changed files. The transaction makes deletion + new rows + settings atomic.
void scan(std::wstring root, bool recursive) {
    std::wstring message;
    try {
        DB db;
        // Reserve the writer before deleting. Until COMMIT, failure preserves the
        // previous on-disk index; individual unreadable files are instead skipped.
        db.exec("BEGIN IMMEDIATE; DELETE FROM uses;");
        Statement add(db, "INSERT OR IGNORE INTO uses(tag,path) VALUES(?,?)");
        size_t indexed = 0, large = 0, unreadable = 0, links = 0;
        // Both recursive and single-folder traversal share these filtering rules.
        auto visit = [&](const fs::directory_entry& entry) {
            std::error_code ec;
            if (entry.is_symlink(ec)) { ++links; return; }
            if (!entry.is_regular_file(ec)) { if (ec) ++unreadable; return; }
            fs::path path = entry.path();
            std::wstring p = path.wstring();
            // Avoid indexing our own database and temporary SQLite sidecars.
            if (_wcsicmp(p.c_str(), databasePath.c_str()) == 0
                || _wcsicmp(p.c_str(), (databasePath + L"-journal").c_str()) == 0
                || _wcsicmp(p.c_str(), (databasePath + L"-wal").c_str()) == 0
                || _wcsicmp(p.c_str(), (databasePath + L"-shm").c_str()) == 0) return;
            auto size = entry.file_size(ec);
            if (ec) { ++unreadable; return; }
            if (size > 1048576) { ++large; return; }
            std::ifstream file(path, std::ios::binary);
            if (!file) { ++unreadable; return; }
            // Read at most the limit plus one byte, even if a file grows during scanning.
            std::vector<unsigned char> data(1048577);
            file.read(reinterpret_cast<char*>(data.data()), data.size());
            if (file.bad() || (file.fail() && !file.eof())) { ++unreadable; return; }
            data.resize(size_t(file.gcount()));
            if (data.size() > 1048576) { ++large; return; }
            auto tags = extractTags(data);
            std::string filePath = utf8(p);
            // extractTags() deduplicates within this file; the composite SQL key
            // also prevents repeating the same tag/path pair across inserts.
            for (const auto& tag : tags) { add.bind(1, tag); add.bind(2, filePath); add.insert(); }
            ++indexed;
            if (indexed % 25 == 0) PostMessageW(window, Progress, indexed, 0);
        };
        if (recursive) {
            for (const auto& entry : fs::recursive_directory_iterator(fs::path(root))) visit(entry);
        } else {
            for (const auto& entry : fs::directory_iterator(fs::path(root))) visit(entry);
        }
        // Save preferences in the same transaction, so they describe this index.
        Statement settings(db, "INSERT OR REPLACE INTO settings(key,value) VALUES(?,?)");
        settings.bind(1, "scan_folder"); settings.bind(2, utf8(root)); settings.insert();
        settings.bind(1, "scan_recursive"); settings.bind(2, recursive ? "1" : "0"); settings.insert();
        db.exec("COMMIT;");
        message = L"Scan saved to tags.sqlite3: " + std::to_wstring(indexed) + L" files indexed; " + std::to_wstring(large)
                + L" over 1 MB; " + std::to_wstring(unreadable) + L" unreadable; " + std::to_wstring(links) + L" links skipped.";
    } catch (const std::exception& e) { message = L"Scan failed; previous index retained. " + wide(e.what()); }
    PostMessageW(window, Done, 0, reinterpret_cast<LPARAM>(new std::wstring(message)));
}
// UI-thread query of saved data only: searching never reads the source files.
void search() {
    try {
        DB db;
        bool exact = SendMessageW(exactBox, BM_GETCHECK, 0, 0) == BST_CHECKED;
        std::string q = utf8(text(queryBox));
        ListView_DeleteAllItems(results);
        if (q.empty()) { SetWindowTextW(statusBox, L"Enter a tag name to search."); return; }
        // instr(), unlike LIKE, treats '_' and '%' literally. Fetch one extra row
        // to detect that the 5,000-row display limit was reached. Exact lookup can
        // use the NOCASE index; substring matching scans candidate tag strings.
        Statement s(db, exact ? "SELECT tag,path FROM uses WHERE tag=? COLLATE NOCASE ORDER BY tag,path LIMIT 5001"
                            : "SELECT tag,path FROM uses WHERE instr(lower(tag),lower(?))>0 ORDER BY tag,path LIMIT 5001");
        s.bind(1, q);
        int count = 0, rc;
        while ((rc = sqlite3_step(s.p)) == SQLITE_ROW) {
            if (count == 5000) break;
            std::wstring tag = wide(reinterpret_cast<const char*>(sqlite3_column_text(s.p, 0)));
            std::wstring path = wide(reinterpret_cast<const char*>(sqlite3_column_text(s.p, 1)));
            LVITEMW item{}; item.mask = LVIF_TEXT; item.iItem = count; item.pszText = tag.data();
            int row = ListView_InsertItem(results, &item);
            auto name = fs::path(path).filename().wstring();
            ListView_SetItemText(results, row, 1, name.data());
            ListView_SetItemText(results, row, 2, path.data());
            ++count;
        }
        if (rc != SQLITE_DONE && rc != SQLITE_ROW) throw std::runtime_error(sqlite3_errmsg(db.p));
        std::wstring msg = std::to_wstring(count) + (rc == SQLITE_ROW ? L" results shown (limit reached). Refine your search." : L" tag/file matches. Right-click to copy a field; double-click to locate the file.");
        SetWindowTextW(statusBox, msg.c_str());
    } catch (const std::exception& e) { MessageBoxW(window, wide(e.what()).c_str(), L"Search error", MB_ICONERROR); }
}
// Read the full cell value, growing the buffer instead of truncating long paths
// or tag strings. This is the shared source for all clipboard actions.
std::wstring resultField(int row, int column) {
    std::vector<wchar_t> buffer(256);
    for (;;) {
        LVITEMW item{}; item.iSubItem = column; item.pszText = buffer.data(); item.cchTextMax = int(buffer.size());
        int count = int(SendMessageW(results, LVM_GETITEMTEXTW, WPARAM(row), LPARAM(&item)));
        if (count < int(buffer.size()) - 1) return std::wstring(buffer.data(), count);
        buffer.resize(buffer.size() * 2);
    }
}
// Copy one selected row's field, or its three tab-separated fields, as Unicode.
void copyResult(UINT command) {
    int row = ListView_GetNextItem(results, -1, LVNI_SELECTED);
    if (row < 0) { SetWindowTextW(statusBox, L"Select a result first, then right-click to choose a field to copy."); return; }
    int column = command == CopyField ? selectedColumn : int(command - CopyTag);
    std::wstring value = command == CopyRow
        ? resultField(row, 0) + L"\t" + resultField(row, 1) + L"\t" + resultField(row, 2)
        : resultField(row, column);
    SIZE_T bytes = (value.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!memory) { MessageBoxW(window, L"Not enough memory to copy the selected field.", L"Copy failed", MB_ICONERROR); return; }
    void* destination = GlobalLock(memory);
    if (!destination) { GlobalFree(memory); MessageBoxW(window, L"Cannot prepare clipboard text.", L"Copy failed", MB_ICONERROR); return; }
    std::memcpy(destination, value.c_str(), bytes); GlobalUnlock(memory);
    bool copied = false;
    // On success Windows owns this allocation. Free it ourselves only on failure.
    if (OpenClipboard(window)) {
        if (EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, memory)) copied = true;
        CloseClipboard();
    }
    if (!copied) {
        GlobalFree(memory);
        MessageBoxW(window, L"Cannot access the clipboard. Please try again.", L"Copy failed", MB_ICONERROR);
        return;
    }
    const wchar_t* labels[] = {L"Tag name copied.", L"File name copied.", L"Full path copied."};
    SetWindowTextW(statusBox, command == CopyRow ? L"Row copied (tab-separated)." : labels[column]);
}
// Mouse invocation selects the row/column under the pointer. Keyboard invocation
// uses the current selection; (-1,-1) is Win32's keyboard-menu position marker.
void copyMenu(LPARAM location) {
    POINT point{int(short(LOWORD(location))), int(short(HIWORD(location)))};
    if (point.x == -1 && point.y == -1) {
        int row = ListView_GetNextItem(results, -1, LVNI_SELECTED);
        if (row < 0) return;
        RECT rect{}; ListView_GetItemRect(results, row, &rect, LVIR_BOUNDS);
        point = {rect.left + 12, rect.bottom}; ClientToScreen(results, &point);
    } else {
        LVHITTESTINFO hit{}; hit.pt = point; ScreenToClient(results, &hit.pt);
        int row = ListView_SubItemHitTest(results, &hit);
        if (row < 0 || hit.iSubItem < 0 || hit.iSubItem > 2) return;
        selectedColumn = hit.iSubItem;
        ListView_SetItemState(results, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_SetItemState(results, row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    }
    SetFocus(results);
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, CopyField, L"Copy clicked field\tCtrl+C");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, CopyTag, L"Copy tag name");
    AppendMenuW(menu, MF_STRING, CopyFileName, L"Copy file name");
    AppendMenuW(menu, MF_STRING, CopyPath, L"Copy full path");
    AppendMenuW(menu, MF_STRING, CopyRow, L"Copy entire row");
    UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, 0, window, nullptr);
    DestroyMenu(menu);
    if (command) copyResult(command);
}
// Create a child control with the shared GUI font and an ID used by proc().
HWND control(const wchar_t* cls, const wchar_t* title, DWORD style, int id) {
    HWND h = CreateWindowExW(cls == std::wstring(L"EDIT") ? WS_EX_CLIENTEDGE : 0, cls, title,
        WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, window, HMENU(INT_PTR(id)), nullptr, nullptr);
    SendMessageW(h, WM_SETFONT, WPARAM(GetStockObject(DEFAULT_GUI_FONT)), TRUE); return h;
}
// Main event dispatcher: construct/resize controls, handle user actions, consume
// worker notifications, and coordinate shutdown. Only this thread updates the UI.
LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        window = h;
        folderBox = control(L"EDIT", lastScanFolder.c_str(), ES_AUTOHSCROLL | WS_TABSTOP, 10);
        browseButton = control(L"BUTTON", L"Choose folder...", WS_TABSTOP, 11);
        scanButton = control(L"BUTTON", L"Scan / replace index", WS_TABSTOP, 12);
        recursiveBox = control(L"BUTTON", L"Include subfolders", BS_AUTOCHECKBOX | WS_TABSTOP, 13);
        SendMessageW(recursiveBox, BM_SETCHECK, lastScanRecursive ? BST_CHECKED : BST_UNCHECKED, 0);
        queryBox = control(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, 14);
        SendMessageW(queryBox, EM_SETCUEBANNER, 0, LPARAM(L"Tag name, e.g. Me.UPSStatus_BF"));
        searchButton = control(L"BUTTON", L"Search", WS_TABSTOP, 15);
        exactBox = control(L"BUTTON", L"Exact name", BS_AUTOCHECKBOX | WS_TABSTOP, 16);
        results = control(WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | WS_TABSTOP, 17);
        ListView_SetExtendedListViewStyle(results, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
        int col = 0;
        for (const auto* title : {L"Tag name", L"File name", L"Full path"}) {
            LVCOLUMNW c{}; c.mask = LVCF_TEXT | LVCF_WIDTH; c.pszText = const_cast<wchar_t*>(title); c.cx = col == 2 ? 540 : 250;
            ListView_InsertColumn(results, col++, &c);
        }
        statusBox = control(L"STATIC", L"Search the saved tags.sqlite3 index anytime. Scan / replace index refreshes it when your files change.", 0, 18);
        return 0;
    }
    case WM_SIZE: {
        int w = LOWORD(lp), height = HIWORD(lp);
        MoveWindow(folderBox, 12, 12, w-342, 28, TRUE);
        MoveWindow(browseButton, w-322, 12, 140, 28, TRUE); MoveWindow(scanButton, w-174, 12, 162, 28, TRUE);
        MoveWindow(recursiveBox, 12, 47, 220, 24, TRUE);
        MoveWindow(queryBox, 12, 80, w-266, 28, TRUE); MoveWindow(searchButton, w-246, 80, 96, 28, TRUE);
        MoveWindow(exactBox, w-140, 80, 128, 28, TRUE);
        MoveWindow(results, 12, 120, w-24, height-170, TRUE); MoveWindow(statusBox, 12, height-42, w-24, 36, TRUE); return 0;
    }
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lp); info->ptMinTrackSize = {720, 400}; return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wp) >= CopyField && LOWORD(wp) <= CopyRow) { copyResult(LOWORD(wp)); return 0; }
        if (LOWORD(wp) == 11) {
            IFileDialog* dialog = nullptr;
            if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
                DWORD options; dialog->GetOptions(&options); dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
                if (SUCCEEDED(dialog->Show(h))) {
                    IShellItem* item = nullptr;
                    if (SUCCEEDED(dialog->GetResult(&item))) {
                        PWSTR path = nullptr;
                        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) { SetWindowTextW(folderBox, path); CoTaskMemFree(path); }
                        item->Release();
                    }
                }
                dialog->Release();
            }
        } else if (LOWORD(wp) == 12 && !busy) {
            std::wstring root = text(folderBox);
            std::error_code ec;
            if (root.empty() || !fs::is_directory(fs::path(root), ec)) { MessageBoxW(h, L"Choose an existing folder first.", L"Tag Finder", MB_ICONINFORMATION); return 0; }
            busy = true; enabled(false); SetWindowTextW(statusBox, L"Scanning... The saved index will be replaced when the scan succeeds.");
            bool recursive = SendMessageW(recursiveBox, BM_GETCHECK, 0, 0) == BST_CHECKED;
            // Pass copies of the inputs. WM_CLOSE keeps the window alive until
            // this detached worker has posted Done and the UI has processed it.
            std::thread(scan, fs::absolute(fs::path(root)).lexically_normal().wstring(), recursive).detach();
        } else if (LOWORD(wp) == 15) search();
        return 0;
    case WM_CONTEXTMENU:
        if (reinterpret_cast<HWND>(wp) == results) { copyMenu(lp); return 0; }
        break;
    case WM_NOTIFY:
        if (reinterpret_cast<NMHDR*>(lp)->idFrom == 17 && reinterpret_cast<NMHDR*>(lp)->code == NM_CLICK) {
            auto* item = reinterpret_cast<NMITEMACTIVATE*>(lp);
            if (item->iItem >= 0 && item->iSubItem >= 0 && item->iSubItem <= 2) selectedColumn = item->iSubItem;
        }
        if (reinterpret_cast<NMHDR*>(lp)->idFrom == 17 && reinterpret_cast<NMHDR*>(lp)->code == NM_DBLCLK) {
            int row = ListView_GetNextItem(results, -1, LVNI_SELECTED);
            if (row >= 0) {
                wchar_t path[32768]; ListView_GetItemText(results, row, 2, path, 32768);
                PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(path);
                if (pidl) { SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0); ILFree(pidl); }
                else MessageBoxW(h, L"The file is no longer available. Rescan to refresh the index.", L"Tag Finder", MB_ICONINFORMATION);
            }
        }
        return 0;
    case Progress: {
        std::wstring s = L"Scanning... " + std::to_wstring(wp) + L" files indexed."; SetWindowTextW(statusBox, s.c_str()); return 0;
    }
    case Done: {
        // Release the worker's message allocation and allow the next operation.
        std::unique_ptr<std::wstring> s(reinterpret_cast<std::wstring*>(lp)); busy = false; enabled(true);
        ListView_DeleteAllItems(results); SetWindowTextW(statusBox, s->c_str()); return 0;
    }
    case WM_CLOSE:
        if (busy) { MessageBoxW(h, L"Please wait for the scan to finish.", L"Scan in progress", MB_ICONINFORMATION); return 0; }
        DestroyWindow(h); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}
// Windows GUI entry point: initialize COM/common controls, locate the portable
// database, ensure its schema, restore preferences, then run the message loop.
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    SetProcessDPIAware();
    INITCOMMONCONTROLSEX cc{sizeof(cc), ICC_LISTVIEW_CLASSES}; InitCommonControlsEx(&cc);
    try {
        std::vector<wchar_t> executable(32768);
        DWORD length = GetModuleFileNameW(nullptr, executable.data(), DWORD(executable.size()));
        if (!length || length >= executable.size()) throw std::runtime_error("Cannot locate the executable folder");
        fs::path dir = fs::path(std::wstring(executable.data(), length)).parent_path();
        // Resolve beside the EXE, not beside the process's current working folder.
        databasePath = (dir / L"tags.sqlite3").wstring();
        DB db;
        // DELETE journaling leaves one portable database after a clean commit.
        // CREATE IF NOT EXISTS also opens older indexes without discarding data.
        db.exec("PRAGMA journal_mode=DELETE; PRAGMA synchronous=FULL;"
                "CREATE TABLE IF NOT EXISTS uses(tag TEXT NOT NULL,path TEXT NOT NULL,PRIMARY KEY(tag,path));"
                "CREATE INDEX IF NOT EXISTS tag_lookup ON uses(tag COLLATE NOCASE);"
                "CREATE TABLE IF NOT EXISTS settings(key TEXT PRIMARY KEY,value TEXT NOT NULL);");
        {
            Statement settings(db, "SELECT key,value FROM settings");
            int rc;
            while ((rc = sqlite3_step(settings.p)) == SQLITE_ROW) {
                std::string key = reinterpret_cast<const char*>(sqlite3_column_text(settings.p, 0));
                std::string value = reinterpret_cast<const char*>(sqlite3_column_text(settings.p, 1));
                if (key == "scan_folder") lastScanFolder = wide(value);
                else if (key == "scan_recursive") lastScanRecursive = value == "1";
            }
            if (rc != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(db.p));
        }
        WNDCLASSW wc{}; wc.lpfnWndProc = proc; wc.hInstance = instance; wc.lpszClassName = L"LayoutTagFinder";
        wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(101));
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW); wc.hbrBackground = HBRUSH(COLOR_BTNFACE+1); RegisterClassW(&wc);
        HWND h = CreateWindowW(wc.lpszClassName, L"Layout Tag Finder", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1100, 680, nullptr, nullptr, instance, nullptr);
        if (!h) throw std::runtime_error("Cannot create window");
        SendMessageW(h, WM_SETICON, ICON_SMALL, LPARAM(LoadImageW(instance, MAKEINTRESOURCEW(101), IMAGE_ICON,
            GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED)));
        ShowWindow(h, show);
        MSG msg;
        // Handle app shortcuts before dialog-style Tab navigation and dispatch.
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            if (msg.message == WM_KEYDOWN && msg.wParam == 'C' && (GetKeyState(VK_CONTROL) & 0x8000)
                && GetFocus() == results) { copyResult(CopyField); continue; }
            if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN && GetFocus() == queryBox && !busy) { search(); continue; }
            if (!IsDialogMessageW(h, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        }
    } catch (const std::exception& e) {
        std::wstring message = wide(e.what()) + L"\n\nDatabase: " + databasePath
            + L"\nKeep TagFinder.exe and tags.sqlite3 in a folder you can write to, such as Documents or a USB drive.";
        MessageBoxW(nullptr, message.c_str(), L"Tag Finder error", MB_ICONERROR);
    }
    CoUninitialize(); return 0;
}
