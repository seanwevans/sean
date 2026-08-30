// unit_tests.c

#include "dv_heap.h"
#include "dv_heap_mt.h"

#include <inttypes.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_CAPACITY 8u
#define DIFF_OPS 20000u
#define MT_THREADS 4u
#define MT_ITEMS_PER_THREAD 10000u

#define ASSERT_TRUE(expr)                                                      \
  do {                                                                         \
    if (!(expr)) {                                                             \
      fprintf(stderr, "ASSERT_TRUE failed: %s (%s:%d)\n", #expr, __FILE__,     \
              __LINE__);                                                       \
      return false;                                                            \
    }                                                                          \
  } while (0)

#define ASSERT_FALSE(expr) ASSERT_TRUE(!(expr))

#define ASSERT_EQ_U64(a, b)                                                    \
  do {                                                                         \
    const uint64_t _a = (uint64_t)(a);                                         \
    const uint64_t _b = (uint64_t)(b);                                         \
    if (_a != _b) {                                                            \
      fprintf(stderr,                                                          \
              "ASSERT_EQ_U64 failed: %s != %s (got=%llu expected=%llu) "       \
              "(%s:%d)\n",                                                     \
              #a, #b, (unsigned long long)_a, (unsigned long long)_b,          \
              __FILE__, __LINE__);                                             \
      return false;                                                            \
    }                                                                          \
  } while (0)

#define ASSERT_PTR_EQ(a, b)                                                    \
  do {                                                                         \
    void *_a = (void *)(a);                                                    \
    void *_b = (void *)(b);                                                    \
    if (_a != _b) {                                                            \
      fprintf(stderr,                                                          \
              "ASSERT_PTR_EQ failed: %s != %s (got=%p expected=%p) (%s:%d)\n", \
              #a, #b, _a, _b, __FILE__, __LINE__);                             \
      return false;                                                            \
    }                                                                          \
  } while (0)

#define ASSERT_STATUS(expr, expected)                                          \
  do {                                                                         \
    const dv_heap_status_t _s = (expr);                                        \
    const dv_heap_status_t _e = (expected);                                    \
    if (_s != _e) {                                                            \
      fprintf(stderr,                                                          \
              "ASSERT_STATUS failed: %s -> %s (expected %s) (%s:%d)\n", #expr, \
              dv_heap_status_str(_s), dv_heap_status_str(_e), __FILE__,        \
              __LINE__);                                                       \
      return false;                                                            \
    }                                                                          \
  } while (0)

#define ASSERT_OK(expr) ASSERT_STATUS((expr), DV_HEAP_OK)

#define ASSERT_VALID(h)                                                        \
  do {                                                                         \
    if (!dv_heap_is_valid(h)) {                                                \
      fprintf(stderr, "heap-order property violated: %s (%s:%d)\n", #h,        \
              __FILE__, __LINE__);                                             \
      return false;                                                            \
    }                                                                          \
  } while (0)

typedef bool (*test_fn)(void);

typedef struct {
  const char *name;
  test_fn fn;
} test_case_t;

// --- Test allocator ----------------------------------------------------

typedef struct {
  size_t live_bytes;
  size_t peak_bytes;
  size_t alloc_calls;
  size_t realloc_calls;
  size_t free_calls;
  size_t budget; // SIZE_MAX means unlimited
  size_t last_align;
  bool align_respected;
} tracking_state_t;

static void tracking_init(tracking_state_t *st) {
  memset(st, 0, sizeof(*st));
  st->budget = SIZE_MAX;
  st->align_respected = true;
}

static bool tracking_take_budget(tracking_state_t *st) {
  if (st->budget == SIZE_MAX)
    return true;
  if (st->budget == 0u)
    return false;

  --st->budget;
  return true;
}

static void *tracking_raw_alloc(size_t size, size_t align) {
  const size_t padded = (size + align - 1u) & ~(align - 1u);
  return aligned_alloc(align, padded);
}

static void *tracking_alloc(size_t size, size_t align, void *user_data) {
  tracking_state_t *st = (tracking_state_t *)user_data;

  st->last_align = align;
  ++st->alloc_calls;
  if (!tracking_take_budget(st))
    return NULL;

  void *mem = tracking_raw_alloc(size, align);
  if (!mem)
    return NULL;

  if (((uintptr_t)mem & (uintptr_t)(align - 1u)) != 0u)
    st->align_respected = false;

  st->live_bytes += size;
  if (st->live_bytes > st->peak_bytes)
    st->peak_bytes = st->live_bytes;

  return mem;
}

static void tracking_free(void *ptr, size_t size, size_t align,
                          void *user_data) {
  tracking_state_t *st = (tracking_state_t *)user_data;
  (void)align;

  if (!ptr)
    return;

  ++st->free_calls;
  st->live_bytes -= size;
  free(ptr);
}

static void *tracking_realloc(void *ptr, size_t old_size, size_t new_size,
                              size_t align, void *user_data) {
  tracking_state_t *st = (tracking_state_t *)user_data;

  st->last_align = align;
  ++st->realloc_calls;
  if (!tracking_take_budget(st))
    return NULL;

  void *mem = tracking_raw_alloc(new_size, align);
  if (!mem)
    return NULL;

  if (((uintptr_t)mem & (uintptr_t)(align - 1u)) != 0u)
    st->align_respected = false;

  const size_t copy = old_size < new_size ? old_size : new_size;
  if (copy > 0u)
    memcpy(mem, ptr, copy);
  free(ptr);

  st->live_bytes -= old_size;
  st->live_bytes += new_size;
  if (st->live_bytes > st->peak_bytes)
    st->peak_bytes = st->live_bytes;

  return mem;
}

static dv_heap_allocator_t tracking_hooks(tracking_state_t *st,
                                          bool with_realloc) {
  const dv_heap_allocator_t hooks = {
      .alloc = tracking_alloc,
      .realloc = with_realloc ? tracking_realloc : NULL,
      .free = tracking_free,
      .user_data = st,
  };
  return hooks;
}

// --- Helpers -----------------------------------------------------------

static dv_heap_config_t u64_config(dv_heap_order_t order,
                                   size_t initial_capacity, bool growable) {
  const dv_heap_config_t config = {
      .elem_size = sizeof(uint64_t),
      .elem_align = alignof(uint64_t),
      .initial_capacity = initial_capacity,
      .max_capacity = 0u,
      .growable = growable,
      .cache_aligned = false,
      .order = order,
      .key_type = DV_HEAP_KEY_U64,
      .key_offset = 0u,
      .key_indirect = false,
      .compare = NULL,
      .compare_user_data = NULL,
      .on_move = NULL,
      .move_user_data = NULL,
  };
  return config;
}

static uint64_t xorshift64(uint64_t *state) {
  uint64_t x = *state;
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  *state = x;
  return x;
}

// --- Configuration -----------------------------------------------------

static int dummy_compare(const void *a, const void *b, void *user_data) {
  (void)a;
  (void)b;
  (void)user_data;
  return 0;
}

static bool test_init_rejects_bad_config(void) {
  dv_heap_t h;
  const dv_heap_config_t valid = u64_config(DV_HEAP_MIN, TEST_CAPACITY, false);

  ASSERT_STATUS(dv_heap_init(NULL, &valid, NULL), DV_HEAP_ERR_INVALID);
  ASSERT_STATUS(dv_heap_init(&h, NULL, NULL), DV_HEAP_ERR_INVALID);

  dv_heap_config_t bad = valid;
  bad.elem_size = 0u;
  ASSERT_STATUS(dv_heap_init(&h, &bad, NULL), DV_HEAP_ERR_INVALID);

  bad = valid;
  bad.elem_align = 3u;
  ASSERT_STATUS(dv_heap_init(&h, &bad, NULL), DV_HEAP_ERR_INVALID);

  bad = valid;
  bad.elem_size = 6u;
  bad.elem_align = 4u;
  ASSERT_STATUS(dv_heap_init(&h, &bad, NULL), DV_HEAP_ERR_INVALID);

  bad = valid;
  bad.order = (dv_heap_order_t)7;
  ASSERT_STATUS(dv_heap_init(&h, &bad, NULL), DV_HEAP_ERR_INVALID);

  bad = valid;
  bad.key_type = (dv_heap_key_type_t)99;
  ASSERT_STATUS(dv_heap_init(&h, &bad, NULL), DV_HEAP_ERR_INVALID);

  // A custom ordering without a comparator has no ordering at all.
  bad = valid;
  bad.key_type = DV_HEAP_KEY_CUSTOM;
  bad.compare = NULL;
  ASSERT_STATUS(dv_heap_init(&h, &bad, NULL), DV_HEAP_ERR_INVALID);

  // The key must fit inside the element.
  bad = valid;
  bad.key_offset = 4u; // an 8-byte key at offset 4 of an 8-byte element
  ASSERT_STATUS(dv_heap_init(&h, &bad, NULL), DV_HEAP_ERR_INVALID);

  bad = valid;
  bad.elem_size = 4u;
  bad.elem_align = 4u;
  bad.key_type = DV_HEAP_KEY_U64; // an 8-byte key in a 4-byte element
  ASSERT_STATUS(dv_heap_init(&h, &bad, NULL), DV_HEAP_ERR_INVALID);

  // An indirect key requires a pointer-sized slot to dereference.
  bad = valid;
  bad.elem_size = 16u;
  bad.elem_align = 8u;
  bad.key_indirect = true;
  ASSERT_STATUS(dv_heap_init(&h, &bad, NULL), DV_HEAP_ERR_INVALID);

  bad = valid;
  bad.growable = true;
  bad.initial_capacity = 16u;
  bad.max_capacity = 8u;
  ASSERT_STATUS(dv_heap_init(&h, &bad, NULL), DV_HEAP_ERR_INVALID);

  tracking_state_t st;
  tracking_init(&st);
  dv_heap_allocator_t hooks = tracking_hooks(&st, true);
  hooks.free = NULL;
  ASSERT_STATUS(dv_heap_init(&h, &valid, &hooks), DV_HEAP_ERR_INVALID);

  hooks = tracking_hooks(&st, true);
  hooks.alloc = NULL;
  ASSERT_STATUS(dv_heap_init(&h, &valid, &hooks), DV_HEAP_ERR_INVALID);

  dv_heap_destroy(&h); // a rejected init leaves a zeroed, destroyable block

  // A custom ordering ignores key_offset entirely, so an offset that would be
  // out of bounds for a built-in key is accepted here.
  dv_heap_config_t custom = valid;
  custom.key_type = DV_HEAP_KEY_CUSTOM;
  custom.compare = dummy_compare;
  custom.key_offset = 4096u;
  ASSERT_OK(dv_heap_init(&h, &custom, NULL));
  dv_heap_destroy(&h);

  ASSERT_OK(dv_heap_init(&h, &valid, NULL));
  dv_heap_destroy(&h);
  return true;
}

static bool test_empty_heap_accessors(void) {
  dv_heap_t h;
  const dv_heap_config_t config = u64_config(DV_HEAP_MIN, TEST_CAPACITY, false);
  uint64_t out = 0u;

  ASSERT_EQ_U64(dv_heap_size(NULL), 0u);
  ASSERT_EQ_U64(dv_heap_capacity(NULL), 0u);
  ASSERT_EQ_U64(dv_heap_elem_size(NULL), 0u);
  ASSERT_TRUE(dv_heap_is_empty(NULL));
  ASSERT_TRUE(dv_heap_is_full(NULL));
  ASSERT_PTR_EQ(dv_heap_top(NULL), NULL);
  ASSERT_PTR_EQ(dv_heap_at(NULL, 0u), NULL);
  ASSERT_TRUE(dv_heap_at_const(NULL, 0u) == NULL);
  ASSERT_TRUE(dv_heap_top_const(NULL) == NULL);
  ASSERT_FALSE(dv_heap_is_ptr_mode(NULL));
  ASSERT_FALSE(dv_heap_is_valid(NULL));
  dv_heap_clear(NULL);
  dv_heap_destroy(NULL);

  ASSERT_STATUS(dv_heap_push(NULL, &out), DV_HEAP_ERR_INVALID);
  ASSERT_STATUS(dv_heap_pop(NULL, &out), DV_HEAP_ERR_INVALID);
  ASSERT_STATUS(dv_heap_peek(NULL, &out), DV_HEAP_ERR_INVALID);
  ASSERT_STATUS(dv_heap_replace_top(NULL, &out, NULL), DV_HEAP_ERR_INVALID);
  ASSERT_STATUS(dv_heap_update_at(NULL, 0u, &out), DV_HEAP_ERR_INVALID);
  ASSERT_STATUS(dv_heap_remove_at(NULL, 0u, NULL), DV_HEAP_ERR_INVALID);
  ASSERT_STATUS(dv_heap_heapify(NULL), DV_HEAP_ERR_INVALID);
  ASSERT_STATUS(dv_heap_build(NULL, &out, 1u), DV_HEAP_ERR_INVALID);
  ASSERT_STATUS(dv_heap_reserve(NULL, 4u), DV_HEAP_ERR_INVALID);
  ASSERT_STATUS(dv_heap_shrink_to_fit(NULL), DV_HEAP_ERR_INVALID);
  ASSERT_STATUS(dv_heap_reset(NULL), DV_HEAP_ERR_INVALID);
  ASSERT_EQ_U64(dv_heap_push_bulk(NULL, &out, 1u), 0u);
  ASSERT_EQ_U64(dv_heap_pop_bulk(NULL, &out, 1u), 0u);

  ASSERT_OK(dv_heap_init(&h, &config, NULL));
  ASSERT_EQ_U64(dv_heap_size(&h), 0u);
  ASSERT_EQ_U64(dv_heap_capacity(&h), TEST_CAPACITY);
  ASSERT_EQ_U64(dv_heap_elem_size(&h), sizeof(uint64_t));
  ASSERT_TRUE(dv_heap_is_empty(&h));
  ASSERT_FALSE(dv_heap_is_full(&h));
  ASSERT_TRUE(dv_heap_is_valid(&h));
  ASSERT_PTR_EQ(dv_heap_top(&h), NULL);

  ASSERT_STATUS(dv_heap_pop(&h, &out), DV_HEAP_ERR_EMPTY);
  ASSERT_STATUS(dv_heap_peek(&h, &out), DV_HEAP_ERR_EMPTY);
  ASSERT_STATUS(dv_heap_replace_top(&h, &out, NULL), DV_HEAP_ERR_EMPTY);
  ASSERT_STATUS(dv_heap_push(&h, NULL), DV_HEAP_ERR_INVALID);
  ASSERT_STATUS(dv_heap_peek(&h, NULL), DV_HEAP_ERR_INVALID);
  ASSERT_STATUS(dv_heap_update_at(&h, 0u, &out), DV_HEAP_ERR_INVALID);
  ASSERT_STATUS(dv_heap_remove_at(&h, 0u, NULL), DV_HEAP_ERR_INVALID);

  // heapify and build on an empty heap are well defined no-ops.
  ASSERT_OK(dv_heap_heapify(&h));
  ASSERT_OK(dv_heap_build(&h, NULL, 0u));
  ASSERT_EQ_U64(dv_heap_size(&h), 0u);

  dv_heap_destroy(&h);
  ASSERT_EQ_U64(dv_heap_capacity(&h), 0u);
  dv_heap_destroy(&h); // idempotent
  return true;
}

static bool test_status_strings(void) {
  const dv_heap_status_t codes[] = {
      DV_HEAP_OK,       DV_HEAP_ERR_INVALID, DV_HEAP_ERR_EMPTY,
      DV_HEAP_ERR_FULL, DV_HEAP_ERR_NOMEM,   DV_HEAP_ERR_OVERFLOW,
  };

  for (size_t i = 0u; i < sizeof(codes) / sizeof(codes[0]); ++i) {
    const char *name = dv_heap_status_str(codes[i]);
    ASSERT_TRUE(name != NULL);
    ASSERT_TRUE(strncmp(name, "DV_HEAP_", 8u) == 0);
  }

  ASSERT_TRUE(strcmp(dv_heap_status_str((dv_heap_status_t)999),
                     "DV_HEAP_ERR_UNKNOWN") == 0);
  return true;
}

// --- Ordering ----------------------------------------------------------

// Drains a heap and confirms the keys come out monotonically in the direction
// the order demands, with the size and the invariant checked at every step.
static bool drain_is_ordered(dv_heap_t *h, dv_heap_order_t order,
                             size_t expected_count) {
  uint64_t previous = order == DV_HEAP_MIN ? 0u : UINT64_MAX;
  size_t seen = 0u;

  while (dv_heap_size(h) > 0u) {
    uint64_t peeked = 0u;
    ASSERT_OK(dv_heap_peek(h, &peeked));
    ASSERT_EQ_U64(peeked, *(const uint64_t *)dv_heap_top(h));

    uint64_t value = 0u;
    ASSERT_OK(dv_heap_pop(h, &value));
    ASSERT_EQ_U64(value, peeked);

    if (order == DV_HEAP_MIN)
      ASSERT_TRUE(value >= previous);
    else
      ASSERT_TRUE(value <= previous);

    previous = value;
    ++seen;
    ASSERT_VALID(h);
  }

  ASSERT_EQ_U64(seen, expected_count);
  return true;
}

static bool run_order_test(dv_heap_order_t order) {
  dv_heap_t h;
  const dv_heap_config_t config = u64_config(order, 0u, true);
  ASSERT_OK(dv_heap_init(&h, &config, NULL));

  uint64_t rng = 0xDEADBEEFCAFEBABEull;
  const size_t count = 2000u;

  for (size_t i = 0u; i < count; ++i) {
    const uint64_t value = xorshift64(&rng) >> 8;
    ASSERT_OK(dv_heap_push(&h, &value));
    ASSERT_VALID(&h);
  }

  ASSERT_EQ_U64(dv_heap_size(&h), count);
  if (!drain_is_ordered(&h, order, count))
    return false;

  dv_heap_destroy(&h);
  return true;
}

static bool test_min_heap_order(void) { return run_order_test(DV_HEAP_MIN); }
static bool test_max_heap_order(void) { return run_order_test(DV_HEAP_MAX); }

// Equal keys must all survive; a heap that drops or duplicates ties is broken
// even though every drain looks sorted.
static bool test_duplicate_keys(void) {
  dv_heap_t h;
  const dv_heap_config_t config = u64_config(DV_HEAP_MIN, 0u, true);
  ASSERT_OK(dv_heap_init(&h, &config, NULL));

  const uint64_t keys[] = {5u, 1u, 5u, 1u, 5u, 3u, 3u, 1u};
  const size_t count = sizeof(keys) / sizeof(keys[0]);

  for (size_t i = 0u; i < count; ++i)
    ASSERT_OK(dv_heap_push(&h, &keys[i]));

  ASSERT_EQ_U64(dv_heap_size(&h), count);

  const uint64_t expected[] = {1u, 1u, 1u, 3u, 3u, 5u, 5u, 5u};
  for (size_t i = 0u; i < count; ++i) {
    uint64_t value = 0u;
    ASSERT_OK(dv_heap_pop(&h, &value));
    ASSERT_EQ_U64(value, expected[i]);
  }

  ASSERT_TRUE(dv_heap_is_empty(&h));
  dv_heap_destroy(&h);
  return true;
}

// --- Key types ---------------------------------------------------------

// Every built-in key type must order its own domain, including the negative
// half of the signed types, which a naive byte comparison would get wrong.
static bool test_builtin_key_types(void) {
  struct {
    dv_heap_key_type_t type;
    size_t elem_size;
    size_t elem_align;
  } cases[] = {
      {DV_HEAP_KEY_U64, sizeof(uint64_t), alignof(uint64_t)},
      {DV_HEAP_KEY_I64, sizeof(int64_t), alignof(int64_t)},
      {DV_HEAP_KEY_U32, sizeof(uint32_t), alignof(uint32_t)},
      {DV_HEAP_KEY_I32, sizeof(int32_t), alignof(int32_t)},
      {DV_HEAP_KEY_F64, sizeof(double), alignof(double)},
      {DV_HEAP_KEY_F32, sizeof(float), alignof(float)},
      {DV_HEAP_KEY_PTR, sizeof(void *), alignof(void *)},
  };

  const double source[] = {3.0, -7.0, 0.0, 42.0, -1.0, 19.0, -100.0, 5.0};
  const size_t count = sizeof(source) / sizeof(source[0]);

  for (size_t c = 0u; c < sizeof(cases) / sizeof(cases[0]); ++c) {
    dv_heap_t h;
    dv_heap_config_t config = u64_config(DV_HEAP_MIN, 0u, true);
    config.elem_size = cases[c].elem_size;
    config.elem_align = cases[c].elem_align;
    config.key_type = cases[c].type;

    ASSERT_OK(dv_heap_init(&h, &config, NULL));

    for (size_t i = 0u; i < count; ++i) {
      // Unsigned and pointer keys cannot carry the negative half, so they are
      // biased into range while preserving the relative order.
      const double raw = source[i];
      unsigned char element[16];
      memset(element, 0, sizeof(element));

      switch (cases[c].type) {
      case DV_HEAP_KEY_U64: {
        const uint64_t v = (uint64_t)(raw + 1000.0);
        memcpy(element, &v, sizeof(v));
        break;
      }
      case DV_HEAP_KEY_I64: {
        const int64_t v = (int64_t)raw;
        memcpy(element, &v, sizeof(v));
        break;
      }
      case DV_HEAP_KEY_U32: {
        const uint32_t v = (uint32_t)(raw + 1000.0);
        memcpy(element, &v, sizeof(v));
        break;
      }
      case DV_HEAP_KEY_I32: {
        const int32_t v = (int32_t)raw;
        memcpy(element, &v, sizeof(v));
        break;
      }
      case DV_HEAP_KEY_F64: {
        memcpy(element, &raw, sizeof(raw));
        break;
      }
      case DV_HEAP_KEY_F32: {
        const float v = (float)raw;
        memcpy(element, &v, sizeof(v));
        break;
      }
      case DV_HEAP_KEY_PTR: {
        void *v = (void *)(uintptr_t)((uint64_t)(raw + 1000.0) * 8u);
        memcpy(element, &v, sizeof(v));
        break;
      }
      case DV_HEAP_KEY_CUSTOM:
        break;
      }

      ASSERT_OK(dv_heap_push(&h, element));
      ASSERT_VALID(&h);
    }

    ASSERT_EQ_U64(dv_heap_size(&h), count);

    // Draining must reproduce the sorted source order under every encoding.
    double sorted[8];
    memcpy(sorted, source, sizeof(source));
    for (size_t i = 0u; i < count; ++i)
      for (size_t j = i + 1u; j < count; ++j)
        if (sorted[j] < sorted[i]) {
          const double tmp = sorted[i];
          sorted[i] = sorted[j];
          sorted[j] = tmp;
        }

    for (size_t i = 0u; i < count; ++i) {
      unsigned char element[16];
      memset(element, 0, sizeof(element));
      ASSERT_OK(dv_heap_pop(&h, element));

      double observed = 0.0;
      switch (cases[c].type) {
      case DV_HEAP_KEY_U64: {
        uint64_t v = 0u;
        memcpy(&v, element, sizeof(v));
        observed = (double)v - 1000.0;
        break;
      }
      case DV_HEAP_KEY_I64: {
        int64_t v = 0;
        memcpy(&v, element, sizeof(v));
        observed = (double)v;
        break;
      }
      case DV_HEAP_KEY_U32: {
        uint32_t v = 0u;
        memcpy(&v, element, sizeof(v));
        observed = (double)v - 1000.0;
        break;
      }
      case DV_HEAP_KEY_I32: {
        int32_t v = 0;
        memcpy(&v, element, sizeof(v));
        observed = (double)v;
        break;
      }
      case DV_HEAP_KEY_F64: {
        memcpy(&observed, element, sizeof(observed));
        break;
      }
      case DV_HEAP_KEY_F32: {
        float v = 0.0f;
        memcpy(&v, element, sizeof(v));
        observed = (double)v;
        break;
      }
      case DV_HEAP_KEY_PTR: {
        void *v = NULL;
        memcpy(&v, element, sizeof(v));
        observed = (double)((uint64_t)(uintptr_t)v / 8u) - 1000.0;
        break;
      }
      case DV_HEAP_KEY_CUSTOM:
        break;
      }

      if (observed != sorted[i]) {
        fprintf(stderr, "key type %d: got %.1f expected %.1f at %zu\n",
                (int)cases[c].type, observed, sorted[i], i);
        return false;
      }
    }

    dv_heap_destroy(&h);
  }

  return true;
}

typedef struct {
  uint64_t tag;
  uint64_t key;
} tagged_t;

// A key living at a non-zero offset inside a larger payload is the common
// scheduling shape: the element is the work item, not the priority.
static bool test_key_offset_in_struct(void) {
  dv_heap_t h;
  dv_heap_config_t config = u64_config(DV_HEAP_MIN, 0u, true);
  config.elem_size = sizeof(tagged_t);
  config.elem_align = alignof(tagged_t);
  config.key_offset = offsetof(tagged_t, key);

  ASSERT_OK(dv_heap_init(&h, &config, NULL));

  const uint64_t keys[] = {9u, 2u, 7u, 1u, 8u, 3u};
  const size_t count = sizeof(keys) / sizeof(keys[0]);

  for (size_t i = 0u; i < count; ++i) {
    const tagged_t item = {.tag = 100u + i, .key = keys[i]};
    ASSERT_OK(dv_heap_push(&h, &item));
    ASSERT_VALID(&h);
  }

  uint64_t previous = 0u;
  for (size_t i = 0u; i < count; ++i) {
    tagged_t item = {0u, 0u};
    ASSERT_OK(dv_heap_pop(&h, &item));
    ASSERT_TRUE(item.key >= previous);
    previous = item.key;

    // The payload must travel with its key through every sift.
    bool matched = false;
    for (size_t j = 0u; j < count; ++j)
      if (keys[j] == item.key && item.tag == 100u + j)
        matched = true;
    ASSERT_TRUE(matched);
  }

  dv_heap_destroy(&h);
  return true;
}

typedef struct {
  uint64_t key;
  size_t heap_index;
  size_t live_index;
  uint64_t tag;
} node_t;

// A pointer payload whose key lives inside the pointed-to object: the heap
// dereferences the slot before applying key_offset.
static bool test_pointer_payload_indirect_key(void) {
  dv_heap_t h;
  dv_heap_config_t config = u64_config(DV_HEAP_MIN, 0u, true);
  config.elem_size = sizeof(void *);
  config.elem_align = alignof(void *);
  config.key_offset = offsetof(node_t, key);
  config.key_indirect = true;

  ASSERT_OK(dv_heap_init(&h, &config, NULL));
  ASSERT_TRUE(dv_heap_is_ptr_mode(&h));

  const size_t count = 64u;
  node_t *nodes = (node_t *)calloc(count, sizeof(*nodes));
  ASSERT_TRUE(nodes != NULL);

  uint64_t rng = 0x1234567890ABCDEFull;
  for (size_t i = 0u; i < count; ++i) {
    nodes[i].key = xorshift64(&rng) >> 32;
    nodes[i].tag = i;
    node_t *ptr = &nodes[i];
    ASSERT_OK(dv_heap_push(&h, &ptr));
    ASSERT_VALID(&h);
  }

  uint64_t previous = 0u;
  for (size_t i = 0u; i < count; ++i) {
    node_t *ptr = NULL;
    ASSERT_OK(dv_heap_pop(&h, &ptr));
    ASSERT_TRUE(ptr != NULL);
    ASSERT_TRUE(ptr->key >= previous);
    previous = ptr->key;
  }

  free(nodes);
  dv_heap_destroy(&h);
  return true;
}

// A custom comparator orders by something the built-ins cannot express.
static int compare_by_digit_sum(const void *a, const void *b, void *user_data) {
  size_t *calls = (size_t *)user_data;
  ++*calls;

  uint64_t x = *(const uint64_t *)a;
  uint64_t y = *(const uint64_t *)b;

  uint64_t sx = 0u;
  uint64_t sy = 0u;
  while (x > 0u) {
    sx += x % 10u;
    x /= 10u;
  }
  while (y > 0u) {
    sy += y % 10u;
    y /= 10u;
  }

  if (sx < sy)
    return -1;
  if (sx > sy)
    return 1;
  return 0;
}

static uint64_t digit_sum(uint64_t x) {
  uint64_t sum = 0u;
  while (x > 0u) {
    sum += x % 10u;
    x /= 10u;
  }
  return sum;
}

static bool test_custom_comparator(void) {
  size_t calls = 0u;
  dv_heap_t h;
  dv_heap_config_t config = u64_config(DV_HEAP_MIN, 0u, true);
  config.key_type = DV_HEAP_KEY_CUSTOM;
  config.compare = compare_by_digit_sum;
  config.compare_user_data = &calls;

  ASSERT_OK(dv_heap_init(&h, &config, NULL));

  const uint64_t values[] = {91u, 12u, 55u, 7u, 40u, 300u, 88u, 19u};
  const size_t count = sizeof(values) / sizeof(values[0]);

  for (size_t i = 0u; i < count; ++i)
    ASSERT_OK(dv_heap_push(&h, &values[i]));

  ASSERT_VALID(&h);
  ASSERT_TRUE(calls > 0u);

  uint64_t previous = 0u;
  for (size_t i = 0u; i < count; ++i) {
    uint64_t value = 0u;
    ASSERT_OK(dv_heap_pop(&h, &value));
    ASSERT_TRUE(digit_sum(value) >= previous);
    previous = digit_sum(value);
  }

  dv_heap_destroy(&h);
  return true;
}

// The same comparator under DV_HEAP_MAX must reverse the order without any
// change to the comparator itself.
static bool test_custom_comparator_max_order(void) {
  size_t calls = 0u;
  dv_heap_t h;
  dv_heap_config_t config = u64_config(DV_HEAP_MAX, 0u, true);
  config.key_type = DV_HEAP_KEY_CUSTOM;
  config.compare = compare_by_digit_sum;
  config.compare_user_data = &calls;

  ASSERT_OK(dv_heap_init(&h, &config, NULL));

  const uint64_t values[] = {91u, 12u, 55u, 7u, 40u, 300u, 88u, 19u};
  const size_t count = sizeof(values) / sizeof(values[0]);

  for (size_t i = 0u; i < count; ++i)
    ASSERT_OK(dv_heap_push(&h, &values[i]));

  uint64_t previous = UINT64_MAX;
  for (size_t i = 0u; i < count; ++i) {
    uint64_t value = 0u;
    ASSERT_OK(dv_heap_pop(&h, &value));
    ASSERT_TRUE(digit_sum(value) <= previous);
    previous = digit_sum(value);
  }

  dv_heap_destroy(&h);
  return true;
}

// --- Capacity ----------------------------------------------------------

static bool test_fixed_capacity_boundary(void) {
  dv_heap_t h;
  const dv_heap_config_t config = u64_config(DV_HEAP_MIN, TEST_CAPACITY, false);
  ASSERT_OK(dv_heap_init(&h, &config, NULL));

  for (uint64_t i = 0u; i < TEST_CAPACITY; ++i) {
    const uint64_t value = TEST_CAPACITY - i;
    ASSERT_OK(dv_heap_push(&h, &value));
  }

  ASSERT_TRUE(dv_heap_is_full(&h));
  ASSERT_EQ_U64(dv_heap_size(&h), TEST_CAPACITY);
  ASSERT_VALID(&h);

  const uint64_t overflow_value = 99u;
  ASSERT_STATUS(dv_heap_push(&h, &overflow_value), DV_HEAP_ERR_FULL);
  ASSERT_EQ_U64(dv_heap_size(&h), TEST_CAPACITY);

  ASSERT_STATUS(dv_heap_reserve(&h, TEST_CAPACITY + 1u), DV_HEAP_ERR_FULL);
  ASSERT_OK(dv_heap_reserve(&h, TEST_CAPACITY));
  ASSERT_OK(dv_heap_shrink_to_fit(&h));
  ASSERT_EQ_U64(dv_heap_capacity(&h), TEST_CAPACITY);

  // Popping and removing at exactly full capacity exercises the scratch slot
  // that sits one past the last element.
  uint64_t value = 0u;
  ASSERT_OK(dv_heap_pop(&h, &value));
  ASSERT_EQ_U64(value, 1u);
  ASSERT_VALID(&h);

  ASSERT_OK(dv_heap_push(&h, &overflow_value));
  ASSERT_TRUE(dv_heap_is_full(&h));
  ASSERT_OK(dv_heap_remove_at(&h, dv_heap_size(&h) / 2u, NULL));
  ASSERT_VALID(&h);

  dv_heap_destroy(&h);
  return true;
}

static bool test_growth_and_ceiling(void) {
  dv_heap_t h;
  dv_heap_config_t config = u64_config(DV_HEAP_MIN, 0u, true);
  config.max_capacity = 10u;
  ASSERT_OK(dv_heap_init(&h, &config, NULL));

  for (uint64_t i = 0u; i < 10u; ++i)
    ASSERT_OK(dv_heap_push(&h, &i));

  ASSERT_EQ_U64(dv_heap_capacity(&h), 10u);
  ASSERT_TRUE(dv_heap_is_full(&h));

  const uint64_t extra = 42u;
  ASSERT_STATUS(dv_heap_push(&h, &extra), DV_HEAP_ERR_FULL);
  ASSERT_STATUS(dv_heap_reserve(&h, 11u), DV_HEAP_ERR_FULL);
  ASSERT_VALID(&h);

  dv_heap_destroy(&h);
  return true;
}

static bool test_reserve_shrink_reset_clear(void) {
  dv_heap_t h;
  const dv_heap_config_t config = u64_config(DV_HEAP_MIN, 4u, true);
  ASSERT_OK(dv_heap_init(&h, &config, NULL));

  ASSERT_OK(dv_heap_reserve(&h, 128u));
  ASSERT_EQ_U64(dv_heap_capacity(&h), 128u);
  ASSERT_OK(dv_heap_reserve(&h, 64u)); // never shrinks
  ASSERT_EQ_U64(dv_heap_capacity(&h), 128u);

  for (uint64_t i = 0u; i < 5u; ++i)
    ASSERT_OK(dv_heap_push(&h, &i));

  ASSERT_OK(dv_heap_shrink_to_fit(&h));
  ASSERT_EQ_U64(dv_heap_capacity(&h), 5u);
  ASSERT_EQ_U64(dv_heap_size(&h), 5u);
  ASSERT_VALID(&h);

  dv_heap_clear(&h);
  ASSERT_EQ_U64(dv_heap_size(&h), 0u);
  ASSERT_EQ_U64(dv_heap_capacity(&h), 5u);

  ASSERT_OK(dv_heap_reserve(&h, 200u));
  for (uint64_t i = 0u; i < 9u; ++i)
    ASSERT_OK(dv_heap_push(&h, &i));

  ASSERT_OK(dv_heap_reset(&h));
  ASSERT_EQ_U64(dv_heap_size(&h), 0u);
  ASSERT_EQ_U64(dv_heap_capacity(&h), 4u);

  ASSERT_OK(dv_heap_shrink_to_fit(&h));
  ASSERT_EQ_U64(dv_heap_capacity(&h), 0u);
  ASSERT_PTR_EQ(h.buffer, NULL);

  const uint64_t value = 5u;
  ASSERT_OK(dv_heap_push(&h, &value)); // still usable
  ASSERT_EQ_U64(dv_heap_size(&h), 1u);

  dv_heap_destroy(&h);
  return true;
}

// --- Maintenance and updates -------------------------------------------

static bool test_build_and_heapify(void) {
  dv_heap_t h;
  const dv_heap_config_t config = u64_config(DV_HEAP_MIN, 0u, true);
  ASSERT_OK(dv_heap_init(&h, &config, NULL));

  const size_t count = 1000u;
  uint64_t *items = (uint64_t *)malloc(count * sizeof(*items));
  ASSERT_TRUE(items != NULL);

  uint64_t rng = 0xABCDEF0123456789ull;
  for (size_t i = 0u; i < count; ++i)
    items[i] = xorshift64(&rng) >> 16;

  ASSERT_OK(dv_heap_build(&h, items, count));
  ASSERT_EQ_U64(dv_heap_size(&h), count);
  ASSERT_VALID(&h);

  // Building over existing contents replaces them rather than appending.
  ASSERT_OK(dv_heap_build(&h, items, 10u));
  ASSERT_EQ_U64(dv_heap_size(&h), 10u);
  ASSERT_VALID(&h);

  ASSERT_OK(dv_heap_build(&h, items, count));

  // Rewriting keys in place breaks the invariant; heapify restores it.
  for (size_t i = 0u; i < count; ++i) {
    uint64_t *slot = (uint64_t *)dv_heap_at(&h, i);
    ASSERT_TRUE(slot != NULL);
    *slot = xorshift64(&rng) >> 16;
  }

  ASSERT_OK(dv_heap_heapify(&h));
  ASSERT_VALID(&h);

  uint64_t previous = 0u;
  for (size_t i = 0u; i < count; ++i) {
    uint64_t value = 0u;
    ASSERT_OK(dv_heap_pop(&h, &value));
    ASSERT_TRUE(value >= previous);
    previous = value;
  }

  free(items);
  dv_heap_destroy(&h);
  return true;
}

// replace_top is the top-k primitive: keep a max-heap of size k and replace
// the root whenever a smaller element arrives.
static bool test_replace_top_top_k(void) {
  const size_t k = 16u;
  dv_heap_t h;
  const dv_heap_config_t config = u64_config(DV_HEAP_MAX, k, false);
  ASSERT_OK(dv_heap_init(&h, &config, NULL));

  const size_t count = 5000u;
  uint64_t *values = (uint64_t *)malloc(count * sizeof(*values));
  ASSERT_TRUE(values != NULL);

  uint64_t rng = 0x0F1E2D3C4B5A6978ull;
  for (size_t i = 0u; i < count; ++i)
    values[i] = xorshift64(&rng) >> 20;

  for (size_t i = 0u; i < count; ++i) {
    if (dv_heap_size(&h) < k) {
      ASSERT_OK(dv_heap_push(&h, &values[i]));
      continue;
    }

    const uint64_t *top = (const uint64_t *)dv_heap_top(&h);
    ASSERT_TRUE(top != NULL);
    if (values[i] < *top) {
      uint64_t displaced = 0u;
      ASSERT_OK(dv_heap_replace_top(&h, &values[i], &displaced));
      ASSERT_TRUE(displaced >= values[i]);
      ASSERT_VALID(&h);
    }
  }

  ASSERT_EQ_U64(dv_heap_size(&h), k);

  // The heap must hold exactly the k smallest values.
  uint64_t *sorted = (uint64_t *)malloc(count * sizeof(*sorted));
  ASSERT_TRUE(sorted != NULL);
  memcpy(sorted, values, count * sizeof(*values));
  for (size_t i = 0u; i < count; ++i)
    for (size_t j = i + 1u; j < count; ++j)
      if (sorted[j] < sorted[i]) {
        const uint64_t tmp = sorted[i];
        sorted[i] = sorted[j];
        sorted[j] = tmp;
      }

  for (size_t i = k; i-- > 0u;) {
    uint64_t value = 0u;
    ASSERT_OK(dv_heap_pop(&h, &value));
    ASSERT_EQ_U64(value, sorted[i]);
  }

  free(values);
  free(sorted);
  dv_heap_destroy(&h);
  return true;
}

static void node_on_move(void *slot, size_t index, void *user_data) {
  size_t *moves = (size_t *)user_data;
  node_t *node = *(node_t **)slot;

  node->heap_index = index;
  if (moves)
    ++*moves;
}

static dv_heap_config_t node_heap_config(size_t *moves) {
  const dv_heap_config_t config = {
      .elem_size = sizeof(void *),
      .elem_align = alignof(void *),
      .initial_capacity = 0u,
      .max_capacity = 0u,
      .growable = true,
      .cache_aligned = false,
      .order = DV_HEAP_MIN,
      .key_type = DV_HEAP_KEY_U64,
      .key_offset = offsetof(node_t, key),
      .key_indirect = true,
      .compare = NULL,
      .compare_user_data = NULL,
      .on_move = node_on_move,
      .move_user_data = moves,
  };
  return config;
}

// The on_move hook is the index map: after every mutation each node knows
// where it sits, which is what makes decrease_key and cancellation possible.
static bool indices_are_consistent(const dv_heap_t *h) {
  for (size_t i = 0u; i < dv_heap_size(h); ++i) {
    node_t *const *slot = (node_t *const *)dv_heap_at_const(h, i);
    ASSERT_TRUE(slot != NULL);
    ASSERT_EQ_U64((*slot)->heap_index, i);
  }
  return true;
}

static bool test_update_at_both_directions(void) {
  size_t moves = 0u;
  dv_heap_t h;
  const dv_heap_config_t config = node_heap_config(&moves);
  ASSERT_OK(dv_heap_init(&h, &config, NULL));

  const size_t count = 200u;
  node_t *nodes = (node_t *)calloc(count, sizeof(*nodes));
  ASSERT_TRUE(nodes != NULL);

  uint64_t rng = 0x5555AAAA33339999ull;
  for (size_t i = 0u; i < count; ++i) {
    nodes[i].key = 1000u + (xorshift64(&rng) % 9000u);
    nodes[i].tag = i;
    node_t *ptr = &nodes[i];
    ASSERT_OK(dv_heap_push(&h, &ptr));
  }

  ASSERT_VALID(&h);
  if (!indices_are_consistent(&h))
    return false;

  // decrease_key: drop a key below every other and it must reach the root.
  node_t *target = &nodes[count / 2u];
  target->key = 0u;
  ASSERT_OK(dv_heap_update_at(&h, target->heap_index, &target));
  ASSERT_VALID(&h);
  ASSERT_EQ_U64(target->heap_index, 0u);
  ASSERT_PTR_EQ(*(node_t **)dv_heap_top(&h), target);
  if (!indices_are_consistent(&h))
    return false;

  // increase_key: raise the root above everything and it must leave the root.
  target->key = UINT64_MAX;
  ASSERT_OK(dv_heap_update_at(&h, target->heap_index, &target));
  ASSERT_VALID(&h);
  ASSERT_TRUE(target->heap_index != 0u);
  if (!indices_are_consistent(&h))
    return false;

  // A key that does not change must leave the element exactly where it was.
  node_t *stable = *(node_t **)dv_heap_at(&h, count / 3u);
  const size_t before = stable->heap_index;
  ASSERT_OK(dv_heap_update_at(&h, before, &stable));
  ASSERT_EQ_U64(stable->heap_index, before);
  ASSERT_VALID(&h);

  ASSERT_STATUS(dv_heap_update_at(&h, dv_heap_size(&h), &stable),
                DV_HEAP_ERR_INVALID);
  ASSERT_STATUS(dv_heap_update_at(&h, 0u, NULL), DV_HEAP_ERR_INVALID);

  free(nodes);
  dv_heap_destroy(&h);
  return true;
}

static bool test_remove_at_positions(void) {
  size_t moves = 0u;
  dv_heap_t h;
  const dv_heap_config_t config = node_heap_config(&moves);

  // Removing the root, an interior node, and the physically last node all take
  // different paths through the backfill, so each is exercised in turn.
  const size_t positions[] = {0u, 1u, 5u, 30u, 63u};

  for (size_t p = 0u; p < sizeof(positions) / sizeof(positions[0]); ++p) {
    ASSERT_OK(dv_heap_init(&h, &config, NULL));

    const size_t count = 64u;
    node_t *nodes = (node_t *)calloc(count, sizeof(*nodes));
    ASSERT_TRUE(nodes != NULL);

    uint64_t rng = 0x7777111122223333ull + p;
    for (size_t i = 0u; i < count; ++i) {
      nodes[i].key = xorshift64(&rng) >> 32;
      nodes[i].tag = i;
      node_t *ptr = &nodes[i];
      ASSERT_OK(dv_heap_push(&h, &ptr));
    }

    ASSERT_VALID(&h);

    node_t *removed = NULL;
    ASSERT_OK(dv_heap_remove_at(&h, positions[p], &removed));
    ASSERT_TRUE(removed != NULL);
    ASSERT_EQ_U64(dv_heap_size(&h), count - 1u);
    ASSERT_VALID(&h);
    if (!indices_are_consistent(&h))
      return false;

    // The removed node must be gone and every other still present.
    for (size_t i = 0u; i < dv_heap_size(&h); ++i)
      ASSERT_TRUE(*(node_t **)dv_heap_at(&h, i) != removed);

    ASSERT_STATUS(dv_heap_remove_at(&h, dv_heap_size(&h), NULL),
                  DV_HEAP_ERR_INVALID);

    uint64_t previous = 0u;
    while (dv_heap_size(&h) > 0u) {
      node_t *node = NULL;
      ASSERT_OK(dv_heap_pop(&h, &node));
      ASSERT_TRUE(node->key >= previous);
      previous = node->key;
    }

    free(nodes);
    dv_heap_destroy(&h);
  }

  return true;
}

// --- Bulk --------------------------------------------------------------

static bool test_push_bulk_and_pop_bulk(void) {
  dv_heap_t h;
  const dv_heap_config_t config = u64_config(DV_HEAP_MIN, 0u, true);
  ASSERT_OK(dv_heap_init(&h, &config, NULL));

  uint64_t items[64];
  uint64_t rng = 0x2468ACE013579BDFull;
  for (size_t i = 0u; i < 64u; ++i)
    items[i] = xorshift64(&rng) >> 32;

  // A first bulk push into an empty heap takes the bottom-up build path.
  ASSERT_EQ_U64(dv_heap_push_bulk(&h, items, 64u), 64u);
  ASSERT_EQ_U64(dv_heap_size(&h), 64u);
  ASSERT_VALID(&h);

  // A short append takes the sift-each path instead.
  ASSERT_EQ_U64(dv_heap_push_bulk(&h, items, 4u), 4u);
  ASSERT_EQ_U64(dv_heap_size(&h), 68u);
  ASSERT_VALID(&h);

  uint64_t drained[68];
  ASSERT_EQ_U64(dv_heap_pop_bulk(&h, drained, 68u), 68u);
  ASSERT_TRUE(dv_heap_is_empty(&h));

  for (size_t i = 1u; i < 68u; ++i)
    ASSERT_TRUE(drained[i] >= drained[i - 1u]);

  // Short reads stop at the bottom, and a NULL destination discards.
  ASSERT_EQ_U64(dv_heap_pop_bulk(&h, drained, 8u), 0u);
  ASSERT_EQ_U64(dv_heap_push_bulk(&h, items, 10u), 10u);
  ASSERT_EQ_U64(dv_heap_pop_bulk(&h, NULL, 4u), 4u);
  ASSERT_EQ_U64(dv_heap_size(&h), 6u);
  ASSERT_VALID(&h);

  ASSERT_EQ_U64(dv_heap_push_bulk(&h, items, 0u), 0u);
  ASSERT_EQ_U64(dv_heap_push_bulk(&h, NULL, 4u), 0u);
  ASSERT_EQ_U64(dv_heap_pop_bulk(&h, drained, 0u), 0u);

  dv_heap_destroy(&h);
  return true;
}

static bool test_push_bulk_partial_on_full(void) {
  dv_heap_t h;
  const dv_heap_config_t config = u64_config(DV_HEAP_MIN, TEST_CAPACITY, false);
  ASSERT_OK(dv_heap_init(&h, &config, NULL));

  uint64_t items[12];
  for (uint64_t i = 0u; i < 12u; ++i)
    items[i] = 12u - i;

  ASSERT_EQ_U64(dv_heap_push_bulk(&h, items, 12u), TEST_CAPACITY);
  ASSERT_EQ_U64(dv_heap_size(&h), TEST_CAPACITY);
  ASSERT_VALID(&h);
  ASSERT_EQ_U64(dv_heap_push_bulk(&h, items, 1u), 0u);

  dv_heap_destroy(&h);
  return true;
}

// --- Memory contracts --------------------------------------------------

static bool run_allocator_accounting(bool with_realloc) {
  tracking_state_t st;
  tracking_init(&st);
  const dv_heap_allocator_t hooks = tracking_hooks(&st, with_realloc);

  dv_heap_t h;
  const dv_heap_config_t config = u64_config(DV_HEAP_MIN, 2u, true);
  ASSERT_OK(dv_heap_init(&h, &config, &hooks));

  // The buffer carries one slot past the capacity for the sift scratch.
  ASSERT_EQ_U64(st.live_bytes, 3u * sizeof(uint64_t));

  uint64_t rng = 0xFEEDFACEDEADBEEFull;
  for (size_t i = 0u; i < 500u; ++i) {
    const uint64_t value = xorshift64(&rng) >> 24;
    ASSERT_OK(dv_heap_push(&h, &value));
  }

  ASSERT_VALID(&h);
  ASSERT_EQ_U64(st.live_bytes, (dv_heap_capacity(&h) + 1u) * sizeof(uint64_t));
  ASSERT_TRUE(st.align_respected);

  uint64_t previous = 0u;
  for (size_t i = 0u; i < 500u; ++i) {
    uint64_t value = 0u;
    ASSERT_OK(dv_heap_pop(&h, &value));
    ASSERT_TRUE(value >= previous);
    previous = value;
  }

  ASSERT_OK(dv_heap_reset(&h));
  ASSERT_EQ_U64(st.live_bytes, 3u * sizeof(uint64_t));

  dv_heap_destroy(&h);
  ASSERT_EQ_U64(st.live_bytes, 0u);
  ASSERT_TRUE(st.free_calls > 0u);

  if (with_realloc)
    ASSERT_TRUE(st.realloc_calls > 0u);
  else
    ASSERT_EQ_U64(st.realloc_calls, 0u);

  return true;
}

static bool test_allocator_accounting_with_realloc(void) {
  return run_allocator_accounting(true);
}

static bool test_allocator_accounting_without_realloc(void) {
  return run_allocator_accounting(false);
}

static bool test_allocation_failure_is_recoverable(void) {
  tracking_state_t st;
  tracking_init(&st);
  const dv_heap_allocator_t hooks = tracking_hooks(&st, true);

  dv_heap_t h;
  const dv_heap_config_t config = u64_config(DV_HEAP_MIN, 2u, true);

  st.budget = 0u;
  ASSERT_STATUS(dv_heap_init(&h, &config, &hooks), DV_HEAP_ERR_NOMEM);
  ASSERT_EQ_U64(dv_heap_capacity(&h), 0u);
  dv_heap_destroy(&h);

  st.budget = 1u;
  ASSERT_OK(dv_heap_init(&h, &config, &hooks));

  const uint64_t a = 5u;
  const uint64_t b = 3u;
  const uint64_t c = 1u;
  ASSERT_OK(dv_heap_push(&h, &a));
  ASSERT_OK(dv_heap_push(&h, &b));
  ASSERT_STATUS(dv_heap_push(&h, &c), DV_HEAP_ERR_NOMEM);

  // A refused growth leaves the heap exactly as it was, invariant included.
  ASSERT_EQ_U64(dv_heap_size(&h), 2u);
  ASSERT_EQ_U64(dv_heap_capacity(&h), 2u);
  ASSERT_VALID(&h);
  ASSERT_EQ_U64(*(const uint64_t *)dv_heap_top(&h), 3u);
  ASSERT_STATUS(dv_heap_reserve(&h, 64u), DV_HEAP_ERR_NOMEM);
  ASSERT_EQ_U64(dv_heap_capacity(&h), 2u);
  ASSERT_STATUS(dv_heap_build(&h, &a, 8u), DV_HEAP_ERR_NOMEM);

  st.budget = SIZE_MAX;
  ASSERT_OK(dv_heap_push(&h, &c));
  ASSERT_EQ_U64(dv_heap_size(&h), 3u);
  ASSERT_EQ_U64(*(const uint64_t *)dv_heap_top(&h), 1u);
  ASSERT_VALID(&h);

  dv_heap_destroy(&h);
  ASSERT_EQ_U64(st.live_bytes, 0u);
  return true;
}

static bool test_byte_size_overflow_is_refused(void) {
  dv_heap_t h;
  dv_heap_config_t config = u64_config(DV_HEAP_MIN, 0u, true);
  config.elem_size = SIZE_MAX / 2u;
  config.elem_align = 1u;
  config.key_offset = 0u;

  ASSERT_OK(dv_heap_init(&h, &config, NULL));
  ASSERT_STATUS(dv_heap_reserve(&h, 4u), DV_HEAP_ERR_OVERFLOW);
  ASSERT_EQ_U64(dv_heap_capacity(&h), 0u);
  dv_heap_destroy(&h);

  // The capacity ceiling that keeps child indices inside size_t is enforced
  // even when the byte count itself would fit.
  dv_heap_config_t narrow = u64_config(DV_HEAP_MIN, 0u, true);
  narrow.elem_size = 1u;
  narrow.elem_align = 1u;
  narrow.key_type = DV_HEAP_KEY_CUSTOM;
  narrow.compare = dummy_compare;
  ASSERT_OK(dv_heap_init(&h, &narrow, NULL));
  ASSERT_STATUS(dv_heap_reserve(&h, SIZE_MAX - 1u), DV_HEAP_ERR_OVERFLOW);
  dv_heap_destroy(&h);

  config.initial_capacity = 8u;
  ASSERT_STATUS(dv_heap_init(&h, &config, NULL), DV_HEAP_ERR_OVERFLOW);
  ASSERT_EQ_U64(dv_heap_capacity(&h), 0u);
  return true;
}

static bool test_cache_aligned_buffer(void) {
  tracking_state_t st;
  tracking_init(&st);
  const dv_heap_allocator_t hooks = tracking_hooks(&st, true);

  dv_heap_t h;
  dv_heap_config_t config = u64_config(DV_HEAP_MIN, 4u, true);
  config.cache_aligned = true;
  ASSERT_OK(dv_heap_init(&h, &config, &hooks));
  ASSERT_EQ_U64(st.last_align, DV_CACHE_LINE_SIZE);
  ASSERT_EQ_U64((uintptr_t)h.buffer % DV_CACHE_LINE_SIZE, 0u);

  uint64_t rng = 0x13579BDF2468ACE0ull;
  for (size_t i = 0u; i < 200u; ++i) {
    const uint64_t value = xorshift64(&rng) >> 32;
    ASSERT_OK(dv_heap_push(&h, &value));
    ASSERT_EQ_U64((uintptr_t)h.buffer % DV_CACHE_LINE_SIZE, 0u);
  }

  ASSERT_VALID(&h);
  ASSERT_TRUE(st.align_respected);

  dv_heap_destroy(&h);
  ASSERT_EQ_U64(st.live_bytes, 0u);
  return true;
}

// --- Randomized differential -------------------------------------------

typedef struct {
  node_t **live;
  size_t live_count;
} model_t;

static void model_add(model_t *m, node_t *node) {
  node->live_index = m->live_count;
  m->live[m->live_count++] = node;
}

static void model_remove(model_t *m, node_t *node) {
  const size_t slot = node->live_index;
  --m->live_count;
  m->live[slot] = m->live[m->live_count];
  m->live[slot]->live_index = slot;
}

static node_t *model_min(const model_t *m) {
  node_t *best = m->live[0];
  for (size_t i = 1u; i < m->live_count; ++i)
    if (m->live[i]->key < best->key)
      best = m->live[i];
  return best;
}

// Mirrors every mutation onto a plain array and checks after each step that
// the heap holds the same multiset, still satisfies the heap-order property,
// and still reports every element's index correctly.
static bool test_randomized_differential(void) {
  size_t moves = 0u;
  dv_heap_t h;
  const dv_heap_config_t config = node_heap_config(&moves);
  ASSERT_OK(dv_heap_init(&h, &config, NULL));

  const size_t pool_size = 512u;
  node_t *pool = (node_t *)calloc(pool_size, sizeof(*pool));
  node_t **live = (node_t **)malloc(pool_size * sizeof(*live));
  node_t **free_list = (node_t **)malloc(pool_size * sizeof(*free_list));
  ASSERT_TRUE(pool != NULL && live != NULL && free_list != NULL);

  for (size_t i = 0u; i < pool_size; ++i)
    free_list[i] = &pool[i];
  size_t free_count = pool_size;

  model_t model = {.live = live, .live_count = 0u};
  uint64_t rng = 0x0123456789ABCDEFull;
  bool ok = true;

  for (size_t op = 0u; op < DIFF_OPS && ok; ++op) {
    const uint64_t roll = xorshift64(&rng);
    const unsigned choice = (unsigned)(roll % 100u);

    if (choice < 45u && free_count > 0u) {
      node_t *node = free_list[--free_count];
      node->key = xorshift64(&rng) >> 30;
      node->tag = op;
      ok = dv_heap_push(&h, &node) == DV_HEAP_OK;
      model_add(&model, node);
    } else if (choice < 75u && model.live_count > 0u) {
      const node_t *expected = model_min(&model);
      const uint64_t expected_key = expected->key;

      node_t *popped = NULL;
      ok = dv_heap_pop(&h, &popped) == DV_HEAP_OK;
      ok = ok && popped != NULL && popped->key == expected_key;
      if (ok) {
        model_remove(&model, popped);
        free_list[free_count++] = popped;
      }
    } else if (choice < 90u && model.live_count > 0u) {
      node_t *node = model.live[xorshift64(&rng) % model.live_count];
      node->key = xorshift64(&rng) >> 30;
      ok = dv_heap_update_at(&h, node->heap_index, &node) == DV_HEAP_OK;
    } else if (model.live_count > 0u) {
      node_t *node = model.live[xorshift64(&rng) % model.live_count];
      ok = dv_heap_remove_at(&h, node->heap_index, NULL) == DV_HEAP_OK;
      if (ok) {
        model_remove(&model, node);
        free_list[free_count++] = node;
      }
    }

    ok = ok && dv_heap_size(&h) == model.live_count;
    ok = ok && dv_heap_is_valid(&h);

    // Spot-check the index map rather than sweeping it on every operation.
    if (ok && model.live_count > 0u) {
      const size_t probe = (size_t)(xorshift64(&rng) % model.live_count);
      node_t *const *slot = (node_t *const *)dv_heap_at(&h, probe);
      ok = slot != NULL && (*slot)->heap_index == probe;
    }
  }

  // Whatever survives must still drain in order.
  uint64_t previous = 0u;
  while (ok && dv_heap_size(&h) > 0u) {
    node_t *node = NULL;
    ok = dv_heap_pop(&h, &node) == DV_HEAP_OK && node->key >= previous;
    if (ok)
      previous = node->key;
  }

  free(pool);
  free(live);
  free(free_list);
  dv_heap_destroy(&h);

  ASSERT_TRUE(ok);
  ASSERT_TRUE(moves > 0u);
  return true;
}

// --- Thread-safe wrapper -----------------------------------------------

typedef struct {
  dv_heap_mt_t *heap;
  size_t thread_id;
  uint64_t sum;
  size_t count;
} mt_arg_t;

static void *mt_producer(void *arg) {
  mt_arg_t *a = (mt_arg_t *)arg;
  uint64_t rng = 0x9E3779B97F4A7C15ull + (uint64_t)a->thread_id;

  for (size_t i = 0u; i < MT_ITEMS_PER_THREAD; ++i) {
    const uint64_t value = xorshift64(&rng) >> 16;
    if (dv_heap_mt_push(a->heap, &value) != DV_HEAP_OK)
      return NULL;

    a->sum += value;
    ++a->count;
  }

  return NULL;
}

static void *mt_consumer(void *arg) {
  mt_arg_t *a = (mt_arg_t *)arg;

  while (a->count < MT_ITEMS_PER_THREAD) {
    uint64_t out = 0u;
    if (dv_heap_mt_pop(a->heap, &out) != DV_HEAP_OK)
      continue;

    a->sum += out;
    ++a->count;
  }

  return NULL;
}

// The wrapper makes no ordering promise across threads, so the invariant under
// test is conservation: everything pushed is popped exactly once.
static bool test_mt_wrapper_conserves_elements(void) {
  dv_heap_mt_t heap;
  const dv_heap_config_t config = u64_config(DV_HEAP_MIN, 64u, true);
  ASSERT_OK(dv_heap_mt_init(&heap, &config, NULL));

  pthread_t producers[MT_THREADS];
  pthread_t consumers[MT_THREADS];
  mt_arg_t pargs[MT_THREADS];
  mt_arg_t cargs[MT_THREADS];

  for (size_t i = 0u; i < MT_THREADS; ++i) {
    pargs[i] =
        (mt_arg_t){.heap = &heap, .thread_id = i, .sum = 0u, .count = 0u};
    cargs[i] =
        (mt_arg_t){.heap = &heap, .thread_id = i, .sum = 0u, .count = 0u};
  }

  for (size_t i = 0u; i < MT_THREADS; ++i)
    ASSERT_EQ_U64(pthread_create(&consumers[i], NULL, mt_consumer, &cargs[i]),
                  0);
  for (size_t i = 0u; i < MT_THREADS; ++i)
    ASSERT_EQ_U64(pthread_create(&producers[i], NULL, mt_producer, &pargs[i]),
                  0);

  for (size_t i = 0u; i < MT_THREADS; ++i)
    ASSERT_EQ_U64(pthread_join(producers[i], NULL), 0);
  for (size_t i = 0u; i < MT_THREADS; ++i)
    ASSERT_EQ_U64(pthread_join(consumers[i], NULL), 0);

  uint64_t produced_sum = 0u;
  uint64_t consumed_sum = 0u;
  size_t produced = 0u;
  size_t consumed = 0u;

  for (size_t i = 0u; i < MT_THREADS; ++i) {
    produced_sum += pargs[i].sum;
    produced += pargs[i].count;
    consumed_sum += cargs[i].sum;
    consumed += cargs[i].count;
  }

  const size_t expected = MT_THREADS * MT_ITEMS_PER_THREAD;
  ASSERT_EQ_U64(produced, expected);
  ASSERT_EQ_U64(consumed, expected);
  ASSERT_EQ_U64(produced_sum, consumed_sum);
  ASSERT_EQ_U64(dv_heap_mt_size(&heap), 0u);
  ASSERT_TRUE(dv_heap_mt_is_empty(&heap));

  dv_heap_mt_destroy(&heap);
  return true;
}

static void mt_drain_to_sum(dv_heap_t *h, void *user_data) {
  uint64_t *total = (uint64_t *)user_data;
  uint64_t value = 0u;

  while (dv_heap_pop(h, &value) == DV_HEAP_OK)
    *total += value;
}

static bool test_mt_critical_section(void) {
  dv_heap_mt_t heap;
  const dv_heap_config_t config = u64_config(DV_HEAP_MIN, 4u, true);
  ASSERT_OK(dv_heap_mt_init(&heap, &config, NULL));
  ASSERT_OK(dv_heap_mt_reserve(&heap, 32u));

  const uint64_t items[5] = {5u, 1u, 4u, 2u, 3u};
  ASSERT_EQ_U64(dv_heap_mt_push_bulk(&heap, items, 5u), 5u);

  uint64_t peeked = 0u;
  ASSERT_OK(dv_heap_mt_peek(&heap, &peeked));
  ASSERT_EQ_U64(peeked, 1u);

  const uint64_t replacement = 9u;
  uint64_t displaced = 0u;
  ASSERT_OK(dv_heap_mt_replace_top(&heap, &replacement, &displaced));
  ASSERT_EQ_U64(displaced, 1u);

  uint64_t drained[2] = {0u, 0u};
  ASSERT_EQ_U64(dv_heap_mt_pop_bulk(&heap, drained, 2u), 2u);
  ASSERT_EQ_U64(drained[0], 2u);
  ASSERT_EQ_U64(drained[1], 3u);

  uint64_t total = 0u;
  ASSERT_OK(dv_heap_mt_with(&heap, mt_drain_to_sum, &total));
  ASSERT_EQ_U64(total, 18u); // 4 + 5 + 9
  ASSERT_EQ_U64(dv_heap_mt_size(&heap), 0u);
  ASSERT_STATUS(dv_heap_mt_with(&heap, NULL, NULL), DV_HEAP_ERR_INVALID);

  dv_heap_mt_clear(&heap);
  dv_heap_mt_destroy(&heap);
  return true;
}

int main(void) {
  const test_case_t tests[] = {
      {"init rejects bad config", test_init_rejects_bad_config},
      {"empty heap accessors", test_empty_heap_accessors},
      {"status strings", test_status_strings},
      {"min heap order", test_min_heap_order},
      {"max heap order", test_max_heap_order},
      {"duplicate keys", test_duplicate_keys},
      {"builtin key types", test_builtin_key_types},
      {"key offset in struct", test_key_offset_in_struct},
      {"pointer payload indirect key", test_pointer_payload_indirect_key},
      {"custom comparator", test_custom_comparator},
      {"custom comparator max order", test_custom_comparator_max_order},
      {"fixed capacity boundary", test_fixed_capacity_boundary},
      {"growth and ceiling", test_growth_and_ceiling},
      {"reserve shrink reset clear", test_reserve_shrink_reset_clear},
      {"build and heapify", test_build_and_heapify},
      {"replace top for top k", test_replace_top_top_k},
      {"update at both directions", test_update_at_both_directions},
      {"remove at positions", test_remove_at_positions},
      {"push bulk and pop bulk", test_push_bulk_and_pop_bulk},
      {"push bulk partial on full", test_push_bulk_partial_on_full},
      {"allocator accounting with realloc",
       test_allocator_accounting_with_realloc},
      {"allocator accounting without realloc",
       test_allocator_accounting_without_realloc},
      {"allocation failure is recoverable",
       test_allocation_failure_is_recoverable},
      {"byte size overflow is refused", test_byte_size_overflow_is_refused},
      {"cache aligned buffer", test_cache_aligned_buffer},
      {"randomized differential", test_randomized_differential},
      {"mt wrapper conserves elements", test_mt_wrapper_conserves_elements},
      {"mt critical section", test_mt_critical_section},
  };

  const size_t ntests = sizeof(tests) / sizeof(tests[0]);

  for (size_t i = 0u; i < ntests; ++i) {
    printf("[TEST] %s\n", tests[i].name);
    if (!tests[i].fn()) {
      fprintf(stderr, "[FAIL] %s\n", tests[i].name);
      return EXIT_FAILURE;
    }
    printf("[PASS] %s\n", tests[i].name);
  }

  printf("All %zu tests passed.\n", ntests);
  return EXIT_SUCCESS;
}
