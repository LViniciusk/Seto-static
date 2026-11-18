#include "ge_queue.h"

#include <stdlib.h>

int ge_queue_init(ge_queue *q, uint32_t capacity) {
    if (!q || capacity == 0 || (capacity & (capacity - 1)) != 0) return -1;
    q->buf = (ge_cmd *)calloc(capacity, sizeof *q->buf);
    if (!q->buf) return -1;
    q->cap = capacity;
    q->mask = capacity - 1;
    atomic_init(&q->head, 0);
    atomic_init(&q->tail, 0);
    return 0;
}

void ge_queue_destroy(ge_queue *q) {
    ge_cmd c;
    if (!q) return;
    if (q->buf) {
        while (ge_queue_pop(q, &c)) free(c.tex);
        free(q->buf);
    }
    q->buf = NULL;
    q->cap = q->mask = 0;
}

int ge_queue_push(ge_queue *q, ge_cmd c) {
    const uint32_t head = atomic_load_explicit(&q->head, memory_order_relaxed);
    const uint32_t tail = atomic_load_explicit(&q->tail, memory_order_acquire);
    if (head - tail >= q->cap) return 0;
    q->buf[head & q->mask] = c;
    atomic_store_explicit(&q->head, head + 1, memory_order_release);
    return 1;
}

int ge_queue_pop(ge_queue *q, ge_cmd *out) {
    const uint32_t tail = atomic_load_explicit(&q->tail, memory_order_relaxed);
    const uint32_t head = atomic_load_explicit(&q->head, memory_order_acquire);
    if (tail == head) return 0;
    *out = q->buf[tail & q->mask];
    atomic_store_explicit(&q->tail, tail + 1, memory_order_release);
    return 1;
}
