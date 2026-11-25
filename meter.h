/* meter: meet de grafische kaarten, de processen en het systeem zelf (Windows-tellers, NVML, proceslijst). */
#ifndef METER_H
#define METER_H
#include <windows.h>

#define MAXG 4
#define MAXP 160
#define MAXTOP 10
#define NSOORT 8
#define HIST 600

typedef struct {
    char naam[64], soort[16], runtime[40], binnen[96];
    int pid, gpu;
    double vram, gedeeld, paged, pct, e3d, ecopy, evideo, ereken, ram, cpu;
} Proc;
typedef struct {
    int geldig, nvidia;
    char naam[96], driver[32], cuda[16], model[16], bus[24], cc[12];
    double vram_tot, vram_gebr, gedeeld, toegewezen, util, util_nvml, util_mem, temp, w, w_max, pstate, sm, sm_max,
           memklok, videoklok, rx, tx, gen, breedte, enc, dec;
    char snaam[NSOORT][16];
    double svram[NSOORT];
    int ns, np;
    Proc p[MAXP];
} Gpu;
typedef struct { char naam[64]; double rss, cpu; } Top;
typedef struct {
    double cpu, kern[128], ram_tot, ram_gebr, wissel_tot, wissel_gebr;
    int nkern, logisch, fysiek, ntcpu, ntram;
    Top tcpu[MAXTOP], tram[MAXTOP];
} Sys;
typedef struct {
    Gpu k[MAXG];
    int ng, hn;
    Sys s;
    float hu[MAXG][HIST], hv[MAXG][HIST];   /* verloop: belasting en videogeheugen in procent */
    DWORD tik;                              /* GetTickCount van de laatste meting */
    double per_s, meet_ms;                  /* gemeten: metingen per seconde, tijd per meting */
} Beeld;

/* Start de meter. Na elke meting krijgt 'venster' het bericht 'bericht' (venster mag NULL zijn). */
int meter_start(HWND venster, UINT bericht, int interval_ms);
void meter_interval(int ms);
void meter_kopie(Beeld *b);
/* Het venster roept dit aan na het tekenen: pas dan komt het volgende bericht. */
void meter_getekend(void);
/* Zoekt in een proces de geladen bibliotheken (runtime) en gekoppelde modelbestanden. Voor de proef. */
void meter_scan_pid(int pid, char *runtime, size_t nr, char *model, size_t nm);

#endif
