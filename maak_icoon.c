/* Maakt gpu-venster.ico: een dikke gouden G op een donker vlak, zoals de S van SIGINT en de N van NAVINT
 * (gemeten in hun iconen: vlak 0A0E18, letter C9A227, hoogte van de letter 0,42 van het vlak, in het midden).
 * Alleen nodig bij het bouwen. De letter komt uit Windows (Arial vet), 8 keer te groot getekend en dan verkleind. */
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

static void w16(FILE *f, unsigned v) { fputc(v & 255, f); fputc(v >> 8 & 255, f); }
static void w32(FILE *f, unsigned v) { w16(f, v & 0xFFFF); w16(f, v >> 16); }

enum { SS = 8 };

/* ligt plek (u, v) in 0..1 in het afgeronde vlak? */
static int in_vlak(double u, double v)
{
    double rad = 0.16, cx = u < rad ? rad : u > 1 - rad ? 1 - rad : u, cy = v < rad ? rad : v > 1 - rad ? 1 - rad : v;
    return (u - cx) * (u - cx) + (v - cy) * (v - cy) <= rad * rad;
}

int main(void)
{
    static const int maat[] = { 16, 20, 24, 32, 40, 48, 64, 256 };
    enum { N = 8 };
    FILE *f = fopen("gpu-venster.ico", "wb");
    unsigned plek = 6 + 16 * N;
    int i, x, y, a, c;
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
        int s = maat[i], g = s * SS, x0 = g, x1 = 0, y0 = g, y1 = 0, dx, dy;
        BITMAPINFO bi = { { sizeof(BITMAPINFOHEADER), g, -g, 1, 32, BI_RGB } };
        uint32_t *px;
        HDC dc = CreateCompatibleDC(NULL);
        HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)&px, NULL, 0);
        HFONT font = CreateFontW(-(int)((s <= 24 ? 0.82 : 0.587) * g), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, NONANTIALIASED_QUALITY, 0, L"Arial");
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
        w32(f, 40); w32(f, s); w32(f, s * 2); w16(f, 1); w16(f, 32); w32(f, 0); w32(f, 0); w32(f, 0); w32(f, 0); w32(f, 0); w32(f, 0);
        for (y = s - 1; y >= 0; y--) for (x = 0; x < s; x++) {
            int vlak = 0, letter = 0;
            double r, gr, b, d;
            for (a = 0; a < SS; a++) for (c = 0; c < SS; c++) {
                int gx = x * SS + c + dx, gy = y * SS + a + dy;
                if (!in_vlak((x + (c + 0.5) / SS) / s, (y + (a + 0.5) / SS) / s)) continue;
                vlak++;
                if (gx >= 0 && gy >= 0 && gx < g && gy < g && (px[gy * g + gx] & 0xFFFFFF)) letter++;
            }
            d = vlak ? (double)letter / vlak : 0;
            r = 0x0A + (0xC9 - 0x0A) * d; gr = 0x0E + (0xA2 - 0x0E) * d; b = 0x18 + (0x27 - 0x18) * d;
            fputc((int)b, f); fputc((int)gr, f); fputc((int)r, f); fputc(vlak * 255 / (SS * SS), f);
        }
        for (y = 0; y < s; y++) for (x = 0; x < (s + 31) / 32 * 4; x++) fputc(0, f);
        DeleteObject(font); DeleteDC(dc); DeleteObject(bmp);
    }
    fclose(f);
    return 0;
}
