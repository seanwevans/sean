// dv_heap_mt.c - mutex-guarded heap wrapper.

#include "dv_heap_mt.h"

// A failed lock or unlock means the mutex is corrupt or the caller violated
// the ownership contract; neither is recoverable inside a container, so the
// result is checked in debug builds and deliberately ignored otherwise.
static void dv_heap_mt_lock(dv_heap_mt_t *m) {
  const int err = pthread_mutex_lock(&m->lock);
  DV_HEAP_ASSERT(err == 0);
  (void)err;
}

static void dv_heap_mt_unlock(dv_heap_mt_t *m) {
  const int err = pthread_mutex_unlock(&m->lock);
  DV_HEAP_ASSERT(err == 0);
  (void)err;
}

dv_heap_status_t dv_heap_mt_init(dv_heap_mt_t *m,
                                 const dv_heap_config_t *config,
                                 const dv_heap_allocator_t *allocator) {
  if (!m)
    return DV_HEAP_ERR_INVALID;

  if (pthread_mutex_init(&m->lock, NULL) != 0) {
    memset(&m->heap, 0, sizeof(m->heap));
    return DV_HEAP_ERR_NOMEM;
  }

  const dv_heap_status_t status = dv_heap_init(&m->heap, config, allocator);
  if (status != DV_HEAP_OK)
    (void)pthread_mutex_destroy(&m->lock);

  return status;
}

void dv_heap_mt_destroy(dv_heap_mt_t *m) {
  if (!m)
    return;

  dv_heap_destroy(&m->heap);
  (void)pthread_mutex_destroy(&m->lock);
}

dv_heap_status_t dv_heap_mt_push(dv_heap_mt_t *m, const void *elem) {
  if (!m)
    return DV_HEAP_ERR_INVALID;

  dv_heap_mt_lock(m);
  const dv_heap_status_t status = dv_heap_push(&m->heap, elem);
  dv_heap_mt_unlock(m);
  return status;
}

dv_heap_status_t dv_heap_mt_pop(dv_heap_mt_t *m, void *out) {
  if (!m)
    return DV_HEAP_ERR_INVALID;

  dv_heap_mt_lock(m);
  const dv_heap_status_t status = dv_heap_pop(&m->heap, out);
  dv_heap_mt_unlock(m);
  return status;
}

dv_heap_status_t dv_heap_mt_peek(dv_heap_mt_t *m, void *out) {
  if (!m)
    return DV_HEAP_ERR_INVALID;

  dv_heap_mt_lock(m);
  const dv_heap_status_t status = dv_heap_peek(&m->heap, out);
  dv_heap_mt_unlock(m);
  return status;
}

dv_heap_status_t dv_heap_mt_replace_top(dv_heap_mt_t *m, const void *elem,
                                        void *out) {
  if (!m)
    return DV_HEAP_ERR_INVALID;

  dv_heap_mt_lock(m);
  const dv_heap_status_t status = dv_heap_replace_top(&m->heap, elem, out);
  dv_heap_mt_unlock(m);
  return status;
}

size_t dv_heap_mt_push_bulk(dv_heap_mt_t *m, const void *elems, size_t count) {
  if (!m)
    return 0u;

  dv_heap_mt_lock(m);
  const size_t pushed = dv_heap_push_bulk(&m->heap, elems, count);
  dv_heap_mt_unlock(m);
  return pushed;
}

size_t dv_heap_mt_pop_bulk(dv_heap_mt_t *m, void *out, size_t count) {
  if (!m)
    return 0u;

  dv_heap_mt_lock(m);
  const size_t popped = dv_heap_pop_bulk(&m->heap, out, count);
  dv_heap_mt_unlock(m);
  return popped;
}

size_t dv_heap_mt_size(dv_heap_mt_t *m) {
  if (!m)
    return 0u;

  dv_heap_mt_lock(m);
  const size_t size = dv_heap_size(&m->heap);
  dv_heap_mt_unlock(m);
  return size;
}

bool dv_heap_mt_is_empty(dv_heap_mt_t *m) { return dv_heap_mt_size(m) == 0u; }

void dv_heap_mt_clear(dv_heap_mt_t *m) {
  if (!m)
    return;

  dv_heap_mt_lock(m);
  dv_heap_clear(&m->heap);
  dv_heap_mt_unlock(m);
}

dv_heap_status_t dv_heap_mt_reserve(dv_heap_mt_t *m, size_t capacity) {
  if (!m)
    return DV_HEAP_ERR_INVALID;

  dv_heap_mt_lock(m);
  const dv_heap_status_t status = dv_heap_reserve(&m->heap, capacity);
  dv_heap_mt_unlock(m);
  return status;
}

dv_heap_status_t dv_heap_mt_with(dv_heap_mt_t *m,
                                 void (*fn)(dv_heap_t *h, void *user_data),
                                 void *user_data) {
  if (!m || !fn)
    return DV_HEAP_ERR_INVALID;

  dv_heap_mt_lock(m);
  fn(&m->heap, user_data);
  dv_heap_mt_unlock(m);
  return DV_HEAP_OK;
}
