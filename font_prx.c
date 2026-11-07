/* SETO-static S5: place the pre-relocated sceFont_Library image in guest RAM.
 *
 * This is not ModuleMgr and does not execute the PRX. It only proves the
 * offline-relocated image can be placed in the reserved RAM window without
 * touching the EBOOT's module mapping, and that .bss is zeroed.
 */
#include <psprecomp/mem.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FONT_BASE       0x08900000u
#define FONT_END_MAX    0x08C00000u   /* runtime heap starts here */
#define EBOOT_END       0x00640000u   /* EBOOT module mapping */

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
    free(d);
    printf("font PRX 0x%08X..0x%08X (2 segments, bss zeroed)\n", lo, hi);
    return 0;
}
