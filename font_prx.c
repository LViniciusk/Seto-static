/* SETO-static S5: load the relocated sceFont_Library image and bind its exports.
 *
 * The image is placed in the reserved RAM window; its module info and export
 * table are parsed from guest memory; the seven sceLibFont NIDs are forwarded
 * to the recompiled exports through psp_call_from_host().
 */
#include <psprecomp/mem.h>
#include <psprecomp/cpu.h>
#include <psprecomp/dispatch.h>
#include <psprecomp/hle.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FONT_BASE       0x08900000u
#define FONT_END_MAX    0x08C00000u   /* runtime heap starts here */
#define EBOOT_END       0x00640000u   /* EBOOT module mapping */
#define FONT_UID        0x00070001u
#define FONT_LBN_PATH   "disc0:/sce_lbn0x52c0_size0x5ba0"

#ifdef HAVE_TITLE_CORPUS
int psp_title_load_uid(const char *path, uint32_t *uid);
int psp_title_start(uint32_t uid, uint32_t status_ptr);
int psp_story_load_uid(const char *path, uint32_t *uid);
int psp_story_start(uint32_t uid, uint32_t status_ptr);
int psp_field_load_uid(const char *path, uint32_t *uid);
int psp_field_start(uint32_t uid, uint32_t status_ptr);
int psp_deck_load_uid(const char *path, uint32_t *uid);
int psp_deck_start(uint32_t uid, uint32_t status_ptr);
int psp_decktut_load_uid(const char *path, uint32_t *uid);
int psp_decktut_start(uint32_t uid, uint32_t status_ptr);
#endif
#ifdef HAVE_DUEL_CORPUS
int psp_duel_load_uid(const char *path, uint32_t *uid);
int psp_duel_start(uint32_t uid, uint32_t status_ptr);
#endif
#if defined(HAVE_CHARALIST_CORPUS) || defined(HAVE_SHOP_CORPUS)
int psp_store_load_uid(const char *path, uint32_t *uid);
int psp_store_start(uint32_t uid, uint32_t status_ptr);
#endif

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int zero_range(uint32_t addr, uint32_t size) {
    static const uint8_t zeros[256];
    while (size) {
        uint32_t n = size < sizeof zeros ? size : sizeof zeros;
        if (psp_mem_write_block(addr, zeros, n) != 0) return -1;
        addr += n;
        size -= n;
    }
    return 0;
}

static int g_font_loaded;
static uint32_t g_font_gp, g_font_end;
static struct { uint32_t nid, addr; } g_font_exports[32];
static int g_font_nexports;
static uint32_t g_font_uid;

static uint32_t font_export(uint32_t nid) {
    for (int i = 0; i < g_font_nexports; i++)
        if (g_font_exports[i].nid == nid) return g_font_exports[i].addr;
    return 0;
}

int psp_font_image_load(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "font PRX: cannot open %s\n", path); return -1; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    uint8_t *d = n > 52 ? (uint8_t *)malloc((size_t)n) : NULL;
    if (!d || fread(d, 1, (size_t)n, f) != (size_t)n) {
        fclose(f); free(d);
        fprintf(stderr, "font PRX: cannot read %s\n", path);
        return -1;
    }
    fclose(f);

    if (memcmp(d, "\x7f" "ELF", 4) != 0 || d[4] != 1 || d[5] != 1 ||
        rd16(d + 18) != 8) {
        free(d);
        fprintf(stderr, "font PRX: not an ELF32 MIPS LE image\n");
        return -1;
    }

    uint32_t phoff = rd32(d + 28);
    uint16_t phentsize = rd16(d + 42), phnum = rd16(d + 44);
    if (phentsize < 32 || phoff + (uint32_t)phnum * phentsize > (uint32_t)n) {
        free(d);
        fprintf(stderr, "font PRX: bad program header table\n");
        return -1;
    }

    struct { uint32_t off, vaddr, filesz, memsz; } seg[2];
    int ns = 0;
    for (uint16_t i = 0; i < phnum; i++) {
        const uint8_t *p = d + phoff + (uint32_t)i * phentsize;
        if (rd32(p) != 1) continue;               /* PT_LOAD */
        if (ns == 2) { free(d); fprintf(stderr, "font PRX: more than two PT_LOAD\n"); return -1; }
        seg[ns].off = rd32(p + 4);
        seg[ns].vaddr = rd32(p + 8);
        seg[ns].filesz = rd32(p + 16);
        seg[ns].memsz = rd32(p + 20);
        ns++;
    }
    if (ns != 2) { free(d); fprintf(stderr, "font PRX: expected two PT_LOAD\n"); return -1; }
    if (seg[0].vaddr != FONT_BASE) {
        free(d);
        fprintf(stderr, "font PRX: image is not based at 0x%08X\n", FONT_BASE);
        return -1;
    }

    for (int i = 0; i < 2; i++) {
        if (seg[i].filesz > seg[i].memsz ||
            (uint64_t)seg[i].off + seg[i].filesz > (uint64_t)n ||
            seg[i].vaddr < FONT_BASE ||
            seg[i].vaddr < EBOOT_END ||
            (uint64_t)seg[i].vaddr + seg[i].memsz > FONT_END_MAX) {
            free(d);
            fprintf(stderr, "font PRX: segment %d outside the reserved window\n", i);
            return -1;
        }
    }
    for (int i = 0; i < 2; i++)
        for (int j = i + 1; j < 2; j++)
            if (!(seg[i].vaddr + seg[i].memsz <= seg[j].vaddr ||
                  seg[j].vaddr + seg[j].memsz <= seg[i].vaddr)) {
                free(d);
                fprintf(stderr, "font PRX: segments overlap\n");
                return -1;
            }

    uint32_t lo = seg[0].vaddr, hi = 0;
    for (int i = 0; i < 2; i++) {
        if (psp_mem_write_block(seg[i].vaddr, d + seg[i].off, seg[i].filesz) != 0 ||
            zero_range(seg[i].vaddr + seg[i].filesz, seg[i].memsz - seg[i].filesz) != 0) {
            free(d);
            fprintf(stderr, "font PRX: cannot place segment %d\n", i);
            return -1;
        }
        if (psp_read32(seg[i].vaddr) != rd32(d + seg[i].off)) {
            free(d);
            fprintf(stderr, "font PRX: read-back mismatch in segment %d\n", i);
            return -1;
        }
        if (seg[i].memsz > seg[i].filesz &&
            psp_read8(seg[i].vaddr + seg[i].filesz) != 0) {
            free(d);
            fprintf(stderr, "font PRX: .bss is not zero in segment %d\n", i);
            return -1;
        }
        if (seg[i].vaddr + seg[i].memsz > hi) hi = seg[i].vaddr + seg[i].memsz;
    }
    /* The module info is at p_paddr of the first PT_LOAD (a file offset in a
     * PRX). Parse it from the placed image: these are the addresses the
     * translated code will use. */
    uint32_t modoff = rd32(d + phoff + 12) & 0x7FFFFFFFu;
    if (modoff < seg[0].off || modoff >= seg[0].off + seg[0].filesz) {
        free(d);
        fprintf(stderr, "font PRX: module info is outside the first segment\n");
        return -1;
    }
    uint32_t info = FONT_BASE + (modoff - seg[0].off);
    if (info < lo || info + 0x34 > hi) {
        free(d);
        fprintf(stderr, "font PRX: module info outside the image\n");
        return -1;
    }
    uint32_t gp = psp_read32(info + 0x20);
    uint32_t ent = psp_read32(info + 0x24);
    uint32_t ent_end = psp_read32(info + 0x28);
    if (ent < FONT_BASE || ent_end > hi || ent_end <= ent) {
        free(d);
        fprintf(stderr, "font PRX: bad export table bounds\n");
        return -1;
    }
    g_font_nexports = 0;
    for (uint32_t e = ent; e + 0x10 <= ent_end; ) {
        uint8_t ent_len = psp_read8(e + 8);
        uint8_t nvar = psp_read8(e + 9);
        uint16_t nfunc = psp_read16(e + 10);
        uint32_t table = psp_read32(e + 12);
        uint32_t total = (uint32_t)nfunc + nvar;
        if (table < FONT_BASE || table + total * 8 > hi) {
            free(d);
            fprintf(stderr, "font PRX: export table outside the image\n");
            return -1;
        }
        for (uint16_t i = 0; i < nfunc; i++) {
            uint32_t nid = psp_read32(table + (uint32_t)i * 4);
            uint32_t addr = psp_read32(table + (total + i) * 4);
            if (addr < FONT_BASE || addr >= hi ||
                g_font_nexports >= (int)(sizeof g_font_exports / sizeof g_font_exports[0])) {
                free(d);
                fprintf(stderr, "font PRX: bad export address\n");
                return -1;
            }
            g_font_exports[g_font_nexports].nid = nid;
            g_font_exports[g_font_nexports].addr = addr;
            g_font_nexports++;
        }
        uint32_t stride = ent_len ? (uint32_t)ent_len * 4 : 0x10;
        if (stride < 0x10) break;
        e += stride;
    }
    g_font_gp = gp;
    g_font_end = hi;
    g_font_loaded = 1;
    free(d);
    printf("font PRX 0x%08X..0x%08X (2 segments, bss zeroed), gp 0x%08X, %d exports\n",
           lo, hi, gp, g_font_nexports);
    return 0;
}

/* ---- EBOOT -> PRX binding ------------------------------------------------ */

static void font_call(uint32_t nid) {
    uint32_t addr = font_export(nid);
    if (!addr) {
        fprintf(stderr, "font: no export for NID 0x%08X\n", nid);
        psp_ret(0x80020001u);
        return;
    }
    /* This PRX corpus has no $gp-relative access. Keep the caller's $gp so
     * EBOOT callbacks invoked by the font code see their own globals. */
    uint32_t gp = psp_cpu.r[PSP_REG_GP];
    psp_call_from_host(addr);
    psp_cpu.r[PSP_REG_GP] = gp;
}

static void font_newlib(void)       { font_call(0x67F17ED7u); }
static void font_find_optimum(void) { font_call(0x099EF33Cu); }
static void font_open(void)         { font_call(0xA834319Du); }
static void font_font_info(void)    { font_call(0x0DA7535Eu); }
static void font_set_alt(void)      { font_call(0xEE232411u); }
static void font_char_info(void)    { font_call(0xDCC80C2Fu); }
static void font_char_glyph(void)   { font_call(0x980F4895u); }

/* ---- minimal ModuleMgr for this one module ------------------------------- */

static void hle_LoadModule(void) {
    char path[128];
    psp_str(psp_arg(0), path, sizeof path);
    if (getenv("SETO_TRACE_MODULE"))
        fprintf(stderr, "module: load '%s'\n", path);
#ifdef HAVE_TITLE_CORPUS
    uint32_t tuid;
    if (psp_title_load_uid(path, &tuid)) {
        psp_ret(tuid);
        return;
    }
#endif
#ifdef HAVE_STORY_CORPUS
    uint32_t suid;
    if (psp_story_load_uid(path, &suid)) {
        psp_ret(suid);
        return;
    }
#endif
#ifdef HAVE_FIELD_CORPUS
    uint32_t fuid;
    if (psp_field_load_uid(path, &fuid)) {
        psp_ret(fuid);
        return;
    }
#endif
#ifdef HAVE_DECK_CORPUS
    uint32_t duid;
    if (psp_deck_load_uid(path, &duid)) {
        psp_ret(duid);
        return;
    }
#endif
#ifdef HAVE_DECKTUT_CORPUS
    uint32_t dtuid;
    if (psp_decktut_load_uid(path, &dtuid)) {
        psp_ret(dtuid);
        return;
    }
#endif
#ifdef HAVE_DUEL_CORPUS
    uint32_t dueluid;
    if (psp_duel_load_uid(path, &dueluid)) {
        psp_ret(dueluid);
        return;
    }
#endif
#if defined(HAVE_CHARALIST_CORPUS) || defined(HAVE_SHOP_CORPUS)
    uint32_t storeuid;
    if (psp_store_load_uid(path, &storeuid)) {
        psp_ret(storeuid);
        return;
    }
#endif
    if (!g_font_loaded || strcmp(path, FONT_LBN_PATH) != 0) {
        /* A module the game loads at run time and this host does not carry.
         * Naming it is the difference between "the scene is stuck" and why.
         * Deduplicate by path so repeated polls cannot hide the next missing PRX. */
        static char seen[16][128];
        static int nseen;
        int known = 0;
        for (int i = 0; i < nseen; i++) {
            if (strcmp(seen[i], path) == 0) { known = 1; break; }
        }
        if (!known && nseen < 16) {
            snprintf(seen[nseen++], sizeof seen[0], "%s", path);
            fprintf(stderr, "ModuleMgr: '%s' not integrated (ENOENT)\n", path);
        }
        psp_ret(0x80010002u);          /* ENOENT */
        return;
    }
    if (!g_font_uid) g_font_uid = FONT_UID;
    psp_ret(g_font_uid);
}

static void hle_StartModule(void) {
#ifdef HAVE_TITLE_CORPUS
    if (psp_title_start(psp_arg(0), psp_arg(3))) return;
#endif
#ifdef HAVE_STORY_CORPUS
    if (psp_story_start(psp_arg(0), psp_arg(3))) return;
#endif
#ifdef HAVE_FIELD_CORPUS
    if (psp_field_start(psp_arg(0), psp_arg(3))) return;
#endif
#ifdef HAVE_DECK_CORPUS
    if (psp_deck_start(psp_arg(0), psp_arg(3))) return;
#endif
#ifdef HAVE_DECKTUT_CORPUS
    if (psp_decktut_start(psp_arg(0), psp_arg(3))) return;
#endif
#ifdef HAVE_DUEL_CORPUS
    if (psp_duel_start(psp_arg(0), psp_arg(3))) return;
#endif
#if defined(HAVE_CHARALIST_CORPUS) || defined(HAVE_SHOP_CORPUS)
    if (psp_store_start(psp_arg(0), psp_arg(3))) return;
#endif
    if (psp_arg(0) != g_font_uid) {
        psp_ret(SCE_KERNEL_ERROR_UNKNOWN_UID);
        return;
    }
    /* The entry is 0xFFFFFFFF and the export table has no module_start: this is
     * a library, so starting it has no guest initializer to run. */
    uint32_t status = psp_arg(3);
    if (status) psp_write32(status, 0);
    psp_ret(0);
}

int psp_font_bind(void) {
    static const struct {
        uint32_t nid;
        const char *name;
        psp_hle_fn fn;
    } bind[] = {
        { 0x67F17ED7u, "sceFontNewLib",              font_newlib },
        { 0x099EF33Cu, "sceFontFindOptimumFont",     font_find_optimum },
        { 0xA834319Du, "sceFontOpen",                font_open },
        { 0x0DA7535Eu, "sceFontGetFontInfo",         font_font_info },
        { 0xEE232411u, "sceFontSetAltCharacterCode", font_set_alt },
        { 0xDCC80C2Fu, "sceFontGetCharInfo",         font_char_info },
        { 0x980F4895u, "sceFontGetCharGlyphImage",   font_char_glyph },
    };

    if (!g_font_loaded) return -1;
    for (size_t i = 0; i < sizeof bind / sizeof bind[0]; i++) {
        uint32_t addr = font_export(bind[i].nid);
        if (!addr || addr >= g_font_end || !psp_lookup(addr)) {
            fprintf(stderr, "font: export 0x%08X (%s) missing at 0x%08X\n",
                    bind[i].nid, bind[i].name, addr);
            return -1;
        }
        psp_hle_register(bind[i].nid, "sceLibFont", bind[i].name, bind[i].fn);
        fprintf(stderr, "font export 0x%08X (%s) -> 0x%08X\n",
                bind[i].nid, bind[i].name, addr);
    }
    psp_hle_register(0x977DE386u, "ModuleMgrForUser", "sceKernelLoadModule",  hle_LoadModule);
    psp_hle_register(0x50F0C1ECu, "ModuleMgrForUser", "sceKernelStartModule", hle_StartModule);
    return 0;
}
