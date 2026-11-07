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
void psp_umd_register(void);
void psp_umd_set_iso(const char *path);
void psp_fpl_register(void);
int  psp_font_image_load(const char *path);
int  psp_font_bind(void);
#ifdef HAVE_FONT_CORPUS
void font_psp_recomp_register(void);   /* relocated PRX corpus */
#endif
#ifdef HAVE_TITLE_CORPUS
int  psp_title_image_load(const char *path, uint32_t eboot_info);
#endif

/* The EBOOT's single PT_LOAD has memsz 0x631A80; .bss runs to there. */
#define MODULE_SPAN 0x00640000u
#define DISPATCH_BUDGET 5000000u

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
 * instead, so a hung first stage still names itself. */
static DWORD WINAPI bringup_cap(LPVOID unused) {
    (void)unused;
    Sleep(8000);
    report("stopped by the 8 s bring-up cap");
    fflush(stderr);
    _Exit(5);
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
    printf("image   0x%08X..0x%08X  entry 0x%08X  gp 0x%08X  ctors %d\n",
           m.base, m.image_end, m.entry, m.gp, m.nctors);
    printf("dispatch %u functions/labels registered\n", psp_dispatch_count());
    fflush(stdout);

    psp_set_miss_handler(on_miss);
    psp_dispatch_set_budget(DISPATCH_BUDGET, on_budget);
    psp_trace_watch(m.entry, on_entry);
    CreateThread(NULL, 0, bringup_cap, NULL, 0, NULL);

    uint32_t r = psp_module_start(&m);
    printf("module_start returned 0x%08X\n", r);

    /* If module_start returns, keep the scheduler turning for a bounded slice
     * of real time before reporting. */
    for (int i = 0; i < 5000 && !psp_exit_requested(); i++) {
        psp_threadman_yield();
        Sleep(1);
    }
    report("run ended");
    return 0;
}
