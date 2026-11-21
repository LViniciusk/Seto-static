/* Load and run Store / Character-list subsystem PRX corpora (modcharalist, modshop).
 *
 * modcharalist sits in PSP main RAM after decktut (0x09410000..0x09700000);
 * modshop (~7.76 MB with .bss) sits in the low window after modduelrecord
 * (0x03600000..0x04000000) via psp_mem_map_extra. Both import only libehsys
 * from the EBOOT. */

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

typedef struct { uint32_t nid, addr; } store_export_t;
typedef struct { uint32_t nid, addr, gp; } store_mod_export_t;
#define MAX_MOD_EXPORTS 512
static store_mod_export_t g_mod_exports[MAX_MOD_EXPORTS];
static int g_mod_nexports;

typedef struct {
    const char *name;
    const char *lbn;
    const char *prx;
    uint32_t base, end_max, uid, entry, gp, filesz, memsz;
    int map_extra;
    void (*reg)(void);
    int loaded;
    uint8_t *image;
} store_mod_t;

#ifdef HAVE_CHARALIST_CORPUS
void charalist_psp_recomp_register(void);
#else
static void charalist_psp_recomp_register(void) { }
#endif
#ifdef HAVE_SHOP_CORPUS
void shop_psp_recomp_register(void);
#else
static void shop_psp_recomp_register(void) { }
#endif

static store_mod_t g_store[] = {
    { "modcharalist", "sce_lbn0x56b0", "libcharalist.prx",
      0x09410000u, 0x09700000u, 0x00080005u, 0, 0, 0, 0, 0, charalist_psp_recomp_register, 0, NULL },
    { "modshop",      "sce_lbn0x5dd0", "libshop.prx",
      0x03600000u, 0x04000000u, 0x00080006u, 0, 0, 0, 0, 1, shop_psp_recomp_register,      0, NULL },
};
#define NSTORE ((int)(sizeof g_store / sizeof g_store[0]))

static store_export_t g_eboot_exports[MAX_EXPORTS];
static int g_eboot_nexports;
static uint32_t g_eboot_gp;

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static int cmp_export(const void *a, const void *b) {
    uint32_t x = ((const store_export_t *)a)->nid;
    uint32_t y = ((const store_export_t *)b)->nid;
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
        fprintf(stderr, "store PRX: EBOOT is not relocated to 0x08000000\n");
        return -1;
    }
    char name[29];
    for (int i = 0; i < 28; i++) name[i] = (char)psp_read8(info + 4u + (uint32_t)i);
    name[28] = 0;
    if (strcmp(name, "modehsys") != 0) {
        fprintf(stderr, "store PRX: EBOOT module is '%s'\n", name);
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
        fprintf(stderr, "store PRX: no EBOOT exports resolved\n");
        return -1;
    }
    qsort(g_eboot_exports, (size_t)g_eboot_nexports, sizeof g_eboot_exports[0], cmp_export);
    return 0;
}

static int load_mod(store_mod_t *m, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "store PRX: cannot open %s\n", path); return -1; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    uint8_t *d = n > 52 ? (uint8_t *)malloc((size_t)n) : NULL;
    if (!d || fread(d, 1, (size_t)n, f) != (size_t)n) {
        fclose(f); free(d);
        fprintf(stderr, "store PRX: cannot read %s\n", path);
        return -1;
    }
    fclose(f);
    if (memcmp(d, "\x7f" "ELF", 4) != 0 || d[4] != 1 || d[5] != 1 ||
        rd16(d + 18) != 8) {
        free(d); fprintf(stderr, "store PRX: %s is not ELF32 MIPS LE\n", m->name);
        return -1;
    }
    uint32_t phoff = rd32(d + 28);
    uint16_t phentsize = rd16(d + 42), phnum = rd16(d + 44);
    if (phentsize < 32 || phoff + (uint32_t)phnum * phentsize > (uint32_t)n) {
        free(d); fprintf(stderr, "store PRX: %s has bad program headers\n", m->name);
        return -1;
    }
    uint32_t off = 0, vaddr = 0, filesz = 0, memsz = 0;
    int ns = 0;
    for (uint16_t i = 0; i < phnum; i++) {
        const uint8_t *p = d + phoff + (uint32_t)i * phentsize;
        if (rd32(p) != 1) continue;
        if (ns++) { free(d); fprintf(stderr, "store PRX: %s has two PT_LOAD\n", m->name); return -1; }
        off = rd32(p + 4);
        vaddr = rd32(p + 8);
        filesz = rd32(p + 16);
        memsz = rd32(p + 20);
    }
    if (ns != 1 || vaddr != m->base) {
        free(d);
        fprintf(stderr, "store PRX: %s is not one segment at 0x%08X\n", m->name, m->base);
        return -1;
    }
    if (filesz > memsz || (uint64_t)off + filesz > (uint64_t)n ||
        (uint64_t)vaddr + memsz > m->end_max) {
        free(d); fprintf(stderr, "store PRX: %s leaves its window\n", m->name);
        return -1;
    }
    if (m->map_extra && psp_mem_map_extra(vaddr, memsz) != 0) {
        free(d); fprintf(stderr, "store PRX: cannot map extra window for %s\n", m->name);
        return -1;
    }
    if (psp_mem_write_block(vaddr, d + off, filesz) != 0) {
        free(d); fprintf(stderr, "store PRX: cannot place %s\n", m->name);
        return -1;
    }
    if (psp_read32(vaddr) != rd32(d + off) ||
        (memsz > filesz && psp_read8(vaddr + filesz) != 0)) {
        free(d); fprintf(stderr, "store PRX: %s read-back failed\n", m->name);
        return -1;
    }
    uint32_t modoff = rd32(d + phoff + 12) & 0x7FFFFFFFu;
    if (modoff < off || modoff >= off + filesz) {
        free(d); fprintf(stderr, "store PRX: %s module info outside segment\n", m->name);
        return -1;
    }
    uint32_t info = vaddr + (modoff - off);
    char modname[29];
    for (int i = 0; i < 28; i++) modname[i] = (char)psp_read8(info + 4u + (uint32_t)i);
    modname[28] = 0;
    if (strcmp(modname, m->name) != 0) {
        free(d);
        fprintf(stderr, "store PRX: module is '%s', expected %s\n", modname, m->name);
        return -1;
    }
    m->gp = psp_read32(info + 0x20);
    m->entry = rd32(d + 24);
    if (m->base == 0x03600000u)
        psp_dispatch_clear_range(0x03600000u, 0x04000000u);
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
    m->filesz = filesz;
    m->memsz = memsz;
    free(m->image);
    m->image = (uint8_t *)malloc(filesz ? filesz : 1u);
    if (m->image && filesz) memcpy(m->image, d + off, filesz);
    m->loaded = 1;
    free(d);
    fprintf(stderr, "store PRX %s 0x%08X..0x%08X (%u bytes .bss), gp 0x%08X, entry 0x%08X, %d exports\n",
            m->name, m->base, m->base + memsz, memsz - filesz, m->gp, m->entry,
            g_mod_nexports - mod_exp_start);
    return 0;
}

static void restore_mod_image(store_mod_t *m) {
    if (!m->loaded || !m->image || !m->memsz) return;
    if (m->map_extra) {
        psp_mem_map_extra(m->base, m->memsz);
        psp_mem_write_block(m->base, m->image, m->filesz);
    } else {
        psp_mem_write_block(m->base, m->image, m->filesz);
        if (m->memsz > m->filesz) {
            uint32_t n = m->memsz - m->filesz;
            uint8_t *bss = psp_mem_unwatched_ptr(m->base + m->filesz, n);
            if (bss) memset(bss, 0, n);
            else for (uint32_t i = 0; i < n; i++) psp_write8(m->base + m->filesz + i, 0);
        }
    }
    if (m->base == 0x03600000u)
        psp_dispatch_clear_range(0x03600000u, 0x04000000u);
    m->reg();
    psp_dispatch_module_gp(m->base, m->base + m->memsz, m->gp);
}

int psp_store_images_load(const char *charalist_path, const char *shop_path, uint32_t eboot_info) {
    const char *paths[NSTORE] = { charalist_path, shop_path };
    int any = 0;
    for (int i = 0; i < NSTORE; i++) if (paths[i]) any = 1;
    if (!any) return 0;
    if (build_eboot_exports(eboot_info ? eboot_info : EBOOT_INFO) != 0) return -1;
    for (int i = 0; i < NSTORE; i++) {
        if (!paths[i]) continue;
        if (load_mod(&g_store[i], paths[i]) != 0) return -1;
    }
    return 0;
}

int psp_store_load_uid(const char *path, uint32_t *uid) {
    const char *slash = strrchr(path, '/');
    const char *base = slash ? slash + 1 : path;
    for (int i = 0; i < NSTORE; i++) {
        store_mod_t *m = &g_store[i];
        if (!m->loaded) continue;
        if (strstr(path, m->lbn) || strcmp(base, m->prx) == 0) {
            restore_mod_image(m);
            *uid = m->uid;
            return 1;
        }
    }
    return 0;
}

int psp_store_start(uint32_t uid, uint32_t status_ptr) {
    for (int i = 0; i < NSTORE; i++) {
        store_mod_t *m = &g_store[i];
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

uint32_t psp_store_export(uint32_t nid, uint32_t *gp_out) {
    for (int i = 0; i < g_mod_nexports; i++) {
        if (g_mod_exports[i].nid == nid) {
            if (gp_out) *gp_out = g_mod_exports[i].gp;
            return g_mod_exports[i].addr;
        }
    }
    return 0;
}

void store_import_forward(uint32_t nid) {
    uint32_t gp = g_eboot_gp;
    uint32_t addr = eboot_export(nid);
    if (!addr) addr = psp_store_export(nid, &gp);
    if (!addr) {
        static unsigned logged;
        if (logged++ < 20)
            fprintf(stderr, "store PRX: import NID 0x%08X has no EBOOT export\n", nid);
        psp_ret(0x80020001u);
        return;
    }
    uint32_t caller_gp = psp_cpu.r[PSP_REG_GP];
    psp_cpu.r[PSP_REG_GP] = gp;
    psp_call_from_host(addr);
    psp_cpu.r[PSP_REG_GP] = caller_gp;
}
