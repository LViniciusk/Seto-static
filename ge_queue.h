#ifndef SETO_GE_QUEUE_H
#define SETO_GE_QUEUE_H

#include <stdatomic.h>
#include <stdint.h>

#include <psprecomp/render.h>

/* Fixed-size SPSC ring between the guest (producer) and the GPU thread
 * (consumer). The command payload is copied; the consumer never dereferences
 * guest pointers. */
#define GE_MAX_VERTS 512
#define GE_TEX_CACHE_SLOTS 64

typedef struct {
    uint32_t type;
    uint32_t color;
    uint32_t prim;
    uint32_t count;
    uint32_t addr;
    uint32_t stride;
    uint32_t fmt;
    int32_t  tex_slot;
    /* GE_CMD_DRAW: optional decoded RGBA8888 (tex_w * tex_h) upload payload for
     * `tex_slot`, allocated by the guest thread and freed by the GPU thread.
     * When `tex == NULL` and `tex_slot >= 0`, reuse the slot's cached GL tex. */
    uint32_t *tex;
    psp_draw_state state;
    psp_vertex verts[GE_MAX_VERTS];
} ge_cmd;

enum {
    GE_CMD_TEST_COLOR = 1,
    GE_CMD_TARGET = 2,
    GE_CMD_DRAW = 3,
    GE_CMD_PRESENT = 4,
    GE_CMD_QUIT = 5
};

typedef struct {
    ge_cmd *buf;
    uint32_t cap;
    uint32_t mask;
    _Atomic uint32_t head;
    _Atomic uint32_t tail;
} ge_queue;

int  ge_queue_init(ge_queue *q, uint32_t capacity); /* power of two */
void ge_queue_destroy(ge_queue *q);
int  ge_queue_push(ge_queue *q, ge_cmd c);          /* 0 = full */
int  ge_queue_pop(ge_queue *q, ge_cmd *out);        /* 0 = empty */

#endif
