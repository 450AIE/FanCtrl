#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>

// ---- 转速规格 ----
constexpr int kRpmMax = 6000;

// 两只风扇的硬件上限（来自 LENOVO_FAN_TEST_DATA）
constexpr int kFan1MaxRpm = 5700;
constexpr int kFan2MaxRpm = 6000;

// ---- 功耗曲线（主控）----
// 横轴：封装功耗 W；纵轴：目标转速
// 点位不均匀，0~30W 一段平线，30W 之后逐级抬升
constexpr int kPPointCount = 7;
constexpr int kPEndW       = 70;
constexpr int kPX[kPPointCount] = { 0, 30, 40, 50, 60, 65, 70 };

// 实测推荐曲线：室温 26-27℃，目标把温度压在 83-85℃
// 依据：50W/3300rpm→84℃，60W/4800rpm→83℃，65W 满转兜底
constexpr int kRecommended[kPPointCount] = { 2000, 2000, 2500, 3300, 4800, 6000, 6000 };

// ---- 温度曲线（保留，仅作参考不再驱动风扇）----
constexpr int kPointCount = 9;
constexpr int kTempMin    = 20;
constexpr int kTempStep   = 10;
constexpr int kTempMax    = kTempMin + kTempStep * (kPointCount - 1);

enum RunMode {
    MODE_CURVE = 0,   // 按功耗曲线控制
    MODE_AUTO  = 1,   // 交还 EC
    MODE_FULL  = 2    // 全速
};

// 自定义消息
#define WM_FANCTRL_TRAY  (WM_APP + 11)   // 托盘回调
#define WM_FANCTRL_SHOW  (WM_APP + 12)   // 请求显示窗口
#define WM_FANCTRL_MODE  (WM_APP + 13)   // 请求切换模式（wParam = RunMode）

struct AppState {
    // 功耗曲线：每个点对应的目标转速
    int  pPoints[kPPointCount] = { 2000, 2000, 2500, 3300, 4800, 6000, 6000 };

    // 温度曲线（保留但不再使用）
    int  points[kPointCount] = { 0, 0, 0, 1800, 2400, 3000, 3800, 4800, 6000 };

    int  safetyTemp    = 92;    // 达到即交还 EC
    int  forceTemp     = 88;    // 达到即强制补转速
    int  minWriteDelta = 60;
    bool topmost       = false;
    bool startWithWindows = false;

    RunMode mode       = MODE_CURVE;
    bool    manual     = false;   // 当前是否处于手动接管状态
    int     lastF1     = -1;
    int     lastF2     = -1;
    int     zeroCount  = 0;

    // 实时数据
    int    temp    = -1;
    int    fan1    = -1;
    int    fan2    = -1;
    int    target  = -1;

    // 功耗
    double pkgW    = 0.0;    // 封装总功耗（CPU + 核显 + 内存）
    double cpuW    = 0.0;    // CPU 核心
    double gpuW    = 0.0;    // 核显
    double smoothW = 0.0;    // 平滑后的控制输入
    bool   powerOk = false;

    int    previewRpm = 0;   // >0 表示试听模式，风扇固定在该转速

    bool sensorOk = false;
    bool fanOk    = false;

    wchar_t status[128] = L"正在初始化";
};

extern AppState g_app;

// ---- sensor.cpp ----
bool SensorInit();
int  SensorReadTempC();          // 返回摄氏度，失败 -1
bool SensorReadPower(double* pkg, double* cpu, double* gpu);  // 失败 false
int  SensorTjMax();
void SensorClose();

// ---- fan.cpp ----
bool FanInit();
int  FanReadRpm(int index);              // index: 0=风扇1, 1=风扇2；失败 -1
bool FanWriteRpm(int index, int rpm);    // 写目标转速
bool FanRelease();                       // 写 0，交还 EC
void FanClose();

// ---- config.cpp ----
void ConfigLoad();
void ConfigSave();

// ---- tray.cpp ----
bool TrayInit(HWND hwnd);
void TrayRemove();
bool TrayHandleMessage(HWND, UINT, WPARAM, LPARAM);
void TrayBalloon(const wchar_t* title, const wchar_t* text);
void TrayUpdateTip();
HICON TrayAppIcon(bool useSmall);
bool AutoStartIsEnabled();
bool AutoStartSet(bool enable);

// ---- 由功耗求目标转速 ----
inline int PowerTargetRpm(double w) {
    if (w <= (double)kPX[0]) return g_app.pPoints[0];
    for (int i = 0; i < kPPointCount - 1; i++) {
        double lo = (double)kPX[i], hi = (double)kPX[i + 1];
        if (w <= hi) {
            double f = (w - lo) / (hi - lo);
            int a = g_app.pPoints[i], b = g_app.pPoints[i + 1];
            return a + (int)((b - a) * f + 0.5);
        }
    }
    return g_app.pPoints[kPPointCount - 1];
}

// ---- 温度曲线（保留，仅用于显示参考）----
inline int CurveTargetRpm(int tempC) {
    if (tempC <= kTempMin) return g_app.points[0];
    if (tempC >= kTempMax) return g_app.points[kPointCount - 1];
    int idx = (tempC - kTempMin) / kTempStep;
    int lo  = kTempMin + idx * kTempStep;
    int frac = tempC - lo;
    int a = g_app.points[idx];
    int b = g_app.points[idx + 1];
    return a + (b - a) * frac / kTempStep;
}
