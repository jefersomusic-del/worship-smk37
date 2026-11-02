/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Jeferson Mesquita (IPB Mata da Praia) and contributors */
/* WORSHIP (SMK-37 Pro): a performance mode for church music, on top of Felucca. The keyboard powers on in it;
 * GLOBE switches to the full synthesizer (Felucca as ever) and back.
 *
 *   the keys     TECLAS (track 1: piano, e-piano, organ, strings ..) and LEAD (track 2: solos), by MODO:
 *                TECLAS, SOLO (the lead alone), SOMA (both, layered), DIVIDIR (the lead from C5 up)
 *   the pads     TOM view: the twelve keys as a piano (top row C D E F G A B, bottom row the sharps); a tap
 *                starts the ambient pad in that key, crossfading from the one before (worship_dsp.c); the
 *                top-right pad (or CHORD) stops it, the key playing tapped again too.
 *                SETLIST view (SEQ): sixteen songs; a tap loads one (key, BPM, sounds, mode, pad); held one
 *                second it saves what plays now into that song
 *   the knobs    K1 MASTER, K2 TECLAS sound, K3 LEAD sound, K4 PAD sound, K5 BRILHO (the pad's brightness),
 *                K6 TRANSICAO (the pad's fade), K7 DELAY (the lead's dotted eighth echo), K8 BPM
 *   the faders   1 TECLAS, 2 PAD, 3 LEAD, 4 AMBIENCIA (the reverb of all three)
 *   the wheels   pitch bends the lead (the keys in TECLAS mode); MOD swells the pad (brighter, a little louder)
 *   buttons      ARP MODO, SEQ TOM / SETLIST, CHORD PAD on / off, NOTE REPEAT TAP tempo, < > previous / next
 *                song, OCT- / OCT+ the octave, BT SALVAR (into the current song), PATCH silences the keys,
 *                GLOBE the synthesizer
 *
 * The setlist and the live settings live in their own A/B pair of flash sectors (0xB1000 / 0xB2000, in the SMK
 * store's spare room, FL_DATA), written only by SALVAR / a held pad: an erase stops the audio for a few
 * milliseconds, so nothing here writes on its own. */

/* ------------------------------------------------------------ sounds --- */
typedef struct { const char *name; uint8_t engine, preset; } wor_snd_t;
static const wor_snd_t WOR_KEYS[] = {
    {"PIANO", 4, 0},       {"E.PIANO", 12, 0},   {"ORGAO", 7, 0},     {"CORDAS", 0, 11},  {"CORAL", 5, 0},
    {"SYNTH PAD", 0, 1},   {"HARPA", 9, 8},      {"SINO", 12, 1},     {"MARIMBA", 9, 1},  {"KALIMBA", 9, 4},
    {"ORGAO FM", 12, 6},   {"SINE KEYS", 0, 5},
};
static const wor_snd_t WOR_LEADS[] = {
    {"SAW LEAD", 0, 0},    {"FLAUTA", 4, 2},     {"SOPRO", 0, 10},    {"SYNC LEAD", 6, 2}, {"WAVE LEAD", 3, 3},
    {"SAX", 4, 3},         {"VOZ", 5, 1},        {"BRASS", 0, 9},     {"ARP LEAD", 6, 1}, {"PLUCK", 0, 8},
};
#define WOR_NKEYS (sizeof WOR_KEYS / sizeof WOR_KEYS[0])
#define WOR_NLEADS (sizeof WOR_LEADS / sizeof WOR_LEADS[0])
#define WOR_TK 0u                                /* track 1: TECLAS */
#define WOR_TL 1u                                /* track 2: LEAD */
enum { WM_KEYS, WM_SOLO, WM_LAYER, WM_SPLIT, WM_N };
static const char *const WOR_MODE[WM_N] = {"TECLAS", "SOLO", "SOMA", "DIVIDIR"};
#define WOR_SPLIT_KB 24u                         /* DIVIDIR: the lead from keybed key 24 (C5) up */
static const char *const WOR_KEYNAME[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

/* ------------------------------------------------------------ state --- */
#define WOR_NSONG 16u
#define WOR_NOKEY 12u
typedef struct {                                 /* a song of the setlist, and the live settings: 12 bytes */
    uint8_t used, key, bpm, keys_snd, lead_snd, pad_snd, mode, bright, xfade, dly;
    int8_t oct;
    uint8_t rsv;
} wor_song_t;
typedef struct {
    uint32_t magic, ver;
    wor_song_t song[WOR_NSONG];
    wor_song_t live;
    uint8_t cur;                                 /* the song loaded (0..15), 0xFF none */
    uint8_t view;                                /* 0 TOM, 1 SETLIST */
    uint8_t rsv[2];
} wor_store_t;
#define WOR_MAGIC 0x50485357u                    /* "WSHP" */
#define WOR_VER 1u
_Static_assert(sizeof(wor_song_t) == 12u && sizeof(wor_store_t) == 8u + 17u * 12u + 4u, "WORSHIP store layout");

static wor_store_t wor_st;
static wor_store_t wor_tmp __attribute__((section(".pool")));   /* (flash: a copy read back, the copy written) */
static struct {
    uint8_t applied_keys, applied_lead;          /* the sounds on the tracks now (0xFF: none yet) */
    uint8_t dirty_a, dirty_b;                    /* the screen's halves to draw again */
    uint32_t pend_keys_t, pend_lead_t;           /* a sound turned to, loaded once the knob rests (fm1_ms | 1) */
    uint32_t pad_t0[SMK_NPAD];                   /* SETLIST: when each pad went down (fm1_ms | 1), 0 = up */
    uint16_t pad_saved;                          /* .. those whose hold saved already */
    uint32_t tap[4], ntap;                       /* TAP tempo: the last taps (ms) */
    uint32_t meter_t;                            /* the pad meter's last draw */
    uint32_t msg_t;                              /* the message shows until then (fm1_ms), 0 none */
    char msg[32];
    uint8_t fader_last[4];
    uint8_t first;                               /* the sounds applied once since power-on */
    uint8_t last_meter;
} wor;

static void str_cat(char *d, const char *s, unsigned max)   /* (libc.c has str_cpy) */
{
    unsigned n = str_len(d);
    if (n + 1u < max)
        str_cpy(d + n, s, max - n);
}

static void wor_say(const char *a, const char *b)
{
    str_cpy(wor.msg, a, sizeof wor.msg);
    if (b)
        str_cat(wor.msg, b, sizeof wor.msg);
    wor.msg_t = fm1_ms + 2200u;
    if (!wor.msg_t)
        wor.msg_t = 1;
    wor.dirty_b = 1;
}

static void wor_num(char *b, uint32_t n, uint32_t sz)    /* n in decimal (b: at least 4) */
{
    char t[8];
    uint32_t i = 0, j = 0;
    do {
        t[i++] = (char)('0' + n % 10u);
        n /= 10u;
    } while (n && i < sizeof t);
    while (i && j + 1u < sz)
        b[j++] = t[--i];
    b[j] = 0;
}

static void wor_song_defaults(wor_song_t *s)
{
    memset(s, 0, sizeof *s);
    s->key = WOR_NOKEY;
    s->bpm = 72;
    s->keys_snd = 0;                             /* PIANO */
    s->lead_snd = 0;                             /* SAW LEAD */
    s->pad_snd = 0;                              /* QUENTE */
    s->mode = WM_KEYS;
    s->bright = 64;
    s->xfade = 64;
    s->dly = 40;
}

static void wor_defaults(void)
{
    uint32_t i;
    memset(&wor_st, 0, sizeof wor_st);
    wor_st.magic = WOR_MAGIC;
    wor_st.ver = WOR_VER;
    for (i = 0; i < WOR_NSONG; i++)
        wor_song_defaults(&wor_st.song[i]);
    wor_song_defaults(&wor_st.live);
    wor_st.cur = 0xFF;
}

static void wor_song_fix(wor_song_t *s)          /* any stored bytes: in range */
{
    s->used = s->used != 0u;
    if (s->key > WOR_NOKEY)
        s->key = WOR_NOKEY;
    if (s->bpm < 40u || s->bpm > 240u)
        s->bpm = 72;
    s->keys_snd = (uint8_t)(s->keys_snd % WOR_NKEYS);
    s->lead_snd = (uint8_t)(s->lead_snd % WOR_NLEADS);
    s->pad_snd = (uint8_t)(s->pad_snd % WP_NVOICES);
    s->mode = (uint8_t)(s->mode % WM_N);
    s->bright &= 127u;
    s->xfade &= 127u;
    s->dly &= 127u;
    s->oct = (int8_t)clamp(s->oct, -3, 3);
}

/* ------------------------------------------------------------ flash --- */
#if FELUCCA_FLASH
#define WOR_SEC_A 0xB1000u                       /* the SMK store's spare room (storage.c: 0xB1000..0xBAFFF) */
#define WOR_SEC_B 0xB2000u
#define WOR_TYPE 33u                             /* its commit records' type (no st_load object: >= OBJ_COUNT) */
_Static_assert(WOR_SEC_A >= ST_SMK_VBANK + ST_SECTOR && WOR_SEC_B + ST_SECTOR <= FL_DATA_HI &&
               FL_WIN_SAFE(WOR_SEC_A, WOR_SEC_B + ST_SECTOR), "WORSHIP store: inside FL_DATA, past the voice bank");
_Static_assert(sizeof(wor_store_t) <= ST_PAYLOAD_MAX, "WORSHIP store: one sector");

static int wor_head(uint32_t copy, st_hdr_t *h)
{
    uint32_t base = copy ? WOR_SEC_B : WOR_SEC_A;
    if (st_read(base, h, sizeof *h))
        return -1;
    if (h->magic != ST_MAGIC || h->type != WOR_TYPE || h->slot != copy || h->len != sizeof(wor_store_t) ||
        h->hcrc != st_crc32(h, sizeof *h - 4u))
        return -1;
    return 0;
}

static int wor_body(uint32_t copy, const st_hdr_t *h, wor_store_t *out)
{
    uint32_t base = copy ? WOR_SEC_B : WOR_SEC_A;
    if (st_read(base + ST_PAYLOAD_OFF, out, sizeof *out) || st_crc32(out, sizeof *out) != h->crc ||
        out->magic != WOR_MAGIC)
        return -1;
    return 0;
}

static int wor_current(st_hdr_t *h, wor_store_t *out)   /* the newest valid copy (0 A, 1 B), -1 none */
{
    st_hdr_t a, b;
    int va = flash_ok && wor_head(0, &a) == 0, vb = flash_ok && wor_head(1, &b) == 0;
    if (vb && (!va || (b.seq != a.seq && b.seq - a.seq < 0x80000000u)) && wor_body(1, &b, out) == 0) {
        *h = b;
        return 1;
    }
    if (va && wor_body(0, &a, out) == 0) {
        *h = a;
        return 0;
    }
    if (vb && wor_body(1, &b, out) == 0) {
        *h = b;
        return 1;
    }
    return -1;
}

static void wor_load(void)
{
    st_hdr_t h;
    uint32_t i;
    wor_defaults();
    if (wor_current(&h, &wor_tmp) < 0) {
        if (st_owner_cleared && flash_ok) {      /* the store was another firmware's: its leftovers here too */
            st_erase(WOR_SEC_A);                 /* (fm1-to-smk37 docs/08-pitfalls.md; power-on: no audio yet) */
            st_erase(WOR_SEC_B);
        }
        return;
    }
    wor_st = wor_tmp;
    for (i = 0; i < WOR_NSONG; i++)
        wor_song_fix(&wor_st.song[i]);
    wor_song_fix(&wor_st.live);
    if (wor_st.cur >= WOR_NSONG)
        wor_st.cur = 0xFF;
    wor_st.view &= 1u;
}

/* both copies kept A / B: the new one goes to the older, its commit record last; a cut write leaves the other */
static int wor_save(void)
{
    wor_store_t *tmp = &wor_tmp;
    st_hdr_t h;
    st_hdr_t chk;
    uint32_t seq, base, off, to;
    int cur, rc;
    if (!flash_ok)
        return -1;
    cur = wor_current(&h, tmp);
    seq = cur < 0 ? 0u : h.seq;
    to = cur == 0 ? 1u : 0u;
    base = to ? WOR_SEC_B : WOR_SEC_A;
    *tmp = wor_st;
    if ((rc = st_erase(base)) != 0)
        return rc;
    for (off = 0; off < sizeof *tmp; off += 256u) {
        uint32_t n = sizeof *tmp - off > 256u ? 256u : sizeof *tmp - off;
        if ((rc = st_prog(base + ST_PAYLOAD_OFF + off, (const uint8_t *)tmp + off, n)) != 0)
            return rc;
    }
    h.magic = ST_MAGIC;
    h.type = WOR_TYPE;
    h.slot = (uint16_t)to;
    h.seq = seq + 1u;
    h.len = sizeof *tmp;
    h.crc = st_crc32(tmp, sizeof *tmp);
    h.rsv[0] = h.rsv[1] = 0xFFFFFFFFu;
    h.hcrc = st_crc32(&h, sizeof h - 4u);
    if ((rc = st_prog(base, &h, sizeof h)) != 0)
        return rc;
    if (wor_head(to, &chk) || memcmp(&chk, &h, sizeof h) || wor_body(to, &chk, tmp))
        return -7;
    return 0;
}
#else
static void wor_load(void) { wor_defaults(); }
static int wor_save(void) { return -1; }
#endif

/* ------------------------------------------------------- the sounds --- */
static void wor_load_sound(uint32_t tr, const wor_snd_t *s, int lead)
{
    track_t *t = &trk[tr];
    int16_t lvl = t->p[P_LEVEL];                 /* (the fader's) */
    set_engine_of(t, s->engine);                 /* (the engine's defaults, its first preset) */
    apply_preset_to(t, s->preset);             /* (the audio ISR switches the engine, with its fade) */
    t->p[P_LEVEL] = lvl;
    t->p[P_CHRD] = 0;                            /* one key, one note: the routing (wor_press) */
    t->p[P_AMODE] = 0;                           /* (no ARP, no scale snapping, no transpose: the keys as printed) */
    t->p[P_QUANT] = 0;                           /* QN_OFF */
    t->p[P_TRANS] = 0;
    if (lead) {
        t->p[P_VOICE] = V_LEGATO;                /* a solo voice: legato, a short glide, */
        t->p[P_GLIDE] = 12;
        t->p[P_PRIO] = 2;                        /* the highest key held: SOMA's chord under it, the melody on top */
        t->p[P_DLY] = wor_st.live.dly;
        t->p[P_REV] = (int16_t)(40 + wor_st.live.dly / 4);
    } else {
        t->p[P_DLY] = 0;
        t->p[P_REV] = (int16_t)(t->p[P_REV] > 30 ? t->p[P_REV] : 30);
    }
}

static void wor_apply_keys(void)
{
    uint32_t k = wor_st.live.keys_snd % WOR_NKEYS;
    if (wor.applied_keys == k)
        return;
    wor.applied_keys = (uint8_t)k;
    wor_load_sound(WOR_TK, &WOR_KEYS[k], 0);
}

static void wor_apply_lead(void)
{
    uint32_t k = wor_st.live.lead_snd % WOR_NLEADS;
    if (wor.applied_lead == k)
        return;
    wor.applied_lead = (uint8_t)k;
    wor_load_sound(WOR_TL, &WOR_LEADS[k], 1);
}

/* the selected track: the wheels bend it (seq.c smk_wheels_block); the lead whenever it plays */
static void wor_sel(void)
{
    song.sel = (uint8_t)(wor_st.live.mode == WM_KEYS ? WOR_TK : WOR_TL);
}

static void wor_apply_globals(void)
{
    wor_song_t *l = &wor_st.live;
    song.g[G_BPM] = l->bpm;
    song.g[G_DTIME] = 10;                        /* the delay: a dotted eighth (fx.c delay_samples) */
    if (song.g[G_DFDBK] >= 60 || song.g[G_DFDBK] < 25)
        song.g[G_DFDBK] = 40;                    /* (Felucca's 60: a long tail; a few repeats under a solo) */
    song.octave = l->oct;
    trk[WOR_TL].p[P_DLY] = l->dly;
    wp_bright = l->bright;
    wp_xfade = l->xfade;
    wor_sel();
}

static void wor_apply_all(void)
{
    wor_apply_keys();
    wor_apply_lead();
    wor_apply_globals();
}

/* ------------------------------------------------------- the routing --- */
/* audio ISR (seq.c): keybed key kb down, velocity vel; the note for a track as its keys play it (kb_map: octave) */
static int wor_note(uint32_t tr, uint32_t kb, uint32_t vel, int on)
{
    track_t *t = &trk[tr];
    uint32_t mc = trk_midi_ch(tr), x;
    if (on) {
        x = kb_map(t, (uint32_t)((int32_t)kb - (int32_t)SMK_KEY_F3));
        if (x == KB_SILENT)
            return 0;
        wor_knote[kb] = (uint8_t)x;
        if (midi_local_held(t, x))
            return 1;
        input_on(t, x, vel);
        midi_out_event(0x09u | (0x90u | mc) << 8 | x << 16 | vel << 24);
    } else {
        x = wor_knote[kb];
        if (midi_local_held(t, x))
            return 1;
        input_off(t, x);
        midi_out_event(0x08u | (0x80u | mc) << 8 | x << 16);
    }
    return 1;
}

static void wor_press(uint32_t kb, uint32_t vel)
{
    uint32_t m, mode = wor_st.live.mode;
    if (kb >= FM1_NKEY)
        return;
    m = mode == WM_KEYS ? 1u : mode == WM_SOLO ? 2u : mode == WM_LAYER ? 3u : kb >= WOR_SPLIT_KB ? 2u : 1u;
    wor_kmask[kb] = (uint8_t)(((m & 1u) && wor_note(WOR_TK, kb, vel, 1)) | ((m & 2u) && wor_note(WOR_TL, kb, vel, 1)) << 1);
}

static void wor_release(uint32_t kb)
{
    uint32_t m;
    if (kb >= FM1_NKEY)
        return;
    m = wor_kmask[kb];
    wor_kmask[kb] = 0;
    if (m & 1u)
        wor_note(WOR_TK, kb, 0, 0);
    if (m & 2u)
        wor_note(WOR_TL, kb, 0, 0);
}

/* --------------------------------------------------------- the songs --- */
static uint32_t wor_key_now(void)                /* the pad's key now, else the one chosen last */
{
    uint32_t k = wp_key_now;
    return k < 12u ? k : wor_st.live.key;
}

static void wor_song_load(uint32_t i)
{
    char b[8];
    if (i >= WOR_NSONG)
        return;
    if (!wor_st.song[i].used) {
        wor_num(b, i + 1u, sizeof b);
        wor_say("MUSICA ", b);
        str_cat(wor.msg, " VAZIA", sizeof wor.msg);
        return;
    }
    wor_st.cur = (uint8_t)i;
    wor_st.live = wor_st.song[i];
    wor_st.live.used = 1;
    wor_apply_all();
    if (wor_st.live.key < 12u && wp_sounding())  /* the pad follows into the new key */
        wp_key(wor_st.live.key, wor_st.live.pad_snd);
    wor_num(b, i + 1u, sizeof b);
    wor_say("MUSICA ", b);
    if (wor_st.live.key < 12u) {
        str_cat(wor.msg, "  TOM ", sizeof wor.msg);
        str_cat(wor.msg, WOR_KEYNAME[wor_st.live.key], sizeof wor.msg);
    }
    wor.dirty_a = 1;
}

static void wor_song_save(uint32_t i)
{
    char b[8];
    int rc;
    if (i >= WOR_NSONG)
        return;
    wor_st.live.key = (uint8_t)wor_key_now();
    wor_st.live.used = 1;
    wor_st.song[i] = wor_st.live;
    wor_st.cur = (uint8_t)i;
    rc = wor_save();
    wor_num(b, i + 1u, sizeof b);
    wor_say(rc ? "ERRO AO SALVAR " : "SALVO NA MUSICA ", b);
    wor.dirty_a = 1;
}

/* ---------------------------------------------------------- the pads --- */
/* TOM view: hardware pad -> key 0..11; WP_STOPPAD stops; -1 nothing (the piano's gaps) */
#define WOR_STOPPAD 7u
static const int8_t WOR_PADKEY[SMK_NPAD] = {0, 2, 4, 5, 7, 9, 11, -2, 1, 3, -1, 6, 8, 10, -1, -1};

static void wor_pad_key(uint32_t key)
{
    if (wp_key_now == key) {                     /* the key playing, tapped: it stops */
        wp_stop();
        wor_say("PAD PARADO", 0);
    } else {
        wp_key(key, wor_st.live.pad_snd);
        wor_say("PAD EM ", WOR_KEYNAME[key]);
    }
    wor_st.live.key = (uint8_t)key;
    wor.dirty_a = 1;
}

static void wor_pads(void)
{
    int32_t e;
    uint32_t p;
    while ((e = smk_padev_take()) >= 0) {
        uint32_t pad = ((uint32_t)e >> 8) % SMK_NPAD, vel = (uint32_t)e & 0x7Fu;
        if (!vel) {                              /* let go: a tap in SETLIST loads (a hold saved already) */
            uint32_t t0 = wor.pad_t0[pad];
            wor.pad_t0[pad] = 0;
            if (wor_st.view && t0 && !((wor.pad_saved >> pad) & 1u))
                wor_song_load(pad);
            wor.pad_saved &= (uint16_t)~(1u << pad);
            continue;
        }
        if (wor_st.view) {
            wor.pad_t0[pad] = fm1_ms | 1u;
            continue;
        }
        if (WOR_PADKEY[pad] >= 0)
            wor_pad_key((uint32_t)WOR_PADKEY[pad]);
        else if (pad == WOR_STOPPAD) {
            wp_stop();
            wor_say("PAD PARADO", 0);
            wor.dirty_a = 1;
        }
    }
    if (!wor_st.view)
        return;
    for (p = 0; p < SMK_NPAD; p++)               /* SETLIST: a pad held a second saves into its song */
        if (wor.pad_t0[p] && !((wor.pad_saved >> p) & 1u) && fm1_ms - wor.pad_t0[p] >= 1000u) {
            wor.pad_saved |= (uint16_t)(1u << p);
            wor_song_save(p);
        }
}

/* --------------------------------------------------------- the faders --- */
/* 1 TECLAS, 2 PAD, 3 LEAD, 4 AMBIENCIA; each takes over once it moves (as main.c smk_faders) */
static void wor_faders(void)
{
    uint32_t i;
    for (i = 0; i < 4u; i++) {
        uint32_t v = fm1_in.fader[i];
        if (wor.fader_last[i] == 0xFF || fm1_in.frames < 200u) {
            wor.fader_last[i] = (uint8_t)v;
            continue;
        }
        if (v == wor.fader_last[i])
            continue;
        wor.fader_last[i] = (uint8_t)v;
        if (i == 0)
            trk[WOR_TK].p[P_LEVEL] = (int16_t)v;
        else if (i == 1)
            wp_level = (uint8_t)v;
        else if (i == 2)
            trk[WOR_TL].p[P_LEVEL] = (int16_t)v;
        else {
            wp_amb = (uint8_t)v;
            trk[WOR_TK].p[P_REV] = (int16_t)(v * 3u / 4u);
            trk[WOR_TL].p[P_REV] = (int16_t)(v * 7u / 8u);
        }
        wor.dirty_b = 1;
    }
}

/* ---------------------------------------------------------- the LEDs --- */
static void wor_leds(void)
{
    uint32_t p, i, k = wp_key_now, sk = wor_st.live.key;
    uint32_t bpm = wor_st.live.bpm ? wor_st.live.bpm : 72u;
    for (i = 0; i < SMK_NLED; i++)
        fm1_led[i] = 0;
    fm1_led[panel.btn[B_SEQ]] = wor_st.view != 0u;
    fm1_led[panel.btn[B_ARP]] = wor_st.live.mode != WM_KEYS;
    fm1_led[panel.btn[B_ENV]] = k < 12u;                          /* CHORD: the pad sounds */
    fm1_led[panel.btn[B_LFO]] = fm1_ms % (60000u / bpm) < 12000u / bpm;   /* NOTE REPEAT (TAP): the beat, flashing */
    fm1_led[panel.btn[B_GLO]] = 1;                                /* GLOBE: WORSHIP is on */
    fm1_led[panel.btn[B_OCTDN]] = wor_st.live.oct < 0;
    fm1_led[panel.btn[B_OCTUP]] = wor_st.live.oct > 0;
    if ((int32_t)(fm1_ms - padled_hold_until) < 0)
        return;
    for (p = 0; p < SMK_NPAD; p++) {
        uint8_t r = 0, g = 0, b = 0;
        if (wor_st.view) {                                        /* SETLIST */
            if (p == wor_st.cur) {
                r = 0; g = 170; b = 255;
            } else if (wor_st.song[p].used) {
                r = 0; g = 24; b = 40;
            } else {
                r = 3; g = 3; b = 3;
            }
            if (wor.pad_t0[p]) {                                  /* held: white, brighter towards the save */
                uint32_t h = fm1_ms - wor.pad_t0[p];
                uint8_t w = (uint8_t)(h >= 1000u ? 255u : 40u + h / 5u);
                r = g = b = w;
            }
        } else if (WOR_PADKEY[p] >= 0) {                          /* TOM */
            uint32_t key = (uint32_t)WOR_PADKEY[p];
            if (key == k) {
                r = 255; g = 140; b = 20;
            } else if (k >= 12u && key == sk) {
                r = 60; g = 34; b = 6;
            } else if (key == 1u || key == 3u || key == 6u || key == 8u || key == 10u) {
                r = 6; g = 6; b = 10;
            } else {
                r = 16; g = 14; b = 12;
            }
        } else if (p == WOR_STOPPAD) {
            if (k < 12u) {
                r = 120; g = 0; b = 0;
            } else {
                r = 12; g = 0; b = 0;
            }
        }
        padled_set(p, r, g, b);
    }
}

/* ---------------------------------------------------------- the screen --- */
static void wor_draw_head(void)
{
    char b[32], n[8];
    cv_text(10, 5, &AF_S, "WORSHIP", T_THEME);
    b[0] = 0;
    if (wor_st.cur < WOR_NSONG) {
        str_cpy(b, "MUSICA ", sizeof b);
        wor_num(n, wor_st.cur + 1u, sizeof n);
        str_cat(b, n, sizeof b);
        str_cat(b, "  \xB7  ", sizeof b);
    }
    wor_num(n, wor_st.live.bpm, sizeof n);
    str_cat(b, n, sizeof b);
    str_cat(b, " BPM", sizeof b);
    cv_text_r(230, 5, &AF_S, b, T_MID, T_BG);
    cv_rect(10, 23, 220, 1, T_LINE);
}

static void wor_draw_tom(void)
{
    char b[24], n[8];
    uint32_t k = wp_key_now, sk = wor_st.live.key, ms, w;
    int32_t pk;
    cv_rrect(10, 30, 100, 86, 8, T_SURF, T_BG);
    cv_text(20, 36, &AF_S, "TOM", T_MID);
    if (k < 12u)
        cv_text_in(10, 56, 100, &AF_L, WOR_KEYNAME[k], T_ACCENT, T_SURF);
    else
        cv_text_in(10, 56, 100, &AF_L, sk < 12u ? WOR_KEYNAME[sk] : "-", T_DIM, T_SURF);
    cv_text_in(10, 96, 100, &AF_S, k < 12u ? "TOCANDO" : "PARADO", k < 12u ? T_TEXT : T_MID, T_SURF);
    cv_text(122, 32, &AF_S, "PAD", T_MID);
    cv_text(122, 48, &AF_M, WP_VOICES[wor_st.live.pad_snd % WP_NVOICES].name, T_TEXT);
    str_cpy(b, "BRILHO ", sizeof b);
    wor_num(n, wor_st.live.bright, sizeof n);
    str_cat(b, n, sizeof b);
    cv_text(122, 72, &AF_S, b, T_MID);
    ms = wp_fade_samples() * 10u / FS;           /* tenths of a second */
    str_cpy(b, "TRANSICAO ", sizeof b);
    wor_num(n, ms / 10u, sizeof n);
    str_cat(b, n, sizeof b);
    str_cat(b, ".", sizeof b);
    wor_num(n, ms % 10u, sizeof n);
    str_cat(b, n, sizeof b);
    str_cat(b, " S", sizeof b);
    cv_text(122, 88, &AF_S, b, T_MID);
    cv_rect(122, 108, 108, 6, T_RAISE);           /* the pad's level */
    pk = wp.peak;
    w = 0;
    while (pk > 64 && w < 108u) {                /* ~6 dB a step of 9 px: a log meter */
        pk >>= 1;
        w += 9u;
    }
    wor.last_meter = (uint8_t)w;
    if (w)
        cv_rect(122, 108, (int32_t)(w > 108u ? 108u : w), 6, k < 12u ? T_THEME : T_MID);
}

static void wor_draw_setlist(void)
{
    uint32_t i;
    char n[8];
    for (i = 0; i < WOR_NSONG; i++) {
        int32_t x = 10 + (int32_t)(i % 8u) * 28, y = 30 + (int32_t)(i / 8u) * 44;
        const wor_song_t *s = &wor_st.song[i];
        uint16_t fill = i == wor_st.cur ? T_SEL : s->used ? T_SURF : T_BG;
        uint16_t ink = i == wor_st.cur ? T_INK : s->used ? T_TEXT : T_DIM;
        if (s->used || i == wor_st.cur)
            cv_rrect(x, y, 26, 40, 5, fill, T_BG);
        else
            cv_frame(x, y, 26, 40, T_LINE);
        wor_num(n, i + 1u, sizeof n);
        cv_text_in(x, y + 3, 26, &AF_S, n, ink, fill);
        if (s->used)
            cv_text_in(x, y + 19, 26, &AF_M, s->key < 12u ? WOR_KEYNAME[s->key] : "-", ink, fill);
    }
}

static void wor_row(int32_t y, const char *label, const char *value, uint16_t vc)
{
    cv_rrect(10, y, 220, 26, 6, T_SURF, T_BG);
    cv_text(18, y + 6, &AF_S, label, T_MID);
    cv_text(84, y + 4, &AF_M, value, vc);
}

static void wor_draw(void)
{
    uint32_t mode = wor_st.live.mode;
    if (ui.force) {
        ui.force = 0;
        wor.dirty_a = wor.dirty_b = 1;
    }
    if (!wor_st.view && fm1_ms - wor.meter_t >= 100u && (wp_sounding() || wor.last_meter)) {
        wor.meter_t = fm1_ms;
        wor.dirty_a = 1;
    }
    if (wor.msg_t && (int32_t)(fm1_ms - wor.msg_t) >= 0) {
        wor.msg_t = 0;
        wor.dirty_b = 1;
    }
    if (wor.dirty_a) {
        wor.dirty_a = 0;
        cv_begin(240, 120, T_BG);
        wor_draw_head();
        if (wor_st.view)
            wor_draw_setlist();
        else
            wor_draw_tom();
        cv_blit(0, 0);
    }
    if (wor.dirty_b) {
        wor.dirty_b = 0;
        cv_begin(240, 120, T_BG);
        wor_row(2, "TECLAS", WOR_KEYS[wor_st.live.keys_snd % WOR_NKEYS].name, mode == WM_SOLO ? T_DIM : T_TEXT);
        wor_row(32, "LEAD", WOR_LEADS[wor_st.live.lead_snd % WOR_NLEADS].name, mode == WM_KEYS ? T_DIM : T_TEXT);
        wor_row(62, "MODO", WOR_MODE[mode % WM_N], T_ACCENT);
        if (wor.msg_t)
            cv_text_in(0, 96, 240, &AF_M, wor.msg, T_TEXT, T_BG);
        else
            cv_text_in(0, 99, 240, &AF_S, wor_st.view ? "TOQUE: CARREGAR   SEGURE: SALVAR" :
                       "PADS: TOM   SEQ: SETLIST   ARP: MODO", T_MID, T_BG);
        cv_blit(0, 120);
    }
}

/* ---------------------------------------------------------- the knobs --- */
static int32_t wor_turn(int32_t v, int32_t s, int32_t lo, int32_t hi) { return clamp(v + s, lo, hi); }

static void wor_knobs(void)
{
    wor_song_t *l = &wor_st.live;
    int32_t s;
    char n[8];
    if ((s = panel_enc(EN_SELECT)) != 0) {       /* K2: TECLAS */
        l->keys_snd = (uint8_t)(((int32_t)l->keys_snd + s % (int32_t)WOR_NKEYS + (int32_t)WOR_NKEYS) % (int32_t)WOR_NKEYS);
        wor.pend_keys_t = fm1_ms | 1u;
        wor_say("TECLAS: ", WOR_KEYS[l->keys_snd].name);
    }
    if ((s = panel_enc(EN_ALGO)) != 0) {         /* K3: LEAD */
        l->lead_snd = (uint8_t)(((int32_t)l->lead_snd + s % (int32_t)WOR_NLEADS + (int32_t)WOR_NLEADS) % (int32_t)WOR_NLEADS);
        wor.pend_lead_t = fm1_ms | 1u;
        wor_say("LEAD: ", WOR_LEADS[l->lead_snd].name);
    }
    if ((s = panel_enc(EN_PRESET)) != 0) {       /* K4: PAD (crossfades into it while sounding) */
        l->pad_snd = (uint8_t)(((int32_t)l->pad_snd + s % (int32_t)WP_NVOICES + (int32_t)WP_NVOICES) % (int32_t)WP_NVOICES);
        if (wp_key_now < 12u)
            wp_key(wp_key_now, l->pad_snd);
        wor_say("PAD: ", WP_VOICES[l->pad_snd].name);
        wor.dirty_a = 1;
    }
    if ((s = panel_enc(EN_K1)) != 0) {           /* K5: BRILHO */
        l->bright = (uint8_t)wor_turn(l->bright, s * 2, 0, 127);
        wp_bright = l->bright;
        wor_num(n, l->bright, sizeof n);
        wor_say("BRILHO ", n);
        wor.dirty_a = 1;
    }
    if ((s = panel_enc(EN_K2)) != 0) {           /* K6: TRANSICAO */
        l->xfade = (uint8_t)wor_turn(l->xfade, s * 2, 0, 127);
        wp_xfade = l->xfade;
        wor.dirty_a = 1;
        wor_say("TRANSICAO", 0);
    }
    if ((s = panel_enc(EN_K3)) != 0) {           /* K7: DELAY of the lead */
        l->dly = (uint8_t)wor_turn(l->dly, s * 2, 0, 127);
        trk[WOR_TL].p[P_DLY] = l->dly;
        wor_num(n, l->dly, sizeof n);
        wor_say("DELAY DO LEAD ", n);
    }
    if ((s = panel_enc(EN_K4)) != 0) {           /* K8: BPM */
        l->bpm = (uint8_t)wor_turn(l->bpm, s, 40, 240);
        song.g[G_BPM] = l->bpm;
        wor_num(n, l->bpm, sizeof n);
        wor_say("BPM ", n);
        wor.dirty_a = 1;
    }
    if (wor.pend_keys_t && fm1_ms - wor.pend_keys_t > 250u) {   /* the sound once the knob rests */
        wor.pend_keys_t = 0;
        wor_apply_keys();
    }
    if (wor.pend_lead_t && fm1_ms - wor.pend_lead_t > 250u) {
        wor.pend_lead_t = 0;
        wor_apply_lead();
    }
}

static void wor_tap(void)
{
    uint32_t now = fm1_ms, i, sum = 0, n = 0;
    char b[8];
    if (wor.ntap && now - wor.tap[(wor.ntap - 1u) % 4u] > 2000u)
        wor.ntap = 0;                            /* a pause: a new count */
    wor.tap[wor.ntap % 4u] = now;
    wor.ntap++;
    if (wor.ntap < 2u) {
        wor_say("TAP...", 0);
        return;
    }
    for (i = 1; i < wor.ntap && i < 4u; i++) {
        uint32_t a = wor.tap[(wor.ntap - i) % 4u], c = wor.tap[(wor.ntap - i - 1u) % 4u];
        sum += a - c;
        n++;
    }
    if (!n || !sum)
        return;
    wor_st.live.bpm = (uint8_t)clamp((int32_t)(60000u * n / sum), 40, 240);
    song.g[G_BPM] = wor_st.live.bpm;
    wor_num(b, wor_st.live.bpm, sizeof b);
    wor_say("BPM ", b);
    wor.dirty_a = 1;
}

/* ------------------------------------------------------ enter, leave --- */
static void wor_enter(void)
{
    uint32_t i;
    ui.menu = 0;
    ui.confirm = 0;
    name_close();
    song.grid = 0;
    song.seq_mode = 0;
    for (i = 0; i < 4u; i++)
        wor.fader_last[i] = 0xFF;                /* each fader takes over once it moves */
    if (!wor.first) {
        wor.first = 1;
        wor.applied_keys = wor.applied_lead = 0xFF;
        wor_apply_all();
    } else
        wor_apply_globals();
    smk_pad_raw = 1;
    wor_on = 1;
    ui.force = 1;
    wor.dirty_a = wor.dirty_b = 1;
    wor_say("WORSHIP", 0);
}

static void wor_leave(void)
{
    wor_on = 0;
    smk_pad_raw = 0;
    ui.home = 1;
    ui.layer = 0;
    ui.force = 1;
    lcd_fill(0, 0, 240, 240, T_BG);
    ui_say("SYNTH  ", "GLOBE: VOLTA");    /* (ui.msg: 23 characters at most) */
}

/* power-on (main.c, after the music is restored): the setlist, the live settings, WORSHIP on */
static void wor_boot(void)
{
    wor_load();
    wor.first = 0;
    wor_enter();
    wor_say(wor_st.cur < WOR_NSONG ? "PRONTO  \xB7  SETLIST CARREGADO" : "PRONTO", 0);
}

/* ui_input (ui_input.c): 1 = WORSHIP took this pass. Off: GLOBE turns it on */
static int wor_ui_input(uint32_t pressed, uint32_t notes)
{
    uint32_t k;
    if (!wor_on) {
        if (((pressed >> panel.btn[B_GLO]) & 1u) && !ui.menu && !ui.confirm && !name_on()) {
            wor_enter();
            return 1;
        }
        return 0;
    }
    wp_swell = smk_modw;
    wor_pads();                                  /* (the pads act with the screen dark too: they never wake it) */
    if (scr_input(pressed, notes))               /* MENU > SCREEN OFF: as ever */
        return 1;
    wor_knobs();
    if ((pressed >> panel.btn[B_GLO]) & 1u) {    /* GLOBE: the synthesizer */
        wor_leave();
        return 1;
    }
    if ((pressed >> panel.btn[B_ARP]) & 1u) {    /* ARP: MODO */
        wor_st.live.mode = (uint8_t)((wor_st.live.mode + 1u) % WM_N);
        wor_sel();
        wor_say("MODO ", WOR_MODE[wor_st.live.mode]);
    }
    if ((pressed >> panel.btn[B_SEQ]) & 1u) {    /* SEQ: TOM / SETLIST */
        wor_st.view ^= 1u;
        for (k = 0; k < SMK_NPAD; k++)
            wor.pad_t0[k] = 0;
        wor.pad_saved = 0;
        wor_say(wor_st.view ? "SETLIST" : "TOM", 0);
        wor.dirty_a = 1;
    }
    if ((pressed >> panel.btn[B_ENV]) & 1u) {    /* CHORD: the pad on / off */
        if (wp_key_now < 12u) {
            wp_stop();
            wor_say("PAD PARADO", 0);
        } else if (wor_st.live.key < 12u) {
            wp_key(wor_st.live.key, wor_st.live.pad_snd);
            wor_say("PAD EM ", WOR_KEYNAME[wor_st.live.key]);
        } else
            wor_say("ESCOLHA O TOM NOS PADS", 0);
        wor.dirty_a = 1;
    }
    if ((pressed >> panel.btn[B_LFO]) & 1u)      /* NOTE REPEAT: TAP tempo */
        wor_tap();
    if ((pressed >> panel.btn[B_SAVE]) & 1u) {   /* BT: SALVAR into the current song */
        if (wor_st.cur < WOR_NSONG)
            wor_song_save(wor_st.cur);
        else
            wor_say("SEQ: SEGURE UM PAD P/ SALVAR", 0);
    }
    if ((pressed >> panel.btn[B_HOME]) & 1u) {   /* PATCH: the keys and the lead silenced (not the pad) */
        panic_req |= (uint8_t)(1u << WOR_TK | 1u << WOR_TL);
        wor_say("TECLAS SILENCIADAS", 0);
    }
    if (((pressed >> SMK_BTN_LEFT) & 1u) || ((pressed >> SMK_BTN_RIGHT) & 1u)) {   /* < >: the songs */
        int32_t d = ((pressed >> SMK_BTN_RIGHT) & 1u) ? 1 : -1, i = wor_st.cur < WOR_NSONG ? wor_st.cur : -d, n;
        for (n = 0; n < (int32_t)WOR_NSONG; n++) {
            i = (i + d + (int32_t)WOR_NSONG) % (int32_t)WOR_NSONG;
            if (wor_st.song[i].used)
                break;
        }
        if (n < (int32_t)WOR_NSONG)
            wor_song_load((uint32_t)i);
        else
            wor_say("SETLIST VAZIO", 0);
    }
    if ((pressed >> panel.btn[B_OCTDN]) & 1u || (pressed >> panel.btn[B_OCTUP]) & 1u) {
        char b[4] = {0, 0, 0, 0};
        int32_t o = wor_st.live.oct + (((pressed >> panel.btn[B_OCTUP]) & 1u) ? 1 : 0) -
                    (((pressed >> panel.btn[B_OCTDN]) & 1u) ? 1 : 0);
        wor_st.live.oct = (int8_t)clamp(o, -3, 3);
        song.octave = wor_st.live.oct;
        b[0] = wor_st.live.oct < 0 ? '-' : '+';
        b[1] = (char)('0' + (wor_st.live.oct < 0 ? -wor_st.live.oct : wor_st.live.oct));
        wor_say("OITAVA ", b);
    }
    return 1;
}
