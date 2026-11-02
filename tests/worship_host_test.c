/* SPDX-License-Identifier: GPL-3.0-only */
/* WORSHIP end to end: the SMK-37 build (web/emu/smk_web.c: the firmware's sources against a simulated panel) driven
 * as a musician would: power on (WORSHIP on), a pad tap (the ambient pad in G), a key change (C), the keys in each
 * MODO (which tracks sound), the knobs (sounds, BPM), a song saved into the setlist (a pad held), a power cycle (the
 * setlist read back from the simulated flash), a song loaded with < >, GLOBE to the synthesizer and back.
 * Writes the screens to OUT/*.ppm and the audio to OUT/worship.wav.
 *   cc -O1 -w -ffp-contract=off -std=gnu99 -Ibuild/gen -Ifirmware/src -Ifirmware/hal -o build/worship_host \
 *      tests/worship_host_test.c -lm && build/worship_host build/worship_out */
#include "../web/emu/smk_web.c"
#include <sys/stat.h>

static const char *outdir = "build/worship_out";
static FILE *wavf;
static uint32_t wav_frames;
static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } else if (verbose) { printf("ok: "); printf(__VA_ARGS__); printf("\n"); } } while (0)
static int verbose = 1;
static double rms_acc;
static uint32_t rms_n;

static void run_ms(uint32_t ms)                    /* the device runs ms milliseconds; audio to the WAV */
{
    uint32_t n = ms * FS / 1000u;
    while (n) {
        uint32_t k = n > 1024u ? 1024u : n, i;
        web_render(k);
        for (i = 0; i < k; i++) {
            int16_t s[2];
            float l = out_l[i] * 32767.0f, r = out_r[i] * 32767.0f;
            s[0] = (int16_t)(l > 32767 ? 32767 : l < -32768 ? -32768 : l);
            s[1] = (int16_t)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
            if (wavf)
                fwrite(s, 2, 2, wavf);
            wav_frames++;
            rms_acc += (double)l * l;
            rms_n++;
        }
        n -= k;
    }
}
static double rms_since(void)
{
    double r = rms_n ? sqrt(rms_acc / rms_n) : 0;
    rms_acc = 0;
    rms_n = 0;
    return r;
}

static void shot(const char *name)
{
    char path[256];
    FILE *f;
    uint32_t i;
    snprintf(path, sizeof path, "%s/%s.ppm", outdir, name);
    f = fopen(path, "wb");
    if (!f)
        return;
    fprintf(f, "P6\n240 240\n255\n");
    for (i = 0; i < 240u * 240u; i++) {
        uint16_t c = (uint16_t)((web_fb[i] >> 8) | (web_fb[i] << 8));
        uint8_t rgb[3] = {(uint8_t)((c >> 11) << 3), (uint8_t)(((c >> 5) & 63u) << 2), (uint8_t)((c & 31u) << 3)};
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    /* the pads' colours alongside, a 4 x 2 strip per row (text) */
    snprintf(path, sizeof path, "%s/%s.pads", outdir, name);
    f = fopen(path, "w");
    if (f) {
        for (i = 0; i < 16u; i++)
            fprintf(f, "%06x%c", padled_rgb[i], i % 8u == 7u ? '\n' : ' ');
        fprintf(f, "buttons %05x\n", web_lit_buttons());
        fclose(f);
    }
}

static void tap_button(uint32_t id)
{
    web_buttons(1u << id);
    run_ms(40);
    web_buttons(0);
    run_ms(40);
}
static void tap_pad(uint32_t p)
{
    web_pad(p, 100);
    run_ms(60);
    web_pad(p, 0);
    run_ms(40);
}
static uint32_t voices(uint32_t t)
{
    uint32_t i, n = 0;
    for (i = 0; i < NVOICE; i++)
        n += trk[t].v[i].active && trk[t].v[i].note;
    return n;
}
static int note_on(uint32_t t, uint32_t note)
{
    uint32_t i;
    for (i = 0; i < NVOICE; i++)
        if (trk[t].v[i].active && trk[t].v[i].note == note && trk[t].v[i].stage < 3)
            return 1;
    return 0;
}

int main(int argc, char **argv)
{
    char path[256];
    uint32_t w0, i;
    double r;
    if (argc > 1)
        outdir = argv[1];
    mkdir(outdir, 0755);
    snprintf(path, sizeof path, "%s/worship.wav", outdir);
    wavf = fopen(path, "wb");
    if (wavf)
        wav_hdr(wavf, 0);
    web_nor_erase();
    web_wheels(540, 629);                         /* the wheels at rest (mod: up = lower raw; 629 = none) */
    web_boot();
    run_ms(1500);
    CHECK(wor_on, "WORSHIP on at power-on");
    CHECK(smk_pad_raw, "the pads are WORSHIP's (RAW)");
    CHECK(trk[0].eng_req == 4 && trk[1].eng_req == 0, "TECLAS = SAMPLE (PIANO), LEAD = ANALOG (SAW LEAD): %u %u",
          trk[0].eng_req, trk[1].eng_req);
    shot("01_boot");

    /* the pad: G (top row, fifth pad) */
    rms_since();
    run_ms(500);
    r = rms_since();
    CHECK(r < 5.0, "silence before the pad (rms %.1f)", r);
    tap_pad(4);
    run_ms(3000);
    CHECK(wp_key_now == 7, "the pad in G (%u)", wp_key_now);
    r = rms_since();
    CHECK(r > 300.0, "the pad sounds (rms %.0f)", r);
    shot("02_pad_G");
    /* to C: crossfade */
    tap_pad(0);
    run_ms(200);
    CHECK(wp_key_now == 0, "the pad in C");
    { uint32_t n = 0; for (i = 0; i < WP_NSET; i++) n += wp.s[i].active; CHECK(n == 2, "two sets crossfading (%u)", n); }
    run_ms(4000);
    shot("03_pad_C");

    /* the keys: TECLAS (track 1 only) */
    web_key(17, 100);                             /* F4: keybed 17 -> F3 + 12 */
    run_ms(200);
    CHECK(voices(0) == 1 && voices(1) == 0, "MODO TECLAS: the key plays track 1 only (%u, %u)", voices(0), voices(1));
    CHECK(note_on(0, 65), "the note is F4 (65)");
    web_key(17, 0);
    run_ms(600);
    /* ARP: SOLO */
    tap_button(6);
    CHECK(wor_st.live.mode == WM_SOLO, "ARP -> SOLO");
    CHECK(song.sel == 1, "the wheels bend the lead");
    web_key(17, 100);
    run_ms(200);
    CHECK(note_on(1, 65) && !note_on(0, 65), "SOLO: the lead alone");
    web_key(17, 0);
    run_ms(300);
    /* SOMA */
    tap_button(6);
    CHECK(wor_st.live.mode == WM_LAYER, "ARP -> SOMA");
    web_key(17, 100);
    web_key(21, 90);
    run_ms(200);
    CHECK(note_on(0, 65) && note_on(0, 69), "SOMA: the chord on the keys");
    CHECK(note_on(1, 69) && !note_on(1, 65), "SOMA: the lead on the top note");
    shot("04_soma");
    web_key(17, 0);
    web_key(21, 0);
    run_ms(300);
    /* DIVIDIR: below C5 the keys, from C5 the lead (an outer key too: keybed 33 = A5) */
    tap_button(6);
    CHECK(wor_st.live.mode == WM_SPLIT, "ARP -> DIVIDIR");
    web_key(12, 100);                             /* C4 */
    web_key(28, 100);                             /* E5 */
    web_key(2, 100);                              /* D3 (outer, low) */
    run_ms(200);
    CHECK(note_on(0, 60) && !note_on(1, 60), "DIVIDIR: C4 on the keys");
    CHECK(note_on(1, 76) && !note_on(0, 76), "DIVIDIR: E5 on the lead");
    CHECK(note_on(0, 50), "DIVIDIR: an outer key below on the keys");
    web_key(33, 100);                             /* A5 (outer: keybed 33) */
    run_ms(200);
    CHECK(note_on(1, 81) && !note_on(0, 81), "DIVIDIR: an outer key above on the lead");
    web_key(12, 0);
    web_key(28, 0);
    web_key(33, 0);
    web_key(2, 0);
    run_ms(1500);
    for (i = 0; i < NVOICE; i++)
        if ((trk[0].v[i].active || trk[1].v[i].active) && verbose > 1)
            printf("  v%u: t1 %u/%u/%u t2 %u/%u/%u\n", i, trk[0].v[i].active, trk[0].v[i].note, trk[0].v[i].stage,
                   trk[1].v[i].active, trk[1].v[i].note, trk[1].v[i].stage);
    CHECK(!note_on(0, 60) && !note_on(1, 76) && !note_on(1, 81) && !note_on(0, 50), "every key-up ended its note");
    /* a key held across a MODO change still ends */
    web_key(14, 100);                             /* D4: DIVIDIR -> keys */
    run_ms(100);
    tap_button(6);                                /* -> TECLAS */
    web_key(14, 0);
    run_ms(1500);
    CHECK(!note_on(0, 62) && !note_on(1, 62), "a key held across a mode change ends");
    CHECK(wor_st.live.mode == WM_KEYS, "back to TECLAS");

    /* knobs: K2 the keys' sound (E.PIANO), K4 the pad's (SHIMMER, crossfading), K8 BPM */
    web_knob(1, 1);
    run_ms(400);
    CHECK(wor_st.live.keys_snd == 1 && trk[0].eng_req == 12, "K2: E.PIANO (FM6) on track 1 (%u)", trk[0].eng_req);
    web_knob(3, 3);
    run_ms(100);
    CHECK(wor_st.live.pad_snd == 3, "K4: SHIMMER");
    web_knob(7, 6);
    run_ms(100);
    CHECK(wor_st.live.bpm == 78 && song.g[G_BPM] == 78, "K8: 78 BPM");
    run_ms(3000);
    shot("05_shimmer");

    /* the setlist: SEQ, hold pad 3 a second: song 3 saved */
    tap_button(15);
    CHECK(wor_st.view == 1, "SEQ: SETLIST");
    w0 = web_flash_writes;
    web_pad(2, 100);
    run_ms(1200);
    shot("06_saving");
    web_pad(2, 0);
    run_ms(100);
    CHECK(web_flash_writes > w0, "the song written to flash");
    CHECK(wor_st.song[2].used && wor_st.song[2].key == 0 && wor_st.song[2].pad_snd == 3 && wor_st.song[2].bpm == 78,
          "song 3: C, SHIMMER, 78 BPM");
    CHECK(wor_st.cur == 2, "song 3 is the current one");
    /* a second song: G, QUENTE, ORGAO, SOLO-free, 64 BPM, into song 5 */
    tap_button(15);                               /* TOM */
    tap_pad(4);                                   /* G */
    web_knob(3, 2);                               /* SHIMMER -> CORDAS(4) -> QUENTE(0) */
    web_knob(1, 1);                               /* E.PIANO -> ORGAO */
    web_knob(7, -14);                             /* 64 BPM */
    run_ms(500);
    tap_button(15);                               /* SETLIST */
    web_pad(4, 100);
    run_ms(1200);
    web_pad(4, 0);
    run_ms(200);
    CHECK(wor_st.song[4].used && wor_st.song[4].key == 7 && wor_st.song[4].keys_snd == 2 && wor_st.song[4].bpm == 64,
          "song 5: G, ORGAO, 64 BPM");
    shot("07_setlist");

    /* the stop pad, then power off and on: the setlist comes back from the flash */
    tap_button(15);                               /* TOM */
    tap_pad(7);
    run_ms(9000);
    CHECK(!wp_sounding(), "the stop pad faded the pad out");
    {
        static uint8_t keep[0x100000];
        memcpy(keep, nor, sizeof nor);
        memset(&wor_st, 0, sizeof wor_st);
        wor_load();
        CHECK(wor_st.song[2].used && wor_st.song[4].used && wor_st.song[4].key == 7, "the setlist read back after a power cycle");
        CHECK(!memcmp(keep, nor, sizeof nor), "reading wrote nothing");
    }
    /* < >: from song 5, > wraps to song 3 (the next used), < back to 5 */
    tap_button(3);
    CHECK(wor_st.cur == 2, "> : song 3 (%u)", wor_st.cur);
    CHECK(wor_st.live.bpm == 78 && wor_st.live.keys_snd == 1, "song 3's settings loaded");
    run_ms(400);
    CHECK(trk[0].eng_req == 12, "song 3's E.PIANO on track 1");
    /* the pad running: loading a song moves it to the song's key */
    tap_button(9);                                /* CHORD: the pad on, in the song's key (C) */
    run_ms(500);
    CHECK(wp_key_now == 0, "CHORD: the pad on in C");
    tap_button(2);                                /* < : song 5, G */
    run_ms(300);
    CHECK(wor_st.cur == 4 && wp_key_now == 7, "< : song 5, the pad follows to G");
    run_ms(4000);
    shot("08_song5");

    /* faders: the pad's level, the ambience */
    web_fader(1, 40);
    run_ms(100);
    web_fader(1, 30);
    run_ms(100);
    CHECK(wp_level == 30, "fader 2: the pad's level");
    web_fader(3, 100);
    run_ms(100);
    web_fader(3, 110);
    run_ms(100);
    CHECK(wp_amb == 110 && trk[1].p[P_REV] == 110 * 7 / 8, "fader 4: the ambience");

    /* GLOBE: the synthesizer (the pads play notes again), and back */
    tap_button(10);
    CHECK(!wor_on && !smk_pad_raw, "GLOBE: the synthesizer");
    run_ms(300);
    shot("09_synth");
    web_pad(8, 100);                              /* bottom-left pad: F3 on the selected track as a note */
    run_ms(100);
    CHECK(voices(song.sel) >= 1, "the pads play notes in the synthesizer");
    web_pad(8, 0);
    run_ms(500);
    tap_button(10);
    CHECK(wor_on && smk_pad_raw, "GLOBE: back to WORSHIP");
    run_ms(500);
    shot("10_back");
    tap_button(9);                                /* CHORD: stop */
    run_ms(9000);
    CHECK(!wp_sounding(), "CHORD stopped the pad");
    r = rms_since();

    if (wavf) {
        fseek(wavf, 0, SEEK_SET);
        wav_hdr(wavf, wav_frames);
        fclose(wavf);
    }
    printf(fails ? "worship_host_test: %d FAILED\n" : "worship_host_test: ok\n", fails);
    return fails != 0;
}
