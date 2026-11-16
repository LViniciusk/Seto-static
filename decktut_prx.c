/* S9.7: load and run the real moddeck_tutorial PRX corpus.
 *
 * The exit from the Chancellor's Office leads to the deck tutorial, which
 * asks ModuleMgr for libdeck_tutorial.prx (LBN 0x5790). The 130 KB image fits
 * in the free main-RAM gap between modfield and the runtime heap. */

#include <psprecomp/cpu.h>
#include <psprecomp/mem.h>
#include <psprecomp/hle.h>
#include <psprecomp/dispatch.h>
#include <psprecomp/recomp_rt.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DECKTUT_BASE      0x093E0000u
#define DECKTUT_END_MAX   0x09700000u
#define DECKTUT_ENTRY     0x093E0000u
#define DECKTUT_UID       0x00080004u
#define DECKTUT_LBN_PATH  "disc0:/sce_lbn0x5790_size0x292fa"

#define EBOOT_BASE        0x08000000u
#define EBOOT_SIZE        0x00640000u
#define EBOOT_INFO        (EBOOT_BASE + 0x0006B81Cu)
#define MAX_EXPORTS       1600

typedef struct { uint32_t nid, addr; } decktut_export_t;
static decktut_export_t g_exports[MAX_EXPORTS];
static int g_nexports;
static decktut_export_t g_mod_exports[32];
static int g_mod_nexports;
static uint32_t g_eboot_gp, g_decktut_gp;
static int g_decktut_loaded;

void decktut_psp_recomp_register(void);

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static int cmp_export(const void *a, const void *b) {
    uint32_t x = ((const decktut_export_t *)a)->nid;
    uint32_t y = ((const decktut_export_t *)b)->nid;
    return x < y ? -1 : x > y;
}
static uint32_t decktut_export(uint32_t nid) {
    int lo = 0, hi = g_nexports - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (g_exports[mid].nid == nid) return g_exports[mid].addr;
        if (g_exports[mid].nid < nid) lo = mid + 1; else hi = mid - 1;
    }
    return 0;
}

static int build_exports(uint32_t info) {
    if (info != EBOOT_INFO) {
        fprintf(stderr, "decktut PRX: EBOOT is not relocated to 0x08000000\n");
        return -1;
    }
    char name[29];
    for (int i = 0; i < 28; i++) name[i] = (char)psp_read8(info + 4u + (uint32_t)i);
    name[28] = 0;
    if (strcmp(name, "modehsys") != 0) {
        fprintf(stderr, "decktut PRX: EBOOT module is '%s'\n", name);
        return -1;
    }
    g_eboot_gp = psp_read32(info + 0x20);
    uint32_t ent = psp_read32(info + 0x24);
    uint32_t ent_end = psp_read32(info + 0x28);
    g_nexports = 0;
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
                if (g_nexports < MAX_EXPORTS) {
                    g_exports[g_nexports].nid = nid;
                    g_exports[g_nexports].addr = addr;
                    g_nexports++;
                }
            }
        }
        e += stride;
    }
    if (!g_nexports) {
        fprintf(stderr, "decktut PRX: no EBOOT exports resolved\n");
        return -1;
    }
    qsort(g_exports, (size_t)g_nexports, sizeof g_exports[0], cmp_export);
    return 0;
}

void decktut_import_forward(uint32_t nid) {
    uint32_t addr = decktut_export(nid);
    if (!addr) {
        psp_hle_call(nid);
        return;
    }
    uint32_t caller_gp = psp_cpu.r[PSP_REG_GP];
    psp_cpu.r[PSP_REG_GP] = g_eboot_gp;
    psp_call_from_host(addr);
    psp_cpu.r[PSP_REG_GP] = caller_gp;
}

static void decktut_entered(uint32_t addr) {
    fprintf(stderr, "decktut: entered recompiled module_start 0x%08X\n", addr);
}

int psp_decktut_image_load(const char *path, uint32_t eboot_info) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "decktut PRX: cannot open %s\n", path); return -1; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    uint8_t *d = n > 52 ? (uint8_t *)malloc((size_t)n) : NULL;
    if (!d || fread(d, 1, (size_t)n, f) != (size_t)n) {
        fclose(f); free(d);
        fprintf(stderr, "decktut PRX: cannot read %s\n", path);
        return -1;
    }
    fclose(f);
    if (memcmp(d, "\x7f" "ELF", 4) != 0 || d[4] != 1 || d[5] != 1 ||
        rd16(d + 18) != 8) {
        free(d);
        fprintf(stderr, "decktut PRX: not an ELF32 MIPS LE image\n");
        return -1;
    }
    uint32_t phoff = rd32(d + 28);
    uint16_t phentsize = rd16(d + 42), phnum = rd16(d + 44);
    if (phentsize < 32 || phoff + (uint32_t)phnum * phentsize > (uint32_t)n) {
        free(d); fprintf(stderr, "decktut PRX: bad program headers\n"); return -1;
    }
    uint32_t off = 0, vaddr = 0, filesz = 0, memsz = 0;
    int ns = 0;
    for (uint16_t i = 0; i < phnum; i++) {
        const uint8_t *p = d + phoff + (uint32_t)i * phentsize;
        if (rd32(p) != 1) continue;
        if (ns++) { free(d); fprintf(stderr, "decktut PRX: expected one PT_LOAD\n"); return -1; }
        off = rd32(p + 4);
        vaddr = rd32(p + 8);
        filesz = rd32(p + 16);
        memsz = rd32(p + 20);
    }
    if (ns != 1 || vaddr != DECKTUT_BASE) {
        free(d);
        fprintf(stderr, "decktut PRX: image is not one segment at 0x%08X\n", DECKTUT_BASE);
        return -1;
    }
    if (filesz > memsz || (uint64_t)off + filesz > (uint64_t)n ||
        (uint64_t)vaddr + memsz > DECKTUT_END_MAX) {
        free(d); fprintf(stderr, "decktut PRX: segment outside the window\n"); return -1;
    }
    if (psp_mem_write_block(vaddr, d + off, filesz) != 0) {
        free(d); fprintf(stderr, "decktut PRX: cannot place the image\n"); return -1;
    }
    if (psp_read32(vaddr) != rd32(d + off) ||
        (memsz > filesz && psp_read8(vaddr + filesz) != 0)) {
        free(d); fprintf(stderr, "decktut PRX: image read-back failed\n"); return -1;
    }
    uint32_t modoff = rd32(d + phoff + 12) & 0x7FFFFFFFu;
    if (modoff < off || modoff >= off + filesz) {
        free(d); fprintf(stderr, "decktut PRX: module info outside segment\n"); return -1;
    }
    uint32_t info = vaddr + (modoff - off);
    char modname[29];
    for (int i = 0; i < 28; i++) modname[i] = (char)psp_read8(info + 4u + (uint32_t)i);
    modname[28] = 0;
    if (strcmp(modname, "moddeck_tutorial") != 0) {
        free(d); fprintf(stderr, "decktut PRX: module is '%s', expected moddeck_tutorial\n", modname);
        return -1;
    }
    g_decktut_gp = psp_read32(info + 0x20);
    decktut_psp_recomp_register();
    if (!psp_lookup(DECKTUT_ENTRY)) {
        free(d); fprintf(stderr, "decktut PRX: dispatch entry missing\n"); return -1;
    }
    if (build_exports(eboot_info ? eboot_info : EBOOT_INFO) != 0) {
        free(d); return -1;
    }
    g_mod_nexports = 0;
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
            for (uint16_t i = 0; i < nfunc && g_mod_nexports < 32; i++) {
                uint32_t nid = psp_read32(table + (uint32_t)i * 4u);
                uint32_t addr = psp_read32(table + (total + i) * 4u);
                if (addr < DECKTUT_BASE || addr >= DECKTUT_BASE + memsz || !psp_lookup(addr)) continue;
                g_mod_exports[g_mod_nexports].nid = nid;
                g_mod_exports[g_mod_nexports].addr = addr;
                g_mod_nexports++;
            }
        }
        e += stride;
    }
    psp_dispatch_module_gp(DECKTUT_BASE, DECKTUT_BASE + memsz, g_decktut_gp);
    g_decktut_loaded = 1;
    free(d);
    fprintf(stderr, "decktut PRX 0x%08X..0x%08X (%u bytes .bss), gp 0x%08X, "
            "%d EBOOT exports\n", vaddr, vaddr + memsz, memsz - filesz,
            g_decktut_gp, g_nexports);
    return 0;
}

uint32_t psp_decktut_export(uint32_t nid, uint32_t *gp_out) {
    if (!g_decktut_loaded) return 0;
    for (int i = 0; i < g_mod_nexports; i++) {
        if (g_mod_exports[i].nid == nid) {
            if (gp_out) *gp_out = g_decktut_gp;
            return g_mod_exports[i].addr;
        }
    }
    return 0;
}

int psp_decktut_load_uid(const char *path, uint32_t *uid) {
    if (!g_decktut_loaded) return 0;
    const char *slash = strrchr(path, '/');
    if (strcmp(path, DECKTUT_LBN_PATH) != 0 &&
        (!slash || strcmp(slash + 1, "libdeck_tutorial.prx") != 0))
        return 0;
    *uid = DECKTUT_UID;
    return 1;
}

int psp_decktut_start(uint32_t uid, uint32_t status_ptr) {
    if (uid != DECKTUT_UID || !g_decktut_loaded) return 0;
    uint32_t caller_gp = psp_cpu.r[PSP_REG_GP];
    psp_cpu.r[PSP_REG_A0] = psp_arg(1);
    psp_cpu.r[PSP_REG_A1] = psp_arg(2);
    psp_cpu.r[PSP_REG_GP] = g_decktut_gp;
    psp_trace_watch(DECKTUT_ENTRY, decktut_entered);
    psp_call_from_host(DECKTUT_ENTRY);
    psp_cpu.r[PSP_REG_GP] = caller_gp;
    if (status_ptr) psp_write32(status_ptr, 0);
    psp_ret(0);
    return 1;
}
