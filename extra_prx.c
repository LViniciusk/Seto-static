/* S13: load and run the remaining 9 game PRX corpora from TF1.iso.
 *
 *   Main-RAM modules (fit cleanly under HEAP_LO = 0x09700000):
 *     0x09420000  modcardalbum      (libcardalbum.prx,    LBN 0x5690)
 *     0x09430000  modlimitlist      (liblimitlist.prx,    LBN 0x5D30)
 *     0x09440000  modrecipe_viewer  (librecipe_viewer.prx,LBN 0x5D70)
 *     0x09450000  modtutoriallist   (libtutoriallist.prx, LBN 0x5F10)
 *     0x09460000  modselect_card    (libselect_card.prx,  LBN 0x5D90)
 *     0x09480000  modlabo           (liblabo.prx,         LBN 0x5D10)
 *
 *   Dedicated extra windows (between VRAM end 0x04400000 and RAM 0x08000000):
 *     0x05000000  modconv_machine   (libconv_machine.prx, LBN 0x56D0)
 *     0x05700000  modpassword       (libpassword.prx,     LBN 0x5D50)
 *     0x05B00000  module_htmlview   (libhtml_view.prx,    LBN 0x5D00)
 */

#include <psprecomp/cpu.h>
#include <psprecomp/mem.h>
#include <psprecomp/hle.h>
#include <psprecomp/dispatch.h>
#include <psprecomp/recomp_rt.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EBOOT_BASE          0x08000000u
#define EBOOT_SIZE          0x00640000u
#define EBOOT_INFO          (EBOOT_BASE + 0x0006B81Cu)
#define MAX_EXPORTS         1600
#define MAX_MOD_EXPORTS     512
#define NEXTRA              9

typedef struct { uint32_t nid, addr; } extra_export_t;
typedef struct { uint32_t nid, addr, gp; } extra_mod_export_t;

typedef struct {
    const char *name;
    const char *lbn;
    const char *prx;
    const char *slug;
    uint32_t base;
    uint32_t end_max;
    uint32_t uid;
    int map_extra;
    int compiled;
    void (*reg)(void);
    uint32_t gp;
    uint32_t entry;
    int loaded;
} extra_mod_t;

#ifdef HAVE_CARDALBUM_CORPUS
void cardalbum_psp_recomp_register(void);
#else
static void cardalbum_psp_recomp_register(void) {}
#endif
#ifdef HAVE_LIMITLIST_CORPUS
void limitlist_psp_recomp_register(void);
#else
static void limitlist_psp_recomp_register(void) {}
#endif
#ifdef HAVE_RECIPEVIEWER_CORPUS
void recipeviewer_psp_recomp_register(void);
#else
static void recipeviewer_psp_recomp_register(void) {}
#endif
#ifdef HAVE_TUTORIALLIST_CORPUS
void tutoriallist_psp_recomp_register(void);
#else
static void tutoriallist_psp_recomp_register(void) {}
#endif
#ifdef HAVE_SELECTCARD_CORPUS
void selectcard_psp_recomp_register(void);
#else
static void selectcard_psp_recomp_register(void) {}
#endif
#ifdef HAVE_LABO_CORPUS
void labo_psp_recomp_register(void);
#else
static void labo_psp_recomp_register(void) {}
#endif
#ifdef HAVE_CONVMACHINE_CORPUS
void convmachine_psp_recomp_register(void);
#else
static void convmachine_psp_recomp_register(void) {}
#endif
#ifdef HAVE_PASSWORD_CORPUS
void password_psp_recomp_register(void);
#else
static void password_psp_recomp_register(void) {}
#endif
#ifdef HAVE_HTMLVIEW_CORPUS
void htmlview_psp_recomp_register(void);
#else
static void htmlview_psp_recomp_register(void) {}
#endif

static extra_mod_t g_extra[NEXTRA] = {
    { "modcardalbum", "sce_lbn0x5690", "libcardalbum.prx", "cardalbum",
      0x09420000u, 0x09700000u, 0x00080030u, 0,
#ifdef HAVE_CARDALBUM_CORPUS
      1,
#else
      0,
#endif
      cardalbum_psp_recomp_register, 0, 0xFFFFFFFFu, 0 },
    { "modlimitlist", "sce_lbn0x5d30", "liblimitlist.prx", "limitlist",
      0x09430000u, 0x09700000u, 0x00080031u, 0,
#ifdef HAVE_LIMITLIST_CORPUS
      1,
#else
      0,
#endif
      limitlist_psp_recomp_register, 0, 0xFFFFFFFFu, 0 },
    { "modrecipe_viewer", "sce_lbn0x5d70", "librecipe_viewer.prx", "recipeviewer",
      0x09440000u, 0x09700000u, 0x00080032u, 0,
#ifdef HAVE_RECIPEVIEWER_CORPUS
      1,
#else
      0,
#endif
      recipeviewer_psp_recomp_register, 0, 0xFFFFFFFFu, 0 },
    { "modtutoriallist", "sce_lbn0x5f10", "libtutoriallist.prx", "tutoriallist",
      0x09450000u, 0x09700000u, 0x00080033u, 0,
#ifdef HAVE_TUTORIALLIST_CORPUS
      1,
#else
      0,
#endif
      tutoriallist_psp_recomp_register, 0, 0xFFFFFFFFu, 0 },
    { "modselect_card", "sce_lbn0x5d90", "libselect_card.prx", "selectcard",
      0x09460000u, 0x09700000u, 0x00080034u, 0,
#ifdef HAVE_SELECTCARD_CORPUS
      1,
#else
      0,
#endif
      selectcard_psp_recomp_register, 0, 0xFFFFFFFFu, 0 },
    { "modlabo", "sce_lbn0x5d10", "liblabo.prx", "labo",
      0x09480000u, 0x09700000u, 0x00080035u, 0,
#ifdef HAVE_LABO_CORPUS
      1,
#else
      0,
#endif
      labo_psp_recomp_register, 0, 0xFFFFFFFFu, 0 },
    { "modconv_machine", "sce_lbn0x56d0", "libconv_machine.prx", "conv",
      0x05000000u, 0x08000000u, 0x00080036u, 1,
#ifdef HAVE_CONVMACHINE_CORPUS
      1,
#else
      0,
#endif
      convmachine_psp_recomp_register, 0, 0xFFFFFFFFu, 0 },
    { "modpassword", "sce_lbn0x5d50", "libpassword.prx", "password",
      0x05700000u, 0x08000000u, 0x00080037u, 1,
#ifdef HAVE_PASSWORD_CORPUS
      1,
#else
      0,
#endif
      password_psp_recomp_register, 0, 0xFFFFFFFFu, 0 },
    { "module_htmlview", "sce_lbn0x5d00", "libhtml_view.prx", "htmlview",
      0x05B00000u, 0x08000000u, 0x00080038u, 1,
#ifdef HAVE_HTMLVIEW_CORPUS
      1,
#else
      0,
#endif
      htmlview_psp_recomp_register, 0, 0xFFFFFFFFu, 0 },
};

static extra_export_t g_eboot_exports[MAX_EXPORTS];
static int g_eboot_nexports;
static uint32_t g_eboot_gp;

static extra_mod_export_t g_mod_exports[MAX_MOD_EXPORTS];
static int g_mod_nexports;

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static int cmp_export(const void *a, const void *b) {
    uint32_t x = ((const extra_export_t *)a)->nid;
    uint32_t y = ((const extra_export_t *)b)->nid;
    return x < y ? -1 : x > y;
}
static uint32_t eboot_export(uint32_t nid) {
    int lo = 0, hi = g_eboot_nexports - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (g_eboot_exports[mid].nid == nid) return g_eboot_exports[mid].addr;
        if (g_eboot_exports[mid].nid < nid) lo = mid + 1; else hi = mid - 1;
    }
    return 0;
}

static int build_eboot_exports(uint32_t info) {
    if (g_eboot_nexports > 0) return 0;
    if (info != EBOOT_INFO) {
        fprintf(stderr, "extra PRX: EBOOT is not relocated to 0x08000000\n");
        return -1;
    }
    char name[29];
    for (int i = 0; i < 28; i++) name[i] = (char)psp_read8(info + 4u + (uint32_t)i);
    name[28] = 0;
    if (strcmp(name, "modehsys") != 0) {
        fprintf(stderr, "extra PRX: EBOOT module is '%s'\n", name);
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
        fprintf(stderr, "extra PRX: no EBOOT exports resolved\n");
        return -1;
    }
    qsort(g_eboot_exports, (size_t)g_eboot_nexports, sizeof g_eboot_exports[0], cmp_export);
    return 0;
}

static int load_mod(extra_mod_t *m, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "extra PRX: cannot open %s\n", path); return -1; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    uint8_t *d = n > 52 ? (uint8_t *)malloc((size_t)n) : NULL;
    if (!d || fread(d, 1, (size_t)n, f) != (size_t)n) {
        fclose(f); free(d);
        fprintf(stderr, "extra PRX: cannot read %s\n", path);
        return -1;
    }
    fclose(f);
    if (memcmp(d, "\x7f" "ELF", 4) != 0 || d[4] != 1 || d[5] != 1 ||
        rd16(d + 18) != 8) {
        free(d); fprintf(stderr, "extra PRX: %s is not ELF32 MIPS LE\n", m->name);
        return -1;
    }
    uint32_t phoff = rd32(d + 28);
    uint16_t phentsize = rd16(d + 42), phnum = rd16(d + 44);
    if (phentsize < 32 || phoff + (uint32_t)phnum * phentsize > (uint32_t)n) {
        free(d); fprintf(stderr, "extra PRX: %s has bad program headers\n", m->name);
        return -1;
    }
    uint32_t off = 0, vaddr = 0, filesz = 0, memsz = 0;
    int ns = 0;
    for (uint16_t i = 0; i < phnum; i++) {
        const uint8_t *p = d + phoff + (uint32_t)i * phentsize;
        if (rd32(p) != 1) continue;
        if (ns++) { free(d); fprintf(stderr, "extra PRX: %s has two PT_LOAD\n", m->name); return -1; }
        off = rd32(p + 4);
        vaddr = rd32(p + 8);
        filesz = rd32(p + 16);
        memsz = rd32(p + 20);
    }
    if (ns != 1 || vaddr != m->base) {
        free(d);
        fprintf(stderr, "extra PRX: %s is not one segment at 0x%08X (got 0x%08X)\n",
                m->name, m->base, vaddr);
        return -1;
    }
    if (filesz > memsz || (uint64_t)off + filesz > (uint64_t)n ||
        (uint64_t)vaddr + memsz > m->end_max) {
        free(d); fprintf(stderr, "extra PRX: %s leaves its window\n", m->name);
        return -1;
    }
    if (m->map_extra && psp_mem_map_extra(vaddr, memsz) != 0) {
        free(d); fprintf(stderr, "extra PRX: cannot map extra window for %s\n", m->name);
        return -1;
    }
    if (psp_mem_write_block(vaddr, d + off, filesz) != 0) {
        free(d); fprintf(stderr, "extra PRX: cannot place %s\n", m->name);
        return -1;
    }
    if (psp_read32(vaddr) != rd32(d + off) ||
        (memsz > filesz && psp_read8(vaddr + filesz) != 0)) {
        free(d); fprintf(stderr, "extra PRX: %s read-back failed\n", m->name);
        return -1;
    }
    uint32_t modoff = rd32(d + phoff + 12) & 0x7FFFFFFFu;
    if (modoff < off || modoff >= off + filesz) {
        free(d); fprintf(stderr, "extra PRX: %s module info outside segment\n", m->name);
        return -1;
    }
    uint32_t info = vaddr + (modoff - off);
    char modname[29];
    for (int i = 0; i < 28; i++) modname[i] = (char)psp_read8(info + 4u + (uint32_t)i);
    modname[28] = 0;
    if (strcmp(modname, m->name) != 0) {
        free(d);
        fprintf(stderr, "extra PRX: module is '%s', expected %s\n", modname, m->name);
        return -1;
    }
    m->gp = psp_read32(info + 0x20);
    m->entry = rd32(d + 24);
    m->reg();
    psp_dispatch_module_gp(m->base, m->base + memsz, m->gp);
    int mod_exp_start = g_mod_nexports;
    uint32_t ent = psp_read32(info + 0x24);
    uint32_t ent_end = psp_read32(info + 0x28);
    for (uint32_t e = ent; e + 0x10 <= ent_end; ) {
        uint32_t namep = psp_read32(e);
        uint8_t ent_len = psp_read8(e + 8);
        uint8_t nvar = psp_read8(e + 9);
        uint16_t nfunc = psp_read16(e + 10);
        uint32_t table = psp_read32(e + 12);
        uint32_t stride = ent_len ? (uint32_t)ent_len * 4u : 0x10u;
        if (stride < 0x10) break;
        if (namep) {
            uint32_t total = (uint32_t)nfunc + nvar;
            for (uint16_t i = 0; i < nfunc; i++) {
                uint32_t nid = psp_read32(table + (uint32_t)i * 4u);
                uint32_t addr = psp_read32(table + (total + i) * 4u);
                if (addr < m->base || addr >= m->base + memsz || !psp_lookup(addr)) continue;
                if (g_mod_nexports < MAX_MOD_EXPORTS) {
                    g_mod_exports[g_mod_nexports].nid = nid;
                    g_mod_exports[g_mod_nexports].addr = addr;
                    g_mod_exports[g_mod_nexports].gp = m->gp;
                    g_mod_nexports++;
                }
            }
        }
        e += stride;
    }
    m->loaded = 1;
    free(d);
    fprintf(stderr, "extra PRX %s 0x%08X..0x%08X (%u bytes .bss), gp 0x%08X, entry 0x%08X, %d exports\n",
            m->name, m->base, m->base + memsz, memsz - filesz, m->gp, m->entry,
            g_mod_nexports - mod_exp_start);
    return 0;
}

static int file_exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

static int find_auto_path(const char *dir_hint, const char *slug, char *out, size_t out_sz) {
    if (dir_hint && dir_hint[0]) {
        const char *s1 = strrchr(dir_hint, '/');
        const char *s2 = strrchr(dir_hint, '\\');
        const char *sep = s1 && s2 ? (s1 > s2 ? s1 : s2) : (s1 ? s1 : s2);
        if (sep) {
            size_t dlen = (size_t)(sep - dir_hint);
            if (dlen + 32 < out_sz) {
                snprintf(out, out_sz, "%.*s/s13-%s-reloc.elf", (int)dlen, dir_hint, slug);
                if (file_exists(out)) return 1;
            }
        }
    }
    snprintf(out, out_sz, "build/s13-%s-reloc.elf", slug);
    if (file_exists(out)) return 1;
    snprintf(out, out_sz, "s13-%s-reloc.elf", slug);
    return file_exists(out);
}

int psp_extra_images_load(const char *const *paths, const char *dir_hint, uint32_t eboot_info) {
    if (build_eboot_exports(eboot_info ? eboot_info : EBOOT_INFO) != 0) return -1;
    for (int i = 0; i < NEXTRA; i++) {
        extra_mod_t *m = &g_extra[i];
        char auto_buf[512];
        const char *path = (paths && paths[i] && paths[i][0]) ? paths[i] : NULL;
        if (!path) {
            if (!m->compiled) continue;
            if (!find_auto_path(dir_hint, m->slug, auto_buf, sizeof auto_buf)) continue;
            path = auto_buf;
        } else if (!m->compiled) {
            fprintf(stderr, "extra PRX: %s requested but corpus not compiled in\n", m->name);
            return -1;
        }
        if (load_mod(m, path) != 0) return -1;
    }
    return 0;
}

int psp_extra_load_uid(const char *path, uint32_t *uid) {
    const char *slash = strrchr(path, '/');
    const char *base = slash ? slash + 1 : path;
    for (int i = 0; i < NEXTRA; i++) {
        extra_mod_t *m = &g_extra[i];
        if (!m->loaded) continue;
        if (strstr(path, m->lbn) || strcmp(base, m->prx) == 0) {
            *uid = m->uid;
            return 1;
        }
    }
    return 0;
}

int psp_extra_start(uint32_t uid, uint32_t status_ptr) {
    for (int i = 0; i < NEXTRA; i++) {
        extra_mod_t *m = &g_extra[i];
        if (!m->loaded || m->uid != uid) continue;
        uint32_t caller_gp = psp_cpu.r[PSP_REG_GP];
        psp_cpu.r[PSP_REG_A0] = psp_arg(1);
        psp_cpu.r[PSP_REG_A1] = psp_arg(2);
        if (m->entry < 0x80000000u) {
            psp_cpu.r[PSP_REG_GP] = m->gp;
            if (getenv("SETO_TRACE_MODULE"))
                fprintf(stderr, "module: start '%s' entry=0x%08X\n", m->name, m->entry);
            psp_call_from_host(m->entry);
            if (getenv("SETO_TRACE_MODULE"))
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

uint32_t psp_extra_export(uint32_t nid, uint32_t *gp_out) {
    for (int i = 0; i < g_mod_nexports; i++) {
        if (g_mod_exports[i].nid == nid) {
            if (gp_out) *gp_out = g_mod_exports[i].gp;
            return g_mod_exports[i].addr;
        }
    }
    return 0;
}

#if defined(HAVE_CHARALIST_CORPUS) || defined(HAVE_SHOP_CORPUS)
uint32_t psp_store_export(uint32_t nid, uint32_t *gp_out);
#endif
#ifdef HAVE_DUEL_CORPUS
uint32_t psp_duel_export(uint32_t nid, uint32_t *gp_out);
#endif
#ifdef HAVE_DECKTUT_CORPUS
uint32_t psp_decktut_export(uint32_t nid, uint32_t *gp_out);
#endif

void extra_import_forward(uint32_t nid) {
    uint32_t gp = g_eboot_gp;
    uint32_t addr = eboot_export(nid);
    if (!addr) addr = psp_extra_export(nid, &gp);
#if defined(HAVE_CHARALIST_CORPUS) || defined(HAVE_SHOP_CORPUS)
    if (!addr) addr = psp_store_export(nid, &gp);
#endif
#ifdef HAVE_DUEL_CORPUS
    if (!addr) addr = psp_duel_export(nid, &gp);
#endif
#ifdef HAVE_DECKTUT_CORPUS
    if (!addr) addr = psp_decktut_export(nid, &gp);
#endif
    if (!addr) {
        psp_hle_call(nid);
        return;
    }
    uint32_t caller_gp = psp_cpu.r[PSP_REG_GP];
    psp_cpu.r[PSP_REG_GP] = gp;
    psp_call_from_host(addr);
    psp_cpu.r[PSP_REG_GP] = caller_gp;
}

#include "seto_net.h"

/* ---- Firmware HLE handlers for module_htmlview (net/http/ssl/utility) --- */

enum {
    HTML_STATUS_NONE = 0,
    HTML_STATUS_INIT = 1,
    HTML_STATUS_RUN  = 2,
    HTML_STATUS_QUIT = 3,
    HTML_STATUS_SHUT = 4
};
static int g_html_status = HTML_STATUS_NONE;
static uint32_t g_html_param;

static void hle_net_ok(void) { psp_ret(0); }

static void hle_html_init_start(void) {
    uint32_t param = psp_arg(0);
    g_html_param = param;
    if (param) psp_write32(param + 0x2Cu, 0);
    g_html_status = HTML_STATUS_INIT;
    psp_ret(0);
}

static void hle_html_get_status(void) {
    int cur = g_html_status;
    if (cur == HTML_STATUS_INIT) g_html_status = HTML_STATUS_RUN;
    else if (cur == HTML_STATUS_SHUT) g_html_status = HTML_STATUS_NONE;
    psp_ret((uint32_t)cur);
}

static void hle_html_update(void) {
    if (g_html_status == HTML_STATUS_RUN) {
        if (g_html_param) psp_write32(g_html_param + 0x2Cu, 0);
        g_html_status = HTML_STATUS_QUIT;
    }
    psp_ret(0);
}

static void hle_html_shutdown_start(void) {
    g_html_status = HTML_STATUS_SHUT;
    psp_ret(0);
}

void psp_extra_bind_hle(void) {
    static const struct {
        uint32_t nid;
        const char *lib;
        const char *name;
        psp_hle_fn fn;
    } named[] = {
        { 0x17943399u, "sceNetInet",     "sceNetInetInit",                    hle_net_ok },
        { 0xA9ED66B9u, "sceNetInet",     "sceNetInetTerm",                    hle_net_ok },
        { 0xE2F91F9Bu, "sceNetApctl",    "sceNetApctlInit",                   hle_net_ok },
        { 0xB3EDD0ECu, "sceNetApctl",    "sceNetApctlTerm",                   hle_net_ok },
        { 0xF3370E61u, "sceNetResolver", "sceNetResolverInit",                hle_net_ok },
        { 0x6138194Au, "sceNetResolver", "sceNetResolverTerm",                hle_net_ok },
        { 0x957ECBE2u, "sceSsl",         "sceSslInit",                        hle_net_ok },
        { 0x191CDEFFu, "sceSsl",         "sceSslEnd",                         hle_net_ok },
        { 0xAB1ABE07u, "sceHttp",        "sceHttpInit",                       hle_net_ok },
        { 0xD1C8945Eu, "sceHttp",        "sceHttpEnd",                        hle_net_ok },
        { 0xF1657B22u, "sceHttp",        "sceHttpLoadSystemCookie",           hle_net_ok },
        { 0x76D1363Bu, "sceHttp",        "sceHttpSaveSystemCookie",           hle_net_ok },
        { 0xCDC3AA41u, "sceUtility",     "sceUtilityHtmlViewerInitStart",     hle_html_init_start },
        { 0xBDA7D894u, "sceUtility",     "sceUtilityHtmlViewerGetStatus",     hle_html_get_status },
        { 0x05AFB9E4u, "sceUtility",     "sceUtilityHtmlViewerUpdate",        hle_html_update },
        { 0xF5CE1134u, "sceUtility",     "sceUtilityHtmlViewerShutdownStart", hle_html_shutdown_start },
    };
    for (size_t i = 0; i < sizeof named / sizeof named[0]; i++)
        psp_hle_register(named[i].nid, named[i].lib, named[i].name, named[i].fn);

    psp_hle_register_unnamed(0x87797BDDu, "sceHttp", hle_net_ok);
    psp_hle_register_unnamed(0xE4D21302u, "sceHttp", hle_net_ok);
    psp_hle_register_unnamed(0xF9D8EB63u, "sceHttp", hle_net_ok);

    seto_net_register_hle();
}
