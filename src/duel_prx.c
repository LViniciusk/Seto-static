/* S9.9: load and run the duel subsystem PRX corpora.
 *
 * The first scripted duel asks ModuleMgr for modduel_mgr (LBN 0x5c20). It
 * imports libduel_eng, libduel_draw, libdeck_swap and libtutorial, so those
 * five images load together. All of them fit in the low window below VRAM, so
 * they stay resident next to story/field and the runtime heap. */

#include <psprecomp/cpu.h>
#include <psprecomp/mem.h>
#include <psprecomp/hle.h>
#include <psprecomp/dispatch.h>
#include <psprecomp/recomp_rt.h>

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EBOOT_BASE      0x08000000u
#define EBOOT_SIZE      0x00640000u
#define EBOOT_INFO      (EBOOT_BASE + 0x0006B81Cu)
#define MAX_EXPORTS     1600
#ifdef HAVE_TF3ENG_CORPUS
#include "tf3_bridge.h"
#define MAX_DEXPORTS    1600
#else
#define MAX_DEXPORTS    256
#endif

typedef struct { uint32_t nid, addr, gp; } duel_export_t;

typedef struct {
    const char *name;
    const char *lbn;       /* "sce_lbn0x..." token the game passes */
    const char *prx;       /* basename alias, when the game asks by name */
    uint32_t base, end_max, uid, entry, gp, filesz, memsz;
    void (*reg)(void);
    int loaded, nexports;
    uint8_t *image;
    duel_export_t ex[MAX_DEXPORTS];
} duel_mod_t;

void duelmgr_psp_recomp_register(void);
#ifdef HAVE_TF3ENG_CORPUS
void tf3eng_psp_recomp_register(void);
#endif
#if !defined(HAVE_TF3ENG_CORPUS) || defined(HAVE_DUELENG_CORPUS)
void dueleng_psp_recomp_register(void);
#endif
void dueldraw_psp_recomp_register(void);
void duelswap_psp_recomp_register(void);
void dueltut_psp_recomp_register(void);
void duelrec_psp_recomp_register(void);

static duel_mod_t g_duel[] = {
    { "modduel_mgr", "sce_lbn0x5c20", "libduel_mgr.prx",
      0x02200000u, 0x02E00000u, 0x00080010u, 0, 0, 0, 0, duelmgr_psp_recomp_register, 0, 0, NULL, {{0,0,0}} },
#ifdef HAVE_TF3ENG_CORPUS
    { "modduel_eng", "sce_lbn0x5940", "libduel_eng.prx",
      0x03600000u, 0x03860000u, 0x00080011u, 0, 0, 0, 0, tf3eng_psp_recomp_register, 0, 0, NULL, {{0,0,0}} },
#else
    { "modduel_eng", "sce_lbn0x5940", "libduel_eng.prx",
      0x02E00000u, 0x03000000u, 0x00080011u, 0, 0, 0, 0, dueleng_psp_recomp_register, 0, 0, NULL, {{0,0,0}} },
#endif
    { "modduel_draw", "sce_lbn0x5800", "libduel_draw.prx",
      0x03000000u, 0x03500000u, 0x00080012u, 0, 0, 0, 0, dueldraw_psp_recomp_register, 0, 0, NULL, {{0,0,0}} },
    { "moddeck_swap", "sce_lbn0x5750", "libdeck_swap.prx",
      0x03500000u, 0x03530000u, 0x00080013u, 0, 0, 0, 0, duelswap_psp_recomp_register, 0, 0, NULL, {{0,0,0}} },
    { "modtutorial", "sce_lbn0x5ef0", "libtutorial.prx",
      0x03530000u, 0x03550000u, 0x00080014u, 0, 0, 0, 0, dueltut_psp_recomp_register, 0, 0, NULL, {{0,0,0}} },
    { "modduelrecord", "sce_lbn0x57f0", "libduelrecord.prx",
      0x03550000u, 0x03600000u, 0x00080015u, 0, 0, 0, 0, duelrec_psp_recomp_register, 0, 0, NULL, {{0,0,0}} },
};
#define NDUEL ((int)(sizeof g_duel / sizeof g_duel[0]))

#if defined(HAVE_TF3ENG_CORPUS) && defined(HAVE_DUELENG_CORPUS)
static duel_mod_t g_duel_tf1_eng = {
    "modduel_eng", "sce_lbn0x5940", "libduel_eng.prx",
    0x02E00000u, 0x03000000u, 0x00080011u, 0, 0, 0, 0, dueleng_psp_recomp_register, 0, 0, NULL, {{0,0,0}}
};
#endif
static int g_duel_tutorial_mode;

static struct { uint32_t nid, addr; } g_eboot_exports[MAX_EXPORTS];
static int g_eboot_nexports;
static uint32_t g_eboot_gp;
static int g_duel_ready;
static _Atomic int g_duel_mgr_start_requested;

int psp_duel_manager_start_requested(void) {
    return atomic_load(&g_duel_mgr_start_requested);
}

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static int cmp_eboot(const void *a, const void *b) {
    uint32_t x = ((const duel_export_t *)a)->nid;
    uint32_t y = ((const duel_export_t *)b)->nid;
    return x < y ? -1 : x > y;
}
static int cmp_mod(const void *a, const void *b) { return cmp_eboot(a, b); }

static uint32_t eboot_export(uint32_t nid) {
    int lo = 0, hi = g_eboot_nexports - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (g_eboot_exports[mid].nid == nid) return g_eboot_exports[mid].addr;
        if (g_eboot_exports[mid].nid < nid) lo = mid + 1; else hi = mid - 1;
    }
    return 0;
}

/* libehsys only: the same table modstory/modfield resolve from the EBOOT. */
static int build_eboot_exports(uint32_t info) {
    if (info != EBOOT_INFO) {
        fprintf(stderr, "duel PRX: EBOOT is not relocated to 0x08000000\n");
        return -1;
    }
    char name[29];
    for (int i = 0; i < 28; i++) name[i] = (char)psp_read8(info + 4u + (uint32_t)i);
    name[28] = 0;
    if (strcmp(name, "modehsys") != 0) {
        fprintf(stderr, "duel PRX: EBOOT module is '%s'\n", name);
        return -1;
    }
    g_eboot_gp = psp_read32(info + 0x20);
    uint32_t ent = psp_read32(info + 0x24);
    uint32_t ent_end = psp_read32(info + 0x28);
    g_eboot_nexports = 0;
    for (uint32_t e = ent; e + 0x10 <= ent_end; ) {
        uint32_t namep = psp_read32(e);
        uint8_t ent_len = psp_read8(e + 8);
        uint8_t nvar = psp_read8(e + 9);
        uint16_t nfunc = psp_read16(e + 10);
        uint32_t table = psp_read32(e + 12);
        uint32_t stride = ent_len ? (uint32_t)ent_len * 4u : 0x10u;
        if (stride < 0x10) break;
        char lib[32] = "";
        if (namep) {
            for (int i = 0; i < 31; i++) {
                char c = (char)psp_read8(namep + (uint32_t)i);
                if (!c) break;
                lib[i] = c;
            }
        }
        if (strcmp(lib, "libehsys") == 0) {
            uint32_t total = (uint32_t)nfunc + nvar;
            for (uint16_t i = 0; i < nfunc; i++) {
                uint32_t nid = psp_read32(table + (uint32_t)i * 4u);
                uint32_t addr = psp_read32(table + (total + i) * 4u);
                if (addr < EBOOT_BASE || addr >= EBOOT_BASE + EBOOT_SIZE ||
                    !psp_lookup(addr)) continue;
                if (g_eboot_nexports < MAX_EXPORTS) {
                    g_eboot_exports[g_eboot_nexports].nid = nid;
                    g_eboot_exports[g_eboot_nexports].addr = addr;
                    g_eboot_nexports++;
                }
            }
        }
        e += stride;
    }
    if (!g_eboot_nexports) {
        fprintf(stderr, "duel PRX: no EBOOT exports resolved\n");
        return -1;
    }
    qsort(g_eboot_exports, (size_t)g_eboot_nexports, sizeof g_eboot_exports[0], cmp_eboot);
    return 0;
}

static int load_mod(duel_mod_t *m, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "duel PRX: cannot open %s\n", path); return -1; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    uint8_t *d = n > 52 ? (uint8_t *)malloc((size_t)n) : NULL;
    if (!d || fread(d, 1, (size_t)n, f) != (size_t)n) {
        fclose(f); free(d);
        fprintf(stderr, "duel PRX: cannot read %s\n", path);
        return -1;
    }
    fclose(f);
    if (memcmp(d, "\x7f" "ELF", 4) != 0 || d[4] != 1 || d[5] != 1 ||
        rd16(d + 18) != 8) {
        free(d); fprintf(stderr, "duel PRX: %s is not ELF32 MIPS LE\n", m->name);
        return -1;
    }
    uint32_t phoff = rd32(d + 28);
    uint16_t phentsize = rd16(d + 42), phnum = rd16(d + 44);
    if (phentsize < 32 || phoff + (uint32_t)phnum * phentsize > (uint32_t)n) {
        free(d); fprintf(stderr, "duel PRX: %s has bad program headers\n", m->name);
        return -1;
    }
    uint32_t off = 0, vaddr = 0, filesz = 0, memsz = 0;
    int ns = 0;
    for (uint16_t i = 0; i < phnum; i++) {
        const uint8_t *p = d + phoff + (uint32_t)i * phentsize;
        if (rd32(p) != 1) continue;
        if (ns++) { free(d); fprintf(stderr, "duel PRX: %s has two PT_LOAD\n", m->name); return -1; }
        off = rd32(p + 4);
        vaddr = rd32(p + 8);
        filesz = rd32(p + 16);
        memsz = rd32(p + 20);
    }
    if (ns != 1 || vaddr != m->base) {
        free(d);
        fprintf(stderr, "duel PRX: %s is not one segment at 0x%08X\n", m->name, m->base);
        return -1;
    }
    if (filesz > memsz || (uint64_t)off + filesz > (uint64_t)n ||
        (uint64_t)vaddr + memsz > m->end_max) {
        free(d); fprintf(stderr, "duel PRX: %s leaves its window\n", m->name);
        return -1;
    }
    if (psp_mem_map_extra(vaddr, memsz) != 0 ||
        psp_mem_write_block(vaddr, d + off, filesz) != 0) {
        free(d); fprintf(stderr, "duel PRX: cannot place %s\n", m->name);
        return -1;
    }
    if (psp_read32(vaddr) != rd32(d + off) ||
        (memsz > filesz && psp_read8(vaddr + filesz) != 0)) {
        free(d); fprintf(stderr, "duel PRX: %s read-back failed\n", m->name);
        return -1;
    }
    uint32_t modoff = rd32(d + phoff + 12) & 0x7FFFFFFFu;
    if (modoff < off || modoff >= off + filesz) {
        free(d); fprintf(stderr, "duel PRX: %s module info outside segment\n", m->name);
        return -1;
    }
    uint32_t info = vaddr + (modoff - off);
    char modname[29];
    for (int i = 0; i < 28; i++) modname[i] = (char)psp_read8(info + 4u + (uint32_t)i);
    modname[28] = 0;
    if (strcmp(modname, m->name) != 0) {
        free(d);
        fprintf(stderr, "duel PRX: module is '%s', expected %s\n", modname, m->name);
        return -1;
    }
    m->gp = psp_read32(info + 0x20);
    m->entry = rd32(d + 24);
    m->nexports = 0;
    for (uint32_t e = psp_read32(info + 0x24), end = psp_read32(info + 0x28);
         e + 0x10 <= end; ) {
        uint8_t ent_len = psp_read8(e + 8);
        uint8_t nvar = psp_read8(e + 9);
        uint16_t nfunc = psp_read16(e + 10);
        uint32_t table = psp_read32(e + 12);
        uint32_t stride = ent_len ? (uint32_t)ent_len * 4u : 0x10u;
        if (stride < 0x10) break;
        uint32_t total = (uint32_t)nfunc + nvar;
        for (uint16_t i = 0; i < nfunc && m->nexports < MAX_DEXPORTS; i++) {
            m->ex[m->nexports].nid = psp_read32(table + (uint32_t)i * 4u);
            m->ex[m->nexports].addr = psp_read32(table + (total + i) * 4u);
            m->ex[m->nexports].gp = m->gp;
            m->nexports++;
        }
        e += stride;
    }
    qsort(m->ex, (size_t)m->nexports, sizeof m->ex[0], cmp_mod);
    if (m->base == 0x03600000u)
        psp_dispatch_clear_range(0x03600000u, 0x04000000u);
    m->reg();
    psp_dispatch_module_gp(m->base, m->base + memsz, m->gp);
    m->filesz = filesz;
    m->memsz = memsz;
    free(m->image);
    m->image = (uint8_t *)malloc(filesz ? filesz : 1u);
    if (m->image && filesz) memcpy(m->image, d + off, filesz);
    m->loaded = 1;
    free(d);
    fprintf(stderr, "duel PRX %s 0x%08X..0x%08X (%u bytes .bss), gp 0x%08X, "
            "%d exports, entry 0x%08X\n", m->name, m->base, m->base + memsz,
            memsz - filesz, m->gp, m->nexports, m->entry);
    return 0;
}

static void restore_duel_mod_image(duel_mod_t *m) {
    if (!m->loaded || !m->image || !m->memsz) return;
    psp_mem_map_extra(m->base, m->memsz);
    psp_mem_write_block(m->base, m->image, m->filesz);
    if (m->base == 0x03600000u)
        psp_dispatch_clear_range(0x03600000u, 0x04000000u);
    m->reg();
    psp_dispatch_module_gp(m->base, m->base + m->memsz, m->gp);
}

static void duel_check_scene_mode(uint32_t nid, uint32_t caller_ra, uint32_t ret_v0) {
#if defined(HAVE_TF3ENG_CORPUS) && defined(HAVE_DUELENG_CORPUS)
    if (nid != 0x60ABAEE7u || caller_ra != 0x0220005Cu || !g_duel_tf1_eng.loaded) return;
    const int is_tut = ((ret_v0 & 0xFFFFu) == 1u) ? 1 : 0;
    g_duel_tutorial_mode = is_tut;
    if (is_tut) {
        restore_duel_mod_image(&g_duel_tf1_eng);
    } else {
        restore_duel_mod_image(&g_duel[1]);
        duel_tf3_on_duel_load();
    }
#else
    (void)nid; (void)caller_ra; (void)ret_v0;
#endif
}

int psp_duel_images_load(const char *const *paths, uint32_t eboot_info) {
    if (build_eboot_exports(eboot_info ? eboot_info : EBOOT_INFO) != 0) return -1;
    for (int i = 0; i < NDUEL; i++) {
        const char *p = paths[i];
        if (!p) continue;
#ifdef HAVE_TF3ENG_CORPUS
#ifdef HAVE_DUELENG_CORPUS
        if (i == 1) {
            if (load_mod(&g_duel_tf1_eng, p) != 0) return -1;
        }
#endif
        char tf3_alt[512];
        if (i == 1) {
            const char *tok = strstr(p, "duel-eng-reloc.elf");
            if (tok) {
                size_t dir_len = (size_t)(tok - p);
                if (dir_len + sizeof("duel-tf3eng-reloc.elf") <= sizeof(tf3_alt)) {
                    memcpy(tf3_alt, p, dir_len);
                    strcpy(tf3_alt + dir_len, "duel-tf3eng-reloc.elf");
                    FILE *probe = fopen(tf3_alt, "rb");
                    if (probe) { fclose(probe); p = tf3_alt; }
                }
            }
        }
#endif
        if (load_mod(&g_duel[i], p) != 0) return -1;
    }
#ifdef HAVE_TF3ENG_CORPUS
    duel_tf3_on_duel_load();
#endif
    g_duel_ready = 1;
    return 0;
}

static duel_mod_t *find_path(const char *path) {
    const char *slash = strrchr(path, '/');
    const char *base = slash ? slash + 1 : path;
    for (int i = 0; i < NDUEL; i++) {
        duel_mod_t *m = &g_duel[i];
        if (!m->loaded) continue;
        if (strstr(path, m->lbn) || strcmp(base, m->prx) == 0) return m;
    }
    return NULL;
}

int psp_duel_load_uid(const char *path, uint32_t *uid) {
    duel_mod_t *m = find_path(path);
    if (!m) return 0;
    restore_duel_mod_image(m);
#if defined(HAVE_TF3ENG_CORPUS) && defined(HAVE_DUELENG_CORPUS)
    if (m == &g_duel[1])
        restore_duel_mod_image(&g_duel_tf1_eng);
    if (m == &g_duel[4]) {
        g_duel_tutorial_mode = 1;
        restore_duel_mod_image(&g_duel_tf1_eng);
    }
#endif
    *uid = m->uid;
    return 1;
}

int psp_duel_start(uint32_t uid, uint32_t status_ptr) {
    if (!g_duel_ready) return 0;
    for (int i = 0; i < NDUEL; i++) {
        duel_mod_t *m = &g_duel[i];
        if (!m->loaded || m->uid != uid) continue;
#if defined(HAVE_TF3ENG_CORPUS) && defined(HAVE_DUELENG_CORPUS)
        if (m == &g_duel[1] && g_duel_tutorial_mode && g_duel_tf1_eng.loaded) {
            m = &g_duel_tf1_eng;
        }
#endif
        uint32_t caller_gp = psp_cpu.r[PSP_REG_GP];
        psp_cpu.r[PSP_REG_A0] = psp_arg(1);
        psp_cpu.r[PSP_REG_A1] = psp_arg(2);
        if (m->entry < 0x80000000u) {
            static int trace_mod = -1;
            if (trace_mod < 0) trace_mod = getenv("SETO_TRACE_MODULE") != NULL ? 1 : 0;
            psp_cpu.r[PSP_REG_GP] = m->gp;
            if (m == &g_duel[0]) {
                atomic_store(&g_duel_mgr_start_requested, 1);
                if (trace_mod)
                    fprintf(stderr, "module: start '%s' entry=0x%08X\n",
                            m->name, m->entry);
            }
            psp_call_from_host(m->entry);
            if (trace_mod)
                fprintf(stderr, "module: start '%s' entry=0x%08X returned 0x%08X\n",
                        m->name, m->entry, psp_cpu.r[PSP_REG_V0]);
        }
        psp_cpu.r[PSP_REG_GP] = caller_gp;
        if (status_ptr) psp_write32(status_ptr, 0);
        psp_ret(0);
        return 1;
    }
    return 0;
}

uint32_t psp_duel_export(uint32_t nid, uint32_t *gp_out) {
    for (int i = 0; i < NDUEL; i++) {
#if defined(HAVE_TF3ENG_CORPUS) && defined(HAVE_DUELENG_CORPUS)
        duel_mod_t *m = (i == 1 && g_duel_tutorial_mode && g_duel_tf1_eng.loaded)
                        ? &g_duel_tf1_eng : &g_duel[i];
#else
        duel_mod_t *m = &g_duel[i];
#endif
        if (!m->loaded) continue;
        int lo = 0, hi = m->nexports - 1;
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            if (m->ex[mid].nid == nid) {
                if (gp_out) *gp_out = m->ex[mid].gp;
                return m->ex[mid].addr;
            }
            if (m->ex[mid].nid < nid) lo = mid + 1; else hi = mid - 1;
        }
    }
    return 0;
}

uint32_t duel_eboot_export_addr(uint32_t nid, uint32_t *gp_out) {
    uint32_t addr = eboot_export(nid);
    if (addr && gp_out) *gp_out = g_eboot_gp;
    return addr;
}

void duel_import_forward(uint32_t nid) {
    if (g_tf3_post_deck_trace) {
        extern uint64_t psp_display_vblanks(void);
        static uint32_t seen[256];
        static unsigned nseen;
        static uint32_t vb_nid[128];
        static unsigned vb_cnt[128];
        static unsigned vb_n;
        static uint64_t vb_thresh = (uint64_t)-1;
        if (vb_thresh == (uint64_t)-1) {
            const char *e = getenv("SETO_TRACE_DUEL_VB");
            vb_thresh = e ? (uint64_t)strtoull(e, NULL, 10) : (uint64_t)-2;
        }
        uint32_t trace_a0 = psp_cpu.r[PSP_REG_A0], trace_a1 = psp_cpu.r[PSP_REG_A1];
        uint32_t trace_a2 = psp_cpu.r[PSP_REG_A2], trace_a3 = psp_cpu.r[PSP_REG_A3];
        const uint32_t trace_ra = psp_cpu.r[PSP_REG_RA];
        int handled = 0;
#ifdef HAVE_TF3ENG_CORPUS
        if (!g_duel_tutorial_mode)
            handled = duel_tf3_handle_export(nid);
#endif
        uint32_t gp = 0;
        uint32_t addr = handled ? 0u : psp_duel_export(nid, &gp);
        int is_duel_mod = (handled || addr != 0u);
        if (!handled) {
            if (!addr) {
                addr = eboot_export(nid);
                gp = g_eboot_gp;
            }
            if (!addr) {
                static unsigned logged;
                if (logged++ < 20)
                    fprintf(stderr, "duel PRX: import NID 0x%08X has no export\n", nid);
                psp_ret(0x80020001u);
                return;
            }
            uint32_t caller_gp = psp_cpu.r[PSP_REG_GP];
            psp_cpu.r[PSP_REG_GP] = gp;
#ifdef HAVE_TF3ENG_CORPUS
            static int b25_probe_done;
            static int test_b25 = -1;
            if (test_b25 < 0) test_b25 = getenv("SETO_TEST_B25") != NULL ? 1 : 0;
            if (!b25_probe_done && addr == 0x030136A0u && trace_a0 == 24u &&
                test_b25) {
                b25_probe_done = 1;
                const uint32_t kind = psp_read32(0x03848518u);
                const uint32_t count = psp_read32(0x03848520u);
                const uint32_t card0 = psp_read32(0x03848528u);
                const uint32_t card1 = psp_read32(0x0384852Cu);
                const uint32_t where = psp_read32(0x03848D28u);
                const psp_cpu_state saved = psp_cpu;
                psp_write32(0x03848518u, 8u);
                psp_write32(0x03848520u, 2u);
                psp_write32(0x03848528u, 0x914Fu);
                psp_write32(0x0384852Cu, 0x914Fu);
                psp_write32(0x03848D28u, 0x000B000Bu);
                psp_cpu.r[PSP_REG_A0] = 50u;
                psp_cpu.r[PSP_REG_A1] = 0u;
                psp_cpu.r[PSP_REG_A2] = 8u;
                psp_cpu.r[PSP_REG_A3] = 0u;
                fprintf(stderr, "[B25_FIXTURE] kind=8 n=2 card=0x114F owner=1 zone=0xB view=%u\n",
                        psp_read32(0x03821594u));
                psp_call_from_host(addr);
                fprintf(stderr, "[B25_FIXTURE_DONE] dispatch=%u\n", psp_cpu.r[PSP_REG_V0]);
                fflush(stderr);
                psp_cpu = saved;
                psp_write32(0x03848518u, kind);
                psp_write32(0x03848520u, count);
                psp_write32(0x03848528u, card0);
                psp_write32(0x0384852Cu, card1);
                psp_write32(0x03848D28u, where);
            }
#endif
            psp_call_from_host(addr);
            psp_cpu.r[PSP_REG_GP] = caller_gp;
            duel_check_scene_mode(nid, trace_ra, psp_cpu.r[PSP_REG_V0]);
        }
#ifdef HAVE_TF3ENG_CORPUS
        static int trace_selraw = -1;
        if (trace_selraw < 0) trace_selraw = getenv("SETO_TRACE_SELRAW") != NULL ? 1 : 0;
        if (nid == 0x9992B977u && trace_ra == 0x03015F84u && trace_a0 < 512u &&
            trace_selraw) {
            const uint32_t returned = psp_cpu.r[PSP_REG_V0];
            const uint32_t entry = 0x03848528u + 4u * trace_a0;
            const uint32_t raw = psp_read16(entry);
            const uint32_t where = psp_read16(0x03848D28u + 2u * trace_a0);
            fprintf(stderr, "[PICKDRAW] vb=%llu turn=%u phase=%u view=%u kind=%u i=%u source=0x%04X owner=%u zone=0x%X returned=0x%08X\n",
                    (unsigned long long)psp_display_vblanks(), psp_read32(0x0384BDE0u),
                    psp_read32(0x0384BDE8u), psp_read32(0x03821594u),
                    psp_read32(0x03848518u), trace_a0, raw, (raw >> 15) & 1u,
                    where & 0x7Fu, returned);
        }
#endif
        /* After the real call: the earlier dump ran before duel_finish and
         * always saw result == 0. */
        /* Opponent hand leak: log what the draw asks about a hand slot. */
        if (g_tf3_post_deck_trace && (trace_a1 == 0x0Bu || trace_a1 == 0x11u) && trace_a0 == 1u) {
            static unsigned hand_log;
            if (hand_log++ < 60u)
                fprintf(stderr, "[HAND] vb=%llu nid=0x%08X a0=%u a1=0x%X a2=0x%X -> 0x%X\n",
                        (unsigned long long)psp_display_vblanks(), nid, trace_a0, trace_a1,
                        trace_a2, psp_cpu.r[PSP_REG_V0]);
        }
        if (nid == 0xAFF0389Du && g_tf3_post_deck_trace) {
            extern uint32_t tf3_bridge_gs2_base(void);
            const uint32_t gs2 = tf3_bridge_gs2_base();
            fprintf(stderr, "[DUEL_RESULTS] vb=%llu ret=%u gs2=0x%X data=",
                    (unsigned long long)psp_display_vblanks(), psp_cpu.r[PSP_REG_V0], gs2);
            if (gs2 >= 0x08000000u && gs2 < 0x0C000000u) {
                for (uint32_t off = 0; off < 0x140u; off += 4u)
                    fprintf(stderr, " %08X", psp_read32(gs2 + off));
                fprintf(stderr, " extra: +0x358=%08X +0x394=%08X",
                        psp_read32(gs2 + 0x358u), psp_read32(gs2 + 0x394u));
            } else {
                fprintf(stderr, " (no base)");
            }
            fputc('\n', stderr);
            fflush(stderr);
        }
        if (g_tf3_post_deck_trace) {
            if (nid == 0xC6A21E9Fu || nid == 0xC86C9FF5u || nid == 0xF14C9EFDu || nid == 0xACB25738u) {
                static uint32_t last_discard_count = ~0u;
                const uint32_t result = psp_cpu.r[PSP_REG_V0];
                if ((nid != 0xC86C9FF5u || result != last_discard_count) &&
                    (nid != 0xACB25738u || trace_a0 == 11u || trace_a0 == 49u))
                    fprintf(stderr, "[ABI_UI] vb=%llu nid=0x%08X caller=0x%08X args=[0x%X,0x%X,0x%X,0x%X] -> 0x%X\n",
                            (unsigned long long)psp_display_vblanks(), nid, trace_ra,
                            trace_a0, trace_a1, trace_a2, trace_a3, result);
                if (nid == 0xC86C9FF5u)
                    last_discard_count = result;
            }
            static unsigned n_f51, n_e5z13, n_btn, n_ff;
            unsigned *cnt = NULL;
            if (nid == 0xF51DDF37u) cnt = &n_f51;
            else if (nid == 0xE5F090D9u && trace_a1 == 13u) cnt = &n_e5z13;
            else if (nid == 0x8171F765u) cnt = &n_btn;
            else if (nid == 0x2F061354u || nid == 0x32CB2B73u) cnt = &n_ff;
            if (cnt && *cnt < 500u) {
                (*cnt)++;
                fprintf(stderr, "[ENGTRACE vb=%llu] nid=0x%08X a0=0x%X a1=0x%X a2=0x%X a3=0x%X ra=0x%X -> v0=0x%X state=%u active=%u view=%u turn=%u phase=%u\n",
                        (unsigned long long)psp_display_vblanks(), nid,
                        trace_a0, trace_a1, trace_a2, trace_a3, trace_ra,
                        psp_cpu.r[PSP_REG_V0],
                        psp_read32(0x0384BE4Cu), psp_read32(0x0384BE54u),
                        psp_read32(0x03821594u), psp_read32(0x0384BDD8u),
                        psp_read32(0x0384BDE8u));
                fflush(stderr);
            }
        }
        if (is_duel_mod) {
            int found = 0;
            for (unsigned i = 0; i < nseen; i++) {
                if (seen[i] == nid) { found = 1; break; }
            }
            if (!found && nseen < 256u) {
                seen[nseen++] = nid;
                fprintf(stderr, "[POST_DECK] duel_exp #%u (%s) nid=0x%08X a0=0x%08X a1=0x%08X -> v0=0x%08X\n",
                        nseen, handled ? "tf3" : "duel", nid, trace_a0, trace_a1,
                        psp_cpu.r[PSP_REG_V0]);
                fflush(stderr);
            }
            if (vb_thresh != (uint64_t)-2 && psp_display_vblanks() >= vb_thresh) {
                static uint64_t last_mgr_key = ~0ULL;
                static int dumped_snap = 0;
                uint16_t mgr_st = psp_read16(0x02D10000u + 31964u);
                uint32_t mgr_sub = psp_read32(0x02D10000u + 32392u);
                uint32_t tf3_st = psp_read32(0x0384BE0Cu);
                uint32_t tf3_s14 = psp_read32(0x0384BE14u);
                uint32_t tf3_s10 = psp_read32(0x0384BE10u);
                uint32_t tf3_ph = psp_read32(0x0384BDF4u);
                uint32_t tf3_trn = psp_read32(0x0384BDF0u);
                uint32_t tf3_cur = psp_read32(0x03821594u);
                uint64_t mgr_key = ((uint64_t)mgr_st << 48) | ((uint64_t)(mgr_sub & 0xFFu) << 40) |
                                   ((uint64_t)(tf3_st & 0xFFu) << 32) | ((uint64_t)(tf3_s14 & 0xFFu) << 24) |
                                   ((uint64_t)(tf3_s10 & 0xFFu) << 16) | ((uint64_t)(tf3_ph & 0xFFu) << 8) |
                                   ((uint64_t)(tf3_trn & 0xFu) << 4) | (tf3_cur & 0xFu);
                if (mgr_key != last_mgr_key) {
                    last_mgr_key = mgr_key;
                    fprintf(stderr, "[MGR_ST vb=%llu] state=%u sub=%u tf3_st=%u sub14=%u sub10=%u ph=%u turn=%u cur=%u\n",
                            (unsigned long long)psp_display_vblanks(), mgr_st, mgr_sub,
                            tf3_st, tf3_s14, tf3_s10, tf3_ph, tf3_trn, tf3_cur);
                    fflush(stderr);
                }
                if (nid == 0x04F712B8u && trace_a0 >= 0x08000000u && trace_a0 < 0x0C000000u) {
                    static unsigned evt_cnt;
                    uint32_t w0 = psp_read32(trace_a0 + 0u);
                    if (w0 != 1u && evt_cnt < 80u) {
                        evt_cnt++;
                        fprintf(stderr, "[EVT_POP %llu] w=[%u, 0x%X, 0x%X, 0x%X] mgr=%u/%u\n",
                                (unsigned long long)psp_display_vblanks(),
                                w0, psp_read32(trace_a0 + 4u), psp_read32(trace_a0 + 8u), psp_read32(trace_a0 + 12u),
                                mgr_st, mgr_sub);
                        fflush(stderr);
                    }
                }
                if (nid == 0xACB25738u && trace_a0 != 1u) {
                    static unsigned draw_evt_cnt;
                    static int trace_vis = -1;
                    if (trace_vis < 0) trace_vis = getenv("SETO_ABI_TRACE_VISUAL") != NULL ? 1 : 0;
                    if (draw_evt_cnt < 80u || trace_vis) {
                        draw_evt_cnt++;
                        fprintf(stderr, "[DRAW_EVT %llu] a0=%u a1=0x%X a2=0x%X a3=0x%X -> v0=0x%X\n",
                                (unsigned long long)psp_display_vblanks(),
                                trace_a0, trace_a1, trace_a2, trace_a3,
                                psp_cpu.r[PSP_REG_V0]);
                        fflush(stderr);
                    }
                }
                if (!dumped_snap && psp_display_vblanks() >= 5200u) {
                    dumped_snap = 1;
                    fprintf(stderr, "[DUEL_SNAP vb=%llu] mgr=%u/%u tf3: st=%u sub14=%u sub10=%u ph=%u turn=%u cur=%u ctrl=[%u,%u,%u,%u] cmd=%u/%u 30048=%u 30104=%u bdc0=0x%X/0x%X be00=%u bdd4=%u\n",
                            (unsigned long long)psp_display_vblanks(), mgr_st, psp_read32(0x02D10000u + 32392u),
                            psp_read32(0x0384BE0Cu), psp_read32(0x0384BE14u), psp_read32(0x0384BE10u),
                            psp_read32(0x0384BDF4u), psp_read32(0x0384BDF0u), psp_read32(0x03821594u),
                            psp_read32(0x03821598u), psp_read32(0x0382159Cu), psp_read32(0x038215A0u), psp_read32(0x038215A4u),
                            psp_read32(0x03848500u), psp_read32(0x03848504u),
                            psp_read32(0x03847560u), psp_read32(0x03847598u),
                            psp_read32(0x0384BDC0u), psp_read32(0x0384BDC4u),
                            psp_read32(0x0384BE00u), psp_read32(0x0384BDD4u));
                    fprintf(stderr, "[DUEL_SNAP_MEM] 885E0882=[%u,%u,%u,%u, %08X,%08X] 88B6C7F0+280=[%02X %02X %02X %02X] +400=[%02X %02X %02X %02X] s2+288=[%u,%u,%u,%u] s2+296=[%u,%u,%u,%u]\n",
                            psp_read32(0x08272F80u), psp_read32(0x08272F84u), psp_read32(0x08272F88u), psp_read32(0x08272F8Cu),
                            psp_read32(0x08272F90u), psp_read32(0x08272F94u),
                            psp_read8(0x08272840u + 280), psp_read8(0x08272840u + 281), psp_read8(0x08272840u + 282), psp_read8(0x08272840u + 283),
                            psp_read8(0x08272840u + 400), psp_read8(0x08272840u + 401), psp_read8(0x08272840u + 402), psp_read8(0x08272840u + 403),
                            psp_read16(0x08272840u + 288), psp_read16(0x08272840u + 392 + 288),
                            psp_read16(0x08272840u + 784 + 288), psp_read16(0x08272840u + 1176 + 288),
                            psp_read16(0x08272840u + 296), psp_read16(0x08272840u + 392 + 296),
                            psp_read16(0x08272840u + 784 + 296), psp_read16(0x08272840u + 1176 + 296));
                    fprintf(stderr, "[DRAW_SNAP] ci=[0x%X,0x%X,0x%X,0x%X, 0x%X,0x%X,0x%X,0x%X,0x%X] dd_mode=%u st_31084=%u st_30600=%u fade=%u/%u dd_ci=[mode=%u cid=%u disp=%u] tbl=0x%X\n",
                            psp_read32(0x083E5F2Cu), psp_read32(0x083E5F30u), psp_read32(0x083E5F34u), psp_read32(0x083E5F38u),
                            psp_read32(0x083E5F3Cu), psp_read32(0x083E5F40u), psp_read32(0x083E5F44u), psp_read32(0x083E5F48u), psp_read32(0x083E5F4Cu),
                            psp_read32(0x03453468u), psp_read32(0x0345796Cu), psp_read32(0x03457788u),
                            psp_read16(0x083E6444u), psp_read16(0x083E6446u),
                            psp_read32(0x03439920u), psp_read32(0x0343992Cu), psp_read32(0x03439928u),
                            psp_read32(0x080EAE88u));
                    fflush(stderr);
                }
                static int dumped_snap2 = 0;
                if (!dumped_snap2 && psp_display_vblanks() >= 5800u) {
                    dumped_snap2 = 1;
                    fprintf(stderr, "[DRAW_SNAP2 vb=%llu] dd_mode=%u st_31084=%u st_30600=%u fade=%u/%u menu=[ptr=0x%X,0x%X,0x%X]\n",
                            (unsigned long long)psp_display_vblanks(),
                            psp_read32(0x03453468u), psp_read32(0x0345796Cu), psp_read32(0x03457788u),
                            psp_read16(0x083E6444u), psp_read16(0x083E6446u),
                            psp_read32(0x03457770u), psp_read32(0x03457774u), psp_read32(0x03457778u));
                    fflush(stderr);
                }
                static uint32_t vb_last_v0[128];
                unsigned idx = vb_n;
                for (unsigned i = 0; i < vb_n; i++) {
                    if (vb_nid[i] == nid) { idx = i; break; }
                }
                if (idx == vb_n && vb_n < 128u) {
                    vb_nid[vb_n] = nid; vb_cnt[vb_n] = 0; vb_last_v0[vb_n] = 0xDEADBEEFu; vb_n++;
                }
                if (idx < vb_n && (vb_cnt[idx] < 3u ||
                    ((nid == 0xDD6E593Fu || nid == 0x1725EABBu || nid == 0xAFF0389Du ||
                      nid == 0xBE759EC1u || nid == 0xF109A0CEu || nid == 0x46FF14F7u) &&
                     vb_last_v0[idx] != psp_cpu.r[PSP_REG_V0] && vb_cnt[idx] < 20u))) {
                    vb_cnt[idx]++;
                    vb_last_v0[idx] = psp_cpu.r[PSP_REG_V0];
                    fprintf(stderr, "[DUEL_VB %llu] (%s) nid=0x%08X a0=0x%08X a1=0x%08X -> v0=0x%08X\n",
                            (unsigned long long)psp_display_vblanks(), handled ? "tf3" : "duel",
                            nid, trace_a0, trace_a1, psp_cpu.r[PSP_REG_V0]);
                    fflush(stderr);
                }
            }
        } else if (vb_thresh != (uint64_t)-2 && psp_display_vblanks() >= vb_thresh &&
                   (nid == 0x0441C9EFu || nid == 0xD62744B5u || nid == 0x1CC5878Cu ||
                    nid == 0x450CD6C5u || nid == 0x878EE1D3u)) {
            static unsigned eb_cnt[5];
            int k = (nid == 0x0441C9EFu) ? 0 : (nid == 0xD62744B5u) ? 1 : (nid == 0x1CC5878Cu) ? 2 :
                    (nid == 0x450CD6C5u) ? 3 : 4;
            if (eb_cnt[k] < 6u) {
                eb_cnt[k]++;
                fprintf(stderr, "[EB_VB %llu] nid=0x%08X a0=0x%08X a1=0x%08X -> v0=0x%08X\n",
                        (unsigned long long)psp_display_vblanks(), nid, trace_a0, trace_a1,
                        psp_cpu.r[PSP_REG_V0]);
                fflush(stderr);
            }
        }
        return;
    }
#ifdef HAVE_TF3ENG_CORPUS
    if (!g_duel_tutorial_mode && duel_tf3_handle_export(nid)) return;
#endif
    uint32_t gp = 0;
    uint32_t addr = psp_duel_export(nid, &gp);
    if (!addr) {
        addr = eboot_export(nid);
        gp = g_eboot_gp;
    }
    if (!addr) {
        static unsigned logged;
        if (logged++ < 20)
            fprintf(stderr, "duel PRX: import NID 0x%08X has no export\n", nid);
        psp_ret(0x80020001u);
        return;
    }
    uint32_t caller_gp = psp_cpu.r[PSP_REG_GP];
    const uint32_t caller_ra = psp_cpu.r[PSP_REG_RA];
    psp_cpu.r[PSP_REG_GP] = gp;
    psp_call_from_host(addr);
    psp_cpu.r[PSP_REG_GP] = caller_gp;
    duel_check_scene_mode(nid, caller_ra, psp_cpu.r[PSP_REG_V0]);
}
