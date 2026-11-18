#include "ge_queue.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fail(const char *what) {
    fprintf(stderr, "test-ge-queue: %s\n", what);
    return 1;
}

int main(void) {
    ge_queue q;
    ge_cmd c;
    uint32_t i;

    if (ge_queue_init(&q, 4) != 0) return fail("init");
    if (ge_queue_init(&q, 3) == 0) return fail("accepted non power-of-two");
    ge_queue_destroy(&q);
    if (ge_queue_init(&q, 4) != 0) return fail("re-init");

    for (i = 0; i < 4; i++) {
        c.type = GE_CMD_TEST_COLOR;
        c.color = i;
        if (!ge_queue_push(&q, c)) return fail("push before full");
    }
    c.color = 99;
    if (ge_queue_push(&q, c)) return fail("push into full queue");

    for (i = 0; i < 4; i++) {
        if (!ge_queue_pop(&q, &c)) return fail("pop");
        if (c.color != i) return fail("FIFO order");
    }
    if (ge_queue_pop(&q, &c)) return fail("pop from empty");

    /* Wrap the ring several times and check FIFO. */
    for (i = 0; i < 20; i++) {
        c.type = GE_CMD_TEST_COLOR;
        c.color = i;
        if (!ge_queue_push(&q, c)) return fail("wrap push");
        while (ge_queue_pop(&q, &c)) {
            if (c.color != i) return fail("wrap order");
        }
    }

    /* Verify GE_CMD_DRAW payload copy (tex_slot, count, verts, tex pointer). */
    memset(&c, 0, sizeof c);
    c.type = GE_CMD_DRAW;
    c.prim = PSP_PRIM_TRIANGLES;
    c.count = 1;
    c.tex_slot = 7;
    c.tex = (uint32_t *)malloc(16);
    if (!c.tex) return fail("malloc tex");
    c.tex[0] = 0xDEADBEEFu;
    c.verts[0].x = 123;
    c.verts[0].y = 45;
    c.verts[0].rgba = 0x11223344u;
    if (!ge_queue_push(&q, c)) {
        free(c.tex);
        return fail("push draw");
    }
    memset(&c, 0, sizeof c);
    if (!ge_queue_pop(&q, &c)) return fail("pop draw");
    if (c.type != GE_CMD_DRAW || c.tex_slot != 7 || c.count != 1 ||
        c.verts[0].x != 123 || c.verts[0].y != 45 || c.verts[0].rgba != 0x11223344u ||
        !c.tex || c.tex[0] != 0xDEADBEEFu) {
        free(c.tex);
        return fail("draw payload mismatch");
    }
    free(c.tex);

    /* Leave an unpopped command with malloc'd tex so destroy cleans it up. */
    memset(&c, 0, sizeof c);
    c.type = GE_CMD_DRAW;
    c.tex_slot = 0;
    c.tex = (uint32_t *)malloc(16);
    if (!c.tex || !ge_queue_push(&q, c)) {
        free(c.tex);
        return fail("push unpopped draw");
    }

    ge_queue_destroy(&q);
    printf("test-ge-queue: ok\n");
    return 0;
}
