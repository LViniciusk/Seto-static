/* SETO-static: load the decrypted Tag Force EBOOT and start its module.
 *
 *   Seto-static.exe [path\to\ULUS10136_EBOOT.elf] [path\to\TF1.iso]
 *
 * Console host only. The generated code is the game's own translated code;
 * nothing here fakes a function or a firmware call.
 */
#include <psprecomp/cpu.h>
#include <psprecomp/mem.h>
#include <psprecomp/hle.h>
#include <psprecomp/module.h>
#include <psprecomp/dispatch.h>
#include <psprecomp/recomp_rt.h>
#include <psprecomp/render.h>

#include "gl_present.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <mmsystem.h>

extern const psp_render_backend seto_ge_backend;

void psp_recomp_register(void);   /* generated tagforce_register.c */
int  seto_window_start(void);
void seto_window_stop(void);
void seto_osk_register(void);
int  seto_window_osk_active(void);
void psp_umd_register(void);
void psp_umd_set_iso(const char *path);
void psp_fpl_register(void);
void seto_multimedia_register(void);
void seto_multimedia_shutdown(void);
int  psp_font_image_load(const char *path);
int  psp_font_bind(void);
#ifdef HAVE_FONT_CORPUS
void font_psp_recomp_register(void);   /* relocated PRX corpus */
#endif
#ifdef HAVE_STORY_CORPUS
int  psp_story_image_load(const char *path, uint32_t eboot_info);
int  psp_story_started(void);
#endif
#ifdef HAVE_FIELD_CORPUS
int  psp_field_image_load(const char *path, uint32_t eboot_info);
int  psp_field_started(void);
#endif
#ifdef HAVE_DECK_CORPUS
void psp_deck_prepare(const char *path, uint32_t eboot_info);
#endif
#ifdef HAVE_DECKTUT_CORPUS
int  psp_decktut_image_load(const char *path, uint32_t eboot_info);
#endif
#if defined(HAVE_CHARALIST_CORPUS) || defined(HAVE_SHOP_CORPUS)
int  psp_store_images_load(const char *charalist_path, const char *shop_path, uint32_t eboot_info);
#endif
#ifdef HAVE_EXTRA_CORPUS
int  psp_extra_images_load(const char *const *paths, const char *dir_hint, uint32_t eboot_info);
void psp_extra_bind_hle(void);
#endif
#ifdef HAVE_DUEL_CORPUS
int  psp_duel_images_load(const char *const *paths, uint32_t eboot_info);
int  psp_duel_manager_start_requested(void);
#elif defined(HAVE_TITLE_CORPUS)
static int psp_duel_manager_start_requested(void) { return 0; }
#endif
#ifdef HAVE_TITLE_CORPUS
int  psp_title_image_load(const char *path, uint32_t eboot_info);
int  psp_title_menu_ready(void);
void psp_title_start_request(int on);
int  psp_title_start_done(void);
int  psp_title_scene3(void);
int  psp_title_scene_state(void);
int  psp_title_confirm_sel(void);
int  psp_title_confirm_state(void);
int  psp_title_scene_phase(void);
int  psp_title_scene_cursor(void);
void psp_title_menu_request(uint32_t mask);
void psp_title_pad_request(uint32_t mask);
void psp_title_walk(uint32_t mask, int ax, int ay);
#endif

/* The EBOOT's single PT_LOAD has memsz 0x631A80; .bss runs to there. */
#define MODULE_SPAN 0x00640000u

static void report(const char *why) {
    fprintf(stderr, "%s\n", why);
    fprintf(stderr, "  last translated function: 0x%08X\n", psp_trace_last());
    fprintf(stderr, "  last loop back-edge:      0x%08X (%llu hits)\n",
            psp_trace_loop_addr(), (unsigned long long)psp_trace_loop_hits());
    fprintf(stderr, "  dispatch: %u registered, %llu calls, %llu misses\n",
            psp_dispatch_count(), (unsigned long long)psp_dispatch_calls(),
            (unsigned long long)psp_dispatch_misses());
    fprintf(stderr, "  memory:   %llu bad accesses\n",
            (unsigned long long)psp_mem_bad_access);
    fprintf(stderr, "  iso:      %llu bytes read\n",
            (unsigned long long)psp_io_bytes_read());
    fprintf(stderr, "  display:  fb 0x%08X, %llu vblanks\n",
            psp_display_framebuffer(),
            (unsigned long long)psp_display_vblanks());
    fprintf(stderr, "  ge:       %llu commands, %llu vertices, %llu pixels\n",
            (unsigned long long)psp_ge_command_count(),
            (unsigned long long)psp_ge_vertex_count(),
            (unsigned long long)psp_ge_pixels());
    psp_ge_dump_stats(stderr);
    psp_threadman_dump(stderr);
    fprintf(stderr, "  traps:    %llu unimplemented, %llu syscalls\n",
            (unsigned long long)psp_unimplemented_total(),
            (unsigned long long)psp_syscall_total());
    fprintf(stderr, "  threads:  %llu created, %llu started\n",
            (unsigned long long)psp_threadman_created(),
            (unsigned long long)psp_threadman_started());
    fprintf(stderr, "  sched:    %llu switches, %llu parks, %llu handovers\n",
            (unsigned long long)psp_threadman_switches(),
            (unsigned long long)psp_threadman_parks(),
            (unsigned long long)psp_threadman_handovers());
    fprintf(stderr, "  hle:      %llu unimplemented calls, %llu zero returns\n",
            (unsigned long long)psp_hle_unimplemented_calls(),
            (unsigned long long)psp_hle_zero_calls());
    psp_hle_dump_recent(stderr);
    psp_hle_dump_counts(stderr, 20);
    if (psp_display_framebuffer()) {
        if (psp_display_capture("build/s6-frame.ppm") == 0)
            fprintf(stderr, "  capture:  build/s6-frame.ppm\n");
        else
            fprintf(stderr, "  capture:  failed\n");
    } else {
        fprintf(stderr, "  capture:  no framebuffer\n");
    }

}

static void on_miss(uint32_t addr) {
    fprintf(stderr, "DISPATCH MISS: indirect call to 0x%08X\n", addr);
    report("stopped at the first dispatch miss");
    exit(2);
}

static void on_budget(void) {
    report("stopped by the dispatch budget");
    exit(3);
}

/* A guest loop that never parks cannot reach the host's run loop, and the
 * dispatcher budget does not see direct calls. Report from a host thread
 * instead, so a hung first stage still names itself. The cap is opt-in:
 * SETO_BRINGUP_CAP=<seconds> bounds the run; without it the game keeps running. */
static unsigned g_cap_seconds;

static DWORD WINAPI bringup_cap(LPVOID unused) {
    (void)unused;
    Sleep(g_cap_seconds * 1000u);
    char why[64];
    snprintf(why, sizeof why, "stopped by the %u s bring-up cap", g_cap_seconds);
    report(why);
    fflush(stderr);
    _Exit(5);
}

/* Real Windows keyboard, sampled without a window or message loop. START only;
 * the rest of the pad is added when a scene asks for it. The thread ends with
 * the process (or when the game exits) and releases what it held. */
#define PSP_CTRL_SELECT   0x00000001u
#define PSP_CTRL_START    0x00000008u
#define PSP_CTRL_UP       0x00000010u
#define PSP_CTRL_RIGHT    0x00000020u
#define PSP_CTRL_DOWN     0x00000040u
#define PSP_CTRL_LEFT     0x00000080u
#define PSP_CTRL_LTRIGGER 0x00000100u
#define PSP_CTRL_RTRIGGER 0x00000200u
#define PSP_CTRL_TRIANGLE 0x00001000u
#define PSP_CTRL_CIRCLE   0x00002000u
#define PSP_CTRL_CROSS    0x00004000u
#define PSP_CTRL_SQUARE   0x00008000u

/* Keyboard state, only while one of our own windows has the foreground, so
 * typing in another application does not drive the game. */
static SHORT host_key_state(int vk) {
    DWORD pid = 0;
    const HWND fg = GetForegroundWindow();
    if (!fg) return 0;
    GetWindowThreadProcessId(fg, &pid);
    if (pid != GetCurrentProcessId()) return 0;
    return GetAsyncKeyState(vk);
}

static uint32_t poll_host_pad(void) {
    uint32_t fm = 0;
    if (host_key_state(VK_LEFT) & 0x8000)   fm |= PSP_CTRL_LEFT;
    if (host_key_state(VK_RIGHT) & 0x8000)  fm |= PSP_CTRL_RIGHT;
    if (host_key_state(VK_UP) & 0x8000)     fm |= PSP_CTRL_UP;
    if (host_key_state(VK_DOWN) & 0x8000)   fm |= PSP_CTRL_DOWN;
    if (host_key_state('Z') & 0x8000)       fm |= PSP_CTRL_CROSS;
    if (host_key_state('X') & 0x8000)       fm |= PSP_CTRL_CIRCLE;
    if (host_key_state('C') & 0x8000)       fm |= PSP_CTRL_TRIANGLE;
    if (host_key_state('A') & 0x8000)       fm |= PSP_CTRL_SQUARE;
    if (host_key_state('V') & 0x8000)       fm |= PSP_CTRL_SQUARE;
    if (host_key_state('Q') & 0x8000)       fm |= PSP_CTRL_LTRIGGER;
    if (host_key_state('W') & 0x8000)       fm |= PSP_CTRL_RTRIGGER;
    if (host_key_state('E') & 0x8000)       fm |= PSP_CTRL_RTRIGGER;
    if (host_key_state(VK_RETURN) & 0x8000) fm |= PSP_CTRL_START;
    if (host_key_state(VK_BACK) & 0x8000)   fm |= PSP_CTRL_SELECT;
    if (host_key_state(VK_SPACE) & 0x8000)  fm |= PSP_CTRL_SELECT;
    return fm;
}

#ifdef HAVE_TITLE_CORPUS
/* Send one menu button until the guest changes the observed cursor or scene
 * state, or a short timeout expires. The title input update supplies the edge. */
static int box_up(void) {
    static uint32_t px[480 * 272];
    if (psp_display_read(px) != 0) return 0;
    int light = 0, total = 0;
    for (int y = 210; y < 262; y += 8)
        for (int x = 60; x < 420; x += 15) {
            const uint32_t c = px[y * 480 + x];
            const int r = (int)(c & 0xFF), g = (int)((c >> 8) & 0xFF),
                      b = (int)((c >> 16) & 0xFF);
            if (r > 140 && g > 130 && b > 115) light++;
            total++;
        }
    /* The title name-confirm panel is shorter than field dialogue boxes. */
    return light * 5 > total * 3;
}

static int name_prompt_up(void) {
    static uint32_t px[480 * 272];
    if (psp_display_read(px) != 0) return 0;
    int red = 0;
    for (int y = 230; y < 260; y++)
        for (int x = 45; x < 160; x++) {
            const uint32_t c = px[y * 480 + x];
            const int r = (int)(c & 0xFF), g = (int)((c >> 8) & 0xFF),
                      b = (int)((c >> 16) & 0xFF);
            if (r > 130 && r > g * 1.35 && r > b * 1.25) red++;
        }
    return red >= 20;
}

/* Title's name confirmation is a centered brown panel with one selected
 * YES/NO pill: YES is drawn blue, NO pink. Return -1 unless that exact state
 * and a visible highlight exist. */
static int title_confirm_choice(void) {
    static uint32_t px[480 * 272];
    if (psp_display_read(px) != 0) {
        if (getenv("SETO_TRACE_CONFIRM"))
            fprintf(stderr, "input: confirm pixels unavailable\n");
        return -1;
    }
    int panel = 0, yes = 0, no = 0;
    for (int y = 90; y < 183; y++)
        for (int x = 68; x < 412; x++) {
            const uint32_t c = px[y * 480 + x];
            const int r = (int)(c & 0xFF), g = (int)((c >> 8) & 0xFF),
                      b = (int)((c >> 16) & 0xFF);
            panel += r < 120 && g < 100 && b < 60;
        }
    for (int y = 145; y < 176; y++)
        for (int x = 165; x < 235; x++) {
            const uint32_t c = px[y * 480 + x];
            const int r = (int)(c & 0xFF), g = (int)((c >> 8) & 0xFF),
                      b = (int)((c >> 16) & 0xFF);
            yes += b > 200 && b > r + 80 && g > r + 40;
        }
    for (int y = 145; y < 176; y++)
        for (int x = 245; x < 315; x++) {
            const uint32_t c = px[y * 480 + x];
            const int r = (int)(c & 0xFF), g = (int)((c >> 8) & 0xFF),
                      b = (int)((c >> 16) & 0xFF);
            no += r > 200 && r > g + 80 && b > g + 40;
        }
    const int choice = panel < 10000 ? -1 :
                       yes > 200 && yes > no * 2 ? 1 :
                       no > 200 && no > yes * 2 ? 0 : -1;
    if (getenv("SETO_TRACE_CONFIRM"))
        fprintf(stderr, "input: confirm pixels panel=%d YES=%d NO=%d choice=%d\n",
                panel, yes, no, choice);
    return choice;
}

static int stable_title_confirm_choice(void) {
    int last = -1, stable = 0;
    for (int i = 0; i < 30; i++) {
        Sleep(100);
        const int choice = title_confirm_choice();
        if (choice < 0) { last = -1; stable = 0; continue; }
        if (choice == last) stable++; else { last = choice; stable = 1; }
        if (stable >= 3) return choice;
    }
    return -1;
}

static int menu_panel(void) {
    static uint32_t px[480 * 272];
    if (psp_display_read(px) != 0) return 0;
    int light = 0, total = 0;
    for (int y = 40; y < 130; y += 6)
        for (int x = 14; x < 92; x += 6) {
            const uint32_t c = px[y * 480 + x];
            if ((int)(c & 0xFF) > 150 && (int)((c >> 8) & 0xFF) > 140 &&
                (int)((c >> 16) & 0xFF) > 110) light++;
            total++;
        }
    return light * 2 > total;
}

/* Row of the orange highlight in the destination list, or -1. */
/* Row of the orange highlight in the 5-item command menu (Talk=0, Duel=1,
 * Item=2, PDA=3, Map=4; 19 px pitch starting at y=40). */
static int cmd_menu_row(void) {
    static uint32_t px[480 * 272];
    if (psp_display_read(px) != 0) return -1;
    int first = -1, last = -1;
    for (int y = 38; y < 142; y++) {
        const uint32_t c = px[y * 480 + 15];
        const int r = (int)(c & 0xFF), g = (int)((c >> 8) & 0xFF),
                  b = (int)((c >> 16) & 0xFF);
        if (r > 235 && g > 130 && g < 205 && b < 125) {
            if (first < 0) first = y;
            last = y;
        }
    }
    if (first < 0) return -1;
    return ((first + last) / 2 - 40) / 19;
}

static int menu_row(void) {
    static uint32_t px[480 * 272];
    if (psp_display_read(px) != 0) return -1;
    for (int y = 34; y < 172; y++) {
        const uint32_t c = px[y * 480 + 30];
        const int r = (int)(c & 0xFF), g = (int)((c >> 8) & 0xFF),
                  b = (int)((c >> 16) & 0xFF);
        if (r > 235 && g > 130 && g < 205 && b < 125) return (y - 30) / 28;
    }
    return -1;
}

static void field_press(uint32_t mask) {
    if (psp_duel_manager_start_requested()) return;
    psp_title_walk(mask, 128, 128);
    psp_ctrl_set(mask, 128, 128);
    if (getenv("SETO_TRACE_PAD")) {
        uint8_t ax, ay;
        uint32_t rb = psp_ctrl_get(&ax, &ay);
        fprintf(stderr, "pad: set=%08X now=%08X ax=%u ay=%u\n", mask, rb, ax, ay);
        Sleep(150);
        rb = psp_ctrl_get(&ax, &ay);
        fprintf(stderr, "pad: held=%08X ax=%u ay=%u\n", rb, ax, ay);
        Sleep(150);
    } else {
        Sleep(300);
    }
    psp_title_walk(0, 128, 128);
    psp_ctrl_set(0, 128, 128);
    Sleep(120);
}

/* Turn/face without crossing the interaction radius: the field applies the
 * camera-relative step on the first frames, so a long hold can walk past an
 * adjacent NPC. */
static void field_tap(uint32_t mask) {
    if (psp_duel_manager_start_requested()) return;
    psp_title_walk(mask, 128, 128);
    psp_ctrl_set(mask, 128, 128);
    Sleep(40);
    psp_title_walk(0, 128, 128);
    psp_ctrl_set(0, 128, 128);
    Sleep(120);
}

/* Short hold: the first frames turn the character, the rest move only a
 * fraction of a long press, keeping an adjacent NPC inside the talk radius. */
static void field_nudge(uint32_t mask) {
    if (psp_duel_manager_start_requested()) return;
    psp_title_walk(mask, 128, 128);
    psp_ctrl_set(mask, 128, 128);
    Sleep(100);
    psp_title_walk(0, 128, 128);
    psp_ctrl_set(0, 128, 128);
    Sleep(200);
}

/* A single frame: open the command menu without the held CROSS also selecting
 * the remembered entry on the following frame. */
static void field_one_shot(uint32_t mask) {
    if (psp_duel_manager_start_requested()) return;
    psp_title_pad_request(mask);
    psp_ctrl_set(mask, 128, 128);
    Sleep(20);
    psp_ctrl_set(0, 128, 128);
    Sleep(300);
}

/* The field keeps the command-menu/PDA substate next to the scene state; dump
 * a small window around it before and after each input so a box=0 result is
 * not confused with an unopened menu. */
static void dump_field_state(const char *tag) {
    fprintf(stderr, "probe state %s:", tag);
    for (uint32_t a = 0x093CDAD0u; a <= 0x093CDB30u; a += 4)
        fprintf(stderr, " %08X=%08X", a, psp_read32(a));
    fprintf(stderr, "\n");
}

/* After the auto travel, keep the real Win32 window alive and hand the
 * field the keyboard directly. This is the computer-use surface: capture,
 * look, press a key, capture again. */
static void hold_field_input(void) {
    fprintf(stderr, "window: holding; arrows/Z/X/C/Enter control the field\n");
    int t_down = 0;
    for (;;) {
        psp_ctrl_set(seto_window_osk_active() ? 0u : poll_host_pad(), 128, 128);
        /* T: log every GE primitive of the next frame to stderr. */
        const int t = (host_key_state('T') & 0x8000) != 0;
        if (t && !t_down) {
            psp_ge_trace_next_frame();
            fprintf(stderr, "input: manual GE trace requested\n");
        }
        t_down = t;
        Sleep(10);
    }
}

static int screen_black(void) {
    static uint32_t px[480 * 272];
    if (psp_display_read(px) != 0) return 0;
    int bright = 0;
    for (int y = 20; y < 250; y += 10)
        for (int x = 20; x < 460; x += 10) {
            const uint32_t c = px[y * 480 + x];
            const int r = (int)(c & 0xFF), g = (int)((c >> 8) & 0xFF),
                      b = (int)((c >> 16) & 0xFF);
            if (r + g + b > 40) bright++;
        }
    return bright < 40;
}

static void auto_press(uint32_t mask) {
    if (psp_duel_manager_start_requested()) return;
    /* The title's vblank hook consumes the one-shot; scenes that run without
     * that hook read the direct state, which is held here for 100 ms. */
    psp_title_pad_request(mask);
    psp_ctrl_set(mask, 128, 128);
    Sleep(100);
    psp_ctrl_set(0, 128, 128);
}

/* Retry unconsumed edges, but stop as soon as the original window accepts YES. */
static int title_confirm_yes(void) {
    for (int i = 0; i < 200 && psp_title_confirm_state() == 2; i++) {
        if (psp_duel_manager_start_requested()) return 0;
        Sleep(10);
    }
    if (psp_duel_manager_start_requested() || psp_title_confirm_state() != 3 ||
        stable_title_confirm_choice() != 1) return 0;
    psp_title_menu_request(PSP_CTRL_CROSS);
    for (int i = 0; i < 200 && psp_title_confirm_state() == 3; i++) {
        if (psp_duel_manager_start_requested()) break;
        Sleep(10);
    }
    psp_title_menu_request(0);
    psp_ctrl_set(0, 128, 128);
    return psp_title_confirm_state() != 3;
}

/* The "Data loaded." [OK] window is the same message window object as the
 * name dialog, but it has a single button, so the YES/NO classifier in
 * title_confirm_yes() cannot accept it. Feed both the title-menu edge and the
 * digital pad and wait for the window state to leave 2. */
static int title_confirm_ok(void) {
    /* The window's confirm byte stays 2 even after it accepts the [OK]; the
     * scene state/phase is what actually moves. Watch that, and repeat the
     * press because the first edge can land while the load is still drawing. */
    const int s0 = psp_title_scene_state(), p0 = psp_title_scene_phase();
    for (int attempt = 0; attempt < 8; attempt++) {
        if (psp_title_scene_state() != s0 || psp_title_scene_phase() != p0) return 1;
        psp_title_menu_request(PSP_CTRL_CROSS);
        psp_title_pad_request(PSP_CTRL_CROSS);
        psp_ctrl_set(PSP_CTRL_CROSS, 128, 128);
        Sleep(300);
        psp_title_menu_request(0);
        psp_ctrl_set(0, 128, 128);
        for (int i = 0; i < 25; i++) {
            if (psp_title_scene_state() != s0 || psp_title_scene_phase() != p0) return 1;
            Sleep(20);
        }
    }
    return 0;
}

static void try_talk_aluno(void) {
    for (int t = 0; t < 20 && box_up(); t++) {
        auto_press(PSP_CTRL_CROSS);
        Sleep(400);
    }
    static const uint32_t dpad[4] = { 0x00000080u, 0x00000020u,
                                      0x00000010u, 0x00000040u };
    /* Camera-relative: DOWN walks toward the Classroom door/Crowler. */
    static const uint32_t dpad_north[4] = { 0x00000040u, 0x00000010u,
                                            0x00000080u, 0x00000020u };
    const int nudge = getenv("SETO_TALK_NUDGE") != NULL;
    const int north = getenv("SETO_TALK_NORTH") != NULL;
    const uint32_t *dirs = (nudge || north) ? dpad_north : dpad;
    for (int d = 0; d < 4 && !box_up(); d++) {
        if (nudge) field_nudge(dirs[d]);
        else if (getenv("SETO_TALK_TAP")) field_tap(dirs[d]);
        else field_press(dirs[d]);
        {
            char path[48];
            snprintf(path, sizeof path, "build/s439-pos-%d.ppm", d);
            psp_display_capture(path);
        }
        auto_press(PSP_CTRL_CROSS);
        Sleep(800);
        if (psp_read32(0x093CDAFCu) == 6u) {
            /* The command menu remembers its cursor: the Map travel left it
             * on Map, so reset to Talk before selecting it. */
            for (int u = 0; u < 4; u++) { field_press(0x00000010u); Sleep(250); }
            auto_press(PSP_CTRL_CROSS);    /* Talk is the first entry */
            Sleep(1200);
        }
        {
            char path[48];
            snprintf(path, sizeof path, "build/s439-box-%d.ppm", d);
            psp_display_capture(path);
        }
        fprintf(stderr, "input: talk dir=%d box=%d state=%u\n", d, box_up(),
                psp_read32(0x093CDAFCu));
    }
    psp_display_capture("build/s428-talk.ppm");
    for (int t = 0; t < 12 && box_up(); t++) {
        auto_press(PSP_CTRL_CROSS);
        Sleep(400);
    }
    psp_display_capture("build/s428-talk-end.ppm");
    fprintf(stderr, "input: talk ended box=%d state=%u\n",
            box_up(), psp_read32(0x093CDAFCu));
}

static void try_talk_menu(void) {
    psp_title_walk(0, 255, 255);
    psp_ctrl_set(0, 255, 255);
    Sleep(1200);
    psp_title_walk(0, 128, 128);
    psp_ctrl_set(0, 128, 128);
    Sleep(500);
    field_press(PSP_CTRL_CROSS);
    Sleep(1500);
    psp_display_capture("build/s430-menu.ppm");
    fprintf(stderr, "input: talk menu state=%u\n", psp_read32(0x093CDAFCu));
    field_press(PSP_CTRL_CROSS);      /* Talk is the first entry */
    Sleep(2000);
    psp_display_capture("build/s430-talk-menu.ppm");
    fprintf(stderr, "input: talk menu select state=%u box=%d\n",
            psp_read32(0x093CDAFCu), box_up());
    for (int t = 0; t < 12 && box_up(); t++) {
        auto_press(PSP_CTRL_CROSS);
        Sleep(400);
    }
    psp_display_capture("build/s430-talk-menu-end.ppm");
    fprintf(stderr, "input: talk menu ended state=%u box=%d\n",
            psp_read32(0x093CDAFCu), box_up());
}

static void try_talk_start(void) {
    field_press(PSP_CTRL_CIRCLE);   /* close a PDA/action menu left open */
    Sleep(500);
    field_press(PSP_CTRL_START);
    Sleep(1500);
    psp_display_capture("build/s431-start.ppm");
    fprintf(stderr, "input: start menu state=%u\n", psp_read32(0x093CDAFCu));
    field_press(PSP_CTRL_CROSS);      /* Talk is the first entry */
    Sleep(2000);
    psp_display_capture("build/s431-start-talk.ppm");
    fprintf(stderr, "input: start talk state=%u box=%d\n",
            psp_read32(0x093CDAFCu), box_up());
    for (int t = 0; t < 12 && box_up(); t++) {
        auto_press(PSP_CTRL_CROSS);
        Sleep(400);
    }
    psp_display_capture("build/s431-start-talk-end.ppm");
    fprintf(stderr, "input: start talk ended state=%u box=%d\n",
            psp_read32(0x093CDAFCu), box_up());
}

static void try_walk_class(void) {
    for (int i = 0; i < 40; i++) {
        field_press((i & 1) ? 0x00000080u : 0x00000020u);
        if (i % 10 == 9) {
            char path[48];
            snprintf(path, sizeof path, "build/s442-walk-%d.ppm", i / 10);
            psp_display_capture(path);
            fprintf(stderr, "input: walk step=%d state=%u box=%d\n",
                    i + 1, psp_read32(0x093CDAFCu), box_up());
        }
        if (box_up() || psp_read32(0x093CDAFCu) != 1u) break;
    }
    psp_display_capture("build/s442-walk-end.ppm");
}

/* Camera-relative: the s504 sequence clears the doorway (RIGHT, UP) before
 * DOWN; a pure DOWN from the spawn is blocked by the door wall. */
static void try_walk_down(void) {
    dump_field_state("walk-before");
    /* Replay the exact s504 movement prefix: the blocked LEFT sets the facing,
     * then RIGHT/UP clear the doorway before the long DOWN. */
    field_press(0x00000080u);
    field_press(0x00000020u);
    field_press(0x00000010u);
    for (int i = 0; i < 12; i++) {
        field_press(0x00000040u);
        if (i % 3 == 2) {
            char path[64];
            snprintf(path, sizeof path, "build/s493-walk-%d.ppm", i);
            psp_display_capture(path);
            fprintf(stderr, "input: walk-down %d state=%u box=%d\n", i + 1,
                    psp_read32(0x093CDAFCu), box_up());
            dump_field_state("walk");
        }
        if (box_up() || psp_read32(0x093CDAFCu) != 1u) break;
    }
    psp_display_capture("build/s493-walk-end.ppm");
}

static int title_press_until_change(uint32_t mask) {
    const int state = psp_title_scene_state(), phase = psp_title_scene_phase();
    for (int attempt = 0; attempt < 4; attempt++) {
        auto_press(mask);
        for (int i = 0; i < 50 && psp_title_scene_state() == state &&
                            psp_title_scene_phase() == phase; i++)
            Sleep(20);
        if (psp_title_scene_state() != state || psp_title_scene_phase() != phase)
            return 1;
    }
    return 0;
}

static void ensure_field_free_roam(void) {
    for (int i = 0; i < 16 && !psp_duel_manager_start_requested(); i++) {
        uint32_t st = psp_read32(0x093CDAFCu);
        if (st == 1u && !box_up()) break;
        if (st == 6u) {
            auto_press(PSP_CTRL_CIRCLE);
            Sleep(250);
        } else {
            auto_press(PSP_CTRL_CROSS);
            Sleep(250);
        }
    }
}

static int try_talk_here(void) {
    if (psp_duel_manager_start_requested()) return 1;
    ensure_field_free_roam();
    if (psp_duel_manager_start_requested()) return 1;
    if (psp_read32(0x093CDAFCu) != 1u) return 0;
    auto_press(PSP_CTRL_CROSS);
    Sleep(400);
    if (psp_duel_manager_start_requested()) return 1;
    if (psp_read32(0x093CDAFCu) == 6u) {
        int r = cmd_menu_row();
        for (int u = 0; u < 5 && r > 0; u++) {
            auto_press(PSP_CTRL_UP);
            Sleep(200);
            r = cmd_menu_row();
        }
        if (r == 0) {
            auto_press(PSP_CTRL_CROSS);
            Sleep(500);
        } else {
            ensure_field_free_roam();
            return 0;
        }
    }
    if (box_up() || psp_read32(0x093CDAFCu) == 23u || psp_read32(0x093CDAFCu) == 10u) {
        fprintf(stderr, "input: class_sweep talking state=%u\n", psp_read32(0x093CDAFCu));
        for (int t = 0; t < 80 && !psp_duel_manager_start_requested(); t++) {
            uint32_t st = psp_read32(0x093CDAFCu);
            if (st == 1u && !box_up()) break;
            if (st == 6u) {
                ensure_field_free_roam();
                break;
            }
            auto_press(PSP_CTRL_CROSS);
            Sleep(300);
        }
        fprintf(stderr, "input: class_sweep talk done state=%u duel=%d\n",
                psp_read32(0x093CDAFCu), psp_duel_manager_start_requested());
    }
    ensure_field_free_roam();
    return psp_duel_manager_start_requested();
}

static void analog_walk_ms(int ax, int ay, int ms) {
    ensure_field_free_roam();
    if (psp_read32(0x093CDAFCu) != 1u) return;
    psp_title_walk(0, ax, ay);
    psp_ctrl_set(0, (uint8_t)ax, (uint8_t)ay);
    Sleep((DWORD)ms);
    psp_title_walk(0, 128, 128);
    psp_ctrl_set(0, 128, 128);
    Sleep(100);
}

static void class_sweep_and_talk(void) {
    /* Classroom spawn sits right inside the hallway doorway with Crowler
     * blocking down-left; clear Crowler by stepping down-right/down first, then
     * sweep the desk aisles across the room while closing any stale state=6
     * menu before each walk step. */
    static const struct { int ax, ay, chunks, talk; } legs[] = {
        { 175, 255, 3, 0 }, /* step down-right past Crowler out of doorway */
        { 255, 220, 2, 0 }, /* clear into open front aisle */
        {   0, 140, 7, 1 }, /* sweep left across upper desk aisle */
        {  60, 240, 4, 1 }, /* down-left to middle aisle */
        { 255, 110, 8, 1 }, /* sweep right across middle desk aisle */
        { 160, 255, 4, 1 }, /* down-right to lower aisle */
        {   0, 140, 8, 1 }, /* sweep left across lower desk aisle */
        { 200,  20, 4, 1 }, /* up-right along desks */
        { 255, 160, 7, 1 }, /* sweep back right */
        {  40, 220, 4, 1 }, /* bottom-left desks */
        { 220,  40, 6, 1 }, /* back up-right toward Crowler */
        { 128,   0, 3, 1 }, /* step up into doorway near Crowler */
    };
    psp_display_capture("build/class-sweep-0.ppm");
    fprintf(stderr, "input: class_sweep start state=%u\n", psp_read32(0x093CDAFCu));
    ensure_field_free_roam();
    for (int l = 0; l < (int)(sizeof(legs) / sizeof(legs[0])) &&
                    !psp_duel_manager_start_requested(); l++) {
        for (int s = 0; s < legs[l].chunks && !psp_duel_manager_start_requested(); s++) {
            analog_walk_ms(legs[l].ax, legs[l].ay, 350);
            if (legs[l].talk && try_talk_here()) return;
        }
        char cap[64];
        snprintf(cap, sizeof cap, "build/class-sweep-leg-%d.ppm", l);
        psp_display_capture(cap);
        if (l == 4) psp_display_capture("build/class-sweep-mid.ppm");
    }
    psp_display_capture("build/class-sweep-end.ppm");
    fprintf(stderr, "input: class_sweep finished state=%u duel=%d\n",
            psp_read32(0x093CDAFCu), psp_duel_manager_start_requested());
}

/* Row of the orange highlight on the main menu (Story Mode starts at y 63,
 * one row per 25 px). The main menu does not read the normal pad path, so
 * the caller drives psp_title_menu_request and checks this after each hold. */
static int main_menu_row(void) {
    static uint32_t px[480 * 272];
    if (psp_display_read(px) != 0) return -1;
    int y0 = -1, y1 = -1;
    for (int y = 40; y < 240; y++) {
        int c = 0;
        for (int x = 140; x < 372; x++) {
            const uint32_t v = px[y * 480 + x];
            const int r = (int)(v & 0xFF), g = (int)((v >> 8) & 0xFF),
                      b = (int)((v >> 16) & 0xFF);
            if (r > 195 && g > 85 && g < 165 && b < 70) c++;
        }
        if (c > 80) { if (y0 < 0) y0 = y; y1 = y; }
    }
    if (y0 < 0) return -1;
    return ((y0 + y1) / 2 - 71 + 12) / 25;
}

/* Row of the orange highlight in the destination list (left panel). The list
 * remembers the previous destination, so a fixed DOWN count is not reliable. */
static int map_list_row(void) {
    static uint32_t px[480 * 272];
    if (psp_display_read(px) != 0) return -1;
    int y0 = -1, y1 = -1;
    for (int y = 45; y < 180; y++) {
        int c = 0;
        for (int x = 70; x < 155; x++) {
            const uint32_t v = px[y * 480 + x];
            const int r = (int)(v & 0xFF), g = (int)((v >> 8) & 0xFF),
                      b = (int)((v >> 16) & 0xFF);
            if (r > 220 && g > 140 && g < 200 && b < 110) c++;
        }
        if (c > 60) { if (y0 < 0) y0 = y; y1 = y; }
    }
    if (y0 < 0) return -1;
    const int row = ((y0 + y1) / 2 - 54 + 12) / 24;
    return row < 0 ? 0 : row > 4 ? 4 : row;
}

/* One short menu_request hold: the hook re-issues the edge every input update
 * while the request is set, so keep it near one update. */
static void menu_hold(uint32_t mask, int ms) {
    psp_title_menu_request(mask);
    Sleep(ms);
    psp_title_menu_request(0);
    Sleep(300);
}

static void auto_menu(uint32_t mask, int by_cursor) {
    const int c0 = psp_title_scene_cursor();
    const int s0 = psp_title_scene_state();
    psp_title_menu_request(mask);
    for (int i = 0; i < 200; i++) {
        const int c = psp_title_scene_cursor();
        const int s = psp_title_scene_state();
        if (s != s0 || (by_cursor && c != c0)) break;
        Sleep(10);
    }
    psp_title_menu_request(0);
    fprintf(stderr, "input: auto menu 0x%04X state %d->%d cursor %d->%d\n",
            mask, s0, psp_title_scene_state(), c0, psp_title_scene_cursor());
}
#endif

/* Read the field state machine word (0x093CDAFC, inside modfield's .bss) and
 * log every change. Headless runs otherwise only see pixels; this names the
 * state the field script is in. */
#ifdef HAVE_TITLE_CORPUS
static int is_normal_duel_teal(void) {
    static uint32_t px[480 * 272];
    if (psp_display_read(px) != 0) return 0;
    int teal = 0;
    for (int y = 70; y < 220; y += 10) {
        for (int x = 100; x < 380; x += 14) {
            const uint32_t v = px[y * 480 + x];
            const int r = (int)(v & 0xFF), g = (int)((v >> 8) & 0xFF),
                      b = (int)((v >> 16) & 0xFF);
            if (r < 140 && g > 140 && b > 120) teal++;
        }
    }
    return teal >= 60;
}

static int is_cream_list(void) {
    static uint32_t px[480 * 272];
    if (psp_display_read(px) != 0) return 0;
    int cream = 0;
    for (int y = 70; y < 220; y += 10) {
        for (int x = 100; x < 380; x += 14) {
            const uint32_t v = px[y * 480 + x];
            const int r = (int)(v & 0xFF), g = (int)((v >> 8) & 0xFF),
                      b = (int)((v >> 16) & 0xFF);
            if (r > 230 && g > 230 && b > 210) cream++;
        }
    }
    return cream >= 200;
}

/* Hold button across multiple vblanks so 30Hz title/menu loops always sample the edge. */
static void hold_pad(uint32_t mask, int ms) {
    if (psp_duel_manager_start_requested()) return;
    psp_title_walk(mask, 128, 128);
    psp_ctrl_set(mask, 128, 128);
    Sleep(ms);
    psp_title_walk(0, 128, 128);
    psp_ctrl_set(0, 128, 128);
}

static void hold_pad_direct(uint32_t mask, int ms) {
    if (psp_duel_manager_start_requested()) return;
    psp_ctrl_set(mask, 128, 128);
    Sleep(ms);
    psp_ctrl_set(0, 128, 128);
    Sleep(120);
}

static void enter_free_duel(void) {
    int row = -1;
    for (int w = 0; w < 80 && row < 0; w++) {
        Sleep(100);
        if (psp_title_scene_state() == 3) row = main_menu_row();
    }
    fprintf(stderr, "input: free menu row=%d cursor=%d\n", row, psp_title_scene_cursor());
    for (int i = 0; i < 6 && row >= 0 && row != 1; i++) {
        auto_menu(row < 1 ? PSP_CTRL_DOWN : PSP_CTRL_UP, 1);
        Sleep(150);
        row = main_menu_row();
        fprintf(stderr, "input: free menu row=%d cursor=%d\n", row, psp_title_scene_cursor());
    }
    psp_display_capture("build/s93-free-menu.ppm");
    if (row != 1) {
        fprintf(stderr, "input: free duel unavailable (row=%d)\n", row);
        return;
    }
    for (int i = 0; i < 3 && psp_title_scene_state() == 3; i++)
        auto_menu(PSP_CTRL_CROSS, 0);
    for (int i = 0; i < 6; i++) {
        Sleep(700);
        if (i == 2 || i == 5) {
            char path[64];
            snprintf(path, sizeof path, "build/s93-duel%d.ppm", i);
            psp_display_capture(path);
        }
    }
    fprintf(stderr, "input: free duel state=%d phase=%d\n",
            psp_title_scene_state(), psp_title_scene_phase());
    if (getenv("SETO_FREE_DUEL_PLAY")) {
        /* Headless navigation: four CROSSes leave the settings pages, then
         * RIGHT selects the Start Duel button on the opponent/recipe summary. */
        if (getenv("SETO_FREE_DUEL_START")) {
            for (int i = 0; i < 4; i++) {
                hold_pad_direct(PSP_CTRL_CROSS, 160);
                Sleep(700);
            }
            psp_display_capture("build/free-start-summary.ppm");
            for (int i = 0; i < 12 && !psp_duel_manager_start_requested(); i++) {
                Sleep(1000);
                if (i == 2 || i == 6) psp_display_capture("build/free-start-wait.ppm");
            }
            fprintf(stderr, "input: free start wait=%d phase=%d\n",
                    psp_duel_manager_start_requested(), psp_title_scene_phase());
            if (!psp_duel_manager_start_requested()) {
                hold_pad_direct(PSP_CTRL_RIGHT, 120);
                Sleep(300);
                psp_display_capture("build/free-start-right.ppm");
                hold_pad_direct(PSP_CTRL_CROSS, 160);
                Sleep(1500);
                fprintf(stderr, "input: free start right=%d phase=%d\n",
                        psp_duel_manager_start_requested(), psp_title_scene_phase());
            }
            if (!psp_duel_manager_start_requested()) {
                hold_pad_direct(PSP_CTRL_START, 160);
                Sleep(1500);
                fprintf(stderr, "input: free start START=%d phase=%d\n",
                        psp_duel_manager_start_requested(), psp_title_scene_phase());
            }
        }
        for (int w = 0; w < 40 && !is_normal_duel_teal() && psp_title_scene_phase() != 6; w++)
            Sleep(100);
        for (int i = 0; i < 10 && !is_normal_duel_teal() && !psp_duel_manager_start_requested(); i++) {
            hold_pad_direct(PSP_CTRL_CROSS, 150);
            for (int w = 0; w < 8 && !is_normal_duel_teal(); w++) Sleep(100);
        }
        if (is_normal_duel_teal()) {
            Sleep(2000);
            hold_pad_direct(PSP_CTRL_CROSS, 180);
            Sleep(800);
            psp_display_capture("build/s93-free-play4.ppm");
        }
        for (int i = 0; i < 35 && !psp_duel_manager_start_requested(); i++) {
            int ph = psp_title_scene_phase();
            int cream = is_cream_list();
            if (i < 4 || i == 6 || i == 11 || i == 15) {
                char path[64];
                snprintf(path, sizeof path, "build/s93-free-play%d.ppm", i);
                psp_display_capture(path);
            }
            fprintf(stderr,
                    "input: free try=%d ph=%d cream=%d state=%d start=%d pad=0x%08x\n",
                    i, ph, cream, psp_title_scene_state(),
                    psp_duel_manager_start_requested(),
                    psp_read32(0x08101540u));
            if (ph == 11) {
                Sleep(500);
                continue;
            }
            if (ph == 6 || cream) {
                if (cream && i >= 6 && i < 9) {
                    hold_pad_direct(PSP_CTRL_DOWN, 120);
                    Sleep(200);
                    hold_pad_direct(PSP_CTRL_CROSS, 180);
                    Sleep(800);
                } else if (cream && i >= 9) {
                    hold_pad_direct((i & 1) ? PSP_CTRL_START : PSP_CTRL_CROSS, 180);
                    Sleep(800);
                } else {
                    hold_pad_direct(PSP_CTRL_CROSS, 180);
                    Sleep(800);
                }
                continue;
            }
            uint32_t btn = (i & 1) ? PSP_CTRL_CROSS : PSP_CTRL_START;
            hold_pad_direct(btn, 250);
            for (int w = 0; w < 12 && !psp_duel_manager_start_requested(); w++)
                Sleep(50);
        }
        fprintf(stderr, "input: free duel play start=%d\n",
                psp_duel_manager_start_requested());
        if (psp_duel_manager_start_requested()) {
            for (int i = 0; i < 20 && !psp_exit_requested(); i++) {
                Sleep(2500);
                if (getenv("SETO_DUEL_ADVANCE") && (i == 2 || i == 4 || i == 6 || i == 8)) {
                    psp_ctrl_set(PSP_CTRL_CROSS, 128, 128);
                    Sleep(60);
                    psp_ctrl_set(0, 128, 128);
                }
                if (getenv("SETO_DUEL_ADVANCE") && i == 13) {
                    psp_ctrl_set(PSP_CTRL_TRIANGLE, 128, 128);
                    Sleep(60);
                    psp_ctrl_set(0, 128, 128);
                }
                if (getenv("SETO_TRACE_DUEL") && (i == 3 || i == 7 || i == 11 || i == 14 || i == 16))
                    psp_ge_trace_next_frame();
                char path[64];
                snprintf(path, sizeof path, "build/s93-free-live%d.ppm", i);
                psp_display_capture(path);
            }
        }
    }
}
#endif

static DWORD WINAPI field_state_monitor(LPVOID unused) {
    (void)unused;
    uint32_t last = 0xFFFFFFFFu;
    for (;;) {
        const uint32_t s = psp_read32(0x093CDAFCu);
        if (s != last) {
            fprintf(stderr, "field state: %u (vb %llu)\n",
                    s, (unsigned long long)psp_display_vblanks());
            fflush(stderr);
            last = s;
        }
        Sleep(50);
    }
    return 0;
}

static volatile int g_input_done;

/* Passive frame recorder for the headless runs: the input thread is inside the
 * long classroom/menu loops when the duel starts, so it cannot capture it. */
static DWORD WINAPI shot_thread(LPVOID unused) {
    (void)unused;
    const char *dir = getenv("SETO_SHOT_DIR");
    if (!dir) return 0;
    for (int i = 0; !psp_exit_requested(); i++) {
        char path[160];
        snprintf(path, sizeof path, "%s/frame-%05d.ppm", dir, i);
        psp_display_capture(path);
        Sleep(400);
    }
    return 0;
}

/* Headless duel detector: the duel HUD is the only screen with the blue LP
 * bar at (216,10), the red bar at (403,10) and a black field at (236,187).
 * On that signature, trace the next GE lists and capture the frame. */
static DWORD WINAPI duel_detect_thread(LPVOID unused) {
    (void)unused;
    static uint32_t px[480 * 272];
    for (int i = 0; !psp_exit_requested(); i++) {
        if (psp_display_read(px) == 0) {
            const uint32_t lb = px[10 * 480 + 216];
            const uint32_t rb = px[10 * 480 + 403];
            const uint32_t f = px[187 * 480 + 236];
            const int blue = (int)(lb & 0xFF) < 100 && (int)((lb >> 16) & 0xFF) > 140;
            const int red = (int)(rb & 0xFF) > 140 && (int)((rb >> 8) & 0xFF) < 100 &&
                            (int)((rb >> 16) & 0xFF) < 100;
            const int dark = (int)(f & 0xFF) + (int)((f >> 8) & 0xFF) +
                             (int)((f >> 16) & 0xFF) < 30;
            if (blue && red) {
                fprintf(stderr, "duel: HUD signature at vb %llu\n",
                        (unsigned long long)psp_display_vblanks());
                /* The user sees a transition asset mid-screen that disappears
                 * as the battle settles; grab the first second. */
                for (int k = 0; k < 12; k++) {
                    char path[64];
                    snprintf(path, sizeof path, "build/duel-live-%02d.ppm", k);
                    psp_display_capture(path);
                    Sleep(80);
                }
                psp_ge_trace_next_frame();
                Sleep(80);
                psp_display_capture("build/duel-live.ppm");
                for (int s = 0; s < 180 && !psp_exit_requested(); s++) {
                    Sleep(1000);
                    char spath[64];
                    snprintf(spath, sizeof spath, "build/duel-settled-%03d.ppm", s);
                    psp_display_capture(spath);
                    if (s == 3 || s == 10 || s == 25 || s == 60)
                        psp_ge_trace_next_frame();
                }
                return 0;
            }
        }
        Sleep(40);
    }
    return 0;
}

/* Per-second FPS/VPS for headless runs; the window title shows the same
 * counters. */
static DWORD WINAPI fps_log_thread(LPVOID unused) {
    (void)unused;
    uint64_t lf = psp_display_frames(), lv = psp_display_vblanks();
    DWORD lt = GetTickCount();
    while (!psp_exit_requested()) {
        Sleep(1000);
        DWORD now = GetTickCount();
        uint64_t f = psp_display_frames(), v = psp_display_vblanks();
        double dt = (double)(now - lt) / 1000.0;
        fprintf(stderr, "fps: %.1f vps: %.1f frames=%llu vblanks=%llu\n",
                (double)(f - lf) / dt, (double)(v - lv) / dt,
                (unsigned long long)f, (unsigned long long)v);
        lf = f; lv = v; lt = now;
    }
    return 0;
}

static DWORD WINAPI input_thread(LPVOID unused) {
    (void)unused;
    const char *auto_start = getenv("SETO_AUTO_START");
    int sent = 0;
    int duel_traces = 0;
    DWORD duel_last = 0;
    const int trace_duel = getenv("SETO_TRACE_DUEL") != NULL;
    /* Full manual: no auto travel, the window keyboard drives the game from
     * the title. The hold loop maps arrows/Z/X/C/Enter to the pad. */
    if (getenv("SETO_MANUAL") && getenv("SETO_HOLD_WINDOW")) {
        hold_field_input();
        return 0;
    }
    while (!psp_exit_requested()) {
        if (trace_duel && psp_duel_manager_start_requested()) {
            const DWORD now = GetTickCount();
            if (duel_traces < 6 && (duel_last == 0 || now - duel_last >= 1500)) {
                duel_last = now;
                duel_traces++;
                psp_ge_trace_next_frame();
                fprintf(stderr, "input: duel trace %d\n", duel_traces);
            }
        }
        /* libtitle accepts START at its PRESS START step: animation 5 owns the
         * frame and its step index (0x126C6FC) is 1. The same index occurs
         * during the loading sequence, but then the animation is not 5. */
#ifdef HAVE_TITLE_CORPUS
        if (auto_start) {
            if (!sent) {
                if (getenv("SETO_SKIP_MOVIE") && !psp_title_menu_ready() &&
                    psp_display_vblanks() >= 250ull) {
                    static DWORD last_skip = 0;
                    DWORD now = GetTickCount();
                    if (last_skip == 0 || now - last_skip > 1200) {
                        last_skip = now;
                        psp_ctrl_set(0x0008u /* START */, 128, 128);
                        Sleep(80);
                        psp_ctrl_set(0, 128, 128);
                    }
                }
                if (psp_title_menu_ready()) {
                    sent = 1;
                    psp_title_start_request(1);
                    fprintf(stderr, "input: auto START request (vb %llu)\n",
                            (unsigned long long)psp_display_vblanks());
                    for (int i = 0; i < 500 && !psp_title_start_done(); i++)
                        Sleep(10);
                    for (int i = 0; i < 500 && !psp_title_scene3(); i++)
                        Sleep(10);
                    psp_title_start_request(0);
                    fprintf(stderr, "input: auto START done=%d scene3=%d (vb %llu)\n",
                            psp_title_start_done(), psp_title_scene3(),
                            (unsigned long long)psp_display_vblanks());
                    if (psp_title_scene3()) {
                        fprintf(stderr, "input: auto scene3 cursor=%d state=%d\n",
                                psp_title_scene_cursor(), psp_title_scene_state());
                        if (getenv("SETO_CONTINUE")) {
                            psp_display_capture("build/s92-continue-menu.ppm");
                            for (int t = 0; t < 4 && psp_title_scene_cursor() != 1; t++)
                                auto_menu(PSP_CTRL_DOWN, 1);
                            auto_menu(PSP_CTRL_CROSS, 0);
                            fprintf(stderr, "input: continue CROSS state=%d phase=%d cursor=%d\n",
                                    psp_title_scene_state(), psp_title_scene_phase(),
                                    psp_title_scene_cursor());
                            int prev_s = psp_title_scene_state(), prev_p = psp_title_scene_phase(), ok_sent = 0;
                            for (int i = 0; i < 150 && !psp_story_started(); i++) {
                                Sleep(100);
                                int s = psp_title_scene_state(), p = psp_title_scene_phase();
                                if (s != prev_s || p != prev_p) {
                                    fprintf(stderr, "input: continue state %d->%d phase %d->%d\n",
                                            prev_s, s, prev_p, p);
                                    prev_s = s; prev_p = p;
                                }
                                if (s == 4 && p == 3 && !ok_sent) {
                                    ok_sent = 1;
                                    Sleep(400);
                                    psp_display_capture("build/s92-continue-after.ppm");
                                    title_confirm_ok();
                                    fprintf(stderr, "input: continue OK pressed state=%d phase=%d confirm=%d\n",
                                            psp_title_scene_state(), psp_title_scene_phase(),
                                            psp_title_confirm_state());
                                } else if (ok_sent && s == 3 && p == 0) {
                                    Sleep(400);
                                    break;
                                }
                            }
                            psp_display_capture("build/s92-continue-main.ppm");
                            fprintf(stderr, "input: continue settled state=%d phase=%d cursor=%d story=%d row=%d\n",
                                    psp_title_scene_state(), psp_title_scene_phase(),
                                    psp_title_scene_cursor(), psp_story_started(),
                                    main_menu_row());
                            if (getenv("SETO_SAVE_TEST") && psp_title_scene_state() == 3) {
                                /* The menu fades in after the load; pressing before
                                 * it is drawn selects whatever row happens to be
                                 * highlighted and derails the test. */
                                int row = -1;
                                for (int w = 0; w < 80 && row < 0; w++) {
                                    Sleep(100);
                                    if (psp_title_scene_state() == 3)
                                        row = main_menu_row();
                                }
                                fprintf(stderr, "input: save_test start row=%d\n", row);
                                for (int i = 0; i < 12 && row != 6; i++) {
                                    if (row < 0) break;
                                    /* menu_hold() relies on the title vblank hook
                                     * toggling g_menu_on; an 8 ms window can miss
                                     * the release and leave the pad held, so the
                                     * list runs to the last row and stops. The
                                     * one-frame pad request always releases. */
                                    auto_press(row < 6 ? PSP_CTRL_DOWN : PSP_CTRL_UP);
                                    row = main_menu_row();
                                    fprintf(stderr, "input: save_test row=%d\n", row);
                                }
                                psp_display_capture("build/s92-save-menu.ppm");
                                auto_menu(PSP_CTRL_CROSS, 0);
                                int sp = psp_title_scene_phase(), ss = psp_title_scene_state();
                                int yes_sent = 0, ok2_sent = 0, p20 = 0;
                                for (int i = 0; i < 300; i++) {
                                    Sleep(100);
                                    int s = psp_title_scene_state(), p = psp_title_scene_phase();
                                    if (s != ss || p != sp) {
                                        fprintf(stderr, "input: save_test state %d->%d phase %d->%d\n", ss, s, sp, p);
                                        ss = s; sp = p;
                                    }
                                    if (s == 4 && p == 20) {
                                        /* Save Data menu: walk the entry list with a
                                         * periodic CROSS until the confirm appears. */
                                        if (++p20 % 15 == 0) auto_press(PSP_CTRL_CROSS);
                                    } else if (s == 4 && p == 2 && !yes_sent) {
                                        yes_sent = 1;
                                        Sleep(400);
                                        psp_display_capture("build/s92-save-prompt.ppm");
                                        auto_press(PSP_CTRL_LEFT);
                                        Sleep(250);
                                        title_confirm_yes();
                                    } else if (yes_sent && s == 4 && p == 3 && !ok2_sent) {
                                        ok2_sent = 1;
                                        Sleep(400);
                                        psp_display_capture("build/s92-save-done.ppm");
                                        title_confirm_ok();
                                    } else if (ok2_sent && s == 3 && p == 0) {
                                        Sleep(300);
                                        break;
                                    }
                                }
                                fprintf(stderr, "input: save_test end state=%d phase=%d row=%d\n",
                                        psp_title_scene_state(), psp_title_scene_phase(), main_menu_row());
                                g_input_done = 1;
                                return 0;
                            }
                            if (!getenv("SETO_CONTINUE_STORY")) {
                                if (getenv("SETO_FREE_DUEL") && psp_title_scene_state() == 3)
                                    enter_free_duel();
                                g_input_done = 1;
                                return 0;
                            }
                            if (psp_title_scene_state() == 3) {
                                int row = -1;
                                for (int w = 0; w < 80 && row < 0; w++) {
                                    Sleep(100);
                                    if (psp_title_scene_state() == 3)
                                        row = main_menu_row();
                                }
                                for (int i = 0; i < 10 && row > 0; i++) {
                                    auto_press(PSP_CTRL_UP);
                                    row = main_menu_row();
                                }
                                fprintf(stderr, "input: continue_story menu row=%d\n", row);
                                auto_menu(PSP_CTRL_CROSS, 0);
                            }
                            int part_up = 0;
                            for (int i = 0; i < 200 && !psp_story_started(); i++) {
                                const int ts = psp_title_scene_state();
                                if (ts == 2 && !part_up) {
                                    Sleep(250);
                                    auto_press(PSP_CTRL_UP); /* From Last Continue */
                                    Sleep(150);
                                    part_up = 1;
                                }
                                if (ts == 2 || ts == 4)
                                    auto_press(PSP_CTRL_CROSS);
                                Sleep(100);
                            }
                            psp_display_capture("build/s92-continue-story.ppm");
                            fprintf(stderr, "input: continue story=%d state=%d phase=%d\n",
                                    psp_story_started(), psp_title_scene_state(),
                                    psp_title_scene_phase());
                            if (!getenv("SETO_CLASS_SWEEP") && !getenv("SETO_DUEL") &&
                                !getenv("SETO_TRAVEL_STORE")) {
                                g_input_done = 1;
                                return 0;
                            }
                            if (psp_story_started()) goto story_started;
                        }
                        for (int t = 0; t < 4 && psp_title_scene_cursor() != 0; t++)
                            auto_menu(PSP_CTRL_UP, 1);
                        if (psp_title_scene_cursor() != 0) {
                            fprintf(stderr, "input: pause; title cursor %d never reached 0\n",
                                    psp_title_scene_cursor());
                            g_input_done = 1;
                            return 0;
                        }
                        auto_menu(PSP_CTRL_CROSS, 0);
                        fprintf(stderr, "input: auto menu result state=%d cursor=%d\n",
                                psp_title_scene_state(), psp_title_scene_cursor());
                        for (int i = 0; i < 500 && psp_title_scene_phase() < 3; i++)
                            Sleep(10);
                        fprintf(stderr, "input: auto name phase=%d state=%d\n",
                                psp_title_scene_phase(), psp_title_scene_state());
                        int name_prompt = 0;
                        for (int i = 0; i < 400 && psp_title_scene_state() == 4 &&
                                            !name_prompt; i++) {
                            Sleep(100);
                            if (psp_title_scene_phase() == 4)
                                name_prompt = name_prompt_up();
                            /* The title's own animation rate varies between
                             * runs; a second START after ten seconds is what a
                             * player would do when the prompt looks idle. */
                            if (i == 100 && !name_prompt &&
                                psp_title_scene_phase() < 4)
                                title_press_until_change(PSP_CTRL_START);
                        }
                        fprintf(stderr, "input: auto name prompt=%d phase=%d state=%d\n",
                                name_prompt, psp_title_scene_phase(),
                                psp_title_scene_state());
                        if (!name_prompt) {
                            fprintf(stderr, "input: name prompt did not appear\n");
                            g_input_done = 1;
                            return 0;
                        }
                        if (psp_display_capture("build/s92-after.ppm") == 0)
                            fprintf(stderr, "capture: build/s92-after.ppm\n");
                        fprintf(stderr, "input: auto name START confirmation\n");
                        title_press_until_change(PSP_CTRL_START);
                        for (int i = 0; i < 100 &&
                                        psp_title_scene_state() == 4 &&
                                        psp_title_scene_phase() != 6 &&
                                        !psp_story_started(); i++)
                            Sleep(100);
                        int choice = psp_title_scene_state() == 4 &&
                                     psp_title_scene_phase() == 6
                                   ? stable_title_confirm_choice() : -1;
                        fprintf(stderr, "input: name confirm choice=%d state=%d phase=%d\n",
                                choice, psp_title_scene_state(),
                                psp_title_scene_phase());
                        psp_display_capture("build/s93-name-confirm-dialog.ppm");
                        if (choice == 1) {
                            psp_display_capture("build/s93-name-confirm-yes.ppm");
                            if (!title_confirm_yes()) {
                                fprintf(stderr, "input: pause; YES was not acknowledged\n");
                                g_input_done = 1;
                                return 0;
                            }
                        } else if (psp_title_scene_state() == 4 &&
                                   psp_title_scene_phase() == 6) {
                            /* The original message window reads the digital
                             * D-pad from 0x08101540; the earlier attempt
                             * only moved the analog stick. */
                            int sel = psp_title_confirm_sel();
                            fprintf(stderr, "input: name confirm LEFT (sel=%d)\n", sel);
                            for (int t = 0; t < 12 && sel != 2; t++) {
                                psp_title_pad_request(PSP_CTRL_LEFT);
                                if (t >= 4) {
                                    psp_ctrl_set(PSP_CTRL_LEFT, 128, 128);
                                    Sleep(40);
                                    psp_ctrl_set(0, 128, 128);
                                }
                                Sleep(200);
                                const int now = psp_title_confirm_sel();
                                if (now != sel)
                                    fprintf(stderr, "input: name confirm sel %d->%d\n",
                                            sel, now);
                                sel = now;
                            }
                            for (int i = 0; i < 10; i++) {
                                choice = title_confirm_choice();
                                if (choice == 1) break;
                                Sleep(100);
                            }
                            if (choice == 1) choice = stable_title_confirm_choice();
                            fprintf(stderr, "input: name confirm after LEFT choice=%d sel=%d\n",
                                    choice, sel);
                            psp_display_capture("build/s93-name-confirm-left.ppm");
                            if (choice == 1) {
                                psp_display_capture("build/s93-name-confirm-yes.ppm");
                                if (!title_confirm_yes()) {
                                    fprintf(stderr, "input: pause; YES was not acknowledged\n");
                                    g_input_done = 1;
                                    return 0;
                                }
                            } else {
                                fprintf(stderr, "input: pause at name confirmation; YES not visible\n");
                                g_input_done = 1;
                                return 0;
                            }
                        } else if (psp_title_scene_state() == 4) {
                            fprintf(stderr, "input: pause; name confirmation phase 6 did not arrive\n");
                            g_input_done = 1;
                            return 0;
                        }
                        int last_state = psp_title_scene_state();
                        fprintf(stderr, "input: name confirm CROSS state=%d phase=%d story=%d\n",
                                last_state, psp_title_scene_phase(),
                                psp_story_started());
                        fprintf(stderr, "input: auto name dialog state=%d phase=%d cursor=%d story=%d\n",
                                psp_title_scene_state(), psp_title_scene_phase(),
                                psp_title_scene_cursor(), psp_story_started());
                        for (int i = 0; i < 300 && !psp_story_started() &&
                                        psp_title_scene_state() != 3; i++) {
                            Sleep(100);
                            const int s = psp_title_scene_state();
                            if (s != last_state) {
                                fprintf(stderr, "input: auto state %d->%d phase=%d story=%d\n",
                                        last_state, s, psp_title_scene_phase(),
                                        psp_story_started());
                                last_state = s;
                            }
                        }
                        if (psp_display_capture("build/s92-menu2.ppm") == 0)
                            fprintf(stderr, "capture: build/s92-menu2.ppm\n");
                        if (!psp_story_started() && psp_title_scene_state() == 3) {
                            Sleep(100);
                            fprintf(stderr, "input: auto main menu cursor=%d\n",
                                    psp_title_scene_cursor());
                            if (getenv("SETO_FREE_DUEL")) {
                                enter_free_duel();
                            } else {
                            auto_press(0x00004000u);
                            for (int i = 0; i < 40 && !psp_story_started(); i++) {
                                auto_press((i & 1) ? 0x00004000u : 0x00000008u);
                                Sleep(60);
                            }
                            fprintf(stderr, "input: auto main menu state=%d phase=%d story=%d\n",
                                    psp_title_scene_state(), psp_title_scene_phase(),
                                    psp_story_started());
                            }
                        }
                        if (psp_display_capture("build/s92-name.ppm") == 0)
                            fprintf(stderr, "capture: build/s92-name.ppm\n");
story_started:
                        for (int w = 0; w < 60 && !psp_story_started() && !psp_field_started(); w++)
                            Sleep(100);
                        if (psp_story_started()) {
                            for (int i = 0; i < 200 && !psp_field_started(); i++)
                                Sleep(100);
                            fprintf(stderr, "input: auto story advance field=%d\n",
                                    psp_field_started());
                            for (int i = 0; i < 200 && !box_up(); i++)
                                Sleep(100);
                            int guard = 0;
                            for (; guard < 300 && box_up(); guard++) {
                                auto_press(0x00004000u);
                                Sleep(200);
                            }
                            fprintf(stderr, "input: auto advance guard=%d box=%d\n",
                                    guard, box_up());
                            if (getenv("SETO_CONTINUE") && getenv("SETO_TRAVEL_STORE") &&
                                psp_read32(0x093CDAFCu) == 0u) {
                                Sleep(500);
                                for (int step = 0; step < 6 && map_list_row() < 0; step++) {
                                    psp_title_walk(PSP_CTRL_RIGHT | PSP_CTRL_UP, 215, 65);
                                    Sleep(400);
                                    psp_title_walk(0, 128, 128);
                                    Sleep(250);
                                    psp_display_capture("build/s93-map-move.ppm");
                                    auto_press(PSP_CTRL_CROSS);
                                    Sleep(900);
                                }
                                if (map_list_row() >= 0) {
                                    goto map_list_open;
                                }
                            }
                            /* The command menu opens after an analog
                             * down-right walk (255,255) followed by CROSS. */
                            int menu = 0, seen_script = guard > 0;
                            for (int i = 0; i < 1500 && !menu; i++) {
                                const uint32_t st = psp_read32(0x093CDAFCu);
                                if (st == 5u || st == 6u || st == 23u) seen_script = 1;
                                if (st == 1u && seen_script) {
                                    for (int pass = 0; pass < 2 && box_up(); pass++) {
                                        psp_title_walk(0, 255, 255);
                                        psp_ctrl_set(0, 255, 255);
                                        Sleep(1500);
                                        psp_title_walk(0, 128, 128);
                                        psp_ctrl_set(0, 128, 128);
                                        Sleep(500);
                                    }
                                    for (int g = 0; g < 20 && box_up(); g++) {
                                        auto_press(0x00004000u);
                                        Sleep(250);
                                    }
                                    psp_title_walk(0, 255, 255);
                                    psp_ctrl_set(0, 255, 255);
                                    Sleep(900);
                                    psp_title_walk(0, 128, 128);
                                    psp_ctrl_set(0, 128, 128);
                                    Sleep(800);
                                    field_press(0x00004000u);
                                    Sleep(1500);
                                    psp_display_capture("build/s93-menu-open.ppm");
                                    fprintf(stderr, "input: auto menu-open state=%u\n",
                                            psp_read32(0x093CDAFCu));
                                    /* The menu is Talk/Duel/Item/PDA/Map. A
                                     * press can be lost, which lands on PDA
                                     * (state 11/12); back out and repeat with
                                     * one more DOWN rather than opening Email. */
                                    for (int attempt = 0; attempt < 3; attempt++) {
                                        const int downs = 4 + attempt;
                                        for (int d = 0; d < downs; d++) {
                                            field_press(0x00000040u);
                                            Sleep(600);
                                        }
                                        psp_display_capture("build/s93-map-sel.ppm");
                                        field_press(0x00004000u);
                                        Sleep(3000);
                                        const uint32_t st = psp_read32(0x093CDAFCu);
                                        fprintf(stderr, "input: menu attempt=%d downs=%d state=%u\n",
                                                attempt, downs, st);
                                        if (st != 11u && st != 12u) break;
                                        field_press(0x00002000u);   /* CIRCLE: leave PDA */
                                        Sleep(1500);
                                        field_press(0x00004000u);   /* reopen the menu */
                                        Sleep(1500);
                                    }
                                    psp_display_capture("build/s93-map.ppm");
                                    auto_press(0x00004000u);
                                    Sleep(4000);
                                    psp_display_capture("build/s93-exit-yes.ppm");
                                    /* Confirm the highlighted destination on
                                     * the world map, then let the travel and
                                     * the story request their modules. */
                                    /* After YES the field unloads and the
                                     * destination list appears over the world
                                     * map. Wait for it without pressing: a
                                     * blind CROSS on the map travels somewhere
                                     * else. Row 0 is Main Gate, so one DOWN is
                                     * Classroom. */
                                    Sleep(4000);
                                    for (int w = 0; w < 30 && screen_black(); w++) Sleep(500);
                                    Sleep(800);
                                    psp_display_capture("build/s93-map-world.ppm");
                                    fprintf(stderr, "input: travel map state=%u\n",
                                            psp_read32(0x093CDAFCu));
                                    if (getenv("SETO_TRACE_FIELD")) psp_ge_trace_next_frame();
                                    psp_display_capture("build/s93-map-list.ppm");
                                    for (int wait_row = 0; wait_row < 24 && map_list_row() < 0; wait_row++) {
                                        if (!screen_black()) auto_press(0x00004000u);
                                        Sleep(800);
                                        for (int w = 0; w < 20 && screen_black(); w++) Sleep(500);
                                    }
map_list_open:
                                    const int target_row = getenv("SETO_TRAVEL_STORE") ? 2 : 1;
                                    if (map_list_row() >= 0 && map_list_row() != target_row) {
                                        auto_press(map_list_row() < target_row ? 0x00000040u : 0x00000010u);
                                        Sleep(900);
                                    }
                                    for (int i = 0; i < 6; i++) {
                                        const int row = map_list_row();
                                        if (row < 0 || row == target_row) break;
                                        auto_press(row < target_row ? 0x00000040u
                                                                    : 0x00000010u);
                                        Sleep(700);
                                    }
                                    psp_display_capture("build/s93-map-class.ppm");
                                    auto_press(0x00004000u);
                                    Sleep(1500);
                                    for (int t = 0; t < 60 &&
                                            (psp_read32(0x093CDAFCu) == 0u ||
                                             map_list_row() >= 0 || screen_black()); t++) {
                                        if ((t == 2 || t == 5) && map_list_row() == target_row)
                                            auto_press(0x00004000u);
                                        if (t % 10 == 9) {
                                            char path[48];
                                            snprintf(path, sizeof path,
                                                     "build/s93-load%d.ppm", t);
                                            psp_display_capture(path);
                                        }
                                        Sleep(1000);
                                    }
                                    fprintf(stderr, "input: travel reloaded state=%u\n",
                                            psp_read32(0x093CDAFCu));
                                    psp_display_dump_at("build/vram-a.ppm", 0x04000000u);
                                    psp_display_dump_at("build/vram-b.ppm", 0x04088000u);
                                    psp_display_capture("build/s93-travel-confirm.ppm");
                                    /* Brief state-1 gaps separate dialogue beats;
                                     * wait for quiet before capturing free roam. */
                                    int seen_class_box = 0, quiet = 0, class_done = 0;
                                    int room_shot = 0, talk_done = 0;
                                    for (int k = 0; k < 600 && !class_done &&
                                            !psp_duel_manager_start_requested(); k++) {
                                        if (!room_shot && psp_read32(0x093CDAFCu) == 1u) {
                                            room_shot = 1;
                                            if (getenv("SETO_TRACE_ROOM") ||
                                                getenv("SETO_WATCH_CHAR"))
                                                psp_ge_trace_next_frame();
                                            if (getenv("SETO_CAPTURE_DUEL"))
                                                psp_display_capture("build/duel-fix.ppm");
                                            const char *wf = getenv("SETO_WATCH_FIELD");
                                            if (wf) psp_mem_watch_write((uint32_t)strtoul(wf, NULL, 0));
                                            if (getenv("SETO_FACE_TEST")) {
                                                psp_title_walk(0, 0, 128);
                                                Sleep(900);
                                                psp_title_walk(0, 128, 128);
                                                Sleep(400);
                                                psp_display_capture("build/s373-room-left.ppm");
                                                psp_title_walk(0, 255, 128);
                                                Sleep(900);
                                                psp_title_walk(0, 128, 128);
                                                Sleep(400);
                                                psp_display_capture("build/s373-room-right.ppm");
                                            }
                                            if (getenv("SETO_TRACE_FACE"))
                                                psp_ge_trace_next_frame();
                                            Sleep(1200);
                                            if (getenv("SETO_WATCH_CHAR")) {
                                                const uint32_t cv =
                                                    psp_ge_trace_character_vaddr();
                                                fprintf(stderr, "watch char vaddr %08X\n", cv);
                                                if (cv) psp_mem_watch_write(cv + 10);
                                            }
                                            psp_display_capture("build/s93-room.ppm");
                                            fprintf(stderr, "input: arrived room state=%u\n",
                                                    psp_read32(0x093CDAFCu));
                                        }
                                        const int box = box_up();
                                        const uint32_t st = psp_read32(0x093CDAFCu);
                                        if (box) {
                                            seen_class_box = 1;
                                            quiet = 0;
                                            auto_press(PSP_CTRL_CROSS);
                                            Sleep(800);
                                            if (k % 25 == 24) {
                                                char path[64];
                                                snprintf(path, sizeof path,
                                                         "build/s93-class%d.ppm", k + 1);
                                                psp_display_capture(path);
                                            }
                                        } else if ((seen_class_box || getenv("SETO_DUEL")) && st == 1u) {
                                            if (++quiet >= 30) class_done = 1;
                                            Sleep(100);
                                        } else {
                                            quiet = 0;
                                            /* The analog walk + CROSS can leave the
                                             * field command menu open (state 6);
                                             * select Talk (first entry). */
                                            if (st == 6u) {
                                                for (int u = 0; u < 4; u++) {
                                                    field_press(0x00000010u);
                                                    Sleep(250);
                                                }
                                                auto_press(PSP_CTRL_CROSS);
                                                Sleep(1200);
                                                psp_display_capture("build/s531-menu-talk.ppm");
                                                fprintf(stderr,
                                                        "input: menu talk state=%u box=%d\n",
                                                        psp_read32(0x093CDAFCu), box_up());
                                                if (box_up()) {
                                                    seen_class_box = 1;
                                                    quiet = 0;
                                                }
                                                Sleep(100);
                                                continue;
                                            }
                                            /* State 23 is the story dialogue after
                                             * Talk; the s435 logs opened the tutorial
                                             * with START + CROSS here. */
                                            if (st == 23u) {
                                                field_press(PSP_CTRL_START);
                                                Sleep(1200);
                                                field_press(PSP_CTRL_CROSS);
                                                Sleep(1200);
                                                psp_display_capture("build/s532-start-talk.ppm");
                                                fprintf(stderr,
                                                        "input: state23 start-talk state=%u box=%d\n",
                                                        psp_read32(0x093CDAFCu), box_up());
                                                if (box_up()) {
                                                    seen_class_box = 1;
                                                    quiet = 0;
                                                }
                                                Sleep(100);
                                                continue;
                                            }
                                            /* Analog-nub walk: the HLE ctrl read
                                             * showed ax/ay centered while the D-pad
                                             * buttons arrived, so drive the nub. */
                                            if (getenv("SETO_ANALOG_WALK") && st == 1u &&
                                                k == 30) {
                                                static const int axs[4] = { 0, 255, 128, 128 };
                                                static const int ays[4] = { 128, 128, 0, 255 };
                                                static const char *const anames[4] = {
                                                    "left", "right", "up", "down" };
                                                dump_field_state("analog-before");
                                                for (int d = 0; d < 4; d++) {
                                                    psp_title_walk(0, axs[d], ays[d]);
                                                    psp_ctrl_set(0, (uint8_t)axs[d], (uint8_t)ays[d]);
                                                    Sleep(1500);
                                                    psp_title_walk(0, 128, 128);
                                                    psp_ctrl_set(0, 128, 128);
                                                    Sleep(300);
                                                    char apath[64];
                                                    snprintf(apath, sizeof apath,
                                                             "build/s530-%s.ppm", anames[d]);
                                                    psp_display_capture(apath);
                                                    if (getenv("SETO_TRACE_ROOM"))
                                                        psp_ge_trace_next_frame();
                                                    fprintf(stderr,
                                                            "analog: %s state=%u box=%d\n",
                                                            anames[d],
                                                            psp_read32(0x093CDAFCu),
                                                            box_up());
                                                }
                                                auto_press(PSP_CTRL_CROSS);
                                                Sleep(800);
                                                psp_display_capture("build/s530-talk.ppm");
                                                fprintf(stderr, "analog: talk state=%u box=%d\n",
                                                        psp_read32(0x093CDAFCu), box_up());
                                                dump_field_state("analog-talk");
                                                if (psp_read32(0x093CDAFCu) == 6u) {
                                                    for (int u = 0; u < 4; u++) {
                                                        field_press(0x00000010u);
                                                        Sleep(250);
                                                    }
                                                    auto_press(PSP_CTRL_CROSS);
                                                    Sleep(1200);
                                                    psp_display_capture("build/s530-talk2.ppm");
                                                    fprintf(stderr,
                                                            "analog: talk2 state=%u box=%d\n",
                                                            psp_read32(0x093CDAFCu),
                                                            box_up());
                                                }
                                            }
                                            /* Pure free-roam probe: advance an invisible
                                             * cutscene, then verify each D-pad direction
                                             * with a frame capture and a GE trace. */
                                            if (getenv("SETO_WALK_PROBE") && st == 1u &&
                                                k == 30) {
                                                for (int i = 0; i < 10; i++) {
                                                    auto_press(PSP_CTRL_CROSS);
                                                    Sleep(500);
                                                }
                                                psp_display_capture("build/s490-advance.ppm");
                                                fprintf(stderr, "probe: advance state=%u box=%d\n",
                                                        psp_read32(0x093CDAFCu), box_up());
                                                psp_display_capture("build/s490-before.ppm");
                                                dump_field_state("before");
                                                if (getenv("SETO_POS_SCAN")) {
                                                    int n = 0;
                                                    for (uint32_t a = 0x08C00000u;
                                                         a < 0x093D2D90u; a += 4) {
                                                        uint32_t v = psp_read32(a);
                                                        if (v == 0x40B80000u || v == 0x3FE00000u) {
                                                            fprintf(stderr,
                                                                    "pos %08X=%08X\n", a, v);
                                                            if (n++ == 0)
                                                                psp_mem_watch_write(a);
                                                            if (n >= 12) break;
                                                        }
                                                    }
                                                    fprintf(stderr, "pos scan %d\n", n);
                                                }
                                                if (getenv("SETO_WATCH_READ"))
                                                    psp_mem_watch_read(0x08C40DD4u);
                                                if (getenv("SETO_TRACE_ROOM"))
                                                    psp_ge_trace_next_frame();
                                                static const uint32_t dirs[4] = {
                                                    0x00000080u, 0x00000020u,
                                                    0x00000010u, 0x00000040u };
                                                static const char *const dnames[4] = {
                                                    "left", "right", "up", "down" };
                                                for (int d = 0; d < 4; d++) {
                                                    field_press(dirs[d]);
                                                    Sleep(800);
                                                    char path[64];
                                                    snprintf(path, sizeof path,
                                                             "build/s490-%s.ppm", dnames[d]);
                                                    psp_display_capture(path);
                                                    dump_field_state(dnames[d]);
                                                    if (getenv("SETO_TRACE_ROOM"))
                                                        psp_ge_trace_next_frame();
                                                    fprintf(stderr,
                                                            "probe: %s state=%u box=%d\n",
                                                            dnames[d],
                                                            psp_read32(0x093CDAFCu),
                                                            box_up());
                                                    /* Same context, one direction
                                                     * press then CROSS: does the
                                                     * command menu open? */
                                                    field_one_shot(PSP_CTRL_CROSS);
                                                    Sleep(700);
                                                    char cpath[64];
                                                    snprintf(cpath, sizeof cpath,
                                                             "build/s504-%s-cross.ppm",
                                                             dnames[d]);
                                                    psp_display_capture(cpath);
                                                    fprintf(stderr,
                                                            "probe: %s cross state=%u box=%d\n",
                                                            dnames[d],
                                                            psp_read32(0x093CDAFCu),
                                                            box_up());
                                                    dump_field_state("cross");
                                                    if (psp_read32(0x093CDAFCu) == 6u) {
                                                        for (int u = 0; u < 4; u++) {
                                                            field_press(0x00000010u);
                                                            Sleep(250);
                                                        }
                                                        auto_press(PSP_CTRL_CROSS);
                                                        Sleep(1200);
                                                        psp_display_capture("build/s504-talk.ppm");
                                                        fprintf(stderr,
                                                                "probe: %s talk state=%u box=%d\n",
                                                                dnames[d],
                                                                psp_read32(0x093CDAFCu),
                                                                box_up());
                                                        dump_field_state("talk");
                                                        if (box_up()) break;
                                                    }
                                                }
                                                /* The probe block's field_press path is
                                                 * the one that moves; use it to walk
                                                 * into the room and try the talk there. */
                                                if (!box_up()) {
                                                    for (int i = 0; i < 12; i++)
                                                        field_press(0x00000040u);
                                                    Sleep(800);
                                                    /* The walk ends right/behind
                                                     * Crowler; face him before CROSS. */
                                                    field_press(0x00000080u);
                                                    Sleep(800);
                                                    field_one_shot(PSP_CTRL_CROSS);
                                                    Sleep(700);
                                                    psp_display_capture(
                                                        "build/s504-probe-talk.ppm");
                                                    fprintf(stderr,
                                                            "probe: walk-talk state=%u box=%d\n",
                                                            psp_read32(0x093CDAFCu),
                                                            box_up());
                                                    dump_field_state("walk-talk");
                                                    if (psp_read32(0x093CDAFCu) == 6u) {
                                                        for (int u = 0; u < 4; u++) {
                                                            field_press(0x00000010u);
                                                            Sleep(250);
                                                        }
                                                        auto_press(PSP_CTRL_CROSS);
                                                        Sleep(1200);
                                                        psp_display_capture(
                                                            "build/s504-probe-talk2.ppm");
                                                        fprintf(stderr,
                                                                "probe: walk-talk2 state=%u box=%d\n",
                                                                psp_read32(0x093CDAFCu),
                                                                box_up());
                                                        dump_field_state("walk-talk2");
                                                    }
                                                }
                                            }
                                            if (getenv("SETO_TRAVEL_STORE") && !talk_done &&
                                                st == 1u && k > 10) {
                                                talk_done = 1;
                                                psp_title_walk(0, 128, 0);
                                                psp_ctrl_set(0, 128, 0);
                                                Sleep(2200);
                                                psp_title_walk(0, 128, 128);
                                                psp_ctrl_set(0, 128, 128);
                                                Sleep(400);
                                                auto_press(PSP_CTRL_CROSS);
                                                Sleep(1200);
                                                psp_display_capture("build/s93-store-talk.ppm");
                                                fprintf(stderr, "input: store talk state=%u box=%d\n",
                                                        psp_read32(0x093CDAFCu), box_up());
                                                for (int s = 0; s < 10; s++) {
                                                    if (box_up()) auto_press(PSP_CTRL_CROSS);
                                                    Sleep(1500);
                                                    if (s == 2 || s == 5 || s == 9) {
                                                        char spath[64];
                                                        snprintf(spath, sizeof spath,
                                                                 "build/s93-store-ui%d.ppm", s);
                                                        psp_display_capture(spath);
                                                    }
                                                }
                                                fprintf(stderr, "input: store ui done state=%u box=%d\n",
                                                        psp_read32(0x093CDAFCu), box_up());
                                            }
                                            if (getenv("SETO_TALK_ALUNO") && !talk_done &&
                                                st == 1u && k > 20) {
                                                talk_done = 1;
                                                if (getenv("SETO_WALK_THEN_TALK"))
                                                    try_walk_down();
                                                if (getenv("SETO_TALK_START_ONLY"))
                                                    try_talk_start();
                                                else
                                                    try_talk_aluno();
                                                if (getenv("SETO_WALK_CLASS"))
                                                    try_walk_class();
                                                if (getenv("SETO_TALK_START") &&
                                                    !getenv("SETO_TALK_START_ONLY"))
                                                    try_talk_start();
                                                else if (getenv("SETO_TALK_MENU"))
                                                    try_talk_menu();
                                            }
                                            Sleep(100);
                                        }
                                    }
                                    fprintf(stderr, "input: classroom done=%d state=%u duel-start=%d\n",
                                            class_done, psp_read32(0x093CDAFCu),
                                            psp_duel_manager_start_requested());
                                    if (class_done) {
                                        if (getenv("SETO_PDA")) {
                                            field_press(PSP_CTRL_CROSS);
                                            Sleep(1500);
                                            for (int d = 0; d < 3; d++) {
                                                field_press(PSP_CTRL_DOWN);
                                                Sleep(500);
                                            }
                                            field_press(PSP_CTRL_CROSS);
                                            Sleep(2000);
                                            psp_display_capture("build/s440-pda.ppm");
                                            fprintf(stderr, "input: pda state=%u\n",
                                                    psp_read32(0x093CDAFCu));
                                            for (int d = 0; d < 4; d++) {
                                                field_press(PSP_CTRL_DOWN);
                                                Sleep(600);
                                                char path[48];
                                                snprintf(path, sizeof path,
                                                         "build/s440-pda-%d.ppm", d);
                                                psp_display_capture(path);
                                            }
                                            psp_display_capture("build/s440-pda-end.ppm");
                                        }
                                        if (getenv("SETO_CLASS_SWEEP") &&
                                            !psp_duel_manager_start_requested()) {
                                            class_sweep_and_talk();
                                        }
                                        if (getenv("SETO_DUEL")) {
                                            char path[64];
                                            for (int attempt = 0; attempt < 2 &&
                                                            !psp_duel_manager_start_requested();
                                                 attempt++) {
                                                if (psp_read32(0x093CDAFCu) == 1u) {
                                                    field_press(PSP_CTRL_CROSS);
                                                    Sleep(1500);
                                                }
                                                snprintf(path, sizeof path,
                                                         "build/s435-menu-%d.ppm", attempt);
                                                psp_display_capture(path);
                                                fprintf(stderr,
                                                        "input: duel menu attempt=%d state=%u\n",
                                                        attempt, psp_read32(0x093CDAFCu));
                                                if (psp_read32(0x093CDAFCu) == 6u) {
                                                    for (int u = 0; u < 8; u++) {
                                                        int r = cmd_menu_row();
                                                        fprintf(stderr, "input: cmd_menu row=%d\n", r);
                                                        if (r == 1) break;
                                                        field_press((r < 0 || r > 1) ? PSP_CTRL_UP : PSP_CTRL_DOWN);
                                                        Sleep(350);
                                                    }
                                                    Sleep(300);
                                                    snprintf(path, sizeof path,
                                                             "build/s435-sel-%d.ppm", attempt);
                                                    psp_display_capture(path);
                                                    field_press(PSP_CTRL_CROSS);
                                                    Sleep(2000);
                                                }
                                                snprintf(path, sizeof path,
                                                         "build/s435-scene-%d.ppm", attempt);
                                                psp_display_capture(path);
                                                fprintf(stderr,
                                                        "input: duel scene attempt=%d state=%u box=%d\n",
                                                        attempt, psp_read32(0x093CDAFCu),
                                                        box_up());
                                                for (int t = 0; t < 100 &&
                                                                !psp_duel_manager_start_requested();
                                                     t++) {
                                                    if (!box_up() && t >= 4) {
                                                        Sleep(500);
                                                        continue;
                                                    }
                                                    auto_press(PSP_CTRL_CROSS);
                                                    Sleep(500);
                                                    if (t == 6) {
                                                        snprintf(path, sizeof path,
                                                                 "build/s435-class-%d.ppm", attempt);
                                                        psp_display_capture(path);
                                                    }
                                                }
                                                snprintf(path, sizeof path,
                                                         "build/s435-end-%d.ppm", attempt);
                                                psp_display_capture(path);
                                                fprintf(stderr,
                                                        "input: duel end attempt=%d start=%d state=%u\n",
                                                        attempt,
                                                        psp_duel_manager_start_requested(),
                                                        psp_read32(0x093CDAFCu));
                                            }
                                            psp_display_capture("build/s435-duel3.ppm");
                                            fprintf(stderr, "input: duel start=%d state=%u\n",
                                                    psp_duel_manager_start_requested(),
                                                    psp_read32(0x093CDAFCu));
                                            if (getenv("SETO_DUEL_ADVANCE") &&
                                                psp_duel_manager_start_requested()) {
                                                fprintf(stderr, "input: advancing duel dialogue...\n");
                                                for (int i = 0; i < 65 && !psp_exit_requested(); i++) {
                                                    auto_press(PSP_CTRL_CROSS);
                                                    Sleep(800);
                                                }
                                            }
                                        }
                                        if (getenv("SETO_TRACE_FIELD")) psp_ge_trace_next_frame();
                                        /* Two display buffers alternate at 30 fps;
                                         * capture both plus repeats so a missing
                                         * room cannot be blamed on one frame. */
                                        for (int f = 0; f < 6; f++) {
                                            char path[48];
                                            snprintf(path, sizeof path,
                                                     "build/free-roam-%d.ppm", f);
                                            psp_display_capture(path);
                                            Sleep(33);
                                        }
                                        if (getenv("SETO_TRACE_FIELD")) {
                                            psp_display_dump_at("build/vram-a.ppm", 0x04000000u);
                                            psp_display_dump_at("build/vram-b.ppm", 0x04088000u);
                                            psp_display_dump_at("build/vram-shown.ppm",
                                                                psp_display_framebuffer());
                                        }
                                        psp_display_capture("build/classroom-free-roam.ppm");
                                        fprintf(stderr, "input: free roam vb=%llu box=%d state=%u\n",
                                                (unsigned long long)psp_display_vblanks(),
                                                box_up(), psp_read32(0x093CDAFCu));
                                        if (getenv("SETO_HOLD_WINDOW"))
                                            hold_field_input();
                                        g_input_done = 1;
                                        return 0;
                                    }
                                    menu = 1;
                                    break;
                                }
                                auto_press(0x00004000u);
                                Sleep(150);
                            }
                            if (menu) {
                                fprintf(stderr, "input: auto menu path done\n");
                                if (getenv("SETO_HOLD_WINDOW"))
                                    hold_field_input();
                                g_input_done = 1;
                                return 0;
                            }
                            static const struct { int ax, ay; const char *name; }
                            dirs[] = {
                                { 128, 255, "up" }, { 0, 128, "left" },
                                { 255, 128, "right" }, { 128, 0, "down" },
                            };
                            for (int d = 0; d < 4; d++) {
                                for (int g = 0; g < 80 && box_up(); g++) {
                                    auto_press(0x00004000u);
                                    Sleep(250);
                                }
                                psp_title_walk(0, dirs[d].ax, dirs[d].ay);
                                Sleep(4000);
                                psp_title_walk(0, 128, 128);
                                Sleep(1200);
                                if (strcmp(dirs[d].name, "left") == 0 ||
                                    strcmp(dirs[d].name, "right") == 0) {
                                    auto_press(0x00004000u);
                                    Sleep(1500);
                                }
                                char path[64];
                                snprintf(path, sizeof path, "build/s93-dir-%s.ppm",
                                         dirs[d].name);
                                psp_display_capture(path);
                                fprintf(stderr, "input: auto dir %s box=%d\n",
                                        dirs[d].name, box_up());
                            }
                            auto_press(0x00000008u);
                            Sleep(5000);
                            psp_display_capture("build/s93-pause.ppm");
                            fprintf(stderr, "input: auto pause box=%d black=%d\n",
                                    box_up(), screen_black());
                            if (getenv("SETO_MENU_MAP")) {
                                auto_press(0x00000040u);
                                Sleep(900);
                                auto_press(0x00004000u);
                                Sleep(3000);
                                for (int i = 0; i < 30; i++) {
                                    auto_press(0x00004000u);
                                    Sleep(900);
                                }
                                for (int w = 0; w < 4; w++) {
                                    for (int g = 0; g < 40 && box_up(); g++) {
                                        auto_press(0x00004000u);
                                        Sleep(250);
                                    }
                                    psp_title_walk(0, 255, 0);
                                    Sleep(4000);
                                    psp_title_walk(0, 128, 128);
                                    Sleep(800);
                                    auto_press(0x00004000u);
                                    Sleep(2500);
                                    if (screen_black()) Sleep(3000);
                                }
                                auto_press(0x00000008u);
                                Sleep(5000);
                                psp_display_capture("build/s93-menu2.ppm");
                                for (int i = 0; i < 4; i++) {
                                    auto_press(0x00000040u);
                                    Sleep(400);
                                }
                                psp_display_capture("build/s93-map-sel.ppm");
                                auto_press(0x00004000u);
                                Sleep(5000);
                                psp_display_capture("build/s93-map.ppm");
                                auto_press(0x00004000u);
                                Sleep(4000);
                                psp_display_capture("build/s93-exit-yes.ppm");
                                for (int i = 0; i < 20; i++) {
                                    auto_press(0x00004000u);
                                    Sleep(800);
                                    if (i == 5 || i == 10 || i == 15) {
                                        char path[64];
                                        snprintf(path, sizeof path, "build/s93-exit-dlg%d.ppm", i);
                                        psp_display_capture(path);
                                    }
                                }
                                for (int i = 0; i < 8; i++) Sleep(1000);
                                psp_display_capture("build/s93-exit-wait.ppm");
                                fprintf(stderr, "input: auto map done state=%d phase=%d box=%d\n",
                                        psp_title_scene_state(), psp_title_scene_phase(), box_up());
                            } else {
                            auto_press(0x00000040u);
                            Sleep(900);
                            psp_display_capture("build/s93-pause2.ppm");
                            auto_press(0x00004000u);
                            Sleep(3000);
                            psp_display_capture("build/s93-duel-start.ppm");
                            for (int i = 0; i < 30; i++) {
                                auto_press(0x00004000u);
                                Sleep(900);
                                if (i == 5 || i == 15 || i == 25) {
                                    char path[64];
                                    snprintf(path, sizeof path, "build/s93-pduel%d.ppm", i);
                                    psp_display_capture(path);
                                }
                            }
                            fprintf(stderr, "input: auto after duel state=%d phase=%d box=%d\n",
                                    psp_title_scene_state(), psp_title_scene_phase(), box_up());
                            }
                            fprintf(stderr, "input: auto story done state=%d phase=%d\n",
                                    psp_title_scene_state(), psp_title_scene_phase());
                        }
                    }
                } else {
                    Sleep(1);
                    continue;
                }
            }
        } else
#endif
        {
            /* Manual play: always forward focused host pad state directly across
             * all screens (title, menus, field, duel) without scene-state locks. */
            psp_ctrl_set(seto_window_osk_active() ? 0u : poll_host_pad(), 128, 128);
        }
        Sleep(10);
    }
    psp_ctrl_set(0, 128, 128);
    g_input_done = 1;
    return 0;
}

/* Entry reached: the first thing the run has to prove. */
static uint32_t g_font_entry;

static void on_font_entry(uint32_t addr) {
    fprintf(stderr, "entered PRX function 0x%08X\n", addr);
}

static void on_entry(uint32_t addr) {
    fprintf(stderr, "reached translated entry 0x%08X\n", addr);
    if (g_font_entry) psp_trace_watch(g_font_entry, on_font_entry);
}

int main(int argc, char **argv) {
    const char *elf = argc > 1 ? argv[1] : "private/ULUS10136_EBOOT.elf";
    const char *iso = argc > 2 ? argv[2] : "private/TF1.iso";
    const char *font = argc > 3 ? argv[3] : NULL;
    const char *title = argc > 4 ? argv[4] : NULL;
    const char *story = argc > 5 ? argv[5] : NULL;
    const char *field = argc > 6 ? argv[6] : NULL;
    const char *deck = argc > 7 ? argv[7] : NULL;
    const char *decktut = argc > 8 ? argv[8] : NULL;
    const char *duel_mgr = argc > 9 ? argv[9] : NULL;
    const char *duel_eng = argc > 10 ? argv[10] : NULL;
    const char *duel_draw = argc > 11 ? argv[11] : NULL;
    const char *duel_swap = argc > 12 ? argv[12] : NULL;
    const char *duel_tut = argc > 13 ? argv[13] : NULL;
    const char *duel_rec = argc > 14 ? argv[14] : NULL;
    const char *charalist = argc > 15 ? argv[15] : NULL;
    const char *shop = argc > 16 ? argv[16] : NULL;
    const char *extra_paths[9];
    for (int i = 0; i < 9; i++) extra_paths[i] = argc > 17 + i ? argv[17 + i] : NULL;
    const int free_duel = getenv("SETO_FREE_DUEL") != NULL &&
                          !getenv("SETO_FREE_DUEL_PLAY");

    if (psp_mem_init() != 0) { fprintf(stderr, "memory init failed\n"); return 1; }
    timeBeginPeriod(1);

    psp_module m;
    if (psp_module_load(elf, MODULE_SPAN, &m) != 0) {
        fprintf(stderr, "usage: %s [decrypted EBOOT .elf] [disc image .iso]\n", argv[0]);
        return 1;
    }
    psp_cpu_reset();
    psp_hle_init();
    const char *ww = getenv("SETO_WATCH_WRITE");
    psp_io_set_root(getenv("SETO_IO_ROOT") ? getenv("SETO_IO_ROOT") : "build");
    if (ww) psp_mem_watch_write((uint32_t)strtoul(ww, NULL, 0));
    psp_umd_set_iso(iso);
    psp_umd_register();
    psp_fpl_register();
    seto_osk_register();
    psp_recomp_register();
    seto_multimedia_register();

    /* S5: the pre-relocated PRX image plus its namespaced corpus. The image is
     * placed, its exports are bound to the seven NIDs and called from the
     * EBOOT. Without the corpus this build stays EBOOT-only. */
#ifdef HAVE_FONT_CORPUS
    if (font) {
        if (psp_font_image_load(font) != 0) return 1;
        font_psp_recomp_register();
        if (!psp_lookup(0x08900000u)) {
            fprintf(stderr, "font PRX: dispatch entry at 0x08900000 is missing\n");
            return 1;
        }
        if (psp_font_bind() != 0) return 1;
        g_font_entry = 0x08900000u;   /* sceFontNewLib, the first expected call */
    }
#ifdef HAVE_EXTRA_CORPUS
    psp_extra_bind_hle();
#endif
#else
    if (font) {
        fprintf(stderr, "font PRX: this build has no font corpus (build/s5-font-reloc)\n");
        return 1;
    }
#endif

    /* S6: modtitle, relocated after the EBOOT. Its 197 libehsys imports are
     * forwarded to the real exports parsed from the EBOOT's own tables. */
#ifdef HAVE_TITLE_CORPUS
    if (title && psp_title_image_load(title, m.info) != 0) return 1;
#else
    if (title) {
        fprintf(stderr, "title PRX: this build has no title corpus (build/s6-title-reloc)\n");
        return 1;
    }
#endif
    /* Story mode and Free Duel use the same guest window (0x08910000), so the
     * host loads only the image set the selected mode will ask for. */
    if (!free_duel) {
#ifdef HAVE_STORY_CORPUS
        if (story && psp_story_image_load(story, m.info) != 0) return 1;
#else
        if (story) {
            fprintf(stderr, "story PRX: this build has no story corpus (build/s93-story-reloc)\n");
            return 1;
        }
#endif
#ifdef HAVE_FIELD_CORPUS
        if (field && psp_field_image_load(field, m.info) != 0) return 1;
#else
        if (field) {
            fprintf(stderr, "field PRX: this build has no field corpus (build/s93-field-reloc)\n");
            return 1;
        }
#endif
    }
#ifdef HAVE_DECK_CORPUS
    /* moddeck is 13.8 MB of .bss and does not fit next to story/field in main
     * RAM. It loads into the low window (0x01400000) when the game asks for
     * it, after libtitle has been stopped and unloaded. */
    if (deck) psp_deck_prepare(deck, m.info);
#else
    if (deck) {
        fprintf(stderr, "deck PRX: this build has no deck corpus (build/s93-deck-reloc)\n");
        return 1;
    }
#endif
#ifdef HAVE_DECKTUT_CORPUS
    if (decktut && psp_decktut_image_load(decktut, m.info) != 0) return 1;
#else
    if (decktut) {
        fprintf(stderr, "decktut PRX: this build has no corpus (build/s93-decktut-reloc)\n");
        return 1;
    }
#endif
#if defined(HAVE_CHARALIST_CORPUS) || defined(HAVE_SHOP_CORPUS)
    if (psp_store_images_load(charalist, shop, m.info) != 0) return 1;
#else
    if (charalist || shop) {
        fprintf(stderr, "store PRX: this build has no store corpora (build/s9charalist-reloc, build/s9shop-reloc)\n");
        return 1;
    }
#endif
#ifdef HAVE_DUEL_CORPUS
    {
        const char *duel[6] = { duel_mgr, duel_eng, duel_draw,
                                duel_swap, duel_tut, duel_rec };
        if (psp_duel_images_load(duel, m.info) != 0) return 1;
    }
#else
    if (duel_mgr || duel_eng || duel_draw || duel_swap || duel_tut || duel_rec) {
        fprintf(stderr, "duel PRX: this build has no duel corpora (build/s9duel-*-reloc)\n");
        return 1;
    }
#endif
#ifdef HAVE_EXTRA_CORPUS
    if (psp_extra_images_load(extra_paths,
                              shop ? shop : (charalist ? charalist : elf),
                              m.info) != 0)
        return 1;
#else
    for (int i = 0; i < 9; i++) {
        if (extra_paths[i]) {
            fprintf(stderr, "extra PRX: this build has no extra corpora (build/s13-*-reloc)\n");
            return 1;
        }
    }
#endif

    /* This EBOOT has no .ctors section. The upstream heuristic scans the last
     * 64 KB of the image for a run of pointers into that same window and here
     * matches eight .data words (first target 0x000EA0D8), which are not code.
     * Running them would dispatch into data before module_start. */
    m.ctors = 0;
    m.nctors = 0;

    printf("module  \"%s\"\n", elf);
    printf("disc    \"%s\"\n", iso);
    if (font) printf("font    \"%s\"\n", font);
    if (title) printf("title   \"%s\"\n", title);
    if (story) printf("story   \"%s\"\n", story);
    if (field) printf("field   \"%s\"\n", field);
    if (deck) printf("deck    \"%s\"\n", deck);
    if (decktut) printf("decktut \"%s\"\n", decktut);
    if (charalist) printf("charalist \"%s\"\n", charalist);
    if (shop) printf("shop    \"%s\"\n", shop);
    printf("image   0x%08X..0x%08X  entry 0x%08X  gp 0x%08X  ctors %d\n",
           m.base, m.image_end, m.entry, m.gp, m.nctors);
    printf("dispatch %u functions/labels registered\n", psp_dispatch_count());
    fflush(stdout);

    psp_set_miss_handler(on_miss);
    /* Opt-in like the cap: SETO_DISPATCH_BUDGET=<calls>. The title's wait loop
     * legitimately runs millions of dispatches, so the old fixed 5M ended
     * normal runs at the title. */
    const char *db = getenv("SETO_DISPATCH_BUDGET");
    uint64_t budget = db ? strtoull(db, NULL, 0) : 0;
    if (budget) {
        printf("dispatch budget %llu\n", (unsigned long long)budget);
        psp_dispatch_set_budget(budget, on_budget);
    }
    psp_trace_watch(m.entry, on_entry);
    if (!getenv("SETO_NO_WINDOW")) {
        if (seto_window_start() == 0)
            fprintf(stderr, "window: 960x544 live framebuffer\n");
        else
            fprintf(stderr, "window: failed; continuing headless\n");
    } else {
        fprintf(stderr, "window: disabled by SETO_NO_WINDOW\n");
    }
    if (seto_ge_gpu_draw_mode() && getenv("SETO_RENDERER") &&
        strcmp(getenv("SETO_RENDERER"), "gl") == 0) {
        psp_render_set_backend(&seto_ge_backend);
        fprintf(stderr, "render: GL GE frontend active\n");
    }
    CreateThread(NULL, 0, input_thread, NULL, 0, NULL);
    if (getenv("SETO_SHOT_DIR"))
        CreateThread(NULL, 0, shot_thread, NULL, 0, NULL);
    if (getenv("SETO_DUEL_DETECT"))
        CreateThread(NULL, 0, duel_detect_thread, NULL, 0, NULL);
    if (getenv("SETO_FPS_LOG"))
        CreateThread(NULL, 0, fps_log_thread, NULL, 0, NULL);
    if (getenv("SETO_TRACE_FIELD"))
        CreateThread(NULL, 0, field_state_monitor, NULL, 0, NULL);
    const char *cap = getenv("SETO_BRINGUP_CAP");
    if (cap && atoi(cap) > 0) {
        g_cap_seconds = (unsigned)atoi(cap);
        printf("bring-up cap %u s\n", g_cap_seconds);
        CreateThread(NULL, 0, bringup_cap, NULL, 0, NULL);
    }

    uint32_t r = psp_module_start(&m);
    printf("module_start returned 0x%08X\n", r);

    /* Keep the scheduler turning until the scripted input finishes, then for a
     * short grace slice so the last presses can request modules. */
    int idle = 0;
    while (!psp_exit_requested() && idle < 5000) {
        const uint32_t step = g_input_done ? 16u : 50u;
        psp_threadman_host_sleep_ms(step);
        idle = g_input_done ? idle + (int)step : 0;
    }
    seto_window_stop();
    seto_multimedia_shutdown();
    timeEndPeriod(1);
    report("run ended");
    return 0;
}
