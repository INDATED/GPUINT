/* Maakt de iconen: een dikke gouden G op een donker vlak, zoals de S van SIGINT en de N van NAVINT
 * (gemeten in hun iconen: vlak 0A0E18, letter C9A227, hoogte van de letter 0,42 van het vlak, in het midden).
 *   gpu-venster.ico                              voor Windows (afgeronde hoeken)
 *   android/res/mipmap-.../ic_launcher.png       voor Android (vol vierkant; Android knipt zelf de vorm)
 * Alleen nodig bij het bouwen. De letter komt uit Windows (Arial vet), 8 keer te groot getekend en dan verkleind. */
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static void w16(FILE *f, unsigned v) { fputc(v & 255, f); fputc(v >> 8 & 255, f); }
static void w32(FILE *f, unsigned v) { w16(f, v & 0xFFFF); w16(f, v >> 16); }

enum { SS = 8 };

/* ligt plek (u, v) in 0..1 in het afgeronde vlak? */
static int in_vlak(double u, double v)
{
    double rad = 0.16, cx = u < rad ? rad : u > 1 - rad ? 1 - rad : u, cy = v < rad ? rad : v > 1 - rad ? 1 - rad : v;
    return (u - cx) * (u - cx) + (v - cy) * (v - cy) <= rad * rad;
}

/* Tekent het icoon van s x s punten in rgba (bovenste rij eerst). afgerond = hoeken rond en doorzichtig. */
static void maak(int s, int afgerond, unsigned char *rgba)
{
    int g = s * SS, x0 = g, x1 = 0, y0 = g, y1 = 0, dx, dy, x, y, a, c;
    BITMAPINFO bi = { { sizeof(BITMAPINFOHEADER), g, -g, 1, 32, BI_RGB } };
    uint32_t *px;
    HDC dc = CreateCompatibleDC(NULL);
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)&px, NULL, 0);
    /* kleine iconen op Windows: de letter groter, anders is hij niet te lezen */
    HFONT font = CreateFontW(-(int)((afgerond && s <= 24 ? 0.82 : 0.587) * g), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, NONANTIALIASED_QUALITY, 0, L"Arial");
    SelectObject(dc, bmp); SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(255, 255, 255));
    TextOutW(dc, g / 4, g / 8, L"G", 1);
    GdiFlush();
    for (y = 0; y < g; y++) for (x = 0; x < g; x++) if (px[y * g + x] & 0xFFFFFF) {
        if (x < x0) x0 = x;
        if (x > x1) x1 = x;
        if (y < y0) y0 = y;
        if (y > y1) y1 = y;
    }
    dx = (x0 + x1 + 1) / 2 - g / 2;     /* de letter precies in het midden */
    dy = (y0 + y1 + 1) / 2 - g / 2;
    for (y = 0; y < s; y++) for (x = 0; x < s; x++) {
        int vlak = 0, letter = 0;
        double d;
        unsigned char *p = rgba + (y * s + x) * 4;
        for (a = 0; a < SS; a++) for (c = 0; c < SS; c++) {
            int gx = x * SS + c + dx, gy = y * SS + a + dy;
            if (afgerond && !in_vlak((x + (c + 0.5) / SS) / s, (y + (a + 0.5) / SS) / s)) continue;
            vlak++;
            if (gx >= 0 && gy >= 0 && gx < g && gy < g && (px[gy * g + gx] & 0xFFFFFF)) letter++;
        }
        d = vlak ? (double)letter / vlak : 0;
        p[0] = (unsigned char)(0x0A + (0xC9 - 0x0A) * d);
        p[1] = (unsigned char)(0x0E + (0xA2 - 0x0E) * d);
        p[2] = (unsigned char)(0x18 + (0x27 - 0x18) * d);
        p[3] = (unsigned char)(vlak * 255 / (SS * SS));
    }
    DeleteObject(font); DeleteDC(dc); DeleteObject(bmp);
}

/* ---- PNG, zonder samenpersen (deflate met "opgeslagen" blokken): genoeg voor een klein icoon ---- */
static unsigned crc_tabel[256];
static unsigned crc(unsigned c, const unsigned char *d, size_t n)
{
    size_t i;
    if (!crc_tabel[1]) { unsigned k, j; for (k = 0; k < 256; k++) { unsigned v = k; for (j = 0; j < 8; j++) v = v & 1 ? 0xEDB88320u ^ (v >> 1) : v >> 1; crc_tabel[k] = v; } }
    for (i = 0; i < n; i++) c = crc_tabel[(c ^ d[i]) & 255] ^ (c >> 8);
    return c;
}
static void be32(unsigned char *p, unsigned v) { p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16); p[2] = (unsigned char)(v >> 8); p[3] = (unsigned char)v; }
static void blok(FILE *f, const char *soort, const unsigned char *d, unsigned n)
{
    unsigned char k[4];
    unsigned c;
    be32(k, n); fwrite(k, 1, 4, f);
    fwrite(soort, 1, 4, f);
    if (n) fwrite(d, 1, n, f);
    c = crc(0xFFFFFFFFu, (const unsigned char *)soort, 4);
    c = crc(c, d, n) ^ 0xFFFFFFFFu;
    be32(k, c); fwrite(k, 1, 4, f);
}
static int schrijf_png(const char *pad, int s, const unsigned char *rgba)
{
    static const unsigned char kop[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    unsigned rij = (unsigned)s * 4 + 1, ruw_n = rij * (unsigned)s, n = 0, a = 1, b = 0, i, plek = 0;
    unsigned char ihdr[13], *ruw = malloc(ruw_n), *z = malloc(ruw_n + ruw_n / 65535 * 5 + 16);
    FILE *f = fopen(pad, "wb");
    if (!f || !ruw || !z) return 0;
    for (i = 0; i < (unsigned)s; i++) { ruw[i * rij] = 0; memcpy(ruw + i * rij + 1, rgba + i * (unsigned)s * 4, (size_t)s * 4); }
    z[n++] = 0x78; z[n++] = 0x01;
    while (plek < ruw_n) {
        unsigned deel = ruw_n - plek > 65535 ? 65535 : ruw_n - plek;
        z[n++] = plek + deel == ruw_n; z[n++] = deel & 255; z[n++] = deel >> 8; z[n++] = ~deel & 255; z[n++] = (~deel >> 8) & 255;
        memcpy(z + n, ruw + plek, deel); n += deel; plek += deel;
    }
    for (i = 0; i < ruw_n; i++) { a = (a + ruw[i]) % 65521; b = (b + a) % 65521; }
    be32(z + n, b << 16 | a); n += 4;
    fwrite(kop, 1, 8, f);
    be32(ihdr, (unsigned)s); be32(ihdr + 4, (unsigned)s); ihdr[8] = 8; ihdr[9] = 6; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    blok(f, "IHDR", ihdr, 13);
    blok(f, "IDAT", z, n);
    blok(f, "IEND", NULL, 0);
    fclose(f); free(ruw); free(z);
    return 1;
}

int main(void)
{
    static const int maat[] = { 16, 20, 24, 32, 40, 48, 64, 256 };
    static const struct { const char *map; int s; } droid[] = { { "mdpi", 48 }, { "hdpi", 72 }, { "xhdpi", 96 }, { "xxhdpi", 144 }, { "xxxhdpi", 192 } };
    enum { N = 8 };
    static unsigned char rgba[256 * 256 * 4];
    FILE *f = fopen("gpu-venster.ico", "wb");
    unsigned plek = 6 + 16 * N;
    int i, x, y;
    if (!f) return 1;
    w16(f, 0); w16(f, 1); w16(f, N);
    for (i = 0; i < N; i++) {
        int s = maat[i];
        unsigned n = 40 + (unsigned)s * s * 4 + (unsigned)((s + 31) / 32 * 4) * s;
        fputc(s & 255, f); fputc(s & 255, f); fputc(0, f); fputc(0, f);
        w16(f, 1); w16(f, 32); w32(f, n); w32(f, plek);
        plek += n;
    }
    for (i = 0; i < N; i++) {
        int s = maat[i];
        maak(s, 1, rgba);
        w32(f, 40); w32(f, s); w32(f, s * 2); w16(f, 1); w16(f, 32); w32(f, 0); w32(f, 0); w32(f, 0); w32(f, 0); w32(f, 0); w32(f, 0);
        for (y = s - 1; y >= 0; y--) for (x = 0; x < s; x++) {      /* ICO: onderste rij eerst, volgorde B G R A */
            const unsigned char *p = rgba + (y * s + x) * 4;
            fputc(p[2], f); fputc(p[1], f); fputc(p[0], f); fputc(p[3], f);
        }
        for (y = 0; y < s; y++) for (x = 0; x < (s + 31) / 32 * 4; x++) fputc(0, f);
    }
    fclose(f);
    if (GetFileAttributesA("android") != INVALID_FILE_ATTRIBUTES) {     /* de map android bestaat: ook de iconen voor Android */
        CreateDirectoryA("android\\res", NULL);
        for (i = 0; i < 5; i++) {
            char pad[96];
            snprintf(pad, sizeof pad, "android\\res\\mipmap-%s", droid[i].map);
            CreateDirectoryA(pad, NULL);
            snprintf(pad, sizeof pad, "android\\res\\mipmap-%s\\ic_launcher.png", droid[i].map);
            maak(droid[i].s, 0, rgba);
            if (!schrijf_png(pad, droid[i].s, rgba)) return 1;
        }
    }
    return 0;
}
