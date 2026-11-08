/* SETO-static S6: place the relocated modtitle image and run its module_start.
 *
 * The EBOOT is `modehsys` and exports 1,245 libehsys functions. libtitle.prx
 * imports 197 of them. Its own export table is empty, so the only binding that
 * matters is the other direction: each libtitle import is forwarded to the
 * real EBOOT export, resolved from the EBOOT's own module tables in guest RAM.
 *
 * The module links one PT_LOAD and reserves 12.7 MB of .bss, which does not fit
 * the RAM window next to sceFont_Library and the runtime heap. It is mapped in
 * the free window after the EBOOT and before VRAM (0x00640000..0x0126FE90).
 */
#include <psprecomp/mem.h>
#include <psprecomp/cpu.h>
#include <psprecomp/dispatch.h>
#include <psprecomp/hle.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TITLE_BASE      0x00640000u
#define TITLE_END_MAX   0x04000000u   /* VRAM starts here */
#define TITLE_UID       0x00070002u
/* The EBOOT's loader resolves libtitle.prx to this ISO extent before it calls
 * sceKernelLoadModule; the 0x38B12 size is the 232,210-byte ELF checked below. */
#define TITLE_LBN_PATH  "disc0:/sce_lbn0x5e70_size0x38b12"
/* The EBOOT corpus is relocated to 0x08000000 so the game's own pointers and
 * hashed handles occupy the live RAM window instead of the low scratch area. */
#define EBOOT_BASE      0x08000000u
#define EBOOT_INFO      (EBOOT_BASE + 0x0006B81Cu)   /* modehsys module info */
#define EBOOT_SIZE      0x00631A80u
#define TITLE_MAX_EXP   1300

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void title_psp_recomp_register(void);

static int g_title_loaded;
static uint32_t g_title_gp, g_eboot_gp;
static struct { uint32_t nid, addr; } g_exports[TITLE_MAX_EXP];
static int g_nexports;

static int cmp_export(const void *a, const void *b) {
    uint32_t x = ((const uint32_t *)a)[0], y = ((const uint32_t *)b)[0];
    return x < y ? -1 : x > y;
}

/* Built from the EBOOT's .lib.ent table in guest memory. Only libehsys is
 * needed; the unnamed main-module entry is ignored. */
static int build_exports(uint32_t info) {
    if (info != EBOOT_INFO) {
        fprintf(stderr, "title PRX: EBOOT corpus is not relocated to 0x08000000 "
                        "(module info 0x%08X, expected 0x%08X); see README\n",
                info, EBOOT_INFO);
        return -1;
    }
    char name[29];
    for (int i = 0; i < 28; i++) name[i] = (char)psp_read8(info + 4u + (uint32_t)i);
    name[28] = 0;
    if (strcmp(name, "modehsys") != 0) {
        fprintf(stderr, "title PRX: EBOOT module is '%s', expected modehsys\n", name);
        return -1;
    }
    g_eboot_gp = psp_read32(info + 0x20);
    if (!g_eboot_gp) {
        fprintf(stderr, "title PRX: EBOOT module info has no $gp\n");
        return -1;
    }
    uint32_t ent = psp_read32(info + 0x24);
    uint32_t ent_end = psp_read32(info + 0x28);
    if (ent >= ent_end) {
        fprintf(stderr, "title PRX: empty EBOOT export table\n");
        return -1;
    }
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
                if (g_nexports < TITLE_MAX_EXP) {
                    g_exports[g_nexports].nid = nid;
                    g_exports[g_nexports].addr = addr;
                    g_nexports++;
                }
            }
            fprintf(stderr, "title PRX: %d libehsys exports in guest memory\n", nfunc);
        }
        e += stride;
    }
    if (!g_nexports) {
        fprintf(stderr, "title PRX: no libehsys export resolved\n");
        return -1;
    }
    qsort(g_exports, (size_t)g_nexports, sizeof g_exports[0], cmp_export);
    return 0;
}

static uint32_t title_export(uint32_t nid) {
    int lo = 0, hi = g_nexports - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (g_exports[mid].nid == nid) return g_exports[mid].addr;
        if (g_exports[mid].nid < nid) lo = mid + 1; else hi = mid - 1;
    }
    return 0;
}

/* Called by every libtitle import stub (the corpus is compiled with
 * -Dpsp_hle_call=title_import_forward). The call reaches the real EBOOT
 * function through psp_call_from_host(), so the guest register file, stack
 * arguments, $ra and frame chain are the normal generated path. */
void title_import_forward(uint32_t nid) {
    uint32_t addr = title_export(nid);
    if (!addr) {
        static unsigned logged;
        if (logged++ < 20)
            fprintf(stderr, "title PRX: import NID 0x%08X has no EBOOT export\n", nid);
        psp_ret(0x80020001u);
        return;
    }
    /* A cross-module call must run the callee with its own globals base. The
     * title code has no $gp-relative access, but the EBOOT does. */
    uint32_t caller_gp = psp_cpu.r[PSP_REG_GP];
    psp_cpu.r[PSP_REG_GP] = g_eboot_gp;
    psp_call_from_host(addr);
    psp_cpu.r[PSP_REG_GP] = caller_gp;
}

static void title_entered(uint32_t addr) {
    fprintf(stderr, "title: entered recompiled function 0x%08X (module_start)\n", addr);
}

static void title_callback_entered(uint32_t addr) {
    fprintf(stderr, "title: entered recompiled callback 0x%08X\n", addr);
}

int psp_title_image_load(const char *path, uint32_t eboot_info) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "title PRX: cannot open %s\n", path); return -1; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    uint8_t *d = n > 52 ? (uint8_t *)malloc((size_t)n) : NULL;
    if (!d || fread(d, 1, (size_t)n, f) != (size_t)n) {
        fclose(f); free(d);
        fprintf(stderr, "title PRX: cannot read %s\n", path);
        return -1;
    }
    fclose(f);

    if (memcmp(d, "\x7f" "ELF", 4) != 0 || d[4] != 1 || d[5] != 1 ||
        rd16(d + 18) != 8) {
        free(d);
        fprintf(stderr, "title PRX: not an ELF32 MIPS LE image\n");
        return -1;
    }

    uint32_t phoff = rd32(d + 28);
    uint16_t phentsize = rd16(d + 42), phnum = rd16(d + 44);
    if (phentsize < 32 || phoff + (uint32_t)phnum * phentsize > (uint32_t)n) {
        free(d);
        fprintf(stderr, "title PRX: bad program header table\n");
        return -1;
    }

    uint32_t off = 0, vaddr = 0, filesz = 0, memsz = 0;
    int ns = 0;
    for (uint16_t i = 0; i < phnum; i++) {
        const uint8_t *p = d + phoff + (uint32_t)i * phentsize;
        if (rd32(p) != 1) continue;               /* PT_LOAD */
        if (ns++) { free(d); fprintf(stderr, "title PRX: expected one PT_LOAD\n"); return -1; }
        off = rd32(p + 4);
        vaddr = rd32(p + 8);
        filesz = rd32(p + 16);
        memsz = rd32(p + 20);
    }
    if (ns != 1 || vaddr != TITLE_BASE) {
        free(d);
        fprintf(stderr, "title PRX: image is not one segment at 0x%08X\n", TITLE_BASE);
        return -1;
    }
    if (filesz > memsz || (uint64_t)off + filesz > (uint64_t)n ||
        (uint64_t)vaddr + memsz > TITLE_END_MAX) {
        free(d);
        fprintf(stderr, "title PRX: segment outside the reserved window\n");
        return -1;
    }
    if (psp_mem_map_extra(vaddr, memsz) != 0 ||
        psp_mem_write_block(vaddr, d + off, filesz) != 0) {
        free(d);
        fprintf(stderr, "title PRX: cannot place the image at 0x%08X\n", vaddr);
        return -1;
    }
    if (psp_read32(vaddr) != rd32(d + off)) {
        free(d);
        fprintf(stderr, "title PRX: read-back mismatch\n");
        return -1;
    }
    if (memsz > filesz && psp_read8(vaddr + filesz) != 0) {
        free(d);
        fprintf(stderr, "title PRX: .bss is not zero\n");
        return -1;
    }

    /* The module info is at p_paddr of the PT_LOAD (a file offset in a PRX). */
    uint32_t modoff = rd32(d + phoff + 12) & 0x7FFFFFFFu;
    if (modoff < off || modoff >= off + filesz) {
        free(d);
        fprintf(stderr, "title PRX: module info is outside the segment\n");
        return -1;
    }
    uint32_t info = vaddr + (modoff - off);
    char modname[29];
    for (int i = 0; i < 28; i++) modname[i] = (char)psp_read8(info + 4u + (uint32_t)i);
    modname[28] = 0;
    if (strcmp(modname, "modtitle") != 0) {
        free(d);
        fprintf(stderr, "title PRX: module is '%s', expected modtitle\n", modname);
        return -1;
    }
    g_title_gp = psp_read32(info + 0x20);

    title_psp_recomp_register();
    if (!psp_lookup(TITLE_BASE)) {
        free(d);
        fprintf(stderr, "title PRX: dispatch entry at 0x%08X is missing\n", TITLE_BASE);
        return -1;
    }
    if (build_exports(eboot_info ? eboot_info : EBOOT_INFO) != 0) {
        free(d);
        return -1;
    }
    /* Every dispatch into the module image runs with its $gp; the caller's is
     * restored on return. Imports switch to the EBOOT's in the forwarder. */
    psp_dispatch_module_gp(TITLE_BASE, TITLE_BASE + memsz, g_title_gp);
    g_title_loaded = 1;
    free(d);
    fprintf(stderr,
            "title PRX 0x%08X..0x%08X (one segment, %u bytes .bss), gp 0x%08X "
            "(EBOOT 0x%08X), %d libehsys exports, %d imports bound\n",
            vaddr, vaddr + memsz, memsz - filesz, g_title_gp, g_eboot_gp,
            g_nexports, 197);
    return 0;
}

int psp_title_load_uid(const char *path, uint32_t *uid) {
    if (!g_title_loaded) return 0;
    const char *slash = strrchr(path, '/');
    if (strcmp(path, TITLE_LBN_PATH) != 0 &&
        (!slash || strcmp(slash + 1, "libtitle.prx") != 0))
        return 0;
    *uid = TITLE_UID;
    return 1;
}

/* Called from the single ModuleMgr handler in font_prx.c. The module_start
 * receives the same argc/argp the EBOOT passed to sceKernelStartModule. */
int psp_title_start(uint32_t uid, uint32_t status_ptr) {
    if (uid != TITLE_UID || !g_title_loaded) return 0;
    psp_cpu.r[PSP_REG_A0] = psp_arg(1);
    psp_cpu.r[PSP_REG_A1] = psp_arg(2);
    psp_trace_watch(TITLE_BASE, title_entered);
    psp_call_from_host(TITLE_BASE);
    psp_trace_watch_label(TITLE_BASE + 0x34u, title_callback_entered);
    if (status_ptr) psp_write32(status_ptr, 0);
    psp_ret(0);
    return 1;
}
