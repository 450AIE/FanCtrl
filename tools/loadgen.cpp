// loadgen.cpp - 受控 CPU 负载发生器
//
// 用法: loadgen.exe <线程数> <占空比百分比> <持续秒数>
// 每个线程以 100ms 为周期，忙等 duty% 的时间、Sleep 剩余时间，
// 用 timeBeginPeriod(1) 把 Sleep 精度提到 1ms。
//
// 自带硬超时：无论外部发生什么，到时间一定退出，不会把 CPU 一直压满。

#include <windows.h>
#include <mmsystem.h>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <thread>
#include <atomic>

#pragma comment(lib, "winmm.lib")

static std::atomic<bool> g_stop(false);

int wmain(int argc, wchar_t** argv) {
    int threads = (argc > 1) ? _wtoi(argv[1]) : 8;
    int duty    = (argc > 2) ? _wtoi(argv[2]) : 100;
    int secs    = (argc > 3) ? _wtoi(argv[3]) : 60;

    if (threads < 1) threads = 1;
    if (threads > 64) threads = 64;
    if (duty < 0) duty = 0;
    if (duty > 100) duty = 100;
    if (secs < 1) secs = 1;

    wprintf(L"[loadgen] threads=%d duty=%d secs=%d\n", threads, duty, secs);
    fflush(stdout);

    timeBeginPeriod(1);

    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    const LONGLONG period = f.QuadPart / 10;              // 100 ms
    const LONGLONG busy   = period * duty / 100;

    std::vector<std::thread> pool;
    pool.reserve(threads);
    for (int i = 0; i < threads; i++) {
        pool.emplace_back([f, period, busy]() {
            while (!g_stop.load(std::memory_order_relaxed)) {
                LARGE_INTEGER a, b;
                QueryPerformanceCounter(&a);
                // 忙等
                do { QueryPerformanceCounter(&b); }
                while (b.QuadPart - a.QuadPart < busy && !g_stop.load(std::memory_order_relaxed));

                LONGLONG used = b.QuadPart - a.QuadPart;
                LONGLONG rest = period - used;
                if (rest > 0) {
                    DWORD ms = (DWORD)(rest * 1000 / f.QuadPart);
                    if (ms > 0) Sleep(ms);
                }
            }
        });
    }

    // 硬超时：主线程负责按时收工
    for (int t = 0; t < secs * 10 && !g_stop.load(); t++) Sleep(100);
    g_stop.store(true);

    for (auto& th : pool) th.join();
    timeEndPeriod(1);
    wprintf(L"[loadgen] done\n");
    return 0;
}
