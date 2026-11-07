/* SETO-static S3: the fixed-length pool calls Tag Force's boot uses.
 *
 * sceKernelCreateFpl / AllocateFpl / DeleteFpl only: the game creates one
 * 64 KiB pool and takes its single block, then uses that memory as its own
 * heap. FreeFpl and the Try/CB variants are not imported by this EBOOT.
 */
#include <psprecomp/hle.h>

#include <string.h>

#define MAX_FPL 8

typedef struct {
    int used;
    uint32_t uid;
    uint32_t base;
    uint32_t block;
    uint32_t count;
    uint32_t taken;
} fpl;

static fpl g_fpl[MAX_FPL];
static uint32_t g_next_uid = 0x00050000;

static fpl *find(uint32_t uid) {
    for (int i = 0; i < MAX_FPL; i++)
        if (g_fpl[i].used && g_fpl[i].uid == uid) return &g_fpl[i];
    return NULL;
}

static void hle_CreateFpl(void) {
    /* (name, partition, attr, block size, block count, options) */
    uint32_t block = psp_arg(3), count = psp_arg(4);
    if (!block || !count) { psp_ret(0x800200D2u); return; }

    uint32_t base = psp_sysmem_alloc(block * count, 0);
    if (!base) { psp_ret(SCE_KERNEL_ERROR_NO_MEMORY); return; }

    for (int i = 0; i < MAX_FPL; i++) {
        if (g_fpl[i].used) continue;
        g_fpl[i] = (fpl){ 1, g_next_uid++, base, block, count, 0 };
        psp_ret(g_fpl[i].uid);
        return;
    }
    psp_sysmem_release(base);
    psp_ret(SCE_KERNEL_ERROR_NO_MEMORY);
}

static void hle_AllocateFpl(void) {
    /* (uid, void **block, unsigned int *timeout) */
    fpl *p = find(psp_arg(0));
    if (!p) { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_UID); return; }
    if (p->taken >= p->count) { psp_ret(SCE_KERNEL_ERROR_NO_MEMORY); return; }

    uint32_t addr = p->base + p->taken * p->block;
    p->taken++;
    psp_write32(psp_arg(1), addr);
    psp_ret(0);
}

static void hle_DeleteFpl(void) {
    fpl *p = find(psp_arg(0));
    if (!p) { psp_ret(SCE_KERNEL_ERROR_UNKNOWN_UID); return; }
    psp_sysmem_release(p->base);
    memset(p, 0, sizeof *p);
    psp_ret(0);
}

void psp_fpl_register(void) {
    psp_hle_register(0xC07BB470, "ThreadManForUser", "sceKernelCreateFpl",   hle_CreateFpl);
    psp_hle_register(0xD979E9BF, "ThreadManForUser", "sceKernelAllocateFpl", hle_AllocateFpl);
    psp_hle_register(0xED1410E0, "ThreadManForUser", "sceKernelDeleteFpl",   hle_DeleteFpl);
}
