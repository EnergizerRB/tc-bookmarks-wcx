#define UNICODE
#define _UNICODE
#include <windows.h>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <set>

// Константы WCX API Total Commander
#define E_END_ARCHIVE     10
#define E_NO_MEMORY       11
#define E_BAD_DATA        12
#define E_BAD_ARCHIVE     13
#define E_UNKNOWN_FORMAT  14
#define E_EOPEN           15
#define E_ECREATE         16
#define E_ECLOSE          17
#define E_EREAD           18
#define E_EWRITE          19

#define PK_OM_LIST        0
#define PK_OM_EXTRACT     1

#define PK_SKIP           0
#define PK_TEST           1
#define PK_EXTRACT        2

#define PK_CAPS_NEW       1
#define PK_CAPS_MODIFY    2
#define PK_CAPS_MULTIPLE  4
#define PK_CAPS_DELETE    8
#define PK_CAPS_OPTIONS   16
#define PK_CAPS_MEMPACK   32
#define PK_CAPS_BY_CONTENT 64
#define PK_CAPS_SEARCHTEXT 128
#define PK_CAPS_HIDE      256

#pragma pack(push, 1)
struct tOpenArchiveDataW {
    WCHAR* ArcName;
    int OpenMode;
    int OpenResult;
    WCHAR* CmtBuf;
    int CmtBufSize;
    int CmtSize;
    int CmtState;
};

struct tHeaderDataExW {
    WCHAR ArcName[1024];
    WCHAR FileName[1024];
    int Flags;
    unsigned int PackSize;
    unsigned int PackSizeHigh;
    unsigned int UnpSize;
    unsigned int UnpSizeHigh;
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
#pragma pack(pop)

struct BookmarkItem {
    std::wstring relPath;
    std::string urlContent;
    bool isDir;
};

struct ArchiveHandle {
    std::vector<BookmarkItem> items;
    size_t currentIndex = 0;
};

// Конвертация UTF-8 в UTF-16
std::wstring Utf8ToWide(const std::string& str) {
    if (str.empty()) return L"";
    int size = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), nullptr, 0);
    std::wstring res(size, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), &res[0], size);
    return res;
}

// Декодирование базовых HTML-сущностей
std::wstring DecodeHtmlEntities(const std::wstring& text) {
    std::wstring res;
    res.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == L'&') {
            size_t semi = text.find(L';', i);
            if (semi != std::wstring::npos && semi - i <= 10) {
                std::wstring ent = text.substr(i + 1, semi - i - 1);
                if (ent == L"amp") { res += L'&'; i = semi; continue; }
                if (ent == L"lt") { res += L'<'; i = semi; continue; }
                if (ent == L"gt") { res += L'>'; i = semi; continue; }
                if (ent == L"quot") { res += L'"'; i = semi; continue; }
                if (ent == L"apos" || ent == L"#39") { res += L'\''; i = semi; continue; }
                if (!ent.empty() && ent[0] == L'#') {
                    try {
                        wchar_t ch = (ent[1] == L'x' || ent[1] == L'X')
                            ? (wchar_t)std::stoi(ent.substr(2), nullptr, 16)
                            : (wchar_t)std::stoi(ent.substr(1), nullptr, 10);
                        res += ch;
                        i = semi;
                        continue;
                    } catch (...) {}
                }
            }
        }
        res += text[i];
    }
    return res;
}

// Очистка имени от запрещённых в Windows символов
std::wstring SanitizeName(const std::wstring& raw, const std::wstring& fallback) {
    std::wstring decoded = DecodeHtmlEntities(raw);
    std::wstring clean;
    for (wchar_t c : decoded) {
        if (c < 32 || wcschr(L"<>:\"/\\|?*", c)) {
            clean += L'_';
        } else if (c == L'\r' || c == L'\n' || c == L'\t') {
            clean += L' ';
        } else {
            clean += c;
        }
    }
    // Убираем лишние пробелы и точки в конце
    size_t start = clean.find_first_not_of(L' ');
    if (start == std::wstring::npos) return fallback;
    size_t end = clean.find_last_not_of(L" .");
    clean = clean.substr(start, end - start + 1);
    if (clean.empty()) return fallback;
    if (clean.size() > 110) clean = clean.substr(0, 110);
    return clean;
}

std::wstring ToLower(const std::wstring& s) {
    std::wstring res = s;
    for (auto& c : res) c = towlower(c);
    return res;
}

// Уникальное имя в рамках одной папки
std::wstring GetUniqueName(std::set<std::wstring>& used, const std::wstring& base, const std::wstring& ext) {
    std::wstring candidate = base + ext;
    int counter = 2;
    while (used.count(ToLower(candidate))) {
        candidate = base + L" (" + std::to_wstring(counter++) + L")" + ext;
    }
    used.insert(ToLower(candidate));
    return candidate;
}

// Парсер файла закладок Firefox (Netscape Bookmark Format)
void ParseBookmarks(const std::string& utf8Html, std::vector<BookmarkItem>& items) {
    std::wstring html = Utf8ToWide(utf8Html);
    std::wstring lowerHtml = ToLower(html);

    std::vector<std::wstring> folderStack;
    std::vector<std::set<std::wstring>> usedStack(1);
    std::wstring pendingFolder;

    size_t pos = 0;
    while (pos < html.size()) {
        size_t lt = html.find(L'<', pos);
        if (lt == std::wstring::npos) break;
        size_t gt = html.find(L'>', lt);
        if (gt == std::wstring::npos) break;

        std::wstring tagLower = lowerHtml.substr(lt + 1, gt - lt - 1);

        if (tagLower.rfind(L"dl", 0) == 0 && (tagLower.size() == 2 || iswspace(tagLower[2]))) {
            if (!pendingFolder.empty()) {
                folderStack.push_back(pendingFolder);
                usedStack.emplace_back();
                pendingFolder.clear();
            }
        } else if (tagLower.rfind(L"/dl", 0) == 0) {
            if (!folderStack.empty()) {
                folderStack.pop_back();
                usedStack.pop_back();
            }
            pendingFolder.clear();
        } else if (tagLower.rfind(L"h3", 0) == 0 && (tagLower.size() == 2 || iswspace(tagLower[2]))) {
            size_t closeH3 = lowerHtml.find(L"</h3", gt);
            if (closeH3 != std::wstring::npos) {
                std::wstring title = SanitizeName(html.substr(gt + 1, closeH3 - gt - 1), L"Folder");
                std::wstring uniqueFolder = GetUniqueName(usedStack.back(), title, L"");
                pendingFolder = uniqueFolder;

                std::wstring fullPath;
                for (const auto& f : folderStack) fullPath += f + L"\\";
                fullPath += uniqueFolder;

                items.push_back({ fullPath, "", true });
                pos = closeH3;
                continue;
            }
        } else if (tagLower.rfind(L"a", 0) == 0 && (tagLower.size() == 1 || iswspace(tagLower[1]))) {
            size_t closeA = lowerHtml.find(L"</a", gt);
            if (closeA != std::wstring::npos) {
                std::wstring tagOrig = html.substr(lt + 1, gt - lt - 1);
                std::wstring tagOrigLower = ToLower(tagOrig);
                size_t hrefPos = tagOrigLower.find(L"href=");
                std::string urlUtf8;

                if (hrefPos != std::wstring::npos) {
                    size_t valStart = hrefPos + 5;
                    wchar_t quote = 0;
                    if (valStart < tagOrig.size() && (tagOrig[valStart] == L'"' || tagOrig[valStart] == L'\'')) {
                        quote = tagOrig[valStart++];
                    }
                    size_t valEnd = quote ? tagOrig.find(quote, valStart) : tagOrig.find_first_of(L" \t\r\n", valStart);
                    if (valEnd == std::wstring::npos) valEnd = tagOrig.size();
                    std::wstring urlW = DecodeHtmlEntities(tagOrig.substr(valStart, valEnd - valStart));

                    // Пропускаем внутренние служебные ссылки Firefox
                    if (ToLower(urlW).rfind(L"place:", 0) != 0 && !urlW.empty()) {
                        int len = WideCharToMultiByte(CP_UTF8, 0, urlW.c_str(), (int)urlW.size(), nullptr, 0, nullptr, nullptr);
                        urlUtf8.resize(len);
                        WideCharToMultiByte(CP_UTF8, 0, urlW.c_str(), (int)urlW.size(), &urlUtf8[0], len, nullptr, nullptr);
                    }
                }

                if (!urlUtf8.empty()) {
                    std::wstring title = SanitizeName(html.substr(gt + 1, closeA - gt - 1), L"Bookmark");
                    std::wstring fileName = GetUniqueName(usedStack.back(), title, L".url");

                    std::wstring fullPath;
                    for (const auto& f : folderStack) fullPath += f + L"\\";
                    fullPath += fileName;

                    std::string content = "[InternetShortcut]\r\nURL=" + urlUtf8 + "\r\n";
                    items.push_back({ fullPath, content, false });
                }
                pos = closeA;
                continue;
            }
        }
        pos = gt + 1;
    }
}

extern "C" {

__declspec(dllexport) HANDLE __stdcall OpenArchiveW(tOpenArchiveDataW* ArchiveData) {
    if (!ArchiveData || !ArchiveData->ArcName) return nullptr;

    std::ifstream file(ArchiveData->ArcName, std::ios::binary);
    if (!file.is_open()) {
        ArchiveData->OpenResult = E_EOPEN;
        return nullptr;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();

    // Проверяем, что это файл закладок Netscape/Firefox
    if (content.find("NETSCAPE-Bookmark-file") == std::string::npos &&
        content.find("<DL>") == std::string::npos &&
        content.find("<dl>") == std::string::npos) {
        ArchiveData->OpenResult = E_UNKNOWN_FORMAT;
        return nullptr;
    }

    auto* handle = new (std::nothrow) ArchiveHandle();
    if (!handle) {
        ArchiveData->OpenResult = E_NO_MEMORY;
        return nullptr;
    }

    ParseBookmarks(content, handle->items);
    return (HANDLE)handle;
}

__declspec(dllexport) int __stdcall ReadHeaderExW(HANDLE hArcData, tHeaderDataExW* HeaderData) {
auto* handle = (ArchiveHandle*)hArcData;
if (!handle || !HeaderData) return E_BAD_ARCHIVE;
if (handle->currentIndex >= handle->items.size()) {
return E_END_ARCHIVE;
}
memset(HeaderData, 0, sizeof(tHeaderDataExW));
const auto& item = handle->items[handle->currentIndex];
wcsncpy(HeaderData->FileName, item.relPath.c_str(), 1023);
if (item.isDir) {
HeaderData->FileAttr = FILE_ATTRIBUTE_DIRECTORY;
} else {
HeaderData->FileAttr = FILE_ATTRIBUTE_ARCHIVE;
HeaderData->PackSize = (unsigned int)item.urlContent.size();
HeaderData->UnpSize = (unsigned int)item.urlContent.size();
}
return 0;
}
__declspec(dllexport) int __stdcall ProcessFileW(HANDLE hArcData, int Operation, WCHAR* DestPath, WCHAR* DestName) {
auto* handle = (ArchiveHandle*)hArcData;
if (!handle || handle->currentIndex >= handle->items.size()) return E_BAD_ARCHIVE;
const auto& item = handle->items[handle->currentIndex];
handle->currentIndex++;
if (Operation == PK_EXTRACT) {
std::wstring target;
if (DestPath && DestPath[0]) {
target = DestPath;
if (target.back() != L'\') target += L'\';
}
if (DestName) target += DestName;
if (item.isDir) {
CreateDirectoryW(target.c_str(), nullptr);
} else {
std::ofstream out(target, std::ios::binary);
if (!out.is_open()) return E_ECREATE;
out.write(item.urlContent.data(), item.urlContent.size());
}
}
return 0;
}
__declspec(dllexport) int __stdcall CloseArchive(HANDLE hArcData) {
auto* handle = (ArchiveHandle*)hArcData;
delete handle;
return 0;
}
__declspec(dllexport) int __stdcall GetPackerCaps() {
return PK_CAPS_BY_CONTENT | PK_CAPS_SEARCHTEXT;
}
__declspec(dllexport) void __stdcall SetChangeVolProcW(HANDLE, void*) {}
__declspec(dllexport) void __stdcall SetProcessDataProcW(HANDLE, void*) {}
} // extern "C"
