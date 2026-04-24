#include "dv_queue_bench.h"
#include "dv_queue.h"

struct dvq_bench_handle {
  mpmc_queue_t *q;
};

dvq_bench_handle *dvq_bench_create(size_t capacity) {
  dvq_bench_handle *h =
      (dvq_bench_handle *)dv_aligned_alloc(DV_CACHE_LINE_SIZE, sizeof(*h));
  if (!h)
    return NULL;

  h->q = mpmc_init(capacity);
  if (!h->q) {
    dv_aligned_free(h);
    return NULL;
  }

  return h;
}

void dvq_bench_destroy(dvq_bench_handle *h) {
  if (!h)
    return;

  mpmc_free(h->q);
  dv_aligned_free(h);
}

bool dvq_bench_enqueue(dvq_bench_handle *h, void *value) {
  if (!h)
    return false;
  return mpmc_enqueue(h->q, value);
}

bool dvq_bench_dequeue(dvq_bench_handle *h, void **value) {
  if (!h || !value)
    return false;

  if (!mpmc_dequeue(h->q, value))
    return false;
  return true;
}
