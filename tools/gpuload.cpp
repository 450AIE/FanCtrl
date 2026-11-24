// gpuload.cpp - 核显满载负载发生器（D3D11 compute shader）
//
// 用法: gpuload.exe <持续秒数> [每组线程数=256] [dispatch组数=2048] [内层迭代=4096]
//
// 提交大量 FMA 运算的 compute dispatch，让 iGPU 持续满载。
// 自带硬超时，到点退出。

#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <cstdlib>
#include <string>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")

static const char* kHlsl = R"(
RWStructuredBuffer<float4> g_out : register(u0);

cbuffer Params : register(b0)
{
    uint  g_iter;
    float g_seed;
    float2 g_pad;
};

[numthreads(256, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    float4 a = float4(g_seed, 1.0, 2.0, 3.0) + (float4)tid.x * 1e-6;
    float4 b = float4(1.0000001, 0.9999999, 1.0000002, 0.9999998);
    float4 c = float4(1e-7, 1e-7, 1e-7, 1e-7);

    [loop]
    for (uint i = 0; i < g_iter; i++)
    {
        a = a * b + c;
        a = a * b + c;
        a = a * b + c;
        a = a * b + c;
        a = a * b + c;
        a = a * b + c;
        a = a * b + c;
        a = a * b + c;
    }

    // 依赖结果做条件写，编译器无法把计算优化掉
    if (a.x == 12345.6789)
        g_out[tid.x & 1023] = a;
}
)";

int wmain(int argc, wchar_t** argv) {
    int secs     = (argc > 1) ? _wtoi(argv[1]) : 60;
    int groupSz  = (argc > 2) ? _wtoi(argv[2]) : 256;
    int groups   = (argc > 3) ? _wtoi(argv[3]) : 2048;
    int iter     = (argc > 4) ? _wtoi(argv[4]) : 4096;

    if (secs < 1) secs = 1;
    if (groups < 16) groups = 16;
    if (groups > 8192) groups = 8192;
    if (iter < 16) iter = 16;
    if (iter > 65536) iter = 65536;

    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                   nullptr, 0, D3D11_SDK_VERSION, &dev, &fl, &ctx);
    if (FAILED(hr) || !dev) {
        printf("[gpuload] D3D11CreateDevice failed hr=0x%08X\n", (unsigned)hr);
        return 1;
    }

    ID3DBlob* code = nullptr;
    ID3DBlob* err = nullptr;
    hr = D3DCompile(kHlsl, strlen(kHlsl), "cs", nullptr, nullptr, "main", "cs_5_0",
                    D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    if (FAILED(hr)) {
        printf("[gpuload] compile failed: %s\n", err ? (char*)err->GetBufferPointer() : "?");
        return 1;
    }

    ID3D11ComputeShader* cs = nullptr;
    hr = dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &cs);
    code->Release();
    if (FAILED(hr)) { printf("[gpuload] CreateComputeShader failed\n"); return 1; }

    // 输出 buffer（结构化，float4，1024 个）
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = 1024 * sizeof(float) * 4;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    bd.StructureByteStride = sizeof(float) * 4;
    ID3D11Buffer* outBuf = nullptr;
    hr = dev->CreateBuffer(&bd, nullptr, &outBuf);
    if (FAILED(hr)) { printf("[gpuload] CreateBuffer failed\n"); return 1; }

    D3D11_UNORDERED_ACCESS_VIEW_DESC uavd = {};
    uavd.Format = DXGI_FORMAT_UNKNOWN;
    uavd.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    uavd.Buffer.FirstElement = 0;
    uavd.Buffer.NumElements = 1024;
    ID3D11UnorderedAccessView* uav = nullptr;
    hr = dev->CreateUnorderedAccessView(outBuf, &uavd, &uav);
    if (FAILED(hr)) { printf("[gpuload] CreateUAV failed\n"); return 1; }

    struct Params { unsigned int iter; float seed; float p0, p1; } params;
    params.iter = (unsigned)iter;
    params.seed = 1.0f;
    params.p0 = params.p1 = 0.0f;

    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth = sizeof(Params);
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Buffer* cb = nullptr;
    hr = dev->CreateBuffer(&cbd, nullptr, &cb);
    if (FAILED(hr)) { printf("[gpuload] CreateCB failed\n"); return 1; }

    printf("[gpuload] groups=%d x %d threads, iter=%d, secs=%d, feature=0x%X\n",
           groups, groupSz, iter, secs, (unsigned)fl);
    fflush(stdout);

    LARGE_INTEGER f, t0, now;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);

    ctx->CSSetShader(cs, nullptr, 0);
    ID3D11Buffer* cbs[1] = { cb };
    ID3D11UnorderedAccessView* uavs[1] = { uav };
    ctx->CSSetConstantBuffers(0, 1, cbs);
    ctx->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);

    long long submits = 0;
    while (true) {
        QueryPerformanceCounter(&now);
        double elapsed = (double)(now.QuadPart - t0.QuadPart) / (double)f.QuadPart;
        if (elapsed >= secs) break;

        // 每 8 次提交刷新一次，避免命令队列无限增长，同时保持 GPU 满载
        for (int k = 0; k < 8; k++) {
            ctx->Dispatch((UINT)groups, 1, 1);
            submits++;
        }
        ctx->Flush();
    }

    // 等 GPU 收尾
    ctx->Flush();
    Sleep(150);

    QueryPerformanceCounter(&now);
    double total = (double)(now.QuadPart - t0.QuadPart) / (double)f.QuadPart;
    printf("[gpuload] done: %lld dispatches in %.1f s (%.0f/s)\n",
           submits, total, submits / (total > 0 ? total : 1));

    uav->Release();
    outBuf->Release();
    cb->Release();
    cs->Release();
    ctx->Release();
    dev->Release();
    return 0;
}
