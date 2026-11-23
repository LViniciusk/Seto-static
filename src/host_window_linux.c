/* Linux SDL2 host window, OpenGL/software presenter host, keyboard/gamepad input,
 * and PSP sceUtilityOsk modal overlay (parallel to Win32 src/host_window.c).
 */
#ifndef _WIN32

#include <psprecomp/cpu.h>
#include <psprecomp/hle.h>
#include <psprecomp/mem.h>

#include "gl_present.h"
#include "perf_mon.h"
#include "seto_os.h"

#include <SDL2/SDL.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCREEN_W 480
#define SCREEN_H 272
#define WINDOW_W 960
#define WINDOW_H 544
#define TIMER_MS 16

static SDL_Window *g_win;
static SDL_Renderer *g_sw_ren;
static SDL_Texture *g_sw_tex;
static seto_thread_t g_win_thread;
static seto_event_t g_win_ready;
static int g_win_ok;
static atomic_int g_win_stop_req;

static uint32_t *g_read;
static uint32_t *g_sw_px;
static int g_have_frame;
static char g_fps_text[64] = "FPS --.- | VPS --.-";
static int g_hud_visible = -1;

/* Snapshot updated on the SDL event thread, read by host.c input_thread. */
static atomic_uint g_pad_buttons;
static atomic_int g_pad_ax = 128;
static atomic_int g_pad_ay = 128;
static atomic_int g_pad_key_t;
static SDL_GameController *g_pad_ctrl;

static int hud_is_visible(void) {
    if (g_hud_visible < 0) {
        const char *hud = getenv("SETO_GL_HUD");
        g_hud_visible = (!hud || !hud[0] || hud[0] != '0') ? 1 : 0;
    }
    return g_hud_visible;
}

/* Compact 5x7 ASCII bitmap font for the FPS HUD & Linux OSK overlay. */
static const uint8_t FONT5X7[96][5] = {
    {0x00,0x00,0x00,0x00,0x00}, /*   */ {0x00,0x00,0x5F,0x00,0x00}, /* ! */
    {0x00,0x07,0x00,0x07,0x00}, /* " */ {0x14,0x7F,0x14,0x7F,0x14}, /* # */
    {0x24,0x2A,0x7F,0x2A,0x12}, /* $ */ {0x23,0x13,0x08,0x64,0x62}, /* % */
    {0x36,0x49,0x55,0x22,0x50}, /* & */ {0x00,0x05,0x03,0x00,0x00}, /* ' */
    {0x00,0x1C,0x22,0x41,0x00}, /* ( */ {0x00,0x41,0x22,0x1C,0x00}, /* ) */
    {0x14,0x08,0x3E,0x08,0x14}, /* * */ {0x08,0x08,0x3E,0x08,0x08}, /* + */
    {0x00,0x50,0x30,0x00,0x00}, /* , */ {0x08,0x08,0x08,0x08,0x08}, /* - */
    {0x00,0x60,0x60,0x00,0x00}, /* . */ {0x20,0x10,0x08,0x04,0x02}, /* / */
    {0x3E,0x51,0x49,0x45,0x3E}, /* 0 */ {0x00,0x42,0x7F,0x40,0x00}, /* 1 */
    {0x42,0x61,0x51,0x49,0x46}, /* 2 */ {0x21,0x41,0x45,0x4B,0x31}, /* 3 */
    {0x18,0x14,0x12,0x7F,0x10}, /* 4 */ {0x27,0x45,0x45,0x45,0x39}, /* 5 */
    {0x3C,0x4A,0x49,0x49,0x30}, /* 6 */ {0x01,0x71,0x09,0x05,0x03}, /* 7 */
    {0x36,0x49,0x49,0x49,0x36}, /* 8 */ {0x06,0x49,0x49,0x29,0x1E}, /* 9 */
    {0x00,0x36,0x36,0x00,0x00}, /* : */ {0x00,0x56,0x36,0x00,0x00}, /* ; */
    {0x08,0x14,0x22,0x41,0x00}, /* < */ {0x14,0x14,0x14,0x14,0x14}, /* = */
    {0x00,0x41,0x22,0x14,0x08}, /* > */ {0x02,0x01,0x51,0x09,0x06}, /* ? */
    {0x32,0x49,0x79,0x41,0x3E}, /* @ */ {0x7E,0x11,0x11,0x11,0x7E}, /* A */
    {0x7F,0x49,0x49,0x49,0x36}, /* B */ {0x3E,0x41,0x41,0x41,0x22}, /* C */
    {0x7F,0x41,0x41,0x22,0x1C}, /* D */ {0x7F,0x49,0x49,0x49,0x41}, /* E */
    {0x7F,0x09,0x09,0x09,0x01}, /* F */ {0x3E,0x41,0x49,0x49,0x7A}, /* G */
    {0x7F,0x08,0x08,0x08,0x7F}, /* H */ {0x00,0x41,0x7F,0x41,0x00}, /* I */
    {0x20,0x40,0x41,0x3F,0x01}, /* J */ {0x7F,0x08,0x14,0x22,0x41}, /* K */
    {0x7F,0x40,0x40,0x40,0x40}, /* L */ {0x7F,0x02,0x0C,0x02,0x7F}, /* M */
    {0x7F,0x04,0x08,0x10,0x7F}, /* N */ {0x3E,0x41,0x41,0x41,0x3E}, /* O */
    {0x7F,0x09,0x09,0x09,0x06}, /* P */ {0x3E,0x41,0x51,0x21,0x5E}, /* Q */
    {0x7F,0x09,0x19,0x29,0x46}, /* R */ {0x46,0x49,0x49,0x49,0x31}, /* S */
    {0x01,0x01,0x7F,0x01,0x01}, /* T */ {0x3F,0x40,0x40,0x40,0x3F}, /* U */
    {0x1F,0x20,0x40,0x20,0x1F}, /* V */ {0x3F,0x40,0x38,0x40,0x3F}, /* W */
    {0x63,0x14,0x08,0x14,0x63}, /* X */ {0x07,0x08,0x70,0x08,0x07}, /* Y */
    {0x61,0x51,0x49,0x45,0x43}, /* Z */ {0x00,0x7F,0x41,0x41,0x00}, /* [ */
    {0x02,0x04,0x08,0x10,0x20}, /* \ */ {0x00,0x41,0x41,0x7F,0x00}, /* ] */
    {0x04,0x02,0x01,0x02,0x04}, /* ^ */ {0x40,0x40,0x40,0x40,0x40}, /* _ */
    {0x00,0x01,0x02,0x04,0x00}, /* ` */ {0x20,0x54,0x54,0x54,0x78}, /* a */
    {0x7F,0x48,0x44,0x44,0x38}, /* b */ {0x38,0x44,0x44,0x44,0x20}, /* c */
    {0x38,0x44,0x44,0x48,0x7F}, /* d */ {0x38,0x54,0x54,0x54,0x18}, /* e */
    {0x08,0x7E,0x09,0x01,0x02}, /* f */ {0x0C,0x52,0x52,0x52,0x3E}, /* g */
    {0x7F,0x08,0x04,0x04,0x78}, /* h */ {0x00,0x44,0x7D,0x40,0x00}, /* i */
    {0x20,0x40,0x44,0x3D,0x00}, /* j */ {0x7F,0x10,0x28,0x44,0x00}, /* k */
    {0x00,0x41,0x7F,0x40,0x00}, /* l */ {0x7C,0x04,0x18,0x04,0x78}, /* m */
    {0x7C,0x08,0x04,0x04,0x78}, /* n */ {0x38,0x44,0x44,0x44,0x38}, /* o */
    {0x7C,0x14,0x14,0x14,0x08}, /* p */ {0x08,0x14,0x14,0x18,0x7C}, /* q */
    {0x7C,0x08,0x04,0x04,0x08}, /* r */ {0x48,0x54,0x54,0x54,0x20}, /* s */
    {0x04,0x3F,0x44,0x40,0x20}, /* t */ {0x3C,0x40,0x40,0x20,0x7C}, /* u */
    {0x1C,0x20,0x40,0x20,0x1C}, /* v */ {0x3C,0x40,0x30,0x40,0x3C}, /* w */
    {0x44,0x28,0x10,0x28,0x44}, /* x */ {0x0C,0x50,0x50,0x50,0x3C}, /* y */
    {0x44,0x64,0x54,0x4C,0x44}, /* z */ {0x00,0x08,0x36,0x41,0x00}, /* { */
    {0x00,0x00,0x7F,0x00,0x00}, /* | */ {0x00,0x41,0x36,0x08,0x00}, /* } */
    {0x10,0x08,0x08,0x10,0x08}, /* ~ */ {0x00,0x00,0x00,0x00,0x00}
};

static void draw_rect(uint32_t *buf, int bw, int bh, int x, int y, int w, int h, uint32_t abgr) {
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > bw ? bw : x + w, y1 = y + h > bh ? bh : y + h;
    for (int py = y0; py < y1; py++) {
        uint32_t *row = buf + (size_t)py * (size_t)bw;
        for (int px = x0; px < x1; px++) row[px] = abgr;
    }
}

static void draw_frame(uint32_t *buf, int bw, int bh, int x, int y, int w, int h, uint32_t abgr) {
    draw_rect(buf, bw, bh, x, y, w, 1, abgr);
    draw_rect(buf, bw, bh, x, y + h - 1, w, 1, abgr);
    draw_rect(buf, bw, bh, x, y, 1, h, abgr);
    draw_rect(buf, bw, bh, x + w - 1, y, 1, h, abgr);
}

static void draw_text_ascii(uint32_t *buf, int bw, int bh, int x, int y, int scale,
                            uint32_t abgr, const char *s) {
    if (!s || scale <= 0) return;
    int cx = x;
    for (; *s; s++) {
        unsigned char uc = (unsigned char)*s;
        if (uc < 32 || uc > 127) uc = '?';
        const uint8_t *cols = FONT5X7[uc - 32];
        for (int col = 0; col < 5; col++) {
            uint8_t bits = cols[col];
            for (int row = 0; row < 7; row++) {
                if (bits & (1u << row)) {
                    draw_rect(buf, bw, bh, cx + col * scale, y + row * scale,
                              scale, scale, abgr);
                }
            }
        }
        cx += 6 * scale;
    }
}

typedef struct {
    int inited;
    int active;
    int done;
    int cancelled;
    int status;
    uint32_t params_addr;
    uint32_t data_addr;
    uint32_t out_addr;
    int max_chars;
    int len;
    uint64_t open_tick;
    uint64_t close_tick;
    uint16_t prompt[64];
    uint16_t initial[32];
    uint16_t text[32];
} seto_osk_state;

static seto_mutex_t g_osk_cs;
static seto_osk_state g_osk;

static void osk_read_wstr(uint32_t addr, uint16_t *dst, int cap, int replace_nl) {
    int i = 0;
    if (cap <= 0) return;
    if (addr) {
        while (i + 1 < cap) {
            uint16_t ch = psp_read16(addr + (uint32_t)(i * 2));
            if (!ch) break;
            if (replace_nl && (ch == '\r' || ch == '\n')) ch = ' ';
            dst[i++] = ch;
        }
    }
    dst[i] = 0;
}

static size_t osk_wlen(const uint16_t *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

static void osk_wcpy(uint16_t *dst, const uint16_t *src, size_t cap) {
    size_t i = 0;
    while (i + 1 < cap && src[i]) {
        dst[i] = src[i];
        i++;
    }
    if (cap > 0) dst[i] = 0;
}

static void osk_wset_ascii(uint16_t *dst, size_t cap, const char *src) {
    size_t i = 0;
    while (i + 1 < cap && src && src[i]) {
        dst[i] = (uint16_t)(unsigned char)src[i];
        i++;
    }
    if (cap > 0) dst[i] = 0;
}

static void osk_wto_ascii(const uint16_t *src, char *dst, size_t cap) {
    size_t i = 0;
    while (i + 1 < cap && src[i]) {
        uint16_t c = src[i];
        dst[i] = (c >= 32 && c < 127) ? (char)c : '?';
        i++;
    }
    if (cap > 0) dst[i] = '\0';
}

static void osk_write_guest_result(uint32_t data_addr, uint32_t out_addr,
                                   const uint16_t *str, int result_code) {
    if (out_addr) {
        int i = 0;
        for (; str[i] && i < 31; i++)
            psp_write16(out_addr + (uint32_t)(i * 2), str[i]);
        psp_write16(out_addr + (uint32_t)(i * 2), 0);
    }
    if (data_addr)
        psp_write32(data_addr + 0x2Cu, (uint32_t)result_code);
}

static void hle_osk_init_start(void) {
    uint32_t params = psp_arg(0);
    if (!g_osk.inited) {
        seto_mutex_init(&g_osk_cs);
        g_osk.inited = 1;
    }
    if (!params) {
        psp_ret(0x80110001u);
        return;
    }
    psp_write32(params + 0x1Cu, 0);
    uint32_t datacount = psp_read32(params + 0x30u);
    uint32_t data = psp_read32(params + 0x34u);
    if (datacount == 0 || data == 0) {
        seto_mutex_lock(&g_osk_cs);
        g_osk.active = 0;
        g_osk.done = 1;
        g_osk.status = 3;
        g_osk.params_addr = params;
        g_osk.data_addr = 0;
        g_osk.out_addr = 0;
        seto_mutex_unlock(&g_osk_cs);
        psp_ret(0);
        return;
    }

    uint32_t desc_addr = psp_read32(data + 0x1Cu);
    uint32_t intext_addr = psp_read32(data + 0x20u);
    int outlen = (int)psp_read32(data + 0x24u);
    uint32_t out_addr = psp_read32(data + 0x28u);
    int outlimit = (int)psp_read32(data + 0x30u);
    int cap = outlen > 1 ? outlen - 1 : 8;
    if (outlimit > 0 && outlimit < cap) cap = outlimit;
    if (cap > 31) cap = 31;

    uint16_t prompt[64], initial[32];
    osk_read_wstr(desc_addr, prompt, 64, 1);
    if (!prompt[0]) osk_wset_ascii(prompt, 64, "Enter Name");
    osk_read_wstr(intext_addr, initial, cap + 1, 1);

    const char *env_name = getenv("SETO_PLAYER_NAME");
    const int auto_mode = getenv("SETO_AUTO_START") != NULL;
    const int headless = (g_win == NULL);

    if (auto_mode || headless) {
        uint16_t auto_buf[32];
        if (env_name && env_name[0]) {
            osk_wset_ascii(auto_buf, (size_t)(cap + 1), env_name);
        } else if (initial[0]) {
            osk_wcpy(auto_buf, initial, 32);
        } else {
            osk_wset_ascii(auto_buf, 32, "YUGI");
        }
        osk_write_guest_result(data, out_addr, auto_buf, 2);
        seto_mutex_lock(&g_osk_cs);
        g_osk.active = 0;
        g_osk.done = 1;
        g_osk.cancelled = 0;
        g_osk.status = 1;
        g_osk.params_addr = params;
        g_osk.data_addr = data;
        g_osk.out_addr = out_addr;
        g_osk.max_chars = cap;
        osk_wcpy(g_osk.text, auto_buf, 32);
        g_osk.len = (int)osk_wlen(auto_buf);
        seto_mutex_unlock(&g_osk_cs);
        psp_ret(0);
        return;
    }

    seto_mutex_lock(&g_osk_cs);
    g_osk.active = 1;
    g_osk.done = 0;
    g_osk.cancelled = 0;
    g_osk.status = 1;
    g_osk.params_addr = params;
    g_osk.data_addr = data;
    g_osk.out_addr = out_addr;
    g_osk.max_chars = cap;
    g_osk.open_tick = seto_ticks_ms();
    osk_wcpy(g_osk.prompt, prompt, 64);
    osk_wcpy(g_osk.initial, initial, 32);
    osk_wcpy(g_osk.text, initial, 32);
    g_osk.len = (int)osk_wlen(g_osk.text);
    seto_mutex_unlock(&g_osk_cs);
    psp_ret(0);
}

static void hle_osk_get_status(void) {
    if (!g_osk.inited) { psp_ret(0); return; }
    int commit = 0, cancelled = 0;
    uint32_t data_addr = 0, out_addr = 0;
    uint16_t buf[32];

    seto_mutex_lock(&g_osk_cs);
    int r = g_osk.status;
    if (g_osk.status == 1) {
        g_osk.status = 2;
    } else if (g_osk.status == 2 && g_osk.done) {
        commit = 1;
        cancelled = g_osk.cancelled;
        data_addr = g_osk.data_addr;
        out_addr = g_osk.out_addr;
        if (cancelled) {
            if (g_osk.initial[0]) osk_wcpy(buf, g_osk.initial, 32);
            else osk_wset_ascii(buf, 32, "YUGI");
        } else {
            osk_wcpy(buf, g_osk.text, 32);
        }
        g_osk.status = 3;
    }
    seto_mutex_unlock(&g_osk_cs);

    if (commit)
        osk_write_guest_result(data_addr, out_addr, buf, cancelled ? 1 : 2);
    psp_ret((uint32_t)r);
}

static void hle_osk_update(void) {
    (void)psp_arg(0);
    psp_ret(0);
}

static void hle_osk_shutdown_start(void) {
    if (g_osk.inited) {
        seto_mutex_lock(&g_osk_cs);
        g_osk.status = 4;
        g_osk.active = 0;
        seto_mutex_unlock(&g_osk_cs);
    }
    psp_ret(0);
}

void seto_osk_register(void) {
    if (!g_osk.inited) {
        seto_mutex_init(&g_osk_cs);
        g_osk.inited = 1;
    }
    psp_hle_register(0xF6269B82u, "sceUtility", "sceUtilityOskInitStart", hle_osk_init_start);
    psp_hle_register(0xF3F76017u, "sceUtility", "sceUtilityOskGetStatus", hle_osk_get_status);
    psp_hle_register(0x4B85C861u, "sceUtility", "sceUtilityOskUpdate", hle_osk_update);
    psp_hle_register(0x3DFAEBA9u, "sceUtility", "sceUtilityOskShutdownStart", hle_osk_shutdown_start);
}

int seto_window_osk_active(void) {
    if (!g_osk.inited) return 0;
    seto_mutex_lock(&g_osk_cs);
    int a = g_osk.active;
    uint64_t ct = g_osk.close_tick;
    seto_mutex_unlock(&g_osk_cs);
    if (a) return 1;
    if (ct && (seto_ticks_ms() - ct) < 300) {
        const uint8_t *ks = SDL_GetKeyboardState(NULL);
        if (ks && (ks[SDL_SCANCODE_RETURN] || ks[SDL_SCANCODE_ESCAPE]))
            return 1;
    }
    return 0;
}

static uint32_t *g_ovl_rgba;
static size_t g_ovl_rgba_cap;
static char g_ovl_last_fps[64];
static int g_ovl_was_osk;
static int g_ovl_active_badge;
static uint64_t g_ovl_last_caret;

static void gl_free_overlay(void) {
    free(g_ovl_rgba);
    g_ovl_rgba = NULL;
    g_ovl_rgba_cap = 0;
    g_ovl_last_fps[0] = '\0';
    g_ovl_was_osk = 0;
    g_ovl_active_badge = 0;
}

static void render_osk_to_rgba(uint32_t *dst, int cw, int ch, const seto_osk_state *snap) {
    memset(dst, 0, (size_t)cw * (size_t)ch * 4u);
    int bw = cw - 40;
    if (bw > 460) bw = 460;
    if (bw < 220) bw = cw > 20 ? cw - 20 : cw;
    int bh = 166;
    if (bh > ch - 20) bh = ch > 20 ? ch - 20 : ch;
    int bx = (cw - bw) / 2;
    int by = (ch - bh) / 2;

    draw_rect(dst, cw, ch, bx + 5, by + 5, bw, bh, 0xEE0E0806u);
    draw_rect(dst, cw, ch, bx, by, bw, bh, 0xEE2A1912u);
    draw_frame(dst, cw, ch, bx, by, bw, bh, 0xFFFFAC60u);

    char prompt_a[64];
    osk_wto_ascii(snap->prompt, prompt_a, sizeof prompt_a);
    draw_text_ascii(dst, cw, ch, bx + 18, by + 18, 2, 0xFFFFEEE1u,
                    prompt_a[0] ? prompt_a : "Enter Name");

    int box_x = bx + 18, box_y = by + 54, box_w = bw - 36, box_h = 48;
    draw_rect(dst, cw, ch, box_x, box_y, box_w, box_h, 0xEE180D09u);
    draw_frame(dst, cw, ch, box_x, box_y, box_w, box_h, 0xFFFFC382u);

    char disp[36];
    osk_wto_ascii(snap->text, disp, sizeof disp);
    size_t dlen = strlen(disp);
    if (((seto_ticks_ms() / 450u) & 1u) == 0u &&
        snap->len < snap->max_chars && dlen + 1 < sizeof disp) {
        disp[dlen++] = '_';
        disp[dlen] = '\0';
    }
    draw_text_ascii(dst, cw, ch, box_x + 12, box_y + 14, 3, 0xFFFFFFFFu, disp);

    char cnt[16];
    snprintf(cnt, sizeof cnt, "%d/%d", snap->len, snap->max_chars);
    draw_text_ascii(dst, cw, ch, box_x + box_w - 58, box_y + 18, 2, 0xFFD2AF96u, cnt);

    draw_text_ascii(dst, cw, ch, bx + 18, by + 122, 1, 0xFFD7C0AFu,
                    "Enter: OK  |  Backspace: Del  |  Del: Clear  |  Esc: Cancel");
}

static void gl_sync_overlay(SDL_Window *win) {
    seto_osk_state snap;
    memset(&snap, 0, sizeof snap);
    int osk_on = 0;
    if (g_osk.inited) {
        seto_mutex_lock(&g_osk_cs);
        snap = g_osk;
        osk_on = g_osk.active;
        seto_mutex_unlock(&g_osk_cs);
    }

    if (osk_on) {
        int cw = 0, ch = 0;
        SDL_GetWindowSize(win, &cw, &ch);
        if (cw <= 0 || ch <= 0) return;
        const size_t npx = (size_t)cw * (size_t)ch;
        if (npx > g_ovl_rgba_cap) {
            uint32_t *p = (uint32_t *)realloc(g_ovl_rgba, npx * 4u);
            if (!p) return;
            g_ovl_rgba = p;
            g_ovl_rgba_cap = npx;
        }
        uint64_t caret = seto_ticks_ms() / 450u;
        if (!g_ovl_was_osk || caret != g_ovl_last_caret || g_ovl_active_badge != 2) {
            g_ovl_last_caret = caret;
            render_osk_to_rgba(g_ovl_rgba, cw, ch, &snap);
            seto_gl_present_set_overlay(g_ovl_rgba, 0, 0, cw, ch);
            g_ovl_was_osk = 1;
            g_ovl_active_badge = 2;
        }
        return;
    }

    const int hud_on = hud_is_visible();
    if (!hud_on || !g_fps_text[0]) {
        if (g_ovl_active_badge || g_ovl_was_osk) {
            seto_gl_present_set_overlay(NULL, 0, 0, 0, 0);
            g_ovl_active_badge = 0;
            g_ovl_was_osk = 0;
            g_ovl_last_fps[0] = '\0';
        }
        return;
    }

    if (strcmp(g_ovl_last_fps, g_fps_text) == 0 && !g_ovl_was_osk)
        return;

    const int bw = 220, bh = 24;
    const size_t npx = (size_t)bw * (size_t)bh;
    if (npx > g_ovl_rgba_cap) {
        uint32_t *p = (uint32_t *)realloc(g_ovl_rgba, npx * 4u);
        if (!p) return;
        g_ovl_rgba = p;
        g_ovl_rgba_cap = npx;
    }
    draw_rect(g_ovl_rgba, bw, bh, 0, 0, bw, bh, 0xD814100Cu);
    draw_text_ascii(g_ovl_rgba, bw, bh, 8, 5, 2, 0xD878FF50u, g_fps_text);
    seto_gl_present_set_overlay(g_ovl_rgba, 8, 6, bw, bh);
    snprintf(g_ovl_last_fps, sizeof g_ovl_last_fps, "%s", g_fps_text);
    g_ovl_was_osk = 0;
    g_ovl_active_badge = 1;
}

static void refresh_pad_snapshot(SDL_Window *win) {
    uint32_t fm = 0;
    int dx = 0, dy = 0;
    const uint32_t flags = win ? SDL_GetWindowFlags(win) : 0;
    const int focused = (flags & SDL_WINDOW_INPUT_FOCUS) != 0;

    if (focused) {
        const uint8_t *ks = SDL_GetKeyboardState(NULL);
        if (ks) {
            if (ks[SDL_SCANCODE_LEFT])      fm |= 0x0080u;
            if (ks[SDL_SCANCODE_RIGHT])     fm |= 0x0020u;
            if (ks[SDL_SCANCODE_UP])        fm |= 0x0010u;
            if (ks[SDL_SCANCODE_DOWN])      fm |= 0x0040u;
            if (ks[SDL_SCANCODE_Z])         fm |= 0x4000u;
            if (ks[SDL_SCANCODE_X])         fm |= 0x2000u;
            if (ks[SDL_SCANCODE_C])         fm |= 0x1000u;
            if (ks[SDL_SCANCODE_V])         fm |= 0x8000u;
            if (ks[SDL_SCANCODE_Q])         fm |= 0x0100u;
            if (ks[SDL_SCANCODE_E])         fm |= 0x0200u;
            if (ks[SDL_SCANCODE_RETURN])    fm |= 0x0008u;
            if (ks[SDL_SCANCODE_BACKSPACE]) fm |= 0x0001u;
            if (ks[SDL_SCANCODE_SPACE])     fm |= 0x0001u;
            dx = (ks[SDL_SCANCODE_D] ? 1 : 0) - (ks[SDL_SCANCODE_A] ? 1 : 0);
            dy = (ks[SDL_SCANCODE_S] ? 1 : 0) - (ks[SDL_SCANCODE_W] ? 1 : 0);
            atomic_store_explicit(&g_pad_key_t, ks[SDL_SCANCODE_T] ? 1 : 0, memory_order_relaxed);
        }
    } else {
        atomic_store_explicit(&g_pad_key_t, 0, memory_order_relaxed);
    }

    uint8_t ax = dx < 0 ? 0u : (dx > 0 ? 255u : 128u);
    uint8_t ay = dy < 0 ? 0u : (dy > 0 ? 255u : 128u);

    if (g_pad_ctrl && SDL_GameControllerGetAttached(g_pad_ctrl)) {
        if (SDL_GameControllerGetButton(g_pad_ctrl, SDL_CONTROLLER_BUTTON_DPAD_LEFT))  fm |= 0x0080u;
        if (SDL_GameControllerGetButton(g_pad_ctrl, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) fm |= 0x0020u;
        if (SDL_GameControllerGetButton(g_pad_ctrl, SDL_CONTROLLER_BUTTON_DPAD_UP))    fm |= 0x0010u;
        if (SDL_GameControllerGetButton(g_pad_ctrl, SDL_CONTROLLER_BUTTON_DPAD_DOWN))  fm |= 0x0040u;
        if (SDL_GameControllerGetButton(g_pad_ctrl, SDL_CONTROLLER_BUTTON_A))          fm |= 0x4000u;
        if (SDL_GameControllerGetButton(g_pad_ctrl, SDL_CONTROLLER_BUTTON_B))          fm |= 0x2000u;
        if (SDL_GameControllerGetButton(g_pad_ctrl, SDL_CONTROLLER_BUTTON_Y))          fm |= 0x1000u;
        if (SDL_GameControllerGetButton(g_pad_ctrl, SDL_CONTROLLER_BUTTON_X))          fm |= 0x8000u;
        if (SDL_GameControllerGetButton(g_pad_ctrl, SDL_CONTROLLER_BUTTON_LEFTSHOULDER))  fm |= 0x0100u;
        if (SDL_GameControllerGetButton(g_pad_ctrl, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) fm |= 0x0200u;
        if (SDL_GameControllerGetButton(g_pad_ctrl, SDL_CONTROLLER_BUTTON_START))      fm |= 0x0008u;
        if (SDL_GameControllerGetButton(g_pad_ctrl, SDL_CONTROLLER_BUTTON_BACK))       fm |= 0x0001u;
        if (dx == 0 && dy == 0) {
            int16_t lx = SDL_GameControllerGetAxis(g_pad_ctrl, SDL_CONTROLLER_AXIS_LEFTX);
            int16_t ly = SDL_GameControllerGetAxis(g_pad_ctrl, SDL_CONTROLLER_AXIS_LEFTY);
            if (lx < -8000 || lx > 8000) ax = (uint8_t)((lx + 32768) >> 8);
            if (ly < -8000 || ly > 8000) ay = (uint8_t)((ly + 32768) >> 8);
        }
    }

    atomic_store_explicit(&g_pad_buttons, fm, memory_order_relaxed);
    atomic_store_explicit(&g_pad_ax, (int)ax, memory_order_relaxed);
    atomic_store_explicit(&g_pad_ay, (int)ay, memory_order_relaxed);
}

uint32_t seto_linux_poll_pad(uint8_t *out_ax, uint8_t *out_ay) {
    if (out_ax) *out_ax = (uint8_t)atomic_load_explicit(&g_pad_ax, memory_order_relaxed);
    if (out_ay) *out_ay = (uint8_t)atomic_load_explicit(&g_pad_ay, memory_order_relaxed);
    return atomic_load_explicit(&g_pad_buttons, memory_order_relaxed);
}

int seto_linux_key_t_down(void) {
    return atomic_load_explicit(&g_pad_key_t, memory_order_relaxed);
}

static void update_frame(SDL_Window *win) {
    const uint32_t flags = SDL_GetWindowFlags(win);
    if (flags & SDL_WINDOW_MINIMIZED) return;

    const int gl_on = seto_gl_present_active();
    const int gpu_ready = gl_on && seto_gl_present_has_gpu_frame();
    if (!gpu_ready && psp_display_read_present(g_read) != 0)
        return;

    {
        static uint64_t last_ms;
        static uint64_t last_frames, last_vblanks;
        uint64_t now = seto_ticks_ms();
        if (!last_ms) {
            last_ms = now;
            last_frames = psp_display_frames();
            last_vblanks = psp_display_vblanks();
        } else if (now - last_ms >= 500u) {
            uint64_t cur_frames = psp_display_frames();
            uint64_t cur_vblanks = psp_display_vblanks();
            double dt = (double)(now - last_ms) / 1000.0;
            double fps = (double)(cur_frames - last_frames) / dt;
            double vps = (double)(cur_vblanks - last_vblanks) / dt;
            snprintf(g_fps_text, sizeof g_fps_text, "FPS %.1f | VPS %.1f", fps, vps);
            seto_perf_tick(fps, vps, seto_ge_gpu_current_scale(), seto_ge_gpu_current_msaa());
            if (hud_is_visible()) {
                char title[96];
                snprintf(title, sizeof title, "Seto-static | %s", g_fps_text);
                SDL_SetWindowTitle(win, title);
            } else {
                SDL_SetWindowTitle(win, "Seto-static \xE2\x80\x94 Yu-Gi-Oh! GX Tag Force");
            }
            last_ms = now;
            last_frames = cur_frames;
            last_vblanks = cur_vblanks;
        }
    }

    if (gl_on) {
        gl_sync_overlay(win);
        seto_gl_present_frame(win, gpu_ready ? NULL : g_read);
    } else if (g_sw_ren && g_sw_tex) {
        g_have_frame = 1;
        for (int i = 0; i < SCREEN_W * SCREEN_H; i++)
            g_sw_px[i] = g_read[i] | 0xFF000000u;
        SDL_UpdateTexture(g_sw_tex, NULL, g_sw_px, SCREEN_W * 4);
        int cw = 0, ch = 0;
        SDL_GetRendererOutputSize(g_sw_ren, &cw, &ch);
        SDL_SetRenderDrawColor(g_sw_ren, 0, 0, 0, 255);
        SDL_RenderClear(g_sw_ren);
        if (cw > 0 && ch > 0 && g_have_frame) {
            int dw, dh;
            if ((int64_t)cw * 17 > (int64_t)ch * 30) {
                dh = ch;
                dw = (int)((int64_t)ch * 30 / 17);
            } else {
                dw = cw;
                dh = (int)((int64_t)cw * 17 / 30);
            }
            SDL_Rect dst = { (cw - dw) / 2, (ch - dh) / 2, dw, dh };
            SDL_RenderCopy(g_sw_ren, g_sw_tex, NULL, &dst);
        }
        SDL_RenderPresent(g_sw_ren);
    }
}

static seto_thread_ret_t SETO_THREAD_CALL window_thread(seto_thread_arg_t unused) {
    (void)unused;

    g_read  = (uint32_t *)malloc((size_t)SCREEN_W * SCREEN_H * sizeof(uint32_t));
    g_sw_px = (uint32_t *)malloc((size_t)SCREEN_W * SCREEN_H * sizeof(uint32_t));
    if (!g_read || !g_sw_px) {
        fprintf(stderr, "window: cannot allocate frame buffers\n");
        free(g_read); free(g_sw_px);
        g_read = g_sw_px = NULL;
        seto_event_set(&g_win_ready);
        return NULL;
    }

    if (SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "window: SDL_InitSubSystem failed (%s)\n", SDL_GetError());
        free(g_read); free(g_sw_px);
        g_read = g_sw_px = NULL;
        seto_event_set(&g_win_ready);
        return NULL;
    }

    int win_w = WINDOW_W, win_h = WINDOW_H;
    const char *scale = getenv("SETO_SCALE");
    if (scale && atoi(scale) > 0) {
        win_w = SCREEN_W * atoi(scale);
        win_h = SCREEN_H * atoi(scale);
    }

    const char *present = getenv("SETO_PRESENT");
    const int want_gl = (present && strcmp(present, "gl") == 0);
    Uint32 win_flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_SHOWN;
    if (want_gl) {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
        SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
        win_flags |= SDL_WINDOW_OPENGL;
    }

    g_win = SDL_CreateWindow("Seto-static \xE2\x80\x94 Yu-Gi-Oh! GX Tag Force",
                             SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                             win_w, win_h, win_flags);
    if (!g_win) {
        fprintf(stderr, "window: SDL_CreateWindow failed (%s)\n", SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER);
        free(g_read); free(g_sw_px);
        g_read = g_sw_px = NULL;
        seto_event_set(&g_win_ready);
        return NULL;
    }

    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        if (SDL_IsGameController(i)) {
            g_pad_ctrl = SDL_GameControllerOpen(i);
            if (g_pad_ctrl) break;
        }
    }

    int gl_ok = want_gl ? seto_gl_present_init(g_win) : 0;
    if (!gl_ok) {
        const char *f = getenv("SETO_FILTER");
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, (f && atoi(f) > 0) ? "1" : "0");
        g_sw_ren = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        if (!g_sw_ren) g_sw_ren = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_SOFTWARE);
        if (g_sw_ren) {
            g_sw_tex = SDL_CreateTexture(g_sw_ren, SDL_PIXELFORMAT_ABGR8888,
                                         SDL_TEXTUREACCESS_STREAMING, SCREEN_W, SCREEN_H);
        }
    }

    g_win_ok = 1;
    seto_event_set(&g_win_ready);

    int osk_text_active = 0;
    int user_closed = 0;
    while (!atomic_load_explicit(&g_win_stop_req, memory_order_acquire) && !psp_exit_requested()) {
        int osk_now = 0;
        if (g_osk.inited) {
            seto_mutex_lock(&g_osk_cs);
            osk_now = g_osk.active;
            seto_mutex_unlock(&g_osk_cs);
        }
        if (osk_now != osk_text_active) {
            osk_text_active = osk_now;
            if (osk_text_active) SDL_StartTextInput();
            else SDL_StopTextInput();
        }

        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) {
                user_closed = 1;
                atomic_store_explicit(&g_win_stop_req, 1, memory_order_release);
                break;
            } else if (ev.type == SDL_CONTROLLERDEVICEADDED && !g_pad_ctrl) {
                if (SDL_IsGameController(ev.cdevice.which))
                    g_pad_ctrl = SDL_GameControllerOpen(ev.cdevice.which);
            } else if (ev.type == SDL_CONTROLLERDEVICEREMOVED && g_pad_ctrl) {
                SDL_Joystick *j = SDL_GameControllerGetJoystick(g_pad_ctrl);
                if (j && SDL_JoystickInstanceID(j) == ev.cdevice.which) {
                    SDL_GameControllerClose(g_pad_ctrl);
                    g_pad_ctrl = NULL;
                }
            } else if (ev.type == SDL_WINDOWEVENT) {
                if (ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                    if (seto_gl_present_active())
                        seto_gl_present_resize(ev.window.data1, ev.window.data2);
                } else if (ev.window.event == SDL_WINDOWEVENT_EXPOSED) {
                    if (seto_gl_present_active()) seto_gl_present_invalidate();
                }
            } else if (ev.type == SDL_KEYDOWN) {
                const SDL_Keycode sym = ev.key.keysym.sym;
                if (g_osk.inited) {
                    int handled = 0;
                    seto_mutex_lock(&g_osk_cs);
                    if (g_osk.active) {
                        if (sym == SDLK_RETURN || sym == SDLK_KP_ENTER) {
                            if (g_osk.len > 0) {
                                g_osk.cancelled = 0;
                                g_osk.done = 1;
                                g_osk.active = 0;
                                g_osk.close_tick = seto_ticks_ms();
                            }
                            handled = 1;
                        } else if (sym == SDLK_ESCAPE) {
                            g_osk.cancelled = 1;
                            g_osk.done = 1;
                            g_osk.active = 0;
                            g_osk.close_tick = seto_ticks_ms();
                            handled = 1;
                        } else if (sym == SDLK_DELETE) {
                            g_osk.len = 0;
                            g_osk.text[0] = 0;
                            handled = 1;
                        } else if (sym == SDLK_BACKSPACE) {
                            if (ev.key.keysym.mod & KMOD_CTRL) {
                                g_osk.len = 0;
                                g_osk.text[0] = 0;
                            } else if (g_osk.len > 0) {
                                g_osk.text[--g_osk.len] = 0;
                            }
                            handled = 1;
                        }
                    }
                    seto_mutex_unlock(&g_osk_cs);
                    if (handled) {
                        g_ovl_was_osk = 0;
                        continue;
                    }
                }
                if (sym == SDLK_F5 && ev.key.repeat == 0) {
                    g_hud_visible = !hud_is_visible();
                    if (g_hud_visible && g_fps_text[0]) {
                        char title[96];
                        snprintf(title, sizeof title, "Seto-static | %s", g_fps_text);
                        SDL_SetWindowTitle(g_win, title);
                    } else {
                        SDL_SetWindowTitle(g_win, "Seto-static \xE2\x80\x94 Yu-Gi-Oh! GX Tag Force");
                    }
                    if (seto_gl_present_active()) gl_sync_overlay(g_win);
                }
            } else if (ev.type == SDL_TEXTINPUT && g_osk.inited) {
                seto_mutex_lock(&g_osk_cs);
                if (g_osk.active && (seto_ticks_ms() - g_osk.open_tick) >= 120) {
                    for (const char *p = ev.text.text; *p && g_osk.len < g_osk.max_chars; p++) {
                        unsigned char ch = (unsigned char)*p;
                        if (ch >= 0x20 && ch < 0x7F) {
                            g_osk.text[g_osk.len++] = (uint16_t)ch;
                            g_osk.text[g_osk.len] = 0;
                        }
                    }
                }
                seto_mutex_unlock(&g_osk_cs);
                g_ovl_was_osk = 0;
            }
        }

        refresh_pad_snapshot(g_win);
        update_frame(g_win);
        seto_sleep_ms(TIMER_MS);
    }

    if (osk_text_active) SDL_StopTextInput();
    gl_free_overlay();
    if (seto_gl_present_active()) seto_gl_present_shutdown(g_win);
    if (g_sw_tex) { SDL_DestroyTexture(g_sw_tex); g_sw_tex = NULL; }
    if (g_sw_ren) { SDL_DestroyRenderer(g_sw_ren); g_sw_ren = NULL; }
    if (g_pad_ctrl) { SDL_GameControllerClose(g_pad_ctrl); g_pad_ctrl = NULL; }
    if (g_win) { SDL_DestroyWindow(g_win); g_win = NULL; }
    SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER);
    free(g_sw_px);
    free(g_read);
    g_sw_px = g_read = NULL;
    if (user_closed) _Exit(0);
    return NULL;
}

int seto_window_start(void) {
    if (getenv("SETO_HEADLESS")) return -1;
    atomic_store_explicit(&g_win_stop_req, 0, memory_order_release);
    g_win_ok = 0;
    if (seto_event_init(&g_win_ready, 1) != 0) return -1;
    if (seto_thread_create(&g_win_thread, window_thread, NULL) != 0) {
        seto_event_destroy(&g_win_ready);
        return -1;
    }
    int got = seto_event_wait_ms(&g_win_ready, 5000);
    seto_event_destroy(&g_win_ready);
    if (!got || !g_win_ok || !g_win) {
        atomic_store_explicit(&g_win_stop_req, 1, memory_order_release);
        seto_thread_join(&g_win_thread, 2000);
        return -1;
    }
    return 0;
}

void seto_window_stop(void) {
    atomic_store_explicit(&g_win_stop_req, 1, memory_order_release);
    seto_thread_join(&g_win_thread, 2000);
}

#endif /* !_WIN32 */
