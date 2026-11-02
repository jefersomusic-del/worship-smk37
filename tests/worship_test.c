/* SPDX-License-Identifier: GPL-3.0-only */
/* WORSHIP's pad (src/worship_dsp.c) on the host: a G pad, a key change to C (the crossfade), a voice change, a stop;
 * every voice in every key. Checks: no clipping, no click (no jump between two samples beyond what the pad's own
 * waves make), the fade really fades (level at the end of a stop is silence), the crossfade has no hole (the level
 * through a key change stays within a few dB of the held level), the ISR cost per sample.
 *   cc -O2 -Ibuild/gen -Ifirmware/src -o build/worship_test tests/worship_test.c && build/worship_test [out.wav]
 * Writes the render to out.wav (16-bit stereo 44.1 kHz) when given. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "felucca_tables.h"

static inline int32_t clamp(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }
#define RING_PUBLISH() __asm__ volatile("" ::: "memory")
#define FM1_NKEY 37u
#include "worship_dsp.c"

static int16_t *pcm;
static uint32_t npcm, cap;
static int32_t ml[CTL], mr[CTL], sc[CTL], sv[CTL];
static int32_t max_abs, max_jump;
static int32_t last_l, last_r;

static void run(double sec, double *rms_out)
{
    uint32_t total = (uint32_t)(sec * FS), done = 0;
    double acc = 0;
    while (done < total) {
        uint32_t n = CTL, i;
        memset(ml, 0, sizeof ml);
        memset(mr, 0, sizeof mr);
        memset(sc, 0, sizeof sc);
        memset(sv, 0, sizeof sv);
        wp_mix(ml, mr, sc, sv, n);
        for (i = 0; i < n; i++) {
            int32_t l = ml[i], r = mr[i], jl = l - last_l, jr = r - last_r;
            if (jl < 0) jl = -jl;
            if (jr < 0) jr = -jr;
            if (jl > max_jump) max_jump = jl;
            if (jr > max_jump) max_jump = jr;
            last_l = l;
            last_r = r;
            if ((l < 0 ? -l : l) > max_abs) max_abs = l < 0 ? -l : l;
            if ((r < 0 ? -r : r) > max_abs) max_abs = r < 0 ? -r : r;
            acc += (double)l * l + (double)r * r;
            if (npcm + 2 > cap) {
                cap = cap ? cap * 2 : 1 << 20;
                pcm = realloc(pcm, cap * sizeof *pcm);
            }
            pcm[npcm++] = (int16_t)clamp(l, -32768, 32767);
            pcm[npcm++] = (int16_t)clamp(r, -32768, 32767);
        }
        done += n;
    }
    if (rms_out)
        *rms_out = acc / (2.0 * done);
}

static void wav(const char *path)
{
    FILE *f = fopen(path, "wb");
    uint32_t bytes = npcm * 2, v;
    if (!f) return;
    fwrite("RIFF", 1, 4, f); v = 36 + bytes; fwrite(&v, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f); v = 16; fwrite(&v, 4, 1, f);
    { uint16_t a[2] = {1, 2}; fwrite(a, 2, 2, f); }
    v = FS; fwrite(&v, 4, 1, f); v = FS * 4; fwrite(&v, 4, 1, f);
    { uint16_t a[2] = {4, 16}; fwrite(a, 2, 2, f); }
    fwrite("data", 1, 4, f); fwrite(&bytes, 4, 1, f);
    fwrite(pcm, 2, npcm, f);
    fclose(f);
}

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

int main(int argc, char **argv)
{
    double held, mid, tail, r;
    uint32_t v, k;
    clock_t t0;
    wp_level = 108;                                   /* unity */
    wp_xfade = 64;
    /* G, held */
    wp_key(7, 0);
    run(4.0, 0);
    run(1.0, &held);
    /* to C: the crossfade, measured across it */
    wp_key(0, 0);
    {
        double lo = 1e30, hi = 0;
        int i;
        for (i = 0; i < 20; i++) {
            run(0.15, &r);
            if (r < lo) lo = r;
            if (r > hi) hi = r;
        }
        mid = lo;
        printf("held rms %.0f, through the change: min %.0f max %.0f (%.1f dB .. %.1f dB)\n", held, lo, hi,
               10 * __builtin_log10(lo / held), 10 * __builtin_log10(hi / held));
        CHECK(lo > held * 0.35, "a hole in the crossfade (%.1f dB)", 10 * __builtin_log10(lo / held));
        CHECK(hi < held * 2.5, "a bump in the crossfade (%.1f dB)", 10 * __builtin_log10(hi / held));
    }
    run(2.0, 0);
    CHECK(wp_key_now == 0, "the key now is C");
    /* SHIMMER, same key */
    wp_key(0, 3);
    run(5.0, 0);
    /* the wheel all the way (swell) */
    wp_swell = 127;
    run(2.0, 0);
    wp_swell = 0;
    /* a fast run of changes: no more than WP_NSET sets, nothing stuck */
    wp_xfade = 10;
    for (k = 0; k < 12; k++) {
        wp_key(k, k % WP_NVOICES);
        run(0.12, 0);
    }
    run(1.0, 0);
    /* stop */
    wp_stop();
    run(1.5, 0);
    run(0.5, &tail);
    CHECK(!wp_sounding(), "the pad stopped");
    CHECK(tail < 1.0, "silence after the stop (rms %.2f)", tail);
    CHECK(wp_key_now == WP_NONE, "no key after the stop");
    printf("peak %d, largest step between samples %d\n", max_abs, max_jump);
    CHECK(max_abs < 32000, "clipping (peak %d)", max_abs);
    CHECK(max_jump < 6000, "a click (step %d)", max_jump);
    if (argc > 1)
        wav(argv[1]);
    /* every voice in every key: peaks */
    for (v = 0; v < WP_NVOICES; v++) {
        int32_t worst = 0;
        for (k = 0; k < 12; k++) {
            memset(&wp, 0, sizeof wp);
            max_abs = 0;
            wp_xfade = 0;
            wp_key(k, v);
            run(1.5, 0);
            if (max_abs > worst) worst = max_abs;
        }
        printf("voice %-8s peak %5d (unity level)\n", WP_VOICES[v].name, worst);
        CHECK(worst < 30000 && worst > 3000, "voice %s level %d", WP_VOICES[v].name, worst);
    }
    /* the cost: three sets sounding (a crossfade in progress) */
    memset(&wp, 0, sizeof wp);
    wp_xfade = 127;
    wp_key(0, 3); run(0.05, 0);
    wp_key(5, 3); run(0.05, 0);
    wp_key(9, 3); run(0.05, 0);
    npcm = 0;
    t0 = clock();
    run(10.0, 0);
    printf("host: %.1f ns per sample with three sets (5 notes x 2 oscillators each)\n",
           (double)(clock() - t0) / CLOCKS_PER_SEC * 1e9 / (10.0 * FS));
    (void)mid;
    printf(fails ? "worship_test: %d FAILED\n" : "worship_test: ok\n", fails);
    return fails != 0;
}
