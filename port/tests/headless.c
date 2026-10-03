/* Host smoke test:  headless <rom.gba> <frames> [out.ppm] */
#include "../host/host.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
int main(int argc, char **argv) {
    if (argc < 3) return 2;
    FILE *f = fopen(argv[1], "rb"); if (!f) { perror("rom"); return 2; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *rom = malloc(n); fread(rom, 1, n, f); fclose(f);
    int want = atoi(argv[2]);
    int rc = wl4_start(rom, n); if (rc) { printf("start failed %d\n", rc); return 1; }
    static uint32_t fb[240 * 160]; int fr = 0;
    for (int i = 0; i < 2000 && fr < want; i++) { usleep(5000); fr = wl4_copy_frame(fb); if (!wl4_running()) break; }
    printf("frames=%d running=%d\n", fr, wl4_running()); fflush(stdout);
    if (argc > 3) { FILE *o = fopen(argv[3], "wb"); fprintf(o, "P6\n240 160\n255\n"); for (int i = 0; i < 240 * 160; i++) { fputc(fb[i] >> 16, o); fputc(fb[i] >> 8, o); fputc(fb[i], o); } fclose(o); }
    wl4_stop(); return 0;
}
