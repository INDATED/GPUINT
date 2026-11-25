/* meter_proef: drukt een meting van de eigen meter af, om te vergelijken met "gputaskman --json".
 * Gebruik: meter_proef [interval_ms] [seconden]   of   meter_proef scan PID */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "meter.h"

static int op_vram(const void *a, const void *b)
{
    double d = ((const Proc *)b)->vram - ((const Proc *)a)->vram;
    if (d == 0) d = ((const Proc *)b)->gedeeld - ((const Proc *)a)->gedeeld;
    return d > 0 ? 1 : d < 0 ? -1 : 0;
}

int main(int argc, char **argv)
{
    static Beeld b;
    int i, j, ms = argc > 1 ? atoi(argv[1]) : 1000, sec = argc > 2 ? atoi(argv[2]) : 3;
    if (argc > 2 && !strcmp(argv[1], "scan")) {
        char rt[64], model[96];
        meter_scan_pid(atoi(argv[2]), rt, sizeof rt, model, sizeof model);
        printf("runtime: [%s]\nmodel: [%s]\n", rt, model);
        return 0;
    }
    if (!meter_start(NULL, 0, ms)) { printf("geen kaart gevonden\n"); return 1; }
    Sleep((DWORD)sec * 1000);
    meter_kopie(&b);
    printf("metingen per s: %.1f, tijd per meting: %.2f ms, kaarten: %d\n", b.per_s, b.meet_ms, b.ng);
    for (i = 0; i < b.ng; i++) {
        Gpu *q = &b.k[i];
        printf("GPU %d: %s | driver %s cuda %s bus %s cc %s\n", i, q->naam, q->driver, q->cuda, q->bus, q->cc);
        printf("  belasting %.0f (nvml %.0f) temp %.0f W %.1f/%.0f vram %.0f/%.0f MB gedeeld %.0f MB toegewezen %.0f MB\n", q->util,
               q->util_nvml, q->temp, q->w, q->w_max, q->vram_gebr / 1048576, q->vram_tot / 1048576, q->gedeeld / 1048576, q->toegewezen / 1048576);
        printf("  P%.0f sm %.0f/%.0f mem %.0f video %.0f pcie gen%.0f x%.0f rx %.0f tx %.0f enc %.0f dec %.0f\n", q->pstate, q->sm, q->sm_max,
               q->memklok, q->videoklok, q->gen, q->breedte, q->rx, q->tx, q->enc, q->dec);
        qsort(q->p, (size_t)q->np, sizeof q->p[0], op_vram);
        for (j = 0; j < q->np && j < 12; j++) {
            Proc *p = &q->p[j];
            printf("  %-28s %6d %-10s [%s] vram %.0f gedeeld %.0f paged %.0f MB gpu %.1f (3d %.1f copy %.1f) ram %.0f MB cpu %.1f | %s\n", p->naam, p->pid,
                   p->soort, p->runtime, p->vram / 1048576, p->gedeeld / 1048576, p->paged / 1048576, p->pct, p->e3d, p->ecopy, p->ram / 1048576, p->cpu, p->binnen);
        }
        printf("  (%d processen)\n", q->np);
    }
    printf("cpu %.1f (%d kernen, %d draden) ram %.1f/%.1f GB wissel %.1f/%.1f GB\n", b.s.cpu, b.s.fysiek, b.s.logisch, b.s.ram_gebr / 1073741824,
           b.s.ram_tot / 1073741824, b.s.wissel_gebr / 1073741824, b.s.wissel_tot / 1073741824);
    for (i = 0; i < 3 && i < b.s.ntcpu; i++) printf("  cpu: %s %.1f | ram: %s %.2f GB\n", b.s.tcpu[i].naam, b.s.tcpu[i].cpu, b.s.tram[i].naam, b.s.tram[i].rss / 1073741824);
    return 0;
}
