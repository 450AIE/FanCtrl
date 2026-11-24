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

void SensorClose() {
    if (s_dev != INVALID_HANDLE_VALUE) {
        CloseHandle(s_dev);
        s_dev = INVALID_HANDLE_VALUE;
    }
}
