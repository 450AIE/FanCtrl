// plinfo.cpp - 读取 Intel CPU 功耗墙（PL1/PL2）、实际封装功耗、频率、温度
//
// 只读工具，不写任何寄存器。通过 PawnIO 驱动执行 RDMSR。
//
// 用法:
//   plinfo.exe            打印一次配置
//   plinfo.exe 10         连续采样 10 秒（每 500ms 一行）

#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <cstring>
#include <intrin.h>

#ifndef IOCTL_PIO_LOAD_BINARY
#define IOCTL_PIO_LOAD_BINARY 0xA1B22084u
#endif
#ifndef IOCTL_PIO_EXECUTE_FN
#define IOCTL_PIO_EXECUTE_FN  0xA1B22104u
#endif

#define FN_NAME_LENGTH 32

static HANDLE s_dev = INVALID_HANDLE_VALUE;
static int    s_tjMax = 100;

static bool LoadModuleFromFile(HANDLE dev) {
    const wchar_t* paths[] = { L"res\\IntelMSR.bin", L"IntelMSR.bin", L"..\\res\\IntelMSR.bin" };
    for (auto p : paths) {
        HANDLE f = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        if (f == INVALID_HANDLE_VALUE) continue;
        DWORD size = GetFileSize(f, NULL);
        if (size == 0 || size > (1 << 20)) { CloseHandle(f); continue; }
        std::vector<BYTE> buf(size);
        DWORD got = 0;
        BOOL ok = ReadFile(f, buf.data(), size, &got, NULL);
        CloseHandle(f);
        if (!ok || got != size) continue;
        DWORD ret = 0;
        if (DeviceIoControl(dev, IOCTL_PIO_LOAD_BINARY, buf.data(), size, NULL, 0, &ret, NULL))
            return true;
    }
    return false;
}

static bool ExecFn(const char* name, const uint64_t* in, size_t inCount,
                   uint64_t* out, size_t outCount) {
    if (s_dev == INVALID_HANDLE_VALUE) return false;
    std::vector<BYTE> buf(FN_NAME_LENGTH + inCount * sizeof(uint64_t), 0);
    size_t nl = strlen(name);
    if (nl > FN_NAME_LENGTH - 1) nl = FN_NAME_LENGTH - 1;
    memcpy(buf.data(), name, nl);
    if (inCount) memcpy(buf.data() + FN_NAME_LENGTH, in, inCount * sizeof(uint64_t));

    std::vector<uint64_t> ob(outCount ? outCount : 1, 0);
    DWORD read = 0;
    BOOL ok = DeviceIoControl(s_dev, IOCTL_PIO_EXECUTE_FN,
                              buf.data(), (DWORD)buf.size(),
                              ob.data(), (DWORD)(ob.size() * sizeof(uint64_t)),
                              &read, NULL);
    if (!ok) return false;
    if (out && outCount) {
        size_t n = read / sizeof(uint64_t);
        if (n > outCount) n = outCount;
        memcpy(out, ob.data(), n * sizeof(uint64_t));
    }
    return true;
}

static bool Rd(uint32_t idx, uint64_t* v) {
    uint64_t in[1] = { idx };
    uint64_t out[1] = { 0 };
    if (!ExecFn("ioctl_read_msr", in, 1, out, 1)) return false;
    *v = out[0];
    return true;
}

static double NowSec() {
    static LARGE_INTEGER f = {};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}

int wmain(int argc, wchar_t** argv) {
    double watchSec = (argc > 1) ? _wtof(argv[1]) : 0;

    s_dev = CreateFileW(L"\\\\?\\GLOBALROOT\\Device\\PawnIO",
                        GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        NULL, OPEN_EXISTING, 0, NULL);
    if (s_dev == INVALID_HANDLE_VALUE) {
        printf("[ERR] PawnIO device open failed (err=%lu), need admin.\n", GetLastError());
        return 1;
    }
    if (!LoadModuleFromFile(s_dev)) {
        printf("[ERR] cannot load IntelMSR module\n");
        return 1;
    }

    uint64_t v = 0;
    if (Rd(0x1A2, &v)) {
        int tj = (int)((v >> 16) & 0xFF);
        if (tj > 0 && tj <= 120) s_tjMax = tj;
    }

    uint64_t pu = 0;
    if (!Rd(0x606, &pu)) { printf("[ERR] MSR 0x606 failed\n"); return 1; }
    int powerExp  = (int)(pu & 0xF);
    int energyExp = (int)((pu >> 8) & 0x1F);
    int timeExp   = (int)((pu >> 16) & 0xF);
    double pUnitW = 1.0 / (double)(1 << powerExp);
    double eUnitJ = 1.0 / (double)(1 << energyExp);
    double tUnitS = 1.0 / (double)(1 << timeExp);

    int regs[4] = {};
    __cpuid(regs, 0);
    int maxBasic = regs[0];
    char brand[49] = {};
    __cpuid(regs, 0x80000000);
    if ((unsigned)regs[0] >= 0x80000004u) {
        for (int i = 0; i < 3; i++) {
            __cpuid(regs, 0x80000002 + i);
            memcpy(brand + i * 16, regs, 16);
        }
    }
    // 去掉品牌串前面的多余空格
    char* p = brand;
    while (*p == ' ') p++;

    printf("===== CPU =====\n  %s\n", p);

    printf("\n===== RAPL unit =====\n");
    printf("  power  1/%d W   energy 1/%d J   time 1/%d s\n",
           1 << powerExp, 1 << energyExp, 1 << timeExp);

    uint64_t pl = 0;
    if (!Rd(0x610, &pl)) { printf("[ERR] MSR 0x610 failed\n"); return 1; }
    double pl1 = (double)(pl & 0x7FFF) * pUnitW;
    int    pl1en = (int)((pl >> 15) & 1);
    int    pl1clamp = (int)((pl >> 16) & 1);
    double pl1win = (double)((pl >> 17) & 0x7F) * tUnitS;
    double pl2 = (double)(((pl >> 32) & 0x7FFF)) * pUnitW;
    int    pl2en = (int)((pl >> 47) & 1);
    double pl2win = (double)((pl >> 49) & 0x7F) * tUnitS;
    int    lock = (int)((pl >> 63) & 1);

    printf("\n===== MSR 0x610 PKG_POWER_LIMIT =====\n");
    printf("  raw  = 0x%016llX\n", (unsigned long long)pl);
    printf("  PL1  = %.2f W  en=%d clamp=%d win=%.2f s\n", pl1, pl1en, pl1clamp, pl1win);
    printf("  PL2  = %.2f W  en=%d          win=%.2f s\n", pl2, pl2en, pl2win);
    printf("  lock = %d (%s)\n", lock, lock ? "LOCKED" : "writable");

    uint64_t info = 0;
    if (Rd(0x614, &info)) {
        double pmin = (double)(info & 0x7FFF) * pUnitW;
        double pmax = (double)((info >> 16) & 0x7FFF) * pUnitW;
        double tmax = (double)((info >> 32) & 0x7F) * tUnitS;
        printf("\n===== MSR 0x614 PKG_POWER_INFO =====\n");
        printf("  min %.2f W   max %.2f W   max window %.2f s\n", pmin, pmax, tmax);
    } else {
        printf("\n===== MSR 0x614 =====  read FAILED\n");
    }

    printf("\n===== HWP =====\n");
    uint64_t hwp = 0, req = 0;
    if (Rd(0x774, &hwp))
        printf("  CAP: highest=%llu guaranteed=%llu efficient=%llu lowest=%llu (x100MHz)\n",
               (unsigned long long)((hwp >> 8) & 0xFF), (unsigned long long)((hwp >> 16) & 0xFF),
               (unsigned long long)((hwp >> 24) & 0xFF), (unsigned long long)(hwp & 0xFF));
    else
        printf("  CAP: read FAILED\n");
    if (Rd(0x777, &req))
        printf("  REQ: min=%llu max=%llu desired=%llu\n",
               (unsigned long long)(req & 0xFF), (unsigned long long)((req >> 8) & 0xFF),
               (unsigned long long)((req >> 16) & 0xFF));
    else
        printf("  REQ: read FAILED\n");

    printf("\n===== now =====\n");
    uint64_t perf = 0;
    if (Rd(0x198, &perf))
        printf("  freq      = %.0f MHz\n", (double)(perf & 0xFFFF) * 100.0);
    uint64_t pkgT = 0;
    if (Rd(0x1B1, &pkgT)) {
        int temp = ((pkgT >> 31) & 1) ? s_tjMax - (int)((pkgT >> 16) & 0x7F) : -1;
        printf("  temp      = %d C (TjMax %d)\n", temp, s_tjMax);
        printf("  flags     = therm:%d prochot:%d crit:%d  (status bits)\n",
               (int)(pkgT & 1), (int)((pkgT >> 2) & 1), (int)((pkgT >> 4) & 1));
    }

    if (watchSec <= 0) { CloseHandle(s_dev); return 0; }

    // RAPL 各功耗域：封装 / CPU 核心 / 核显 / 内存 / 平台
    const uint32_t domMsr[5]  = { 0x611, 0x639, 0x641, 0x619, 0x64D };
    const char*    domName[5] = { "pkg", "cores", "gpu", "dram", "psys" };
    uint64_t prevDom[5] = {};
    bool     domOk[5] = {};
    for (int i = 0; i < 5; i++) domOk[i] = Rd(domMsr[i], &prevDom[i]);

    printf("\n  domains:");
    for (int i = 0; i < 5; i++) printf(" %s=%s", domName[i], domOk[i] ? "ok" : "NA");
    printf("\n\n  t(s)     pkg  cores    gpu   dram   psys    MHz  temp\n");

    double prevT = NowSec();
    int n = (int)(watchSec * 2);
    for (int i = 0; i < n; i++) {
        Sleep(500);
        double t2 = NowSec();
        double dt = t2 - prevT;
        double watt[5] = { 0, 0, 0, 0, 0 };
        for (int d = 0; d < 5; d++) {
            if (!domOk[d]) continue;
            uint64_t cur = 0;
            if (!Rd(domMsr[d], &cur)) { domOk[d] = false; continue; }
            uint32_t dE = (uint32_t)(cur & 0xFFFFFFFF) - (uint32_t)(prevDom[d] & 0xFFFFFFFF);
            prevDom[d] = cur;
            watt[d] = (double)dE * eUnitJ / dt;
        }
        prevT = t2;

        int tp = -1; double mhz = 0;
        uint64_t tt = 0, pf = 0;
        if (Rd(0x1B1, &tt) && ((tt >> 31) & 1)) tp = s_tjMax - (int)((tt >> 16) & 0x7F);
        if (Rd(0x198, &pf)) mhz = (double)(pf & 0xFFFF) * 100.0;
        printf("  %6.1f  %5.1f  %5.1f  %5.1f  %5.1f  %5.1f  %5.0f  %4d\n",
               t2, watt[0], watt[1], watt[2], watt[3], watt[4], mhz, tp);
        fflush(stdout);
    }

    CloseHandle(s_dev);
    return 0;
}
