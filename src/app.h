#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>

// ---- 曲线规格 ----
constexpr int kPointCount = 9;
constexpr int kTempMin    = 20;    // 曲线起点温度
constexpr int kTempStep   = 10;    // 每档 10 度
constexpr int kTempMax    = kTempMin + kTempStep * (kPointCount - 1);  // 100
constexpr int kRpmMax     = 6000;

// 两只风扇的硬件上限（来自 LENOVO_FAN_TEST_DATA）
constexpr int kFan1MaxRpm = 5700;
constexpr int kFan2MaxRpm = 6000;

enum RunMode {
    MODE_CURVE = 0,   // 按曲线控制
    MODE_AUTO  = 1,   // 交还 EC
    MODE_FULL  = 2    // 全速
};

// 自定义消息
#define WM_FANCTRL_TRAY  (WM_APP + 11)   // 托盘回调
#define WM_FANCTRL_SHOW  (WM_APP + 12)   // 请求显示窗口
#define WM_FANCTRL_MODE  (WM_APP + 13)   // 请求切换模式（wParam = RunMode）

struct AppState {
    int  points[kPointCount] = { 0, 0, 0, 1800, 2400, 3000, 3800, 4800, 6000 };
    int  safetyTemp          = 92;
    int  minWriteDelta       = 60;
    bool topmost             = false;
    bool startWithWindows    = false;   // 计划任务状态（运行时查询，不写 ini）

    RunMode mode       = MODE_CURVE;
    bool    manual     = false;   // 当前是否处于手动接管状态
    int     lastF1     = -1;
    int     lastF2     = -1;
    int     zeroCount  = 0;

    // 实时数据
    int temp   = -1;
    int fan1   = -1;
    int fan2   = -1;
    int target = -1;

    bool sensorOk = false;
    bool fanOk    = false;

    wchar_t status[128] = L"正在初始化";
};

extern AppState g_app;

// ---- sensor.cpp ----
bool SensorInit();
int  SensorReadTempC();      // 返回摄氏度，失败 -1
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

// ---- tray.cpp：托盘图标 + 开机自启（计划任务） ----
bool TrayInit(HWND hwnd);                                    // 创建图标并加入托盘
void TrayRemove();                                           // 移除托盘图标
bool TrayHandleMessage(HWND, UINT, WPARAM, LPARAM);           // 返回 true 表示已处理
void TrayBalloon(const wchar_t* title, const wchar_t* text);  // 气泡提示
void TrayUpdateTip();                                        // 更新悬停提示
HICON TrayAppIcon(bool useSmall);                               // 取生成的窗口图标
bool AutoStartIsEnabled();
bool AutoStartSet(bool enable);

// ---- 曲线 ----
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
