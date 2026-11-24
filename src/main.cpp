// main.cpp - 小新风扇控制 · 主窗口
//
// 单窗口、全自绘（GDI），无子控件。
// 布局：左侧曲线编辑卡片 + 右侧数据面板 + 底部操作条

#include "app.h"
#include <windowsx.h>
#include <shellapi.h>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cwchar>

AppState g_app;

// ---------------- 颜色 ----------------
static const COLORREF C_BG        = RGB(250, 249, 246);
static const COLORREF C_CARD      = RGB(255, 255, 255);
static const COLORREF C_BORDER    = RGB(226, 223, 215);
static const COLORREF C_GRID      = RGB(240, 238, 233);
static const COLORREF C_TEXT      = RGB(44, 44, 42);
static const COLORREF C_MUTED     = RGB(136, 135, 128);
static const COLORREF C_ACCENT    = RGB(24, 95, 165);
static const COLORREF C_ACCENT_BG = RGB(232, 241, 251);
static const COLORREF C_RED       = RGB(226, 75, 74);
static const COLORREF C_GREEN     = RGB(99, 153, 34);
static const COLORREF C_AMBER     = RGB(186, 117, 23);
static const COLORREF C_BTN       = RGB(247, 246, 243);

// ---------------- 全局 ----------------
static HWND  g_hwnd = NULL;
static int   g_dpi = 96;
static HFONT g_fLabel = NULL, g_fSmall = NULL, g_fBig = NULL, g_fValue = NULL;

struct Layout {
    RECT card, plot, right, bottom;
    RECT btnCurve, btnAuto, btnFull, btnSave, btnRelease, chkTop, chkAuto;
};
static Layout L;

static int g_drag = -1;      // 正在拖动的节点索引
static bool g_tracking = false;
static bool g_ballooned = false;

static int S(int v) { return MulDiv(v, g_dpi, 96); }

static HFONT MakeFont(int pt, int weight) {
    return CreateFontW(-MulDiv(pt, g_dpi, 72), 0, 0, 0, weight, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                       L"Microsoft YaHei UI");
}

static void RebuildFonts() {
    if (g_fLabel) DeleteObject(g_fLabel);
    if (g_fSmall) DeleteObject(g_fSmall);
    if (g_fBig)   DeleteObject(g_fBig);
    if (g_fValue) DeleteObject(g_fValue);
    g_fLabel = MakeFont(9, FW_NORMAL);
    g_fSmall = MakeFont(8, FW_NORMAL);
    g_fBig   = MakeFont(21, FW_NORMAL);
    g_fValue = MakeFont(10, FW_NORMAL);
}

static void Text(HDC dc, const wchar_t* s, RECT r, HFONT f, COLORREF c, UINT fmt) {
    HGDIOBJ old = SelectObject(dc, f);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, c);
    DrawTextW(dc, s, -1, &r, fmt | DT_NOPREFIX);
    SelectObject(dc, old);
}

static void FillRound(HDC dc, RECT r, int radius, COLORREF fill, COLORREF border) {
    HBRUSH b = CreateSolidBrush(fill);
    HPEN p = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ ob = SelectObject(dc, b);
    HGDIOBJ op = SelectObject(dc, p);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius, radius);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(b);
    DeleteObject(p);
}

static void HLine(HDC dc, int x1, int x2, int y, COLORREF c) {
    HPEN p = CreatePen(PS_SOLID, 1, c);
    HGDIOBJ op = SelectObject(dc, p);
    MoveToEx(dc, x1, y, NULL);
    LineTo(dc, x2, y);
    SelectObject(dc, op);
    DeleteObject(p);
}

// 温度 -> 像素
static int PlotX(int tempC) {
    double t = (double)(tempC - kTempMin) / (kTempMax - kTempMin);
    return L.plot.left + (int)(t * (L.plot.right - L.plot.left) + 0.5);
}
// 转速 -> 像素
static int PlotY(int rpm) {
    double v = (double)rpm / kRpmMax;
    return L.plot.bottom - (int)(v * (L.plot.bottom - L.plot.top) + 0.5);
}

static int RpmFromY(int y) {
    int h = L.plot.bottom - L.plot.top;
    if (h <= 0) return 0;
    double v = (double)(L.plot.bottom - y) / h * kRpmMax;
    int rpm = (int)(v / 50.0 + 0.5) * 50;
    if (rpm < 0) rpm = 0;
    if (rpm > kRpmMax) rpm = kRpmMax;
    return rpm;
}

// ---------------- 布局 ----------------
static void ComputeLayout(int w, int h) {
    int pad     = S(16);
    int rightW  = S(212);
    int bottomH = S(58);

    int cardRight = w - pad - rightW - S(12);

    L.card   = { pad, pad, cardRight, h - pad - bottomH };
    L.plot   = { L.card.left + S(50), L.card.top + S(18),
                 L.card.right - S(18), L.card.bottom - S(30) };
    L.right  = { cardRight + S(12), pad, w - pad, h - pad - bottomH };
    L.bottom = { pad, h - pad - bottomH + S(8), w - pad, h - pad };

    int bw = S(66), bh = S(30), gap = S(4);
    int by = L.bottom.top + (L.bottom.bottom - L.bottom.top - bh) / 2;
    L.btnCurve   = { L.bottom.left, by, L.bottom.left + bw, by + bh };
    L.btnAuto    = { L.btnCurve.right + gap, by, L.btnCurve.right + gap + bw, by + bh };
    L.btnFull    = { L.btnAuto.right + gap, by, L.btnAuto.right + gap + bw, by + bh };

    int sw = S(86);
    L.btnRelease = { L.bottom.right - sw, by, L.bottom.right, by + bh };
    L.btnSave    = { L.btnRelease.left - S(8) - sw, by, L.btnRelease.left - S(8), by + bh };

    L.chkTop  = { L.right.left, L.right.bottom - S(26), L.right.right, L.right.bottom };
    L.chkAuto = { L.right.left, L.right.bottom - S(54), L.right.right, L.right.bottom - S(26) };
}

// ---------------- 绘制 ----------------
static void DrawCurveCard(HDC dc) {
    FillRound(dc, L.card, S(12), C_CARD, C_BORDER);

    // 网格
    const int rpms[5] = { 0, 1500, 3000, 4500, 6000 };
    for (int i = 0; i < 5; i++) {
        int y = PlotY(rpms[i]);
        HLine(dc, L.plot.left, L.plot.right, y, C_GRID);
        wchar_t buf[16];
        swprintf_s(buf, L"%d", rpms[i]);
        RECT r = { L.card.left + S(6), y - S(8), L.plot.left - S(6), y + S(8) };
        Text(dc, buf, r, g_fSmall, C_MUTED, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }

    for (int t = kTempMin; t <= kTempMax; t += kTempStep) {
        int x = PlotX(t);
        if (t != kTempMin && t != kTempMax) {
            HPEN p = CreatePen(PS_SOLID, 1, C_GRID);
            HGDIOBJ op = SelectObject(dc, p);
            MoveToEx(dc, x, L.plot.top, NULL);
            LineTo(dc, x, L.plot.bottom);
            SelectObject(dc, op);
            DeleteObject(p);
        }
        if ((t - kTempMin) % 20 == 0) {
            wchar_t buf[16];
            swprintf_s(buf, L"%d°", t);
            RECT r = { x - S(20), L.plot.bottom + S(4), x + S(20), L.plot.bottom + S(22) };
            Text(dc, buf, r, g_fSmall, C_MUTED, DT_CENTER | DT_TOP | DT_SINGLELINE);
        }
    }

    // 填充区域
    POINT poly[kPointCount + 2];
    for (int i = 0; i < kPointCount; i++) {
        poly[i].x = PlotX(kTempMin + i * kTempStep);
        poly[i].y = PlotY(g_app.points[i]);
    }
    poly[kPointCount].x     = PlotX(kTempMax);
    poly[kPointCount].y     = L.plot.bottom;
    poly[kPointCount + 1].x = PlotX(kTempMin);
    poly[kPointCount + 1].y = L.plot.bottom;

    HBRUSH fill = CreateSolidBrush(RGB(240, 246, 252));
    HPEN noPen = CreatePen(PS_NULL, 0, RGB(0, 0, 0));
    HGDIOBJ ob = SelectObject(dc, fill);
    HGDIOBJ op = SelectObject(dc, noPen);
    Polygon(dc, poly, kPointCount + 2);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(fill);
    DeleteObject(noPen);

    // 折线
    HPEN line = CreatePen(PS_SOLID, S(2), C_ACCENT);
    op = SelectObject(dc, line);
    for (int i = 0; i < kPointCount; i++) {
        int x = PlotX(kTempMin + i * kTempStep);
        int y = PlotY(g_app.points[i]);
        if (i == 0) MoveToEx(dc, x, y, NULL);
        else        LineTo(dc, x, y);
    }
    SelectObject(dc, op);
    DeleteObject(line);

    // 当前温度标记
    if (g_app.temp >= 0) {
        int t = g_app.temp;
        if (t < kTempMin) t = kTempMin;
        if (t > kTempMax) t = kTempMax;
        int x = PlotX(t);
        HPEN dash = CreatePen(PS_DASH, 1, C_RED);
        op = SelectObject(dc, dash);
        MoveToEx(dc, x, L.plot.top, NULL);
        LineTo(dc, x, L.plot.bottom);
        SelectObject(dc, op);
        DeleteObject(dash);

        int y = PlotY(CurveTargetRpm(t));
        HBRUSH rb = CreateSolidBrush(C_RED);
        ob = SelectObject(dc, rb);
        op = SelectObject(dc, GetStockObject(NULL_PEN));
        Ellipse(dc, x - S(4), y - S(4), x + S(4), y + S(4));
        SelectObject(dc, ob);
        SelectObject(dc, op);
        DeleteObject(rb);
    }

    // 节点
    for (int i = 0; i < kPointCount; i++) {
        int x = PlotX(kTempMin + i * kTempStep);
        int y = PlotY(g_app.points[i]);
        HBRUSH wb = CreateSolidBrush(C_CARD);
        HPEN bp = CreatePen(PS_SOLID, S(2), C_ACCENT);
        ob = SelectObject(dc, wb);
        op = SelectObject(dc, bp);
        int r = (g_drag == i) ? S(6) : S(5);
        Ellipse(dc, x - r, y - r, x + r, y + r);
        SelectObject(dc, ob);
        SelectObject(dc, op);
        DeleteObject(wb);
        DeleteObject(bp);
    }
}

static void DrawCheck(HDC dc, RECT row, const wchar_t* label, bool on) {
    RECT box = { row.left + S(16), row.top + S(6),
                 row.left + S(16) + S(13), row.top + S(19) };
    HBRUSH b = CreateSolidBrush(on ? C_ACCENT : C_CARD);
    HPEN p = CreatePen(PS_SOLID, 1, on ? C_ACCENT : C_BORDER);
    HGDIOBJ ob = SelectObject(dc, b);
    HGDIOBJ op = SelectObject(dc, p);
    RoundRect(dc, box.left, box.top, box.right, box.bottom, S(4), S(4));
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(b);
    DeleteObject(p);

    if (on) {
        HPEN tick = CreatePen(PS_SOLID, S(2), RGB(255, 255, 255));
        op = SelectObject(dc, tick);
        MoveToEx(dc, box.left + S(3), box.top + S(7), NULL);
        LineTo(dc, box.left + S(6), box.top + S(10));
        LineTo(dc, box.left + S(10), box.top + S(3));
        SelectObject(dc, op);
        DeleteObject(tick);
    }

    RECT rt = { box.right + S(8), row.top, row.right, row.bottom };
    Text(dc, label, rt, g_fSmall, C_MUTED, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

static void DrawRightPanel(HDC dc) {
    FillRound(dc, L.right, S(12), C_CARD, C_BORDER);

    int x = L.right.left + S(16);
    int y = L.right.top + S(14);
    int w = L.right.right - L.right.left - S(32);
    wchar_t buf[64];

    RECT r = { x, y, x + w, y + S(16) };
    Text(dc, L"CPU 温度", r, g_fSmall, C_MUTED, DT_LEFT | DT_TOP | DT_SINGLELINE);
    y += S(18);

    if (g_app.temp >= 0) swprintf_s(buf, L"%d ℃", g_app.temp);
    else                 swprintf_s(buf, L"-- ℃");
    r = { x, y, x + w, y + S(36) };
    COLORREF tc = C_TEXT;
    if (g_app.temp >= 0 && g_app.temp >= g_app.safetyTemp) tc = C_RED;
    Text(dc, buf, r, g_fBig, tc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    y += S(40);

    HLine(dc, x, x + w, y, C_GRID);
    y += S(12);

    struct Row { const wchar_t* k; wchar_t v[32]; COLORREF c; };
    Row rows[5];
    rows[0].k = L"目标转速";
    if (g_app.target >= 0) swprintf_s(rows[0].v, L"%d rpm", g_app.target);
    else                   swprintf_s(rows[0].v, L"--");
    rows[0].c = C_ACCENT;

    rows[1].k = L"风扇 1";
    if (g_app.fan1 >= 0) swprintf_s(rows[1].v, L"%d rpm", g_app.fan1);
    else                 swprintf_s(rows[1].v, L"--");
    rows[1].c = C_TEXT;

    rows[2].k = L"风扇 2";
    if (g_app.fan2 >= 0) swprintf_s(rows[2].v, L"%d rpm", g_app.fan2);
    else                 swprintf_s(rows[2].v, L"--");
    rows[2].c = C_TEXT;

    rows[3].k = L"控制状态";
    wcsncpy_s(rows[3].v, g_app.status, 31);
    rows[3].c = g_app.manual ? C_AMBER : C_GREEN;

    rows[4].k = L"安全阈值";
    swprintf_s(rows[4].v, L"%d ℃", g_app.safetyTemp);
    rows[4].c = C_MUTED;

    for (int i = 0; i < 5; i++) {
        RECT rk = { x, y, x + w / 2, y + S(20) };
        RECT rv = { x + w / 2, y, x + w, y + S(20) };
        Text(dc, rows[i].k, rk, g_fSmall, C_MUTED, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        Text(dc, rows[i].v, rv, g_fValue, rows[i].c, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        y += S(26);
    }

    // 使用说明
    y += S(10);
    HLine(dc, x, x + w, y, C_GRID);
    y += S(12);
    RECT rh = { x, y, x + w, y + S(16) };
    Text(dc, L"使用说明", rh, g_fSmall, C_MUTED, DT_LEFT | DT_TOP | DT_SINGLELINE);
    y += S(20);
    RECT rb = { x, y, x + w, y + S(116) };
    Text(dc, L"· 拖动圆点设定该温度下的目标转速\n"
             L"· 曲线值为 0 表示交还 EC 控制\n"
             L"· 点保存写入 settings.ini\n"
             L"· 关闭或最小化会缩到托盘\n"
             L"· 托盘图标右键可切换模式",
         rb, g_fSmall, C_MUTED, DT_LEFT | DT_TOP | DT_WORDBREAK);

    // 勾选项
    HLine(dc, x, x + w, L.chkAuto.top - S(10), C_GRID);
    DrawCheck(dc, L.chkAuto, L"开机自启（免 UAC）", g_app.startWithWindows);
    DrawCheck(dc, L.chkTop,  L"窗口置顶", g_app.topmost);
}

static void DrawButton(HDC dc, RECT r, const wchar_t* label, bool active, bool accent) {
    COLORREF bg = active ? C_ACCENT_BG : C_BTN;
    COLORREF bd = active ? C_ACCENT : C_BORDER;
    COLORREF tc = active ? C_ACCENT : (accent ? C_ACCENT : C_TEXT);
    FillRound(dc, r, S(8), bg, bd);
    Text(dc, label, r, g_fValue, tc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

static void DrawBottomBar(HDC dc) {
    DrawButton(dc, L.btnCurve, L"曲线", g_app.mode == MODE_CURVE, false);
    DrawButton(dc, L.btnAuto,  L"自动", g_app.mode == MODE_AUTO,  false);
    DrawButton(dc, L.btnFull,  L"全速", g_app.mode == MODE_FULL,  false);
    DrawButton(dc, L.btnSave,    L"保存曲线", false, false);
    DrawButton(dc, L.btnRelease, L"交还 EC",  false, true);
}

static void Paint(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);

    RECT rc;
    GetClientRect(hwnd, &rc);
    int w = rc.right, h = rc.bottom;

    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, w, h);
    HGDIOBJ oldBmp = SelectObject(mem, bmp);

    HBRUSH bg = CreateSolidBrush(C_BG);
    FillRect(mem, &rc, bg);
    DeleteObject(bg);

    DrawCurveCard(mem);
    DrawRightPanel(mem);
    DrawBottomBar(mem);

    BitBlt(dc, 0, 0, w, h, mem, 0, 0, SRCCOPY);

    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd, &ps);
}

// ---------------- 控制逻辑 ----------------
static void ApplyControl() {
    if (!g_app.fanOk) { wcscpy_s(g_app.status, L"风扇接口不可用"); return; }

    if (g_app.mode == MODE_AUTO) {
        if (g_app.manual) { FanRelease(); g_app.manual = false; g_app.lastF1 = g_app.lastF2 = -1; }
        wcscpy_s(g_app.status, L"交还 EC");
        g_app.target = -1;
        return;
    }

    if (g_app.mode == MODE_FULL) {
        if (!g_app.manual || g_app.lastF1 != kFan1MaxRpm) {
            FanWriteRpm(0, kFan1MaxRpm);
            FanWriteRpm(1, kFan2MaxRpm);
            g_app.manual = true;
            g_app.lastF1 = kFan1MaxRpm;
            g_app.lastF2 = kFan2MaxRpm;
        }
        wcscpy_s(g_app.status, L"全速运行");
        g_app.target = kRpmMax;
        return;
    }

    // MODE_CURVE
    if (g_app.temp < 0) { wcscpy_s(g_app.status, L"温度读取失败"); return; }

    if (g_app.temp >= g_app.safetyTemp) {
        if (g_app.manual) { FanRelease(); g_app.manual = false; g_app.lastF1 = g_app.lastF2 = -1; }
        wcscpy_s(g_app.status, L"超温保护 · 已交还 EC");
        g_app.target = -1;
        return;
    }

    int tgt = CurveTargetRpm(g_app.temp);
    g_app.target = tgt;

    if (tgt <= 0) {
        g_app.zeroCount++;
        if (g_app.manual && g_app.zeroCount >= 2) {
            FanRelease();
            g_app.manual = false;
            g_app.lastF1 = g_app.lastF2 = -1;
        }
        wcscpy_s(g_app.status, L"低温 · 交还 EC");
        return;
    }
    g_app.zeroCount = 0;

    int f1 = tgt * kFan1MaxRpm / kRpmMax;
    int f2 = tgt * kFan2MaxRpm / kRpmMax;
    if (f1 > 0 && f1 < 800) f1 = 800;
    if (f2 > 0 && f2 < 800) f2 = 800;

    bool need = !g_app.manual
             || g_app.lastF1 < 0
             || abs(f1 - g_app.lastF1) >= g_app.minWriteDelta
             || abs(f2 - g_app.lastF2) >= g_app.minWriteDelta;

    if (need) {
        bool ok1 = FanWriteRpm(0, f1);
        bool ok2 = FanWriteRpm(1, f2);
        if (ok1 || ok2) {
            g_app.manual = true;
            g_app.lastF1 = f1;
            g_app.lastF2 = f2;
            wcscpy_s(g_app.status, L"曲线控制中");
        } else {
            wcscpy_s(g_app.status, L"写入失败");
        }
    }
}

static void Poll() {
    g_app.temp = SensorReadTempC();
    if (g_app.fanOk) {
        g_app.fan1 = FanReadRpm(0);
        g_app.fan2 = FanReadRpm(1);
    }
    ApplyControl();
    InvalidateRect(g_hwnd, NULL, FALSE);
}

// ---------------- 交互 ----------------
static bool PtIn(RECT r, POINT p) {
    return p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom;
}

static int HitPoint(POINT p) {
    if (p.y < L.plot.top - S(12) || p.y > L.plot.bottom + S(12)) return -1;
    int best = -1, bestDx = S(14);
    for (int i = 0; i < kPointCount; i++) {
        int x = PlotX(kTempMin + i * kTempStep);
        int dx = abs(p.x - x);
        if (dx < bestDx) { bestDx = dx; best = i; }
    }
    return best;
}

static void OnClick(POINT p) {
    if (PtIn(L.btnCurve, p)) { g_app.mode = MODE_CURVE; g_app.zeroCount = 0; Poll(); return; }
    if (PtIn(L.btnAuto, p))  { g_app.mode = MODE_AUTO; Poll(); return; }
    if (PtIn(L.btnFull, p))  { g_app.mode = MODE_FULL; Poll(); return; }
    if (PtIn(L.btnSave, p)) {
        ConfigSave();
        wcscpy_s(g_app.status, L"已保存");
        InvalidateRect(g_hwnd, NULL, FALSE);
        return;
    }
    if (PtIn(L.btnRelease, p)) {
        FanRelease();
        g_app.mode = MODE_AUTO;
        g_app.manual = false;
        g_app.lastF1 = g_app.lastF2 = -1;
        Poll();
        return;
    }
    if (PtIn(L.chkTop, p)) {
        g_app.topmost = !g_app.topmost;
        SetWindowPos(g_hwnd, g_app.topmost ? HWND_TOPMOST : HWND_NOTOPMOST,
                     0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        InvalidateRect(g_hwnd, NULL, FALSE);
        return;
    }
    if (PtIn(L.chkAuto, p)) {
        bool want = !g_app.startWithWindows;
        if (AutoStartSet(want)) {
            g_app.startWithWindows = want;
            wcscpy_s(g_app.status, want ? L"已开启开机自启" : L"已关闭开机自启");
        } else {
            wcscpy_s(g_app.status, L"计划任务操作失败");
            MessageBoxW(g_hwnd, L"无法修改开机自启的计划任务。\n请确认程序以管理员身份运行。",
                        L"小新风扇控制", MB_ICONWARNING | MB_OK);
        }
        InvalidateRect(g_hwnd, NULL, FALSE);
        return;
    }
}

static void HideToTray() {
    ShowWindow(g_hwnd, SW_HIDE);
    if (!g_ballooned) {
        g_ballooned = true;
        TrayBalloon(L"仍在后台运行",
                    L"风扇曲线继续生效。双击托盘图标可重新打开，右键可切换模式或退出。");
    }
}

static void ShowFromTray() {
    ShowWindow(g_hwnd, SW_SHOW);
    ShowWindow(g_hwnd, SW_RESTORE);
    SetForegroundWindow(g_hwnd);
    InvalidateRect(g_hwnd, NULL, FALSE);
}

// ---------------- 窗口过程 ----------------
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg != WM_CREATE && TrayHandleMessage(hwnd, msg, wp, lp)) return 0;

    switch (msg) {
    case WM_CREATE: {
        g_hwnd = hwnd;
        UINT dpi = 96;
        typedef UINT (WINAPI* GetDpiForWindow_t)(HWND);
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        if (user32) {
            GetDpiForWindow_t fn = (GetDpiForWindow_t)GetProcAddress(user32, "GetDpiForWindow");
            if (fn) dpi = fn(hwnd);
        }
        if (dpi == 0) dpi = 96;
        g_dpi = (int)dpi;
        RebuildFonts();

        ConfigLoad();
        g_app.sensorOk = SensorInit();
        g_app.fanOk = FanInit();
        g_app.startWithWindows = AutoStartIsEnabled();
        TrayInit(hwnd);   // 窗口/任务栏图标来自 exe 的 ICON 资源，这里只装托盘

        RECT rc;
        GetClientRect(hwnd, &rc);
        ComputeLayout(rc.right, rc.bottom);

        if (g_app.topmost)
            SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);

        Poll();
        SetTimer(hwnd, 1, 1000, NULL);
        return 0;
    }
    case WM_SIZE:
        ComputeLayout(LOWORD(lp), HIWORD(lp));
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_GETMINMAXINFO: {
        MINMAXINFO* mm = (MINMAXINFO*)lp;
        mm->ptMinTrackSize.x = S(760);
        mm->ptMinTrackSize.y = S(520);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        Paint(hwnd);
        return 0;
    case WM_TIMER:
        Poll();
        {
            static int tick = 0;
            if (++tick % 5 == 0) TrayUpdateTip();
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == SC_MINIMIZE) { HideToTray(); return 0; }
        break;
    case WM_CLOSE:
        HideToTray();
        return 0;
    case WM_FANCTRL_SHOW:
        ShowFromTray();
        return 0;
    case WM_FANCTRL_MODE:
        g_app.mode = (RunMode)wp;
        g_app.zeroCount = 0;
        Poll();
        return 0;
    case WM_ENDSESSION:
        if (wp) FanRelease();
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            POINT p;
            GetCursorPos(&p);
            ScreenToClient(hwnd, &p);
            if (HitPoint(p) >= 0) { SetCursor(LoadCursor(NULL, IDC_SIZENS)); return TRUE; }
        }
        break;
    case WM_LBUTTONDOWN: {
        POINT p = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        int idx = HitPoint(p);
        if (idx >= 0) {
            g_drag = idx;
            SetCapture(hwnd);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        OnClick(p);
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (g_drag >= 0) {
            POINT p = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            g_app.points[g_drag] = RpmFromY(p.y);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        if (!g_tracking) {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
            g_tracking = true;
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        g_tracking = false;
        return 0;
    case WM_LBUTTONUP:
        if (g_drag >= 0) {
            g_drag = -1;
            ReleaseCapture();
            Poll();
        }
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, 1);
        TrayRemove();
        FanRelease();
        FanClose();
        SensorClose();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int) {
    // 单实例：已在运行则唤出已有窗口
    HANDLE mutex = CreateMutexW(NULL, TRUE, L"FanCtrl_SingleInstance");
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND prev = FindWindowW(L"FanCtrlWnd", NULL);
        if (prev) PostMessageW(prev, WM_FANCTRL_SHOW, 0, 0);
        return 0;
    }

    // 命令行：/tray 表示启动后直接进托盘（开机自启用）
    bool startHidden = false;
    {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (argv) {
            for (int i = 1; i < argc; i++)
                if (_wcsicmp(argv[i], L"/tray") == 0) startHidden = true;
            LocalFree(argv);
        }
    }

    typedef BOOL (WINAPI* SetDpiCtx_t)(HANDLE);
    HMODULE user32 = LoadLibraryW(L"user32.dll");
    if (user32) {
        SetDpiCtx_t fn = (SetDpiCtx_t)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
        if (fn) fn((HANDLE)-4);   // PER_MONITOR_AWARE_V2
        else SetProcessDPIAware();
    }

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = L"FanCtrlWnd";
    wc.hIcon         = LoadIconW(hInst, MAKEINTRESOURCEW(1));
    if (!wc.hIcon) wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wc.hIconSm       = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                         GetSystemMetrics(SM_CXSMICON),
                                         GetSystemMetrics(SM_CYSMICON), 0);
    if (!RegisterClassExW(&wc)) return 1;

    int dw = 900, dh = 580;
    {
        UINT sdpi = 96;
        HMODULE u32 = GetModuleHandleW(L"user32.dll");
        if (u32) {
            typedef UINT (WINAPI* GetDpiForSystem_t)();
            GetDpiForSystem_t fn = (GetDpiForSystem_t)GetProcAddress(u32, "GetDpiForSystem");
            if (fn) sdpi = fn();
        }
        if (sdpi == 0) sdpi = 96;
        dw = MulDiv(900, sdpi, 96);
        dh = MulDiv(580, sdpi, 96);
    }

    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"小新风扇控制",
                                WS_OVERLAPPEDWINDOW,
                                (sw - dw) / 2, (sh - dh) / 2, dw, dh,
                                NULL, NULL, hInst, NULL);
    if (!hwnd) return 1;

    if (startHidden) {
        ShowWindow(hwnd, SW_HIDE);
    } else {
        ShowWindow(hwnd, SW_SHOW);
        UpdateWindow(hwnd);
    }

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
