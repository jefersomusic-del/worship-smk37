/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Jeferson Mesquita (IPB Mata da Praia) and contributors */
/* WORSHIP (SMK-37 Pro): the ambient pad, its own sound generator beside the four parts.
 *
 * Why not a part: a pad held under a whole service would take three to five of the eight voices the parts share
 * (voice.c), and a key change would need twice as many while the old chord fades: the piano would lose notes. Here
 * the pad has its own oscillators, mixed after the parts (fx.c mix_block, wp_mix) into the dry mix, the chorus and
 * the reverb, so a key change is a true crossfade and the keyboard keeps all eight voices.
 *
 * A "set" is one chord: up to WP_NOTE notes (root, fifth, octaves: no third, so it sits under major and minor songs
 * alike), each two detuned oscillators, one left and one right. Up to WP_NSET sets sound at once: the one fading in
 * (or held) and the ones fading out. A new key fades the held set out and a new one in over the TRANSITION time; a
 * fourth request takes the quietest fading set over (its level is tiny by then).
 *
 * Fixed point throughout (the target's int multiplies): phases Q32, samples Q15, the envelope Q24, gains Q15.
 * Audio ISR: wp_mix. Main loop: wp_key, wp_stop, the wp_* settings (single bytes and words: the ISR reads them at a
 * block start, any value is safe). The host test (tests/worship_test.c) renders this file alone. */

#define WP_NSET 3
#define WP_NOTE 5
#define WP_NONE 0xFFu

typedef struct {
    const char *name;
    int8_t semi[WP_NOTE];         /* from the root; -128 = unused */
    uint8_t sine;                 /* bit k: note k is a sine (sub, shimmer), else a band-limited saw */
    uint16_t gain[WP_NOTE];       /* Q15 */
    uint16_t cut;                 /* low-pass, Hz, at BRILHO 64 */
    uint8_t det;                  /* the pair's detune, 1/1024 of the pitch (~1.7 cents a step) */
    uint8_t sparkle;              /* 1: the sines shimmer (a slow tremolo each) */
} wp_voice_t;

#define WP_U (-128)
static const wp_voice_t WP_VOICES[] = {
    {"QUENTE", {0, 7, 12, 19, WP_U}, 0x00u, {24000, 17000, 21000, 11000, 0}, 1300, 4, 0},
    {"ETEREO", {12, 19, 24, 31, 0}, 0x00u, {20000, 15000, 14000, 8000, 9000}, 3200, 6, 0},
    {"ESCURO", {-12, 0, 7, 12, WP_U}, 0x01u, {16000, 22000, 15000, 15000, 0}, 650, 3, 0},
    {"SHIMMER", {0, 7, 12, 24, 31}, 0x18u, {22000, 15000, 18000, 7000, 5000}, 1500, 5, 1},
    {"CORDAS", {0, 7, 12, 16, 19}, 0x00u, {20000, 14000, 17000, 0, 10000}, 2200, 8, 0},
};
#undef WP_U
#define WP_NVOICES (sizeof WP_VOICES / sizeof WP_VOICES[0])

typedef struct {
    uint32_t ph[WP_NOTE][2], inc[WP_NOTE][2];
    int32_t gain[WP_NOTE];
    int32_t lp[2][2];             /* the two-pole low-pass, per channel */
    int32_t env;                  /* Q24, 0 .. 1 << 24 */
    int32_t g_prev;               /* the last block's gain (the ramp across a block) */
    uint32_t trem[WP_NOTE];       /* SHIMMER: each sine's tremolo phase */
    uint8_t n, on, key, voice, sine, active, sparkle;
    uint16_t cut;
} wp_set_t;

static struct {
    wp_set_t s[WP_NSET];
    uint32_t lfo;                 /* the slow movement (cutoff), ~0.07 Hz */
    uint32_t seed;
    int32_t peak;                 /* for the screen's meter: the last block's, Q15 */
} wp;

/* settings (main loop writes, the ISR reads) */
static volatile uint8_t wp_req = WP_NONE;     /* a key 0..11 asked for, 12 = stop; WP_NONE: nothing new */
static volatile uint8_t wp_req_voice;         /* .. its voice (WP_VOICES) */
static volatile uint8_t wp_key_now = WP_NONE; /* the key held now (WP_NONE: none), for the screen */
static volatile uint8_t wp_bright = 64;       /* BRILHO 0..127: the low-pass around the voice's own */
static volatile uint8_t wp_xfade = 64;        /* TRANSICAO 0..127: the fade time, ~0.3 .. 8 s */
static volatile uint8_t wp_level = 104;       /* fader 2: 0..127 (until it moves: ~-3 dB) */
static volatile uint8_t wp_amb = 80;          /* fader 4 (AMBIENCIA): its reverb send 0..127 */
static volatile uint8_t wp_swell;             /* the mod wheel: 0..127, opens the filter and lifts the level */

#define WP_MIDI_ROOT 48u                       /* C3: keys C..F# sit from here, G..B an octave lower (G2..B2) */
static uint32_t wp_root(uint32_t key) { return WP_MIDI_ROOT + key - (key >= 7u ? 12u : 0u); }

/* fade time in samples: 0.3 s at 0, 8 s at 127, a curve (more room at the short end) */
static uint32_t wp_fade_samples(void)
{
    uint32_t x = wp_xfade;
    return (uint32_t)FS * 3u / 10u + (uint32_t)FS * (x * x) / (127u * 127u) * 77u / 10u;
}

static uint32_t wp_rand(void)
{
    wp.seed = wp.seed * 1664525u + 1013904223u;
    return wp.seed;
}

/* low-pass coefficient (Q15) of a cutoff in Hz: w / (1 + w), w = 2 pi f / FS */
static int32_t wp_coef(int32_t hz)
{
    int32_t w;
    hz = clamp(hz, 60, 12000);
    w = hz * 4669 / 1000;                      /* 2 pi / 44100 * 32768 = 4.6686 */
    return w * 32768 / (32768 + w);
}

static void wp_start(wp_set_t *s, uint32_t key, uint32_t vi)
{
    const wp_voice_t *v = &WP_VOICES[vi % WP_NVOICES];
    uint32_t k, n = 0, root = wp_root(key);
    for (k = 0; k < WP_NOTE; k++) {
        int32_t note;
        uint32_t inc;
        if (v->semi[k] == -128 || !v->gain[k])
            continue;
        note = (int32_t)root + v->semi[k];
        if (note < 24 || note > 108)
            continue;
        inc = pitch_inc((uint32_t)note * 16u);
        s->inc[n][0] = inc - (inc >> 10) * v->det;   /* the pair: a little flat left, sharp right */
        s->inc[n][1] = inc + (inc >> 10) * v->det;
        s->ph[n][0] = wp_rand();                     /* free phases: no two sets ever start in step */
        s->ph[n][1] = wp_rand();
        s->trem[n] = wp_rand();
        s->gain[n] = v->gain[k];
        s->sine = (uint8_t)((s->sine & ~(1u << n)) | (((v->sine >> k) & 1u) << n));
        n++;
    }
    s->n = (uint8_t)n;
    s->lp[0][0] = s->lp[0][1] = s->lp[1][0] = s->lp[1][1] = 0;
    s->env = 0;
    s->g_prev = 0;
    s->on = 1;
    s->active = 1;
    s->key = (uint8_t)key;
    s->voice = (uint8_t)(vi % WP_NVOICES);
    s->cut = v->cut;
    s->sparkle = v->sparkle;
}

/* a new chord (key 0..11, 12 = stop): the held set fades out, a new one in. ISR, at a block start */
static void wp_apply(uint32_t req, uint32_t vi)
{
    uint32_t i, best = 0;
    int32_t low = 0x7FFFFFFF;
    wp_set_t *held = 0;
    for (i = 0; i < WP_NSET; i++)
        if (wp.s[i].active && wp.s[i].on)
            held = &wp.s[i];
    if (req >= 12u) {                                 /* stop: everything fades */
        for (i = 0; i < WP_NSET; i++)
            wp.s[i].on = 0;
        wp_key_now = WP_NONE;
        return;
    }
    if (held && held->key == req && held->voice == vi % WP_NVOICES)
        return;                                       /* (already there) */
    if (held)
        held->on = 0;
    for (i = 0; i < WP_NSET; i++) {                   /* a free set, or the quietest fading one */
        if (!wp.s[i].active) {
            best = i;
            low = -1;
            break;
        }
        if (wp.s[i].env < low) {
            low = wp.s[i].env;
            best = i;
        }
    }
    wp_start(&wp.s[best], req, vi);
    wp_key_now = (uint8_t)req;
}

/* the fade's shape: 1 - (1 - e)^2 (a gentle start, no dip at the crossing). e, the result: Q15 */
static inline int32_t wp_shape(int32_t e)
{
    int32_t r = 32768 - e;
    return 32768 - ((r * r) >> 15);
}

/* polyBLEP-corrected saw, Q15 */
static inline int32_t wp_saw(uint32_t ph, uint32_t inc)
{
    int32_t s = (int32_t)(ph >> 16) - 32768, x;
    if (ph < inc) {                                   /* just after the wrap */
        x = (int32_t)(((ph >> 12) << 15) / ((inc >> 12) | 1u));
        s -= 2 * x - ((x * x) >> 15) - 32768;
    } else if (ph > 0xFFFFFFFFu - inc) {              /* just before it */
        x = -(int32_t)((((0xFFFFFFFFu - ph) >> 12) << 15) / ((inc >> 12) | 1u));
        s -= ((x * x) >> 15) + 2 * x + 32768;
    }
    return s;
}

static inline int32_t wp_sine(uint32_t ph) { return SINE[ph >> 22]; }

/* one set into l / r (added), n samples; returns 0 once it has faded out */
static int wp_render(wp_set_t *s, int32_t *l, int32_t *r, uint32_t n, uint32_t step, int32_t cut)
{
    uint32_t i, k;
    int32_t g0 = s->g_prev, g1, dg, g;
    int32_t e = s->env + (int32_t)(s->on ? step * n : 0u) - (int32_t)(s->on ? 0u : step * n);
    if (e >= 1 << 24)
        e = 1 << 24;
    if (e <= 0) {
        e = 0;
        if (!s->on && !g0) {
            s->active = 0;
            return 0;
        }
    }
    s->env = e;
    g1 = wp_shape(e >> 9);
    dg = (g1 - g0) / (int32_t)n;
    g = g0;
    for (i = 0; i < n; i++) {
        int32_t sl = 0, sr = 0, y;
        for (k = 0; k < s->n; k++) {
            uint32_t a = s->ph[k][0], b = s->ph[k][1];
            int32_t vl, vr, gk = s->gain[k];
            if ((s->sine >> k) & 1u) {
                vl = wp_sine(a);
                vr = wp_sine(b);
                if (s->sparkle) {                      /* a slow shimmer: each sine's own tremolo */
                    int32_t t = 24576 + (wp_sine(s->trem[k]) >> 2);
                    gk = (gk * t) >> 15;
                    s->trem[k] += s->inc[k][0] >> 10;   /* (~0.3 .. 1 Hz) */
                }
            } else {
                vl = wp_saw(a, s->inc[k][0]);
                vr = wp_saw(b, s->inc[k][1]);
            }
            s->ph[k][0] = a + s->inc[k][0];
            s->ph[k][1] = b + s->inc[k][1];
            sl += (vl * gk) >> 17;                    /* (>> 2 of headroom: five notes) */
            sr += (vr * gk) >> 17;
        }
        s->lp[0][0] += ((sl - s->lp[0][0]) * cut) >> 15;
        s->lp[0][1] += ((s->lp[0][0] - s->lp[0][1]) * cut) >> 15;
        s->lp[1][0] += ((sr - s->lp[1][0]) * cut) >> 15;
        s->lp[1][1] += ((s->lp[1][0] - s->lp[1][1]) * cut) >> 15;
        y = g >> 1;                                   /* (Q15 gain -> Q14: the pad's headroom in the mix) */
        l[i] += (s->lp[0][1] * y) >> 14;
        r[i] += (s->lp[1][1] * y) >> 14;
        g += dg;
    }
    s->g_prev = g1;
    return 1;
}

static int32_t wp_l[CTL], wp_r[CTL];

/* main loop: a key (0..11) with a voice, or stop */
static void wp_key(uint32_t key, uint32_t voice)
{
    wp_req_voice = (uint8_t)(voice % WP_NVOICES);
    RING_PUBLISH();
    wp_req = (uint8_t)(key % 12u);
}
static void wp_stop(void) { wp_req = 12u; }
static int wp_sounding(void)
{
    uint32_t i;
    for (i = 0; i < WP_NSET; i++)
        if (wp.s[i].active)
            return 1;
    return 0;
}

/* audio ISR: the pad into the dry mix and the sends (fx.c mix_block, after the parts). n <= CTL */
static void wp_mix(int32_t *ml, int32_t *mr, int32_t *sc, int32_t *sv, uint32_t n)
{
    uint32_t i, step, any = 0;
    int32_t lvl, amb, cho, pk = 0, lfo, base;
    uint32_t req = wp_req;
    if (req != WP_NONE) {
        wp_req = WP_NONE;
        wp_apply(req, wp_req_voice);
    }
    step = (1u << 24) / wp_fade_samples();
    if (!step)
        step = 1;
    wp.lfo += 6800u * n;                              /* ~0.07 Hz */
    lfo = SINE[wp.lfo >> 22];                         /* Q15 */
    for (i = 0; i < n; i++)
        wp_l[i] = wp_r[i] = 0;
    for (i = 0; i < WP_NSET; i++) {
        wp_set_t *s = &wp.s[i];
        int32_t hz;
        if (!s->active)
            continue;
        /* the cutoff: the voice's, x 0.35 .. x 2.8 by BRILHO, opened up to x 3 more by the wheel, +-18 % LFO */
        hz = (int32_t)s->cut * (36 + wp_bright * 2) / 164;
        hz += hz * (int32_t)wp_swell / 64;
        hz += (hz * (lfo >> 4)) / 11000;
        base = wp_coef(hz);
        any |= (uint32_t)wp_render(s, wp_l, wp_r, n, step, base);
    }
    if (!any) {
        wp.peak = 0;
        return;
    }
    lvl = LEVEL_Q12[wp_level & 127] + (int32_t)LEVEL_Q12[wp_level & 127] * wp_swell / 400;   /* (the wheel: +~3 dB) */
    amb = wp_amb * 258;                               /* Q15 sends */
    cho = 9000;
    for (i = 0; i < n; i++) {
        int32_t l = (wp_l[i] * lvl) >> 12, r = (wp_r[i] * lvl) >> 12, m = (l + r) >> 1, a = m < 0 ? -m : m;
        ml[i] += l;
        mr[i] += r;
        sv[i] += (m >> 1) * (amb >> 8) >> 6;          /* (m * amb >> 15, no overflow) */
        sc[i] += (m >> 1) * (cho >> 8) >> 6;
        if (a > pk)
            pk = a;
    }
    wp.peak = pk;
}

/* the keys' routing (worship.c, audio ISR: seq.c keyboard_block, ext_keys_block): keybed key kb (0 = C3 .. 36 = C6)
 * down / up while WORSHIP is on; wor_kmask[kb]: the tracks it plays (bit 0 the keys, bit 1 the lead), so its
 * release ends what it started even after the mode or WORSHIP changed */
static volatile uint8_t wor_on;
static uint8_t wor_kmask[FM1_NKEY], wor_knote[FM1_NKEY];
static void wor_press(uint32_t kb, uint32_t vel);
static void wor_release(uint32_t kb);
/* (worship.c, after the UI and the storage; called from ui_input.c, ui_draw.c, main.c) */
static int wor_ui_input(uint32_t pressed, uint32_t notes);
static void wor_draw(void);
static void wor_leds(void);
static void wor_faders(void);
static void wor_boot(void);
