// dv_queue.c - Dmitry Vyukov style queue.

#include "dv_queue.h"

mpmc_queue_t *mpmc_init(size_t buffer_size) {
  if (buffer_size < 2u || (buffer_size & (buffer_size - 1u)) != 0u)
    return NULL;

  mpmc_queue_t *q =
      (mpmc_queue_t *)ALIGNED_ALLOC(DV_CACHE_LINE_SIZE, sizeof(mpmc_queue_t));
  if (!q)
    return NULL;

  if (buffer_size > SIZE_MAX / sizeof(cell_t)) {
    ALIGNED_FREE(q);
    return NULL;
  }

  void *raw_buffer =
      ALIGNED_ALLOC(DV_CACHE_LINE_SIZE, sizeof(cell_t) * buffer_size);
  if (!raw_buffer) {
    ALIGNED_FREE(q);
    return NULL;
  }

  q->buffer = (cell_t *)raw_buffer;
  q->buffer_mask = buffer_size - 1u;

  for (size_t i = 0u; i < buffer_size; ++i) {
    atomic_store_explicit(&q->buffer[i].sequence, i, memory_order_relaxed);
    q->buffer[i].data = NULL;
  }

  atomic_store_explicit(&q->head, 0u, memory_order_relaxed);
  atomic_store_explicit(&q->tail, 0u, memory_order_relaxed);
  atomic_store_explicit(&q->count, 0u, memory_order_relaxed);

  return q;
}

bool mpmc_enqueue(mpmc_queue_t *q, void *data) {
  if (!q)
    return false;

  cell_t *cell;
  size_t pos = atomic_load_explicit(&q->head, memory_order_relaxed);

  for (;;) {
    cell = &q->buffer[pos & q->buffer_mask];
    size_t seq = atomic_load_explicit(&cell->sequence, memory_order_acquire);
    intptr_t dif = (intptr_t)seq - (intptr_t)pos;

    if (dif == 0) {
      if (atomic_compare_exchange_weak_explicit(&q->head, &pos, pos + 1u,
                                                memory_order_relaxed,
                                                memory_order_relaxed)) {
        cell->data = data;
        atomic_store_explicit(&cell->sequence, pos + 1u, memory_order_release);
        atomic_fetch_add_explicit(&q->count, 1u, memory_order_relaxed);
        return true;
      }
    } else if (dif < 0) {
      return false;
    } else {
      pos = atomic_load_explicit(&q->head, memory_order_relaxed);
      dv_cpu_relax();
    }
  }
}

bool mpmc_dequeue(mpmc_queue_t *q, void **data) {
  if (!q || !data)
    return false;

  *data = NULL;

  cell_t *cell;
  size_t pos = atomic_load_explicit(&q->tail, memory_order_relaxed);

  for (;;) {
    cell = &q->buffer[pos & q->buffer_mask];
    size_t seq = atomic_load_explicit(&cell->sequence, memory_order_acquire);
    intptr_t dif = (intptr_t)seq - (intptr_t)(pos + 1u);

    if (dif == 0) {
      if (atomic_compare_exchange_weak_explicit(&q->tail, &pos, pos + 1u,
                                                memory_order_relaxed,
                                                memory_order_relaxed)) {
        *data = cell->data;
        atomic_store_explicit(&cell->sequence, pos + q->buffer_mask + 1u,
                              memory_order_release);
        atomic_fetch_sub_explicit(&q->count, 1u, memory_order_relaxed);
        return true;
      }
    } else if (dif < 0) {
      return false;
    } else {
      pos = atomic_load_explicit(&q->tail, memory_order_relaxed);
      dv_cpu_relax();
    }
  }
}

size_t mpmc_enqueue_bulk(mpmc_queue_t *q, void **data_array, size_t count) {
  if (!q || !data_array)
    return 0u;

  size_t success_count = 0u;
  for (size_t i = 0u; i < count; ++i) {
    if (mpmc_enqueue(q, data_array[i])) {
      ++success_count;
    } else {
      break;
    }
  }

  return success_count;
}

size_t mpmc_dequeue_bulk(mpmc_queue_t *q, void **data_array, size_t count) {
  if (!q || !data_array)
    return 0u;

  size_t success_count = 0u;
  for (size_t i = 0u; i < count; ++i) {
    if (mpmc_dequeue(q, &data_array[i])) {
      ++success_count;
    } else {
      break;
    }
  }

  return success_count;
}

size_t mpmc_size(mpmc_queue_t *q) {
  if (!q)
    return 0u;
  return atomic_load_explicit(&q->count, memory_order_relaxed);
}

void mpmc_free(mpmc_queue_t *q) {
  if (q) {
    ALIGNED_FREE(q->buffer);
    ALIGNED_FREE(q);
  }
}