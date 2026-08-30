// dv_stack_mt.h - opt-in mutex-guarded wrapper around dv_stack_t.
//
// The core stack is unsynchronized on purpose. This wrapper is the explicit
// opt-in for callers that need a shared stack, and it lives in its own
// translation unit so the single-threaded hot path never links pthreads.
//
// The wrapper deliberately exposes no interior pointers: dv_stack_top and
// dv_stack_at hand out addresses that stay valid only until the next mutating
// call, which cannot be honored once the lock is released. Callers that need
// compound critical sections use dv_stack_mt_with.

#ifndef DV_STACK_MT_H
#define DV_STACK_MT_H

#include "dv_stack.h"

#include <pthread.h>

typedef struct {
  alignas(DV_CACHE_LINE_SIZE) pthread_mutex_t lock;
  dv_stack_t stack;
} dv_stack_mt_t;

// On failure the wrapper is left uninitialized and must not be passed to
// dv_stack_mt_destroy; the lock is already released by the failing call.
dv_stack_status_t dv_stack_mt_init(dv_stack_mt_t *m,
                                   const dv_stack_config_t *config,
                                   const dv_stack_allocator_t *allocator);

dv_stack_status_t dv_stack_mt_init_ptr(dv_stack_mt_t *m,
                                       size_t initial_capacity, bool growable,
                                       const dv_stack_allocator_t *allocator);

void dv_stack_mt_destroy(dv_stack_mt_t *m);

dv_stack_status_t dv_stack_mt_push(dv_stack_mt_t *m, const void *elem);
dv_stack_status_t dv_stack_mt_pop(dv_stack_mt_t *m, void *out);

// Copies the top element out; there is no by-reference peek under a lock.
dv_stack_status_t dv_stack_mt_peek(dv_stack_mt_t *m, void *out);

dv_stack_status_t dv_stack_mt_push_ptr(dv_stack_mt_t *m, void *value);
dv_stack_status_t dv_stack_mt_pop_ptr(dv_stack_mt_t *m, void **out);

size_t dv_stack_mt_push_bulk(dv_stack_mt_t *m, const void *elems, size_t count);
size_t dv_stack_mt_pop_bulk(dv_stack_mt_t *m, void *out, size_t count);

size_t dv_stack_mt_size(dv_stack_mt_t *m);
bool dv_stack_mt_is_empty(dv_stack_mt_t *m);
void dv_stack_mt_clear(dv_stack_mt_t *m);
dv_stack_status_t dv_stack_mt_reserve(dv_stack_mt_t *m, size_t capacity);

// Runs `fn` against the wrapped stack with the lock held, so several core
// operations compose atomically. `fn` must not re-enter the wrapper.
dv_stack_status_t dv_stack_mt_with(dv_stack_mt_t *m,
                                   void (*fn)(dv_stack_t *s, void *user_data),
                                   void *user_data);

#endif // DV_STACK_MT_H
