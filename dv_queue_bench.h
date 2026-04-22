#ifndef DV_QUEUE_BENCH_H
#define DV_QUEUE_BENCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dvq_bench_handle dvq_bench_handle;

dvq_bench_handle *dvq_bench_create(size_t capacity);
void dvq_bench_destroy(dvq_bench_handle *h);
bool dvq_bench_enqueue(dvq_bench_handle *h, uintptr_t value);
bool dvq_bench_dequeue(dvq_bench_handle *h, uintptr_t *value);

#ifdef __cplusplus
}
#endif

#endif