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

#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

void psp_recomp_register(void);   /* generated tagforce_register.c */
int  seto_window_start(void);
void seto_window_stop(void);
void psp_umd_register(void);
void psp_umd_set_iso(const char *path);
void psp_fpl_register(void);
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
#define PSP_CTRL_START 0x00000008u
#define PSP_CTRL_UP    0x00000010u
#define PSP_CTRL_DOWN  0x00000040u
#define PSP_CTRL_CROSS 0x00004000u

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
static int menu_row(void) {
    static uint32_t px[480 * 272];
    if (psp_display_read(px) != 0) return -1;
    for (int y = 34; y < 172; y++) {
        const uint32_t c = px[y * 480 + 30];
        const int r = (int)(c & 0xFF), g = (int)((c >> 8) & 0xFF),
                  b = (int)((c >> 16) & 0xFF);
        if (r > 235 && g > 145 && g < 205 && b < 125) return (y - 30) / 28;
    }
    return -1;
}

static void field_press(uint32_t mask) {
    psp_title_walk(mask, 128, 128);
    psp_ctrl_set(mask, 128, 128);
    Sleep(300);
    psp_title_walk(0, 128, 128);
    psp_ctrl_set(0, 128, 128);
    Sleep(120);
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
    /* The title's vblank hook consumes the one-shot; scenes that run without
     * that hook read the direct state, which is held here for 100 ms. */
    psp_title_pad_request(mask);
    psp_ctrl_set(mask, 128, 128);
    Sleep(100);
    psp_ctrl_set(0, 128, 128);
}

/* One press of a state-3 menu key: the vblank hook applies it for exactly one
 * frame. The direct pad path is not used here because it would produce a
 * second edge and the cursor would skip an entry. */
static void menu_step(uint32_t mask) {
    const int c0 = psp_title_scene_cursor();
    psp_title_menu_request(mask);
    for (int i = 0; i < 3000 && psp_title_scene_cursor() == c0; i++)
        Sleep(1);
    psp_title_menu_request(0);
    Sleep(150);
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

static DWORD WINAPI input_thread(LPVOID unused) {
    (void)unused;
    const char *auto_start = getenv("SETO_AUTO_START");
    int sent = 0, held = 0, timed_out = 0;
    int held_up = 0, held_down = 0, held_cross = 0;
    uint32_t menu_mask = 0;
    int menu_cursor = 0, menu_state = 0;
    DWORD menu_deadline = 0;
    DWORD press_ms = 0;
    while (!psp_exit_requested()) {
        /* libtitle accepts START at its PRESS START step: animation 5 owns the
         * frame and its step index (0x126C6FC) is 1. The same index occurs
         * during the loading sequence, but then the animation is not 5. */
#ifdef HAVE_TITLE_CORPUS
        if (auto_start) {
            if (!sent) {
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
                        auto_menu(PSP_CTRL_UP, 1);
                        auto_menu(PSP_CTRL_CROSS, 0);
                        fprintf(stderr, "input: auto menu result state=%d cursor=%d\n",
                                psp_title_scene_state(), psp_title_scene_cursor());
                        for (int i = 0; i < 500 && psp_title_scene_phase() < 3; i++)
                            Sleep(10);
                        fprintf(stderr, "input: auto name phase=%d state=%d\n",
                                psp_title_scene_phase(), psp_title_scene_state());
                        for (int i = 0; i < 100 && psp_title_scene_state() == 4 &&
                                            !box_up(); i++)
                            Sleep(100);
                        fprintf(stderr, "input: auto name prompt=%d phase=%d state=%d\n",
                                box_up(), psp_title_scene_phase(),
                                psp_title_scene_state());
                        if (psp_display_capture("build/s92-after.ppm") == 0)
                            fprintf(stderr, "capture: build/s92-after.ppm\n");
                        fprintf(stderr, "input: auto name advance\n");
                        int last_state = psp_title_scene_state();
                        for (int i = 0; i < 80 && psp_title_scene_state() == 4 &&
                                           !psp_story_started(); i++) {
                            auto_press((i & 1) ? PSP_CTRL_START : PSP_CTRL_CROSS);
                            Sleep(300);
                            const int s = psp_title_scene_state();
                            if (s != last_state) {
                                fprintf(stderr, "input: auto state %d->%d phase=%d story=%d\n",
                                        last_state, s, psp_title_scene_phase(),
                                        psp_story_started());
                                last_state = s;
                            }
                        }
                        fprintf(stderr, "input: auto name dialog state=%d phase=%d cursor=%d story=%d\n",
                                psp_title_scene_state(), psp_title_scene_phase(),
                                psp_title_scene_cursor(), psp_story_started());
                        for (int i = 0; i < 300 && !psp_story_started(); i++) {
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
                            Sleep(1500);
                            fprintf(stderr, "input: auto main menu cursor=%d\n",
                                    psp_title_scene_cursor());
                            if (getenv("SETO_FREE_DUEL")) {
                                menu_step(0x00000040u);
                                psp_display_capture("build/s93-free-menu.ppm");
                                menu_step(0x00004000u);
                                for (int i = 0; i < 15; i++) {
                                    Sleep(1000);
                                    if (i == 5 || i == 10) {
                                        char path[64];
                                        snprintf(path, sizeof path, "build/s93-duel%d.ppm", i);
                                        psp_display_capture(path);
                                    }
                                }
                                fprintf(stderr, "input: auto duel state=%d phase=%d\n",
                                        psp_title_scene_state(), psp_title_scene_phase());
                            } else {
                            auto_press(0x00004000u);
                            for (int i = 0; i < 30 && !psp_story_started(); i++) {
                                auto_press((i & 1) ? 0x00004000u : 0x00000008u);
                                Sleep(400);
                            }
                            fprintf(stderr, "input: auto main menu state=%d phase=%d story=%d\n",
                                    psp_title_scene_state(), psp_title_scene_phase(),
                                    psp_story_started());
                            }
                        }
                        if (psp_display_capture("build/s92-name.ppm") == 0)
                            fprintf(stderr, "capture: build/s92-name.ppm\n");
                        if (psp_story_started()) {
                            for (int i = 0; i < 200 && !psp_field_started(); i++)
                                Sleep(100);
                            fprintf(stderr, "input: auto story advance field=%d\n",
                                    psp_field_started());
                            for (int i = 0; i < 100 && !box_up(); i++)
                                Sleep(100);
                            int guard = 0;
                            for (; guard < 300 && box_up(); guard++) {
                                auto_press(0x00004000u);
                                Sleep(200);
                            }
                            fprintf(stderr, "input: auto advance guard=%d box=%d\n",
                                    guard, box_up());
                            /* The command menu opens after an analog
                             * down-right walk (255,255) followed by CROSS. */
                            int menu = 0, seen_script = 0;
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
                                    Sleep(2800);
                                    psp_title_walk(0, 128, 128);
                                    psp_ctrl_set(0, 128, 128);
                                    Sleep(800);
                                    field_press(0x00004000u);
                                    Sleep(1500);
                                    psp_display_capture("build/s93-menu-open.ppm");
                                    fprintf(stderr, "input: auto menu-open state=%u\n",
                                            psp_read32(0x093CDAFCu));
                                    for (int d = 0; d < 4; d++) {
                                        field_press(0x00000040u);
                                        Sleep(600);
                                    }
                                    psp_display_capture("build/s93-map-sel.ppm");
                                    field_press(0x00004000u);
                                    Sleep(5000);
                                    psp_display_capture("build/s93-map.ppm");
                                    field_press(0x00004000u);
                                    Sleep(4000);
                                    psp_display_capture("build/s93-exit-yes.ppm");
                                    /* Confirm the highlighted destination on
                                     * the world map, then let the travel and
                                     * the story request their modules. */
                                    /* The map cursor sits on a location; the
                                     * D-pad should move it between nodes and
                                     * CROSS opens that location's menu. */
                                    static const struct { uint32_t m; const char *n; }
                                    mdirs[] = {
                                        { 0x00000020u, "right" }, { 0x00000010u, "up" },
                                        { 0x00000080u, "left" },  { 0x00000040u, "down" },
                                    };
                                    for (int d = 0; d < 4; d++) {
                                        auto_press(mdirs[d].m);
                                        Sleep(1500);
                                        char path[64];
                                        snprintf(path, sizeof path, "build/s93-map-%s.ppm",
                                                 mdirs[d].n);
                                        psp_display_capture(path);
                                    }
                                    auto_press(0x00004000u);
                                    Sleep(2500);
                                    psp_display_capture("build/s93-map-cross.ppm");
                                    /* The list can open seconds after the map
                                     * CROSS when that press lands in a fade.
                                     * Wait for it, move the highlight to
                                     * Classroom (row 1), then confirm. */
                                    int list = menu_panel();
                                    for (int t = 0; t < 8 && !list; t++) {
                                        auto_press(0x00004000u);
                                        Sleep(1200);
                                        list = menu_panel();
                                    }
                                    fprintf(stderr, "input: travel list=%d row=%d\n",
                                            list, menu_row());
                                    for (int t = 0; t < 6; t++) {
                                        const int row = menu_row();
                                        if (row == 1) break;
                                        if (row == 0) {
                                            auto_press(0x00000040u);
                                            Sleep(1200);
                                        } else {
                                            Sleep(1000);
                                        }
                                    }
                                    psp_display_capture("build/s93-travel-class.ppm");
                                    fprintf(stderr, "input: travel row=%d\n", menu_row());
                                    /* Confirm until the list closes; if it
                                     * resets to Main Gate, move down again. */
                                    for (int t = 0; t < 8 && menu_panel(); t++) {
                                        auto_press(0x00004000u);
                                        Sleep(2000);
                                        if (menu_panel() && menu_row() == 0) {
                                            auto_press(0x00000040u);
                                            Sleep(1200);
                                        }
                                    }
                                    psp_display_capture("build/s93-travel-confirm.ppm");
                                    fprintf(stderr, "input: travel done list=%d\n",
                                            menu_panel());
                                    for (int k = 0; k < 15; k++) {
                                        auto_press(0x00004000u);
                                        Sleep(1000);
                                        if (k == 5 || k == 10) {
                                            char path[64];
                                            snprintf(path, sizeof path,
                                                     "build/s93-travel%d.ppm", k);
                                            psp_display_capture(path);
                                        }
                                    }
                                    /* Advance the Classroom dialogue only while
                                     * its box is visible, then choose Duel in
                                     * free roam. Brief state-1 gaps separate
                                     * beats of the original script. */
                                    int seen_class_box = 0, quiet = 0, class_done = 0;
                                    for (int k = 0; k < 600 && !class_done &&
                                            !psp_duel_manager_start_requested(); k++) {
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
                                        } else if (seen_class_box && st == 1u) {
                                            if (++quiet >= 30) class_done = 1;
                                            Sleep(100);
                                        } else {
                                            quiet = 0;
                                            Sleep(100);
                                        }
                                    }
                                    fprintf(stderr, "input: classroom done=%d state=%u duel-start=%d\n",
                                            class_done, psp_read32(0x093CDAFCu),
                                            psp_duel_manager_start_requested());
#ifdef HAVE_DUEL_CORPUS
                                    if (class_done && psp_read32(0x093CDAFCu) == 1u) {
                                        psp_title_walk(0, 255, 255);
                                        psp_ctrl_set(0, 255, 255);
                                        Sleep(2800);
                                        psp_title_walk(0, 128, 128);
                                        psp_ctrl_set(0, 128, 128);
                                        Sleep(800);
                                        field_press(PSP_CTRL_CROSS);
                                        Sleep(1500);
                                        psp_display_capture("build/s93-class-duel-menu.ppm");
                                        field_press(PSP_CTRL_DOWN);
                                        Sleep(600);
                                        psp_display_capture("build/s93-class-duel-select.ppm");
                                        field_press(PSP_CTRL_CROSS);
                                        for (int k = 0; k < 600 &&
                                                !psp_duel_manager_start_requested(); k++) {
                                            if (box_up()) {
                                                auto_press(PSP_CTRL_CROSS);
                                                Sleep(800);
                                            } else {
                                                Sleep(100);
                                            }
                                        }
                                        fprintf(stderr, "input: duel start requested=%d field=%u\n",
                                                psp_duel_manager_start_requested(),
                                                psp_read32(0x093CDAFCu));
                                        Sleep(2000);
                                        psp_display_capture("build/s93-first-duel.ppm");
                                    }
#endif
                                    menu = 1;
                                    break;
                                }
                                auto_press(0x00004000u);
                                Sleep(150);
                            }
                            if (menu) {
                                fprintf(stderr, "input: auto menu path done\n");
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
#ifdef HAVE_TITLE_CORPUS
            const int state = psp_title_scene_state();
            if (menu_mask) {
                const int c = psp_title_scene_cursor();
                const int s = psp_title_scene_state();
                if (s != menu_state ||
                    (menu_mask != PSP_CTRL_CROSS && c != menu_cursor) ||
                    GetTickCount() > menu_deadline) {
                    psp_title_menu_request(0);
                    fprintf(stderr, "input: menu 0x%04X done state=%d cursor=%d\n",
                            menu_mask, s, c);
                    menu_mask = 0;
                }
            }
            const int kup = (GetAsyncKeyState(VK_UP) & 0x8000) != 0;
            const int kdown = (GetAsyncKeyState(VK_DOWN) & 0x8000) != 0;
            const int kcross = (GetAsyncKeyState('Z') & 0x8000) != 0;
            if (kup && !held_up) {
                held_up = 1;
                if (state == 3 && !menu_mask) {
                    menu_mask = PSP_CTRL_UP;
                    menu_cursor = psp_title_scene_cursor();
                    menu_state = state;
                    menu_deadline = GetTickCount() + 800;
                    psp_title_menu_request(menu_mask);
                    fprintf(stderr, "input: UP press\n");
                }
            } else if (!kup) held_up = 0;
            if (kdown && !held_down) {
                held_down = 1;
                if (state == 3 && !menu_mask) {
                    menu_mask = PSP_CTRL_DOWN;
                    menu_cursor = psp_title_scene_cursor();
                    menu_state = state;
                    menu_deadline = GetTickCount() + 800;
                    psp_title_menu_request(menu_mask);
                    fprintf(stderr, "input: DOWN press\n");
                }
            } else if (!kdown) held_down = 0;
            if (kcross && !held_cross) {
                held_cross = 1;
                if (state == 3 && !menu_mask) {
                    menu_mask = PSP_CTRL_CROSS;
                    menu_cursor = psp_title_scene_cursor();
                    menu_state = state;
                    menu_deadline = GetTickCount() + 800;
                    psp_title_menu_request(menu_mask);
                    fprintf(stderr, "input: CROSS press\n");
                } else if (state == 4) {
                    psp_ctrl_set(PSP_CTRL_CROSS, 128, 128);
                    Sleep(100);
                    psp_ctrl_set(0, 128, 128);
                    fprintf(stderr, "input: CROSS name press\n");
                }
            } else if (!kcross) held_cross = 0;
#endif
            int down = (GetAsyncKeyState(VK_RETURN) & 0x8000) != 0;
            /* One request per physical press. The title re-issues the pad edge
             * until its own step advances, so a slow frame cannot lose it;
             * release clears the request. */
            if (down && !held) {
                held = 1;
                timed_out = 0;
                press_ms = GetTickCount();
#ifdef HAVE_TITLE_CORPUS
                if (state == 4) {
                    psp_ctrl_set(PSP_CTRL_START, 128, 128);
                    Sleep(100);
                    psp_ctrl_set(0, 128, 128);
                } else {
                    psp_title_start_request(1);
                }
#else
                psp_ctrl_set(PSP_CTRL_START, 128, 128);
#endif
                fprintf(stderr, "input: START press\n");
            } else if (!down && held) {
                held = 0;
                timed_out = 0;
#ifdef HAVE_TITLE_CORPUS
                psp_title_start_request(0);
#else
                psp_ctrl_set(0, 128, 128);
#endif
                fprintf(stderr, "input: START release\n");
            }
#ifdef HAVE_TITLE_CORPUS
            else if (held && !timed_out && !psp_title_start_done() &&
                     GetTickCount() - press_ms > 5000) {
                timed_out = 1;
                psp_title_start_request(0);
                fprintf(stderr, "input: START timeout\n");
            }
#endif
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
    const int free_duel = getenv("SETO_FREE_DUEL") != NULL;

    if (psp_mem_init() != 0) { fprintf(stderr, "memory init failed\n"); return 1; }

    psp_module m;
    if (psp_module_load(elf, MODULE_SPAN, &m) != 0) {
        fprintf(stderr, "usage: %s [decrypted EBOOT .elf] [disc image .iso]\n", argv[0]);
        return 1;
    }
    psp_cpu_reset();
    psp_hle_init();
    psp_umd_set_iso(iso);
    psp_umd_register();
    psp_fpl_register();
    psp_recomp_register();

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
    CreateThread(NULL, 0, input_thread, NULL, 0, NULL);
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
        psp_threadman_yield();
        Sleep(1);
        idle = g_input_done ? idle + 1 : 0;
    }
    seto_window_stop();
    report("run ended");
    return 0;
}
