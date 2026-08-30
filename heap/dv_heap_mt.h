// dv_heap_mt.h - opt-in mutex-guarded wrapper around dv_heap_t.
//
// The core heap is unsynchronized on purpose. This wrapper is the explicit
// opt-in for callers that need a shared priority queue, and it lives in its own
// translation unit so the single-threaded hot path never links pthreads.
//
// As with the stack wrapper, no interior pointers escape: dv_heap_top and
// dv_heap_at return addresses valid only until the next mutating call, which
// cannot be honored once the lock is released. Compound critical sections go
// through dv_heap_mt_with.

#ifndef DV_HEAP_MT_H
#define DV_HEAP_MT_H

#include "dv_heap.h"

#include <pthread.h>

typedef struct {
  alignas(DV_CACHE_LINE_SIZE) pthread_mutex_t lock;
  dv_heap_t heap;
} dv_heap_mt_t;

// On failure the wrapper is left uninitialized and must not be passed to
// dv_heap_mt_destroy; the lock is already released by the failing call.
dv_heap_status_t dv_heap_mt_init(dv_heap_mt_t *m,
                                 const dv_heap_config_t *config,
                                 const dv_heap_allocator_t *allocator);

void dv_heap_mt_destroy(dv_heap_mt_t *m);

dv_heap_status_t dv_heap_mt_push(dv_heap_mt_t *m, const void *elem);
dv_heap_status_t dv_heap_mt_pop(dv_heap_mt_t *m, void *out);

// Copies the root out; there is no by-reference peek under a lock.
dv_heap_status_t dv_heap_mt_peek(dv_heap_mt_t *m, void *out);

dv_heap_status_t dv_heap_mt_replace_top(dv_heap_mt_t *m, const void *elem,
                                        void *out);

size_t dv_heap_mt_push_bulk(dv_heap_mt_t *m, const void *elems, size_t count);
size_t dv_heap_mt_pop_bulk(dv_heap_mt_t *m, void *out, size_t count);

size_t dv_heap_mt_size(dv_heap_mt_t *m);
bool dv_heap_mt_is_empty(dv_heap_mt_t *m);
void dv_heap_mt_clear(dv_heap_mt_t *m);
dv_heap_status_t dv_heap_mt_reserve(dv_heap_mt_t *m, size_t capacity);

// Runs `fn` against the wrapped heap with the lock held, so several core
// operations compose atomically. `fn` must not re-enter the wrapper. Index
// based updates belong here: an index obtained under one lock acquisition is
// already stale under the next.
dv_heap_status_t dv_heap_mt_with(dv_heap_mt_t *m,
                                 void (*fn)(dv_heap_t *h, void *user_data),
                                 void *user_data);

#endif // DV_HEAP_MT_H
