// places_wcx.cpp - Total Commander WCX (packer) plugin for Firefox places.sqlite
//
// The plugin exposes the bookmarks of a Firefox profile as an archive:
// every folder of the bookmark tree becomes a directory and every bookmark
// becomes an Internet shortcut (.url file).  The database itself is never
// modified: it is copied to %TEMP% together with its -wal/-shm journals,
// opened read-only through the bundled sqlite3 amalgamation, and the copies
// are removed as soon as the tree has been read into memory.

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include <windows.h>

#include <map>
#include <new>
#include <set>
#include <string>
#include <vector>

#include "sqlite3.h"

// ---------------------------------------------------------------------------
// Total Commander WCX API definitions (subset of wcxhead.h)
// ---------------------------------------------------------------------------

#define PK_SKIP 0
#define PK_TEST 1
#define PK_EXTRACT 2

#define PK_CAPS_BY_CONTENT 64

#define E_SUCCESS 0
#define E_END_ARCHIVE 10
#define E_BAD_ARCHIVE 13
#define E_ECREATE 16
#define E_EWRITE 19
#define E_NOT_SUPPORTED 24

typedef int(__stdcall* tChangeVolProcW)(LPWSTR ArcName, int Mode);
typedef int(__stdcall* tProcessDataProcW)(LPWSTR FileName, int Size);

struct tHeaderDataExW {
  WCHAR ArcName[1024];
  WCHAR FileName[1024];
  int Flags;
  DWORD PackSizeHigh;
  DWORD PackSize;
  DWORD UnpSizeHigh;
  DWORD UnpSize;
  int HostOS;
  int FileCRC;
  int FileTime;
  int UnpVer;
  int Method;
  int FileAttr;
  char* CmtBuf;
  int CmtBufSize;
  int CmtSize;
  int CmtState;
  int Reserved[1024];
};

#define WCX_EXPORT extern "C" __declspec(dllexport)

namespace {

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

std::wstring ToLower(std::wstring s) {
  for (wchar_t& c : s) {
    if (c >= L'A' && c <= L'Z') c = (wchar_t)(c - L'A' + L'a');
  }
  return s;
}

bool StartsWithNoCase(const std::wstring& s, const wchar_t* prefix) {
  size_t n = wcslen(prefix);
  return s.size() >= n && _wcsnicmp(s.c_str(), prefix, n) == 0;
}

bool EndsWithNoCase(const std::wstring& s, const wchar_t* suffix) {
  size_t n = wcslen(suffix);
  return s.size() >= n && _wcsnicmp(s.c_str() + (s.size() - n), suffix, n) == 0;
}

std::string WideToUtf8(const std::wstring& s) {
  if (s.empty()) return std::string();
  int len = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0,
                                nullptr, nullptr);
  if (len <= 0) return std::string();
  std::string out((size_t)len, '\0');
  WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], len, nullptr,
                      nullptr);
  return out;
}

std::wstring Utf8ToWide(const char* s, int len) {
  if (!s || len <= 0) return std::wstring();
  int wlen = MultiByteToWideChar(CP_UTF8, 0, s, len, nullptr, 0);
  if (wlen <= 0) return std::wstring();
  std::wstring out((size_t)wlen, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s, len, &out[0], wlen);
  return out;
}

DWORD CurrentDosTime() {
  SYSTEMTIME st;
  GetLocalTime(&st);
  FILETIME ft;
  WORD date = 0, time = 0;
  if (!SystemTimeToFileTime(&st, &ft) || !FileTimeToDosDateTime(&ft, &date, &time)) {
    return 0;
  }
  return ((DWORD)time << 16) | date;
}

DWORD DosTimeFromFirefoxMicros(sqlite3_int64 micros) {
  if (micros <= 0) return CurrentDosTime();
  const sqlite3_int64 kEpochDiff = 11644473600LL;  // 1601 -> 1970 in seconds
  sqlite3_int64 secs = micros / 1000000;
  unsigned long long ticks = (unsigned long long)(secs + kEpochDiff) * 10000000ULL;
  FILETIME ft;
  ft.dwLowDateTime = (DWORD)(ticks & 0xFFFFFFFFULL);
  ft.dwHighDateTime = (DWORD)(ticks >> 32);
  FILETIME local;
  WORD date = 0, time = 0;
  if (!FileTimeToLocalFileTime(&ft, &local)) return CurrentDosTime();
  if (!FileTimeToDosDateTime(&local, &date, &time)) return CurrentDosTime();
  return ((DWORD)time << 16) | date;
}

bool IsReservedDeviceName(const std::wstring& name) {
  std::wstring base = name;
  size_t dot = base.find(L'.');
  if (dot != std::wstring::npos) base = base.substr(0, dot);
  base = ToLower(base);
  while (!base.empty() && base.back() == L' ') base.pop_back();
  static const wchar_t* kReserved[] = {L"con",  L"prn",  L"aux",  L"nul",
                                       L"com1", L"com2", L"com3", L"com4",
                                       L"com5", L"com6", L"com7", L"com8",
                                       L"com9", L"lpt1", L"lpt2", L"lpt3",
                                       L"lpt4", L"lpt5", L"lpt6", L"lpt7",
                                       L"lpt8", L"lpt9"};
  for (const wchar_t* r : kReserved) {
    if (base == r) return true;
  }
  return false;
}

// Replaces every character Windows rejects in a file or directory name.
std::wstring SanitizeName(const std::wstring& in) {
  std::wstring out;
  out.reserve(in.size());
  for (wchar_t c : in) {
    if (c < 0x20 || c == L'\\' || c == L'/' || c == L':' || c == L'*' ||
        c == L'?' || c == L'"' || c == L'<' || c == L'>' || c == L'|') {
      out.push_back(L'_');
    } else {
      out.push_back(c);
    }
  }
  size_t first = out.find_first_not_of(L' ');
  out = (first == std::wstring::npos) ? std::wstring() : out.substr(first);
  while (!out.empty() && (out.back() == L' ' || out.back() == L'.')) out.pop_back();
  if (out.size() > 100) out.resize(100);
  while (!out.empty() && (out.back() == L' ' || out.back() == L'.')) out.pop_back();
  if (out.empty()) out = L"_";
  if (IsReservedDeviceName(out)) out.insert(out.begin(), L'_');
  return out;
}

std::wstring UniqueName(const std::wstring& base, const std::wstring& ext,
                        std::set<std::wstring>& used) {
  std::wstring candidate = base + ext;
  int n = 1;
  while (used.count(ToLower(candidate))) {
    ++n;
    candidate = base + L" (" + std::to_wstring(n) + L")" + ext;
  }
  used.insert(ToLower(candidate));
  return candidate;
}

// ---------------------------------------------------------------------------
// Temporary workspace holding the database copies
// ---------------------------------------------------------------------------

class TempWorkspace {
 public:
  ~TempWorkspace() { Cleanup(); }

  bool Create() {
    wchar_t tmp[MAX_PATH + 1] = {0};
    DWORD len = GetTempPathW(MAX_PATH, tmp);
    if (len == 0 || len > MAX_PATH) return false;
    std::wstring dir = std::wstring(tmp) + L"places_wcx_" +
                       std::to_wstring(GetCurrentProcessId()) + L"_" +
                       std::to_wstring((unsigned long long)GetTickCount64());
    if (!CreateDirectoryW(dir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
      return false;
    }
    dir_ = dir;
    return true;
  }

  // Copies src (if it exists) into the workspace; returns the copy's path or
  // an empty string when the source is missing or cannot be copied.
  std::wstring CopyIn(const std::wstring& src, const std::wstring& name) {
    if (dir_.empty() || src.empty()) return std::wstring();
    if (GetFileAttributesW(src.c_str()) == INVALID_FILE_ATTRIBUTES) return std::wstring();
    std::wstring dst = dir_ + L"\\" + name;
    if (!CopyFileW(src.c_str(), dst.c_str(), FALSE)) return std::wstring();
    files_.push_back(dst);
    return dst;
  }

 private:
  void Cleanup() {
    for (const std::wstring& f : files_) DeleteFileW(f.c_str());
    files_.clear();
    if (!dir_.empty()) {
      RemoveDirectoryW(dir_.c_str());
      dir_.clear();
    }
  }

  std::wstring dir_;
  std::vector<std::wstring> files_;
};

// ---------------------------------------------------------------------------
// Archive model
// ---------------------------------------------------------------------------

struct Entry {
  std::wstring path;    // relative path inside the archive, backslash separated
  std::string content;  // .url payload
  DWORD dosTime = 0;
};

struct ArchiveHandle {
  std::vector<Entry> entries;
  std::wstring archiveName;  // source database path, reported back as ArcName
  size_t index = 0;
  tChangeVolProcW changeVol = nullptr;
  tProcessDataProcW processData = nullptr;
};

struct Node {
  sqlite3_int64 id = 0;
  sqlite3_int64 parent = 0;
  int type = 0;
  std::wstring title;
  std::wstring url;
  DWORD dosTime = 0;
};

std::wstring ColumnText(sqlite3_stmt* st, int col) {
  const unsigned char* text = sqlite3_column_text(st, col);
  int bytes = sqlite3_column_bytes(st, col);
  return Utf8ToWide(reinterpret_cast<const char*>(text), bytes);
}

std::wstring BookmarkBaseName(const Node& node) {
  std::wstring base = SanitizeName(node.title.empty() ? node.url : node.title);
  if (EndsWithNoCase(base, L".url")) base.resize(base.size() - 4);
  return base.empty() ? std::wstring(L"Bookmark") : base;
}

Entry MakeBookmarkEntry(const Node& node, const std::wstring& name,
                        const std::wstring& parentPath) {
  Entry file;
  file.path = parentPath + name;
  file.dosTime = node.dosTime;
  file.content = "[InternetShortcut]\r\nURL=" + WideToUtf8(node.url) + "\r\n";
  return file;
}

void EmitTree(const std::map<sqlite3_int64, Node>& nodes,
              const std::map<sqlite3_int64, std::vector<sqlite3_int64> >& children,
              sqlite3_int64 parentId, const std::wstring& parentPath,
              std::vector<Entry>& out, int depth) {
  if (depth > 64) return;
  std::map<sqlite3_int64, std::vector<sqlite3_int64> >::const_iterator it =
      children.find(parentId);
  if (it == children.end()) return;

  std::set<std::wstring> used;
  for (sqlite3_int64 childId : it->second) {
    std::map<sqlite3_int64, Node>::const_iterator nit = nodes.find(childId);
    if (nit == nodes.end()) continue;
    const Node& node = nit->second;

    if (node.type == 2) {  // folder: only its contents are listed, the path of
                           // every bookmark carries the folder name
      std::wstring base = SanitizeName(node.title.empty() ? L"Folder" : node.title);
      std::wstring name = UniqueName(base, L"", used);
      EmitTree(nodes, children, childId, parentPath + name + L"\\", out, depth + 1);
    } else if (node.type == 1) {  // bookmark
      if (node.url.empty()) continue;
      if (StartsWithNoCase(node.url, L"place:")) continue;
      std::wstring name = UniqueName(BookmarkBaseName(node), L".url", used);
      out.push_back(MakeBookmarkEntry(node, name, parentPath));
    }
  }
}

struct RootFolder {
  const char* guid;
  const wchar_t* name;
};

// The four top level folders of a Firefox profile, addressed by guid because the
// numeric ids differ between profiles.
const RootFolder kRootFolders[] = {
    {"menu________", L"Меню закладок"},
    {"toolbar_____", L"Панель закладок"},
    {"unfiled_____", L"Другие закладки"},
    {"mobile______", L"Мобильные закладки"},
};

std::map<std::string, sqlite3_int64> FindRootIds(sqlite3* db) {
  std::map<std::string, sqlite3_int64> ids;
  static const char* kSql =
      "SELECT id, guid FROM moz_bookmarks WHERE guid IN "
      "('menu________','toolbar_____','unfiled_____','mobile______')";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db, kSql, -1, &st, nullptr) != SQLITE_OK) {
    if (st) sqlite3_finalize(st);
    return ids;
  }
  while (sqlite3_step(st) == SQLITE_ROW) {
    const unsigned char* guid = sqlite3_column_text(st, 1);
    if (!guid) continue;
    ids[reinterpret_cast<const char*>(guid)] = sqlite3_column_int64(st, 0);
  }
  sqlite3_finalize(st);
  return ids;
}

bool BuildEntries(sqlite3* db, std::vector<Entry>& out) {
  static const char* kSqlWithOrder =
      "SELECT b.id, b.parent, b.type, b.title, p.url, b.dateAdded "
      "FROM moz_bookmarks b LEFT JOIN moz_places p ON p.id = b.fk "
      "ORDER BY b.parent, b.position";
  static const char* kSqlPlain =
      "SELECT b.id, b.parent, b.type, b.title, p.url, 0 "
      "FROM moz_bookmarks b LEFT JOIN moz_places p ON p.id = b.fk";

  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db, kSqlWithOrder, -1, &st, nullptr) != SQLITE_OK) {
    if (st) sqlite3_finalize(st);
    st = nullptr;
    if (sqlite3_prepare_v2(db, kSqlPlain, -1, &st, nullptr) != SQLITE_OK) {
      if (st) sqlite3_finalize(st);
      return false;
    }
  }

  std::map<sqlite3_int64, Node> nodes;
  std::map<sqlite3_int64, std::vector<sqlite3_int64> > children;
  while (sqlite3_step(st) == SQLITE_ROW) {
    Node node;
    node.id = sqlite3_column_int64(st, 0);
    node.parent = sqlite3_column_int64(st, 1);
    node.type = sqlite3_column_int(st, 2);
    node.title = ColumnText(st, 3);
    node.url = ColumnText(st, 4);
    node.dosTime = DosTimeFromFirefoxMicros(sqlite3_column_int64(st, 5));
    if (node.type != 1 && node.type != 2) continue;
    nodes[node.id] = node;
    children[node.parent].push_back(node.id);
  }
  sqlite3_finalize(st);

  if (nodes.empty()) return false;

  const std::map<std::string, sqlite3_int64> rootIds = FindRootIds(db);
  for (const RootFolder& root : kRootFolders) {
    std::map<std::string, sqlite3_int64>::const_iterator it = rootIds.find(root.guid);
    if (it == rootIds.end()) continue;
    EmitTree(nodes, children, it->second, std::wstring(root.name) + L"\\", out, 0);
  }

  if (out.empty()) {
    // Databases without the guid column: fall back to the folders that sit
    // directly below the root, keeping their stored titles.
    for (std::map<sqlite3_int64, Node>::const_iterator it = nodes.begin();
         it != nodes.end(); ++it) {
      const Node& node = it->second;
      if (node.type != 2 || node.parent != 1) continue;
      std::wstring base = SanitizeName(node.title.empty() ? L"Folder" : node.title);
      EmitTree(nodes, children, it->first, base + L"\\", out, 0);
    }
  }
  return !out.empty();
}

bool HasMozBookmarks(const std::wstring& path) {
  std::string utf8 = WideToUtf8(path);
  sqlite3* db = nullptr;
  if (sqlite3_open_v2(utf8.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
    if (db) sqlite3_close(db);
    return false;
  }
  bool found = false;
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db,
                         "SELECT 1 FROM sqlite_master WHERE type='table' "
                         "AND name='moz_bookmarks'",
                         -1, &st, nullptr) == SQLITE_OK) {
    found = sqlite3_step(st) == SQLITE_ROW;
  }
  if (st) sqlite3_finalize(st);
  sqlite3_close(db);
  return found;
}

bool HasSqliteHeader(const std::wstring& path) {
  HANDLE f = CreateFileW(path.c_str(), GENERIC_READ,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) return false;
  char header[16] = {0};
  DWORD read = 0;
  BOOL ok = ReadFile(f, header, sizeof(header), &read, nullptr);
  CloseHandle(f);
  static const char kMagic[16] = {'S', 'Q', 'L', 'i', 't', 'e', ' ', 'f',
                                  'o', 'r', 'm', 'a', 't', ' ', '3', '\0'};
  return ok && read == sizeof(header) && memcmp(header, kMagic, 16) == 0;
}

const Entry* FindEntry(ArchiveHandle* handle, LPCWSTR destName) {
  if (!destName || !*destName) return nullptr;
  std::wstring name = destName;
  while (!name.empty() && (name[0] == L'\\' || name[0] == L'/')) name.erase(0, 1);
  for (wchar_t& c : name) {
    if (c == L'/') c = L'\\';
  }
  for (const Entry& e : handle->entries) {
    if (_wcsicmp(e.path.c_str(), name.c_str()) == 0) return &e;
  }
  // Total Commander may pass just the file name when a single file is extracted;
  // accept it as long as it identifies exactly one entry.
  const Entry* match = nullptr;
  for (const Entry& e : handle->entries) {
    const size_t slash = e.path.rfind(L'\\');
    const std::wstring base =
        (slash == std::wstring::npos) ? e.path : e.path.substr(slash + 1);
    if (_wcsicmp(base.c_str(), name.c_str()) != 0) continue;
    if (match) return nullptr;
    match = &e;
  }
  return match;
}

void CreateParentDirectories(const std::wstring& path) {
  size_t start = 0;  // first position that may hold a separator to split on
  if (path.size() >= 2 && path[1] == L':') {
    start = 2;  // drive letter, e.g. C:
  } else if (path.size() >= 2 && path[0] == L'\\' && path[1] == L'\\') {
    size_t share = path.find(L'\\', 2);
    if (share != std::wstring::npos) share = path.find(L'\\', share + 1);
    start = (share == std::wstring::npos) ? path.size() : share;
  }
  for (size_t i = start + 1; i < path.size(); ++i) {
    if (path[i] == L'\\' || path[i] == L'/') {
      CreateDirectoryW(path.substr(0, i).c_str(), nullptr);
    }
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// Exported WCX interface
// ---------------------------------------------------------------------------

// Total Commander resolves the ANSI and the Unicode entry points with
// GetProcAddress while loading a WCX and refuses the plugin when one of them is
// missing, even though a Unicode plugin is driven only through the *W calls.
// The ANSI entry points exist so the load check passes; they reject every call
// because all real work happens in the wide versions below.

typedef int(__stdcall* tChangeVolProcA)(LPSTR ArcName, int Mode);
typedef int(__stdcall* tProcessDataProcA)(LPSTR FileName, int Size);

struct tHeaderDataA {
  char ArcName[260];
  char FileName[260];
  int Flags;
  int PackSize;
  int UnpSize;
  int HostOS;
  int FileCRC;
  int FileTime;
  int UnpVer;
  int Method;
  int FileAttr;
  char* CmtBuf;
  int CmtBufSize;
  int CmtSize;
  int CmtState;
};

struct tHeaderDataExA {
  char ArcName[1024];
  char FileName[1024];
  int Flags;
  DWORD PackSizeHigh;
  DWORD PackSize;
  DWORD UnpSizeHigh;
  DWORD UnpSize;
  int HostOS;
  int FileCRC;
  int FileTime;
  int UnpVer;
  int Method;
  int FileAttr;
  char* CmtBuf;
  int CmtBufSize;
  int CmtSize;
  int CmtState;
  int Reserved[1024];
};

WCX_EXPORT HANDLE __stdcall OpenArchive(LPSTR FileName, int Mode) {
  (void)FileName;
  (void)Mode;
  return nullptr;
}

WCX_EXPORT int __stdcall ReadHeader(HANDLE hArcData, tHeaderDataA* HeaderData) {
  (void)hArcData;
  (void)HeaderData;
  return E_NOT_SUPPORTED;
}

WCX_EXPORT int __stdcall ReadHeaderEx(HANDLE hArcData,
                                      tHeaderDataExA* HeaderData) {
  (void)hArcData;
  (void)HeaderData;
  return E_NOT_SUPPORTED;
}

WCX_EXPORT int __stdcall ProcessFile(HANDLE hArcData, int Operation,
                                     LPSTR DestPath, LPSTR DestName) {
  (void)hArcData;
  (void)Operation;
  (void)DestPath;
  (void)DestName;
  return E_NOT_SUPPORTED;
}

WCX_EXPORT void __stdcall SetChangeVolProc(HANDLE hArcData,
                                           tChangeVolProcA pChangeVolProc) {
  (void)hArcData;
  (void)pChangeVolProc;
}

WCX_EXPORT void __stdcall SetProcessDataProc(HANDLE hArcData,
                                             tProcessDataProcA pProcessDataProc) {
  (void)hArcData;
  (void)pProcessDataProc;
}

WCX_EXPORT BOOL __stdcall CanYouHandleThisFile(LPSTR FileName) {
  (void)FileName;
  return FALSE;
}

WCX_EXPORT HANDLE __stdcall OpenArchiveW(LPWSTR FileName, int Mode) {
  (void)Mode;
  if (!FileName || !*FileName) return nullptr;

  const std::wstring src(FileName);
  TempWorkspace workspace;
  if (!workspace.Create()) return nullptr;

  std::wstring dbPath = workspace.CopyIn(src, L"places.sqlite");
  if (dbPath.empty()) return nullptr;
  // The journals belong to the copied database, not to the original one.
  workspace.CopyIn(src + L"-wal", L"places.sqlite-wal");
  workspace.CopyIn(src + L"-shm", L"places.sqlite-shm");

  const std::string dbUtf8 = WideToUtf8(dbPath);
  sqlite3* db = nullptr;
  if (sqlite3_open_v2(dbUtf8.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
    if (db) {
      sqlite3_close(db);
      db = nullptr;
    }
    // A WAL database may need write access to its shm file; the workspace copy
    // is ours alone, so falling back to read/write keeps the original intact.
    if (sqlite3_open_v2(dbUtf8.c_str(), &db, SQLITE_OPEN_READWRITE, nullptr) !=
        SQLITE_OK) {
      if (db) sqlite3_close(db);
      return nullptr;
    }
  }
  sqlite3_busy_timeout(db, 2000);

  std::vector<Entry> entries;
  const bool built = BuildEntries(db, entries);
  sqlite3_close(db);
  if (!built) return nullptr;

  ArchiveHandle* handle = new (std::nothrow) ArchiveHandle();
  if (!handle) return nullptr;
  handle->archiveName = src;
  handle->entries.swap(entries);
  return reinterpret_cast<HANDLE>(handle);
}

WCX_EXPORT int __stdcall ReadHeaderExW(HANDLE hArcData, tHeaderDataExW* HeaderData) {
  ArchiveHandle* handle = reinterpret_cast<ArchiveHandle*>(hArcData);
  if (!handle || !HeaderData) return E_BAD_ARCHIVE;
  if (handle->index >= handle->entries.size()) return E_END_ARCHIVE;

  const Entry& entry = handle->entries[handle->index++];
  ZeroMemory(HeaderData, sizeof(*HeaderData));
  lstrcpynW(HeaderData->ArcName, handle->archiveName.c_str(), 1024);
  lstrcpynW(HeaderData->FileName, entry.path.c_str(), 1024);

  const DWORD size = (DWORD)entry.content.size();
  HeaderData->Flags = 0;
  HeaderData->PackSizeHigh = 0;
  HeaderData->PackSize = size;
  HeaderData->UnpSizeHigh = 0;
  HeaderData->UnpSize = size;
  HeaderData->HostOS = 0;
  HeaderData->FileCRC = 0;
  HeaderData->FileTime = (int)entry.dosTime;
  HeaderData->UnpVer = 1;
  HeaderData->Method = 0;
  HeaderData->FileAttr = FILE_ATTRIBUTE_ARCHIVE;
  HeaderData->CmtBuf = nullptr;
  HeaderData->CmtBufSize = 0;
  HeaderData->CmtSize = 0;
  HeaderData->CmtState = 0;
  return E_SUCCESS;
}

WCX_EXPORT int __stdcall ProcessFileW(HANDLE hArcData, int Operation,
                                      LPWSTR DestPath, LPWSTR DestName) {
  ArchiveHandle* handle = reinterpret_cast<ArchiveHandle*>(hArcData);
  if (!handle) return E_BAD_ARCHIVE;
  if (Operation == PK_SKIP || Operation == PK_TEST) return E_SUCCESS;
  if (Operation != PK_EXTRACT) return E_NOT_SUPPORTED;

  const Entry* entry = FindEntry(handle, DestName);
  if (!entry) return E_BAD_ARCHIVE;

  std::wstring target;
  if (DestPath && *DestPath) {
    target = DestPath;
    if (target.back() != L'\\' && target.back() != L'/') target += L'\\';
  }
  const wchar_t* relative = (DestName && *DestName) ? DestName : entry->path.c_str();
  while (*relative == L'\\' || *relative == L'/') ++relative;
  target += relative;

  // Total Commander hands over the whole relative path, so the folders that the
  // listing only implies have to be created here.
  CreateParentDirectories(target);

  HANDLE file = CreateFileW(target.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return E_ECREATE;

  DWORD written = 0;
  const BOOL ok = WriteFile(file, entry->content.data(),
                            (DWORD)entry->content.size(), &written, nullptr);
  CloseHandle(file);
  if (!ok || written != entry->content.size()) return E_EWRITE;

  if (handle->processData) {
    handle->processData(const_cast<LPWSTR>(entry->path.c_str()),
                        (int)entry->content.size());
  }
  return E_SUCCESS;
}

WCX_EXPORT void __stdcall CloseArchive(HANDLE hArcData) {
  delete reinterpret_cast<ArchiveHandle*>(hArcData);
}

WCX_EXPORT void __stdcall SetChangeVolProcW(HANDLE hArcData,
                                            tChangeVolProcW pChangeVolProcW) {
  ArchiveHandle* handle = reinterpret_cast<ArchiveHandle*>(hArcData);
  if (handle) handle->changeVol = pChangeVolProcW;
}

WCX_EXPORT void __stdcall SetProcessDataProcW(HANDLE hArcData,
                                              tProcessDataProcW pProcessDataProcW) {
  ArchiveHandle* handle = reinterpret_cast<ArchiveHandle*>(hArcData);
  if (handle) handle->processData = pProcessDataProcW;
}

WCX_EXPORT int __stdcall GetPackerCaps(void) { return PK_CAPS_BY_CONTENT; }

WCX_EXPORT BOOL __stdcall CanYouHandleThisFileW(LPWSTR FileName) {
  if (!FileName || !*FileName) return FALSE;
  const std::wstring path(FileName);
  const bool nameMatches = EndsWithNoCase(path, L"places.sqlite");
  if (!HasSqliteHeader(path)) return FALSE;
  if (nameMatches) return TRUE;
  return HasMozBookmarks(path) ? TRUE : FALSE;
}
