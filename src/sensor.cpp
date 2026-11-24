// sensor.cpp - 通过 PawnIO 驱动直接读取 Intel CPU 的 MSR 温度寄存器
//
// 原理：CPU 温度在 DTS（Digital Thermal Sensor），只能在内核态用 RDMSR 读取。
// PawnIO 是签名驱动，提供受控的硬件访问。我们加载 IntelMSR 模块后，
// 通过 IOCTL 让驱动代为执行 RDMSR。
//
// 协议来自 namazso/PawnIO 的 pawnio_um.h：
//   设备路径 \\?\GLOBALROOT\Device\PawnIO
//   IOCTL_PIO_LOAD_BINARY = CTL_CODE(0xA1B2, 0x821, METHOD_BUFFERED, FILE_ANY_ACCESS)
//   IOCTL_PIO_EXECUTE_FN  = CTL_CODE(0xA1B2, 0x841, METHOD_BUFFERED, FILE_ANY_ACCESS)
// 执行缓冲区布局：[32 字节函数名][参数数组 int64...]

#include "app.h"
#include <vector>
#include <cstring>
#include <cwchar>

#ifndef IOCTL_PIO_LOAD_BINARY
#define IOCTL_PIO_LOAD_BINARY 0xA1B22084u
#endif
#ifndef IOCTL_PIO_EXECUTE_FN
#define IOCTL_PIO_EXECUTE_FN  0xA1B22104u
#endif

#define FN_NAME_LENGTH 32

static HANDLE s_dev = INVALID_HANDLE_VALUE;
static int    s_tjMax = 100;

// ---- 功耗采样状态 ----
// PKG  0x611 = 封装总功耗（CPU + 核显 + 内存 + uncore）
// PP0  0x639 = CPU 核心
// PP1  0x641 = 核显
static bool     s_powInit  = false;
static uint64_t s_prevE[3] = { 0, 0, 0 };
static double   s_prevTime = 0.0;
static double   s_eUnitJ   = 1.0 / 16384.0;   // RAPL 能量单位，SensorInit 中按 MSR 0x606 重算

static double QpcSec() {
    static LARGE_INTEGER f = {};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}

static bool LoadModuleFromResource(HANDLE dev) {
    HRSRC res = FindResourceW(NULL, MAKEINTRESOURCEW(101), RT_RCDATA);
    if (!res) return false;

    DWORD size = SizeofResource(NULL, res);
    HGLOBAL mem = LoadResource(NULL, res);
    if (!mem || size == 0) return false;

    void* data = LockResource(mem);
    if (!data) return false;

    DWORD ret = 0;
    BOOL ok = DeviceIoControl(dev, IOCTL_PIO_LOAD_BINARY, data, size, NULL, 0, &ret, NULL);
    return ok != FALSE;
}

static bool ExecFn(const char* name, const uint64_t* in, size_t inCount,
                   uint64_t* out, size_t outCount, DWORD* err) {
    if (s_dev == INVALID_HANDLE_VALUE) { if (err) *err = ERROR_INVALID_HANDLE; return false; }

    std::vector<BYTE> buf(FN_NAME_LENGTH + inCount * sizeof(uint64_t), 0);
    size_t nameLen = strlen(name);
    if (nameLen > FN_NAME_LENGTH - 1) nameLen = FN_NAME_LENGTH - 1;
    memcpy(buf.data(), name, nameLen);
    if (inCount) memcpy(buf.data() + FN_NAME_LENGTH, in, inCount * sizeof(uint64_t));

    std::vector<uint64_t> ob(outCount ? outCount : 1, 0);
    DWORD read = 0;
    BOOL ok = DeviceIoControl(s_dev, IOCTL_PIO_EXECUTE_FN,
                              buf.data(), (DWORD)buf.size(),
                              ob.data(), (DWORD)(ob.size() * sizeof(uint64_t)),
                              &read, NULL);
    if (!ok) { if (err) *err = GetLastError(); return false; }
    if (out && outCount) {
        size_t n = read / sizeof(uint64_t);
        if (n > outCount) n = outCount;
        memcpy(out, ob.data(), n * sizeof(uint64_t));
    }
    if (err) *err = 0;
    return true;
}

static bool ReadMsr(uint32_t index, uint64_t* value) {
    uint64_t in[1] = { index };
    uint64_t out[1] = { 0 };
    DWORD err = 0;
    if (!ExecFn("ioctl_read_msr", in, 1, out, 1, &err)) return false;
    *value = out[0];
    return true;
}

bool SensorInit() {
    s_dev = CreateFileW(L"\\\\?\\GLOBALROOT\\Device\\PawnIO",
                        GENERIC_READ | GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE,
                        NULL, OPEN_EXISTING, 0, NULL);
    if (s_dev == INVALID_HANDLE_VALUE) {
        wcscpy_s(g_app.status, L"PawnIO 驱动未安装或无法打开");
        return false;
    }
    if (!LoadModuleFromResource(s_dev)) {
        CloseHandle(s_dev);
        s_dev = INVALID_HANDLE_VALUE;
        wcscpy_s(g_app.status, L"PawnIO 模块加载失败");
        return false;
    }

    uint64_t v = 0;
    if (ReadMsr(0x1A2, &v)) {
        int tj = (int)((v >> 16) & 0xFF);
        if (tj > 0 && tj <= 120) s_tjMax = tj;
    }

    // RAPL 能量单位：1 / 2^(bits 12:8) 焦耳
    if (ReadMsr(0x606, &v)) {
        int ee = (int)((v >> 8) & 0x1F);
        if (ee > 0 && ee < 31) s_eUnitJ = 1.0 / (double)(1 << ee);
    }

    // 试读一次功耗，确认这个平台支持
    double a, b, c;
    s_powInit = false;
    if (SensorReadPower(&a, &b, &c)) {
        // 首次调用只建立基线，返回 false，正常
    }
    return true;
}

int SensorTjMax() { return s_tjMax; }

int SensorReadTempC() {
    uint64_t v = 0;
    if (!ReadMsr(0x1B1, &v)) return -1;          // IA32_PACKAGE_THERM_STATUS
    if (((v >> 31) & 1) == 0) return -1;         // 读数无效标志
    int delta = (int)((v >> 16) & 0x7F);
    return s_tjMax - delta;
}

// 读 RAPL 各域能量计数器，两次采样差除以时间得到瓦数
// 首次调用只建立基线，返回 false
bool SensorReadPower(double* pkg, double* cpu, double* gpu) {
    if (s_dev == INVALID_HANDLE_VALUE) return false;

    uint64_t e[3] = {};
    if (!ReadMsr(0x611, &e[0])) return false;    // PKG
    if (!ReadMsr(0x639, &e[1])) return false;    // PP0 / cores
    if (!ReadMsr(0x641, &e[2])) return false;    // PP1 / graphics

    double t = QpcSec();
    if (!s_powInit) {
        for (int i = 0; i < 3; i++) s_prevE[i] = e[i];
        s_prevTime = t;
        s_powInit = true;
        return false;
    }

    double dt = t - s_prevTime;
    if (dt < 0.05) return false;                 // 间隔太短，噪声大

    double out[3] = {};
    for (int i = 0; i < 3; i++) {
        // 32 位计数器回绕：无符号相减自然正确
        uint32_t d = (uint32_t)(e[i] & 0xFFFFFFFF) - (uint32_t)(s_prevE[i] & 0xFFFFFFFF);
        out[i] = (double)d * s_eUnitJ / dt;
        s_prevE[i] = e[i];
    }
    s_prevTime = t;

    if (pkg) *pkg = out[0];
    if (cpu) *cpu = out[1];
    if (gpu) *gpu = out[2];
    return true;
}

void SensorClose() {
    if (s_dev != INVALID_HANDLE_VALUE) {
        CloseHandle(s_dev);
        s_dev = INVALID_HANDLE_VALUE;
    }
    s_powInit = false;
}
