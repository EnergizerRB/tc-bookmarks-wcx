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

#include <cstddef>
#include <cstdio>
#include <cwctype>
#include <map>
#include <mutex>
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

#define PK_OM_LIST 0
#define PK_OM_EXTRACT 1

#if defined(_WIN64)
#define PLACES_WCX_ARCH "x64"
#else
#define PLACES_WCX_ARCH "x86"
#endif

#define PK_CAPS_NEW 1
#define PK_CAPS_MODIFY 2
#define PK_CAPS_MULTIPLE 4
#define PK_CAPS_DELETE 8
#define PK_CAPS_BY_CONTENT 64

#define E_SUCCESS 0
#define E_END_ARCHIVE 10
#define E_EOPEN 15
#define E_BAD_ARCHIVE 13
#define E_ECREATE 16
#define E_EWRITE 19
#define E_EABORTED 21
#define E_NOT_SUPPORTED 24

typedef int(__stdcall* tChangeVolProcW)(LPWSTR ArcName, int Mode);
typedef int(__stdcall* tProcessDataProcW)(LPWSTR FileName, int Size);

// Layout copied verbatim from the Total Commander wcxhead.h: natural compiler
// alignment, no #pragma pack.  Packing the structure shifts the pointer and the
// size fields, and Total Commander then reads an empty FileName.
struct tHeaderDataExW {
  WCHAR ArcName[1024];
  WCHAR FileName[1024];
  int Flags;
  DWORD PackSize;
  DWORD PackSizeHigh;
  DWORD UnpSize;
  DWORD UnpSizeHigh;
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
  char Reserved[1024];
};

static_assert(offsetof(tHeaderDataExW, ArcName) == 0, "wcxhead.h layout");
static_assert(offsetof(tHeaderDataExW, FileName) == 2048, "wcxhead.h layout");
static_assert(offsetof(tHeaderDataExW, Flags) == 4096, "wcxhead.h layout");
static_assert(offsetof(tHeaderDataExW, PackSize) == 4100, "wcxhead.h layout");
static_assert(offsetof(tHeaderDataExW, PackSizeHigh) == 4104, "wcxhead.h layout");
static_assert(offsetof(tHeaderDataExW, UnpSize) == 4108, "wcxhead.h layout");
static_assert(offsetof(tHeaderDataExW, UnpSizeHigh) == 4112, "wcxhead.h layout");
static_assert(offsetof(tHeaderDataExW, HostOS) == 4116, "wcxhead.h layout");
static_assert(offsetof(tHeaderDataExW, FileCRC) == 4120, "wcxhead.h layout");
static_assert(offsetof(tHeaderDataExW, FileTime) == 4124, "wcxhead.h layout");
static_assert(offsetof(tHeaderDataExW, UnpVer) == 4128, "wcxhead.h layout");
static_assert(offsetof(tHeaderDataExW, Method) == 4132, "wcxhead.h layout");
static_assert(offsetof(tHeaderDataExW, FileAttr) == 4136, "wcxhead.h layout");
#if defined(_WIN64)
static_assert(offsetof(tHeaderDataExW, CmtBuf) == 4144, "wcxhead.h layout");
static_assert(offsetof(tHeaderDataExW, CmtBufSize) == 4152, "wcxhead.h layout");
static_assert(offsetof(tHeaderDataExW, CmtSize) == 4156, "wcxhead.h layout");
static_assert(offsetof(tHeaderDataExW, CmtState) == 4160, "wcxhead.h layout");
static_assert(offsetof(tHeaderDataExW, Reserved) == 4164, "wcxhead.h layout");
static_assert(sizeof(tHeaderDataExW) == 5192, "wcxhead.h layout");
#else
static_assert(offsetof(tHeaderDataExW, CmtBuf) == 4140, "wcxhead.h layout");
static_assert(offsetof(tHeaderDataExW, CmtBufSize) == 4144, "wcxhead.h layout");
static_assert(offsetof(tHeaderDataExW, CmtSize) == 4148, "wcxhead.h layout");
static_assert(offsetof(tHeaderDataExW, CmtState) == 4152, "wcxhead.h layout");
static_assert(offsetof(tHeaderDataExW, Reserved) == 4156, "wcxhead.h layout");
static_assert(sizeof(tHeaderDataExW) == 5180, "wcxhead.h layout");
#endif

// OpenArchive and OpenArchiveW do not receive a plain file name but this
// structure; the first member is a pointer to the archive path.
struct tOpenArchiveDataW {
  WCHAR* ArcName;
  int OpenMode;
  int OpenResult;
  WCHAR* CmtBuf;
  int CmtBufSize;
  int CmtSize;
  int CmtState;
};

struct tOpenArchiveDataA {
  char* ArcName;
  int OpenMode;
  int OpenResult;
  char* CmtBuf;
  int CmtBufSize;
  int CmtSize;
  int CmtState;
};

#if defined(_WIN64)
static_assert(offsetof(tOpenArchiveDataW, OpenMode) == 8, "wcxhead.h layout");
static_assert(offsetof(tOpenArchiveDataW, OpenResult) == 12, "wcxhead.h layout");
static_assert(offsetof(tOpenArchiveDataW, CmtBuf) == 16, "wcxhead.h layout");
static_assert(offsetof(tOpenArchiveDataW, CmtBufSize) == 24, "wcxhead.h layout");
static_assert(offsetof(tOpenArchiveDataW, CmtSize) == 28, "wcxhead.h layout");
static_assert(offsetof(tOpenArchiveDataW, CmtState) == 32, "wcxhead.h layout");
static_assert(sizeof(tOpenArchiveDataW) == 40, "wcxhead.h layout");
static_assert(sizeof(tOpenArchiveDataA) == 40, "wcxhead.h layout");
#else
static_assert(offsetof(tOpenArchiveDataW, OpenMode) == 4, "wcxhead.h layout");
static_assert(offsetof(tOpenArchiveDataW, OpenResult) == 8, "wcxhead.h layout");
static_assert(offsetof(tOpenArchiveDataW, CmtBuf) == 12, "wcxhead.h layout");
static_assert(offsetof(tOpenArchiveDataW, CmtBufSize) == 16, "wcxhead.h layout");
static_assert(offsetof(tOpenArchiveDataW, CmtSize) == 20, "wcxhead.h layout");
static_assert(offsetof(tOpenArchiveDataW, CmtState) == 24, "wcxhead.h layout");
static_assert(sizeof(tOpenArchiveDataW) == 28, "wcxhead.h layout");
static_assert(sizeof(tOpenArchiveDataA) == 28, "wcxhead.h layout");
#endif

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

std::wstring AnsiToWide(const char* s) {
  if (!s || !*s) return std::wstring();
  const int len = (int)strlen(s);
  int wlen = MultiByteToWideChar(CP_ACP, 0, s, len, nullptr, 0);
  if (wlen <= 0) return std::wstring();
  std::wstring out((size_t)wlen, L'\0');
  MultiByteToWideChar(CP_ACP, 0, s, len, &out[0], wlen);
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
  return ((DWORD)date << 16) | time;
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
  return ((DWORD)date << 16) | time;
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
    return CreateIn(std::wstring(tmp, len));
  }

  bool CreateIn(const std::wstring& base) {
    if (base.empty()) return false;
    std::wstring dir = base;
    if (dir.back() != L'\\' && dir.back() != L'/') dir += L'\\';
    dir += L"places_wcx_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
           std::to_wstring((unsigned long long)GetTickCount64());
    if (!CreateDirectoryW(dir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
      return false;
    }
    dir_ = dir;
    dirs_.push_back(dir);
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
    for (const std::wstring& d : dirs_) RemoveDirectoryW(d.c_str());
    dirs_.clear();
    dir_.clear();
  }

  std::wstring dir_;
  std::vector<std::wstring> files_;
  std::vector<std::wstring> dirs_;
};

struct OpenAttempt {
  std::wstring target;  // what was opened, or the folder used for the copy
  int rc = SQLITE_OK;   // sqlite3_open_v2 result
  std::string error;    // sqlite3_errmsg of the failed call
  bool opened = false;
};

std::string EncodeUriPath(const std::wstring& path,
                          const char* query = "?mode=ro&nolock=1") {
  const std::string utf8 = WideToUtf8(path);
  const bool unc = utf8.size() >= 2 && utf8[0] == '\\' && utf8[1] == '\\';
  std::string uri = unc ? "file:" : "file:///";
  for (char c : utf8) {
    switch (c) {
      case '\\': uri.push_back('/'); break;
      case ' ': uri.append("%20"); break;
      case '%': uri.append("%25"); break;
      case '#': uri.append("%23"); break;
      case '?': uri.append("%3F"); break;
      default: uri.push_back(c); break;
    }
  }
  uri.append(query);
  return uri;
}

// Opening a write-ahead log database read-only succeeds and only the first
// read fails, because SQLite then needs write access to the -shm file.  The
// database counts as usable only when a statement actually runs.
bool CanReadDatabase(sqlite3* db) {
  if (!db) return false;
  sqlite3_stmt* st = nullptr;
  bool ok = false;
  if (sqlite3_prepare_v2(db, "SELECT count(*) FROM sqlite_master", -1, &st, nullptr) ==
      SQLITE_OK) {
    ok = sqlite3_step(st) == SQLITE_ROW;
  }
  if (st) sqlite3_finalize(st);
  return ok;
}

// Opens the database where it lives: no copy, no locking, read-only.
sqlite3* OpenReadOnlyUri(const std::wstring& path, OpenAttempt& attempt) {
  attempt.target = path;
  const std::string uri = EncodeUriPath(path);
  sqlite3* db = nullptr;
  attempt.rc =
      sqlite3_open_v2(uri.c_str(), &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI, nullptr);
  if (attempt.rc != SQLITE_OK) {
    attempt.error = db ? sqlite3_errmsg(db) : "sqlite3_open_v2 failed";
    if (db) sqlite3_close(db);
    return nullptr;
  }
  if (!CanReadDatabase(db)) {
    attempt.rc = sqlite3_extended_errcode(db);
    attempt.error = sqlite3_errmsg(db);
    sqlite3_close(db);
    return nullptr;
  }
  attempt.opened = true;
  attempt.error.clear();
  return db;
}

// immutable=1 ignores the write-ahead log and every lock: the last checkpointed
// state is read even when the copy was taken while Firefox was writing.
sqlite3* OpenImmutable(const std::wstring& path, OpenAttempt& attempt) {
  attempt.target = path;
  const std::string uri = EncodeUriPath(path, "?immutable=1");
  sqlite3* db = nullptr;
  attempt.rc =
      sqlite3_open_v2(uri.c_str(), &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI, nullptr);
  if (attempt.rc != SQLITE_OK) {
    attempt.error = db ? sqlite3_errmsg(db) : "sqlite3_open_v2 failed";
    if (db) sqlite3_close(db);
    return nullptr;
  }
  if (!CanReadDatabase(db)) {
    attempt.rc = sqlite3_extended_errcode(db);
    attempt.error = sqlite3_errmsg(db);
    sqlite3_close(db);
    return nullptr;
  }
  attempt.opened = true;
  attempt.error.clear();
  return db;
}

std::wstring TempDirectory() {
  wchar_t tmp[MAX_PATH + 1] = {0};
  const DWORD len = GetTempPathW(MAX_PATH, tmp);
  if (len == 0 || len > MAX_PATH) return std::wstring();
  return std::wstring(tmp, len);
}

// A plain log next to the %TEMP% folder: when Total Commander refuses to show a
// file from the archive, this still records what the plugin was asked to do.
void AppendLog(const std::string& line) {
  const std::wstring dir = TempDirectory();
  if (dir.empty()) return;
  const std::wstring path = dir + L"places_wcx.log";
  HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return;

  SYSTEMTIME st;
  GetLocalTime(&st);
  char stamp[32] = {0};
  std::snprintf(stamp, sizeof(stamp), "%02u:%02u:%02u.%03u ", st.wHour, st.wMinute,
                st.wSecond, st.wMilliseconds);
  const std::string text = std::string(stamp) + line + "\r\n";
  DWORD written = 0;
  WriteFile(file, text.data(), (DWORD)text.size(), &written, nullptr);
  CloseHandle(file);
}

void WriteReportFile(const std::string& text) {
  const std::wstring dir = TempDirectory();
  if (dir.empty()) return;
  const std::wstring path = dir + L"places_wcx_debug.txt";
  HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return;
  DWORD written = 0;
  WriteFile(file, text.data(), (DWORD)text.size(), &written, nullptr);
  CloseHandle(file);
}

std::wstring WindowsTempDirectory() {
  wchar_t win[MAX_PATH + 1] = {0};
  const UINT len = GetWindowsDirectoryW(win, MAX_PATH);
  if (len == 0 || len >= MAX_PATH) return std::wstring();
  return std::wstring(win, len) + L"\\Temp";
}

std::wstring ModuleDirectory() {
  HMODULE module = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(&ModuleDirectory), &module)) {
    return std::wstring();
  }
  wchar_t path[MAX_PATH * 2] = {0};
  const DWORD len = GetModuleFileNameW(module, path, (DWORD)(sizeof(path) / sizeof(path[0])));
  if (len == 0 || len >= sizeof(path) / sizeof(path[0])) return std::wstring();
  const std::wstring full(path, len);
  const size_t slash = full.find_last_of(L"\\/");
  return (slash == std::wstring::npos) ? std::wstring() : full.substr(0, slash);
}

// Fallback: work on a copy when the original cannot be read in place, for
// example while Firefox keeps the database open.
sqlite3* OpenCopiedDatabase(const std::wstring& src, const std::wstring& baseDir,
                            TempWorkspace& workspace, OpenAttempt& attempt) {
  attempt.target = baseDir.empty() ? L"%TEMP%" : baseDir;
  const bool created = baseDir.empty() ? workspace.Create() : workspace.CreateIn(baseDir);
  if (!created) {
    attempt.rc = SQLITE_CANTOPEN;
    attempt.error = "cannot create a temporary folder";
    return nullptr;
  }
  const std::wstring copy = workspace.CopyIn(src, L"tc_places_tmp.sqlite");
  if (copy.empty()) {
    attempt.rc = SQLITE_CANTOPEN;
    attempt.error = "cannot copy the database";
    return nullptr;
  }
  workspace.CopyIn(src + L"-wal", L"tc_places_tmp.sqlite-wal");
  workspace.CopyIn(src + L"-shm", L"tc_places_tmp.sqlite-shm");
  attempt.target = copy;

  sqlite3* db = OpenReadOnlyUri(copy, attempt);
  if (db) return db;

  // Replaying a WAL needs write access to the shm file; the copy is ours alone.
  const std::string path8 = WideToUtf8(copy);
  attempt.rc = sqlite3_open_v2(path8.c_str(), &db, SQLITE_OPEN_READWRITE, nullptr);
  if (attempt.rc == SQLITE_OK) {
    attempt.opened = true;
    attempt.error.clear();
    return db;
  }
  if (db) {
    attempt.error = sqlite3_errmsg(db);
    sqlite3_close(db);
  }

  // The three copies were taken at slightly different moments, so the log may
  // not match the file.  A copy of the main file alone, read without the log,
  // still shows every bookmark that Firefox checkpointed.
  const std::wstring plain = workspace.CopyIn(src, L"tc_places_plain.sqlite");
  if (plain.empty()) return nullptr;
  return OpenImmutable(plain, attempt);
}

// ---------------------------------------------------------------------------
// Archive model
// ---------------------------------------------------------------------------

struct Entry {
  std::wstring path;    // relative path inside the archive, backslash separated
  std::string content;  // .url payload
  DWORD dosTime = 0;
  sqlite3_int64 nodeId = 0;  // row in moz_bookmarks, used by delete and rename
  bool isDirectory = false;
};

struct ArchiveHandle {
  std::vector<Entry> entries;
  std::wstring archiveName;  // source database path, reported back as ArcName
  size_t index = 0;
  size_t lastIndex = 0;  // entry returned by the previous ReadHeaderExW
  tChangeVolProcW changeVol = nullptr;
  tProcessDataProcW processData = nullptr;
};

// Total Commander calls the two callback setters with a placeholder handle
// (it passes -1) right before packing or deleting, when no archive is open at
// all, so a handle may only be dereferenced after it was handed out here.
std::mutex g_handleMutex;
std::set<ArchiveHandle*> g_liveHandles;
tProcessDataProcW g_processDataProc = nullptr;
tChangeVolProcW g_changeVolProc = nullptr;

void RememberHandle(ArchiveHandle* handle) {
  std::lock_guard<std::mutex> lock(g_handleMutex);
  g_liveHandles.insert(handle);
}

void ForgetHandle(ArchiveHandle* handle) {
  std::lock_guard<std::mutex> lock(g_handleMutex);
  g_liveHandles.erase(handle);
}

ArchiveHandle* LookupHandle(HANDLE arcData) {
  std::lock_guard<std::mutex> lock(g_handleMutex);
  ArchiveHandle* handle = reinterpret_cast<ArchiveHandle*>(arcData);
  return g_liveHandles.count(handle) ? handle : nullptr;
}

// Progress callback for the operations that run without an open archive.
bool ReportProgress(const std::wstring& name, int size) {
  tProcessDataProcW proc = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_handleMutex);
    proc = g_processDataProc;
  }
  if (!proc) return true;
  return proc(const_cast<LPWSTR>(name.c_str()), size) != FALSE;
}

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

Entry MakeFolderEntry(const Node& node, const std::wstring& name,
                      const std::wstring& parentPath) {
  Entry dir;
  dir.path = parentPath + name + L"\\";
  dir.dosTime = node.dosTime;
  dir.nodeId = node.id;
  dir.isDirectory = true;
  return dir;
}

Entry MakeBookmarkEntry(const Node& node, const std::wstring& name,
                        const std::wstring& parentPath) {
  Entry file;
  file.path = parentPath + name;
  file.dosTime = node.dosTime;
  file.nodeId = node.id;
  file.content = "[InternetShortcut]\r\nURL=" + WideToUtf8(node.url) + "\r\n";
  return file;
}

void EmitTree(const std::map<sqlite3_int64, Node>& nodes,
              const std::map<sqlite3_int64, std::vector<sqlite3_int64> >& children,
              sqlite3_int64 parentId, const std::wstring& parentPath,
              std::vector<Entry>& out, int depth,
              std::map<std::wstring, sqlite3_int64>* folderIds) {
  if (depth > 64) return;
  std::map<sqlite3_int64, std::vector<sqlite3_int64> >::const_iterator it =
      children.find(parentId);
  if (it == children.end()) return;

  std::set<std::wstring> used;
  for (sqlite3_int64 childId : it->second) {
    std::map<sqlite3_int64, Node>::const_iterator nit = nodes.find(childId);
    if (nit == nodes.end()) continue;
    const Node& node = nit->second;

    if (node.type == 2) {
      std::wstring base = SanitizeName(node.title.empty() ? L"Folder" : node.title);
      std::wstring name = UniqueName(base, L"", used);
      if (folderIds) (*folderIds)[ToLower(parentPath + name)] = childId;
      out.push_back(MakeFolderEntry(node, name, parentPath));
      EmitTree(nodes, children, childId, parentPath + name + L"\\", out, depth + 1,
               folderIds);
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

int CountRows(sqlite3* db, const char* sql) {
  sqlite3_stmt* st = nullptr;
  int count = -1;
  if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) {
    if (st) sqlite3_finalize(st);
    return count;
  }
  if (sqlite3_step(st) == SQLITE_ROW) count = sqlite3_column_int(st, 0);
  sqlite3_finalize(st);
  return count;
}

bool BuildEntries(sqlite3* db, std::vector<Entry>& out, int* bookmarkRows,
                  std::string& error, bool* readError,
                  std::map<std::wstring, sqlite3_int64>* folderIds = nullptr) {
  if (readError) *readError = false;
  static const char* kSqlWithOrder =
      "SELECT b.id, b.parent, b.type, b.title, p.url, b.dateAdded "
      "FROM moz_bookmarks b LEFT JOIN moz_places p ON p.id = b.fk "
      "ORDER BY b.parent, b.position";
  static const char* kSqlPlain =
      "SELECT b.id, b.parent, b.type, b.title, p.url, 0 "
      "FROM moz_bookmarks b LEFT JOIN moz_places p ON p.id = b.fk";

  if (bookmarkRows) *bookmarkRows = CountRows(db, "SELECT count(*) FROM moz_bookmarks");

  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db, kSqlWithOrder, -1, &st, nullptr) != SQLITE_OK) {
    if (st) sqlite3_finalize(st);
    st = nullptr;
    if (sqlite3_prepare_v2(db, kSqlPlain, -1, &st, nullptr) != SQLITE_OK) {
      if (st) sqlite3_finalize(st);
      error = std::string("cannot read moz_bookmarks: ") + sqlite3_errmsg(db);
      if (readError) *readError = true;
      return false;
    }
  }

  std::map<sqlite3_int64, Node> nodes;
  std::map<sqlite3_int64, std::vector<sqlite3_int64> > children;
  int step = SQLITE_ROW;
  while ((step = sqlite3_step(st)) == SQLITE_ROW) {
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
  if (step != SQLITE_DONE) {
    error = std::string("reading moz_bookmarks failed: ") + sqlite3_errmsg(db);
    if (readError) *readError = true;
    return false;
  }

  if (nodes.empty()) {
    error = "moz_bookmarks holds no bookmark or folder rows";
    return false;
  }

  const std::map<std::string, sqlite3_int64> rootIds = FindRootIds(db);
  for (const RootFolder& root : kRootFolders) {
    std::map<std::string, sqlite3_int64>::const_iterator it = rootIds.find(root.guid);
    if (it == rootIds.end()) continue;
    if (folderIds) (*folderIds)[ToLower(root.name)] = it->second;
    Node rootNode;
    rootNode.id = it->second;
    out.push_back(MakeFolderEntry(rootNode, root.name, std::wstring()));
    EmitTree(nodes, children, it->second, std::wstring(root.name) + L"\\", out, 0,
             folderIds);
  }

  if (out.empty()) {
    // Databases without the guid column: fall back to the folders that sit
    // directly below the root, keeping their stored titles.
    for (std::map<sqlite3_int64, Node>::const_iterator it = nodes.begin();
         it != nodes.end(); ++it) {
      const Node& node = it->second;
      if (node.type != 2 || node.parent != 1) continue;
      std::wstring base = SanitizeName(node.title.empty() ? L"Folder" : node.title);
      if (folderIds) (*folderIds)[ToLower(base)] = it->first;
      out.push_back(MakeFolderEntry(node, base, std::wstring()));
      EmitTree(nodes, children, it->first, base + L"\\", out, 0, folderIds);
    }
  }

  if (out.empty()) {
    // Last resort: the profile has an unfamiliar folder layout, so list every
    // bookmark that carries a URL and name the folder it lives in.
    std::set<std::wstring> used;
    for (std::map<sqlite3_int64, Node>::const_iterator it = nodes.begin();
         it != nodes.end(); ++it) {
      const Node& node = it->second;
      if (node.type != 1 || node.url.empty()) continue;
      if (StartsWithNoCase(node.url, L"place:")) continue;
      const std::wstring name = UniqueName(BookmarkBaseName(node), L".url", used);
      out.push_back(MakeBookmarkEntry(node, name, std::wstring()));
    }
  }

  if (out.empty()) {
    error = "the database holds no bookmark with a URL";
    return false;
  }
  return true;
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
  DWORD PackSize;
  DWORD PackSizeHigh;
  DWORD UnpSize;
  DWORD UnpSizeHigh;
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
  char Reserved[1024];
};

HANDLE OpenArchivePath(const std::wstring& src, int mode, int* result);

WCX_EXPORT HANDLE __stdcall OpenArchive(tOpenArchiveDataA* ArchiveData) {
  if (!ArchiveData) return nullptr;
  ArchiveData->CmtSize = 0;
  ArchiveData->CmtState = 0;
  if (!ArchiveData->ArcName || !*ArchiveData->ArcName) {
    ArchiveData->OpenResult = E_EOPEN;
    return nullptr;
  }
  int result = E_SUCCESS;
  HANDLE handle = OpenArchivePath(AnsiToWide(ArchiveData->ArcName),
                                  ArchiveData->OpenMode, &result);
  ArchiveData->OpenResult = result;
  return handle;
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

WCX_EXPORT int __stdcall PackFiles(LPSTR PackedFile, LPSTR SubPath, LPSTR SrcPath,
                                  LPSTR AddList, int Flags) {
  (void)PackedFile;
  (void)SubPath;
  (void)SrcPath;
  (void)AddList;
  (void)Flags;
  return E_NOT_SUPPORTED;
}

WCX_EXPORT int __stdcall DeleteFiles(LPSTR PackedFile, LPSTR DeleteList) {
  (void)PackedFile;
  (void)DeleteList;
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

std::string FileHeaderHex(const std::wstring& path) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return std::string("cannot open");
  unsigned char buffer[16] = {0};
  DWORD read = 0;
  const BOOL ok = ReadFile(file, buffer, sizeof(buffer), &read, nullptr);
  CloseHandle(file);
  if (!ok || read == 0) return std::string("empty");
  std::string hex;
  char byte[4];
  for (DWORD i = 0; i < read; ++i) {
    std::snprintf(byte, sizeof(byte), "%02X ", buffer[i]);
    hex += byte;
  }
  return hex;
}

std::string ListTables(const std::wstring& path) {
  sqlite3* db = nullptr;
  const std::string uri = EncodeUriPath(path);
  if (sqlite3_open_v2(uri.c_str(), &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI, nullptr) !=
      SQLITE_OK) {
    if (db) sqlite3_close(db);
    return std::string("cannot list");
  }
  sqlite3_stmt* st = nullptr;
  std::string tables;
  if (sqlite3_prepare_v2(db, "SELECT name FROM sqlite_master WHERE type='table' ORDER BY name",
                         -1, &st, nullptr) == SQLITE_OK) {
    while (sqlite3_step(st) == SQLITE_ROW) {
      const unsigned char* name = sqlite3_column_text(st, 0);
      if (!name) continue;
      if (!tables.empty()) tables += ", ";
      tables += reinterpret_cast<const char*>(name);
    }
  }
  if (st) sqlite3_finalize(st);
  sqlite3_close(db);
  return tables.empty() ? std::string("none") : tables;
}

// Firefox keeps several databases in the same folder; when the wrong one is
// opened the report can point straight at the right one.
std::wstring SiblingPlacesDatabase(const std::wstring& path) {
  const size_t slash = path.find_last_of(L"\\/");
  if (slash == std::wstring::npos) return std::wstring();
  const std::wstring candidate = path.substr(0, slash + 1) + L"places.sqlite";
  if (_wcsicmp(candidate.c_str(), path.c_str()) == 0) return std::wstring();
  return GetFileAttributesW(candidate.c_str()) == INVALID_FILE_ATTRIBUTES
             ? std::wstring()
             : candidate;
}

std::string BuildDebugReport(const std::wstring& arcName,
                             const std::vector<OpenAttempt>& attempts,
                             int bookmarkRows, const std::string& error) {
  // UTF-8 BOM: the report may contain Cyrillic paths and Total Commander's
  // lister only switches to UTF-8 when it sees one.
  std::string out = "\xEF\xBB\xBF";
  out += "places_wcx could not read any bookmark from this database.\r\n";
  out += "===========================================================\r\n\r\n";
  if (error.find("no such table: moz_bookmarks") != std::string::npos) {
    out += "This is not a places database: it has no moz_bookmarks table.\r\n";
    const std::wstring sibling = SiblingPlacesDatabase(arcName);
    if (!sibling.empty()) {
      out += "The places database in the same folder is:\r\n  " +
             WideToUtf8(sibling) + "\r\n";
    } else {
      out += "Open places.sqlite from the Firefox profile folder instead.\r\n";
    }
    out += "\r\n";
  }
  out += "Archive name (as passed by Total Commander):\r\n  " + WideToUtf8(arcName) +
         "\r\n\r\n";
#if defined(_WIN64)
  out += "Plugin build           : x64\r\n";
#else
  out += "Plugin build           : x86\r\n";
#endif
  out += "sizeof(tHeaderDataExW) : " + std::to_string(sizeof(tHeaderDataExW)) + "\r\n";
  out += "plugin build           : " __DATE__ " " __TIME__ "\r\n";
  out += "sqlite version         : " + std::string(sqlite3_libversion()) + "\r\n";

  WIN32_FILE_ATTRIBUTE_DATA info;
  if (GetFileAttributesExW(arcName.c_str(), GetFileExInfoStandard, &info)) {
    const unsigned long long size =
        ((unsigned long long)info.nFileSizeHigh << 32) | info.nFileSizeLow;
    out += "database file          : " + std::to_string(size) + " bytes\r\n";
  } else {
    out += "database file          : cannot be read (error " +
           std::to_string(GetLastError()) + ")\r\n";
  }
  const wchar_t* const sideFiles[] = {L"-wal", L"-shm"};
  std::string sides;
  for (const wchar_t* suffix : sideFiles) {
    if (GetFileAttributesW((arcName + suffix).c_str()) != INVALID_FILE_ATTRIBUTES) {
      sides += WideToUtf8(suffix + 1);
      sides += " ";
    }
  }
  out += "side files present     : " + (sides.empty() ? std::string("none") : sides) + "\r\n";
  out += "first 16 bytes         : " + FileHeaderHex(arcName) + "\r\n";
  out += "tables                 : " + ListTables(arcName) + "\r\n";
  out += "moz_bookmarks rows     : " + std::to_string(bookmarkRows) + "\r\n";
  if (!error.empty()) out += "Reason                 : " + error + "\r\n";

  out += "\r\nAttempts to open the database:\r\n";
  for (size_t i = 0; i < attempts.size(); ++i) {
    const OpenAttempt& a = attempts[i];
    out += "  " + std::to_string(i + 1) + ") " + WideToUtf8(a.target) + "\r\n";
    out += "     sqlite3_open_v2 = " + std::to_string(a.rc);
    out += a.opened ? " (opened)\r\n" : " (failed)\r\n";
    if (!a.error.empty()) out += "     sqlite3_errmsg  = " + a.error + "\r\n";
  }

  out +=
      "\r\nA row count of 0 means the file is not a Firefox places database or\r\n"
      "the profile never stored bookmarks.\r\n";
  return out;
}

WCX_EXPORT HANDLE __stdcall OpenArchiveW(tOpenArchiveDataW* ArchiveData) {
  if (!ArchiveData) return nullptr;
  ArchiveData->CmtSize = 0;
  ArchiveData->CmtState = 0;
  if (!ArchiveData->ArcName || !*ArchiveData->ArcName) {
    ArchiveData->OpenResult = E_EOPEN;
    return nullptr;
  }
  int result = E_SUCCESS;
  HANDLE handle = OpenArchivePath(std::wstring(ArchiveData->ArcName),
                                  ArchiveData->OpenMode, &result);
  ArchiveData->OpenResult = result;
  return handle;
}

HANDLE OpenArchivePath(const std::wstring& src, int mode, int* result) {
  *result = E_SUCCESS;
  if (mode != PK_OM_LIST && mode != PK_OM_EXTRACT) {
    *result = E_NOT_SUPPORTED;
    return nullptr;
  }

  AppendLog("OpenArchiveW '" + WideToUtf8(src) + "' mode=" + std::to_string(mode) +
            " build=" __DATE__ " " __TIME__ " arch=" PLACES_WCX_ARCH);
  std::vector<OpenAttempt> attempts;
  std::vector<Entry> entries;
  int bookmarkRows = 0;
  std::string error;
  TempWorkspace workspace;  // removes the fallback copies when it goes away
  bool built = false;
  bool readError = false;

  OpenAttempt direct;
  sqlite3* db = OpenReadOnlyUri(src, direct);
  attempts.push_back(direct);
  const bool directOpened = db != nullptr;
  if (db) {
    built = BuildEntries(db, entries, &bookmarkRows, error, &readError);
    sqlite3_close(db);
  }
  AppendLog("  direct rc=" + std::to_string(direct.rc) +
            (direct.error.empty() ? "" : " " + direct.error) +
            " rows=" + std::to_string(bookmarkRows) +
            (error.empty() ? "" : " reason=" + error));

  // Copies are only worth trying when the database could not be read in place.
  if (!built && (!directOpened || readError)) {
    entries.clear();
    const std::wstring folders[] = {WindowsTempDirectory(), ModuleDirectory(),
                                    std::wstring()};
    for (const std::wstring& folder : folders) {
      OpenAttempt attempt;
      sqlite3* copy = OpenCopiedDatabase(src, folder, workspace, attempt);
      AppendLog("  copy in '" + WideToUtf8(attempt.target) + "' rc=" +
                std::to_string(attempt.rc) + (attempt.error.empty() ? "" : " " + attempt.error));
      attempts.push_back(attempt);
      if (!copy) continue;
      built = BuildEntries(copy, entries, &bookmarkRows, error, &readError);
      sqlite3_close(copy);
      if (built) break;
      entries.clear();
    }
  }

  if (!built) {
    // An empty panel would hide the cause, so the archive always carries a file
    // that explains what happened; F3 on it shows the report.  The same text is
    // written to %TEMP%\places_wcx_debug.txt for when the viewer cannot open it.
    entries.clear();
    Entry debug;
    debug.path = L"DEBUG_INFO.txt";
    debug.content = BuildDebugReport(src, attempts, bookmarkRows, error);
    debug.dosTime = CurrentDosTime();
    entries.push_back(debug);
    WriteReportFile(debug.content);
  }
  AppendLog("  -> " + std::to_string(entries.size()) + " entries");

  ArchiveHandle* handle = new (std::nothrow) ArchiveHandle();
  if (!handle) return nullptr;
  handle->archiveName = src;
  handle->entries.swap(entries);
  RememberHandle(handle);
  return reinterpret_cast<HANDLE>(handle);
}

WCX_EXPORT int __stdcall ReadHeaderExW(HANDLE hArcData, tHeaderDataExW* HeaderData) {
  ArchiveHandle* handle = reinterpret_cast<ArchiveHandle*>(hArcData);
  if (!handle || !HeaderData) return E_BAD_ARCHIVE;
  if (handle->index >= handle->entries.size()) return E_END_ARCHIVE;

  const Entry& entry = handle->entries[handle->index++];
  handle->lastIndex = handle->index - 1;
  ZeroMemory(HeaderData, sizeof(*HeaderData));
  lstrcpynW(HeaderData->ArcName, handle->archiveName.c_str(), 1024);
  lstrcpynW(HeaderData->FileName, entry.path.c_str(), 1024);

  const DWORD size = entry.isDirectory ? 0 : (DWORD)entry.content.size();
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
  HeaderData->FileAttr = entry.isDirectory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_ARCHIVE;
  HeaderData->CmtBuf = nullptr;
  HeaderData->CmtBufSize = 0;
  HeaderData->CmtSize = 0;
  HeaderData->CmtState = 0;
  return E_SUCCESS;
}

std::wstring LastComponent(const std::wstring& path) {
  const size_t slash = path.find_last_of(L"\\/");
  return (slash == std::wstring::npos) ? path : path.substr(slash + 1);
}

bool IsAbsolutePath(const std::wstring& path) {
  if (path.size() >= 3 && path[1] == L':' && (path[2] == L'\\' || path[2] == L'/')) {
    return true;
  }
  return path.size() >= 2 && path[0] == L'\\' && path[1] == L'\\';
}

// A path that already names the destination file rather than a folder.
bool LooksLikeFilePath(const std::wstring& path) {
  if (path.empty() || path.back() == L'\\' || path.back() == L'/') return false;
  const std::wstring name = LastComponent(path);
  const size_t dot = name.find_last_of(L'.');
  return dot != std::wstring::npos && dot > 0;
}

int ExtractEntry(ArchiveHandle* handle, int Operation, LPWSTR DestPath,
                 LPWSTR DestName) {
  if (!handle) return E_BAD_ARCHIVE;
  if (Operation != PK_EXTRACT) return E_NOT_SUPPORTED;

  const std::wstring destPath = DestPath ? DestPath : L"";
  const std::wstring destName = DestName ? DestName : L"";

  const Entry* entry = nullptr;
  if (!destName.empty()) {
    entry = FindEntry(handle, destName.c_str());
    if (!entry && IsAbsolutePath(destName)) {
      entry = FindEntry(handle, LastComponent(destName).c_str());
    }
  }
  if (!entry && handle->lastIndex < handle->entries.size()) {
    // Total Commander may extract the entry it listed last without naming it.
    entry = &handle->entries[handle->lastIndex];
  }
  if (!entry && handle->entries.size() == 1) entry = &handle->entries[0];
  if (!entry) return E_BAD_ARCHIVE;
  if (entry->isDirectory) return E_SUCCESS;

  std::wstring target;
  if (!destPath.empty() && LooksLikeFilePath(destPath)) {
    target = destPath;  // Total Commander already named the output file
  } else if (IsAbsolutePath(destName)) {
    target = destName;
  } else {
    target = destPath.empty() ? TempDirectory() : destPath;
    if (!target.empty() && target.back() != L'\\' && target.back() != L'/') {
      target += L'\\';
    }
    const wchar_t* relative = destName.empty() ? entry->path.c_str() : destName.c_str();
    while (*relative == L'\\' || *relative == L'/') ++relative;
    target += relative;
  }

  // The listing implies folders without listing them, so create them here.
  CreateParentDirectories(target);

  HANDLE file = CreateFileW(target.c_str(), GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return E_ECREATE;

  DWORD written = 0;
  const BOOL ok = WriteFile(file, entry->content.data(), (DWORD)entry->content.size(),
                            &written, nullptr);
  CloseHandle(file);
  if (!ok || written != entry->content.size()) return E_EWRITE;

  if (handle->processData) {
    handle->processData(const_cast<LPWSTR>(entry->path.c_str()),
                        (int)entry->content.size());
  }
  return E_SUCCESS;
}

WCX_EXPORT int __stdcall ProcessFileW(HANDLE hArcData, int Operation,
                                      LPWSTR DestPath, LPWSTR DestName) {
  if (Operation == PK_SKIP || Operation == PK_TEST) return E_SUCCESS;
  const int rc = ExtractEntry(reinterpret_cast<ArchiveHandle*>(hArcData), Operation,
                              DestPath, DestName);
  AppendLog("ProcessFileW op=" + std::to_string(Operation) + " destPath='" +
            (DestPath ? WideToUtf8(DestPath) : std::string("<null>")) + "' destName='" +
            (DestName ? WideToUtf8(DestName) : std::string("<null>")) +
            "' rc=" + std::to_string(rc));
  return rc;
}

WCX_EXPORT int __stdcall CloseArchive(HANDLE hArcData) {
  if (ArchiveHandle* handle = LookupHandle(hArcData)) {
    ForgetHandle(handle);
    delete handle;
  }
  return E_SUCCESS;
}

WCX_EXPORT void __stdcall SetChangeVolProcW(HANDLE hArcData,
                                            tChangeVolProcW pChangeVolProcW) {
  {
    std::lock_guard<std::mutex> lock(g_handleMutex);
    g_changeVolProc = pChangeVolProcW;
  }
  if (ArchiveHandle* handle = LookupHandle(hArcData)) handle->changeVol = pChangeVolProcW;
}

WCX_EXPORT void __stdcall SetProcessDataProcW(HANDLE hArcData,
                                              tProcessDataProcW pProcessDataProcW) {
  {
    std::lock_guard<std::mutex> lock(g_handleMutex);
    g_processDataProc = pProcessDataProcW;
  }
  if (ArchiveHandle* handle = LookupHandle(hArcData)) handle->processData = pProcessDataProcW;
}

// ---------------------------------------------------------------------------
// Writing back into the profile database
// ---------------------------------------------------------------------------

// Total Commander edits a plugin archive through PackFilesW and DeleteFilesW
// alone: a new folder arrives as a pack call that names the folder, and a
// rename arrives as unpack, delete and pack under the new name.

std::wstring DirectoryOf(const std::wstring& path) {
  const size_t slash = path.find_last_of(L"\\/");
  return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash + 1);
}

std::vector<std::wstring> SplitArchivePath(const std::wstring& path) {
  std::vector<std::wstring> parts;
  std::wstring current;
  for (size_t i = 0; i < path.size(); ++i) {
    const wchar_t ch = path[i];
    if (ch == L'\\' || ch == L'/') {
      if (!current.empty()) {
        parts.push_back(current);
        current.clear();
      }
    } else {
      current.push_back(ch);
    }
  }
  if (!current.empty()) parts.push_back(current);
  return parts;
}

// Firefox keeps the profile open for as long as it runs.  Writing behind its
// back would desynchronise the bookmark tree it holds in memory, so the
// database is left untouched while the lock file is there.
bool FirefoxHoldsProfile(const std::wstring& dbPath) {
  const std::wstring dir = DirectoryOf(dbPath);
  if (dir.empty()) return false;
  return GetFileAttributesW((dir + L"parent.lock").c_str()) != INVALID_FILE_ATTRIBUTES ||
         GetFileAttributesW((dir + L"lock").c_str()) != INVALID_FILE_ATTRIBUTES;
}

void ReportWriteProblem(const std::wstring& text) {
  AppendLog("write refused: " + WideToUtf8(text));
  MessageBoxW(nullptr, text.c_str(), L"places_wcx",
              MB_ICONWARNING | MB_OK | MB_SETFOREGROUND);
}

sqlite3_int64 NowPrTime() {
  FILETIME fileTime;
  GetSystemTimeAsFileTime(&fileTime);
  ULARGE_INTEGER value;
  value.LowPart = fileTime.dwLowDateTime;
  value.HighPart = fileTime.dwHighDateTime;
  return static_cast<sqlite3_int64>(value.QuadPart / 10ULL - 11644473600000000ULL);
}

std::string NewGuid() {
  static const char kAlphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  static bool seeded = false;
  if (!seeded) {
    srand(static_cast<unsigned>(GetTickCount()));
    seeded = true;
  }
  std::string guid(12, 'A');
  for (size_t i = 0; i < guid.size(); ++i) guid[i] = kAlphabet[rand() % 64];
  return guid;
}

bool TableExists(sqlite3* db, const char* table) {
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db,
                         "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?1",
                         -1, &st, nullptr) != SQLITE_OK) {
    if (st) sqlite3_finalize(st);
    return false;
  }
  sqlite3_bind_text(st, 1, table, -1, SQLITE_TRANSIENT);
  const bool found = sqlite3_step(st) == SQLITE_ROW;
  sqlite3_finalize(st);
  return found;
}

bool HasColumn(sqlite3* db, const char* table, const char* column) {
  const std::string sql = std::string("PRAGMA table_info(") + table + ")";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
    if (st) sqlite3_finalize(st);
    return false;
  }
  bool found = false;
  while (sqlite3_step(st) == SQLITE_ROW) {
    const unsigned char* name = sqlite3_column_text(st, 1);
    if (name && _stricmp(reinterpret_cast<const char*>(name), column) == 0) {
      found = true;
      break;
    }
  }
  sqlite3_finalize(st);
  return found;
}

bool FileExists(const std::wstring& path) {
  const DWORD attributes = GetFileAttributesW(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES &&
         (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

// Mirrors the parts of the Firefox schema this plugin reads and writes, so a
// database created here behaves like a profile that holds no bookmarks yet.
bool CreateEmptyPlacesDb(const std::wstring& path, std::string& error) {
  sqlite3* db = nullptr;
  const std::string utf8 = WideToUtf8(path);
  if (sqlite3_open_v2(utf8.c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                      nullptr) != SQLITE_OK) {
    error = db ? sqlite3_errmsg(db) : "sqlite3_open_v2 failed";
    if (db) sqlite3_close(db);
    return false;
  }
  static const char* kSchema =
      "CREATE TABLE moz_places ("
      " id INTEGER PRIMARY KEY, url LONGVARCHAR, title LONGVARCHAR,"
      " rev_host LONGVARCHAR, visit_count INTEGER DEFAULT 0,"
      " hidden INTEGER DEFAULT 0, typed INTEGER DEFAULT 0,"
      " frecency INTEGER DEFAULT -1, last_visit_date INTEGER,"
      " guid TEXT, foreign_count INTEGER DEFAULT 0);"
      "CREATE TABLE moz_bookmarks ("
      " id INTEGER PRIMARY KEY, type INTEGER, fk INTEGER DEFAULT NULL,"
      " parent INTEGER, position INTEGER, title LONGVARCHAR,"
      " keyword_id INTEGER, folder_type TEXT, dateAdded INTEGER,"
      " lastModified INTEGER, guid TEXT);"
      "CREATE INDEX moz_bookmarks_parent ON moz_bookmarks (parent);"
      "CREATE INDEX moz_places_url ON moz_places (url);"
      "INSERT INTO moz_bookmarks (id, type, parent, position, title, dateAdded,"
      " lastModified, guid) VALUES"
      " (1, 2, 0, 0, '', 0, 0, 'root________'),"
      " (2, 2, 1, 0, 'Bookmarks Menu', 0, 0, 'menu________'),"
      " (3, 2, 1, 1, 'Bookmarks Toolbar', 0, 0, 'toolbar_____'),"
      " (4, 2, 1, 2, 'Other Bookmarks', 0, 0, 'unfiled_____'),"
      " (5, 2, 1, 3, 'Mobile Bookmarks', 0, 0, 'mobile______');";
  char* message = nullptr;
  if (sqlite3_exec(db, kSchema, nullptr, nullptr, &message) != SQLITE_OK) {
    error = message ? message : "cannot create the database schema";
    if (message) sqlite3_free(message);
    sqlite3_close(db);
    return false;
  }
  sqlite3_close(db);
  return true;
}

sqlite3* OpenForWrite(const std::wstring& path, std::string& error) {
  sqlite3* db = nullptr;
  const std::string utf8 = WideToUtf8(path);
  if (!FileExists(path) && !CreateEmptyPlacesDb(path, error)) return nullptr;
  const int rc = sqlite3_open_v2(utf8.c_str(), &db, SQLITE_OPEN_READWRITE, nullptr);
  if (rc != SQLITE_OK) {
    error = db ? sqlite3_errmsg(db) : "sqlite3_open_v2 failed";
    if (db) sqlite3_close(db);
    return nullptr;
  }
  sqlite3_busy_timeout(db, 5000);
  if (!CanReadDatabase(db)) {
    error = sqlite3_errmsg(db);
    sqlite3_close(db);
    return nullptr;
  }
  return db;
}

// Bookmarks are user data; one copy per write operation is cheap insurance
// against a mistake in this file.
std::wstring BackupBeforeWrite(const std::wstring& dbPath) {
  WIN32_FILE_ATTRIBUTE_DATA attributes;
  if (!GetFileAttributesExW(dbPath.c_str(), GetFileExInfoStandard, &attributes)) {
    return std::wstring();
  }
  const ULONGLONG size =
      (static_cast<ULONGLONG>(attributes.nFileSizeHigh) << 32) | attributes.nFileSizeLow;
  if (size > 64ULL * 1024 * 1024) return std::wstring();

  SYSTEMTIME now;
  GetLocalTime(&now);
  wchar_t name[80];
  _snwprintf_s(name, sizeof(name) / sizeof(name[0]), _TRUNCATE,
               L"places_wcx_backup_%04d%02d%02d_%02d%02d%02d.sqlite", now.wYear,
               now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
  const std::wstring target = TempDirectory() + name;
  return CopyFileW(dbPath.c_str(), target.c_str(), FALSE) ? target : std::wstring();
}

bool ExecSql(sqlite3* db, const char* sql, std::string& error) {
  char* message = nullptr;
  if (sqlite3_exec(db, sql, nullptr, nullptr, &message) != SQLITE_OK) {
    error = message ? message : "sqlite error";
    if (message) sqlite3_free(message);
    return false;
  }
  return true;
}

sqlite3_int64 InsertFolder(sqlite3* db, sqlite3_int64 parent, const std::wstring& title,
                           std::string& error) {
  static const char* kSql =
      "INSERT INTO moz_bookmarks (type, fk, parent, position, title, dateAdded,"
      " lastModified, guid) VALUES (2, NULL, ?1,"
      " (SELECT IFNULL(MAX(position), 0) + 1 FROM moz_bookmarks WHERE parent = ?1),"
      " ?2, ?3, ?3, ?4)";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db, kSql, -1, &st, nullptr) != SQLITE_OK) {
    if (st) sqlite3_finalize(st);
    error = sqlite3_errmsg(db);
    return 0;
  }
  const std::string title8 = WideToUtf8(title);
  const std::string guid = NewGuid();
  const sqlite3_int64 now = NowPrTime();
  sqlite3_bind_int64(st, 1, parent);
  sqlite3_bind_text(st, 2, title8.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 3, now);
  sqlite3_bind_text(st, 4, guid.c_str(), -1, SQLITE_TRANSIENT);
  const int rc = sqlite3_step(st);
  sqlite3_finalize(st);
  if (rc != SQLITE_DONE) {
    error = sqlite3_errmsg(db);
    return 0;
  }
  return sqlite3_last_insert_rowid(db);
}

std::wstring ReversedHost(const std::wstring& url) {
  size_t start = 0;
  const size_t scheme = url.find(L"://");
  if (scheme != std::wstring::npos) start = scheme + 3;
  size_t end = url.find_first_of(L"/?#", start);
  if (end == std::wstring::npos) end = url.size();
  std::wstring host = url.substr(start, end - start);
  const size_t at = host.find(L'@');
  if (at != std::wstring::npos) host = host.substr(at + 1);
  const size_t colon = host.find(L':');
  if (colon != std::wstring::npos) host.resize(colon);
  host = ToLower(host);
  return std::wstring(host.rbegin(), host.rend()) + L".";
}

sqlite3_int64 EnsurePlace(sqlite3* db, const std::wstring& url,
                          const std::wstring& title, std::string& error) {
  const std::string url8 = WideToUtf8(url);
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db, "SELECT id FROM moz_places WHERE url = ?1 LIMIT 1", -1, &st,
                         nullptr) == SQLITE_OK) {
    sqlite3_bind_text(st, 1, url8.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_int64 id = 0;
    if (sqlite3_step(st) == SQLITE_ROW) id = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    if (id) return id;
  } else if (st) {
    sqlite3_finalize(st);
  }

  const bool hasGuid = HasColumn(db, "moz_places", "guid");
  std::string sql =
      "INSERT INTO moz_places (url, title, rev_host, visit_count, hidden, typed,"
      " frecency";
  if (hasGuid) sql += ", guid";
  sql += ") VALUES (?1, ?2, ?3, 0, 0, 0, -1";
  if (hasGuid) sql += ", ?4";
  sql += ")";

  const std::string title8 = WideToUtf8(title);
  const std::string host8 = WideToUtf8(ReversedHost(url));
  const std::string guid = NewGuid();
  if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
    if (st) sqlite3_finalize(st);
    error = sqlite3_errmsg(db);
    return 0;
  }
  sqlite3_bind_text(st, 1, url8.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 2, title8.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(st, 3, host8.c_str(), -1, SQLITE_TRANSIENT);
  if (hasGuid) sqlite3_bind_text(st, 4, guid.c_str(), -1, SQLITE_TRANSIENT);
  const int rc = sqlite3_step(st);
  sqlite3_finalize(st);
  if (rc != SQLITE_DONE) {
    error = sqlite3_errmsg(db);
    return 0;
  }
  return sqlite3_last_insert_rowid(db);
}

sqlite3_int64 InsertBookmark(sqlite3* db, sqlite3_int64 parent,
                             const std::wstring& title, const std::wstring& url,
                             std::string& error) {
  const sqlite3_int64 place = EnsurePlace(db, url, title, error);
  if (!place) return 0;

  static const char* kSql =
      "INSERT INTO moz_bookmarks (type, fk, parent, position, title, dateAdded,"
      " lastModified, guid) VALUES (1, ?1, ?2,"
      " (SELECT IFNULL(MAX(position), 0) + 1 FROM moz_bookmarks WHERE parent = ?2),"
      " ?3, ?4, ?4, ?5)";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db, kSql, -1, &st, nullptr) != SQLITE_OK) {
    if (st) sqlite3_finalize(st);
    error = sqlite3_errmsg(db);
    return 0;
  }
  const std::string title8 = WideToUtf8(title);
  const std::string guid = NewGuid();
  const sqlite3_int64 now = NowPrTime();
  sqlite3_bind_int64(st, 1, place);
  sqlite3_bind_int64(st, 2, parent);
  sqlite3_bind_text(st, 3, title8.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(st, 4, now);
  sqlite3_bind_text(st, 5, guid.c_str(), -1, SQLITE_TRANSIENT);
  const int rc = sqlite3_step(st);
  sqlite3_finalize(st);
  if (rc != SQLITE_DONE) {
    error = sqlite3_errmsg(db);
    return 0;
  }
  return sqlite3_last_insert_rowid(db);
}

// Editing a bookmark in the archive re-packs the same name, so an existing row
// is re-pointed at the new URL instead of gaining a twin.
bool UpdateBookmarkUrl(sqlite3* db, sqlite3_int64 id, const std::wstring& title,
                       const std::wstring& url, std::string& error) {
  const sqlite3_int64 place = EnsurePlace(db, url, title, error);
  if (!place) return false;

  const bool hasLastModified = HasColumn(db, "moz_bookmarks", "lastModified");
  const std::string sql =
      hasLastModified ? "UPDATE moz_bookmarks SET fk = ?1, lastModified = ?2 WHERE id = ?3"
                      : "UPDATE moz_bookmarks SET fk = ?1 WHERE id = ?2";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
    if (st) sqlite3_finalize(st);
    error = sqlite3_errmsg(db);
    return false;
  }
  sqlite3_bind_int64(st, 1, place);
  if (hasLastModified) {
    sqlite3_bind_int64(st, 2, NowPrTime());
    sqlite3_bind_int64(st, 3, id);
  } else {
    sqlite3_bind_int64(st, 2, id);
  }
  const int rc = sqlite3_step(st);
  sqlite3_finalize(st);
  if (rc != SQLITE_DONE) {
    error = sqlite3_errmsg(db);
    return false;
  }
  return true;
}

// A folder is deleted together with everything below it, and every removed guid
// is recorded so that Firefox sync does not resurrect it.
bool DeleteNode(sqlite3* db, sqlite3_int64 id, std::string& error) {
  std::vector<sqlite3_int64> ids;
  sqlite3_stmt* st = nullptr;
  static const char* kSql =
      "WITH RECURSIVE sub(id) AS (SELECT ?1 UNION ALL "
      "SELECT b.id FROM moz_bookmarks b JOIN sub s ON b.parent = s.id) "
      "SELECT id FROM sub";
  if (sqlite3_prepare_v2(db, kSql, -1, &st, nullptr) != SQLITE_OK) {
    if (st) sqlite3_finalize(st);
    error = sqlite3_errmsg(db);
    return false;
  }
  sqlite3_bind_int64(st, 1, id);
  while (sqlite3_step(st) == SQLITE_ROW) ids.push_back(sqlite3_column_int64(st, 0));
  sqlite3_finalize(st);
  if (ids.empty()) {
    error = "no such bookmark";
    return false;
  }

  if (TableExists(db, "moz_bookmarks_deleted")) {
    for (size_t i = 0; i < ids.size(); ++i) {
      sqlite3_stmt* guid = nullptr;
      if (sqlite3_prepare_v2(db, "SELECT guid FROM moz_bookmarks WHERE id = ?1", -1, &guid,
                             nullptr) != SQLITE_OK) {
        if (guid) sqlite3_finalize(guid);
        continue;
      }
      sqlite3_bind_int64(guid, 1, ids[i]);
      std::string value;
      if (sqlite3_step(guid) == SQLITE_ROW) {
        const unsigned char* text = sqlite3_column_text(guid, 0);
        if (text) value = reinterpret_cast<const char*>(text);
      }
      sqlite3_finalize(guid);
      if (value.empty()) continue;

      sqlite3_stmt* tomb = nullptr;
      if (sqlite3_prepare_v2(db,
                             "INSERT OR REPLACE INTO moz_bookmarks_deleted (guid,"
                             " dateRemoved) VALUES (?1, ?2)",
                             -1, &tomb, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(tomb, 1, value.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(tomb, 2, NowPrTime());
        sqlite3_step(tomb);
      }
      if (tomb) sqlite3_finalize(tomb);
    }
  }

  for (size_t i = ids.size(); i-- > 0;) {
    sqlite3_stmt* del = nullptr;
    if (sqlite3_prepare_v2(db, "DELETE FROM moz_bookmarks WHERE id = ?1", -1, &del,
                           nullptr) != SQLITE_OK) {
      if (del) sqlite3_finalize(del);
      error = sqlite3_errmsg(db);
      return false;
    }
    sqlite3_bind_int64(del, 1, ids[i]);
    const int rc = sqlite3_step(del);
    sqlite3_finalize(del);
    if (rc != SQLITE_DONE) {
      error = sqlite3_errmsg(db);
      return false;
    }
  }
  return true;
}

// Walks the archive path and creates the folders that are missing.  New folders
// are added to the map so that the rest of the same call sees them.
sqlite3_int64 EnsureFolder(sqlite3* db,
                           std::map<std::wstring, sqlite3_int64>& folders,
                           const std::vector<std::wstring>& parts, std::string& error) {
  std::wstring path;
  sqlite3_int64 current = 0;
  for (size_t i = 0; i < parts.size(); ++i) {
    if (i) path += L'\\';
    path += parts[i];
    const std::wstring key = ToLower(path);
    std::map<std::wstring, sqlite3_int64>::const_iterator it = folders.find(key);
    if (it != folders.end()) {
      current = it->second;
      continue;
    }
    const std::wstring name = SanitizeName(parts[i]);
    current = InsertFolder(db, current, name.empty() ? L"Folder" : name, error);
    if (!current) return 0;
    folders[key] = current;
  }
  return current;
}

bool ReadUrlFromFile(const std::wstring& path, std::wstring* url) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  std::string data(65536, '\0');
  DWORD read = 0;
  const BOOL ok = ReadFile(file, &data[0], static_cast<DWORD>(data.size()), &read, nullptr);
  CloseHandle(file);
  if (!ok || read == 0) return false;
  data.resize(read);

  std::wstring text;
  if (read >= 2 && static_cast<unsigned char>(data[0]) == 0xFF &&
      static_cast<unsigned char>(data[1]) == 0xFE) {
    text.assign(reinterpret_cast<const wchar_t*>(data.data() + 2), (read - 2) / 2);
  } else if (read >= 3 && static_cast<unsigned char>(data[0]) == 0xEF &&
             static_cast<unsigned char>(data[1]) == 0xBB) {
    text = Utf8ToWide(data.c_str() + 3, static_cast<int>(read) - 3);
  } else {
    text = Utf8ToWide(data.c_str(), static_cast<int>(read));
  }

  size_t pos = 0;
  while (pos < text.size()) {
    size_t end = text.find_first_of(L"\r\n", pos);
    if (end == std::wstring::npos) end = text.size();
    const std::wstring line = text.substr(pos, end - pos);
    if (StartsWithNoCase(line, L"URL=")) {
      std::wstring value = line.substr(4);
      while (!value.empty() && (value.front() == L' ' || value.front() == L'\t')) {
        value.erase(value.begin());
      }
      while (!value.empty() && (value.back() == L' ' || value.back() == L'\t')) {
        value.pop_back();
      }
      if (!value.empty()) {
        *url = value;
        return true;
      }
    }
    pos = end + 1;
  }
  return false;
}

std::wstring FileUrlFor(const std::wstring& path) {
  std::wstring url = L"file:///";
  for (size_t i = 0; i < path.size(); ++i) url.push_back(path[i] == L'\\' ? L'/' : path[i]);
  return url;
}

bool AddBookmarkFromFile(sqlite3* db, sqlite3_int64 parent, const std::wstring& diskPath,
                         const std::wstring& fileName, std::string& error) {
  if (GetFileAttributesW(diskPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
    AppendLog("  source not found: '" + WideToUtf8(diskPath) + "'");
    return true;
  }
  std::wstring url;
  if (!ReadUrlFromFile(diskPath, &url)) url = FileUrlFor(diskPath);
  if (url.empty()) return true;

  std::wstring title = fileName;
  if (EndsWithNoCase(title, L".url")) title.resize(title.size() - 4);
  title = SanitizeName(title);
  if (title.empty()) title = L"Bookmark";
  return InsertBookmark(db, parent, title, url, error) != 0;
}

bool PackOne(sqlite3* db, sqlite3_int64 parent, const std::wstring& diskPath,
             const std::wstring& itemName, std::string& error, int depth) {
  if (depth > 32) return true;
  const DWORD attributes = GetFileAttributesW(diskPath.c_str());
  if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY)) {
    const std::wstring name = SanitizeName(itemName);
    const sqlite3_int64 folder =
        InsertFolder(db, parent, name.empty() ? L"Folder" : name, error);
    if (!folder) return false;

    WIN32_FIND_DATAW data;
    HANDLE find = FindFirstFileW((diskPath + L"\\*").c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) return true;
    bool ok = true;
    do {
      if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0) {
        continue;
      }
      if (!PackOne(db, folder, diskPath + L"\\" + data.cFileName, data.cFileName, error,
                   depth + 1)) {
        ok = false;
        break;
      }
    } while (FindNextFileW(find, &data));
    FindClose(find);
    return ok;
  }
  return AddBookmarkFromFile(db, parent, diskPath, itemName, error);
}

std::wstring StripPrefix(const std::wstring& path, const std::wstring& prefix) {
  if (prefix.empty() || path.size() <= prefix.size()) return path;
  if (_wcsnicmp(path.c_str(), prefix.c_str(), prefix.size()) != 0) return path;
  size_t skip = prefix.size();
  while (skip < path.size() && (path[skip] == L'\\' || path[skip] == L'/')) ++skip;
  return path.substr(skip);
}


// Total Commander only offers packing, folder creation and deletion after it
// has been told about the capabilities: an already installed plugin has to be
// removed and added again, otherwise the old mask stays in effect.
WCX_EXPORT int __stdcall GetPackerCaps(void) {
  // Total Commander refuses to pack into an archive unless the plugin claims
  // PK_CAPS_NEW, so a fresh database is created when the target is missing.
  const int caps = PK_CAPS_NEW | PK_CAPS_MODIFY | PK_CAPS_MULTIPLE | PK_CAPS_DELETE |
                   PK_CAPS_BY_CONTENT;
  AppendLog("GetPackerCaps -> " + std::to_string(caps) + " build=" __DATE__ " " __TIME__);
  return caps;
}

WCX_EXPORT int __stdcall PackFilesW(LPWSTR PackedFile, LPWSTR SubPath, LPWSTR SrcPath,
                                    LPWSTR AddList, int Flags) {
  const std::wstring archive = PackedFile ? PackedFile : L"";
  AppendLog("PackFilesW pack='" + WideToUtf8(archive) + "' sub='" +
            (SubPath ? WideToUtf8(SubPath) : std::string("<null>")) + "' src='" +
            (SrcPath ? WideToUtf8(SrcPath) : std::string("<null>")) + "' flags=" +
            std::to_string(Flags));
  for (const WCHAR* item = AddList; item && *item; item += wcslen(item) + 1) {
    AppendLog("  add '" + WideToUtf8(item) + "'");
  }
  if (archive.empty()) return E_ECREATE;

  if (FirefoxHoldsProfile(archive)) {
    ReportWriteProblem(
        L"Firefox запущен и держит базу закладок открытой.\r\n"
        L"Закройте Firefox и повторите операцию.");
    return E_NOT_SUPPORTED;
  }

  const std::wstring backup = BackupBeforeWrite(archive);
  if (!backup.empty()) AppendLog("  backup '" + WideToUtf8(backup) + "'");

  std::string error;
  sqlite3* db = OpenForWrite(archive, error);
  if (!db) {
    ReportWriteProblem(L"Не удалось открыть базу закладок для записи:\r\n" +
                       Utf8ToWide(error.c_str(), static_cast<int>(error.size())));
    return E_ECREATE;
  }

  std::vector<Entry> entries;
  std::map<std::wstring, sqlite3_int64> folders;
  int rows = 0;
  bool readError = false;
  BuildEntries(db, entries, &rows, error, &readError, &folders);

  std::map<std::wstring, sqlite3_int64> files;
  for (size_t i = 0; i < entries.size(); ++i) {
    std::wstring key = ToLower(entries[i].path);
    while (!key.empty() && (key.back() == L'\\' || key.back() == L'/')) key.pop_back();
    files[key] = entries[i].nodeId;
  }

  const std::wstring sourceDir = SrcPath ? SrcPath : L"";
  const std::vector<std::wstring> subParts = SplitArchivePath(SubPath ? SubPath : L"");

  if (!ExecSql(db, "BEGIN IMMEDIATE", error)) {
    AppendLog("  cannot start a transaction: " + error);
    sqlite3_close(db);
    return E_EWRITE;
  }

  sqlite3_int64 parent = 0;
  if (!subParts.empty()) {
    parent = EnsureFolder(db, folders, subParts, error);
  } else {
    // Packing at the archive root: the entry has to land in a real folder to
    // stay visible, so it goes to the unfiled bookmarks.
    std::map<std::wstring, sqlite3_int64>::const_iterator it =
        folders.find(ToLower(std::wstring(L"Другие закладки")));
    parent = (it != folders.end()) ? it->second : 1;
  }
  if (!parent) {
    AppendLog("  cannot resolve the target folder: " + error);
    ExecSql(db, "ROLLBACK", error);
    sqlite3_close(db);
    return E_ECREATE;
  }

  bool ok = true;
  int added = 0;
  for (const WCHAR* item = AddList; item && *item && ok; item += wcslen(item) + 1) {
    const std::wstring raw(item);
    const std::wstring relative = StripPrefix(raw, sourceDir);
    if (!ReportProgress(relative, 0)) {
      ExecSql(db, "ROLLBACK", error);
      sqlite3_close(db);
      return E_EABORTED;
    }
    const bool trailingSlash =
        !relative.empty() && (relative.back() == L'\\' || relative.back() == L'/');

    std::vector<std::wstring> parts = SplitArchivePath(relative);
    if (parts.empty()) continue;
    const std::wstring itemName = parts.back();
    parts.pop_back();

    std::vector<std::wstring> absolute = subParts;
    absolute.insert(absolute.end(), parts.begin(), parts.end());
    const sqlite3_int64 target =
        absolute.empty() ? parent : EnsureFolder(db, folders, absolute, error);
    if (!target) {
      ok = false;
      break;
    }

    std::wstring diskPath;
    if (IsAbsolutePath(raw)) {
      diskPath = raw;
    } else if (!sourceDir.empty()) {
      diskPath = sourceDir + L"\\" + relative;
    } else {
      diskPath = relative;
    }

    const DWORD attributes = GetFileAttributesW(diskPath.c_str());
    const bool exists = attributes != INVALID_FILE_ATTRIBUTES;
    const bool isDirectory = exists && (attributes & FILE_ATTRIBUTE_DIRECTORY);

    std::wstring fullPath;
    for (size_t i = 0; i < absolute.size(); ++i) {
      fullPath += absolute[i];
      fullPath += L'\\';
    }
    fullPath += itemName;
    std::map<std::wstring, sqlite3_int64>::const_iterator known =
        files.find(ToLower(fullPath));
    if (known != files.end() && exists && !isDirectory && !trailingSlash) {
      std::wstring url;
      if (!ReadUrlFromFile(diskPath, &url)) url = FileUrlFor(diskPath);
      std::wstring title = itemName;
      if (EndsWithNoCase(title, L".url")) title.resize(title.size() - 4);
      if (!url.empty() &&
          UpdateBookmarkUrl(db, known->second, SanitizeName(title), url, error)) {
        ++added;
        continue;
      }
      ok = false;
      break;
    }

    // A name that is not on disk cannot be packed as a file: Total Commander
    // uses such an entry to create a folder.
    if (trailingSlash || !exists || isDirectory) {
      if (isDirectory) {
        ok = PackOne(db, target, diskPath, itemName, error, 0);
      } else {
        // Total Commander creates a folder by packing its name alone.
        const std::wstring name = SanitizeName(itemName);
        const sqlite3_int64 created =
            InsertFolder(db, target, name.empty() ? L"Folder" : name, error);
        if (!created) {
          ok = false;
          break;
        }
        absolute.push_back(itemName);
        std::wstring key;
        for (size_t i = 0; i < absolute.size(); ++i) {
          if (i) key += L'\\';
          key += absolute[i];
        }
        folders[ToLower(key)] = created;
      }
    } else {
      ok = PackOne(db, target, diskPath, itemName, error, 0);
    }
    if (ok) ++added;
  }

  if (!ok || !ExecSql(db, "COMMIT", error)) {
    AppendLog("  pack failed: " + error);
    ExecSql(db, "ROLLBACK", error);
    sqlite3_close(db);
    return E_EWRITE;
  }
  sqlite3_close(db);
  AppendLog("  packed " + std::to_string(added));
  return E_SUCCESS;
}

std::wstring ParentKey(const std::wstring& key) {
  const size_t cut = key.find_last_of(L"\\/");
  return cut == std::wstring::npos ? std::wstring() : key.substr(0, cut);
}

std::wstring LeafKey(const std::wstring& key) {
  const size_t cut = key.find_last_of(L"\\/");
  return cut == std::wstring::npos ? key : key.substr(cut + 1);
}

bool HasWildcard(const std::wstring& text) {
  return text.find(L'*') != std::wstring::npos || text.find(L'?') != std::wstring::npos;
}

bool WildcardMatch(const std::wstring& mask, const std::wstring& name) {
  size_t m = 0;
  size_t n = 0;
  size_t star = std::wstring::npos;
  size_t mark = 0;
  while (n < name.size()) {
    if (m < mask.size() && (mask[m] == L'?' || towlower(mask[m]) == towlower(name[n]))) {
      ++m;
      ++n;
    } else if (m < mask.size() && mask[m] == L'*') {
      star = m++;
      mark = n;
    } else if (star != std::wstring::npos) {
      m = star + 1;
      n = ++mark;
    } else {
      return false;
    }
  }
  while (m < mask.size() && mask[m] == L'*') ++m;
  return m == mask.size();
}

WCX_EXPORT int __stdcall DeleteFilesW(LPWSTR PackedFile, LPWSTR DeleteList) {
  const std::wstring archive = PackedFile ? PackedFile : L"";
  AppendLog("DeleteFilesW pack='" + WideToUtf8(archive) + "'");
  for (const WCHAR* item = DeleteList; item && *item; item += wcslen(item) + 1) {
    AppendLog("  del '" + WideToUtf8(item) + "'");
  }
  if (archive.empty() || !DeleteList) return E_BAD_ARCHIVE;

  if (FirefoxHoldsProfile(archive)) {
    ReportWriteProblem(
        L"Firefox запущен и держит базу закладок открытой.\r\n"
        L"Закройте Firefox и повторите операцию.");
    return E_NOT_SUPPORTED;
  }

  const std::wstring backup = BackupBeforeWrite(archive);
  if (!backup.empty()) AppendLog("  backup '" + WideToUtf8(backup) + "'");

  std::string error;
  sqlite3* db = OpenForWrite(archive, error);
  if (!db) {
    ReportWriteProblem(L"Не удалось открыть базу закладок для записи:\r\n" +
                       Utf8ToWide(error.c_str(), static_cast<int>(error.size())));
    return E_ECREATE;
  }

  std::vector<Entry> entries;
  std::map<std::wstring, sqlite3_int64> folders;
  int rows = 0;
  bool readError = false;
  BuildEntries(db, entries, &rows, error, &readError, &folders);

  std::map<std::wstring, sqlite3_int64> files;
  for (size_t i = 0; i < entries.size(); ++i) {
    std::wstring key = ToLower(entries[i].path);
    while (!key.empty() && (key.back() == L'\\' || key.back() == L'/')) key.pop_back();
    files[key] = entries[i].nodeId;
  }

  if (!ExecSql(db, "BEGIN IMMEDIATE", error)) {
    AppendLog("  cannot start a transaction: " + error);
    sqlite3_close(db);
    return E_EWRITE;
  }

  int deleted = 0;
  int requested = 0;
  for (const WCHAR* item = DeleteList; *item; item += wcslen(item) + 1) {
    if (!ReportProgress(item, 0)) {
      ExecSql(db, "ROLLBACK", error);
      sqlite3_close(db);
      return E_EABORTED;
    }
    std::wstring key = ToLower(item);
    while (!key.empty() && (key.back() == L'\\' || key.back() == L'/')) key.pop_back();
    if (key.empty()) continue;
    ++requested;

    // A folder is removed with the mask "Folder\*.*", which stands for the
    // folder itself plus everything below it.
    const std::wstring parent = ParentKey(key);
    const std::wstring leaf = LeafKey(key);
    std::set<std::wstring> victims;
    if (HasWildcard(leaf)) {
      std::map<std::wstring, sqlite3_int64>::const_iterator it;
      for (it = files.begin(); it != files.end(); ++it) {
        if (ParentKey(it->first) == parent && WildcardMatch(leaf, LeafKey(it->first))) {
          victims.insert(it->first);
        }
      }
      for (it = folders.begin(); it != folders.end(); ++it) {
        if (ParentKey(it->first) == parent && WildcardMatch(leaf, LeafKey(it->first))) {
          victims.insert(it->first);
        }
      }
      if ((leaf == L"*.*" || leaf == L"*") && !parent.empty()) victims.insert(parent);
    } else {
      victims.insert(key);
    }

    for (std::set<std::wstring>::const_iterator victim = victims.begin();
         victim != victims.end(); ++victim) {
      if (victim->empty()) continue;
      sqlite3_int64 id = 0;
      std::map<std::wstring, sqlite3_int64>::const_iterator fit = files.find(*victim);
      if (fit != files.end()) {
        id = fit->second;
      } else {
        std::map<std::wstring, sqlite3_int64>::const_iterator dit = folders.find(*victim);
        if (dit != folders.end()) id = dit->second;
      }
      if (!id) {
        AppendLog("  not in the archive: '" + WideToUtf8(*victim) + "'");
        continue;
      }
      std::string why;
      if (!DeleteNode(db, id, why)) {
        AppendLog("  delete failed '" + WideToUtf8(*victim) + "': " + why);
        continue;
      }
      ++deleted;
      // Everything below a removed folder went with it.
      const std::wstring prefix = *victim + L"\\";
      for (std::map<std::wstring, sqlite3_int64>::iterator it = files.begin(); it != files.end();) {
        if (it->first == *victim || it->first.compare(0, prefix.size(), prefix) == 0) {
          files.erase(it++);
        } else {
          ++it;
        }
      }
      for (std::map<std::wstring, sqlite3_int64>::iterator it = folders.begin();
           it != folders.end();) {
        if (it->first == *victim || it->first.compare(0, prefix.size(), prefix) == 0) {
          folders.erase(it++);
        } else {
          ++it;
        }
      }
    }
  }

  if (!ExecSql(db, "COMMIT", error)) {
    AppendLog("  commit failed: " + error);
    ExecSql(db, "ROLLBACK", error);
    sqlite3_close(db);
    return E_EWRITE;
  }
  sqlite3_close(db);
  AppendLog("  deleted " + std::to_string(deleted) + " of " + std::to_string(requested));
  return (deleted > 0 || requested == 0) ? E_SUCCESS : E_BAD_ARCHIVE;
}

WCX_EXPORT BOOL __stdcall CanYouHandleThisFileW(LPWSTR FileName) {
  if (!FileName || !*FileName) return FALSE;
  const std::wstring path(FileName);
  const bool nameMatches = EndsWithNoCase(path, L"places.sqlite");
  if (!HasSqliteHeader(path)) return FALSE;
  if (nameMatches) return TRUE;
  return HasMozBookmarks(path) ? TRUE : FALSE;
}
