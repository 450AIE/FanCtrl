// tray.cpp - 托盘图标（运行时绘制，无外部资源）+ 开机自启（计划任务）
//
// 图标：64x64 超采样逐像素生成，抗锯齿；蓝色圆底 + 白色三叶风扇。
// 开机自启：生成任务计划 XML，用 schtasks /Create /XML 注册，
//           以管理员组身份 + HighestAvailable 运行，避免每次开机弹 UAC。

#include "app.h"
#include <shellapi.h>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <vector>
#include <taskschd.h>
#include <sddl.h>
#include "log.h"

#pragma comment(lib, "taskschd.lib")

#pragma comment(lib, "shell32.lib")

// ---------------- 托盘状态 ----------------
static NOTIFYICONDATAW g_nid = {};
static bool  g_added        = false;
static HICON g_icoBig       = nullptr;
static HICON g_icoSmall     = nullptr;
static UINT  g_msgTaskbar   = 0;

static const UINT kTrayId       = 1;static const UINT kCmdShow      = 40001;
static const UINT kCmdCurve     = 40002;
static const UINT kCmdAuto      = 40003;
static const UINT kCmdFull      = 40004;
static const UINT kCmdAutoStart = 40005;
static const UINT kCmdExit      = 40006;
static const wchar_t* kTaskName = L"FanCtrl";

#ifdef FANCTRL_DEBUG
#include <cstdarg>
static void TrayDbg(const wchar_t* fmt, ...) {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);
    wchar_t* slash = wcsrchr(path, L'\\');
    if (slash) *(slash + 1) = 0;
    wcscat_s(path, MAX_PATH, L"tray_debug.log");
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"at, ccs=UTF-8") != 0 || !f) return;
    va_list ap;
    va_start(ap, fmt);
    vfwprintf(f, fmt, ap);
    va_end(ap);
    fwprintf(f, L"\n");
    fclose(f);
}
#else
#define TrayDbg(...) ((void)0)
#endif

// ---------------- 图标绘制 ----------------
static bool InCircle(double x, double y, double cx, double cy, double r) {
    double dx = x - cx, dy = y - cy;
    return dx * dx + dy * dy <= r * r;
}

static bool InEllipseRot(double x, double y, double cx, double cy,
                         double a, double b, double th) {
    double dx = x - cx, dy = y - cy;
    double ct = cos(th), st = sin(th);
    double u =  dx * ct + dy * st;
    double v = -dx * st + dy * ct;
    return (u * u) / (a * a) + (v * v) / (b * b) <= 1.0;
}

#ifdef FANCTRL_DEBUG
static std::vector<unsigned char> g_lastPx;
static int g_lastSize = 0;

static void WriteBmp32(const wchar_t* file, const unsigned char* px, int w, int h) {
    BITMAPFILEHEADER fh = {};
    BITMAPINFOHEADER ih = {};
    ih.biSize        = sizeof(ih);
    ih.biWidth       = w;
    ih.biHeight      = h;
    ih.biPlanes      = 1;
    ih.biBitCount    = 32;
    ih.biCompression = BI_RGB;
    ih.biSizeImage   = (DWORD)(w * h * 4);
    fh.bfType    = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize    = fh.bfOffBits + ih.biSizeImage;
    FILE* f = nullptr;
    if (_wfopen_s(&f, file, L"wb") != 0 || !f) return;
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&ih, sizeof(ih), 1, f);
    for (int y = h - 1; y >= 0; y--) fwrite(px + (size_t)y * w * 4, 4, w, f);
    fclose(f);
}

static void DumpRender(HICON ic, int size, const wchar_t* file) {
    const int sz = 32;
    BITMAPV5HEADER bi = {};
    bi.bV5Size        = sizeof(bi);
    bi.bV5Width       = sz;
    bi.bV5Height      = -sz;
    bi.bV5Planes      = 1;
    bi.bV5BitCount    = 32;
    bi.bV5Compression = BI_RGB;
    void* bits = nullptr;
    HDC screen = GetDC(NULL);
    HBITMAP bmp = CreateDIBSection(screen, (BITMAPINFO*)&bi, DIB_RGB_COLORS, &bits, NULL, 0);
    HDC dc = CreateCompatibleDC(screen);
    ReleaseDC(NULL, screen);
    HGDIOBJ old = SelectObject(dc, bmp);
    RECT r = { 0, 0, sz, sz };
    HBRUSH wb = CreateSolidBrush(RGB(255, 255, 255));
    FillRect(dc, &r, wb);
    DeleteObject(wb);
    DrawIconEx(dc, 0, 0, ic, sz, sz, 0, NULL, DI_NORMAL);
    WriteBmp32(file, (unsigned char*)bits, sz, sz);
    SelectObject(dc, old);
    DeleteDC(dc);
    DeleteObject(bmp);
}
#endif

static HICON MakeIcon(int size) {
    const int SS = 4;                  // 每像素 4x4 采样
    const double kBgR = 24.0, kBgG = 95.0, kBgB = 165.0;   // 主题蓝
    const double kPi = 3.14159265358979323846;

    BITMAPV5HEADER bi = {};
    bi.bV5Size        = sizeof(bi);
    bi.bV5Width       = size;
    bi.bV5Height      = -size;         // 自上而下
    bi.bV5Planes      = 1;
    bi.bV5BitCount    = 32;
    bi.bV5Compression = BI_RGB;

    void* bits = nullptr;
    HDC screen = GetDC(NULL);
    HBITMAP color = CreateDIBSection(screen, (BITMAPINFO*)&bi, DIB_RGB_COLORS, &bits, NULL, 0);
    ReleaseDC(NULL, screen);
    if (!color || !bits) return nullptr;

    unsigned char* dst = (unsigned char*)bits;
    const int N = SS * SS;
#ifdef FANCTRL_DEBUG
    g_lastPx.assign((size_t)size * size * 4, 0);
    g_lastSize = size;
#endif

    for (int py = 0; py < size; py++) {
        for (int px = 0; px < size; px++) {
            int bg = 0, fg = 0;
            for (int sy = 0; sy < SS; sy++) {
                for (int sx = 0; sx < SS; sx++) {
                    double u = (px + (sx + 0.5) / SS) / size;
                    double v = (py + (sy + 0.5) / SS) / size;
                    if (!InCircle(u, v, 0.5, 0.5, 0.47)) continue;
                    bg++;
                    // 小尺寸用更粗的叶片，否则缩到 16px 会糊成一团
                    const double hubR  = (size <= 16) ? 0.0    : 0.050;
                    const double bladeR = (size <= 16) ? 0.215 : 0.228;
                    const double bladeA = (size <= 16) ? 0.095 : 0.072;
                    const double bladeB = (size <= 16) ? 0.150 : 0.142;

                    bool blade = hubR > 0 ? InCircle(u, v, 0.5, 0.5, hubR) : false;
                    for (int k = 0; k < 3 && !blade; k++) {
                        double th = -kPi / 2.0 + k * (2.0 * kPi / 3.0);
                        double cx = 0.5 + bladeR * cos(th);
                        double cy = 0.5 + bladeR * sin(th);
                        if (InEllipseRot(u, v, cx, cy, bladeA, bladeB, th)) blade = true;
                    }
                    if (blade) fg++;
                }
            }
            unsigned char* p = dst + (py * size + px) * 4;
            if (bg == 0) {
                p[0] = p[1] = p[2] = p[3] = 0;
            } else {
                // bg / fg 是采样计数（不是比例），按计数直接加权平均
                double a  = (double)bg / N;
                double r = ((double)fg * 255.0 + (double)(bg - fg) * kBgR) / (double)bg;
                double g = ((double)fg * 255.0 + (double)(bg - fg) * kBgG) / (double)bg;
                double b = ((double)fg * 255.0 + (double)(bg - fg) * kBgB) / (double)bg;
                p[0] = (unsigned char)(b + 0.5);
                p[1] = (unsigned char)(g + 0.5);
                p[2] = (unsigned char)(r + 0.5);
                p[3] = (unsigned char)(a * 255.0 + 0.5);
            }
        }
    }

    // 1bpp 掩码必须显式置 0，否则未初始化的位可能被当成全透明
    std::vector<BYTE> maskBits((size_t)size * 4, 0);
#ifdef FANCTRL_DEBUG
    memcpy(g_lastPx.data(), dst, (size_t)size * size * 4);
#endif

    HBITMAP mask = CreateBitmap(size, size, 1, 1, maskBits.data());
    ICONINFO ii = {};
    ii.fIcon    = TRUE;
    ii.hbmColor = color;
    ii.hbmMask  = mask;
    HICON icon = CreateIconIndirect(&ii);

    DeleteObject(color);
    if (mask) DeleteObject(mask);
    return icon;
}

// ---------------- 托盘 ----------------
static void FillNid(HWND hwnd) {
    memset(&g_nid, 0, sizeof(g_nid));
    g_nid.cbSize          = sizeof(g_nid);
    g_nid.hWnd            = hwnd;
    g_nid.uID             = kTrayId;
    g_nid.uFlags          = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_FANCTRL_TRAY;
    g_nid.hIcon           = g_icoBig;
    wcscpy_s(g_nid.szTip, L"小新风扇控制");
}

bool TrayInit(HWND hwnd) {
    g_icoBig   = MakeIcon(32);
    g_icoSmall = MakeIcon(16);
    g_msgTaskbar = RegisterWindowMessageW(L"TaskbarCreated");
    FillNid(hwnd);
    g_added = Shell_NotifyIconW(NIM_ADD, &g_nid) != FALSE;
#ifdef FANCTRL_DEBUG
    if (g_lastSize > 0) WriteBmp32(L"icon_raw.bmp", g_lastPx.data(), g_lastSize, g_lastSize);
    if (g_icoBig)   DumpRender(g_icoBig,   32, L"icon_render.bmp");
    if (g_icoSmall) DumpRender(g_icoSmall, 32, L"icon_render16.bmp");
#endif
#ifdef FANCTRL_DEBUG
    {
        NOTIFYICONIDENTIFIER nid = {};
        nid.cbSize = sizeof(nid);
        nid.hWnd   = hwnd;
        nid.uID    = kTrayId;
        RECT rc = {};
        HRESULT hr = Shell_NotifyIconGetRect(&nid, &rc);
        TrayDbg(L"[tray] rect hr=0x%08X  %ld,%ld  %ldx%ld",
                (unsigned)hr, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top);
    }
#endif
    TrayDbg(L"[tray] big=%p small=%p added=%d err=%lu",
            (void*)g_icoBig, (void*)g_icoSmall, (int)g_added, GetLastError());
    return g_added;
}

HICON TrayAppIcon(bool useSmall) {
    return useSmall ? g_icoSmall : g_icoBig;
}

void TrayRemove() {
    if (g_added) {
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        g_added = false;
    }
    if (g_icoBig)   { DestroyIcon(g_icoBig);   g_icoBig = nullptr; }
    if (g_icoSmall) { DestroyIcon(g_icoSmall); g_icoSmall = nullptr; }
}

void TrayBalloon(const wchar_t* title, const wchar_t* text) {
    if (!g_added) return;
    g_nid.uFlags |= NIF_INFO;
    wcsncpy_s(g_nid.szInfoTitle, title, _TRUNCATE);
    wcsncpy_s(g_nid.szInfo, text, _TRUNCATE);
    g_nid.dwInfoFlags = NIIF_INFO;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    g_nid.uFlags &= ~NIF_INFO;
}

void TrayUpdateTip() {
    if (!g_added) return;
    wchar_t tip[128];
    if (g_app.temp >= 0 && g_app.fan1 >= 0)
        swprintf_s(tip, L"小新风扇控制\n%d ℃ · %d rpm · %s", g_app.temp, g_app.fan1, g_app.status);
    else
        swprintf_s(tip, L"小新风扇控制\n%s", g_app.status);
    wcsncpy_s(g_nid.szTip, tip, _TRUNCATE);
    g_nid.uFlags |= NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

// ---------------- 右键菜单 ----------------
static void ShowMenu(HWND hwnd) {
    HMENU m = CreatePopupMenu();
    if (!m) return;

    AppendMenuW(m, MF_STRING, kCmdShow, L"显示主窗口");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING | (g_app.mode == MODE_CURVE ? MF_CHECKED : 0), kCmdCurve, L"曲线控制");
    AppendMenuW(m, MF_STRING | (g_app.mode == MODE_AUTO  ? MF_CHECKED : 0), kCmdAuto,  L"交还 EC");
    AppendMenuW(m, MF_STRING | (g_app.mode == MODE_FULL  ? MF_CHECKED : 0), kCmdFull,  L"全速");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING | (g_app.startWithWindows ? MF_CHECKED : 0), kCmdAutoStart, L"开机自启");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, kCmdExit, L"退出");

    SetMenuDefaultItem(m, kCmdShow, FALSE);

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);          // 必需，否则菜单点外面不消失
    TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN,
                   pt.x, pt.y, 0, hwnd, NULL);
    PostMessage(hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);
}

// ---------------- 开机自启（任务计划 COM 接口） ----------------
// 用 ITaskService 在进程内注册计划任务：
//   触发1 = 该用户登录时（延迟 20 秒）——主通道
//   触发2 = 会话解锁时              ——兜底：主通道没跑起来、或进程中途没了，解锁时补一次
//   身份 = 当前用户 SID  运行级别 = 最高  错过就补跑 + 失败重试 3 次
// 好处：不开子进程、不弹控制台、每次开机不弹 UAC。
//
// 定义里带版本标记（kTaskVerTag）：程序启动时若发现任务还是旧版本定义，
// 会自动重写一遍，免得升级程序后还留着老的触发器配置。

static const wchar_t* kTaskDesc   = L"FanCtrl 开机自启 v2：登录触发 + 解锁兜底，管理员权限运行（无 UAC 提示）";
static const wchar_t* kTaskVerTag = L"FanCtrl 开机自启 v2";

// 当前用户的 SID 字符串：比用户名可靠，不受域/本地账户重名影响
static bool CurrentUserSid(wchar_t* out, int cch) {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
    BYTE buf[256] = {};
    DWORD len = 0;
    bool ok = false;
    if (GetTokenInformation(tok, TokenUser, buf, sizeof(buf), &len)) {
        TOKEN_USER* tu = (TOKEN_USER*)buf;
        wchar_t* s = nullptr;
        if (ConvertSidToStringSidW(tu->User.Sid, &s)) {
            wcsncpy_s(out, (size_t)cch, s, _TRUNCATE);
            LocalFree(s);
            ok = true;
        }
    }
    CloseHandle(tok);
    return ok;
}

static ITaskService* OpenTaskService() {
    ITaskService* svc = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_TaskScheduler, NULL, CLSCTX_INPROC_SERVER,
                                  IID_ITaskService, (void**)&svc);
    if (FAILED(hr) || !svc) return nullptr;
    VARIANT e;
    VariantInit(&e);
    hr = svc->Connect(e, e, e, e);
    VariantClear(&e);
    if (FAILED(hr)) { svc->Release(); return nullptr; }
    return svc;
}

static ITaskFolder* GetRootFolder(ITaskService* svc) {
    ITaskFolder* root = nullptr;
    BSTR path = SysAllocString(L"\\");
    if (FAILED(svc->GetFolder(path, &root))) root = nullptr;
    SysFreeString(path);
    return root;
}

bool AutoStartIsEnabled() {
    HRESULT hrCo = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    bool ok = false;

    ITaskService* svc = OpenTaskService();
    if (svc) {
        ITaskFolder* root = GetRootFolder(svc);
        if (root) {
            IRegisteredTask* t = nullptr;
            BSTR name = SysAllocString(kTaskName);
            ok = SUCCEEDED(root->GetTask(name, &t)) && t != nullptr;
            SysFreeString(name);
            if (t) t->Release();
            root->Release();
        }
        svc->Release();
    }

    if (SUCCEEDED(hrCo)) CoUninitialize();
    return ok;
}

bool AutoStartSet(bool enable) {
    HRESULT hrCo = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    bool ok = false;

    ITaskService* svc = OpenTaskService();
    ITaskFolder* root = svc ? GetRootFolder(svc) : nullptr;

    if (root) {
        BSTR name = SysAllocString(kTaskName);

        if (!enable) {
            HRESULT hrd = root->DeleteTask(name, 0);
            ok = SUCCEEDED(hrd);
            if (!ok) ok = true;   // 本就不存在，视为成功
            Logf(L"[task] 删除自启任务 hr=0x%08X", (unsigned)hrd);
        } else {
            wchar_t sidS[128] = {};
            bool haveSid = CurrentUserSid(sidS, 128);

            ITaskDefinition* def = nullptr;
            if (SUCCEEDED(svc->NewTask(0, &def)) && def) {
                // 描述（带版本标记，下次启动据此判断是否要重写）
                IRegistrationInfo* ri = nullptr;
                if (SUCCEEDED(def->get_RegistrationInfo(&ri)) && ri) {
                    BSTR d = SysAllocString(kTaskDesc);
                    ri->put_Description(d);
                    SysFreeString(d);
                    ri->Release();
                }

                // 身份：当前用户 + 最高权限
                IPrincipal* pr = nullptr;
                if (SUCCEEDED(def->get_Principal(&pr)) && pr) {
                    pr->put_LogonType(TASK_LOGON_INTERACTIVE_TOKEN);
                    pr->put_RunLevel(TASK_RUNLEVEL_HIGHEST);
                    pr->Release();
                }

                // 设置
                ITaskSettings* st = nullptr;
                if (SUCCEEDED(def->get_Settings(&st)) && st) {
                    st->put_StartWhenAvailable(VARIANT_TRUE);           // 错过了（关机/睡眠）也补跑
                    st->put_DisallowStartIfOnBatteries(VARIANT_FALSE);
                    st->put_StopIfGoingOnBatteries(VARIANT_FALSE);
                    st->put_RunOnlyIfIdle(VARIANT_FALSE);
                    st->put_MultipleInstances(TASK_INSTANCES_IGNORE_NEW);
                    st->put_RestartCount(3);                            // 启动失败自动重试
                    BSTR lim = SysAllocString(L"PT0S");
                    st->put_ExecutionTimeLimit(lim);
                    SysFreeString(lim);
                    BSTR rint = SysAllocString(L"PT1M");
                    st->put_RestartInterval(rint);
                    SysFreeString(rint);
                    st->Release();
                }

                // 触发器 1：该用户登录时，延迟 20 秒（等桌面和服务就绪）
                // 触发器 2：会话解锁时兜底 —— 主通道没跑起来、或进程中途被结束，解锁即补
                BSTR bsid = haveSid ? SysAllocString(sidS) : nullptr;
                ITriggerCollection* tc = nullptr;
                if (SUCCEEDED(def->get_Triggers(&tc)) && tc) {
                    ITrigger* trg = nullptr;
                    if (SUCCEEDED(tc->Create(TASK_TRIGGER_LOGON, &trg)) && trg) {
                        ILogonTrigger* lt = nullptr;
                        if (SUCCEEDED(trg->QueryInterface(IID_ILogonTrigger, (void**)&lt)) && lt) {
                            BSTR delay = SysAllocString(L"PT20S");
                            lt->put_Delay(delay);
                            if (bsid) lt->put_UserId(bsid);
                            SysFreeString(delay);
                            lt->Release();
                        }
                        trg->Release();
                    }
                    trg = nullptr;
                    if (SUCCEEDED(tc->Create(TASK_TRIGGER_SESSION_STATE_CHANGE, &trg)) && trg) {
                        ISessionStateChangeTrigger* sct = nullptr;
                        if (SUCCEEDED(trg->QueryInterface(IID_ISessionStateChangeTrigger, (void**)&sct)) && sct) {
                            sct->put_StateChange(TASK_SESSION_UNLOCK);
                            if (bsid) sct->put_UserId(bsid);
                            sct->Release();
                        }
                        trg->Release();
                    }
                    tc->Release();
                }
                if (bsid) SysFreeString(bsid);

                // 动作：<exe> /tray
                IActionCollection* ac = nullptr;
                if (SUCCEEDED(def->get_Actions(&ac)) && ac) {
                    IAction* act = nullptr;
                    if (SUCCEEDED(ac->Create(TASK_ACTION_EXEC, &act)) && act) {
                        IExecAction* ea = nullptr;
                        if (SUCCEEDED(act->QueryInterface(IID_IExecAction, (void**)&ea)) && ea) {
                            wchar_t self[MAX_PATH];
                            GetModuleFileNameW(NULL, self, MAX_PATH);
                            BSTR p = SysAllocString(self);
                            BSTR a = SysAllocString(L"/tray");
                            ea->put_Path(p);
                            ea->put_Arguments(a);
                            SysFreeString(p);
                            SysFreeString(a);
                            ea->Release();
                        }
                        act->Release();
                    }
                    ac->Release();
                }

                // 注册：用 SID 而不是用户名，避免同名账户解析到别人
                BSTR buser = SysAllocString(haveSid ? sidS : L"");
                BSTR sddl  = SysAllocString(L"");

                VARIANT vUser;
                VariantInit(&vUser);
                vUser.vt      = VT_BSTR;
                vUser.bstrVal = buser;

                VARIANT vPass;      // VT_EMPTY：使用当前用户令牌
                VariantInit(&vPass);

                VARIANT vSddl;
                VariantInit(&vSddl);
                vSddl.vt      = VT_BSTR;
                vSddl.bstrVal = sddl;

                IRegisteredTask* reg = nullptr;
                HRESULT hr = root->RegisterTaskDefinition(
                    name, def, TASK_CREATE_OR_UPDATE,
                    vUser, vPass, TASK_LOGON_INTERACTIVE_TOKEN, vSddl, &reg);
                ok = SUCCEEDED(hr);
                if (reg) {
                    reg->put_Enabled(VARIANT_TRUE);   // 明确置为启用
                    reg->Release();
                }
                SysFreeString(buser);
                SysFreeString(sddl);
                def->Release();
                Logf(L"[task] 注册自启任务 hr=0x%08X ok=%d sid=%s", (unsigned)hr, ok,
                     haveSid ? sidS : L"(取不到)");
                if (!ok) {
                    wchar_t msg[256];
                    swprintf_s(msg, L"计划任务注册失败 0x%08X", (unsigned)hr);
                    wcscpy_s(g_app.status, msg);
                }
            }
        }
        SysFreeString(name);
        root->Release();
    }

    if (svc) svc->Release();
    if (SUCCEEDED(hrCo)) CoUninitialize();
    return ok;
}

// 任务定义是不是当前版本：读 XML 找版本标记
static bool AutoStartIsCurrent() {
    HRESULT hrCo = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    bool cur = false;

    ITaskService* svc = OpenTaskService();
    if (svc) {
        ITaskFolder* root = GetRootFolder(svc);
        if (root) {
            IRegisteredTask* t = nullptr;
            BSTR name = SysAllocString(kTaskName);
            if (SUCCEEDED(root->GetTask(name, &t)) && t) {
                BSTR xml = nullptr;
                if (SUCCEEDED(t->get_Xml(&xml)) && xml) {
                    cur = wcsstr(xml, kTaskVerTag) != nullptr;
                    SysFreeString(xml);
                }
                t->Release();
            }
            SysFreeString(name);
            root->Release();
        }
        svc->Release();
    }

    if (SUCCEEDED(hrCo)) CoUninitialize();
    return cur;
}

// 自启开着、但定义是旧版本 → 按当前定义重写一次。
// 升级程序后老任务不会自动变成新配置，靠这个自愈。
bool AutoStartEnsure() {
    if (!AutoStartIsEnabled()) return true;
    if (AutoStartIsCurrent()) return true;
    Logf(L"[task] 检测到旧版任务定义，按当前版本重写");
    bool ok = AutoStartSet(true);
    Logf(L"[task] 重写结果 %d", ok);
    return ok;
}

// ---------------- 消息处理 ----------------
bool TrayHandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_msgTaskbar && msg == g_msgTaskbar) {
        // 资源管理器重启，图标需要重新加入
        g_added = false;
        FillNid(hwnd);
        g_added = Shell_NotifyIconW(NIM_ADD, &g_nid) != FALSE;
        return true;
    }

    if (msg == WM_FANCTRL_TRAY) {
        switch (LOWORD(lp)) {
        case WM_LBUTTONUP:
        case WM_LBUTTONDBLCLK:
            PostMessage(hwnd, WM_FANCTRL_SHOW, 0, 0);
            return true;
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU:
            ShowMenu(hwnd);
            return true;
        }
        return true;
    }

    if (msg == WM_COMMAND) {
        switch (LOWORD(wp)) {
        case kCmdShow:
            PostMessage(hwnd, WM_FANCTRL_SHOW, 0, 0);
            return true;
        case kCmdCurve:
            PostMessage(hwnd, WM_FANCTRL_MODE, MODE_CURVE, 0);
            return true;
        case kCmdAuto:
            PostMessage(hwnd, WM_FANCTRL_MODE, MODE_AUTO, 0);
            return true;
        case kCmdFull:
            PostMessage(hwnd, WM_FANCTRL_MODE, MODE_FULL, 0);
            return true;
        case kCmdAutoStart: {
            bool want = !g_app.startWithWindows;
            if (AutoStartSet(want)) {
                g_app.startWithWindows = want;
                wcscpy_s(g_app.status, want ? L"已开启开机自启" : L"已关闭开机自启");
                Logf(L"[task] 用户切换自启 -> %d", want);
            } else {
                wcscpy_s(g_app.status, L"计划任务操作失败");
                MessageBoxW(hwnd, L"无法修改开机自启的计划任务。\n请确认程序以管理员身份运行。",
                            L"小新风扇控制", MB_ICONWARNING | MB_OK);
            }
            InvalidateRect(hwnd, NULL, FALSE);
            return true;
        }
        case kCmdExit:
            DestroyWindow(hwnd);
            return true;
        default:
            return false;
        }
    }

    return false;
}
