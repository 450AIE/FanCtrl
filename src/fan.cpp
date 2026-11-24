// fan.cpp - 通过 WMI 读写联想风扇转速
//
// 目标类：root\WMI : LENOVO_OTHER_METHOD（ACPI\PNP0C14\GMZN）
//   GetFeatureValue(IDs: uint32) -> value: uint32
//   SetFeatureValue(IDs: uint32, value: uint32) -> bool
// 功能 ID：0x04030001 = 风扇1，0x04030004 = 风扇2
// 写 0 = 交还 EC 自动控制（实测有效）

#include "app.h"
#include <wbemidl.h>
#include <comdef.h>
#include <cstdarg>
#include <cstdio>
#include <cwchar>

#pragma comment(lib, "wbemuuid.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

static const uint32_t kFanIds[2] = { 0x04030001u, 0x04030004u };

static IWbemServices*    s_svc     = nullptr;
static IWbemClassObject* s_inGet   = nullptr;
static IWbemClassObject* s_outGet  = nullptr;
static IWbemClassObject* s_inSet   = nullptr;
static BSTR              s_objPath = nullptr;
static BSTR              s_relPath = nullptr;
static bool              s_useRel  = false;
static bool              s_useI4   = false;
static BSTR              s_mGet    = nullptr;
static BSTR              s_mSet    = nullptr;
static bool              s_comInit = false;

static void SafeRelease(IUnknown* p) { if (p) p->Release(); }

// 调试日志：与 exe 同目录 fan_debug.log
static void Dbg(const wchar_t* fmt, ...) {
#ifdef FANCTRL_DEBUG
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);
    wchar_t* slash = wcsrchr(path, L'\\');
    if (slash) *(slash + 1) = 0;
    wcscat_s(path, MAX_PATH, L"fan_debug.log");

    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"at, ccs=UTF-8") != 0 || !f) return;
    va_list ap;
    va_start(ap, fmt);
    vfwprintf(f, fmt, ap);
    va_end(ap);
    fwprintf(f, L"\n");
    fclose(f);
#else
    (void)fmt;
#endif
}

static bool GetUint32(IWbemClassObject* obj, const wchar_t* name, uint32_t* out) {
    VARIANT v;
    VariantInit(&v);
    if (FAILED(obj->Get(name, 0, &v, NULL, NULL))) { VariantClear(&v); return false; }
    bool ok = false;
    if (v.vt == VT_I4 || v.vt == VT_UI4 || v.vt == VT_INT || v.vt == VT_UINT) {
        *out = (uint32_t)v.ulVal;
        ok = true;
    } else if (v.vt == VT_I2 || v.vt == VT_UI2) {
        *out = (uint32_t)v.uiVal;
        ok = true;
    } else if (v.vt == VT_BSTR) {
        *out = wcstoul(v.bstrVal, NULL, 10);
        ok = true;
    }
    VariantClear(&v);
    return ok;
}

static void PutUint32(IWbemClassObject* obj, const wchar_t* name, uint32_t val) {
    VARIANT v;
    VariantInit(&v);
    if (s_useI4) {
        v.vt = VT_I4;
        v.lVal = (LONG)val;
    } else {
        v.vt = VT_UI4;
        v.ulVal = val;
    }
    HRESULT hr = obj->Put(name, 0, &v, 0);
    VariantClear(&v);
    if (FAILED(hr)) Dbg(L"[put] %s = %u failed hr=0x%08X", name, val, (unsigned)hr);
}

static bool CallGet(uint32_t id, uint32_t* value);
static bool CallSet(uint32_t id, uint32_t value);

bool FanInit() {
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (SUCCEEDED(hr)) s_comInit = true;

    IWbemLocator* loc = nullptr;
    hr = CoCreateInstance(CLSID_WbemLocator, NULL, CLSCTX_INPROC_SERVER,
                          IID_IWbemLocator, (void**)&loc);
    if (FAILED(hr)) { wcscpy_s(g_app.status, L"WMI 定位器创建失败"); return false; }

    BSTR ns = SysAllocString(L"ROOT\\WMI");
    hr = loc->ConnectServer(ns, NULL, NULL, NULL, 0, NULL, NULL, &s_svc);
    SysFreeString(ns);
    loc->Release();

    if (FAILED(hr) || !s_svc) { wcscpy_s(g_app.status, L"WMI 连接失败"); return false; }

    hr = CoSetProxyBlanket(s_svc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, NULL,
                           RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE,
                           NULL, EOAC_NONE);
    if (FAILED(hr)) { wcscpy_s(g_app.status, L"WMI 权限设置失败"); return false; }

    // 取类定义中的方法签名
    BSTR clsName = SysAllocString(L"LENOVO_OTHER_METHOD");
    IWbemClassObject* cls = nullptr;
    hr = s_svc->GetObjectW(clsName, 0, NULL, &cls, NULL);
    SysFreeString(clsName);
    if (FAILED(hr) || !cls) { wcscpy_s(g_app.status, L"未找到联想 WMI 类"); return false; }

    BSTR mGet = SysAllocString(L"GetFeatureValue");
    BSTR mSet = SysAllocString(L"SetFeatureValue");
    cls->GetMethod(mGet, 0, &s_inGet, &s_outGet);
    cls->GetMethod(mSet, 0, &s_inSet, NULL);
    SysFreeString(mGet);
    SysFreeString(mSet);
    cls->Release();

    if (!s_inGet || !s_inSet) { wcscpy_s(g_app.status, L"WMI 方法签名缺失"); return false; }

    // 取实例路径
    BSTR wql = SysAllocString(L"WQL");
    BSTR query = SysAllocString(L"SELECT * FROM LENOVO_OTHER_METHOD");
    IEnumWbemClassObject* en = nullptr;
    hr = s_svc->ExecQuery(wql, query, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
                          NULL, &en);
    SysFreeString(wql);
    SysFreeString(query);
    if (FAILED(hr) || !en) { wcscpy_s(g_app.status, L"查询风扇接口失败"); return false; }

    IWbemClassObject* inst = nullptr;
    ULONG got = 0;
    hr = en->Next(2000, 1, &inst, &got);
    en->Release();

    if (FAILED(hr) || got == 0 || !inst) { wcscpy_s(g_app.status, L"风扇接口实例不存在"); return false; }

    VARIANT vp;
    VariantInit(&vp);
    if (SUCCEEDED(inst->Get(L"__PATH", 0, &vp, NULL, NULL)) && vp.vt == VT_BSTR) {
        s_objPath = SysAllocString(vp.bstrVal);
        Dbg(L"[init] __PATH = %s", vp.bstrVal);
    }
    VariantClear(&vp);

    VariantInit(&vp);
    if (SUCCEEDED(inst->Get(L"__RELPATH", 0, &vp, NULL, NULL)) && vp.vt == VT_BSTR) {
        s_relPath = SysAllocString(vp.bstrVal);
        Dbg(L"[init] __RELPATH = %s", vp.bstrVal);
    }
    VariantClear(&vp);

    inst->Release();

    if (!s_objPath) { wcscpy_s(g_app.status, L"无法获取实例路径"); return false; }

    s_mGet = SysAllocString(L"GetFeatureValue");
    s_mSet = SysAllocString(L"SetFeatureValue");

    // 探测可用的调用组合（路径形式 × 参数类型）
    uint32_t probeVal = 0;
    bool found = false;
    for (int rel = 0; rel <= 1 && !found; rel++) {
        for (int i4 = 0; i4 <= 1 && !found; i4++) {
            s_useRel = (rel == 1);
            s_useI4 = (i4 == 1);
            Dbg(L"[probe] relPath=%d useI4=%d", rel, i4);
            if (CallGet(0x04030001u, &probeVal)) {
                Dbg(L"[probe] OK relPath=%d useI4=%d value=%u", rel, i4, probeVal);
                found = true;
            }
        }
    }
    if (!found) Dbg(L"[probe] all combinations failed");

    return true;
}

static bool CallGet(uint32_t id, uint32_t* value) {
    if (!s_svc || !s_inGet || !s_objPath) return false;

    IWbemClassObject* in = nullptr;
    if (FAILED(s_inGet->SpawnInstance(0, &in)) || !in) return false;
    PutUint32(in, L"IDs", id);

    IWbemClassObject* out = nullptr;
    HRESULT hr = s_svc->ExecMethod(s_useRel ? s_relPath : s_objPath, s_mGet, 0, NULL, in, &out, NULL);
    in->Release();
    if (FAILED(hr) || !out) {
        static int once = 0;
        if (once++ < 3) Dbg(L"[get] ExecMethod hr=0x%08X out=%p id=0x%08X", (unsigned)hr, (void*)out, id);
        return false;
    }

    uint32_t val = 0;
    bool ok = GetUint32(out, L"value", &val);
    out->Release();
    if (!ok) {
        static int once2 = 0;
        if (once2++ < 3) Dbg(L"[get] value property missing id=0x%08X", id);
    }
    if (ok && value) *value = val;
    return ok;
}

static bool CallSet(uint32_t id, uint32_t value) {
    if (!s_svc || !s_inSet || !s_objPath) return false;

    IWbemClassObject* in = nullptr;
    if (FAILED(s_inSet->SpawnInstance(0, &in)) || !in) return false;
    PutUint32(in, L"IDs", id);
    PutUint32(in, L"value", value);

    IWbemClassObject* out = nullptr;
    HRESULT hr = s_svc->ExecMethod(s_useRel ? s_relPath : s_objPath, s_mSet, 0, NULL, in, &out, NULL);
    in->Release();
    if (out) out->Release();
    return SUCCEEDED(hr);
}

int FanReadRpm(int index) {
    if (index < 0 || index > 1) return -1;
    uint32_t v = 0;
    if (!CallGet(kFanIds[index], &v)) return -1;
    return (int)v;
}

bool FanWriteRpm(int index, int rpm) {
    if (index < 0 || index > 1) return false;
    if (rpm < 0) rpm = 0;
    if (rpm > kRpmMax) rpm = kRpmMax;
    return CallSet(kFanIds[index], (uint32_t)rpm);
}

bool FanRelease() {
    bool a = CallSet(kFanIds[0], 0);
    bool b = CallSet(kFanIds[1], 0);
    return a || b;
}

void FanClose() {
    if (s_objPath) { SysFreeString(s_objPath); s_objPath = nullptr; }
    if (s_mGet) { SysFreeString(s_mGet); s_mGet = nullptr; }
    if (s_mSet) { SysFreeString(s_mSet); s_mSet = nullptr; }
    SafeRelease(s_inGet);
    SafeRelease(s_outGet);
    SafeRelease(s_inSet);
    if (s_svc) { s_svc->Release(); s_svc = nullptr; }
    s_inGet = s_outGet = s_inSet = nullptr;
    if (s_comInit) { CoUninitialize(); s_comInit = false; }
}
