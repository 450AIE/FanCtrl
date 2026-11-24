// config.cpp - 极简配置读写（settings.ini，与 exe 同目录）

#include "app.h"
#include <cstdio>
#include <cwchar>

static void GetIniPath(wchar_t* buf, size_t cch) {
    GetModuleFileNameW(NULL, buf, (DWORD)cch);
    wchar_t* slash = wcsrchr(buf, L'\\');
    if (slash) *(slash + 1) = 0;
    wcscat_s(buf, cch, L"settings.ini");
}

void ConfigLoad() {
    wchar_t path[MAX_PATH];
    GetIniPath(path, MAX_PATH);

    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"rt, ccs=UTF-8") != 0 || !f) return;

    wchar_t line[512];
    while (fgetws(line, 512, f)) {
        wchar_t* p = line;
        while (*p == L' ' || *p == L'\t') p++;

        if (wcsncmp(p, L"points=", 7) == 0) {
            int v[kPointCount] = { 0 };
            int n = swscanf_s(p + 7, L"%d,%d,%d,%d,%d,%d,%d,%d,%d",
                              &v[0], &v[1], &v[2], &v[3], &v[4],
                              &v[5], &v[6], &v[7], &v[8]);
            if (n == kPointCount) {
                for (int i = 0; i < kPointCount; i++) {
                    if (v[i] < 0) v[i] = 0;
                    if (v[i] > kRpmMax) v[i] = kRpmMax;
                    g_app.points[i] = v[i];
                }
            }
        } else if (wcsncmp(p, L"safety=", 7) == 0) {
            int v = 0;
            if (swscanf_s(p + 7, L"%d", &v) == 1 && v >= 60 && v <= 110) g_app.safetyTemp = v;
        } else if (wcsncmp(p, L"minDelta=", 9) == 0) {
            int v = 0;
            if (swscanf_s(p + 9, L"%d", &v) == 1 && v >= 0 && v <= 500) g_app.minWriteDelta = v;
        } else if (wcsncmp(p, L"topmost=", 8) == 0) {
            int v = 0;
            if (swscanf_s(p + 8, L"%d", &v) == 1) g_app.topmost = (v != 0);
        }
    }
    fclose(f);
}

void ConfigSave() {
    wchar_t path[MAX_PATH];
    GetIniPath(path, MAX_PATH);

    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"wt, ccs=UTF-8") != 0 || !f) return;

    fwprintf(f, L"points=%d,%d,%d,%d,%d,%d,%d,%d,%d\n",
             g_app.points[0], g_app.points[1], g_app.points[2],
             g_app.points[3], g_app.points[4], g_app.points[5],
             g_app.points[6], g_app.points[7], g_app.points[8]);
    fwprintf(f, L"safety=%d\n", g_app.safetyTemp);
    fwprintf(f, L"minDelta=%d\n", g_app.minWriteDelta);
    fwprintf(f, L"topmost=%d\n", g_app.topmost ? 1 : 0);
    fclose(f);
}
