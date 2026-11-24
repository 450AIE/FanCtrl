// 热替换助手：FanCtrl 以管理员权限常驻，普通会话既停不掉它也覆盖不了它的 exe。
// 本程序需以管理员运行（Start-Process -Verb RunAs），负责：
//   1) 发 WM_COMMAND(kCmdExit) 让旧实例自行退出 —— 这条退出路径会交还 EC，不会残留手动转速
//   2) 用同目录的 FanCtrl_new.exe 替换 FanCtrl.exe
//
// 推荐用法（替换完成后由系统计划任务拉起，进程脱离会话 job，不会随命令结束被回收）：
//   deploy.exe /replace
//   Start-ScheduledTask -TaskName FanCtrl
//
// 不带参数时会自己尝试 CreateProcess 启动，便于临时验证；但该子进程仍在调用方 job 内，
// 命令结束后可能被回收，不适合长期驻留。
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cstdarg>

static const UINT kCmdExit = 40006;   // 与 tray.cpp 的 kCmdExit 保持一致

static FILE* g_log = nullptr;
static void Log(const wchar_t* fmt, ...) {
    if (!g_log) return;
    va_list ap;
    va_start(ap, fmt);
    vfwprintf(g_log, fmt, ap);
    va_end(ap);
    fwprintf(g_log, L"\n");
    fflush(g_log);
}

struct WinEnum { DWORD pid; HWND hwnd; };

static BOOL CALLBACK EnumProc(HWND h, LPARAM lp) {
    WinEnum* e = (WinEnum*)lp;
    DWORD p = 0;
    GetWindowThreadProcessId(h, &p);
    if (p != e->pid) return TRUE;
    wchar_t cls[128] = {};
    GetClassNameW(h, cls, 127);
    if (wcsstr(cls, L"Tray") != nullptr || wcsstr(cls, L"tray") != nullptr) return TRUE;
    if (!GetWindow(h, GW_OWNER) && IsWindowVisible(h)) { e->hwnd = h; return FALSE; }
    return TRUE;
}

static DWORD FindPid(const wchar_t* exeName) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe = { sizeof(pe) };
    DWORD found = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, exeName) == 0) { found = pe.th32ProcessID; break; }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

static bool Launch(const wchar_t* exe) {
    BOOL inJob = FALSE;
    IsProcessInJob(GetCurrentProcess(), nullptr, &inJob);
    Log(L"helper in job = %ld", inJob);

    wchar_t cmd[MAX_PATH + 4];
    swprintf_s(cmd, L"\"%s\"", exe);
    // job 允许时优先 breakaway；不允许则退回常规创建（进程会随会话结束被回收）
    for (int attempt = 0; attempt < 2; attempt++) {
        STARTUPINFOW si = { sizeof(si) };
        PROCESS_INFORMATION pi = {};
        DWORD flags = CREATE_NEW_PROCESS_GROUP | CREATE_UNICODE_ENVIRONMENT;
        if (attempt == 0) flags |= CREATE_BREAKAWAY_FROM_JOB;
        if (CreateProcessW(exe, cmd, nullptr, nullptr, FALSE, flags,
                           nullptr, nullptr, &si, &pi)) {
            Log(L"CreateProcess ok pid=%lu (breakaway=%d)", pi.dwProcessId, attempt == 0);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            return true;
        }
        Log(L"CreateProcess attempt %d failed err=%lu", attempt, GetLastError());
    }
    return false;
}

int wmain(int argc, wchar_t** argv) {
    wchar_t dir[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, dir, MAX_PATH);
    if (wchar_t* slash = wcsrchr(dir, L'\\')) *slash = 0;

    wchar_t logPath[MAX_PATH];
    swprintf_s(logPath, L"%s\\deploy.log", dir);
    _wfopen_s(&g_log, logPath, L"w, ccs=UTF-8");

    wchar_t exe[MAX_PATH], newExe[MAX_PATH], oldExe[MAX_PATH];
    swprintf_s(exe,    L"%s\\FanCtrl.exe", dir);
    swprintf_s(newExe, L"%s\\FanCtrl_new.exe", dir);
    swprintf_s(oldExe, L"%s\\FanCtrl.exe.old", dir);

    bool restartOnly = (argc > 1 && _wcsicmp(argv[1], L"/restart") == 0);
    bool replaceOnly = (argc > 1 && _wcsicmp(argv[1], L"/replace") == 0);

    // 1) 让旧实例走正常退出路径，交还 EC
    DWORD pid = FindPid(L"FanCtrl.exe");
    if (pid) {
        Log(L"old instance pid=%lu", pid);
        WinEnum e = { pid, nullptr };
        EnumWindows(EnumProc, (LPARAM)&e);
        if (e.hwnd) {
            Log(L"posting WM_COMMAND(%u) to hwnd=%p", kCmdExit, (void*)e.hwnd);
            PostMessageW(e.hwnd, WM_COMMAND, (WPARAM)kCmdExit, 0);
        } else {
            Log(L"no visible main window found");
        }
        for (int i = 0; i < 40 && FindPid(L"FanCtrl.exe"); i++) Sleep(200);
        pid = FindPid(L"FanCtrl.exe");
        if (pid) {
            Log(L"still alive, terminating pid=%lu", pid);
            HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
            if (h) { TerminateProcess(h, 0); CloseHandle(h); }
            Sleep(1500);
        } else {
            Log(L"exited gracefully");
        }
    } else {
        Log(L"no old instance");
    }

    // 2) 替换 exe
    if (!restartOnly) {
        if (GetFileAttributesW(newExe) == INVALID_FILE_ATTRIBUTES) {
            Log(L"missing %s", newExe);
            if (g_log) fclose(g_log);
            return 2;
        }
        bool replaced = false;
        for (int i = 0; i < 10; i++) {
            DeleteFileW(oldExe);
            if (MoveFileExW(exe, oldExe, MOVEFILE_REPLACE_EXISTING) ||
                GetFileAttributesW(exe) == INVALID_FILE_ATTRIBUTES) {
                if (CopyFileW(newExe, exe, FALSE)) { replaced = true; break; }
            }
            Sleep(400);
        }
        if (!replaced) {
            Log(L"replace failed, err=%lu", GetLastError());
            if (g_log) fclose(g_log);
            return 3;
        }
        Log(L"exe replaced");
        DeleteFileW(oldExe);
        DeleteFileW(newExe);
    }

    if (replaceOnly) {
        Log(L"replace done, skip launch (use Start-ScheduledTask)");
        if (g_log) fclose(g_log);
        return 0;
    }

    bool up = Launch(exe);
    if (!up) { Log(L"start failed"); if (g_log) fclose(g_log); return 4; }
    Sleep(3000);
    Log(L"new instance pid=%lu", FindPid(L"FanCtrl.exe"));
    if (g_log) fclose(g_log);
    return 0;
}
