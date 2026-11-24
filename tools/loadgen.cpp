// loadgen.cpp - 受控 CPU 负载发生器
//
// 用法: loadgen.exe <线程数> <占空比百分比> <持续秒数> [spin|avx]
//   spin  (默认) 空转 QueryPerformanceCounter，功耗密度中等
//   avx         AVX2 FMA 密集浮点运算，功耗密度最高，用于测真实功耗墙
//
// 每个线程以 100ms 为周期，忙等 duty% 的时间、Sleep 剩余时间，
// 用 timeBeginPeriod(1) 把 Sleep 精度提到 1ms。
//
// 自带硬超时：无论外部发生什么，到时间一定退出，不会把 CPU 一直压满。

#include <windows.h>
#include <mmsystem.h>
#include <immintrin.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <thread>
#include <atomic>
#include <string>

#pragma comment(lib, "winmm.lib")

static std::atomic<bool> g_stop(false);
static volatile float g_sink = 0.0f;

// AVX2 FMA 满载：8 路独立累加器，每轮 8 条 256 位 FMA
static void BurnAvx(LONGLONG until, int tid) {
    __m256 m = _mm256_set1_ps(1.0f + tid * 1e-6f);
    __m256 b = _mm256_set1_ps(1e-7f);
    __m256 a0 = _mm256_set1_ps(1.0f);
    __m256 a1 = _mm256_set1_ps(1.1f);
    __m256 a2 = _mm256_set1_ps(1.2f);
    __m256 a3 = _mm256_set1_ps(1.3f);
    __m256 a4 = _mm256_set1_ps(1.4f);
    __m256 a5 = _mm256_set1_ps(1.5f);
    __m256 a6 = _mm256_set1_ps(1.6f);
    __m256 a7 = _mm256_set1_ps(1.7f);
    LARGE_INTEGER c;
    do {
        for (int k = 0; k < 32; k++) {
            a0 = _mm256_fmadd_ps(a0, m, b);
            a1 = _mm256_fmadd_ps(a1, m, b);
            a2 = _mm256_fmadd_ps(a2, m, b);
            a3 = _mm256_fmadd_ps(a3, m, b);
            a4 = _mm256_fmadd_ps(a4, m, b);
            a5 = _mm256_fmadd_ps(a5, m, b);
            a6 = _mm256_fmadd_ps(a6, m, b);
            a7 = _mm256_fmadd_ps(a7, m, b);
        }
        QueryPerformanceCounter(&c);
    } while (c.QuadPart < until && !g_stop.load(std::memory_order_relaxed));

    a0 = _mm256_add_ps(a0, a1); a2 = _mm256_add_ps(a2, a3);
    a4 = _mm256_add_ps(a4, a5); a6 = _mm256_add_ps(a6, a7);
    a0 = _mm256_add_ps(a0, a2); a4 = _mm256_add_ps(a4, a6);
    a0 = _mm256_add_ps(a0, a4);
    float tmp[8];
    _mm256_storeu_ps(tmp, a0);
    g_sink = tmp[0] + tmp[7];
}

int wmain(int argc, wchar_t** argv) {
    int threads = (argc > 1) ? _wtoi(argv[1]) : 8;
    int duty    = (argc > 2) ? _wtoi(argv[2]) : 100;
    int secs    = (argc > 3) ? _wtoi(argv[3]) : 60;
    bool useAvx = (argc > 4) && (_wcsicmp(argv[4], L"avx") == 0);

    if (threads < 1) threads = 1;
    if (threads > 64) threads = 64;
    if (duty < 0) duty = 0;
    if (duty > 100) duty = 100;
    if (secs < 1) secs = 1;

    wprintf(L"[loadgen] threads=%d duty=%d secs=%d mode=%s\n",
            threads, duty, secs, useAvx ? L"avx" : L"spin");
    fflush(stdout);

    timeBeginPeriod(1);

    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    const LONGLONG period = f.QuadPart / 10;              // 100 ms
    const LONGLONG busy   = period * duty / 100;

    std::vector<std::thread> pool;
    pool.reserve(threads);
    for (int i = 0; i < threads; i++) {
        pool.emplace_back([=]() {
            int tid = i;
            while (!g_stop.load(std::memory_order_relaxed)) {
                LARGE_INTEGER a, b;
                QueryPerformanceCounter(&a);
                if (useAvx) {
                    BurnAvx(a.QuadPart + busy, tid);
                    QueryPerformanceCounter(&b);
                } else {
                    do { QueryPerformanceCounter(&b); }
                    while (b.QuadPart - a.QuadPart < busy && !g_stop.load(std::memory_order_relaxed));
                }

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
    wprintf(L"[loadgen] done (sink=%f)\n", (double)g_sink);
    return 0;
}
