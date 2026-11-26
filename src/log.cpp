// log.cpp - 极简文本日志实现
#include "log.h"
#include <cstdarg>
#include <cstdio>
#include <cwchar>

static wchar_t s_path[MAX_PATH] = {};

static void TrimIfHuge(const wchar_t* path) {
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &fad)) return;
    LARGE_INTEGER sz = {};
    sz.LowPart  = fad.nFileSizeLow;
    sz.HighPart = (LONG)fad.nFileSizeHigh;
    if (sz.QuadPart > 512 * 1024) DeleteFileW(path);   // 超过 512KB 直接重开
}

void LogInit() {
    wchar_t exe[MAX_PATH] = {};
    if (!GetModuleFileNameW(NULL, exe, MAX_PATH)) return;
    wchar_t* slash = wcsrchr(exe, L'\\');
    if (!slash) return;
    *(slash + 1) = 0;
    swprintf_s(s_path, MAX_PATH, L"%sFanCtrl.log", exe);
    TrimIfHuge(s_path);
}

void Logf(const wchar_t* fmt, ...) {
    if (!s_path[0]) LogInit();
    if (!s_path[0]) return;

    TrimIfHuge(s_path);

    FILE* f = nullptr;
    if (_wfopen_s(&f, s_path, L"a, ccs=UTF-8") != 0 || !f) return;

    SYSTEMTIME st;
    GetLocalTime(&st);
    fwprintf(f, L"%04u-%02u-%02u %02u:%02u:%02u.%03u  ",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

    va_list ap;
    va_start(ap, fmt);
    vfwprintf(f, fmt, ap);
    va_end(ap);
    fwprintf(f, L"\n");
    fclose(f);
}

bool IsProcessElevated() {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
    TOKEN_ELEVATION te = {};
    DWORD len = 0;
    bool elev = false;
    if (GetTokenInformation(tok, TokenElevation, &te, sizeof(te), &len))
        elev = te.TokenIsElevated != 0;
    CloseHandle(tok);
    return elev;
}
