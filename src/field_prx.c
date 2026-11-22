/* S9.3: load and run the real modfield PRX corpus.
 *
 * The field module is requested by modstory through ModuleMgr. Its 258 imports
 * are libehsys EBOOT exports, resolved like modstory's. The image is 8.2 MB of
 * .bss, so it lives at 0x08C00000 and the runtime heap starts above it. */

#include <psprecomp/cpu.h>
#include <psprecomp/mem.h>
#include <psprecomp/hle.h>
#include <psprecomp/dispatch.h>
#include <psprecomp/recomp_rt.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FIELD_BASE      0x08C00000u
#define FIELD_END_MAX   0x09600000u
#define FIELD_UID       0x00080002u
#define FIELD_LBN_PATH  "disc0:/sce_lbn0x5c60_size0x4a21a"

#define EBOOT_BASE      0x08000000u
#define EBOOT_SIZE      0x00640000u
#define EBOOT_INFO      (EBOOT_BASE + 0x0006B81Cu)
#define MAX_EXPORTS     1600

typedef struct { uint32_t nid, addr; } field_export_t;
static field_export_t g_exports[MAX_EXPORTS];
static int g_nexports;
static uint32_t g_eboot_gp, g_field_gp;
static int g_field_loaded;
static int g_field_started;

int psp_field_started(void) { return g_field_started; }

#define MAX_FEXPORTS 16
static struct { uint32_t nid, addr; } g_fexports[MAX_FEXPORTS];
static int g_nfexports;

uint32_t psp_field_export(uint32_t nid) {
    for (int i = 0; i < g_nfexports; i++)
        if (g_fexports[i].nid == nid) return g_fexports[i].addr;
    return 0;
}
uint32_t psp_field_gp(void) { return g_field_gp; }

void field_psp_recomp_register(void);

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static int cmp_export(const void *a, const void *b) {
    uint32_t x = ((const field_export_t *)a)->nid;
    uint32_t y = ((const field_export_t *)b)->nid;
    return x < y ? -1 : x > y;
}
static uint32_t field_export(uint32_t nid) {
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
        fprintf(stderr, "field PRX: EBOOT is not relocated to 0x08000000\n");
        return -1;
    }
    char name[29];
    for (int i = 0; i < 28; i++) name[i] = (char)psp_read8(info + 4u + (uint32_t)i);
    name[28] = 0;
    if (strcmp(name, "modehsys") != 0) {
        fprintf(stderr, "field PRX: EBOOT module is '%s'\n", name);
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
        fprintf(stderr, "field PRX: no EBOOT exports resolved\n");
        return -1;
    }
    qsort(g_exports, (size_t)g_nexports, sizeof g_exports[0], cmp_export);
    return 0;
}

void field_import_forward(uint32_t nid) {
    uint32_t addr = field_export(nid);
    if (!addr) {
        static unsigned logged;
        if (logged++ < 20)
            fprintf(stderr, "field PRX: import NID 0x%08X has no EBOOT export\n", nid);
        psp_ret(0x80020001u);
        return;
    }
    uint32_t caller_gp = psp_cpu.r[PSP_REG_GP];
    psp_cpu.r[PSP_REG_GP] = g_eboot_gp;
    psp_call_from_host(addr);
    psp_cpu.r[PSP_REG_GP] = caller_gp;
}

static void field_entered(uint32_t addr) {
    fprintf(stderr, "field: entered recompiled module_start 0x%08X\n", addr);
}

int psp_field_image_load(const char *path, uint32_t eboot_info) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "field PRX: cannot open %s\n", path); return -1; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    uint8_t *d = n > 52 ? (uint8_t *)malloc((size_t)n) : NULL;
    if (!d || fread(d, 1, (size_t)n, f) != (size_t)n) {
        fclose(f); free(d);
        fprintf(stderr, "field PRX: cannot read %s\n", path);
        return -1;
    }
    fclose(f);
    if (memcmp(d, "\x7f" "ELF", 4) != 0 || d[4] != 1 || d[5] != 1 ||
        rd16(d + 18) != 8) {
        free(d);
        fprintf(stderr, "field PRX: not an ELF32 MIPS LE image\n");
        return -1;
    }
    uint32_t phoff = rd32(d + 28);
    uint16_t phentsize = rd16(d + 42), phnum = rd16(d + 44);
    if (phentsize < 32 || phoff + (uint32_t)phnum * phentsize > (uint32_t)n) {
        free(d); fprintf(stderr, "field PRX: bad program headers\n"); return -1;
    }
    uint32_t off = 0, vaddr = 0, filesz = 0, memsz = 0;
    int ns = 0;
    for (uint16_t i = 0; i < phnum; i++) {
        const uint8_t *p = d + phoff + (uint32_t)i * phentsize;
        if (rd32(p) != 1) continue;
        if (ns++) { free(d); fprintf(stderr, "field PRX: expected one PT_LOAD\n"); return -1; }
        off = rd32(p + 4);
        vaddr = rd32(p + 8);
        filesz = rd32(p + 16);
        memsz = rd32(p + 20);
    }
    if (ns != 1 || vaddr != FIELD_BASE) {
        free(d);
        fprintf(stderr, "field PRX: image is not one segment at 0x%08X\n", FIELD_BASE);
        return -1;
    }
    if (filesz > memsz || (uint64_t)off + filesz > (uint64_t)n ||
        (uint64_t)vaddr + memsz > FIELD_END_MAX) {
        free(d); fprintf(stderr, "field PRX: segment outside the window\n"); return -1;
    }
    if (psp_mem_write_block(vaddr, d + off, filesz) != 0) {
        free(d); fprintf(stderr, "field PRX: cannot place the image\n"); return -1;
    }
    if (psp_read32(vaddr) != rd32(d + off) ||
        (memsz > filesz && psp_read8(vaddr + filesz) != 0)) {
        free(d); fprintf(stderr, "field PRX: image read-back failed\n"); return -1;
    }
    uint32_t modoff = rd32(d + phoff + 12) & 0x7FFFFFFFu;
    if (modoff < off || modoff >= off + filesz) {
        free(d); fprintf(stderr, "field PRX: module info outside segment\n"); return -1;
    }
    uint32_t info = vaddr + (modoff - off);
    char modname[29];
    for (int i = 0; i < 28; i++) modname[i] = (char)psp_read8(info + 4u + (uint32_t)i);
    modname[28] = 0;
    if (strcmp(modname, "modfield") != 0) {
        free(d); fprintf(stderr, "field PRX: module is '%s', expected modfield\n", modname);
        return -1;
    }
    g_field_gp = psp_read32(info + 0x20);
    g_nfexports = 0;
    for (uint32_t e = psp_read32(info + 0x24), end = psp_read32(info + 0x28);
         e + 0x10 <= end; ) {
        uint8_t ent_len = psp_read8(e + 8);
        uint8_t nvar = psp_read8(e + 9);
        uint16_t nfunc = psp_read16(e + 10);
        uint32_t table = psp_read32(e + 12);
        uint32_t stride = ent_len ? (uint32_t)ent_len * 4u : 0x10u;
        if (stride < 0x10) break;
        uint32_t total = (uint32_t)nfunc + nvar;
        for (uint16_t i = 0; i < nfunc && g_nfexports < MAX_FEXPORTS; i++) {
            g_fexports[g_nfexports].nid = psp_read32(table + (uint32_t)i * 4u);
            g_fexports[g_nfexports].addr = psp_read32(table + (total + i) * 4u);
            g_nfexports++;
        }
        e += stride;
    }
    fprintf(stderr, "field PRX: %d exports\n", g_nfexports);
    field_psp_recomp_register();
    if (!psp_lookup(FIELD_BASE)) {
        free(d); fprintf(stderr, "field PRX: dispatch entry missing\n"); return -1;
    }
    if (build_exports(eboot_info ? eboot_info : EBOOT_INFO) != 0) {
        free(d); return -1;
    }
    psp_dispatch_module_gp(FIELD_BASE, FIELD_BASE + memsz, g_field_gp);
    g_field_loaded = 1;
    free(d);
    fprintf(stderr, "field PRX 0x%08X..0x%08X (%u bytes .bss), gp 0x%08X, "
            "%d EBOOT exports\n", vaddr, vaddr + memsz, memsz - filesz,
            g_field_gp, g_nexports);
    return 0;
}

int psp_field_load_uid(const char *path, uint32_t *uid) {
    if (!g_field_loaded) return 0;
    const char *slash = strrchr(path, '/');
    if (strcmp(path, FIELD_LBN_PATH) != 0 &&
        (!slash || strcmp(slash + 1, "libfield.prx") != 0))
        return 0;
    *uid = FIELD_UID;
    return 1;
}

int psp_field_start(uint32_t uid, uint32_t status_ptr) {
    if (uid != FIELD_UID || !g_field_loaded) return 0;
    uint32_t caller_gp = psp_cpu.r[PSP_REG_GP];
    psp_cpu.r[PSP_REG_A0] = psp_arg(1);
    psp_cpu.r[PSP_REG_A1] = psp_arg(2);
    psp_cpu.r[PSP_REG_GP] = g_field_gp;
    psp_trace_watch(FIELD_BASE, field_entered);
    psp_call_from_host(FIELD_BASE);
    psp_cpu.r[PSP_REG_GP] = caller_gp;
    g_field_started = 1;
    if (status_ptr) psp_write32(status_ptr, 0);
    psp_ret(0);
    return 1;
}
