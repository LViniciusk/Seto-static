#include <psprecomp/hle.h>
#include <psprecomp/mem.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <windows.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "failed line %d: %s\n", __LINE__, #x); return 1; } } while (0)

static uint32_t call(uint32_t nid, uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e) {
    psp_cpu.r[PSP_REG_A0] = a; psp_cpu.r[PSP_REG_A1] = b;
    psp_cpu.r[PSP_REG_A2] = c; psp_cpu.r[PSP_REG_A3] = d;
    psp_cpu.r[PSP_REG_T0] = e;
    psp_hle_call(nid);
    return psp_cpu.r[PSP_REG_V0];
}

int main(void) {
    char dir[MAX_PATH], path[MAX_PATH];
    const char *name = "disc0:/sce_lbn0x1_size0x8";
    CHECK(GetTempPathA(MAX_PATH, dir) > 0);
    CHECK(GetTempFileNameA(dir, "s7", 0, path));
    FILE *f = fopen(path, "wb");
    CHECK(f);
    for (int i = 0; i < 2056; i++) CHECK(fputc(i < 2048 ? 0x11 : i - 2048, f) != EOF);
    CHECK(fclose(f) == 0);
    CHECK(psp_mem_init() == 0);
    psp_hle_init();
    psp_io_set_umd(path);
    for (unsigned i = 0; i <= strlen(name); i++) psp_write8(0x08800000u + i, name[i]);
    uint32_t fd = call(0x109F50BC, 0x08800000, 1, 0, 0, 0);
    CHECK(fd < 64);
    CHECK(call(0x27EB27B8, fd, 0, 3, 0, 0) == 3);
    CHECK(call(0x6A638D83, fd, 0x08800100, 1, 0, 0) == 1);
    CHECK(psp_read8(0x08800100) == 3);
    CHECK(call(0x27EB27B8, fd, 0, (uint32_t)-2, UINT32_MAX, 1) == 2);
    CHECK(call(0x27EB27B8, fd, 0, (uint32_t)-1, UINT32_MAX, 2) == 7);
    CHECK(call(0x6A638D83, fd, 0x08800100, 4, 0, 0) == 1);
    CHECK(psp_read8(0x08800100) == 7);
    CHECK(call(0x27EB27B8, fd, 0, (uint32_t)-9, UINT32_MAX, 2) == UINT32_MAX);
    CHECK(call(0x810C4BC3, fd, 0, 0, 0, 0) == 0);
    CHECK(remove(path) == 0);
    puts("LBN seek: PASS");
    return 0;
}
