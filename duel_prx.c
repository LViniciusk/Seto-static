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

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EBOOT_BASE      0x08000000u
#define EBOOT_SIZE      0x00640000u
#define EBOOT_INFO      (EBOOT_BASE + 0x0006B81Cu)
#define MAX_EXPORTS     1600
#define MAX_DEXPORTS    256

typedef struct { uint32_t nid, addr, gp; } duel_export_t;

typedef struct {
    const char *name;
    const char *lbn;       /* "sce_lbn0x..." token the game passes */
    const char *prx;       /* basename alias, when the game asks by name */
    uint32_t base, end_max, uid, entry, gp, memsz;
    void (*reg)(void);
    int loaded, nexports;
    duel_export_t ex[MAX_DEXPORTS];
} duel_mod_t;

void duelmgr_psp_recomp_register(void);
void dueleng_psp_recomp_register(void);
void dueldraw_psp_recomp_register(void);
void duelswap_psp_recomp_register(void);
void dueltut_psp_recomp_register(void);
void duelrec_psp_recomp_register(void);

static duel_mod_t g_duel[] = {
    { "modduel_mgr", "sce_lbn0x5c20", "libduel_mgr.prx",
      0x02200000u, 0x02E00000u, 0x00080010u, 0, 0, 0, duelmgr_psp_recomp_register, 0, 0, {{0,0,0}} },
    { "modduel_eng", "sce_lbn0x5940", "libduel_eng.prx",
      0x02E00000u, 0x03000000u, 0x00080011u, 0, 0, 0, dueleng_psp_recomp_register, 0, 0, {{0,0,0}} },
    { "modduel_draw", "sce_lbn0x5800", "libduel_draw.prx",
      0x03000000u, 0x03500000u, 0x00080012u, 0, 0, 0, dueldraw_psp_recomp_register, 0, 0, {{0,0,0}} },
    { "moddeck_swap", "sce_lbn0x5750", "libdeck_swap.prx",
      0x03500000u, 0x03530000u, 0x00080013u, 0, 0, 0, duelswap_psp_recomp_register, 0, 0, {{0,0,0}} },
    { "modtutorial", "sce_lbn0x5ef0", "libtutorial.prx",
      0x03530000u, 0x03550000u, 0x00080014u, 0, 0, 0, dueltut_psp_recomp_register, 0, 0, {{0,0,0}} },
    { "modduelrecord", "sce_lbn0x57f0", "libduelrecord.prx",
      0x03550000u, 0x03600000u, 0x00080015u, 0, 0, 0, duelrec_psp_recomp_register, 0, 0, {{0,0,0}} },
};
#define NDUEL ((int)(sizeof g_duel / sizeof g_duel[0]))

static struct { uint32_t nid, addr; } g_eboot_exports[MAX_EXPORTS];
static int g_eboot_nexports;
static uint32_t g_eboot_gp;
static int g_duel_ready;

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
    m->reg();
    psp_dispatch_module_gp(m->base, m->base + memsz, m->gp);
    m->memsz = memsz;
    m->loaded = 1;
    free(d);
    fprintf(stderr, "duel PRX %s 0x%08X..0x%08X (%u bytes .bss), gp 0x%08X, "
            "%d exports, entry 0x%08X\n", m->name, m->base, m->base + memsz,
            memsz - filesz, m->gp, m->nexports, m->entry);
    return 0;
}

int psp_duel_images_load(const char *const *paths, uint32_t eboot_info) {
    if (build_eboot_exports(eboot_info ? eboot_info : EBOOT_INFO) != 0) return -1;
    for (int i = 0; i < NDUEL; i++) {
        if (!paths[i]) continue;
        if (load_mod(&g_duel[i], paths[i]) != 0) return -1;
    }
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
    *uid = m->uid;
    return 1;
}

int psp_duel_start(uint32_t uid, uint32_t status_ptr) {
    if (!g_duel_ready) return 0;
    for (int i = 0; i < NDUEL; i++) {
        duel_mod_t *m = &g_duel[i];
        if (!m->loaded || m->uid != uid) continue;
        uint32_t caller_gp = psp_cpu.r[PSP_REG_GP];
        psp_cpu.r[PSP_REG_A0] = psp_arg(1);
        psp_cpu.r[PSP_REG_A1] = psp_arg(2);
        if (m->entry < 0x80000000u) {
            psp_cpu.r[PSP_REG_GP] = m->gp;
            psp_call_from_host(m->entry);
        }
        psp_cpu.r[PSP_REG_GP] = caller_gp;
        if (status_ptr) psp_write32(status_ptr, 0);
        psp_ret(0);
        return 1;
    }
    return 0;
}

void duel_import_forward(uint32_t nid) {
    uint32_t addr = 0, gp = 0;
    for (int i = 0; i < NDUEL && !addr; i++) {
        duel_mod_t *m = &g_duel[i];
        if (!m->loaded) continue;
        int lo = 0, hi = m->nexports - 1;
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            if (m->ex[mid].nid == nid) {
                addr = m->ex[mid].addr;
                gp = m->ex[mid].gp;
                break;
            }
            if (m->ex[mid].nid < nid) lo = mid + 1; else hi = mid - 1;
        }
    }
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
    psp_call_from_host(addr);
    psp_cpu.r[PSP_REG_GP] = caller_gp;
}
