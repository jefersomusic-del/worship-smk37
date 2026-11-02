/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments; SMK-37 / WORSHIP: Jeferson Mesquita */
/* The SMK-37 Pro build (BOARD_SMK37, WORSHIP on) on the host, as web/emu/felucca_web.c runs the FM-1's: the
 * firmware's own sources (tests/hostsim.c's order, then the UI, storage, projects, worship.c, main.c) against a
 * simulated SMK-37 panel:
 *   - the device clock: TIMER4 ticks and fm1_ms advance with the audio asked for; the audio ISR (audio.c) renders
 *     each I2S half; main.c's loop runs every 15 ms, ui_input in between;
 *   - the panel: the 18 buttons and 8 knobs by their SMK ids (hal/smk_input.h), the 37 keys with velocity (the 27
 *     middle ones as fm1_in.notes, the 10 outer ones through the outer-key queue), the 16 pads (as notes, or RAW for
 *     WORSHIP), the 4 faders, the wheels; the button LEDs (fm1_led) and the pads' colours (padled_set) read back;
 *   - the screen: lcd_fill / lcd_blit into a 240 x 240 RGB565 frame;
 *   - the flash: a 1 MiB NOR image behind storage.c's hooks (the SMK map: hal/fm1_flash_map.h).
 * Native (tests/worship_host_test.c) and, like felucca_web.c, meant for WebAssembly. Nothing of the firmware changes
 * for this file. */
#include <stdint.h>

#define FELUCCA_WEB 1
#define HOST_SMK 1
#define BOARD_SMK37 1
#ifndef FELUCCA_WORSHIP
#define FELUCCA_WORSHIP 1
#endif
#ifndef FELUCCA_CDC
#define FELUCCA_CDC 1
#endif
#define FELUCCA_FLASH 1
#define FELUCCA_OTA 0
#define FELUCCA_UAC 0
#define FELUCCA_UART 0
#ifndef FELUCCA_VERSION
#define FELUCCA_VERSION "WEB"
#endif

/* ------------------------------------------------- the SMK-37's panel --- */
#define FM1_NCOL 16u
#define FM1_NBTN 18u
#define FM1_NKEY 37u
#define FM1_NENC 8u
#define SMK_NPAD 16u
#define SMK_NLED 19u
#define SMK_KEY_F3 5u
enum { FM1_BTN_OCT_DOWN = 4, FM1_BTN_OCT_UP = 5 };
enum { SMK_BTN_KNOB_BANK = 0, SMK_BTN_FADER_BANK = 1, SMK_BTN_LEFT = 2, SMK_BTN_RIGHT = 3 };
#define FM1_OCT_BOTH (1u << FM1_BTN_OCT_DOWN | 1u << FM1_BTN_OCT_UP)
static const uint8_t SMK_WHITE_NOTE[SMK_NPAD] = {0, 2, 4, 6, 7, 9, 11, 12, 14, 16, 18, 19, 21, 23, 24, 26};
static volatile uint8_t smk_pad_grid;
#define SMK_PAD_OF_WHITE(w) (smk_pad_grid ? (w) : (w) < 8u ? (w) + 8u : (w) - 8u)
static const int8_t SMK_PIANO_NOTE[SMK_NPAD] = {7, 9, 11, 12, 14, 16, 18, 19, 8, 10, -1, 13, 15, 17, -1, 20};
static volatile uint8_t smk_pad_piano;
static uint8_t smk_pad_note[SMK_NPAD];
#define SMK_WHITE_OF_PAD(p) (smk_pad_grid ? (p) : (p) < 8u ? (p) + 8u : (p) - 8u)
#define SMK_TRIM_FLASH 0xC1000u
static struct {
    volatile uint32_t notes, knotes, pnotes, buttons, pressed, released, notes_pressed;
    uint64_t keys;
    uint8_t key_vel[FM1_NKEY];
    int16_t enc_steps[FM1_NENC];
    uint8_t fader[4];
    uint16_t wheel[2], pedal, batt;
    uint32_t frames;
} fm1_in;
static uint8_t fm1_led[SMK_NLED];
#define SMK_EXTQ 32u
static volatile uint16_t smk_extq[SMK_EXTQ];
static volatile uint8_t smk_extq_w, smk_extq_r;
static void smk__extq(uint32_t k, uint32_t vel)
{
    uint8_t w = smk_extq_w;
    if ((uint8_t)(w + 1u) % SMK_EXTQ == smk_extq_r)
        return;
    smk_extq[w] = (uint16_t)(k << 8 | vel);
    smk_extq_w = (uint8_t)((w + 1u) % SMK_EXTQ);
}
static int32_t smk_extkey_take(void)
{
    int32_t v = -1;
    if (smk_extq_r != smk_extq_w) {
        v = smk_extq[smk_extq_r];
        smk_extq_r = (uint8_t)((smk_extq_r + 1u) % SMK_EXTQ);
    }
    return v;
}
static void smk__key_event(uint32_t k, uint32_t vel)     /* as hal/smk_input.h */
{
    if (vel)
        fm1_in.key_vel[k] = (uint8_t)vel;
    if (k >= SMK_KEY_F3 && k < SMK_KEY_F3 + 27u) {
        uint32_t bit = 1u << (k - SMK_KEY_F3);
        if (vel) {
            fm1_in.knotes |= bit;
            fm1_in.notes_pressed |= bit;
        } else {
            fm1_in.knotes &= ~bit;
        }
        fm1_in.notes = fm1_in.knotes | fm1_in.pnotes;
    } else {
        smk__extq(k, vel);
    }
    if (vel)
        fm1_in.keys |= (uint64_t)1u << k;
    else
        fm1_in.keys &= ~((uint64_t)1u << k);
}
/* RAW pads (WORSHIP): as hal/smk_input.h */
#define SMK_PADEV_N 32u
static volatile uint8_t smk_pad_raw;
static volatile uint16_t smk_pad_rawheld;
static volatile uint16_t smk_padev[SMK_PADEV_N];
static volatile uint8_t smk_padev_w, smk_padev_r;
static int smk__pad_raw(uint32_t pad, uint32_t vel)
{
    uint32_t bit = 1u << pad;
    if (vel ? !smk_pad_raw : !(smk_pad_rawheld & bit))
        return 0;
    smk_pad_rawheld = (uint16_t)(vel ? smk_pad_rawheld | bit : smk_pad_rawheld & ~bit);
    if ((uint8_t)(smk_padev_w - smk_padev_r) < SMK_PADEV_N) {
        smk_padev[smk_padev_w % SMK_PADEV_N] = (uint16_t)(pad << 8 | (vel & 0x7Fu));
        smk_padev_w++;
    }
    return 1;
}
static int32_t smk_padev_take(void)
{
    int32_t e;
    if (smk_padev_r == smk_padev_w)
        return -1;
    e = smk_padev[smk_padev_r % SMK_PADEV_N];
    smk_padev_r++;
    return e;
}
static void smk__padq(uint32_t pad, uint32_t vel)
{
    int32_t n0 = smk_pad_piano ? SMK_PIANO_NOTE[pad] : (int32_t)SMK_WHITE_NOTE[SMK_WHITE_OF_PAD(pad)];
    uint32_t n, bit;
    if (!vel && smk_pad_note[pad])
        ;
    else if (smk__pad_raw(pad, vel))
        return;
    if (vel) {
        if (n0 < 0)
            return;
        smk_pad_note[pad] = (uint8_t)(n0 + 1);
    } else {
        if (!smk_pad_note[pad])
            return;
        n0 = smk_pad_note[pad] - 1;
        smk_pad_note[pad] = 0;
    }
    n = (uint32_t)n0;
    bit = 1u << n;
    if (vel) {
        fm1_in.key_vel[SMK_KEY_F3 + n] = (uint8_t)vel;
        fm1_in.pnotes |= bit;
        fm1_in.notes_pressed |= bit;
    } else {
        fm1_in.pnotes &= ~bit;
    }
    fm1_in.notes = fm1_in.knotes | fm1_in.pnotes;
}
static uint32_t fm1_input_edges(uint32_t *released)
{
    uint32_t p = fm1_in.pressed;
    if (released)
        *released = fm1_in.released;
    fm1_in.pressed = fm1_in.released = 0;
    return p;
}
static uint32_t fm1_input_note_edges(void)
{
    uint32_t p = fm1_in.notes_pressed;
    fm1_in.notes_pressed = 0;
    return p;
}
static int32_t fm1_enc_take(uint32_t e)
{
    int32_t s = fm1_in.enc_steps[e % FM1_NENC];
    fm1_in.enc_steps[e % FM1_NENC] = 0;
    return s;
}
static void fm1_led_key(uint32_t id, int on)
{
    if (id < SMK_NLED)
        fm1_led[id] = (uint8_t)(on != 0);
}
static void fm1_input_init(void) {}
static void fm1_input_tick(void) {}
static void fm1_input_scan(void) {}
static int smk_trim_load(const uint8_t *rec) { (void)rec; return 0; }
static void fm1_led_dim_level(uint32_t lo) { (void)lo; }
static void fm1_led_anim_start(uint32_t glow) { (void)glow; }
static int fm1_led_anim_on(void) { return 0; }
static void fm1_led_idle_start(uint32_t glow) { (void)glow; }
static int fm1_led_idle_on(void) { return 0; }
static void fm1_led_anim_stop(void) {}

static uint8_t nor[0x100000];            /* the SPI NOR (erased: 0xFF) */
#define SMP_USER_XIP(k) ((const uint8_t *)nor + SMP_USER_BASE + (k) * SMP_USER_SIZE)   /* eng_sample.c */
#define main hostsim_main
#include "../../tests/hostsim.c"
#undef main

/* ------------------------------------------------------------ the clock --- */
#define FM1_TICKS_PER_US 24u
static uint32_t web_ticks;
static uint32_t fm1_ticks(void) { return web_ticks; }

/* ------------------------------------------------------ audio (ALNK0) --- */
static uint32_t web_half;
#define FM1_AUDIO_HALF 1u
static uint8_t fm1_audio_pending(void) { return FM1_AUDIO_HALF; }
static void fm1_audio_ack_aux(uint8_t p) { (void)p; }
static uint32_t fm1_audio_free_half(void) { return web_half; }
static void fm1_audio_ack_half(void) {}
static void fm1_audio_init(int32_t *buf, uint32_t words, void (*isr)(void), uint32_t prio)
{ (void)buf; (void)words; (void)isr; (void)prio; }
static void fm1_audio_stop(void) {}
void isr_alnk0(void) {}
#include "../../firmware/src/audio.c"

static void fm1_wdt_feed(void) {}
static void fm1_irq_off(void) {}
static void fm1_irq_on(void) {}

/* ----------------------------------------------------------- the screen --- */
static uint16_t web_fb[240 * 240];       /* RGB565 as the panel gets it (high byte first) */
static uint32_t web_draws;
static void lcd_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t c)
{
    uint32_t i, j;
    uint16_t sw = (uint16_t)((c >> 8) | (c << 8));
    for (j = 0; j < h && y + j < 240u; j++)
        for (i = 0; i < w && x + i < 240u; i++)
            web_fb[(y + j) * 240u + x + i] = sw;
    web_draws++;
}
static void lcd_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *p)
{
    uint32_t i, j;
    for (j = 0; j < h && y + j < 240u; j++)
        for (i = 0; i < w && x + i < 240u; i++)
            web_fb[(y + j) * 240u + x + i] = p[j * w + i];
    web_draws++;
}
static void lcd_sync(void) {}
static void lcd_power(uint32_t s)
{
    if (!s) {
        memset(web_fb, 0, sizeof web_fb);
        web_draws++;
    }
}
static void lcd_wake_now(void) {}
static void lcd_init(void) {}
/* the pads' colours (lcd.c): read back by the page */
#define PADLED_N 16u
static uint32_t padled_rgb[PADLED_N];          /* g << 16 | r << 8 | b, as lcd.c */
static uint32_t padled_hold_until;
static void padled_set(uint32_t pad, uint8_t r, uint8_t g, uint8_t b)
{
    if (pad < PADLED_N)
        padled_rgb[pad] = (uint32_t)g << 16 | (uint32_t)r << 8 | b;
}
static void padled_flush(void) {}

#include "../../firmware/src/gfx.c"
#include "../../firmware/src/panel.c"
#include "../../firmware/src/ui.c"
#include "../../firmware/src/menu_items.c"
#include "../../firmware/src/icons.c"
#include "../../firmware/src/ui_graph.c"
#include "../../firmware/src/ui_draw.c"
#include "../../firmware/src/ui_menu.c"
#include "../../firmware/src/ui_input.c"
#include "../../firmware/src/ui_layer.c"

/* ------------------------------------------------------------ the flash --- */
#include "../../firmware/hal/fm1_flash_map.h"   /* the SMK-37's map (FL_GUARD, FL_DATA, the stock marker) */
static uint32_t web_flash_writes, web_flash_erases;
static uint8_t flash_ok;
static int st_read(uint32_t off, void *dst, uint32_t n)
{
    if (off >= sizeof nor || n > sizeof nor - off)
        return -1;
    memcpy(dst, nor + off, n);
    return 0;
}
static int st_erase(uint32_t off)
{
    if (!FL_GUARD(off, 4096u))                   /* the SMK map's allow-list, as the driver */
        return -8;
    memset(nor + (off & ~4095u), 0xFF, 4096);
    web_flash_writes++;
    web_flash_erases++;
    return 0;
}
static int st_prog(uint32_t off, const void *src, uint32_t n)       /* NOR: a write only clears bits */
{
    const uint8_t *s = src;
    uint32_t i;
    if (!FL_GUARD(off, n))
        return -8;
    for (i = 0; i < n; i++)
        nor[off + i] &= s[i];
    web_flash_writes++;
    return 0;
}
static uint32_t irq_save(void) { return 0; }
static void irq_restore(uint32_t f) { (void)f; }
static uint32_t fl_jedec_ram(void) { return 0xEB6014u; }            /* the SMK-37 Pro's part */
static void fl_plain_window_init(void) {}
#define FL_FAR(fn) (fn)
#include "../../firmware/src/storage.c"
#include "../../firmware/src/upreset.c"
#include "../../firmware/src/project.c"
#include "../../firmware/src/worship.c"

/* ------------------------------------------------- main.c and its HAL --- */
#if FELUCCA_CPU2
typedef struct { uint32_t magic, count, vec, pc, emu, dbg, rets, core; } fm1_crash_t;
#else
typedef struct { uint32_t magic, count, vec, pc, emu, dbg, rets; } fm1_crash_t;
#endif
#define FM1_CRASH_MAGIC 0x43525348u
static fm1_crash_t fm1_crash;
static struct { uint8_t p3_rst, wdt_con; uint32_t rst_src; } fm1_boot;
uint32_t _data_start[1], _data_end[1], _data_load[1], _bss_start[1], _bss_end[1];
uint32_t _pool_start[1], _pool_end[1], _rt_start[1], _rt_end[1], _rt_load[1];
/* the FM-1's MASTER pot is K1 here (hal/smk_input.h): a virtual pot */
enum { FM1_ADC_BATT = 3, FM1_ADC_MASTER = 4 };
static int32_t smk_master = 724;
static int32_t fm1_adc_read(uint32_t ch)
{
    if (ch == FM1_ADC_BATT)
        return 620;
    if (ch == FM1_ADC_MASTER) {
        smk_master += fm1_enc_take(0) * 24;
        smk_master = smk_master < 0 ? 0 : smk_master > 1023 ? 1023 : smk_master;
        return smk_master;
    }
    return -1;
}
static void fm1_adc_init(void) {}
static void fm1_timer5_ack(void) {}
static void fm1_timer5_start(void (*isr)(void), uint32_t prio) { (void)isr; (void)prio; }
void isr_timer5(void) {}
static void fm1_reboot(void) {}
static void fm1_enter_uboot(void) {}
static void fm1_guard_lock_top(void) {}
static void fm1_irq_enable_all(void) {}
static void fm1_irq_init(void) {}
static void fm1_time_init(void) {}
static void fm1_reset_reason(void) {}
static void fm1_wdt_arm(uint32_t v) { (void)v; }
static void fm1_mailbox_clear(void) {}
static void fm1_guard_enable(uint32_t m) { (void)m; }
enum { FM1_GUARD_STACK = 1, FM1_GUARD_WRITE = 2, FM1_GUARD_BUS = 4, FM1_GUARD_PC = 8 };
#if FELUCCA_CPU2
uint32_t _c1_start[1], _c1_end[1], _c1_load[1];
static uint8_t fm1_c1_on, fm1_c1_gave_up;
static void fm1_cpu1_halt(void) {}
static int fm1_cpu1_start(void) { return -1; }
static void fm1_guard_pc_open(void) {}
#endif
#if FELUCCA_CDC
static void cdc_task(void) {}
#endif
#define usb_poll() ((void)0)
#define usb_start() ((void)0)
#define usb_retry(ms) ((void)(ms))
#define usb_detach() ((void)0)
#include "../../firmware/src/master.c"
#include "../../firmware/src/main.c"
#undef usb_poll
#undef usb_start
#undef usb_retry
#undef usb_detach

/* --------------------------------------------------------- the device --- */
static uint32_t web_booted, web_boot_ms, web_last_frame;

static void web_power_on(void)
{
    persist_boot();
    settings_init();
    usb_serial_apply();
    lcd_init();
    draw_splash();
    memset(&felucca_dbg, 0, sizeof felucca_dbg);
    felucca_dbg.magic = DBG_MAGIC;
    fm1_input_init();
    fm1_adc_init();
    panel_init();
    felucca_init();
    master_boot();
    kb_boot_hold = 1;
    audio_init();
    boot_leds();
    autosave_boot(boot_clean);
#if FELUCCA_WORSHIP
    wor_boot();                           /* main.c: WORSHIP on at power-on */
#endif
    web_boot_ms = fm1_ms + 430u;
    web_booted = 1;
}

/* one pass of main.c's loop */
static void web_frame(void)
{
    master_poll();
    felucca_dbg.ui_frames++;
    ui_input();
    settings_poll();
    autosave_poll();
    smk_grid();
    ui_leds();
    smk_faders();
    smk_wheels();
    ui_draw();
    fm1_in.frames += 9;                   /* (the scan's 1.6 ms frames over a 15 ms pass) */
}

#define RING 4096u
static float ring[RING * 2];
static uint32_t ring_w, ring_r, web_samples_due;

static void web_ms(void)
{
    fm1_ms++;
    web_ticks += 1000u * FM1_TICKS_PER_US;
    web_samples_due += FS;
    while (web_samples_due >= 1000u * HALF_FRAMES) {
        uint32_t k;
        const int32_t *o = &abuf[web_half * HALF_WORDS];
        web_samples_due -= 1000u * HALF_FRAMES;
        fm1_alnk0_irq();
        for (k = 0; k < HALF_FRAMES; k++) {
            uint32_t i = (ring_w++ % RING) * 2u;
            ring[i] = (float)o[2u * k] / 8388608.0f;
            ring[i + 1] = (float)o[2u * k + 1u] / 8388608.0f;
        }
        web_half ^= 1u;
    }
    if (!web_booted || (int32_t)(fm1_ms - web_boot_ms) < 0)
        return;
    if (web_boot_ms) {
        web_boot_ms = 0;
        kb_boot_hold = 0;
        lcd_fill(0, 0, 240, 240, T_BG);
        ui.force = 1;
        web_last_frame = fm1_ms - 15u;
    }
    if (fm1_ms - web_last_frame >= 15u) {
        web_last_frame = fm1_ms;
        web_frame();
    } else {
        ui_input();
    }
}

#define EXPORT __attribute__((used, visibility("default")))
#undef __attribute__

static float out_l[1024], out_r[1024];
EXPORT float *web_out_l(void) { return out_l; }
EXPORT float *web_out_r(void) { return out_r; }
EXPORT void web_render(uint32_t n)
{
    uint32_t k;
    if (n > 1024u)
        n = 1024u;
    while (ring_w - ring_r < n)
        web_ms();
    for (k = 0; k < n; k++) {
        uint32_t i = (ring_r++ % RING) * 2u;
        out_l[k] = ring[i];
        out_r[k] = ring[i + 1];
    }
}

EXPORT uint8_t *web_nor(void) { return nor; }
EXPORT uint32_t web_nor_size(void) { return sizeof nor; }
EXPORT void web_nor_erase(void) { memset(nor, 0xFF, sizeof nor); }
EXPORT uint32_t web_flash_writes_count(void) { return web_flash_writes; }
EXPORT void web_boot(void)
{
    if (!web_booted)
        web_power_on();
}

/* input, by the SMK's own ids (hal/smk_input.h): buttons bit i = button i (0 KNOB BANK, 1 FADER BANK, 2 <, 3 >,
 * 4 OCT-, 5 OCT+, 6 ARP, 7 NOTE REPEAT, 8 SCALE, 9 CHORD, 10 GLOBE, 11 BT, 12 PATCH, 13 PARA, 14 FX, 15 SEQ,
 * 16 SEQ PLAY, 17 SEQ REC); keybed key 0..36 (C3 .. C6) with a velocity (0 = up); pad 0..15 (0..7 the top row);
 * knob 0..7 (K1..K8) by detents; fader 0..3 (0..127); wheels: pitch 0..1023 (512 rest), mod 0..1023 */
EXPORT void web_buttons(uint32_t m)
{
    uint32_t old = fm1_in.buttons;
    m &= (1u << FM1_NBTN) - 1u;
    fm1_in.pressed |= m & ~old;
    fm1_in.released |= old & ~m;
    fm1_in.buttons = m;
}
EXPORT void web_key(uint32_t k, uint32_t vel)
{
    if (k < FM1_NKEY)
        smk__key_event(k, vel > 127u ? 127u : vel);
}
EXPORT void web_pad(uint32_t p, uint32_t vel) { if (p < SMK_NPAD) smk__padq(p, vel > 127u ? 127u : vel); }
EXPORT void web_knob(uint32_t k, int32_t n) { if (k < FM1_NENC) fm1_in.enc_steps[k] = (int16_t)(fm1_in.enc_steps[k] + n); }
EXPORT void web_fader(uint32_t f, uint32_t v) { if (f < 4u) fm1_in.fader[f] = (uint8_t)(v > 127u ? 127u : v); }
EXPORT void web_wheels(uint32_t pitch, uint32_t mod) { fm1_in.wheel[0] = (uint16_t)pitch; fm1_in.wheel[1] = (uint16_t)mod; }
EXPORT uint32_t web_midi(uint32_t status, uint32_t d1, uint32_t d2)
{
    if (status < 0x80u || status >= 0xF0u)
        return 1;
    return (uint32_t)midi_enqueue((status >> 4) | status << 8 | (d1 & 127u) << 16 | (d2 & 127u) << 24, 1);
}
EXPORT uint32_t web_lit_buttons(void)
{
    uint32_t i, m = 0;
    for (i = 0; i < FM1_NBTN; i++)
        m |= (uint32_t)(fm1_led[i] != 0) << i;
    return m;
}
EXPORT uint32_t *web_pad_colours(void) { return padled_rgb; }
EXPORT uint16_t *web_screen(void) { return web_fb; }
EXPORT uint32_t web_screen_draws(void) { return web_draws; }
EXPORT uint32_t web_now_ms(void) { return fm1_ms; }
EXPORT uint32_t web_sample_rate(void) { return FS; }
EXPORT uint32_t web_worship(void) { return wor_on; }
