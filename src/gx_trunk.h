#ifndef SETO_GX_TRUNK_H
#define SETO_GX_TRUNK_H

#include <stdint.h>

#define GX_EXT_TRUNK_ADDR 0x06600000u
#define GX_EXT_TRUNK_COUNT 1000u
#define GX_EXT_TRUNK_SIZE (GX_EXT_TRUNK_COUNT * 4u)

static inline void gx_trunk_defaults(uint8_t *trunk) {
    for (uint32_t i = 0; i < GX_EXT_TRUNK_SIZE; i++) {
        trunk[i] = 0u;
    }
}

#endif
