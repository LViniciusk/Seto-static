/* S9.1: a minimal Win32 window that presents the PSP front buffer.
 *
 * The guest and the GE keep running on their own threads; this thread only
 * copies the framebuffer the runtime already exposes, converts the channel
 * order for a 32-bit BI_RGB DIB and paints it. No renderer, no input logic and
 * no guest state live here. */

#include <psprecomp/hle.h>

#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCREEN_W 480
#define SCREEN_H 272
#define WINDOW_W 960
#define WINDOW_H 544
#define TIMER_ID 1
#define TIMER_MS 33                 /* ~30 FPS */

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

static void update_frame(HWND hwnd) {
    if (IsIconic(hwnd)) return;
    if (psp_display_read(g_read) != 0) return;
    /* 0xAABBGGRR from psp_display_read; a 32-bit BI_RGB DIB wants B,G,R,X
     * in memory, i.e. 0x00RRGGBB in the word. */
    for (int i = 0; i < SCREEN_W * SCREEN_H; i++) {
        const uint32_t c = g_read[i];
        g_dib[i] = ((c & 0x000000FFu) << 16) |
                   (c & 0x0000FF00u) |
                   ((c & 0x00FF0000u) >> 16);
    }
    g_have_frame = 1;
    InvalidateRect(hwnd, NULL, FALSE);
}

static void paint(HWND hwnd, HDC hdc) {
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
            SetStretchBltMode(g_memdc, COLORONCOLOR);
            StretchDIBits(g_memdc, (cw - dw) / 2, (ch - dh) / 2, dw, dh,
                          0, 0, SCREEN_W, SCREEN_H, g_dib, &g_bmi,
                          DIB_RGB_COLORS, SRCCOPY);
        }
    }
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
        if (wp != SIZE_MINIMIZED) InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_ERASEBKGND:
        return 1;                   /* paint() fills the client area */
    case WM_KEYDOWN:
    case WM_KEYUP:
        if (wp == VK_RETURN) return 0;   /* input thread samples the keyboard */
        break;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, TIMER_ID);
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
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.lpszClassName = L"SetoStaticLive";
    if (!RegisterClassExW(&wc)) {
        fprintf(stderr, "window: RegisterClassEx failed (%lu)\n", GetLastError());
        SetEvent(g_ready);
        return 1;
    }

    RECT r = { 0, 0, WINDOW_W, WINDOW_H };
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

    ShowWindow(g_hwnd, SW_SHOW);
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
