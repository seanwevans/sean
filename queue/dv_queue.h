#ifndef DV_QUEUE_H
#define DV_QUEUE_H

#if defined(_MSC_VER)
#include <malloc.h>
#endif

#include <stdalign.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#if defined(__aarch64__)
#define dv_cpu_relax() __asm__ volatile("yield" ::: "memory")
#elif defined(__x86_64__) || defined(_M_X64) || defined(__i386__) ||           \
    defined(_M_IX86)
#include <emmintrin.h>
#define dv_cpu_relax() _mm_pause()
#else
#define dv_cpu_relax() atomic_signal_fence(memory_order_seq_cst)
#endif

#ifndef DV_CACHE_LINE_SIZE
#if defined(__aarch64__)
#define DV_CACHE_LINE_SIZE 128u
#elif defined(__x86_64__) || defined(_M_X64) || defined(__i386__) ||           \
    defined(_M_IX86)
#define DV_CACHE_LINE_SIZE 64u
#else
#define DV_CACHE_LINE_SIZE 32u
#endif
#endif

#ifndef THREAD_COUNT
#define THREAD_COUNT 8u
#endif

#ifndef ITEMS_PER_THREAD
#define ITEMS_PER_THREAD 1000000u
#endif

#ifndef QUEUE_SIZE
#define QUEUE_SIZE 65536u
#endif

static inline bool dv_is_power_of_two(size_t x) {
  return x != 0u && (x & (x - 1u)) == 0u;
}

static inline bool dv_align_up_size(size_t size, size_t align, size_t *out) {
  if (!out || !dv_is_power_of_two(align))
    return false;

  if (size > SIZE_MAX - (align - 1u))
    return false;

  *out = (size + align - 1u) & ~(align - 1u);
  return true;
}

static inline void *dv_aligned_alloc(size_t align, size_t size) {
  if (!dv_is_power_of_two(align))
    return NULL;

#if defined(_MSC_VER)
  return _aligned_malloc(size, align);
#else
  size_t padded = 0u;
  if (!dv_align_up_size(size, align, &padded))
    return NULL;
  return aligned_alloc(align, padded);
#endif
}

static inline void dv_aligned_free(void *ptr) {
#if defined(_MSC_VER)
  _aligned_free(ptr);
#else
  free(ptr);
#endif
}

#define ALIGNED_ALLOC(align, size) dv_aligned_alloc((align), (size))
#define ALIGNED_FREE(ptr) dv_aligned_free((ptr))

typedef struct {
  alignas(DV_CACHE_LINE_SIZE) atomic_size_t sequence;
  void *data;
} cell_t;

_Static_assert(sizeof(cell_t) <= DV_CACHE_LINE_SIZE,
               "cell_t exceeds cache line");

typedef struct {
  alignas(DV_CACHE_LINE_SIZE) cell_t *buffer;
  size_t buffer_mask;

  alignas(DV_CACHE_LINE_SIZE) atomic_size_t head;
  alignas(DV_CACHE_LINE_SIZE) atomic_size_t tail;
  alignas(DV_CACHE_LINE_SIZE) atomic_size_t count;
} mpmc_queue_t;

mpmc_queue_t *mpmc_init(size_t buffer_size);
bool mpmc_enqueue(mpmc_queue_t *q, void *data);
bool mpmc_dequeue(mpmc_queue_t *q, void **data);
size_t mpmc_enqueue_bulk(mpmc_queue_t *q, void **data_array, size_t count);
size_t mpmc_dequeue_bulk(mpmc_queue_t *q, void **data_array, size_t count);
size_t mpmc_size(mpmc_queue_t *q);
void mpmc_free(mpmc_queue_t *q);

#endif