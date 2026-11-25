/* meter: meet zelf, zonder gputaskman.
 * - Windows-tellers (PDH): videogeheugen en belasting per proces en per kaart ("GPU Engine", "GPU Process Memory",
 *   "GPU Adapter Memory"). Een ronde kost ongeveer 1 ms.
 * - NVML (nvml.dll): temperatuur, vermogen, klok, PCIe van de NVIDIA-kaart.
 * - Proceslijst (NtQuerySystemInformation): naam, RAM en CPU per proces. Dit is traag (5 ms) en loopt apart.
 * - Geheugen van een proces doorlopen: geladen bibliotheken (runtime) en gekoppelde modelbestanden.
 * De lijsten en regels hieronder (soort van een proces, runtime, modelbestanden, drempels) zijn eigen werk.
 * Het idee voor zo'n overzicht komt van gputaskman van Scott Hanselman. */
#ifndef UNICODE
#define UNICODE
#define _UNICODE
#endif
#define COBJMACROS
#include <windows.h>
#include <initguid.h>
#include <dxgi.h>
#include <dwmapi.h>
#include <pdh.h>
#include <psapi.h>
#include <winternl.h>
#include <x86intrin.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "meter.h"

#define MiB 1048576.0

/* ---------- kaarten (DXGI) ---------- */
typedef struct { LUID luid; int nvidia, intel, ingebouwd; char naam[96], driver[32]; double ded, gedeeld; } Kaart;
static Kaart kaart[MAXG];
static int nkaart;

static void zoek_kaarten(void)
{
    IDXGIFactory1 *f;
    IDXGIAdapter1 *a;
    UINT i;
    int j;
    if (FAILED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&f))) return;
    for (i = 0; nkaart < MAXG && IDXGIFactory1_EnumAdapters1(f, i, &a) == S_OK; i++) {
        DXGI_ADAPTER_DESC1 d;
        LARGE_INTEGER v;
        Kaart k = { 0 };
        IDXGIAdapter1_GetDesc1(a, &d);
        if (!(d.Flags & 2) && !(d.VendorId == 0x1414 && d.DeviceId == 0x8C)) {    /* geen software-kaart */
            k.luid = d.AdapterLuid;
            k.nvidia = d.VendorId == 0x10DE;
            k.ded = (double)d.DedicatedVideoMemory;
            k.gedeeld = (double)d.SharedSystemMemory;
            k.intel = d.VendorId == 0x8086;
            k.ingebouwd = (d.VendorId == 0x8086 || d.VendorId == 0x4D4F4351) && k.ded <= 512 * MiB && k.gedeeld > k.ded;
            WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, k.naam, sizeof k.naam, NULL, NULL);
            if (IDXGIAdapter1_CheckInterfaceSupport(a, &IID_IDXGIDevice, &v) == S_OK)
                snprintf(k.driver, sizeof k.driver, "%u.%u.%u.%u", HIWORD(v.HighPart), LOWORD(v.HighPart), HIWORD(v.LowPart), LOWORD(v.LowPart));
            for (j = nkaart; j > 0 && kaart[j - 1].ded < k.ded; j--) kaart[j] = kaart[j - 1];     /* meeste geheugen eerst */
            kaart[j] = k;
            nkaart++;
        }
        IDXGIAdapter1_Release(a);
    }
    IDXGIFactory1_Release(f);
}
static int kaart_van(unsigned hoog, unsigned laag)
{
    int i;
    for (i = 0; i < nkaart; i++) if ((unsigned)kaart[i].luid.HighPart == hoog && kaart[i].luid.LowPart == laag) return i;
    return -1;
}

/* ---------- NVML ---------- */
typedef int (*NvFn)(void *, ...);
static HMODULE nv;
static void *nvdev;
static NvFn nv_fn(const char *naam) { return nv ? (NvFn)GetProcAddress(nv, naam) : NULL; }
static NvFn nv_util, nv_temp, nv_w, nv_wmax, nv_klok, nv_klokmax, nv_pstate, nv_enc, nv_dec, nv_mem, nv_pcie, nv_gen, nv_breedte;
static char nv_driver[32], nv_cuda[16], nv_bus[24], nv_cc[12];
static double nv_sm_max = -1, nv_rx = -1, nv_tx = -1, nv_gen_w = -1, nv_breedte_w = -1;

static void nvml_start(void)
{
    NvFn f;
    int v = 0, a = 0, b = 0;
    struct { char oud[16]; unsigned domein, bus, apparaat, id, sub; char busid[32]; } pci;
    nv = LoadLibraryW(L"nvml.dll");
    if (!nv) return;
    f = nv_fn("nvmlInit_v2");
    if (!f || ((int (*)(void))f)()) { nv = NULL; return; }
    f = nv_fn("nvmlDeviceGetHandleByIndex_v2");
    if (!f || ((int (*)(unsigned, void **))f)(0, &nvdev)) { nv = NULL; return; }
    nv_util = nv_fn("nvmlDeviceGetUtilizationRates"); nv_temp = nv_fn("nvmlDeviceGetTemperature");
    nv_w = nv_fn("nvmlDeviceGetPowerUsage"); nv_wmax = nv_fn("nvmlDeviceGetEnforcedPowerLimit");
    nv_klok = nv_fn("nvmlDeviceGetClockInfo"); nv_klokmax = nv_fn("nvmlDeviceGetMaxClockInfo");
    nv_pstate = nv_fn("nvmlDeviceGetPerformanceState"); nv_enc = nv_fn("nvmlDeviceGetEncoderUtilization");
    nv_dec = nv_fn("nvmlDeviceGetDecoderUtilization"); nv_mem = nv_fn("nvmlDeviceGetMemoryInfo");
    nv_pcie = nv_fn("nvmlDeviceGetPcieThroughput"); nv_gen = nv_fn("nvmlDeviceGetCurrPcieLinkGeneration");
    nv_breedte = nv_fn("nvmlDeviceGetCurrPcieLinkWidth");
    if ((f = nv_fn("nvmlSystemGetDriverVersion")) != NULL) ((int (*)(char *, unsigned))f)(nv_driver, sizeof nv_driver);
    if ((f = nv_fn("nvmlSystemGetCudaDriverVersion")) != NULL && !((int (*)(int *))f)(&v)) snprintf(nv_cuda, sizeof nv_cuda, "%d.%d", v / 1000, v % 1000 / 10);
    if ((f = nv_fn("nvmlDeviceGetPciInfo_v3")) != NULL && !f(nvdev, &pci)) snprintf(nv_bus, sizeof nv_bus, "%08X:%02X:%02X.0", pci.domein, pci.bus, pci.apparaat);
    if ((f = nv_fn("nvmlDeviceGetCudaComputeCapability")) != NULL && !f(nvdev, &a, &b)) snprintf(nv_cc, sizeof nv_cc, "%d.%d", a, b);
    if (nv_klokmax) { unsigned u; if (!nv_klokmax(nvdev, 1, &u)) nv_sm_max = u; }
}
static double nv_getal(NvFn f, int soort)   /* soort < 0: geen extra argument */
{
    unsigned u[2] = { 0, 0 };
    if (!f) return -1;
    if (soort < 0 ? f(nvdev, u, u + 1) : f(nvdev, soort, u)) return -1;
    return u[0];
}

/* ---------- Intel (IGCL: ControlLib.dll, onderdeel van het Intel-stuurprogramma) ----------
 * Gemeten op een UHD in een laptop: de energieteller volgt de belasting van de processor (43 W rustig, 125 W met
 * 20 draden vol). Het is dus het vermogen van de hele processorchip, niet van de kaart alleen. Intel geeft daar geen
 * temperatuur. De opbouw van de gegevens staat in igcl_api.h van Intel; hier staan alleen de plekken die nodig zijn:
 * een waarde is 24 bytes (ondersteund@0, soort@8, waarde@16); in de telemetrie (1024 bytes) staan tijd@8, energie@32,
 * spanning@56 en klok@80. */
static void *ig_dev;
static int (*ig_tele)(void *, void *);
static double ig_w = -1, ig_klok = -1, ig_volt = -1;

static double ig_getal(const BYTE *w)
{
    int soort = *(const int *)(w + 8);
    if (!w[0]) return -1;
    return soort == 9 ? *(const double *)(w + 16) : soort == 8 ? *(const float *)(w + 16)
         : soort == 6 || soort == 7 ? (double)*(const LONGLONG *)(w + 16) : (double)*(const int *)(w + 16);
}
static void igcl_start(void)
{
    HMODULE m = LoadLibraryExW(L"ControlLib.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    int (*init)(void *, void **), (*lijst)(void *, unsigned *, void **);
    struct { unsigned maat; unsigned char versie; unsigned app, vlaggen, ondersteund; unsigned char uid[16]; } a = { 0 };
    void *api = NULL, *dev[8];
    unsigned n = 0;
    if (!m) return;
    init = (void *)GetProcAddress(m, "ctlInit");
    lijst = (void *)GetProcAddress(m, "ctlEnumerateDevices");
    ig_tele = (void *)GetProcAddress(m, "ctlPowerTelemetryGet");
    if (!init || !lijst || !ig_tele) return;
    a.maat = sizeof a; a.app = 0x10001; a.vlaggen = 1;      /* versie 1.1, met Level Zero (nodig voor telemetrie) */
    if (init(&a, &api) || lijst(api, &n, NULL) || !n) return;
    if (n > 8) n = 8;
    if (lijst(api, &n, dev)) return;
    ig_dev = dev[0];
}
static void igcl_meet(double nu_ms)    /* vier keer per seconde is genoeg; vermogen = energie per tijd */
{
    static BYTE t[1024];
    static double e0 = -1, t0, ms0 = -1e9;
    double e, ts;
    if (!ig_dev || nu_ms - ms0 < 250) return;
    memset(t, 0, sizeof t);
    *(unsigned *)t = sizeof t; t[4] = 1;
    if (ig_tele(ig_dev, t)) { ms0 = nu_ms; return; }
    ig_volt = ig_getal(t + 56); ig_klok = ig_getal(t + 80);
    ts = ig_getal(t + 8); e = ig_getal(t + 32);
    if (e >= 0 && e0 >= 0 && ts > t0) ig_w = (e - e0) / (ts - t0);
    e0 = e; t0 = ts; ms0 = nu_ms;
}

/* ---------- processen: naam, RAM, CPU, runtime, modellen (trage draad) ---------- */
/* Runtime: welke bibliotheek voor de kaart een proces geladen heeft. De volgorde hier is de volgorde in het venster:
 * eerst AI, dan rekenen, dan beeld, dan video. De bestandsnamen staan in de documentatie van de makers
 * (Microsoft, Khronos, NVIDIA, AMD, en de projecten zelf). */
enum { R_LLAMA, R_WHISPER, R_GGML, R_TORCH, R_TRT, R_ONNX, R_DML, R_CUDNN, R_CUDA, R_HIP, R_OPENCL,
       R_D3D12, R_VULKAN, R_OPENGL, R_D3D11, R_D3D9, R_NVENC, R_NVDEC, R_N };
static const char *r_naam[R_N] = { "llama.cpp", "whisper.cpp", "ggml", "PyTorch", "TensorRT", "ONNX RT", "DirectML", "cuDNN", "CUDA", "HIP", "OpenCL",
                                   "D3D12", "Vulkan", "OpenGL", "D3D11", "D3D9", "NVENC", "NVDEC" };
static const struct { int r; const char *patroon; } r_dll[] = {
    /* AI */
    { R_LLAMA, "llama.dll" }, { R_LLAMA, "libllama*.dll" }, { R_LLAMA, "llama-*.dll" }, { R_WHISPER, "whisper*.dll" },
    { R_WHISPER, "libwhisper*.dll" }, { R_GGML, "ggml*.dll" }, { R_TORCH, "torch_cuda*.dll" }, { R_TORCH, "torch_cpu.dll" },
    { R_TORCH, "c10_cuda.dll" }, { R_TRT, "nvinfer*.dll" }, { R_ONNX, "onnxruntime*.dll" }, { R_DML, "directml.dll" },
    { R_CUDNN, "cudnn*.dll" },
    /* rekenen */
    { R_CUDA, "nvcuda.dll" }, { R_CUDA, "nvcuda64.dll" }, { R_CUDA, "cudart64_*.dll" }, { R_CUDA, "cublas64_*.dll" },
    { R_HIP, "amdhip64*.dll" }, { R_OPENCL, "opencl.dll" },
    /* beeld */
    { R_D3D12, "d3d12core.dll" }, { R_VULKAN, "vulkan-1.dll" }, { R_OPENGL, "opengl32.dll" }, { R_D3D11, "d3d11.dll" },
    { R_D3D9, "d3d9.dll" },
    /* video */
    { R_NVENC, "nvencodeapi64.dll" }, { R_NVDEC, "nvcuvid.dll" }, { R_NVDEC, "nvcuvid64.dll" } };
#define AI_RUNTIMES (1u << R_LLAMA | 1u << R_WHISPER | 1u << R_GGML | 1u << R_TORCH | 1u << R_TRT | 1u << R_ONNX | 1u << R_DML | 1u << R_CUDNN)

typedef struct {
    int pid, gezien, model_soort;       /* model_soort: 0 geen, 1 model, 2 gguf */
    char naam[64], model[96];
    double rss, cpu;
    ULONGLONG cputijd;
    unsigned runtimes;
    DWORD scan_tik, gpu_tik;            /* gpu_tik: laatste keer dat het proces een kaart gebruikte */
} PInfo;
static CRITICAL_SECTION pslot;
static PInfo pinfo[4096];
static int npinfo;
static Top t_cpu[MAXTOP], t_ram[MAXTOP];
static int nt_cpu, nt_ram;

static PInfo *pinfo_van(int pid)
{
    int i;
    for (i = 0; i < npinfo; i++) if (pinfo[i].pid == pid) return &pinfo[i];
    return NULL;
}

static int past(const char *patroon, const char *s)    /* patroon met * */
{
    for (; *patroon; patroon++, s++) {
        if (*patroon == '*') {
            for (patroon++; ; s++) { if (past(patroon, s)) return 1; if (!*s) return 0; }
        }
        if (*patroon != *s) return 0;
    }
    return !*s;
}

/* Doorloopt het geheugen van een proces: elk gekoppeld bestand een keer. Geeft de runtimes (bits) en modellen. */
static unsigned scan(int pid, char *model, size_t nm, int *model_soort)
{
    static const char *model_ext[] = { ".gguf", ".ggml", ".safetensors", ".onnx", ".engine", ".plan", ".tflite", ".bin", ".pt", ".pth", ".ckpt" };
    HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, (DWORD)pid);
    MEMORY_BASIC_INFORMATION mbi;
    unsigned char *adres = NULL;
    unsigned bits = 0;
    int modellen = 0, stappen = 0;
    size_t n = 0;
    model[0] = 0; *model_soort = 0;
    if (!h) return 0;
    while (VirtualQueryEx(h, adres, &mbi, sizeof mbi) == sizeof mbi && ++stappen < 400000) {
        if ((mbi.Type == MEM_IMAGE || mbi.Type == MEM_MAPPED) && mbi.AllocationBase == mbi.BaseAddress) {
            wchar_t pad[1024];
            char laag[512], *basis, *punt;
            DWORD len = GetMappedFileNameW(h, mbi.BaseAddress, pad, 1000);
            unsigned i;
            if (len) {
                WideCharToMultiByte(CP_UTF8, 0, pad, -1, laag, sizeof laag, NULL, NULL);
                basis = strrchr(laag, '\\');
                basis = basis ? basis + 1 : laag;
                punt = strrchr(basis, '.');
                if (mbi.Type == MEM_IMAGE) {
                    char klein[256];
                    for (i = 0; basis[i] && i < 255; i++) klein[i] = (char)(basis[i] >= 'A' && basis[i] <= 'Z' ? basis[i] + 32 : basis[i]);
                    klein[i] = 0;
                    for (i = 0; i < sizeof r_dll / sizeof r_dll[0]; i++) if (past(r_dll[i].patroon, klein)) bits |= 1u << r_dll[i].r;
                } else if (punt) {
                    for (i = 0; i < sizeof model_ext / sizeof model_ext[0] && _stricmp(punt, model_ext[i]); i++) ;
                    if (i < sizeof model_ext / sizeof model_ext[0]) {
                        wchar_t vol[1100];
                        WIN32_FILE_ATTRIBUTE_DATA fa;
                        double maat = 0;
                        swprintf(vol, 1100, L"\\\\?\\GLOBALROOT%ls", pad);
                        if (GetFileAttributesExW(vol, GetFileExInfoStandard, &fa)) maat = (double)(((ULONGLONG)fa.nFileSizeHigh << 32) | fa.nFileSizeLow);
                        if (maat >= (i >= 7 ? 128 : 16) * MiB) {    /* een model is groot: vanaf 16 MB; bij algemene namen (.bin .pt .pth .ckpt) vanaf 128 MB */
                            if (!*model_soort || i == 0) *model_soort = i == 0 ? 2 : 1;
                            if (modellen++ < 2 && n < nm) {
                                n += (size_t)(maat >= 1024 * MiB ? snprintf(model + n, nm - n, "%s%s %.2f GB", n ? ", " : "", basis, maat / (1024 * MiB))
                                                                  : snprintf(model + n, nm - n, "%s%s %.0f MB", n ? ", " : "", basis, maat / MiB));
                                if (n >= nm) n = nm - 1;
                            }
                        }
                    }
                }
            }
        }
        adres = (unsigned char *)mbi.BaseAddress + mbi.RegionSize;
        if (adres < (unsigned char *)mbi.BaseAddress) break;
    }
    CloseHandle(h);
    return bits;
}

static void runtime_tekst(unsigned bits, char *uit, size_t n)
{
    int i, k = 0;
    size_t m = 0;
    uit[0] = 0;
    for (i = 0; i < R_N && k < 3; i++) if (bits & (1u << i)) {
        m += (size_t)snprintf(uit + m, n - m, m ? " %s" : "%s", r_naam[i]);
        if (m >= n) break;
        k++;
    }
}

void meter_scan_pid(int pid, char *runtime, size_t nr, char *model, size_t nm)
{
    int soort;
    runtime_tekst(scan(pid, model, nm, &soort), runtime, nr);
}

static void top_in(Top *t, int *n, const PInfo *p, int op_cpu)
{
    double w = op_cpu ? p->cpu : p->rss;
    int i = *n;
    if (i == MAXTOP && w <= (op_cpu ? t[i - 1].cpu : t[i - 1].rss)) return;
    if (i < MAXTOP) (*n)++; else i--;
    for (; i > 0 && (op_cpu ? t[i - 1].cpu : t[i - 1].rss) < w; i--) t[i] = t[i - 1];
    strcpy(t[i].naam, p->naam); t[i].rss = p->rss; t[i].cpu = p->cpu;
}

static int ncpu = 1;
static DWORD WINAPI trage_draad(void *arg)
{
    static BYTE buf[8 << 20];
    ULONGLONG vorige = 0;
    (void)arg;
    for (;; Sleep(250)) {
        ULONG len;
        BYTE *e = buf;
        FILETIME ft;
        ULONGLONG nu, dt;
        Top tc[MAXTOP], tr[MAXTOP];
        int ntc = 0, ntr = 0, i, j, kandidaat[8], nk = 0;
        DWORD tik = GetTickCount();
        if (NtQuerySystemInformation(SystemProcessInformation, buf, sizeof buf, &len)) continue;
        GetSystemTimeAsFileTime(&ft);
        nu = ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
        dt = vorige ? nu - vorige : 0;
        vorige = nu;
        EnterCriticalSection(&pslot);
        for (i = 0; i < npinfo; i++) pinfo[i].gezien = 0;
        for (;;) {      /* vaste plekken in SYSTEM_PROCESS_INFORMATION (x64) */
            int pid = (int)*(ULONG_PTR *)(e + 80);
            ULONGLONG tijd = *(ULONGLONG *)(e + 40) + *(ULONGLONG *)(e + 48);
            USHORT nlen = *(USHORT *)(e + 56);
            wchar_t *naam = *(wchar_t **)(e + 64);
            PInfo *p = pinfo_van(pid);
            if (!p && npinfo < (int)(sizeof pinfo / sizeof pinfo[0])) {
                p = &pinfo[npinfo++];
                memset(p, 0, sizeof *p);
                p->pid = pid;
                p->cputijd = tijd;
                if (pid == 0) strcpy(p->naam, "Idle");
                else if (!naam || !nlen) strcpy(p->naam, "System");
                else p->naam[WideCharToMultiByte(CP_UTF8, 0, naam, nlen / 2, p->naam, sizeof p->naam - 1, NULL, NULL)] = 0;
            }
            if (p) {
                p->gezien = 1;
                p->rss = (double)*(SIZE_T *)(e + 144);
                p->cpu = dt && tijd >= p->cputijd ? 100.0 * (double)(tijd - p->cputijd) / (double)dt / ncpu : 0;
                p->cputijd = tijd;
                if (pid) { top_in(tc, &ntc, p, 1); top_in(tr, &ntr, p, 0); }
            }
            if (!*(ULONG *)e) break;
            e += *(ULONG *)e;
        }
        for (i = j = 0; i < npinfo; i++) if (pinfo[i].gezien) pinfo[j++] = pinfo[i];
        npinfo = j;
        memcpy(t_cpu, tc, sizeof tc); memcpy(t_ram, tr, sizeof tr); nt_cpu = ntc; nt_ram = ntr;
        for (i = 0; i < npinfo && nk < 2; i++)      /* processen die een kaart gebruiken: af en toe doorlopen */
            if (pinfo[i].pid > 4 && tik - pinfo[i].gpu_tik < 3000 && (!pinfo[i].scan_tik || tik - pinfo[i].scan_tik > 30000)) {
                kandidaat[nk++] = pinfo[i].pid;
                pinfo[i].scan_tik = tik | 1;
            }
        LeaveCriticalSection(&pslot);
        for (i = 0; i < nk; i++) {
            char model[96];
            int soort;
            unsigned bits = scan(kandidaat[i], model, sizeof model, &soort);
            PInfo *p;
            EnterCriticalSection(&pslot);
            if ((p = pinfo_van(kandidaat[i])) != NULL) { p->runtimes = bits; p->model_soort = soort; strcpy(p->model, model); }
            LeaveCriticalSection(&pslot);
        }
        if (nv && nv_pcie) {      /* PCIe-doorvoer: elke aanroep kost 30 ms, dus hier */
            double tx = nv_getal(nv_pcie, 0), rx = nv_getal(nv_pcie, 1);
            nv_gen_w = nv_getal(nv_gen, -1); nv_breedte_w = nv_getal(nv_breedte, -1);
            nv_tx = tx; nv_rx = rx;
        }
    }
    return 0;
}

/* ---------- soort van een proces ----------
 * De sleutels zijn vaste Engelse woorden; het venster vertaalt ze en zet er de uitleg bij.
 * Volgorde: eerst wat zeker is (kern, Windows zelf), dan wat het proces geladen heeft, dan wat het nu doet. */
static int bevat(const char *s, const char *deel)      /* zonder verschil tussen hoofd- en kleine letters */
{
    size_t n = strlen(deel);
    for (; *s; s++) if (!_strnicmp(s, deel, n)) return 1;
    return 0;
}
static const char *soort_van(int pid, const char *naam, unsigned rt, int model_soort, const double *eng)
{
    /* onderdelen van Windows zelf */
    static const char *windows[] = { "csrss.exe", "winlogon.exe", "lsass.exe", "svchost.exe", "wininit.exe", "services.exe", "smss.exe",
                                     "fontdrvhost.exe", "LogonUI.exe", "sihost.exe" };
    /* programma's die een taalmodel draaien: een deel van de naam is genoeg */
    static const char *taalmodel[] = { "ollama", "llama", "lmstudio", "lm studio", "koboldcpp", "gpt4all", "text-generation" };
    /* browsers, en de ingebouwde browser van Windows-apps (WebView2) */
    static const char *browser[] = { "chrome.exe", "msedge.exe", "msedgewebview2.exe", "firefox.exe", "brave.exe", "opera.exe", "vivaldi.exe",
                                     "arc.exe", "chromium.exe", "librewolf.exe", "waterfox.exe", "zen.exe", "thorium.exe" };
    int i, video = eng[2] > 1, rekent = eng[3] > 1;
    if (pid == 4) return "kernel";
    if (!_stricmp(naam, "dwm.exe")) return "compositor";
    for (i = 0; i < (int)(sizeof windows / sizeof windows[0]); i++) if (!_stricmp(naam, windows[i])) return "system";
    if (model_soort == 2 || (rt & (1u << R_LLAMA)) || !_stricmp(naam, "jan.exe")) return "LLM";
    for (i = 0; i < (int)(sizeof taalmodel / sizeof taalmodel[0]); i++) if (bevat(naam, taalmodel[i])) return "LLM";
    if ((model_soort || (rt & AI_RUNTIMES)) && !video) return "AI";
    for (i = 0; i < (int)(sizeof browser / sizeof browser[0]); i++) if (!_stricmp(naam, browser[i])) return "browser";
    if (video || (rt & (1u << R_NVENC))) return "video";
    if (rekent || (rt & (1u << R_CUDA | 1u << R_HIP))) return "compute";
    if ((rt & (1u << R_D3D12 | 1u << R_VULKAN)) && eng[0] > 15) return "3D/game";
    return "app";
}

/* ---------- de snelle meting ---------- */
static CRITICAL_SECTION bslot;
static Beeld beeld;
static HWND doel;
static UINT doel_bericht;
static volatile LONG interval = 1000, wacht_op_tekenen;
static PDH_HQUERY vraag;
static PDH_HCOUNTER t_eng, t_ded, t_shr, t_com, t_aded, t_ashr, t_wissel;
static BYTE *pdh_buf;
#define PDH_BUF (8 << 20)

typedef struct { int pid, gpu; double ded, shr, com, eng[5]; } Rij;     /* eng: 3d, copy, video, reken, overig */
static Rij rij[2048];
static int nrij;
static Rij *rij_van(int pid, int gpu)
{
    int i;
    for (i = 0; i < nrij; i++) if (rij[i].pid == pid && rij[i].gpu == gpu) return &rij[i];
    if (nrij == (int)(sizeof rij / sizeof rij[0])) return NULL;
    memset(&rij[nrij], 0, sizeof rij[0]);
    rij[nrij].pid = pid; rij[nrij].gpu = gpu;
    return &rij[nrij++];
}
/* "pid_1_luid_0x0_0x1_phys_0_eng_2_engtype_3D" met de hand ontleden (swscanf is hier te traag: 1000 namen per ronde). */
static int ontleed_naam(const wchar_t *s, int *pid, unsigned *hoog, unsigned *laag, int *phys, int *eng, const wchar_t **soort)
{
    wchar_t *e;
    *pid = -1; *eng = -1; *soort = L"";
    if (!wcsncmp(s, L"pid_", 4)) { *pid = (int)wcstol(s + 4, &e, 10); s = e; if (*s == 95) s++; }
    if (wcsncmp(s, L"luid_0x", 7)) return 0;
    *hoog = wcstoul(s + 7, &e, 16); s = e;
    if (wcsncmp(s, L"_0x", 3)) return 0;
    *laag = wcstoul(s + 3, &e, 16); s = e;
    if (wcsncmp(s, L"_phys_", 6)) return 0;
    *phys = (int)wcstol(s + 6, &e, 10); s = e;
    if (!wcsncmp(s, L"_eng_", 5)) { *eng = (int)wcstol(s + 5, &e, 10); s = e; if (!wcsncmp(s, L"_engtype_", 9)) *soort = s + 9; }
    return 1;
}
static PDH_FMT_COUNTERVALUE_ITEM_W *lees(PDH_HCOUNTER t, DWORD *n)
{
    DWORD sz = PDH_BUF;
    *n = 0;
    if (!t || PdhGetFormattedCounterArrayW(t, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &sz, n, (void *)pdh_buf)) *n = 0;
    return (void *)pdh_buf;
}
static double ms_nu(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER t;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return 1000.0 * (double)t.QuadPart / (double)f.QuadPart;
}

static void meet(Gpu *uit, Sys *s)
{
    static double eu[MAXG][128];
    static ULONGLONG vorig_idle[128], vorig_totaal[128];
    static BYTE cpubuf[128 * 48];
    PDH_FMT_COUNTERVALUE_ITEM_W *it;
    PDH_FMT_COUNTERVALUE w;
    MEMORYSTATUSEX ms = { sizeof ms };
    double aded[MAXG] = { 0 }, ashr[MAXG] = { 0 }, som_ded[MAXG] = { 0 };
    DWORD n, i, tik = GetTickCount();
    ULONG len;
    int g, pid, phys, e, j;
    unsigned hoog, laag;
    const wchar_t *soort;
    static double cpu_tijd;
    static int cyclus_ok;
    double nu_ms = ms_nu();

    PdhCollectQueryData(vraag);
    igcl_meet(nu_ms);
    nrij = 0;
    memset(eu, 0, sizeof eu);
    for (it = lees(t_eng, &n), i = 0; i < n; i++) {
        Rij *r;
        if (!ontleed_naam(it[i].szName, &pid, &hoog, &laag, &phys, &e, &soort) || pid < 0 || e < 0) continue;
        if ((g = kaart_van(hoog, laag)) < 0) continue;
        eu[g][(phys * 32 + e) & 127] += it[i].FmtValue.doubleValue;
        if (it[i].FmtValue.doubleValue <= 0 || !(r = rij_van(pid, g))) continue;
        r->eng[!_wcsnicmp(soort, L"3D", 2) ? 0 : !_wcsnicmp(soort, L"Copy", 4) ? 1
               : !_wcsnicmp(soort, L"VideoDecode", 11) || !_wcsnicmp(soort, L"VideoEncode", 11) ? 2
               : !_wcsnicmp(soort, L"Compute", 7) || !_wcsnicmp(soort, L"Cuda", 4) ? 3 : 4] += it[i].FmtValue.doubleValue;
    }
    for (j = 0; j < 3; j++)
        for (it = lees(j == 0 ? t_ded : j == 1 ? t_shr : t_com, &n), i = 0; i < n; i++) {
            Rij *r;
            if (!ontleed_naam(it[i].szName, &pid, &hoog, &laag, &phys, &e, &soort) || pid < 0) continue;
            if ((g = kaart_van(hoog, laag)) < 0 || it[i].FmtValue.doubleValue <= 0 || !(r = rij_van(pid, g))) continue;
            *(j == 0 ? &r->ded : j == 1 ? &r->shr : &r->com) += it[i].FmtValue.doubleValue;
        }
    for (j = 0; j < 2; j++)
        for (it = lees(j ? t_ashr : t_aded, &n), i = 0; i < n; i++) {
            if (!ontleed_naam(it[i].szName, &pid, &hoog, &laag, &phys, &e, &soort) || (g = kaart_van(hoog, laag)) < 0) continue;
            (j ? ashr : aded)[g] += it[i].FmtValue.doubleValue;
        }

    for (g = 0; g < nkaart; g++) {
        Gpu *q = &uit[g];
        double max = 0;
        memset(q, 0, sizeof *q);
        q->geldig = 1;
        q->nvidia = kaart[g].nvidia && nv;
        strcpy(q->naam, kaart[g].naam);
        strcpy(q->driver, kaart[g].driver);
        for (j = 0; j < 128; j++) { double v = eu[g][j] > 100 ? 100 : eu[g][j]; if (v > max) max = v; }
        q->util = max;
        q->vram_tot = kaart[g].ded;
        q->vram_gebr = aded[g];
        q->gedeeld = ashr[g];
        q->util_nvml = q->util_mem = q->temp = q->w = q->w_max = q->pstate = q->sm = q->sm_max = q->memklok = q->videoklok = -1;
        q->rx = q->tx = q->gen = q->breedte = q->enc = q->dec = -1;
        if (kaart[g].intel && ig_dev) { q->w = ig_w; q->w_chip = 1; q->sm = ig_klok; q->volt = ig_volt; }
        if (q->nvidia) {
            unsigned u[2] = { 0, 0 };
            ULONGLONG m[3] = { 0, 0, 0 };
            strcpy(q->driver, nv_driver); strcpy(q->cuda, nv_cuda); strcpy(q->bus, nv_bus); strcpy(q->cc, nv_cc);
            strcpy(q->model, "WDDM");
            if (nv_util && !nv_util(nvdev, u)) { q->util_nvml = u[0]; q->util_mem = u[1]; }
            q->temp = nv_getal(nv_temp, 0);
            if ((q->w = nv_getal(nv_w, -1)) >= 0) q->w /= 1000;
            if ((q->w_max = nv_getal(nv_wmax, -1)) >= 0) q->w_max /= 1000;
            q->pstate = nv_getal(nv_pstate, -1);
            if (q->pstate > 15) q->pstate = -1;
            q->sm = nv_getal(nv_klok, 1); q->memklok = nv_getal(nv_klok, 2); q->videoklok = nv_getal(nv_klok, 3);
            q->sm_max = nv_sm_max;
            q->enc = nv_getal(nv_enc, -1); q->dec = nv_getal(nv_dec, -1);
            q->rx = nv_rx; q->tx = nv_tx; q->gen = nv_gen_w; q->breedte = nv_breedte_w;
            if (nv_mem && !nv_mem(nvdev, m) && m[0]) q->vram_tot = (double)m[0];
        }
    }

    EnterCriticalSection(&pslot);
    for (j = 0; j < nrij; j++) {
        Rij *r = &rij[j];
        Gpu *q = &uit[r->gpu];
        PInfo *pi = pinfo_van(r->pid);
        Proc *p;
        double paged = r->com - r->ded - r->shr, pct = 0;
        const char *naam = pi ? pi->naam : "", *soort_tekst;
        if (paged < 0) paged = 0;
        for (e = 0; e < 5; e++) if (r->eng[e] > pct) pct = r->eng[e];
        if (pct > 1000) { pct = 0; memset(r->eng, 0, sizeof r->eng); }
        som_ded[r->gpu] += r->ded;
        if (kaart[r->gpu].ingebouwd) { r->shr += r->ded; r->ded = 0; }     /* ingebouwde kaart: alles is gedeeld geheugen */
        /* In de lijst: wie nu geheugen op de kaart heeft (vanaf 1 MB eigen of 8 MB gedeeld) of de kaart nu belast (vanaf 1 %). */
        if (!(r->ded >= MiB || r->shr >= 8 * MiB || pct >= 1) || q->np == MAXP) continue;
        p = &q->p[q->np++];
        memset(p, 0, sizeof *p);
        p->pid = r->pid; p->gpu = r->gpu;
        if (pi) { strcpy(p->naam, pi->naam); p->ram = pi->rss; p->cpu = pi->cpu; pi->gpu_tik = tik | 1; runtime_tekst(pi->runtimes, p->runtime, sizeof p->runtime); }
        else snprintf(p->naam, sizeof p->naam, "pid %d", r->pid);
        p->vram = r->ded; p->gedeeld = r->shr; p->paged = paged; p->pct = pct;
        p->e3d = r->eng[0]; p->ecopy = r->eng[1]; p->evideo = r->eng[2]; p->ereken = r->eng[3];
        soort_tekst = soort_van(r->pid, naam, pi ? pi->runtimes : 0, pi ? pi->model_soort : 0, r->eng);
        strcpy(p->soort, soort_tekst);
        if (pi && pi->model[0]) strcpy(p->binnen, pi->model);     /* het geladen modelbestand; de uitleg per soort zet het venster erbij */
        if (p->vram > 0) {      /* videogeheugen per soort, voor de balk */
            for (e = 0; e < q->ns && strcmp(q->snaam[e], p->soort); e++) ;
            if (e == q->ns && q->ns < NSOORT) { strcpy(q->snaam[e], p->soort); q->ns++; }
            if (e < q->ns) q->svram[e] += p->vram;
        }
    }
    memcpy(s->tcpu, t_cpu, sizeof t_cpu); memcpy(s->tram, t_ram, sizeof t_ram); s->ntcpu = nt_cpu; s->ntram = nt_ram;
    LeaveCriticalSection(&pslot);
    for (g = 0; g < nkaart; g++) uit[g].toegewezen = som_ded[g];

    /* systeem */
    /* CPU per draad uit de cycli van de rust-draad (QueryIdleProcessorCycleTime) tegen de klok van de processor (rdtsc).
     * Dat is precies op elke tijdschaal. Elke meting een nieuwe waarde, over de laatste 0,1 s (glijdend). */
    {
        static ULONG64 r_idle[32][128], r_tsc[32];
        static double r_ms[32];
        static int r_n, r_pos;
        ULONG64 idle[128], tsc = __rdtsc();
        ULONG lenb = sizeof idle;
        if (QueryIdleProcessorCycleTime(&lenb, idle)) {
            int nk = (int)(lenb / 8), oud = -1, a;
            for (a = 1; a <= r_n; a++) { oud = (r_pos - a + 32) % 32; if (nu_ms - r_ms[oud] >= 100) break; }
            if (oud >= 0 && tsc > r_tsc[oud]) {
                double dt = (double)(tsc - r_tsc[oud]);
                s->nkern = 0; s->cpu = 0;
                for (j = 0; j < nk && j < 128; j++) {
                    double v = 100.0 * (1.0 - (double)(idle[j] - r_idle[oud][j]) / dt);
                    s->kern[j] = v < 0 ? 0 : v > 100 ? 100 : v;
                    s->cpu += s->kern[j];
                    s->nkern++;
                }
                if (s->nkern) s->cpu /= s->nkern;
                cyclus_ok = 1;
            }
            memcpy(r_idle[r_pos], idle, sizeof idle); r_tsc[r_pos] = tsc; r_ms[r_pos] = nu_ms;
            r_pos = (r_pos + 1) % 32;
            if (r_n < 32) r_n++;
        }
    }
    /* Reserve als dat niet werkt: de gewone tijden, over minstens 0,2 s (Windows telt die in stappen van 15,6 ms). */
    if (!cyclus_ok && nu_ms - cpu_tijd >= 200 && (cpu_tijd = nu_ms, s->nkern = 0, s->cpu = 0, 1) && !NtQuerySystemInformation(SystemProcessorPerformanceInformation, cpubuf, sizeof cpubuf, &len)) {
        int nk = (int)(len / 48);
        for (j = 0; j < nk && j < 128; j++) {
            ULONGLONG idle = *(ULONGLONG *)(cpubuf + j * 48), tot = *(ULONGLONG *)(cpubuf + j * 48 + 8) + *(ULONGLONG *)(cpubuf + j * 48 + 16);
            ULONGLONG di = idle - vorig_idle[j], dt = tot - vorig_totaal[j];
            s->kern[j] = dt && vorig_totaal[j] && dt >= di ? 100.0 * (double)(dt - di) / (double)dt : 0;
            vorig_idle[j] = idle; vorig_totaal[j] = tot;
            s->cpu += s->kern[j];
            s->nkern++;
        }
        if (s->nkern) s->cpu /= s->nkern;
    }
    GlobalMemoryStatusEx(&ms);
    s->ram_tot = (double)ms.ullTotalPhys;
    s->ram_gebr = (double)(ms.ullTotalPhys - ms.ullAvailPhys);
    s->wissel_tot = (double)(ms.ullTotalPageFile > ms.ullTotalPhys ? ms.ullTotalPageFile - ms.ullTotalPhys : 0);
    s->wissel_gebr = t_wissel && !PdhGetFormattedCounterValue(t_wissel, PDH_FMT_DOUBLE, NULL, &w) ? s->wissel_tot * w.doubleValue / 100 : 0;
    s->logisch = ncpu;
}

static DWORD WINAPI snelle_draad(void *arg)
{
    static Gpu g[MAXG];
    static Sys s;
    HANDLE klok = CreateWaitableTimerExW(NULL, NULL, 2 /* hoge resolutie */, TIMER_ALL_ACCESS);
    double volgende = ms_nu(), venster_start = volgende, kosten = 0;
    int i, fysiek = 0, geteld = 0;
    LONG vorige_interval = interval;
    DWORD len = 0;
    (void)arg;
    if (!klok) klok = CreateWaitableTimerW(NULL, TRUE, NULL);
    GetLogicalProcessorInformationEx(RelationProcessorCore, NULL, &len);
    if (len) {
        BYTE *b = malloc(len), *e = b;
        if (GetLogicalProcessorInformationEx(RelationProcessorCore, (void *)b, &len))
            for (; e < b + len; e += ((SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *)e)->Size) fysiek++;
        free(b);
    }
    for (;;) {
        double t0 = ms_nu(), t1, per_s;
        LARGE_INTEGER wacht;
        meet(g, &s);
        s.fysiek = fysiek;
        t1 = ms_nu();
        kosten += t1 - t0;
        geteld++;
        EnterCriticalSection(&bslot);
        if (interval != vorige_interval) { beeld.hn = 0; vorige_interval = interval; }
        memcpy(beeld.k, g, sizeof g);
        beeld.ng = nkaart;
        beeld.s = s;
        if (beeld.hn == HIST) {
            for (i = 0; i < MAXG; i++) { memmove(beeld.hu[i], beeld.hu[i] + 1, sizeof(float) * (HIST - 1)); memmove(beeld.hv[i], beeld.hv[i] + 1, sizeof(float) * (HIST - 1)); }
            beeld.hn--;
        }
        for (i = 0; i < nkaart; i++) {
            beeld.hu[i][beeld.hn] = (float)g[i].util;
            beeld.hv[i][beeld.hn] = g[i].vram_tot > 0 ? (float)(100.0 * g[i].vram_gebr / g[i].vram_tot) : 0;
        }
        beeld.hn++;
        beeld.tik = GetTickCount();
        if (t1 - venster_start >= 1000) {
            per_s = geteld * 1000.0 / (t1 - venster_start);
            beeld.per_s = per_s; beeld.meet_ms = kosten / geteld;
            venster_start = t1; geteld = 0; kosten = 0;
        }
        LeaveCriticalSection(&bslot);
        /* Geen bericht sturen: een stroom berichten gaat voor de muis en het toetsenbord, en dan kan het venster niet meer
         * verplaatst worden. InvalidateRect geeft WM_PAINT, en dat komt pas na de invoer aan de beurt. */
        if (doel && !InterlockedExchange(&wacht_op_tekenen, 1)) InvalidateRect(doel, NULL, FALSE);
        if (interval <= 0) {    /* MAX: een meting per beeld van het scherm (DwmFlush wacht op het volgende beeld) */
            if (FAILED(DwmFlush())) Sleep(4);
            volgende = ms_nu();
            continue;
        }
        volgende += interval;
        if (volgende < t1) volgende = t1;      /* achter: niet inhalen */
        wacht.QuadPart = -(LONGLONG)((volgende - t1) * 10000.0);
        if (wacht.QuadPart < 0) { SetWaitableTimer(klok, &wacht, 0, NULL, NULL, FALSE); WaitForSingleObject(klok, 3000); }
    }
    return 0;
}

int meter_start(HWND venster, UINT bericht, int interval_ms)
{
    SYSTEM_INFO si;
    doel = venster; doel_bericht = bericht; interval = interval_ms;
    GetSystemInfo(&si);
    ncpu = (int)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    if (ncpu < 1) ncpu = (int)si.dwNumberOfProcessors;
    InitializeCriticalSection(&pslot);
    InitializeCriticalSection(&bslot);
    zoek_kaarten();
    nvml_start();
    igcl_start();
    pdh_buf = malloc(PDH_BUF);
    if (PdhOpenQueryW(NULL, 0, &vraag)) return 0;
    if (PdhAddEnglishCounterW(vraag, L"\\GPU Engine(*)\\Utilization Percentage", 0, &t_eng)) t_eng = NULL;
    if (PdhAddEnglishCounterW(vraag, L"\\GPU Process Memory(*)\\Local Usage", 0, &t_ded)
        && PdhAddEnglishCounterW(vraag, L"\\GPU Process Memory(*)\\Dedicated Usage", 0, &t_ded)) t_ded = NULL;
    if (PdhAddEnglishCounterW(vraag, L"\\GPU Process Memory(*)\\Non Local Usage", 0, &t_shr)
        && PdhAddEnglishCounterW(vraag, L"\\GPU Process Memory(*)\\Shared Usage", 0, &t_shr)) t_shr = NULL;
    if (PdhAddEnglishCounterW(vraag, L"\\GPU Process Memory(*)\\Total Committed", 0, &t_com)) t_com = NULL;
    if (PdhAddEnglishCounterW(vraag, L"\\GPU Adapter Memory(*)\\Dedicated Usage", 0, &t_aded)) t_aded = NULL;
    if (PdhAddEnglishCounterW(vraag, L"\\GPU Adapter Memory(*)\\Shared Usage", 0, &t_ashr)) t_ashr = NULL;
    if (PdhAddEnglishCounterW(vraag, L"\\Paging File(_Total)\\% Usage", 0, &t_wissel)) t_wissel = NULL;
    PdhCollectQueryData(vraag);
    CloseHandle(CreateThread(NULL, 0, trage_draad, NULL, 0, NULL));
    Sleep(60);      /* de eerste proceslijst en een eerste tijdvak voor de tellers */
    CloseHandle(CreateThread(NULL, 0, snelle_draad, NULL, 0, NULL));
    return nkaart > 0;
}

void meter_interval(int ms) { interval = ms; }
void meter_getekend(void) { InterlockedExchange(&wacht_op_tekenen, 0); }
void meter_kopie(Beeld *b)
{
    EnterCriticalSection(&bslot);
    *b = beeld;
    LeaveCriticalSection(&bslot);
}
