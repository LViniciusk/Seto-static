/* S9.1: a minimal Win32 window that presents the PSP front buffer.
 *
 * The guest and the GE keep running on their own threads; this thread only
 * copies the framebuffer the runtime already exposes, converts the channel
 * order for a 32-bit BI_RGB DIB and paints it. No renderer, no input logic and
 * no guest state live here. */

#include <psprecomp/hle.h>

#include "gl_present.h"

#include <windows.h>
#include <gdiplus.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCREEN_W 480
#define SCREEN_H 272
#define WINDOW_W 960
#define WINDOW_H 544
#define TIMER_ID 1
#define TIMER_MS 16                 /* ~60 FPS */

static HWND g_hwnd;
static HANDLE g_thread;
static HANDLE g_ready;
static uint32_t *g_read;
static uint32_t *g_dib;
static BITMAPINFO g_bmi;
static int g_have_frame;
static HDC g_memdc;
static HBITMAP g_membmp, g_memold;
static int g_memw, g_memh;
static char g_fps_text[64] = "FPS --.- | VPS --.-";
static ULONG_PTR g_gdiplus_token;
static int g_gdiplus_ready;

static int gdiplus_start_once(void) {
    if (g_gdiplus_ready) return 1;
    GdiplusStartupInput in = {0};
    in.GdiplusVersion = 1;
    if (GdiplusStartup(&g_gdiplus_token, &in, NULL) != Ok) return 0;
    g_gdiplus_ready = 1;
    return 1;
}

static void update_frame(HWND hwnd) {
    if (IsIconic(hwnd)) return;
    if (psp_display_read_present(g_read) != 0) return;
    if (seto_gl_present_active()) {
        seto_gl_present_frame(hwnd, g_read);
    } else {
        /* 0xAABBGGRR from psp_display_read; a 32-bit BI_RGB DIB wants B,G,R,X
         * in memory, i.e. 0x00RRGGBB in the word. */
        for (int i = 0; i < SCREEN_W * SCREEN_H; i++) {
            const uint32_t c = g_read[i];
            g_dib[i] = ((c & 0x000000FFu) << 16) |
                       (c & 0x0000FF00u) |
                       ((c & 0x00FF0000u) >> 16);
        }
        g_have_frame = 1;
    }
    {
        static DWORD last_ms;
        static uint64_t last_frames, last_vblanks;
        DWORD now = GetTickCount();
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
            char title[96];
            snprintf(title, sizeof title, "Seto-static | %s", g_fps_text);
            SetWindowTextA(hwnd, title);
            last_ms = now;
            last_frames = cur_frames;
            last_vblanks = cur_vblanks;
        }
    }
    if (!seto_gl_present_active()) InvalidateRect(hwnd, NULL, FALSE);
}
#include <psprecomp/cpu.h>
#include <psprecomp/hle.h>
#include <psprecomp/mem.h>
#include <stdlib.h>
#include <string.h>

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
    ULONGLONG open_tick;
    ULONGLONG close_tick;
    WCHAR prompt[64];
    WCHAR initial[32];
    WCHAR text[32];
} seto_osk_state;

static CRITICAL_SECTION g_osk_cs;
static seto_osk_state g_osk;

static void osk_read_wstr(uint32_t addr, WCHAR *dst, int cap, int replace_nl) {
    int i = 0;
    if (cap <= 0) return;
    if (addr) {
        while (i + 1 < cap) {
            uint16_t ch = psp_read16(addr + (uint32_t)(i * 2));
            if (!ch) break;
            if (replace_nl && (ch == '\r' || ch == '\n')) ch = ' ';
            dst[i++] = (WCHAR)ch;
        }
    }
    dst[i] = L'\0';
}

static void osk_write_guest_result(uint32_t data_addr, uint32_t out_addr,
                                   const WCHAR *str, int result_code) {
    if (out_addr) {
        int i = 0;
        for (; str[i] && i < 31; i++)
            psp_write16(out_addr + (uint32_t)(i * 2), (uint16_t)str[i]);
        psp_write16(out_addr + (uint32_t)(i * 2), 0);
    }
    if (data_addr)
        psp_write32(data_addr + 0x2Cu, (uint32_t)result_code);
}

static void osk_paint_overlay(HDC memdc, int cw, int ch) {
    seto_osk_state snap;
    if (!g_osk.inited) return;
    EnterCriticalSection(&g_osk_cs);
    snap = g_osk;
    LeaveCriticalSection(&g_osk_cs);
    if (!snap.active) return;

    int bw = cw - 40;
    if (bw > 460) bw = 460;
    if (bw < 220) bw = cw > 20 ? cw - 20 : cw;
    int bh = 166;
    if (bh > ch - 20) bh = ch > 20 ? ch - 20 : ch;
    int bx = (cw - bw) / 2;
    int by = (ch - bh) / 2;

    RECT shadow = { bx + 5, by + 5, bx + bw + 5, by + bh + 5 };
    HBRUSH br_shadow = CreateSolidBrush(RGB(6, 8, 14));
    FillRect(memdc, &shadow, br_shadow);
    DeleteObject(br_shadow);

    RECT panel = { bx, by, bx + bw, by + bh };
    HBRUSH br_panel = CreateSolidBrush(RGB(18, 25, 42));
    FillRect(memdc, &panel, br_panel);
    DeleteObject(br_panel);
    HBRUSH br_border = CreateSolidBrush(RGB(96, 172, 255));
    FrameRect(memdc, &panel, br_border);
    DeleteObject(br_border);

    HFONT f_title = CreateFontW(20, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    HFONT f_input = CreateFontW(26, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                FIXED_PITCH | FF_MODERN, L"Consolas");
    HFONT f_hint = CreateFontW(15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                               CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                               DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

    int old_bk = SetBkMode(memdc, TRANSPARENT);
    HGDIOBJ old_font = SelectObject(memdc, f_title ? f_title : GetStockObject(DEFAULT_GUI_FONT));
    SetTextColor(memdc, RGB(225, 238, 255));
    RECT r_title = { bx + 18, by + 14, bx + bw - 18, by + 44 };
    DrawTextW(memdc, snap.prompt[0] ? snap.prompt : L"Enter Name", -1, &r_title,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

    RECT box = { bx + 18, by + 54, bx + bw - 18, by + 102 };
    HBRUSH br_box = CreateSolidBrush(RGB(9, 13, 24));
    FillRect(memdc, &box, br_box);
    DeleteObject(br_box);
    HBRUSH br_box_fr = CreateSolidBrush(RGB(130, 195, 255));
    FrameRect(memdc, &box, br_box_fr);
    DeleteObject(br_box_fr);

    WCHAR disp[36];
    int caret = ((GetTickCount64() / 450) & 1) == 0;
    int pos = 0;
    for (int i = 0; i < snap.len && pos < 33; i++) disp[pos++] = snap.text[i];
    if (caret && snap.len < snap.max_chars && pos < 33) disp[pos++] = L'_';
    disp[pos] = L'\0';

    SelectObject(memdc, f_input ? f_input : GetStockObject(DEFAULT_GUI_FONT));
    SetTextColor(memdc, RGB(255, 255, 255));
    RECT r_inp = { box.left + 12, box.top, box.right - 64, box.bottom };
    DrawTextW(memdc, disp, -1, &r_inp, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    WCHAR cnt[16];
    wsprintfW(cnt, L"%d/%d", snap.len, snap.max_chars);
    SelectObject(memdc, f_hint ? f_hint : GetStockObject(DEFAULT_GUI_FONT));
    SetTextColor(memdc, RGB(150, 175, 210));
    RECT r_cnt = { box.right - 60, box.top, box.right - 10, box.bottom };
    DrawTextW(memdc, cnt, -1, &r_cnt, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    SetTextColor(memdc, RGB(175, 192, 215));
    RECT r_hint = { bx + 18, by + 116, bx + bw - 18, by + 148 };
    DrawTextW(memdc, L"Enter: OK  |  Backspace: Del  |  Del: Clear  |  Esc: Cancel",
              -1, &r_hint, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    SelectObject(memdc, old_font);
    SetBkMode(memdc, old_bk);
    if (f_title) DeleteObject(f_title);
    if (f_input) DeleteObject(f_input);
    if (f_hint) DeleteObject(f_hint);
}

static void hle_osk_init_start(void) {
    uint32_t params = psp_arg(0);
    if (!g_osk.inited) {
        InitializeCriticalSection(&g_osk_cs);
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
        EnterCriticalSection(&g_osk_cs);
        g_osk.active = 0;
        g_osk.done = 1;
        g_osk.status = 3;
        g_osk.params_addr = params;
        g_osk.data_addr = 0;
        g_osk.out_addr = 0;
        LeaveCriticalSection(&g_osk_cs);
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

    WCHAR prompt[64], initial[32];
    osk_read_wstr(desc_addr, prompt, 64, 1);
    if (!prompt[0]) wcscpy(prompt, L"Enter Name");
    osk_read_wstr(intext_addr, initial, cap + 1, 1);

    const char *env_name = getenv("SETO_PLAYER_NAME");
    const int auto_mode = getenv("SETO_AUTO_START") != NULL;
    const int headless = (g_hwnd == NULL);

    if (auto_mode || headless) {
        WCHAR auto_buf[32];
        if (env_name && env_name[0]) {
            int i = 0;
            while (env_name[i] && i < cap) {
                auto_buf[i] = (WCHAR)(unsigned char)env_name[i];
                i++;
            }
            auto_buf[i] = L'\0';
        } else if (initial[0]) {
            wcscpy(auto_buf, initial);
        } else {
            wcscpy(auto_buf, L"YUGI");
        }
        osk_write_guest_result(data, out_addr, auto_buf, 2);
        EnterCriticalSection(&g_osk_cs);
        g_osk.active = 0;
        g_osk.done = 1;
        g_osk.cancelled = 0;
        g_osk.status = 1;
        g_osk.params_addr = params;
        g_osk.data_addr = data;
        g_osk.out_addr = out_addr;
        g_osk.max_chars = cap;
        wcscpy(g_osk.text, auto_buf);
        g_osk.len = (int)wcslen(auto_buf);
        LeaveCriticalSection(&g_osk_cs);
        psp_ret(0);
        return;
    }

    EnterCriticalSection(&g_osk_cs);
    g_osk.active = 1;
    g_osk.done = 0;
    g_osk.cancelled = 0;
    g_osk.status = 1;
    g_osk.params_addr = params;
    g_osk.data_addr = data;
    g_osk.out_addr = out_addr;
    g_osk.max_chars = cap;
    g_osk.open_tick = GetTickCount64();
    wcscpy(g_osk.prompt, prompt);
    wcscpy(g_osk.initial, initial);
    wcscpy(g_osk.text, initial);
    g_osk.len = (int)wcslen(g_osk.text);
    LeaveCriticalSection(&g_osk_cs);
    if (g_hwnd) InvalidateRect(g_hwnd, NULL, FALSE);
    psp_ret(0);
}

static void hle_osk_get_status(void) {
    if (!g_osk.inited) { psp_ret(0); return; }
    int commit = 0, cancelled = 0;
    uint32_t data_addr = 0, out_addr = 0;
    WCHAR buf[32];

    EnterCriticalSection(&g_osk_cs);
    int r = g_osk.status;
    if (g_osk.status == 1) {
        g_osk.status = 2;
    } else if (g_osk.status == 2 && g_osk.done) {
        commit = 1;
        cancelled = g_osk.cancelled;
        data_addr = g_osk.data_addr;
        out_addr = g_osk.out_addr;
        if (cancelled) {
            if (g_osk.initial[0]) wcscpy(buf, g_osk.initial);
            else wcscpy(buf, L"YUGI");
        } else {
            wcscpy(buf, g_osk.text);
        }
        g_osk.status = 3;
    }
    LeaveCriticalSection(&g_osk_cs);

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
        EnterCriticalSection(&g_osk_cs);
        g_osk.status = 4;
        g_osk.active = 0;
        LeaveCriticalSection(&g_osk_cs);
        if (g_hwnd) InvalidateRect(g_hwnd, NULL, FALSE);
    }
    psp_ret(0);
}

void seto_osk_register(void) {
    if (!g_osk.inited) {
        InitializeCriticalSection(&g_osk_cs);
        g_osk.inited = 1;
    }
    psp_hle_register(0xF6269B82u, "sceUtility", "sceUtilityOskInitStart", hle_osk_init_start);
    psp_hle_register(0xF3F76017u, "sceUtility", "sceUtilityOskGetStatus", hle_osk_get_status);
    psp_hle_register(0x4B85C861u, "sceUtility", "sceUtilityOskUpdate", hle_osk_update);
    psp_hle_register(0x3DFAEBA9u, "sceUtility", "sceUtilityOskShutdownStart", hle_osk_shutdown_start);
}

int seto_window_osk_active(void) {
    if (!g_osk.inited) return 0;
    EnterCriticalSection(&g_osk_cs);
    int a = g_osk.active;
    ULONGLONG ct = g_osk.close_tick;
    LeaveCriticalSection(&g_osk_cs);
    if (a) return 1;
    if (ct && (GetTickCount64() - ct) < 300) {
        if ((GetAsyncKeyState(VK_RETURN) & 0x8000) ||
            (GetAsyncKeyState(VK_ESCAPE) & 0x8000))
            return 1;
    }
    return 0;
}

static void paint(HWND hwnd, HDC hdc) {
    if (seto_gl_present_active()) return;
    RECT rc;
    GetClientRect(hwnd, &rc);
    const int cw = rc.right - rc.left, ch = rc.bottom - rc.top;
    if (cw <= 0 || ch <= 0) return;
    /* Compose off-screen so a repaint never shows the black fill. */
    if (cw != g_memw || ch != g_memh || !g_memdc) {
        if (g_memdc) {
            SelectObject(g_memdc, g_memold);
            DeleteObject(g_membmp);
            DeleteDC(g_memdc);
        }
        g_memdc = CreateCompatibleDC(hdc);
        g_membmp = CreateCompatibleBitmap(hdc, cw, ch);
        g_memold = SelectObject(g_memdc, g_membmp);
        g_memw = cw; g_memh = ch;
    }
    FillRect(g_memdc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
    if (g_have_frame) {
        /* 480:272 is 30:17. Fit the largest 30:17 rectangle and centre it. */
        int dw, dh;
        if ((int64_t)cw * 17 > (int64_t)ch * 30) {
            dh = ch;
            dw = MulDiv(ch, 30, 17);
        } else {
            dw = cw;
            dh = MulDiv(cw, 17, 30);
        }
        if (dw > 0 && dh > 0) {
            static int filter = -1;
            if (filter < 0) {
                const char *f = getenv("SETO_FILTER");
                filter = f ? atoi(f) : 0;
            }
            const int x = (cw - dw) / 2, y = (ch - dh) / 2;
            int drawn = 0;
            if (filter >= 2 && gdiplus_start_once()) {
                GpBitmap *bmp = NULL;
                GpGraphics *gfx = NULL;
                if (GdipCreateBitmapFromScan0(SCREEN_W, SCREEN_H, SCREEN_W * 4,
                                              PixelFormat32bppRGB, (BYTE *)g_dib, &bmp) == Ok &&
                    GdipCreateFromHDC(g_memdc, &gfx) == Ok) {
                    GdipSetInterpolationMode(gfx, InterpolationModeHighQualityBicubic);
                    GdipSetPixelOffsetMode(gfx, PixelOffsetModeHalf);
                    if (GdipDrawImageRectI(gfx, bmp, x, y, dw, dh) == Ok) drawn = 1;
                }
                if (gfx) GdipDeleteGraphics(gfx);
                if (bmp) GdipDisposeImage((GpImage *)bmp);
            }
            if (!drawn) {
                SetStretchBltMode(g_memdc, filter == 0 ? COLORONCOLOR : HALFTONE);
                if (filter != 0) SetBrushOrgEx(g_memdc, 0, 0, NULL);
                StretchDIBits(g_memdc, x, y, dw, dh,
                              0, 0, SCREEN_W, SCREEN_H, g_dib, &g_bmi,
                              DIB_RGB_COLORS, SRCCOPY);
            }
        }
    }
    if (g_fps_text[0]) {
        HFONT old_font = (HFONT)SelectObject(g_memdc, GetStockObject(DEFAULT_GUI_FONT));
        COLORREF old_fg = SetTextColor(g_memdc, RGB(80, 255, 120));
        COLORREF old_bg = SetBkColor(g_memdc, RGB(12, 16, 20));
        int old_mode = SetBkMode(g_memdc, OPAQUE);
        TextOutA(g_memdc, 8, 6, g_fps_text, (int)strlen(g_fps_text));
        SetBkMode(g_memdc, old_mode);
        SetBkColor(g_memdc, old_bg);
        SetTextColor(g_memdc, old_fg);
        SelectObject(g_memdc, old_font);
    }
    osk_paint_overlay(g_memdc, cw, ch);
    BitBlt(hdc, 0, 0, cw, ch, g_memdc, 0, 0, SRCCOPY);
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        SetTimer(hwnd, TIMER_ID, TIMER_MS, NULL);
        update_frame(hwnd);
        return 0;
    case WM_TIMER:
        if (wp == TIMER_ID) update_frame(hwnd);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        paint(hwnd, hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_SIZE:
        if (wp != SIZE_MINIMIZED) {
            if (seto_gl_present_active())
                seto_gl_present_resize(LOWORD(lp), HIWORD(lp));
            else
                InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    case WM_ERASEBKGND:
        return 1;                   /* paint() fills the client area */
    case WM_KEYDOWN:
        if (g_osk.inited) {
            int handled = 0;
            EnterCriticalSection(&g_osk_cs);
            if (g_osk.active) {
                if (wp == VK_RETURN) {
                    if (g_osk.len > 0) {
                        g_osk.cancelled = 0;
                        g_osk.done = 1;
                        g_osk.active = 0;
                        g_osk.close_tick = GetTickCount64();
                    }
                    handled = 1;
                } else if (wp == VK_ESCAPE) {
                    g_osk.cancelled = 1;
                    g_osk.done = 1;
                    g_osk.active = 0;
                    g_osk.close_tick = GetTickCount64();
                    handled = 1;
                } else if (wp == VK_DELETE) {
                    g_osk.len = 0;
                    g_osk.text[0] = L'\0';
                    handled = 1;
                }
            }
            LeaveCriticalSection(&g_osk_cs);
            if (handled) {
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }
        }
        if (wp == VK_RETURN) return 0;   /* input thread samples the keyboard */
        return DefWindowProcW(hwnd, msg, wp, lp);
    case WM_CHAR:
        if (g_osk.inited) {
            int handled = 0;
            EnterCriticalSection(&g_osk_cs);
            if (g_osk.active) {
                WCHAR ch = (WCHAR)wp;
                if (ch == L'\b') {
                    if (GetKeyState(VK_CONTROL) & 0x8000) {
                        g_osk.len = 0;
                        g_osk.text[0] = L'\0';
                    } else if (g_osk.len > 0) {
                        g_osk.text[--g_osk.len] = L'\0';
                    }
                    handled = 1;
                } else if (ch >= 0x20 && ch < 0xFFFE) {
                    int held_repeat = ((lp >> 30) & 1) &&
                                      (GetTickCount64() - g_osk.open_tick) < 350;
                    if (!held_repeat && g_osk.len < g_osk.max_chars) {
                        g_osk.text[g_osk.len++] = ch;
                        g_osk.text[g_osk.len] = L'\0';
                    }
                    handled = 1;
                }
            }
            LeaveCriticalSection(&g_osk_cs);
            if (handled) {
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    case WM_KEYUP:
        if (wp == VK_RETURN) return 0;   /* input thread samples the keyboard */
        return DefWindowProcW(hwnd, msg, wp, lp);
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, TIMER_ID);
        seto_gl_present_shutdown(hwnd);
        if (g_memdc) {
            SelectObject(g_memdc, g_memold);
            DeleteObject(g_membmp);
            DeleteDC(g_memdc);
            g_memdc = NULL;
        }
        g_hwnd = NULL;
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

static DWORD WINAPI window_thread(LPVOID unused) {
    (void)unused;

    g_read = (uint32_t *)malloc((size_t)SCREEN_W * SCREEN_H * sizeof(uint32_t));
    g_dib  = (uint32_t *)malloc((size_t)SCREEN_W * SCREEN_H * sizeof(uint32_t));
    if (!g_read || !g_dib) {
        fprintf(stderr, "window: cannot allocate frame buffers\n");
        SetEvent(g_ready);
        return 1;
    }

    memset(&g_bmi, 0, sizeof g_bmi);
    g_bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    g_bmi.bmiHeader.biWidth = SCREEN_W;
    g_bmi.bmiHeader.biHeight = -SCREEN_H;     /* top-down */
    g_bmi.bmiHeader.biPlanes = 1;
    g_bmi.bmiHeader.biBitCount = 32;
    g_bmi.bmiHeader.biCompression = BI_RGB;
    g_bmi.bmiHeader.biSizeImage = SCREEN_W * SCREEN_H * 4;

    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.lpszClassName = L"SetoStaticLive";
    if (!RegisterClassExW(&wc)) {
        fprintf(stderr, "window: RegisterClassEx failed (%lu)\n", GetLastError());
        SetEvent(g_ready);
        return 1;
    }

    int win_w = WINDOW_W, win_h = WINDOW_H;
    const char *scale = getenv("SETO_SCALE");
    if (scale && atoi(scale) > 0) {
        win_w = SCREEN_W * atoi(scale);
        win_h = SCREEN_H * atoi(scale);
    }
    RECT r = { 0, 0, win_w, win_h };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    g_hwnd = CreateWindowExW(0, wc.lpszClassName,
                             L"Seto-static \u2014 Yu-Gi-Oh! GX Tag Force",
                             WS_OVERLAPPEDWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT,
                             r.right - r.left, r.bottom - r.top,
                             NULL, NULL, wc.hInstance, NULL);
    if (!g_hwnd) {
        fprintf(stderr, "window: CreateWindowEx failed (%lu)\n", GetLastError());
        SetEvent(g_ready);
        return 1;
    }

    seto_gl_present_init(g_hwnd);

    ShowWindow(g_hwnd, SW_SHOW);
    ShowWindow(g_hwnd, SW_SHOWNORMAL);
    UpdateWindow(g_hwnd);
    SetForegroundWindow(g_hwnd);
    SetEvent(g_ready);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    free(g_dib);
    free(g_read);
    g_dib = g_read = NULL;
    return 0;
}

int seto_window_start(void) {
    g_ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!g_ready) return -1;
    g_thread = CreateThread(NULL, 0, window_thread, NULL, 0, NULL);
    if (!g_thread) {
        CloseHandle(g_ready);
        g_ready = NULL;
        return -1;
    }
    const DWORD wait = WaitForSingleObject(g_ready, 3000);
    CloseHandle(g_ready);
    g_ready = NULL;
    if (wait != WAIT_OBJECT_0 || !g_hwnd) {
        WaitForSingleObject(g_thread, 1000);
        CloseHandle(g_thread);
        g_thread = NULL;
        return -1;
    }
    return 0;
}

void seto_window_stop(void) {
    if (g_hwnd) PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
    if (g_thread) {
        WaitForSingleObject(g_thread, 2000);
        CloseHandle(g_thread);
        g_thread = NULL;
    }
}
