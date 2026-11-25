/* gpu-venster: een venster in de stijl van SIGINT met de cijfers van de grafische kaarten.
 * Meten: meter.c (Windows-tellers, NVML, proceslijst), zonder hulpprogramma.
 * Tekenen: GDI+ (vlakken, lijnen) en GDI (tekst) in een eigen beeld (DIB). Maten in dp (96 dpi = 1). */
#ifndef UNICODE
#define UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <gdiplus.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "meter.h"

/* kleuren van SIGINT, 0xRRGGBBAA */
#define GOUD 0xE0B13AFFu
#define GROEN 0x2FD6A0FFu
#define GRIJS 0x8A9BB0FFu
#define LICHT 0xC3CEDEFFu
#define WIT 0xE0E8F0FFu
#define BLAUW 0x1A6FD4FFu
#define HEMEL 0x4A9BF0FFu
#define PAARS 0xB07CFFFFu
#define ROOD 0xFF3333FFu
#define ORANJE 0xFF9933FFu
#define DONKER 0x000000FFu
#define VELD 0x000000FFu
#define LIJNTJE 0xFFFFFF12u
#define SPOOR 0xFFFFFF0Du

enum { LINKS, MIDDEN, RECHTS };

/* Laptop met twee kaarten: dit programma hoort op de ingebouwde kaart, niet op de NVIDIA of AMD. */
__declspec(dllexport) DWORD NvOptimusEnablement = 0;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 0;

static volatile LONG interval_ms = 100;
static int geen_kaart;
static HWND venster;

/* Twee talen: 0 = Nederlands, 1 = Engels. Elke tekst in het venster staat als T("Nederlands", "English"). */
static int taal;
#define T(nl_, en_) (taal ? (en_) : (nl_))

/* bediening */
enum { ID_SNEL = 1, ID_KOP = 10, ID_STOP = 20, ID_TAAL = 30, ID_RIJ = 100 };
static struct { RECT r; int id; } raak[MAXG * MAXP + 32];
static int nraak, rol, sorteer;         /* sorteer: 0 videogeheugen, 1 naam, 2 gedeeld, 3 gepaged, 4 gpu-%, 5 ram */
static int keus_pid = -1, keus_gpu;
static char keus_naam[64];
static int rij_pid[MAXG * MAXP], rij_gpu[MAXG * MAXP], rij_n;    /* de rijen zoals ze het laatst getekend zijn */
static char melding[160];
static DWORD melding_tik;
static const int snelheid[6] = { 16, 100, 250, 500, 1000, 2000 };   /* 16 ms = MAX: 60 per seconde */

static void zet_snelheid(int ms)
{
    if (ms == interval_ms) return;
    RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\gpu-venster", L"interval_ms", REG_DWORD, &ms, sizeof ms);
    interval_ms = ms;
    meter_interval(ms == 16 ? 0 : ms);     /* MAX: de meter loopt mee met de beelden van het scherm */
}

static void meld(const char *f, ...)
{
    va_list a;
    va_start(a, f);
    vsnprintf(melding, sizeof melding, f, a);
    va_end(a);
    melding_tik = GetTickCount();
}

static int is_beheerder(void)
{
    HANDLE t;
    TOKEN_ELEVATION e = { 0 };
    DWORD n;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &t)) return 0;
    GetTokenInformation(t, TokenElevation, &e, sizeof e, &n);
    CloseHandle(t);
    return e.TokenIsElevated != 0;
}
/* Een beheerder mag met dit recht ook processen van andere gebruikers en van het systeem openen. */
static void debug_recht(void)
{
    HANDLE t;
    TOKEN_PRIVILEGES p = { 1 };
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES, &t)) return;
    if (LookupPrivilegeValueW(NULL, L"SeDebugPrivilege", &p.Privileges[0].Luid)) {
        p.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        AdjustTokenPrivileges(t, FALSE, &p, 0, NULL, NULL);
    }
    CloseHandle(t);
}

/* Stopt het gekozen proces. Een onderdeel van Windows pas na een tweede klik (de pc kan vastlopen).
 * Geen toegang en geen beheerder: na nog een klik start het programma opnieuw als beheerder (Windows vraagt toestemming). */
static void beeindig(void)
{
    static const char *windows[] = { "csrss.exe", "wininit.exe", "smss.exe", "lsass.exe", "winlogon.exe", "services.exe",
                                     "Registry", "Memory Compression", "svchost.exe", "dwm.exe", "fontdrvhost.exe" };
    static int zeker_pid = -1, hoger_pid = -1;
    HANDLE h;
    DWORD fout;
    int i;
    if (keus_pid <= 4) { meld(T("Dit is de kern van Windows zelf. Die kan niet stoppen.", "This is the Windows kernel itself. It cannot stop.")); return; }
    for (i = 0; i < (int)(sizeof windows / sizeof windows[0]); i++)
        if (!_stricmp(keus_naam, windows[i]) && zeker_pid != keus_pid) {
            zeker_pid = keus_pid;
            meld(T("%s hoort bij Windows: de pc kan vastlopen. Klik nog een keer op STOP.",
                   "%s is part of Windows: the PC can crash. Click STOP again."), keus_naam);
            return;
        }
    zeker_pid = -1;
    debug_recht();
    h = OpenProcess(PROCESS_TERMINATE, FALSE, (DWORD)keus_pid);
    if (h && TerminateProcess(h, 1)) { meld(T("%s (%d) is gestopt", "%s (%d) was stopped"), keus_naam, keus_pid); keus_pid = -1; CloseHandle(h); return; }
    fout = GetLastError();
    if (h) CloseHandle(h);
    if (fout != ERROR_ACCESS_DENIED) { meld(T("%s (%d) stopt niet: het bestaat niet meer", "%s (%d) does not stop: it no longer exists"), keus_naam, keus_pid); return; }
    if (is_beheerder()) { meld(T("%s: Windows schermt dit proces af, ook voor een beheerder.", "%s: Windows protects this process, also from an administrator."), keus_naam); return; }
    if (hoger_pid != keus_pid) {
        hoger_pid = keus_pid;
        meld(T("Geen toegang. Klik nog een keer op STOP: herstart als beheerder.", "No access. Click STOP again: restart as administrator."));
        return;
    }
    {
        wchar_t pad[MAX_PATH];
        GetModuleFileNameW(NULL, pad, MAX_PATH);
        if ((INT_PTR)ShellExecuteW(NULL, L"runas", pad, NULL, NULL, SW_SHOWNORMAL) > 32) PostMessageW(venster, WM_CLOSE, 0, 0);
        else meld(T("Niet herstart: de vraag van Windows is geweigerd.", "Not restarted: the Windows prompt was declined."));
        hoger_pid = -1;
    }
}

/* ---------- tekenen ---------- */
static HDC dc;
static GpGraphics *g;
static float D;
static uint32_t *px_beeld;      /* het beeld zelf: 0x00RRGGBB per punt, bovenste rij eerst */
static int px_b, px_h;

/* Een punt mengen met een kleur; a = dekking 0..1. */
static void meng(int x, int y, uint32_t k, float a)
{
    uint32_t d, *p;
    int ia;
    if (x < 0 || y < 0 || x >= px_b || y >= px_h || a <= 0) return;
    ia = a >= 1 ? 256 : (int)(a * 256);
    p = &px_beeld[y * px_b + x];
    d = *p;
    *p = ((((d >> 16) & 255) * (256 - ia) + (k >> 24) * ia) >> 8) << 16
       | ((((d >> 8) & 255) * (256 - ia) + ((k >> 16) & 255) * ia) >> 8) << 8
       | (((d & 255) * (256 - ia) + ((k >> 8) & 255) * ia) >> 8);
}
/* Een rechte rechthoek op hele punten, direct in het beeld (veel sneller dan GDI+). */
static void vul_recht(int x0, int y0, int x1, int y1, uint32_t k)
{
    float a = (k & 255) / 255.0f;
    int x, y;
    if (x1 <= x0) x1 = x0 + 1;
    if (y1 <= y0) y1 = y0 + 1;
    for (y = y0; y < y1; y++) for (x = x0; x < x1; x++) meng(x, y, k, a);
}

static ARGB argb(uint32_t k) { return ((k & 0xFFu) << 24) | (k >> 8); }
static COLORREF rgb(uint32_t k) { return RGB(k >> 24, (k >> 16) & 255, (k >> 8) & 255); }
static uint32_t dun(uint32_t k, unsigned alfa) { return (k & 0xFFFFFF00u) | alfa; }

static GpPath *rond_pad(float x, float y, float b, float h, float r)
{
    GpPath *p;
    float d;
    if (r > h / 2) r = h / 2;
    if (r > b / 2) r = b / 2;
    d = r * 2;
    GdipCreatePath(FillModeAlternate, &p);
    if (d < 0.5f) { GdipAddPathRectangle(p, x, y, b, h); return p; }
    GdipAddPathArc(p, x, y, d, d, 180, 90);
    GdipAddPathArc(p, x + b - d, y, d, d, 270, 90);
    GdipAddPathArc(p, x + b - d, y + h - d, d, d, 0, 90);
    GdipAddPathArc(p, x, y + h - d, d, d, 90, 90);
    GdipClosePathFigure(p);
    return p;
}
static void vlak(float x, float y, float b, float h, float r, uint32_t vul, uint32_t rand)
{
    GpPath *p;
    if (b <= 0 || h <= 0) return;
    if (r < 0.25f && !(rand & 255)) {
        if (vul & 255) vul_recht((int)(x + 0.5f), (int)(y + 0.5f), (int)(x + b + 0.5f), (int)(y + h + 0.5f), vul);
        return;
    }
    if (vul & 255) {
        GpSolidFill *br;
        GdipCreateSolidFill(argb(vul), &br);
        if (r < 0.25f) GdipFillRectangle(g, (GpBrush *)br, x, y, b, h);
        else { p = rond_pad(x, y, b, h, r); GdipFillPath(g, (GpBrush *)br, p); GdipDeletePath(p); }
        GdipDeleteBrush((GpBrush *)br);
    }
    if (rand & 255) {
        GpPen *pen;
        p = rond_pad(x + 0.5f, y + 0.5f, b - 1, h - 1, r);
        GdipCreatePen1(argb(rand), 1.0f, UnitPixel, &pen);
        GdipDrawPath(g, pen, p);
        GdipDeletePen(pen);
        GdipDeletePath(p);
    }
}
static void balk(float x, float y, float b, float h, double deel, uint32_t kleur)
{
    if (deel < 0) deel = 0;
    if (deel > 1) deel = 1;
    vlak(x, y, b, h, h / 2, SPOOR, 0);
    if (deel > 0) vlak(x, y, (float)(b * deel) < h ? h : (float)(b * deel), h, h / 2, kleur, 0);
}

static HFONT letter(float dp, int vet)
{
    static struct { int px, vet; HFONT f; } kas[32];
    static int n;
    int px = (int)(dp * D + 0.5f), i;
    for (i = 0; i < n; i++) if (kas[i].px == px && kas[i].vet == vet) return kas[i].f;
    if (n == 32) { for (i = 0; i < n; i++) DeleteObject(kas[i].f); n = 0; }
    kas[n].px = px; kas[n].vet = vet;
    kas[n].f = CreateFontW(-px, 0, 0, 0, vet ? FW_SEMIBOLD : FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    return kas[n++].f;
}
/* Tekst in een vak (verticaal in het midden); te lang = afgekapt met puntjes. Geeft de breedte in pixels. */
static float tekst(float x, float y, float b, float h, const char *t, float dp, float spatie, uint32_t kleur, int lijn, int vet)
{
    wchar_t w[200];
    int n = MultiByteToWideChar(CP_UTF8, 0, t, -1, w, 196) - 1;
    SIZE s;
    if (n <= 0 || b <= 0) return 0;
    SelectObject(dc, letter(dp, vet));
    SetTextCharacterExtra(dc, (int)(spatie * D + 0.5f));
    SetTextColor(dc, rgb(kleur));
    GetTextExtentPoint32W(dc, w, n, &s);
    while (s.cx > b && n > 1) {
        n--;
        w[n] = 0x2026; w[n + 1] = 0;
        GetTextExtentPoint32W(dc, w, n + 1, &s);
        if (s.cx <= b) { n++; break; }
    }
    if (lijn == MIDDEN) x += (b - s.cx) / 2; else if (lijn == RECHTS) x += b - s.cx;
    TextOutW(dc, (int)(x + 0.5f), (int)(y + (h - s.cy) / 2 + 0.5f), w, n);
    return (float)s.cx;
}
/* getal: in het Nederlands met een komma, in het Engels met een punt */
static const char *nl(const char *f, ...)
{
    static char b[16][48];
    static int i;
    char *r = b[i = (i + 1) & 15], *p;
    va_list a;
    va_start(a, f);
    vsnprintf(r, 48, f, a);
    va_end(a);
    if (!taal) for (p = r; *p; p++) if (*p == '.') *p = ',';
    return r;
}
#define GB(x) ((x) / 1073741824.0)
#define MB(x) ((x) / 1048576.0)
static const char *maat(double bytes) { return bytes >= 1073741824.0 ? nl("%.2f GB", GB(bytes)) : nl("%.0f MB", MB(bytes)); }

static void raakvlak(float x, float y, float b, float h, int id)
{
    if (nraak < (int)(sizeof raak / sizeof raak[0])) {
        SetRect(&raak[nraak].r, (int)x, (int)y, (int)(x + b), (int)(y + h));
        raak[nraak++].id = id;
    }
}

static void kopje(float x, float y, float b, const char *t, const char *rechts)
{
    tekst(x, y, b, 16 * D, t, 10.5f, 2, GOUD, LINKS, 0);
    if (rechts) tekst(x, y, b, 16 * D, rechts, 10.5f, 1, GRIJS, RECHTS, 0);
    vlak(x, y + 21 * D, b, 1, 0, 0xE0B13A30u, 0);
}

static uint32_t kleur_bij(double pct, double oranje, double rood)
{
    return pct >= rood ? ROOD : pct >= oranje ? ORANJE : GROEN;
}
static uint32_t soort_kleur(const char *s)
{
    if (!strcmp(s, "app")) return GROEN;
    if (!strcmp(s, "browser")) return HEMEL;
    if (!strcmp(s, "compositor")) return GOUD;
    if (!strcmp(s, "kernel") || !strcmp(s, "system")) return GRIJS;
    if (strstr(s, "game") || strstr(s, "3d")) return ORANJE;
    return PAARS;
}
/* De soort van een proces in de gekozen taal. De meter geeft vaste Engelse sleutels. */
static const char *soort_naam(const char *s)
{
    if (taal) return s;
    if (!strcmp(s, "system")) return "systeem";
    if (!strcmp(s, "compute")) return "rekenen";
    if (!strcmp(s, "3D/game")) return "3D/spel";
    return s;
}
/* Waarvoor een proces van deze soort zijn videogeheugen gebruikt. Eigen teksten, met de gewone technische namen. */
static const char *soort_uitleg(const char *s)
{
    /* kort houden: de tekst moet achter de naam passen */
    if (!strcmp(s, "browser")) return T("tegels, video, WebGL", "tiles, video, WebGL");
    if (!strcmp(s, "compositor")) return T("vensters, swap chain", "windows, swap chain");
    if (!strcmp(s, "kernel")) return T("stuurprogramma", "driver");
    if (!strcmp(s, "video")) return T("videovlakken", "video surfaces");
    if (!strcmp(s, "3D/game")) return T("texturen, render targets", "textures, render targets");
    if (!strcmp(s, "compute")) return T("rekenbuffers", "compute buffers");
    return "";
}

static void mini(float x, float y, float b, const char *label, const char *waarde, const char *eenheid, double deel, uint32_t kleur)
{
    float wb;
    tekst(x, y, b, 14 * D, label, 9.5f, 2, GRIJS, LINKS, 0);
    wb = tekst(x, y + 15 * D, b, 30 * D, waarde, 22, 0, WIT, LINKS, 1);
    tekst(x + wb + 5 * D, y + 23 * D, b - wb - 5 * D, 20 * D, eenheid, 11.5f, 0, GRIJS, LINKS, 0);
    balk(x, y + 49 * D, b, 4 * D, deel, kleur);
}

#define PANEEL_H 212.0f
static void gpu_paneel(const Gpu *q, int nr, float x, float y, float b)
{
    float ix = x + 14 * D, ib = b - 28 * D, mb = (ib - 20 * D) / 3, lx, ry, sx;
    double rest = q->vram_gebr - q->toegewezen;
    char t[200];
    int i, n;
    vlak(x, y, b, PANEEL_H * D, 10 * D, VELD, LIJNTJE);
    tekst(ix, y + 10 * D, ib - 60 * D, 22 * D, q->naam, 15, 0, WIT, LINKS, 1);
    snprintf(t, sizeof t, "GPU %d", nr);
    tekst(ix, y + 10 * D, ib, 22 * D, t, 10.5f, 2, GOUD, RECHTS, 0);
    n = snprintf(t, sizeof t, "driver %s", q->driver);
    if (q->cuda[0]) n += snprintf(t + n, sizeof t - n, "  \xC2\xB7  CUDA %s", q->cuda);
    if (q->model[0]) n += snprintf(t + n, sizeof t - n, "  \xC2\xB7  %s", q->model);
    if (q->bus[0]) n += snprintf(t + n, sizeof t - n, "  \xC2\xB7  %s", q->bus);
    tekst(ix, y + 32 * D, ib, 16 * D, t, 11, 0, GRIJS, LINKS, 0);

    ry = y + 54 * D;
    mini(ix, ry, mb, T("BELASTING", "LOAD"), q->util < 0 ? "-" : nl("%.0f", q->util), "%", q->util / 100, GROEN);
    mini(ix + mb + 10 * D, ry, mb, T("TEMPERATUUR", "TEMPERATURE"), q->temp < 0 ? "-" : nl("%.0f", q->temp), q->temp < 0 ? "" : "\xC2\xB0""C",
         q->temp / 100, kleur_bij(q->temp, 75, 87));
    mini(ix + 2 * (mb + 10 * D), ry, mb, T("VERMOGEN", "POWER"), q->w < 0 ? "-" : nl("%.1f", q->w),
         q->w < 0 ? "" : q->w_max > 0 ? nl("/ %.0f W", q->w_max) : "W", q->w_max > 0 ? q->w / q->w_max : 0, HEMEL);

    /* videogeheugen: een balk in stukken per soort proces */
    ry = y + 116 * D;
    tekst(ix, ry, ib, 16 * D, T("VIDEOGEHEUGEN", "VIDEO MEMORY"), 9.5f, 2, GRIJS, LINKS, 0);
    snprintf(t, sizeof t, T("%s / %s   \xC2\xB7   vrij %s   \xC2\xB7   gedeeld (RAM) %s", "%s / %s   \xC2\xB7   free %s   \xC2\xB7   shared (RAM) %s"),
             maat(q->vram_gebr), maat(q->vram_tot), maat(q->vram_tot - q->vram_gebr), maat(q->gedeeld));
    tekst(ix, ry, ib, 16 * D, t, 11.5f, 0, WIT, RECHTS, 0);
    ry += 21 * D;
    vlak(ix, ry, ib, 8 * D, 4 * D, SPOOR, 0);
    if (rest < 0) rest = 0;
    sx = ix;
    for (i = 0; i <= q->ns && q->vram_tot > 0; i++) {
        double v = i < q->ns ? q->svram[i] : rest;
        float sb = (float)(ib * v / q->vram_tot);
        if (sx + sb > ix + ib) sb = ix + ib - sx;
        if (sb >= 1) vlak(sx, ry, sb, 8 * D, sx == ix ? 3 * D : 0, i < q->ns ? soort_kleur(q->snaam[i]) : 0x4A5568FFu, 0);
        sx += sb;
    }
    ry += 14 * D;
    lx = ix;
    for (i = 0; i <= q->ns; i++) {
        uint32_t k = i < q->ns ? soort_kleur(q->snaam[i]) : 0x4A5568FFu;
        if (i < q->ns) snprintf(t, sizeof t, "%s %s", soort_naam(q->snaam[i]), maat(q->svram[i]));
        else { if (rest < 1048576) continue; snprintf(t, sizeof t, T("niet toegewezen %s", "unassigned %s"), maat(rest)); }
        if (lx + 40 * D > ix + ib) break;
        vlak(lx, ry + 5 * D, 7 * D, 7 * D, 2 * D, k, 0);
        lx += 11 * D;
        lx += tekst(lx, ry, ix + ib - lx, 16 * D, t, 10.5f, 0, GRIJS, LINKS, 0) + 12 * D;
    }

    /* overige cijfers */
    ry += 20 * D;
    if (q->pstate < 0 && q->sm < 0 && q->temp < 0 && q->w < 0) {
        tekst(ix, ry, ib, 16 * D, T("Geen bruikbare cijfers voor temperatuur, vermogen en klok van deze kaart.",
                                    "No usable figures for temperature, power and clock of this card."), 10.5f, 0, GRIJS, LINKS, 0);
    } else {
        n = 0; t[0] = 0;
        if (q->pstate >= 0) n += snprintf(t + n, sizeof t - n, "P%.0f   ", q->pstate);
        if (q->sm >= 0) n += snprintf(t + n, sizeof t - n, "SM %.0f/%.0f MHz   ", q->sm, q->sm_max);
        if (q->memklok >= 0) n += snprintf(t + n, sizeof t - n, "mem %.0f MHz   ", q->memklok);
        if (q->videoklok >= 0) n += snprintf(t + n, sizeof t - n, "video %.0f MHz   ", q->videoklok);
        if (q->cc[0]) n += snprintf(t + n, sizeof t - n, "CC %s", q->cc);
        tekst(ix, ry, ib, 16 * D, t, 10.5f, 0, GRIJS, LINKS, 0);
        n = 0; t[0] = 0;
        if (q->gen >= 0) n += snprintf(t + n, sizeof t - n, "PCIe gen%.0f x%.0f   ", q->gen, q->breedte);
        if (q->rx >= 0) n += snprintf(t + n, sizeof t - n, "\xE2\x86\x93 %.0f  \xE2\x86\x91 %.0f MB/s   ", q->rx / 1024, q->tx / 1024);
        if (q->util_mem >= 0) n += snprintf(t + n, sizeof t - n, T("geheugenactiviteit %.0f %%   ", "memory activity %.0f %%   "), q->util_mem);
        if (q->enc >= 0) n += snprintf(t + n, sizeof t - n, "enc %.0f %%  dec %.0f %%   ", q->enc, q->dec);
        if (q->util_nvml >= 0) n += snprintf(t + n, sizeof t - n, "NVML %.0f %%", q->util_nvml);
        tekst(ix, ry + 17 * D, ib, 16 * D, t, 10.5f, 0, GRIJS, LINKS, 0);
    }
}

static void lijn_grafiek(const float *v, int n, float x, float y, float b, float h, uint32_t kleur, int vullen)
{
    /* Per kolom van het beeld: de hoogste en laagste waarde in die kolom, daartussen een streep met zachte randen.
     * Direct in het beeld; GDI+ deed over deze lijnen 7 ms per beeld (gemeten). */
    float dik = 1.0f * D, stap = (float)(HIST - 1) / b, onder = y + h;
    int c, c1 = (int)(x + b), r, i;
    if (n < 2) return;
    for (c = (int)(x + b * (float)(HIST - n) / (HIST - 1)); c < c1; c++) {
        float f0 = (c - x) * stap - (HIST - n), f1 = f0 + stap, lo = 1e9f, hi = -1e9f, w, top, bot;
        if (f0 < 0) f0 = 0;
        if (f1 > n - 1) f1 = (float)(n - 1);
        if (f1 < f0) continue;
        for (i = (int)f0 - 1; i <= (int)f1 + 1; i++) {     /* de twee randen van de kolom en de metingen ertussen */
            float f = i < f0 ? f0 : i > f1 ? f1 : (float)i;
            int k = (int)f;
            w = k >= n - 1 ? v[n - 1] : v[k] + (v[k + 1] - v[k]) * (f - k);
            w = onder - h * (w > 100 ? 100 : w < 0 ? 0 : w) / 100.0f;
            if (w < lo) lo = w;
            if (w > hi) hi = w;
        }
        top = lo - dik; bot = hi + dik;
        for (r = (int)top; r <= (int)bot; r++) meng(c, r, kleur, (r + 1 < bot ? r + 1 : bot) - (r > top ? r : top));
        if (vullen) for (r = (int)bot + 1; r < (int)onder; r++) meng(c, r, kleur, 0.13f);
    }
}

static int vergelijk(const void *a, const void *b)
{
    const Proc *p = a, *q = b;
    double d = 0;
    if (sorteer == 1) { int c = _stricmp(p->naam, q->naam); if (c) return c; }
    else if (sorteer == 2) d = q->gedeeld - p->gedeeld;
    else if (sorteer == 3) d = q->paged - p->paged;
    else if (sorteer == 4) d = q->pct - p->pct;
    else if (sorteer == 5) d = q->ram - p->ram;
    if (d == 0) d = q->vram - p->vram;
    if (d == 0) d = q->gedeeld - p->gedeeld;
    return d > 0 ? 1 : d < 0 ? -1 : p->pid - q->pid;
}

static void teken(int W, int H)
{
    static Beeld bd;
    static Sys s;
    static Proc alle[MAXG * MAXP];
    static const uint32_t gkleur[MAXG] = { GROEN, HEMEL, ORANJE, PAARS };
    Gpu *k = bd.k;
    float (*hu)[HIST] = bd.hu, (*hv)[HIST] = bd.hv;
    int ng, np = 0, live, i, j, ms = interval_ms;
    float M = 16 * D, x, y, b, gh, lb, bh, mse;
    char t[200];

    meter_kopie(&bd);
    ng = bd.ng;
    s = bd.s;
    live = ng && GetTickCount() - bd.tik < 3500;
    nraak = 0;
    if (ms == 16 && bd.per_s > 1) mse = 1000.0f / (float)bd.per_s; else mse = (float)ms;
    j = mse >= 250 ? 1 : (int)(250 / mse);       /* het getal BELASTING: gemiddelde over 0,25 s, anders is het niet te lezen */
    if (j > bd.hn) j = bd.hn;
    for (i = 0; i < ng && j > 1; i++) { float som = 0; int a; for (a = bd.hn - j; a < bd.hn; a++) som += hu[i][a]; k[i].util = som / j; }

    /* kop: naam, snelheid, live */
    tekst(M, 12 * D, 90 * D, 28 * D, "GPUINT", 12, 3, GOUD, LINKS, 1);
    {
        uint32_t kl = geen_kaart ? ROOD : live ? GROEN : ORANJE;
        float px = W - M - 80 * D, py = 12 * D, tx = M + 92 * D;
        /* taalknop: de twee talen naast elkaar, de gekozen taal in goud */
        vlak(tx, py + 1 * D, 64 * D, 26 * D, 13 * D, 0, LIJNTJE);
        tekst(tx, py + 1 * D, 32 * D, 25 * D, "NL", 10.5f, 1, taal ? GRIJS : GOUD, MIDDEN, !taal);
        tekst(tx + 32 * D, py + 1 * D, 32 * D, 25 * D, "EN", 10.5f, 1, taal ? GOUD : GRIJS, MIDDEN, taal);
        raakvlak(tx, py, 64 * D, 28 * D, ID_TAAL);
        vlak(px, py, 80 * D, 28 * D, 14 * D, dun(kl, 0x1A), dun(kl, 0x60));
        vlak(px + 14 * D, py + 10.5f * D, 7 * D, 7 * D, 3.5f * D, kl, 0);
        tekst(px + 26 * D, py, 48 * D, 27 * D, geen_kaart ? T("FOUT", "ERROR") : live ? "LIVE" : T("WACHT", "WAIT"), 10.5f, 2, kl, LINKS, 1);
        for (i = 5; i >= 0; i--) {
            int aan = snelheid[i] == ms;
            px -= 58 * D;
            vlak(px, py + 1 * D, 52 * D, 26 * D, 13 * D, aan ? dun(GOUD, 0x26) : 0, aan ? dun(GOUD, 0x90) : LIJNTJE);
            tekst(px, py + 1 * D, 52 * D, 25 * D, i ? nl("%.3g s", snelheid[i] / 1000.0) : "MAX", 11, 0, aan ? GOUD : GRIJS, MIDDEN, aan);
            raakvlak(px, py, 52 * D, 28 * D, ID_SNEL + i);
        }
        snprintf(t, sizeof t, T("VERVERSEN  %.0f PER S", "REFRESH  %.0f PER S"), bd.per_s);
        tekst(px - 190 * D, py, 180 * D, 27 * D, t, 9.5f, 2, GRIJS, RECHTS, 0);
        if (melding[0] && GetTickCount() - melding_tik < 6000)
            tekst(tx + 76 * D, py, px - 200 * D - tx - 76 * D, 27 * D, melding, 12, 0, ORANJE, LINKS, 0);
    }
    if (geen_kaart) tekst(M, 60 * D, W - 2 * M, 30 * D, T("Geen grafische kaart gevonden", "No graphics card found"), 14, 0, ORANJE, LINKS, 0);
    if (!ng) { if (!geen_kaart) tekst(M, 60 * D, W - 2 * M, 30 * D, T("Wachten op de eerste meting", "Waiting for the first sample"), 14, 0, GRIJS, LINKS, 0); return; }

    /* een paneel per kaart: naast elkaar als het past, anders onder elkaar */
    y = 52 * D;
    {
        int naast = ng;
        while (naast > 1 && (W - 2 * M - (naast - 1) * 10 * D) / naast < 440 * D) naast--;
        b = (W - 2 * M - (naast - 1) * 10 * D) / naast;
        for (i = 0; i < ng; i++) {
            gpu_paneel(&k[i], i, M + (i % naast) * (b + 10 * D), y, b);
            if (i % naast == naast - 1 || i == ng - 1) y += (PANEEL_H + 10) * D;
        }
    }

    /* grafiek (links) en systeem (rechts) */
    gh = H * 0.25f;     /* laag venster: kleinere grafiek, zodat de proceslijst ruimte houdt */
    if (gh > 232 * D) gh = 232 * D;
    if (gh < 190 * D) gh = 190 * D;
    lb = (W - 2 * M - 10 * D) * 0.60f;
    vlak(M, y, lb, gh, 10 * D, VELD, LIJNTJE);
    i = (int)(HIST * mse);
    snprintf(t, sizeof t, T("VERLOOP  %d %s", "HISTORY  %d %s"), i >= 60000 ? i / 60000 : (i + 500) / 1000, i >= 60000 ? "MIN" : "S");
    kopje(M + 14 * D, y + 10 * D, lb - 28 * D, t, NULL);
    {
        float lx = M + lb - 14 * D, ly = y + 10 * D, lw, px = M + 44 * D, py = y + 44 * D, pb = lb - 58 * D, ph = gh - 44 * D - 16 * D;
        lw = tekst(lx - 200 * D, ly, 200 * D, 16 * D, "VRAM GPU 0", 10, 1, GRIJS, RECHTS, 0);
        vlak(lx - lw - 20 * D, ly + 7 * D, 14 * D, 2 * D, D, GOUD, 0);
        lx -= lw + 34 * D;
        for (i = ng - 1; i >= 0; i--) {
            snprintf(t, sizeof t, "GPU %d", i);
            lw = tekst(lx - 200 * D, ly, 200 * D, 16 * D, t, 10, 1, GRIJS, RECHTS, 0);
            vlak(lx - lw - 20 * D, ly + 7 * D, 14 * D, 2 * D, D, gkleur[i], 0);
            lx -= lw + 34 * D;
        }
        for (i = 0; i <= 2; i++) {
            float gy = (float)(int)(py + ph * i / 2);
            vlak(px, gy, pb, 1, 0, LIJNTJE, 0);
            tekst(M + 10 * D, gy - 8 * D, 28 * D, 16 * D, i == 0 ? "100" : i == 1 ? "50" : "0", 9.5f, 0, GRIJS, RECHTS, 0);
        }
        j = (int)(100 / mse);      /* de lijn: glijdend gemiddelde over 0,1 s; een tijdvak van 6 ms geeft alleen 0 of 100 */
        for (i = 0; i < ng && j > 1; i++) {
            static float glad[HIST];
            float som = 0;
            int a;
            for (a = 0; a < bd.hn; a++) { som += hu[i][a]; if (a >= j) som -= hu[i][a - j]; glad[a] = som / (a < j ? a + 1 : j); }
            memcpy(hu[i], glad, sizeof(float) * (size_t)bd.hn);
        }
        lijn_grafiek(hv[0], bd.hn, px, py, pb, ph, GOUD, 0);
        for (i = ng - 1; i >= 0; i--) lijn_grafiek(hu[i], bd.hn, px, py, pb, ph, gkleur[i], i == 0);
    }
    x = M + lb + 10 * D;
    b = W - M - x;
    vlak(x, y, b, gh, 10 * D, VELD, LIJNTJE);
    kopje(x + 14 * D, y + 10 * D, b - 28 * D, T("SYSTEEM", "SYSTEM"), NULL);
    {
        float ix = x + 14 * D, ib = b - 28 * D, ry = y + 38 * D, kb, hb = (ib - 16 * D) / 2;
        double pct = s.ram_tot > 0 ? 100 * s.ram_gebr / s.ram_tot : 0;
        int c;
        snprintf(t, sizeof t, T("CPU   %d KERNEN  %d DRADEN", "CPU   %d CORES  %d THREADS"), s.fysiek, s.logisch);
        tekst(ix, ry, ib - 70 * D, 18 * D, t, 9.5f, 2, GRIJS, LINKS, 0);
        tekst(ix, ry, ib, 18 * D, nl("%.1f %%", s.cpu), 13, 0, WIT, RECHTS, 1);
        balk(ix, ry + 20 * D, ib, 4 * D, s.cpu / 100, kleur_bij(s.cpu, 70, 90));
        ry += 29 * D;
        if (s.nkern > 0) {
            kb = (ib - (s.nkern - 1) * 2 * D) / s.nkern;
            for (i = 0; i < s.nkern; i++) {
                float kx = ix + i * (kb + 2 * D), kh = 16 * D, vh = (float)(kh * s.kern[i] / 100);
                vlak(kx, ry, kb, kh, 1.5f * D, SPOOR, 0);
                if (vh > 0.5f) vlak(kx, ry + kh - vh, kb, vh, 1.5f * D, kleur_bij(s.kern[i], 70, 90), 0);
            }
        }
        ry += 22 * D;
        snprintf(t, sizeof t, T("RAM   WISSEL %s / %s GB", "RAM   SWAP %s / %s GB"), nl("%.1f", GB(s.wissel_gebr)), nl("%.1f", GB(s.wissel_tot)));
        tekst(ix, ry, ib - 110 * D, 18 * D, t, 9.5f, 2, GRIJS, LINKS, 0);
        tekst(ix, ry, ib, 18 * D, nl("%.1f / %.1f GB", GB(s.ram_gebr), GB(s.ram_tot)), 13, 0, WIT, RECHTS, 1);
        balk(ix, ry + 20 * D, ib, 4 * D, pct / 100, kleur_bij(pct, 80, 92));
        ry += 31 * D;
        for (c = 0; c < 2; c++) {
            const Top *l = c ? s.tram : s.tcpu;
            int n = c ? s.ntram : s.ntcpu;
            float cx = ix + c * (hb + 16 * D), cy = ry;
            tekst(cx, cy, hb, 16 * D, c ? T("MEESTE RAM", "TOP RAM") : T("MEESTE CPU", "TOP CPU"), 9.5f, 2, GRIJS, LINKS, 0);
            cy += 17 * D;
            for (i = 0; i < n && cy + 19 * D <= y + gh - 4 * D; i++, cy += 19 * D) {
                vlak(cx, cy, hb, 1, 0, LIJNTJE, 0);
                tekst(cx, cy, hb - 62 * D, 19 * D, l[i].naam, 11.5f, 0, LICHT, LINKS, 0);
                tekst(cx, cy, hb, 19 * D, c ? nl("%.2f GB", GB(l[i].rss)) : nl("%.1f %%", l[i].cpu), 11.5f, 0, WIT, RECHTS, 0);
            }
        }
    }

    /* processen van alle kaarten */
    y += gh + 10 * D;
    bh = H - y - M;
    if (bh < 70 * D) return;
    b = W - 2 * M;
    for (i = 0; i < ng; i++) for (j = 0; j < k[i].np; j++) { alle[np] = k[i].p[j]; alle[np++].gpu = i; }
    qsort(alle, (size_t)np, sizeof alle[0], vergelijk);
    for (i = 0; i < np; i++) { rij_pid[i] = alle[i].pid; rij_gpu[i] = alle[i].gpu; }
    rij_n = np;
    vlak(M, y, b, bh, 10 * D, VELD, LIJNTJE);
    snprintf(t, sizeof t, "%d", np);
    kopje(M + 14 * D, y + 10 * D, b - 28 * D, T("PROCESSEN", "PROCESSES"), t);
    {
        float r = M + b - 16 * D, ry = y + 38 * D, rh = 26 * D, cx = r;
        float xram, xrek, xvid, xcop, x3d, xpct, xpag, xged, xv, xb, xrt = 0, xso = 0, xgi, xp;
        int rt = b > 1030 * D, so = b > 890 * D, past, van, gekozen = -1;
        double top = 1;
#define KOL(w) (cx -= (w) * D)
        xram = KOL(74); xrek = KOL(66); xvid = KOL(48); xcop = KOL(48); x3d = KOL(44); xpct = KOL(56); xpag = KOL(70); xged = KOL(70); xv = KOL(72); xb = KOL(70);
        if (rt) xrt = KOL(128);
        if (so) xso = KOL(84);
        xgi = KOL(38); xp = KOL(58);
#define KOP(xx, w, naam, s) tekst(xx, ry, (w) * D, 16 * D, naam, 9.5f, 1.5f, sorteer == (s) ? GOUD : GRIJS, RECHTS, 0)
        x = M + 14 * D;
        tekst(x, ry, xp - x, 16 * D, T("NAAM", "NAME"), 9.5f, 1.5f, sorteer == 1 ? GOUD : GRIJS, LINKS, 0);
        raakvlak(x, ry - 4 * D, 120 * D, 24 * D, ID_KOP + 1);
        KOP(xp, 58, "PID", -1); KOP(xgi, 38, "GPU", -1);
        if (so) tekst(xso + 12 * D, ry, 72 * D, 16 * D, T("SOORT", "KIND"), 9.5f, 1.5f, GRIJS, LINKS, 0);
        if (rt) tekst(xrt + 4 * D, ry, 124 * D, 16 * D, "RUNTIME", 9.5f, 1.5f, GRIJS, LINKS, 0);
        KOP(xb, 142, T("VIDEOGEHEUGEN", "VIDEO MEMORY"), 0); raakvlak(xb, ry - 4 * D, 142 * D, 24 * D, ID_KOP);
        KOP(xged, 70, T("GEDEELD", "SHARED"), 2); raakvlak(xged, ry - 4 * D, 70 * D, 24 * D, ID_KOP + 2);
        KOP(xpag, 70, T("GEPAGED", "PAGED"), 3); raakvlak(xpag, ry - 4 * D, 70 * D, 24 * D, ID_KOP + 3);
        KOP(xpct, 56, "GPU %", 4); raakvlak(xpct, ry - 4 * D, 56 * D, 24 * D, ID_KOP + 4);
        KOP(x3d, 44, "3D", -1); KOP(xcop, 48, "COPY", -1); KOP(xvid, 48, "VIDEO", -1); KOP(xrek, 66, T("REKEN", "COMPUTE"), -1);
        KOP(xram, 74, "RAM", 5); raakvlak(xram, ry - 4 * D, 74 * D, 24 * D, ID_KOP + 5);
        ry += 20 * D;
        past = (int)((y + bh - 8 * D - ry) / rh);
        if (rol > np - past) rol = np - past;
        if (rol < 0) rol = 0;
        van = rol;
        for (i = 0; i < np; i++) {
            if (alle[i].vram > top) top = alle[i].vram;
            if (alle[i].pid == keus_pid && alle[i].gpu == keus_gpu) gekozen = i;
        }
        for (i = van; i < np && i < van + past; i++, ry += rh) {
            const Proc *p = &alle[i];
            float nb;
            if (i == gekozen) vlak(M + 6 * D, ry, b - 12 * D, rh, 6 * D, dun(GOUD, 0x1C), dun(GOUD, 0x50));
            else vlak(x, ry, b - 28 * D, 1, 0, LIJNTJE, 0);
            raakvlak(M, ry, b, rh, ID_RIJ + i);
            nb = tekst(x, ry, xp - x - 6 * D, rh, p->naam, 12.5f, 0, i == gekozen ? WIT : LICHT, LINKS, 0);
            {   /* achter de naam: het geladen model, of anders waarvoor deze soort proces het geheugen gebruikt */
                const char *uitleg = p->binnen[0] ? p->binnen : soort_uitleg(p->soort);
                if (uitleg[0]) tekst(x + nb + 8 * D, ry, xp - x - 14 * D - nb, rh, uitleg, 11, 0, GOUD, LINKS, 0);
            }
            snprintf(t, sizeof t, "%d", p->pid);
            tekst(xp, ry, 58 * D, rh, t, 11.5f, 0, GRIJS, RECHTS, 0);
            snprintf(t, sizeof t, "%d", p->gpu);
            tekst(xgi, ry, 38 * D, rh, t, 11.5f, 0, gkleur[p->gpu], RECHTS, 1);
            if (so) {
                vlak(xso + 12 * D, ry + rh / 2 - 3 * D, 6 * D, 6 * D, 2 * D, soort_kleur(p->soort), 0);
                tekst(xso + 23 * D, ry, 60 * D, rh, soort_naam(p->soort), 11.5f, 0, GRIJS, LINKS, 0);
            }
            if (rt) tekst(xrt + 4 * D, ry, 122 * D, rh, p->runtime, 11, 0, GRIJS, LINKS, 0);
            balk(xb + 6 * D, ry + rh / 2 - 2 * D, 58 * D, 4 * D, p->vram / top, GOUD);
            tekst(xv, ry, 72 * D, rh, maat(p->vram), 12, 0, p->vram > 0 ? WIT : GRIJS, RECHTS, 0);
            tekst(xged, ry, 70 * D, rh, maat(p->gedeeld), 11.5f, 0, p->gedeeld >= 1048576 ? LICHT : GRIJS, RECHTS, 0);
            tekst(xpag, ry, 70 * D, rh, maat(p->paged), 11.5f, 0, p->paged >= 1048576 ? LICHT : GRIJS, RECHTS, 0);
            tekst(xpct, ry, 56 * D, rh, nl("%.0f %%", p->pct), 12, 0, p->pct >= 0.5 ? GROEN : GRIJS, RECHTS, p->pct >= 0.5);
            if (p->e3d >= 0.05) tekst(x3d, ry, 44 * D, rh, nl("%.0f", p->e3d), 11.5f, 0, LICHT, RECHTS, 0);
            if (p->ecopy >= 0.05) tekst(xcop, ry, 48 * D, rh, nl("%.0f", p->ecopy), 11.5f, 0, LICHT, RECHTS, 0);
            if (p->evideo >= 0.05) tekst(xvid, ry, 48 * D, rh, nl("%.0f", p->evideo), 11.5f, 0, LICHT, RECHTS, 0);
            if (p->ereken >= 0.05) tekst(xrek, ry, 66 * D, rh, nl("%.0f", p->ereken), 11.5f, 0, LICHT, RECHTS, 0);
            if (p->ram > 0) tekst(xram, ry, 74 * D, rh, maat(p->ram), 11.5f, 0, LICHT, RECHTS, 0);
        }
        if (np > past && past > 0) {    /* schuifstreep */
            float sh = bh - 74 * D, sy = y + 60 * D;
            vlak(M + b - 7 * D, sy + sh * van / np, 3 * D, sh * past / np, 1.5f * D, 0xFFFFFF30u, 0);
        }
        if (gekozen >= 0) {             /* knop: het gekozen proces stoppen */
            strcpy(keus_naam, alle[gekozen].naam);
            float kb = 280 * D, kx = M + b - 14 * D - 40 * D - kb, ky = y + 5 * D;
            snprintf(t, sizeof t, "STOP  %.40s  (%d)", alle[gekozen].naam, alle[gekozen].pid);
            vlak(kx, ky, kb, 24 * D, 12 * D, dun(ROOD, 0x26), dun(ROOD, 0xA0));
            tekst(kx + 10 * D, ky, kb - 20 * D, 23 * D, t, 10.5f, 1, 0xFF6B6BFFu, MIDDEN, 1);
            raakvlak(kx, ky, kb, 24 * D, ID_STOP);
        } else keus_pid = -1;
    }
}

static void schilder(HWND h)
{
    static HDC mem;
    static HBITMAP bmp;
    static void *bits;
    static int bw, bh;
    PAINTSTRUCT ps;
    HDC echt = BeginPaint(h, &ps);
    RECT r;
    GpBitmap *gb;
    GetClientRect(h, &r);
    if (r.right > 0 && r.bottom > 0) {
        if (r.right != bw || r.bottom != bh) {  /* het beeld blijft bewaard tot het venster een andere maat heeft */
            BITMAPINFO bi = { { sizeof(BITMAPINFOHEADER), r.right, -r.bottom, 1, 32, BI_RGB } };
            if (!mem) { mem = CreateCompatibleDC(echt); SetBkMode(mem, TRANSPARENT); }
            if (bmp) { SelectObject(mem, GetStockObject(DEFAULT_GUI_FONT)); DeleteObject(bmp); }
            bmp = CreateDIBSection(echt, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
            SelectObject(mem, bmp);
            bw = r.right; bh = r.bottom;
        }
        dc = mem;
        px_beeld = bits; px_b = bw; px_h = bh;
        GdipCreateBitmapFromScan0(bw, bh, bw * 4, PixelFormat32bppRGB, bits, &gb);
        GdipGetImageGraphicsContext((GpImage *)gb, &g);
        GdipSetSmoothingMode(g, SmoothingModeAntiAlias);
        GdipSetPixelOffsetMode(g, PixelOffsetModeHalf);
        D = GetDpiForWindow(h) / 96.0f;
        { uint32_t *px = bits; int i, n = bw * bh; for (i = 0; i < n; i++) px[i] = 0; }     /* DONKER */
        teken(bw, bh);
        GdipDeleteGraphics(g);
        GdipDisposeImage((GpImage *)gb);
        GdiFlush();
        BitBlt(echt, 0, 0, bw, bh, mem, 0, 0, SRCCOPY);
        meter_getekend();
    }
    EndPaint(h, &ps);
}

static void klik(HWND h, int x, int y)
{
    POINT p = { x, y };
    int i, id = 0;
    for (i = nraak - 1; i >= 0 && !id; i--) if (PtInRect(&raak[i].r, p)) id = raak[i].id;
    if (id >= ID_SNEL && id < ID_SNEL + 6) zet_snelheid(snelheid[id - ID_SNEL]);
    else if (id >= ID_KOP && id < ID_KOP + 6) { sorteer = id - ID_KOP; rol = 0; }
    else if (id == ID_STOP) beeindig();
    else if (id == ID_TAAL) {
        taal = !taal;
        melding[0] = 0;
        RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\gpu-venster", L"taal", REG_DWORD, &taal, sizeof taal);
    }
    else if (id >= ID_RIJ) {
        int rij = id - ID_RIJ;     /* de rij zoals hij het laatst getekend is */
        if (rij < rij_n) {
            if (keus_pid == rij_pid[rij] && keus_gpu == rij_gpu[rij]) keus_pid = -1;
            else { keus_pid = rij_pid[rij]; keus_gpu = rij_gpu[rij]; }
        }
    } else keus_pid = -1;
    InvalidateRect(h, NULL, FALSE);
}

static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_TIMER: InvalidateRect(h, NULL, FALSE); return 0;    /* toont WACHT als de metingen stoppen */
    case WM_PAINT: schilder(h); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_LBUTTONDOWN: klik(h, GET_X_LPARAM(l), GET_Y_LPARAM(l)); return 0;
    case WM_KEYDOWN:
        if (w == VK_DELETE && keus_pid > 0) { beeindig(); InvalidateRect(h, NULL, FALSE); }
        if (w == VK_ESCAPE) { keus_pid = -1; InvalidateRect(h, NULL, FALSE); }
        return 0;
    case WM_MOUSEWHEEL: rol -= GET_WHEEL_DELTA_WPARAM(w) / WHEEL_DELTA * 2; InvalidateRect(h, NULL, FALSE); return 0;
    case WM_GETMINMAXINFO: {
        float d = GetDpiForWindow(h) / 96.0f;
        ((MINMAXINFO *)l)->ptMinTrackSize.x = (LONG)(760 * d);
        ((MINMAXINFO *)l)->ptMinTrackSize.y = (LONG)(560 * d);
        return 0;
    }
    case WM_DPICHANGED: {
        RECT *r = (RECT *)l;
        SetWindowPos(h, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE vorige, PWSTR opdracht, int toon)
{
    GdiplusStartupInput gsi = { 1, NULL, FALSE, FALSE };
    ULONG_PTR gtoken;
    WNDCLASSEXW wc = { sizeof wc };
    BOOL aan = TRUE;
    COLORREF balk_kleur = RGB(0, 0, 0);
    DWORD ms = 0, n = sizeof ms;
    MSG msg;
    RECT werk;
    float d;
    int b, hgt, i;
    (void)vorige; (void)opdracht;

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    GdiplusStartup(&gtoken, &gsi, NULL);
    if (!RegGetValueW(HKEY_CURRENT_USER, L"Software\\gpu-venster", L"interval_ms", RRF_RT_REG_DWORD, NULL, &ms, &n))
        for (i = 0; i < 6; i++) if ((int)ms == snelheid[i]) interval_ms = (LONG)ms;
    /* taal: de laatste keuze, en anders de taal van Windows (Nederlands, of anders Engels) */
    taal = PRIMARYLANGID(GetUserDefaultUILanguage()) != LANG_DUTCH;
    n = sizeof ms;
    if (!RegGetValueW(HKEY_CURRENT_USER, L"Software\\gpu-venster", L"taal", RRF_RT_REG_DWORD, NULL, &ms, &n)) taal = ms != 0;

    wc.lpfnWndProc = proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hbrBackground = CreateSolidBrush(balk_kleur);
    wc.lpszClassName = L"gpu-venster";
    RegisterClassExW(&wc);
    d = GetDpiForSystem() / 96.0f;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &werk, 0);
    b = (int)(1240 * d); hgt = (int)(900 * d);
    if (b > werk.right - werk.left) b = werk.right - werk.left;
    if (hgt > werk.bottom - werk.top) hgt = werk.bottom - werk.top;
    venster = CreateWindowExW(0, wc.lpszClassName, L"GPUINT", WS_OVERLAPPEDWINDOW,
                              werk.left + (werk.right - werk.left - b) / 2, werk.top + (werk.bottom - werk.top - hgt) / 2,
                              b, hgt, NULL, NULL, inst, NULL);
    DwmSetWindowAttribute(venster, 20 /* donkere titelbalk */, &aan, sizeof aan);
    DwmSetWindowAttribute(venster, 35 /* kleur titelbalk */, &balk_kleur, sizeof balk_kleur);
    DwmSetWindowAttribute(venster, 34 /* kleur rand */, &balk_kleur, sizeof balk_kleur);
    SetTimer(venster, 1, 1000, NULL);
    if (!meter_start(venster, WM_APP, interval_ms == 16 ? 0 : interval_ms)) geen_kaart = 1;
    ShowWindow(venster, toon);
    while (GetMessageW(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    GdiplusShutdown(gtoken);
    return 0;
}
