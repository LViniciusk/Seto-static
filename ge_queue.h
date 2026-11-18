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
#define GE_MAX_FBOS 16

typedef struct {
    uint32_t type;
    uint32_t color;
    uint32_t prim;
    uint32_t count;
    uint32_t addr;
    uint32_t stride;
    uint32_t fmt;
    int32_t  fbo_slot;
    int32_t  src_slot;
    int32_t  tex_slot;
    int32_t  fbo_tex_slot;
    int32_t  w;
    int32_t  h;
    int32_t  sx;
    int32_t  sy;
    int32_t  dx;
    int32_t  dy;
    /* Heap payload owned by the queue / GPU thread and freed after use (or on
     * ge_queue_destroy): decoded RGBA8888 texture on GE_CMD_DRAW, bottom-up
     * RGBA8888 target upload on GE_CMD_UPLOAD_TARGET, or bottom-up 480x272
     * RAM frame on GE_CMD_PRESENT when fbo_slot < 0. */
    uint32_t *tex;
    /* Caller-owned synchronous output buffers for GE_CMD_READBACK (NOT freed
     * by the GPU thread or ge_queue_destroy). */
    uint32_t *rb_rgba;
    uint32_t *rb_ds;
    psp_draw_state state;
    psp_vertex verts[GE_MAX_VERTS];
} ge_cmd;

enum {
    GE_CMD_TEST_COLOR = 1,
    GE_CMD_TARGET = 2,
    GE_CMD_DRAW = 3,
    GE_CMD_PRESENT = 4,
    GE_CMD_QUIT = 5,
    GE_CMD_UPLOAD_TARGET = 6,
    GE_CMD_BLIT = 7,
    GE_CMD_READBACK = 8,
    GE_CMD_RESIZE = 9
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
