#include "ge_queue.h"

#include <stdio.h>

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

    ge_queue_destroy(&q);
    printf("test-ge-queue: ok\n");
    return 0;
}
