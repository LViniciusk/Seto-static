#ifndef SETO_GX_SAVEDATA_H
#define SETO_GX_SAVEDATA_H

#include "gx_trunk.h"

#ifdef _WIN32
#include <windows.h>
#endif

static const uint8_t gx_save_magic[] = { 'S', 'E', 'T', 'O', 'G', 'X', 1, 0 };

static uint8_t *gx_save_trunk(uint32_t param, const char *file) {
    if (strcmp(file, "DATA0") != 0) return NULL;
    char game[16];
    read_guest_str(param + OFF_GAMENAME, game, sizeof game);
    if (strcmp(game, "ULUS10136") != 0) return NULL;
    return psp_mem_ptr(GX_EXT_TRUNK_ADDR, GX_EXT_TRUNK_SIZE);
}

/* Write the slot beside its destination and publish it after both payloads close. */
static int gx_save_write(const char *path, const uint8_t *data, size_t size,
                         const uint8_t *trunk) {
    char staging[1100];
    int len = snprintf(staging, sizeof staging, "%s.seto-tmp", path);
    if (len < 0 || (size_t)len >= sizeof staging) return 0;
    FILE *f = fopen(staging, "wb");
    if (!f) return 0;
    int ok = fwrite(data, 1, size, f) == size &&
             fwrite(trunk, 1, GX_EXT_TRUNK_SIZE, f) == GX_EXT_TRUNK_SIZE &&
             fwrite(gx_save_magic, 1, sizeof gx_save_magic, f) == sizeof gx_save_magic;
    if (fclose(f) != 0) ok = 0;
    if (ok) {
#ifdef _WIN32
        ok = MoveFileExA(staging, path, MOVEFILE_REPLACE_EXISTING) != 0;
#else
        ok = rename(staging, path) == 0;
#endif
    }
    if (!ok) remove(staging);
    return ok;
}

/* Restore the private trailer only after HLE has read the native payload. */
static int gx_save_read(FILE *f, long *size, uint8_t *trunk) {
    uint8_t magic[sizeof gx_save_magic];
    const long trailer = GX_EXT_TRUNK_SIZE + sizeof magic;
    int present = *size >= trailer &&
                  fseek(f, -(long)sizeof magic, SEEK_END) == 0 &&
                  fread(magic, 1, sizeof magic, f) == sizeof magic &&
                  memcmp(magic, gx_save_magic, sizeof magic) == 0;
    if (present) {
        *size -= trailer;
        if (fseek(f, *size, SEEK_SET) != 0 ||
            fread(trunk, 1, GX_EXT_TRUNK_SIZE, f) != GX_EXT_TRUNK_SIZE) return -1;
    }
    if (fseek(f, 0, SEEK_SET) != 0) return -1;
    return present;
}

#endif
