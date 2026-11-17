#ifndef SETO_GE_QUEUE_H
#define SETO_GE_QUEUE_H

#include <stdatomic.h>
#include <stdint.h>

/* Fixed-size SPSC ring between the guest (producer) and the GPU thread
 * (consumer). The command payload is copied; the consumer never dereferences
 * guest pointers. */
typedef struct {
    uint32_t type;
    uint32_t color;
} ge_cmd;

enum {
    GE_CMD_TEST_COLOR = 1,
    GE_CMD_QUIT = 2
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
