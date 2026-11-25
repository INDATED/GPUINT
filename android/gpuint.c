/* GPUINT voor Android: dezelfde stijl en dezelfde opzet als de versie voor Windows, in C.
 * Een NativeActivity zonder Java. Tekenen: zelf, punt voor punt, in het beeld van het venster (ANativeWindow).
 * Tekst: het lettertype van het systeem (Roboto), getekend met stb_truetype.
 * Meten: bestanden van de kernel die een gewone app mag lezen (gemeten op een Galaxy S26 Ultra, Adreno 840):
 *   /sys/class/kgsl/kgsl-3d0   belasting, klok, temperatuur en afknijpen van de kaart
 *   /sys/class/kgsl/kgsl       videogeheugen van alle apps samen
 *   /sys/devices/system/cpu    klok en rusttijd per kern
 *   /sys/class/thermal         temperaturen
 *   /proc/meminfo              RAM en wisselgeheugen
 * Android laat een app NIET zien welke andere apps de kaart gebruiken. Daarom is er hier geen proceslijst. */
#include <android/native_activity.h>
#include <android/native_window.h>
#include <android/input.h>
#include <android/configuration.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <math.h>
#include <dlfcn.h>
#include <jni.h>
#include <sys/system_properties.h>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"

/* kleuren van SIGINT, 0xRRGGBBAA */
#define GOUD 0xE0B13AFFu
#define GROEN 0x2FD6A0FFu
#define GRIJS 0x8A9BB0FFu
#define LICHT 0xC3CEDEFFu
#define WIT 0xE0E8F0FFu
#define HEMEL 0x4A9BF0FFu
#define ROOD 0xFF3333FFu
#define ORANJE 0xFF9933FFu
#define ZWART 0x000000FFu
#define LIJNTJE 0xFFFFFF12u
#define SPOOR 0xFFFFFF0Du

enum { LINKS, MIDDEN, RECHTS };
#define HIST 300
#define MAXKERN 16

static int taal;                        /* 0 = Nederlands, 1 = Engels */
#define T(nl_, en_) (taal ? (en_) : (nl_))

/* ---------- tijd en bestanden ---------- */
static double nu_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}
static int lees(const char *pad, char *buf, int n)
{
    int f = open(pad, O_RDONLY | O_CLOEXEC), k;
    if (f < 0) { buf[0] = 0; return -1; }
    k = (int)read(f, buf, (size_t)n - 1);
    close(f);
    if (k < 0) k = 0;
    buf[k] = 0;
    while (k > 0 && (buf[k - 1] == '\n' || buf[k - 1] == ' ')) buf[--k] = 0;
    return k;
}
static double lees_getal(const char *pad)       /* -1 als het bestand er niet is of niet gelezen mag worden */
{
    char b[64];
    return lees(pad, b, sizeof b) > 0 ? strtod(b, NULL) : -1;
}

/* ---------- meten ---------- */
typedef struct {
    char model[48], soc[32];
    double util, klok, klok_min, klok_max, temp, mem, afknijpen, stap;
    double cpu, kern[MAXKERN], kern_mhz[MAXKERN], kern_max[MAXKERN];
    int nkern;
    double ram_tot, ram_vrij, wissel_tot, wissel_vrij;
    double t_cpu, t_gpu, t_accu, t_ddr, t_huis;
} Stand;

static Stand stand;
static float h_util[HIST], h_klok[HIST];
static int h_n;
static double per_s;
#define KGSL "/sys/class/kgsl/kgsl-3d0/"

/* temperatuurzones: bij de start een keer de namen lezen, daarna alleen de waarden */
enum { Z_CPU, Z_GPU, Z_ACCU, Z_DDR, Z_HUIS, Z_N };
static struct { int nr, groep; } zone[160];
static int nzone;

static void zones_zoek(void)
{
    char pad[80], t[48];
    int i;
    for (i = 0; i < 200 && nzone < 160; i++) {
        int g = -1;
        snprintf(pad, sizeof pad, "/sys/class/thermal/thermal_zone%d/type", i);
        if (lees(pad, t, sizeof t) <= 0) continue;
        if (!strncmp(t, "cpu", 3) && !strstr(t, "trip")) g = Z_CPU;
        else if (!strncmp(t, "gpu", 3)) g = Z_GPU;
        else if (!strcmp(t, "battery")) g = Z_ACCU;
        else if (!strcmp(t, "ddr")) g = Z_DDR;
        else if (!strncmp(t, "sys-therm", 9) || !strncmp(t, "skin", 4)) g = Z_HUIS;
        if (g >= 0) { zone[nzone].nr = i; zone[nzone++].groep = g; }
    }
}

static void meet(Stand *s, double dt_ms)
{
    static double idle_vorig[MAXKERN], temp_ms = -1e9, traag_ms = -1e9, t[Z_N] = { -1, -1, -1, -1, -1 };
    static int eerste = 1;
    char b[4096], pad[96];
    double nu = nu_ms(), busy = 0, tot = 0, som = 0;
    int i, j, n = 0;

    if (eerste) {
        lees(KGSL "gpu_model", s->model, sizeof s->model);
        __system_property_get("ro.soc.model", s->soc);
        /* laagste en hoogste klok: uit de tabel met alle standen (min_clock_mhz is een grens die steeds verschuift) */
        s->klok_min = s->klok_max = -1;
        if (lees(KGSL "freq_table_mhz", b, sizeof b) > 0 || lees("/sys/kernel/gpu/gpu_freq_table", b, sizeof b) > 0) {
            char *p = b, *e;
            for (;;) {
                double v = strtod(p, &e);
                if (e == p) break;
                if (s->klok_min < 0 || v < s->klok_min) s->klok_min = v;
                if (v > s->klok_max) s->klok_max = v;
                p = e;
            }
        }
        if (s->klok_max < 0) { s->klok_min = lees_getal(KGSL "min_clock_mhz"); s->klok_max = lees_getal(KGSL "max_clock_mhz"); }
        if (lees("/sys/devices/system/cpu/present", b, sizeof b) > 0) { const char *p = strchr(b, '-'); s->nkern = p ? atoi(p + 1) + 1 : 1; }
        if (s->nkern > MAXKERN) s->nkern = MAXKERN;
        for (i = 0; i < s->nkern; i++) {
            snprintf(pad, sizeof pad, "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", i);
            s->kern_max[i] = lees_getal(pad) / 1000;
        }
        zones_zoek();
        s->util = -1;
    }
    /* kaart: gpubusy = "tijd bezig" en "tijd totaal" van het laatste tijdvak; de kernel vernieuwt dit ongeveer 1 keer per seconde */
    if (lees(KGSL "gpubusy", b, sizeof b) > 0 && sscanf(b, "%lf %lf", &busy, &tot) == 2 && tot > 0) s->util = 100 * busy / tot;
    else if (lees(KGSL "gpu_busy_percentage", b, sizeof b) > 0) s->util = strtod(b, NULL);
    s->klok = lees_getal(KGSL "clock_mhz");
    /* kernen: klok, en belasting uit de rusttijd (cpuidle) */
    for (i = 0; i < s->nkern; i++) {
        double idle = 0, v;
        snprintf(pad, sizeof pad, "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", i);
        s->kern_mhz[i] = lees_getal(pad) / 1000;
        for (j = 0; j < 6; j++) {
            snprintf(pad, sizeof pad, "/sys/devices/system/cpu/cpu%d/cpuidle/state%d/time", i, j);
            if ((v = lees_getal(pad)) < 0) break;
            idle += v;
        }
        if (!eerste && dt_ms > 0 && j > 0) {
            double bezig = 100.0 * (1.0 - (idle - idle_vorig[i]) / (dt_ms * 1000.0));
            s->kern[i] = bezig < 0 ? 0 : bezig > 100 ? 100 : bezig;
            som += s->kern[i]; n++;
        }
        idle_vorig[i] = idle;
    }
    if (n) s->cpu = som / n;
    /* wat langzaam verandert: twee keer per seconde */
    if (nu - traag_ms >= 500) {
        const char *p;
        traag_ms = nu;
        s->temp = lees_getal(KGSL "temp") / 1000;
        s->afknijpen = lees_getal(KGSL "throttling");
        s->stap = lees_getal(KGSL "thermal_pwrlevel");
        s->mem = lees_getal("/sys/class/kgsl/kgsl/page_alloc");
        if (lees("/proc/meminfo", b, sizeof b) > 0) {
            if ((p = strstr(b, "MemTotal:"))) s->ram_tot = strtod(p + 9, NULL) * 1024;
            if ((p = strstr(b, "MemAvailable:"))) s->ram_vrij = strtod(p + 13, NULL) * 1024;
            if ((p = strstr(b, "SwapTotal:"))) s->wissel_tot = strtod(p + 10, NULL) * 1024;
            if ((p = strstr(b, "SwapFree:"))) s->wissel_vrij = strtod(p + 9, NULL) * 1024;
        }
    }
    if (nu - temp_ms >= 1000) {     /* temperaturen: per groep de hoogste zone */
        temp_ms = nu;
        for (i = 0; i < Z_N; i++) t[i] = -1;
        for (i = 0; i < nzone; i++) {
            double v;
            snprintf(pad, sizeof pad, "/sys/class/thermal/thermal_zone%d/temp", zone[i].nr);
            v = lees_getal(pad) / 1000;
            if (v > t[zone[i].groep] && v < 200) t[zone[i].groep] = v;
        }
    }
    s->t_cpu = t[Z_CPU]; s->t_gpu = t[Z_GPU]; s->t_accu = t[Z_ACCU]; s->t_ddr = t[Z_DDR]; s->t_huis = t[Z_HUIS];
    eerste = 0;
}

/* ---------- tekenen: zelf, in het beeld ---------- */
static uint32_t *px_beeld;      /* een punt is 4 bytes: R, G, B, X (WINDOW_FORMAT_RGBX_8888) */
static int px_b, px_h, px_rij;  /* px_rij: punten per rij in het geheugen (kan breder zijn dan het beeld) */
static float D;                 /* punten per dp */

static void meng(int x, int y, uint32_t k, float a)
{
    uint32_t d, *p;
    int ia;
    if (x < 0 || y < 0 || x >= px_b || y >= px_h || a <= 0) return;
    ia = a >= 1 ? 256 : (int)(a * 256);
    p = &px_beeld[y * px_rij + x];
    d = *p;
    *p = 0xFF000000u
       | (((((d >> 16) & 255) * (256 - ia) + ((k >> 8) & 255) * ia) >> 8) << 16)     /* B */
       | (((((d >> 8) & 255) * (256 - ia) + ((k >> 16) & 255) * ia) >> 8) << 8)      /* G */
       | ((((d & 255) * (256 - ia) + (k >> 24) * ia) >> 8));                          /* R */
}
/* Een stuk van een rij in een keer, zonder rekenwerk per punt. ia = dekking 0..256. */
static void streep(int y, int x0, int x1, uint32_t k, int ia)
{
    uint32_t kr = k >> 24, kg = (k >> 16) & 255, kb = (k >> 8) & 255, *p;
    if (y < 0 || y >= px_h || ia <= 0) return;
    if (x0 < 0) x0 = 0;
    if (x1 > px_b) x1 = px_b;
    if (x1 <= x0) return;
    p = px_beeld + y * px_rij + x0;
    if (ia >= 256) { uint32_t v = 0xFF000000u | kb << 16 | kg << 8 | kr; for (; x0 < x1; x0++) *p++ = v; }
    else for (; x0 < x1; x0++, p++) {
        uint32_t d = *p;
        *p = 0xFF000000u | ((((d >> 16) & 255) * (256 - ia) + kb * ia) >> 8) << 16 | ((((d >> 8) & 255) * (256 - ia) + kg * ia) >> 8) << 8
           | (((d & 255) * (256 - ia) + kr * ia) >> 8);
    }
}
static int klem_y0 = -1000000, klem_y1 = 1000000;      /* rond tekent alleen de rijen hiertussen */

/* Afgerond vlak met zachte hoeken. Per rij: het midden als een streep, alleen de randen punt voor punt. */
static void rond(float x, float y, float b, float h, float r, uint32_t k)
{
    float a = (k & 255) / 255.0f;
    int x0 = (int)floorf(x), y0 = (int)floorf(y), x1 = (int)ceilf(x + b), y1 = (int)ceilf(y + h), px, py;
    if (b <= 0 || h <= 0 || y1 < 0 || y0 >= px_h || !(k & 255)) return;
    if (r > h / 2) r = h / 2;
    if (r > b / 2) r = b / 2;
    if (y0 < 0) y0 = 0;
    if (y1 > px_h) y1 = px_h;
    if (y0 < klem_y0) y0 = klem_y0;
    if (y1 > klem_y1) y1 = klem_y1;
    for (py = y0; py < y1; py++) {
        float cy = py + 0.5f, dy = cy < y + r ? y + r - cy : cy > y + h - r ? cy - (y + h - r) : 0, vy = 1, in;
        int m0, m1;
        if (cy - y < 0.5f) vy = cy - y + 0.5f; else if (y + h - cy < 0.5f) vy = y + h - cy + 0.5f;
        if (vy <= 0) continue;
        in = dy > 0 ? r - sqrtf(r * r - dy * dy > 0 ? r * r - dy * dy : 0) : 0;     /* hoe ver de ronding deze rij insnijdt */
        m0 = (int)ceilf(x + in) + 1; m1 = (int)floorf(x + b - in) - 1;
        if (m1 > m0) streep(py, m0, m1, k, (int)(a * vy * 256));
        else m0 = m1 = (x0 + x1) / 2;
        for (px = x0; px < x1; px++) {
            float cx, dx, dek;
            if (px == m0) { px = m1; if (px >= x1) break; }
            cx = px + 0.5f; dx = cx < x + r ? x + r - cx : cx > x + b - r ? cx - (x + b - r) : 0;
            if (dx > 0 && dy > 0) { dek = r - sqrtf(dx * dx + dy * dy) + 0.5f; if (dek <= 0) continue; if (dek > 1) dek = 1; }
            else {
                float vx = 1;
                if (cx - x < 0.5f) vx = cx - x + 0.5f; else if (x + b - cx < 0.5f) vx = x + b - cx + 0.5f;
                dek = vx * vy;
            }
            meng(px, py, k, a * dek);
        }
    }
}
/* Vlak met vulling en een rand van 1 punt. De achtergrond is zwart. Alleen de stroken boven en onder (met de
 * ronde hoeken) worden als vlak getekend; daartussen is de rand twee punten per rij. Dat scheelt het vullen van
 * het hele paneel bij elk beeld. */
static void vlak(float x, float y, float b, float h, float r, uint32_t vul, uint32_t rand)
{
    int strook, py, xa, xb, deel;
    float ri = r - 1 > 0 ? r - 1 : 0;
    if (b <= 0 || h <= 0 || y + h < 0 || y > px_h) return;
    if (!(rand & 255)) { rond(x, y, b, h, r, vul); return; }
    if (r > h / 2) r = h / 2;
    strook = (int)ceilf(r) + 2;
    for (deel = 0; deel < 2; deel++) {
        klem_y0 = deel ? (int)(y + h) - strook : (int)floorf(y);
        klem_y1 = deel ? (int)ceilf(y + h) : (int)floorf(y) + strook;
        if (deel && klem_y0 < (int)floorf(y) + strook) klem_y0 = (int)floorf(y) + strook;
        rond(x, y, b, h, r, rand);
        rond(x + 1, y + 1, b - 2, h - 2, ri, ZWART);
        if (vul & 255) rond(x + 1, y + 1, b - 2, h - 2, ri, vul);
    }
    klem_y0 = -1000000; klem_y1 = 1000000;
    xa = (int)(x + 0.5f); xb = (int)(x + b + 0.5f);
    for (py = (int)floorf(y) + strook; py < (int)(y + h) - strook; py++) {
        streep(py, xa, xa + 1, rand, (int)(rand & 255) * 256 / 255);
        streep(py, xb - 1, xb, rand, (int)(rand & 255) * 256 / 255);
        if (vul & 255) streep(py, xa + 1, xb - 1, vul, (int)(vul & 255) * 256 / 255);
    }
}
static void balk(float x, float y, float b, float h, double deel, uint32_t kleur)
{
    if (deel < 0) deel = 0;
    if (deel > 1) deel = 1;
    rond(x, y, b, h, h / 2, SPOOR);
    if (deel > 0) rond(x, y, (float)(b * deel) < h ? h : (float)(b * deel), h, h / 2, kleur);
}
static uint32_t dun(uint32_t k, unsigned alfa) { return (k & 0xFFFFFF00u) | alfa; }

/* ---------- tekst ---------- */
static stbtt_fontinfo font;
static int font_ok;
typedef struct { int cp, px, b, h, dx, dy; float stap; unsigned char *beeld; } Teken;
static Teken kas[4096];

static void font_laad(void)
{
    static const char *pad[] = { "/system/fonts/RobotoStatic-Regular.ttf", "/system/fonts/Roboto-Regular.ttf", "/system/fonts/DroidSans.ttf" };
    int i;
    for (i = 0; i < 3 && !font_ok; i++) {
        FILE *f = fopen(pad[i], "rb");
        long n;
        unsigned char *d;
        if (!f) continue;
        fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
        d = malloc((size_t)n);
        if (d && fread(d, 1, (size_t)n, f) == (size_t)n && stbtt_InitFont(&font, d, stbtt_GetFontOffsetForIndex(d, 0))) font_ok = 1;
        fclose(f);
    }
}
static Teken *teken_van(int cp, int px)
{
    unsigned i = ((unsigned)cp * 2654435761u + (unsigned)px * 40503u) & 4095;
    float schaal;
    int stap, lsb;
    for (;; i = (i + 1) & 4095) {
        if (kas[i].px == px && kas[i].cp == cp) return &kas[i];
        if (!kas[i].px) break;
    }
    schaal = stbtt_ScaleForMappingEmToPixels(&font, (float)px);
    kas[i].cp = cp; kas[i].px = px;
    kas[i].beeld = stbtt_GetCodepointBitmap(&font, schaal, schaal, cp, &kas[i].b, &kas[i].h, &kas[i].dx, &kas[i].dy);
    stbtt_GetCodepointHMetrics(&font, cp, &stap, &lsb);
    kas[i].stap = stap * schaal;
    return &kas[i];
}
/* Tekst in een vak (verticaal in het midden); te lang = afgekapt met puntjes. Geeft de breedte in punten. */
static float tekst(float x, float y, float b, float h, const char *t, float dp, float spatie, uint32_t kleur, int lijn, int vet)
{
    int cp[200], n = 0, px = (int)(dp * D + 0.5f), i, op, neer, gat, bx, by;
    float br = 0, sp = spatie * D, schaal, a = (kleur & 255) / 255.0f, basis;
    const unsigned char *s = (const unsigned char *)t;
    if (!font_ok || b <= 0 || px < 4) return 0;
    while (*s && n < 196) {     /* UTF-8 naar tekens */
        int c = *s++;
        if (c >= 0xF0) { c = (c & 7) << 18 | (s[0] & 63) << 12 | (s[1] & 63) << 6 | (s[2] & 63); s += 3; }
        else if (c >= 0xE0) { c = (c & 15) << 12 | (s[0] & 63) << 6 | (s[1] & 63); s += 2; }
        else if (c >= 0xC0) { c = (c & 31) << 6 | (s[0] & 63); s += 1; }
        cp[n++] = c;
    }
    if (!n) return 0;
    for (i = 0; i < n; i++) br += teken_van(cp[i], px)->stap + sp;
    while (br > b && n > 1) {   /* te breed: een teken eraf, puntjes erachter */
        br -= teken_van(cp[n - 1], px)->stap + sp;
        if (cp[n - 1] != 0x2026) { n--; if (n > 0) { br -= teken_van(cp[n - 1], px)->stap + sp; cp[n - 1] = 0x2026; br += teken_van(0x2026, px)->stap + sp; } }
        else { n--; if (n > 0) { br -= teken_van(cp[n - 1], px)->stap + sp; cp[n - 1] = 0x2026; br += teken_van(0x2026, px)->stap + sp; } }
    }
    if (lijn == MIDDEN) x += (b - br) / 2; else if (lijn == RECHTS) x += b - br;
    stbtt_GetFontVMetrics(&font, &op, &neer, &gat);
    schaal = stbtt_ScaleForMappingEmToPixels(&font, (float)px);
    basis = y + (h - (op - neer) * schaal) / 2 + op * schaal;
    if (basis + px < 0 || basis - px > px_h) return br;
    for (i = 0; i < n; i++) {
        Teken *g = teken_van(cp[i], px);
        int x0 = (int)(x + 0.5f) + g->dx, y0 = (int)(basis + 0.5f) + g->dy, dik;
        for (dik = 0; dik <= (vet ? 1 : 0); dik++)      /* vet: het teken twee keer, een fractie opgeschoven */
            for (by = 0; by < g->h; by++) for (bx = 0; bx < g->b; bx++) {
                int v = g->beeld[by * g->b + bx];
                if (v) meng(x0 + bx + dik * (px >= 40 ? 2 : 1), y0 + by, kleur, a * v / 255.0f);
            }
        x += g->stap + sp;
    }
    return br;
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
static const char *maat(double bytes) { return bytes >= 1073741824.0 ? nl("%.2f GB", bytes / 1073741824.0) : nl("%.0f MB", bytes / 1048576.0); }

/* ---------- bediening ---------- */
enum { ID_SNEL = 1, ID_TAAL = 30 };
static struct { float x, y, b, h; int id; } raak[32];
static int nraak;
static void raakvlak(float x, float y, float b, float h, int id)
{
    if (nraak < 32) { raak[nraak].x = x; raak[nraak].y = y; raak[nraak].b = b; raak[nraak].h = h; raak[nraak++].id = id; }
}
static const int snelheid[6] = { 0, 100, 250, 500, 1000, 2000 };   /* 0 = MAX: een meting bij elk beeld van het scherm */
static int rand_boven, rand_onder;      /* hoogte van de statusbalk en van de navigatieknoppen, in punten */
static float scherm_hz = 60;            /* het hoogste aantal beelden per seconde dat dit scherm kan */
static int hz_gezet = -1;               /* voor welke snelheid het scherm het laatst is ingesteld */
static int interval_ms = 500;
static float rol, rol_max;
static char bewaar_pad[256];

static void bewaar(void)
{
    FILE *f = fopen(bewaar_pad, "w");
    if (f) { fprintf(f, "%d %d\n", taal, interval_ms); fclose(f); }
}

static void kopje(float x, float y, float b, const char *t, const char *rechts)
{
    tekst(x, y, b, 16 * D, t, 10.5f, 2, GOUD, LINKS, 0);
    if (rechts) tekst(x, y, b, 16 * D, rechts, 10.5f, 1, GRIJS, RECHTS, 0);
    rond(x, y + 21 * D, b, 1, 0, 0xE0B13A30u);
}
static uint32_t kleur_bij(double v, double oranje, double rood) { return v >= rood ? ROOD : v >= oranje ? ORANJE : GROEN; }

static void mini(float x, float y, float b, const char *label, const char *waarde, const char *eenheid, double deel, uint32_t kleur)
{
    float wb;
    tekst(x, y, b, 14 * D, label, 9, 1.5f, GRIJS, LINKS, 0);
    wb = tekst(x, y + 15 * D, b, 30 * D, waarde, 22, 0, WIT, LINKS, 1);
    tekst(x + wb + 4 * D, y + 23 * D, b - wb - 4 * D, 20 * D, eenheid, 11, 0, GRIJS, LINKS, 0);
    balk(x, y + 49 * D, b, 4 * D, deel, kleur);
}

static void lijn_grafiek(const float *v, int n, float x, float y, float b, float h, uint32_t kleur, int vullen)
{
    float dik = 1.0f * D, stap = (float)(HIST - 1) / b, onder = y + h;
    int c, c1 = (int)(x + b), r, i;
    if (n < 2) return;
    for (c = (int)(x + b * (float)(HIST - n) / (HIST - 1)); c < c1; c++) {
        float f0 = (c - x) * stap - (HIST - n), f1 = f0 + stap, lo = 1e9f, hi = -1e9f, w, top, bot;
        if (f0 < 0) f0 = 0;
        if (f1 > n - 1) f1 = (float)(n - 1);
        if (f1 < f0) continue;
        for (i = (int)f0 - 1; i <= (int)f1 + 1; i++) {
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

static void temp_rij(float x, float *y, float b, const char *naam, double v)
{
    if (v < 0) return;
    rond(x, *y, b, 1, 0, LIJNTJE);
    tekst(x, *y, b * 0.5f, 26 * D, naam, 12.5f, 0, LICHT, LINKS, 0);
    balk(x + b * 0.5f, *y + 11 * D, b * 0.5f - 64 * D, 4 * D, v / 100, kleur_bij(v, 60, 80));
    tekst(x, *y, b, 26 * D, nl("%.1f \xC2\xB0""C", v), 12.5f, 0, WIT, RECHTS, 0);
    *y += 26 * D;
}

static void teken(int W, int H, int boven, int live)
{
    Stand *s = &stand;
    float M = 14 * D, b = W - 2 * M, ix = M + 14 * D, ib = b - 28 * D, y = boven + 10 * D - rol, x, mb, y0;
    char t[200];
    int i, ms = interval_ms;
    nraak = 0;

    /* kop: naam, taal, live */
    tekst(M, y, 90 * D, 28 * D, "GPUINT", 13, 3, GOUD, LINKS, 1);
    x = M + 92 * D;
    vlak(x, y + 1 * D, 64 * D, 26 * D, 13 * D, 0, LIJNTJE);
    tekst(x, y + 1 * D, 32 * D, 25 * D, "NL", 10.5f, 1, taal ? GRIJS : GOUD, MIDDEN, !taal);
    tekst(x + 32 * D, y + 1 * D, 32 * D, 25 * D, "EN", 10.5f, 1, taal ? GOUD : GRIJS, MIDDEN, taal);
    raakvlak(x - 6 * D, y - 8 * D, 76 * D, 44 * D, ID_TAAL);
    {
        uint32_t kl = live ? GROEN : ORANJE;
        float px = W - M - 78 * D;
        vlak(px, y, 78 * D, 28 * D, 14 * D, dun(kl, 0x1A), dun(kl, 0x60));
        rond(px + 14 * D, y + 10.5f * D, 7 * D, 7 * D, 3.5f * D, kl);
        tekst(px + 26 * D, y, 48 * D, 27 * D, live ? "LIVE" : T("WACHT", "WAIT"), 10.5f, 2, kl, LINKS, 1);
    }
    /* snelheid */
    y += 40 * D;
    {
        float cw = (b - 5 * 6 * D) / 6;
        for (i = 0; i < 6; i++) {
            int aan = snelheid[i] == ms;
            float cx = M + i * (cw + 6 * D);
            vlak(cx, y, cw, 28 * D, 14 * D, aan ? dun(GOUD, 0x26) : 0, aan ? dun(GOUD, 0x90) : LIJNTJE);
            tekst(cx, y, cw, 27 * D, i ? nl("%.3g s", snelheid[i] / 1000.0) : "MAX", 11, 0, aan ? GOUD : GRIJS, MIDDEN, aan);
            raakvlak(cx - 3 * D, y - 8 * D, cw + 6 * D, 44 * D, ID_SNEL + i);
        }
    }
    y += 36 * D;
    snprintf(t, sizeof t, T("VERVERSEN  %.0f PER S", "REFRESH  %.0f PER S"), per_s);
    tekst(M, y - 4 * D, b, 16 * D, t, 9, 2, GRIJS, RECHTS, 0);
    y += 16 * D;

    /* de kaart */
    y0 = y;
    vlak(M, y, b, 204 * D, 10 * D, ZWART, LIJNTJE);
    tekst(ix, y + 10 * D, ib - 60 * D, 22 * D, s->model[0] ? s->model : T("Grafische kaart", "Graphics card"), 15, 0, WIT, LINKS, 1);
    tekst(ix, y + 10 * D, ib, 22 * D, "GPU 0", 10.5f, 2, GOUD, RECHTS, 0);
    snprintf(t, sizeof t, "Qualcomm Adreno  \xC2\xB7  %s", s->soc);
    tekst(ix, y + 32 * D, ib, 16 * D, s->soc[0] ? t : "", 11, 0, GRIJS, LINKS, 0);
    mb = (ib - 20 * D) / 3;
    y += 54 * D;
    mini(ix, y, mb, T("BELASTING", "LOAD"), s->util < 0 ? "-" : nl("%.0f", s->util), "%", s->util / 100, GROEN);
    mini(ix + mb + 10 * D, y, mb, T("TEMPERATUUR", "TEMPERATURE"), s->temp < 0 ? "-" : nl("%.0f", s->temp), s->temp < 0 ? "" : "\xC2\xB0""C", s->temp / 100, kleur_bij(s->temp, 60, 80));
    mini(ix + 2 * (mb + 10 * D), y, mb, T("KLOK", "CLOCK"), s->klok < 0 ? "-" : nl("%.0f", s->klok), s->klok < 0 ? "" : "MHz", s->klok_max > 0 ? s->klok / s->klok_max : 0, HEMEL);
    y += 62 * D;
    tekst(ix, y, ib, 16 * D, T("VIDEOGEHEUGEN", "VIDEO MEMORY"), 9, 1.5f, GRIJS, LINKS, 0);
    snprintf(t, sizeof t, T("%s  \xC2\xB7  gedeeld met RAM", "%s  \xC2\xB7  shared with RAM"), s->mem < 0 ? "-" : maat(s->mem));
    tekst(ix, y, ib, 16 * D, t, 11.5f, 0, WIT, RECHTS, 0);
    balk(ix, y + 21 * D, ib, 8 * D, s->ram_tot > 0 && s->mem > 0 ? s->mem / s->ram_tot : 0, GOUD);
    y += 40 * D;
    snprintf(t, sizeof t, T("%.0f\xE2\x80\x93%.0f MHz   afknijpen: %s   stap %.0f", "%.0f\xE2\x80\x93%.0f MHz   throttling: %s   level %.0f"),
             s->klok_min, s->klok_max, s->afknijpen > 0 ? T("ja", "yes") : T("nee", "no"), s->stap < 0 ? 0 : s->stap);
    tekst(ix, y, ib, 16 * D, t, 10.5f, 0, GRIJS, LINKS, 0);
    tekst(ix, y + 17 * D, ib, 16 * D, T("Belasting: Android geeft 1 nieuwe waarde per seconde.", "Load: Android gives 1 new value per second."), 10.5f, 0, GRIJS, LINKS, 0);
    y = y0 + 214 * D;

    /* verloop */
    vlak(M, y, b, 150 * D, 10 * D, ZWART, LIJNTJE);
    i = HIST * (ms ? ms : (per_s > 1 ? (int)(1000 / per_s) : 16));
    snprintf(t, sizeof t, T("VERLOOP  %d %s", "HISTORY  %d %s"), i >= 60000 ? i / 60000 : (i + 500) / 1000, i >= 60000 ? "MIN" : "S");
    kopje(ix, y + 10 * D, ib, t, NULL);
    {
        float lx = ix + ib, ly = y + 10 * D, lw, px = ix + 26 * D, py = y + 42 * D, pb = ib - 26 * D, ph = 150 * D - 42 * D - 14 * D;
        lw = tekst(lx - 160 * D, ly, 160 * D, 16 * D, T("KLOK", "CLOCK"), 9.5f, 1, GRIJS, RECHTS, 0);
        rond(lx - lw - 18 * D, ly + 7 * D, 12 * D, 2 * D, D, GOUD);
        lx -= lw + 30 * D;
        lw = tekst(lx - 160 * D, ly, 160 * D, 16 * D, T("BELASTING", "LOAD"), 9.5f, 1, GRIJS, RECHTS, 0);
        rond(lx - lw - 18 * D, ly + 7 * D, 12 * D, 2 * D, D, GROEN);
        for (i = 0; i <= 2; i++) {
            float gy = floorf(py + ph * i / 2);
            rond(px, gy, pb, 1, 0, LIJNTJE);
            tekst(ix, gy - 8 * D, 22 * D, 16 * D, i == 0 ? "100" : i == 1 ? "50" : "0", 9, 0, GRIJS, RECHTS, 0);
        }
        lijn_grafiek(h_klok, h_n, px, py, pb, ph, GOUD, 0);
        lijn_grafiek(h_util, h_n, px, py, pb, ph, GROEN, 1);
    }
    y += 160 * D;

    /* systeem */
    y0 = y;
    vlak(M, y, b, 232 * D, 10 * D, ZWART, LIJNTJE);
    kopje(ix, y + 10 * D, ib, T("SYSTEEM", "SYSTEM"), NULL);
    y += 40 * D;
    snprintf(t, sizeof t, T("CPU   %d KERNEN", "CPU   %d CORES"), s->nkern);
    tekst(ix, y, ib - 70 * D, 18 * D, t, 9, 1.5f, GRIJS, LINKS, 0);
    tekst(ix, y, ib, 18 * D, nl("%.1f %%", s->cpu), 13, 0, WIT, RECHTS, 1);
    balk(ix, y + 21 * D, ib, 4 * D, s->cpu / 100, kleur_bij(s->cpu, 70, 90));
    y += 34 * D;
    if (s->nkern > 0) {     /* per kern: een balk met de belasting, eronder de klok in GHz */
        float kb = (ib - (s->nkern - 1) * 6 * D) / s->nkern, kh = 40 * D;
        for (i = 0; i < s->nkern; i++) {
            float kx = ix + i * (kb + 6 * D), vh = (float)(kh * s->kern[i] / 100);
            rond(kx, y, kb, kh, 3 * D, SPOOR);
            if (vh > 1) rond(kx, y + kh - vh, kb, vh, 3 * D, kleur_bij(s->kern[i], 70, 90));
            tekst(kx - 3 * D, y + kh + 2 * D, kb + 6 * D, 14 * D, nl("%.1f", s->kern_mhz[i] / 1000), 9, 0, GRIJS, MIDDEN, 0);
        }
        tekst(ix, y + kh + 16 * D, ib, 12 * D, T("klok per kern in GHz", "clock per core in GHz"), 8.5f, 0, GRIJS, RECHTS, 0);
        y += kh + 34 * D;
    }
    tekst(ix, y, ib, 18 * D, "RAM", 9, 1.5f, GRIJS, LINKS, 0);
    tekst(ix, y, ib, 18 * D, nl("%.1f / %.1f GB", (s->ram_tot - s->ram_vrij) / 1073741824.0, s->ram_tot / 1073741824.0), 13, 0, WIT, RECHTS, 1);
    balk(ix, y + 21 * D, ib, 4 * D, s->ram_tot > 0 ? (s->ram_tot - s->ram_vrij) / s->ram_tot : 0, kleur_bij(s->ram_tot > 0 ? 100 * (s->ram_tot - s->ram_vrij) / s->ram_tot : 0, 80, 92));
    y += 34 * D;
    tekst(ix, y, ib, 18 * D, T("WISSEL", "SWAP"), 9, 1.5f, GRIJS, LINKS, 0);
    tekst(ix, y, ib, 18 * D, nl("%.1f / %.1f GB", (s->wissel_tot - s->wissel_vrij) / 1073741824.0, s->wissel_tot / 1073741824.0), 13, 0, WIT, RECHTS, 1);
    balk(ix, y + 21 * D, ib, 4 * D, s->wissel_tot > 0 ? (s->wissel_tot - s->wissel_vrij) / s->wissel_tot : 0, HEMEL);
    y = y0 + 242 * D;

    /* temperaturen */
    y0 = y;
    vlak(M, y, b, 178 * D, 10 * D, ZWART, LIJNTJE);
    kopje(ix, y + 10 * D, ib, T("TEMPERATUREN", "TEMPERATURES"), T("hoogste per groep", "highest per group"));
    y += 38 * D;
    temp_rij(ix, &y, ib, T("Processor", "Processor"), s->t_cpu);
    temp_rij(ix, &y, ib, T("Grafische kaart", "Graphics card"), s->t_gpu);
    temp_rij(ix, &y, ib, T("Geheugen", "Memory"), s->t_ddr);
    temp_rij(ix, &y, ib, T("Behuizing", "Housing"), s->t_huis);
    temp_rij(ix, &y, ib, T("Accu", "Battery"), s->t_accu);
    y = y0 + 188 * D;

    /* wat Android niet geeft */
    tekst(M + 4 * D, y, b - 8 * D, 16 * D, T("Android laat een app niet zien welke andere apps", "Android does not let an app see which other apps"), 10.5f, 0, GRIJS, LINKS, 0);
    tekst(M + 4 * D, y + 16 * D, b - 8 * D, 16 * D, T("de kaart gebruiken. Daarom is er hier geen proceslijst.", "use the card. So there is no process list here."), 10.5f, 0, GRIJS, LINKS, 0);
    y += 44 * D + rand_onder;       /* ruimte voor de navigatieknoppen van Android */
    rol_max = y + rol - H;
    if (rol_max < 0) rol_max = 0;
}

/* ---------- het venster en de draad ---------- */
static pthread_mutex_t slot = PTHREAD_MUTEX_INITIALIZER;
static ANativeWindow *venster;
static AInputQueue *invoer;
static int loopt = 1, zichtbaar, inhoud_boven;
static ANativeActivity *activiteit;

static void klik(float x, float y)
{
    int i;
    for (i = nraak - 1; i >= 0; i--)
        if (x >= raak[i].x && x < raak[i].x + raak[i].b && y >= raak[i].y && y < raak[i].y + raak[i].h) {
            if (raak[i].id == ID_TAAL) taal = !taal;
            else if (raak[i].id >= ID_SNEL && raak[i].id < ID_SNEL + 6) { interval_ms = snelheid[raak[i].id - ID_SNEL]; h_n = 0; }
            bewaar();
            return;
        }
}

/* Aanraken: tik = klik, slepen = de inhoud schuiven. Geeft 1 als het beeld opnieuw moet. */
static int verwerk_invoer(void)
{
    static float start_y, laatste_y;
    static int sleept, neer;
    AInputEvent *e;
    int iets = 0;
    if (!invoer) return 0;
    while (AInputQueue_getEvent(invoer, &e) >= 0) {
        int klaar = 0;
        if (AInputQueue_preDispatchEvent(invoer, e)) continue;
        if (AInputEvent_getType(e) == AINPUT_EVENT_TYPE_MOTION) {
            int actie = AMotionEvent_getAction(e) & AMOTION_EVENT_ACTION_MASK;
            float x = AMotionEvent_getX(e, 0), y = AMotionEvent_getY(e, 0);
            if (actie == AMOTION_EVENT_ACTION_DOWN) { start_y = laatste_y = y; sleept = 0; neer = 1; }
            else if (actie == AMOTION_EVENT_ACTION_MOVE && neer) {
                if (fabsf(y - start_y) > 10 * D) sleept = 1;
                if (sleept) { rol -= y - laatste_y; if (rol > rol_max) rol = rol_max; if (rol < 0) rol = 0; }
                laatste_y = y;
            } else if (actie == AMOTION_EVENT_ACTION_UP && neer) { if (!sleept) klik(x, y); neer = 0; }
            else if (actie == AMOTION_EVENT_ACTION_CANCEL) neer = 0;
            klaar = 1; iets = 1;
        }
        AInputQueue_finishEvent(invoer, e, klaar);
    }
    return iets;
}

static void *draad(void *arg)
{
    double vorige = nu_ms(), laatste_meting = 0, tel_start = vorige;
    int geteld = 0;
    (void)arg;
    font_laad();
    while (loopt) {
        ANativeWindow_Buffer buf;
        double nu = nu_ms();
        int vuil, tijd;
        pthread_mutex_lock(&slot);
        vuil = verwerk_invoer();
        tijd = nu - vorige >= interval_ms;      /* MAX (0 ms): bij elk beeld */
        if (!venster || !zichtbaar || (!vuil && !tijd)) { pthread_mutex_unlock(&slot); usleep(4000); continue; }
        if (hz_gezet != interval_ms) {      /* MAX: het scherm op zijn hoogste snelheid vragen (120 Hz); anders de keuze aan Android laten */
            int (*zet)(ANativeWindow *, float, int8_t) = (int (*)(ANativeWindow *, float, int8_t))dlsym(RTLD_DEFAULT, "ANativeWindow_setFrameRate");
            if (zet) zet(venster, interval_ms ? 0.0f : scherm_hz, 0);
            hz_gezet = interval_ms;
        }
        if (tijd) {
            meet(&stand, nu - vorige);
            vorige = nu;
            laatste_meting = nu;
            if (h_n == HIST) { memmove(h_util, h_util + 1, sizeof(float) * (HIST - 1)); memmove(h_klok, h_klok + 1, sizeof(float) * (HIST - 1)); h_n--; }
            h_util[h_n] = (float)(stand.util < 0 ? 0 : stand.util);
            h_klok[h_n] = stand.klok_max > 0 ? (float)(100 * stand.klok / stand.klok_max) : 0;
            h_n++;
            if (++geteld && nu - tel_start >= 1000) { per_s = geteld * 1000.0 / (nu - tel_start); geteld = 0; tel_start = nu; }
        }
        if (ANativeWindow_lock(venster, &buf, NULL) == 0) {
            int r;
            px_beeld = buf.bits; px_b = buf.width; px_h = buf.height; px_rij = buf.stride;
            for (r = 0; r < px_h; r++) memset(px_beeld + r * px_rij, 0, (size_t)px_b * 4);     /* zwart; de vierde byte telt niet mee (RGBX) */
            teken(px_b, px_h, inhoud_boven > rand_boven ? inhoud_boven : rand_boven, nu - laatste_meting < 3500);
            /* de statusbalk van Android staat over het beeld: daaronder zwart, zodat schuivende inhoud er niet doorheen komt */
            for (r = 0; r < rand_boven && r < px_h; r++) memset(px_beeld + r * px_rij, 0, (size_t)px_b * 4);
            ANativeWindow_unlockAndPost(venster);   /* wacht op een vrij beeld: dat houdt MAX gelijk met het scherm */
        }
        pthread_mutex_unlock(&slot);
    }
    return NULL;
}

static void op_venster(ANativeActivity *a, ANativeWindow *w)
{
    (void)a;
    pthread_mutex_lock(&slot);
    venster = w;
    ANativeWindow_setBuffersGeometry(w, 0, 0, WINDOW_FORMAT_RGBX_8888);
    zichtbaar = 1;
    hz_gezet = -1;
    pthread_mutex_unlock(&slot);
}
static void venster_weg(ANativeActivity *a, ANativeWindow *w) { (void)a; (void)w; pthread_mutex_lock(&slot); venster = NULL; pthread_mutex_unlock(&slot); }
static void op_invoer(ANativeActivity *a, AInputQueue *q) { (void)a; pthread_mutex_lock(&slot); invoer = q; pthread_mutex_unlock(&slot); }
static void invoer_weg(ANativeActivity *a, AInputQueue *q) { (void)a; (void)q; pthread_mutex_lock(&slot); invoer = NULL; pthread_mutex_unlock(&slot); }
static void op_inhoud(ANativeActivity *a, const ARect *r) { (void)a; pthread_mutex_lock(&slot); inhoud_boven = r->top; pthread_mutex_unlock(&slot); }
static void op_hervat(ANativeActivity *a) { (void)a; zichtbaar = 1; }
static void op_pauze(ANativeActivity *a) { (void)a; zichtbaar = 0; }      /* niet in beeld: niet meten en niet tekenen */

/* Een maat van het systeem in punten (bv. de hoogte van de statusbalk). Dit is de enige plek met JNI: Android geeft
 * deze maat niet aan C-code. Loopt op de hoofddraad, bij de start. */
static int systeem_maat(ANativeActivity *a, const char *naam)
{
    JNIEnv *e = a->env;
    jclass ca = (*e)->GetObjectClass(e, a->clazz), cr;
    jobject res = (*e)->CallObjectMethod(e, a->clazz, (*e)->GetMethodID(e, ca, "getResources", "()Landroid/content/res/Resources;"));
    jint id;
    if (!res) return 0;
    cr = (*e)->GetObjectClass(e, res);
    id = (*e)->CallIntMethod(e, res, (*e)->GetMethodID(e, cr, "getIdentifier", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)I"),
                             (*e)->NewStringUTF(e, naam), (*e)->NewStringUTF(e, "dimen"), (*e)->NewStringUTF(e, "android"));
    return id ? (*e)->CallIntMethod(e, res, (*e)->GetMethodID(e, cr, "getDimensionPixelSize", "(I)I"), id) : 0;
}

/* Het hoogste aantal beelden per seconde van het scherm (bv. 120). Ook JNI, om dezelfde reden. */
static float hoogste_hz(ANativeActivity *a)
{
    JNIEnv *e = a->env;
    jobject wm = (*e)->CallObjectMethod(e, a->clazz, (*e)->GetMethodID(e, (*e)->GetObjectClass(e, a->clazz), "getWindowManager", "()Landroid/view/WindowManager;"));
    jobject d, modes;
    jclass cd;
    float hz;
    int i, n;
    if (!wm) return 60;
    d = (*e)->CallObjectMethod(e, wm, (*e)->GetMethodID(e, (*e)->GetObjectClass(e, wm), "getDefaultDisplay", "()Landroid/view/Display;"));
    if (!d) return 60;
    cd = (*e)->GetObjectClass(e, d);
    hz = (*e)->CallFloatMethod(e, d, (*e)->GetMethodID(e, cd, "getRefreshRate", "()F"));
    modes = (*e)->CallObjectMethod(e, d, (*e)->GetMethodID(e, cd, "getSupportedModes", "()[Landroid/view/Display$Mode;"));
    n = modes ? (*e)->GetArrayLength(e, (jobjectArray)modes) : 0;
    for (i = 0; i < n; i++) {
        jobject m = (*e)->GetObjectArrayElement(e, (jobjectArray)modes, i);
        float v = (*e)->CallFloatMethod(e, m, (*e)->GetMethodID(e, (*e)->GetObjectClass(e, m), "getRefreshRate", "()F"));
        if (v > hz) hz = v;
    }
    if ((*e)->ExceptionCheck(e)) (*e)->ExceptionClear(e);
    return hz > 1 ? hz : 60;
}

void ANativeActivity_onCreate(ANativeActivity *a, void *bewaard, size_t n)
{
    static int gestart;
    AConfiguration *c = AConfiguration_new();
    char land[4] = { 0 };
    FILE *f;
    pthread_t t;
    (void)bewaard; (void)n;
    activiteit = a;
    AConfiguration_fromAssetManager(c, a->assetManager);
    D = AConfiguration_getDensity(c) / 160.0f;
    if (D < 0.5f) D = 2;
    AConfiguration_getLanguage(c, land);
    AConfiguration_delete(c);
    taal = !(land[0] == 'n' && land[1] == 'l');     /* de taal van de telefoon: Nederlands, of anders Engels */
    rand_boven = systeem_maat(a, "status_bar_height");
    rand_onder = systeem_maat(a, "navigation_bar_height");
    scherm_hz = hoogste_hz(a);
    snprintf(bewaar_pad, sizeof bewaar_pad, "%s/keuze.txt", a->internalDataPath);
    if ((f = fopen(bewaar_pad, "r")) != NULL) {
        int tl, ms, i;
        if (fscanf(f, "%d %d", &tl, &ms) == 2) { taal = tl != 0; for (i = 0; i < 6; i++) if (snelheid[i] == ms) interval_ms = ms; }
        fclose(f);
    }
    a->callbacks->onNativeWindowCreated = op_venster;
    a->callbacks->onNativeWindowDestroyed = venster_weg;
    a->callbacks->onInputQueueCreated = op_invoer;
    a->callbacks->onInputQueueDestroyed = invoer_weg;
    a->callbacks->onContentRectChanged = op_inhoud;
    a->callbacks->onResume = op_hervat;
    a->callbacks->onPause = op_pauze;
    if (!gestart) { gestart = 1; pthread_create(&t, NULL, draad, NULL); pthread_detach(t); }
}
