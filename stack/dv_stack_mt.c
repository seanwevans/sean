// dv_stack_mt.c - mutex-guarded stack wrapper.

#include "dv_stack_mt.h"

// A failed lock or unlock means the mutex is corrupt or the caller violated
// the ownership contract; neither is recoverable inside a container, so the
// result is checked in debug builds and deliberately ignored otherwise.
static void dv_stack_mt_lock(dv_stack_mt_t *m) {
  const int err = pthread_mutex_lock(&m->lock);
  DV_STACK_ASSERT(err == 0);
  (void)err;
}

static void dv_stack_mt_unlock(dv_stack_mt_t *m) {
  const int err = pthread_mutex_unlock(&m->lock);
  DV_STACK_ASSERT(err == 0);
  (void)err;
}

dv_stack_status_t dv_stack_mt_init(dv_stack_mt_t *m,
                                   const dv_stack_config_t *config,
                                   const dv_stack_allocator_t *allocator) {
  if (!m)
    return DV_STACK_ERR_INVALID;

  if (pthread_mutex_init(&m->lock, NULL) != 0) {
    memset(&m->stack, 0, sizeof(m->stack));
    return DV_STACK_ERR_NOMEM;
  }

  const dv_stack_status_t status = dv_stack_init(&m->stack, config, allocator);
  if (status != DV_STACK_OK)
    (void)pthread_mutex_destroy(&m->lock);

  return status;
}

dv_stack_status_t dv_stack_mt_init_ptr(dv_stack_mt_t *m,
                                       size_t initial_capacity, bool growable,
                                       const dv_stack_allocator_t *allocator) {
  const dv_stack_config_t config = {
      .elem_size = sizeof(void *),
      .elem_align = alignof(void *),
      .initial_capacity = initial_capacity,
      .max_capacity = 0u,
      .growable = growable,
      .cache_aligned = false,
  };

  return dv_stack_mt_init(m, &config, allocator);
}

void dv_stack_mt_destroy(dv_stack_mt_t *m) {
  if (!m)
    return;

  dv_stack_destroy(&m->stack);
  (void)pthread_mutex_destroy(&m->lock);
}

dv_stack_status_t dv_stack_mt_push(dv_stack_mt_t *m, const void *elem) {
  if (!m)
    return DV_STACK_ERR_INVALID;

  dv_stack_mt_lock(m);
  const dv_stack_status_t status = dv_stack_push(&m->stack, elem);
  dv_stack_mt_unlock(m);
  return status;
}

dv_stack_status_t dv_stack_mt_pop(dv_stack_mt_t *m, void *out) {
  if (!m)
    return DV_STACK_ERR_INVALID;

  dv_stack_mt_lock(m);
  const dv_stack_status_t status = dv_stack_pop(&m->stack, out);
  dv_stack_mt_unlock(m);
  return status;
}

dv_stack_status_t dv_stack_mt_peek(dv_stack_mt_t *m, void *out) {
  if (!m)
    return DV_STACK_ERR_INVALID;

  dv_stack_mt_lock(m);
  const dv_stack_status_t status = dv_stack_peek(&m->stack, out);
  dv_stack_mt_unlock(m);
  return status;
}

dv_stack_status_t dv_stack_mt_push_ptr(dv_stack_mt_t *m, void *value) {
  if (!m)
    return DV_STACK_ERR_INVALID;

  dv_stack_mt_lock(m);
  const dv_stack_status_t status = dv_stack_push_ptr(&m->stack, value);
  dv_stack_mt_unlock(m);
  return status;
}

dv_stack_status_t dv_stack_mt_pop_ptr(dv_stack_mt_t *m, void **out) {
  if (!m)
    return DV_STACK_ERR_INVALID;

  dv_stack_mt_lock(m);
  const dv_stack_status_t status = dv_stack_pop_ptr(&m->stack, out);
  dv_stack_mt_unlock(m);
  return status;
}

size_t dv_stack_mt_push_bulk(dv_stack_mt_t *m, const void *elems,
                             size_t count) {
  if (!m)
    return 0u;

  dv_stack_mt_lock(m);
  const size_t pushed = dv_stack_push_bulk(&m->stack, elems, count);
  dv_stack_mt_unlock(m);
  return pushed;
}

size_t dv_stack_mt_pop_bulk(dv_stack_mt_t *m, void *out, size_t count) {
  if (!m)
    return 0u;

  dv_stack_mt_lock(m);
  const size_t popped = dv_stack_pop_bulk(&m->stack, out, count);
  dv_stack_mt_unlock(m);
  return popped;
}

size_t dv_stack_mt_size(dv_stack_mt_t *m) {
  if (!m)
    return 0u;

  dv_stack_mt_lock(m);
  const size_t size = dv_stack_size(&m->stack);
  dv_stack_mt_unlock(m);
  return size;
}

bool dv_stack_mt_is_empty(dv_stack_mt_t *m) {
  return dv_stack_mt_size(m) == 0u;
}

void dv_stack_mt_clear(dv_stack_mt_t *m) {
  if (!m)
    return;

  dv_stack_mt_lock(m);
  dv_stack_clear(&m->stack);
  dv_stack_mt_unlock(m);
}

dv_stack_status_t dv_stack_mt_reserve(dv_stack_mt_t *m, size_t capacity) {
  if (!m)
    return DV_STACK_ERR_INVALID;

  dv_stack_mt_lock(m);
  const dv_stack_status_t status = dv_stack_reserve(&m->stack, capacity);
  dv_stack_mt_unlock(m);
  return status;
}

dv_stack_status_t dv_stack_mt_with(dv_stack_mt_t *m,
                                   void (*fn)(dv_stack_t *s, void *user_data),
                                   void *user_data) {
  if (!m || !fn)
    return DV_STACK_ERR_INVALID;

  dv_stack_mt_lock(m);
  fn(&m->stack, user_data);
  dv_stack_mt_unlock(m);
  return DV_STACK_OK;
}
