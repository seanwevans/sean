// unit_tests.c

#include "dv_stack.h"
#include "dv_stack_mt.h"

#include <inttypes.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_CAPACITY 8u
#define STRESS_OPS 200000u
#define MT_THREADS 4u
#define MT_ITEMS_PER_THREAD 20000u

// The pointer-mode guards trip a debug assert before they return a status, so
// the status path is only observable when asserts are compiled out.
#if defined(NDEBUG) || defined(DV_STACK_NO_ASSERT)
#define DV_STACK_ASSERTS_DISABLED 1
#else
#define DV_STACK_ASSERTS_DISABLED 0
#endif

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
    const dv_stack_status_t _s = (expr);                                       \
    const dv_stack_status_t _e = (expected);                                   \
    if (_s != _e) {                                                            \
      fprintf(stderr,                                                          \
              "ASSERT_STATUS failed: %s -> %s (expected %s) (%s:%d)\n", #expr, \
              dv_stack_status_str(_s), dv_stack_status_str(_e), __FILE__,      \
              __LINE__);                                                       \
      return false;                                                            \
    }                                                                          \
  } while (0)

#define ASSERT_OK(expr) ASSERT_STATUS((expr), DV_STACK_OK)

typedef bool (*test_fn)(void);

typedef struct {
  const char *name;
  test_fn fn;
} test_case_t;

// --- Test allocator ----------------------------------------------------
//
// Accounts for every byte handed out so leaks and mismatched sizes surface as
// test failures rather than as sanitizer noise, and can refuse allocations on
// demand to exercise the out-of-memory paths.

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

// `with_realloc` selects between the resize hook and the alloc+copy+free
// fallback the stack must take when an allocator cannot resize.
static dv_stack_allocator_t tracking_hooks(tracking_state_t *st,
                                           bool with_realloc) {
  const dv_stack_allocator_t hooks = {
      .alloc = tracking_alloc,
      .realloc = with_realloc ? tracking_realloc : NULL,
      .free = tracking_free,
      .user_data = st,
  };
  return hooks;
}

// --- Helpers -----------------------------------------------------------

static dv_stack_config_t u64_config(size_t initial_capacity, bool growable) {
  const dv_stack_config_t config = {
      .elem_size = sizeof(uint64_t),
      .elem_align = alignof(uint64_t),
      .initial_capacity = initial_capacity,
      .max_capacity = 0u,
      .growable = growable,
      .cache_aligned = false,
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

// --- Lifecycle and configuration ---------------------------------------

static bool test_init_rejects_bad_config(void) {
  dv_stack_t s;
  const dv_stack_config_t valid = u64_config(TEST_CAPACITY, false);

  ASSERT_STATUS(dv_stack_init(NULL, &valid, NULL), DV_STACK_ERR_INVALID);
  ASSERT_STATUS(dv_stack_init(&s, NULL, NULL), DV_STACK_ERR_INVALID);

  dv_stack_config_t bad = valid;
  bad.elem_size = 0u;
  ASSERT_STATUS(dv_stack_init(&s, &bad, NULL), DV_STACK_ERR_INVALID);

  bad = valid;
  bad.elem_align = 3u; // not a power of two
  ASSERT_STATUS(dv_stack_init(&s, &bad, NULL), DV_STACK_ERR_INVALID);

  bad = valid;
  bad.elem_size = 6u; // stride is not a multiple of the alignment
  bad.elem_align = 4u;
  ASSERT_STATUS(dv_stack_init(&s, &bad, NULL), DV_STACK_ERR_INVALID);

  bad = valid;
  bad.growable = true;
  bad.initial_capacity = 16u;
  bad.max_capacity = 8u;
  ASSERT_STATUS(dv_stack_init(&s, &bad, NULL), DV_STACK_ERR_INVALID);

  tracking_state_t st;
  tracking_init(&st);
  dv_stack_allocator_t hooks = tracking_hooks(&st, true);

  hooks.free = NULL;
  ASSERT_STATUS(dv_stack_init(&s, &valid, &hooks), DV_STACK_ERR_INVALID);

  hooks = tracking_hooks(&st, true);
  hooks.alloc = NULL;
  ASSERT_STATUS(dv_stack_init(&s, &valid, &hooks), DV_STACK_ERR_INVALID);

  // A rejected init leaves a zeroed control block, which destroy accepts.
  dv_stack_destroy(&s);

  ASSERT_OK(dv_stack_init(&s, &valid, NULL));
  dv_stack_destroy(&s);
  return true;
}

static bool test_empty_stack_accessors(void) {
  dv_stack_t s;
  const dv_stack_config_t config = u64_config(TEST_CAPACITY, false);

  ASSERT_EQ_U64(dv_stack_size(NULL), 0u);
  ASSERT_EQ_U64(dv_stack_capacity(NULL), 0u);
  ASSERT_EQ_U64(dv_stack_elem_size(NULL), 0u);
  ASSERT_TRUE(dv_stack_is_empty(NULL));
  ASSERT_TRUE(dv_stack_is_full(NULL));
  ASSERT_PTR_EQ(dv_stack_top(NULL), NULL);
  ASSERT_PTR_EQ(dv_stack_at(NULL, 0u), NULL);
  ASSERT_TRUE(dv_stack_at_const(NULL, 0u) == NULL);
  ASSERT_TRUE(dv_stack_top_const(NULL) == NULL);
  ASSERT_FALSE(dv_stack_is_ptr_mode(NULL));
  dv_stack_clear(NULL);

  uint64_t out = 0u;
  ASSERT_STATUS(dv_stack_push(NULL, &out), DV_STACK_ERR_INVALID);
  ASSERT_STATUS(dv_stack_pop(NULL, &out), DV_STACK_ERR_INVALID);
  ASSERT_STATUS(dv_stack_peek(NULL, &out), DV_STACK_ERR_INVALID);
  ASSERT_STATUS(dv_stack_reserve(NULL, 4u), DV_STACK_ERR_INVALID);
  ASSERT_STATUS(dv_stack_shrink_to_fit(NULL), DV_STACK_ERR_INVALID);
  ASSERT_STATUS(dv_stack_reset(NULL), DV_STACK_ERR_INVALID);
  ASSERT_STATUS(dv_stack_grow_for_push(NULL), DV_STACK_ERR_INVALID);
  ASSERT_EQ_U64(dv_stack_push_bulk(NULL, &out, 1u), 0u);
  ASSERT_EQ_U64(dv_stack_pop_bulk(NULL, &out, 1u), 0u);
  dv_stack_destroy(NULL);

  ASSERT_OK(dv_stack_init(&s, &config, NULL));
  ASSERT_EQ_U64(dv_stack_size(&s), 0u);
  ASSERT_EQ_U64(dv_stack_capacity(&s), TEST_CAPACITY);
  ASSERT_EQ_U64(dv_stack_elem_size(&s), sizeof(uint64_t));
  ASSERT_TRUE(dv_stack_is_empty(&s));
  ASSERT_FALSE(dv_stack_is_full(&s));
  ASSERT_PTR_EQ(dv_stack_top(&s), NULL);

  ASSERT_STATUS(dv_stack_pop(&s, &out), DV_STACK_ERR_EMPTY);
  ASSERT_STATUS(dv_stack_peek(&s, &out), DV_STACK_ERR_EMPTY);
  ASSERT_STATUS(dv_stack_push(&s, NULL), DV_STACK_ERR_INVALID);
  ASSERT_STATUS(dv_stack_peek(&s, NULL), DV_STACK_ERR_INVALID);

  dv_stack_destroy(&s);
  ASSERT_EQ_U64(dv_stack_capacity(&s), 0u);
  dv_stack_destroy(&s); // idempotent
  return true;
}

static bool test_status_strings(void) {
  const dv_stack_status_t codes[] = {
      DV_STACK_OK,       DV_STACK_ERR_INVALID, DV_STACK_ERR_EMPTY,
      DV_STACK_ERR_FULL, DV_STACK_ERR_NOMEM,   DV_STACK_ERR_OVERFLOW,
  };

  for (size_t i = 0u; i < sizeof(codes) / sizeof(codes[0]); ++i) {
    const char *name = dv_stack_status_str(codes[i]);
    ASSERT_TRUE(name != NULL);
    ASSERT_TRUE(strncmp(name, "DV_STACK_", 9u) == 0);
  }

  ASSERT_TRUE(strcmp(dv_stack_status_str((dv_stack_status_t)999),
                     "DV_STACK_ERR_UNKNOWN") == 0);
  return true;
}

// --- Core operations ---------------------------------------------------

static bool test_push_pop_lifo_order(void) {
  dv_stack_t s;
  const dv_stack_config_t config = u64_config(TEST_CAPACITY, false);
  ASSERT_OK(dv_stack_init(&s, &config, NULL));

  for (uint64_t i = 1u; i <= 4u; ++i)
    ASSERT_OK(dv_stack_push(&s, &i));

  ASSERT_EQ_U64(dv_stack_size(&s), 4u);

  for (uint64_t expected = 4u; expected >= 1u; --expected) {
    uint64_t out = 0u;
    ASSERT_OK(dv_stack_pop(&s, &out));
    ASSERT_EQ_U64(out, expected);
  }

  ASSERT_TRUE(dv_stack_is_empty(&s));

  // A NULL destination discards the element instead of copying it out.
  const uint64_t value = 7u;
  ASSERT_OK(dv_stack_push(&s, &value));
  ASSERT_OK(dv_stack_pop(&s, NULL));
  ASSERT_EQ_U64(dv_stack_size(&s), 0u);

  dv_stack_destroy(&s);
  return true;
}

static bool test_peek_top_and_depth_access(void) {
  dv_stack_t s;
  const dv_stack_config_t config = u64_config(TEST_CAPACITY, false);
  ASSERT_OK(dv_stack_init(&s, &config, NULL));

  for (uint64_t i = 10u; i <= 12u; ++i)
    ASSERT_OK(dv_stack_push(&s, &i));

  uint64_t out = 0u;
  ASSERT_OK(dv_stack_peek(&s, &out));
  ASSERT_EQ_U64(out, 12u);
  ASSERT_EQ_U64(dv_stack_size(&s), 3u); // peek does not consume

  const uint64_t *top = (const uint64_t *)dv_stack_top(&s);
  ASSERT_TRUE(top != NULL);
  ASSERT_EQ_U64(*top, 12u);
  ASSERT_EQ_U64(*(const uint64_t *)dv_stack_top_const(&s), 12u);

  ASSERT_EQ_U64(*(const uint64_t *)dv_stack_at(&s, 0u), 12u);
  ASSERT_EQ_U64(*(const uint64_t *)dv_stack_at(&s, 1u), 11u);
  ASSERT_EQ_U64(*(const uint64_t *)dv_stack_at_const(&s, 2u), 10u);
  ASSERT_PTR_EQ(dv_stack_at(&s, 3u), NULL);

  dv_stack_destroy(&s);
  return true;
}

static bool test_fixed_capacity_boundary(void) {
  dv_stack_t s;
  const dv_stack_config_t config = u64_config(TEST_CAPACITY, false);
  ASSERT_OK(dv_stack_init(&s, &config, NULL));

  for (uint64_t i = 0u; i < TEST_CAPACITY; ++i)
    ASSERT_OK(dv_stack_push(&s, &i));

  ASSERT_TRUE(dv_stack_is_full(&s));
  ASSERT_EQ_U64(dv_stack_size(&s), TEST_CAPACITY);

  const uint64_t overflow_value = 99u;
  ASSERT_STATUS(dv_stack_push(&s, &overflow_value), DV_STACK_ERR_FULL);
  ASSERT_EQ_U64(dv_stack_size(&s), TEST_CAPACITY);
  ASSERT_EQ_U64(dv_stack_capacity(&s), TEST_CAPACITY);

  // A fixed stack cannot be grown, and cannot give capacity back either.
  ASSERT_STATUS(dv_stack_reserve(&s, TEST_CAPACITY + 1u), DV_STACK_ERR_FULL);
  ASSERT_OK(dv_stack_reserve(&s, TEST_CAPACITY));
  ASSERT_OK(dv_stack_shrink_to_fit(&s));
  ASSERT_EQ_U64(dv_stack_capacity(&s), TEST_CAPACITY);

  uint64_t out = 0u;
  ASSERT_OK(dv_stack_pop(&s, &out));
  ASSERT_EQ_U64(out, TEST_CAPACITY - 1u);
  ASSERT_FALSE(dv_stack_is_full(&s));
  ASSERT_OK(dv_stack_push(&s, &overflow_value));
  ASSERT_EQ_U64(*(const uint64_t *)dv_stack_top(&s), 99u);

  dv_stack_destroy(&s);
  return true;
}

static bool test_growable_growth(void) {
  dv_stack_t s;
  const dv_stack_config_t config = u64_config(0u, true);
  ASSERT_OK(dv_stack_init(&s, &config, NULL));

  ASSERT_EQ_U64(dv_stack_capacity(&s), 0u);
  ASSERT_FALSE(dv_stack_is_full(&s)); // room to grow is not full

  const uint64_t count = 1000u;
  for (uint64_t i = 0u; i < count; ++i)
    ASSERT_OK(dv_stack_push(&s, &i));

  ASSERT_EQ_U64(dv_stack_size(&s), count);
  ASSERT_TRUE(dv_stack_capacity(&s) >= count);

  for (uint64_t i = count; i-- > 0u;) {
    uint64_t out = 0u;
    ASSERT_OK(dv_stack_pop(&s, &out));
    ASSERT_EQ_U64(out, i);
  }

  ASSERT_TRUE(dv_stack_is_empty(&s));
  dv_stack_destroy(&s);
  return true;
}

static bool test_max_capacity_ceiling(void) {
  dv_stack_t s;
  dv_stack_config_t config = u64_config(2u, true);
  config.max_capacity = 10u;
  ASSERT_OK(dv_stack_init(&s, &config, NULL));

  for (uint64_t i = 0u; i < 10u; ++i)
    ASSERT_OK(dv_stack_push(&s, &i));

  ASSERT_EQ_U64(dv_stack_capacity(&s), 10u);
  ASSERT_TRUE(dv_stack_is_full(&s));

  const uint64_t extra = 42u;
  ASSERT_STATUS(dv_stack_push(&s, &extra), DV_STACK_ERR_FULL);
  ASSERT_STATUS(dv_stack_reserve(&s, 11u), DV_STACK_ERR_FULL);
  ASSERT_EQ_U64(dv_stack_size(&s), 10u);

  dv_stack_destroy(&s);
  return true;
}

static bool test_reserve_shrink_reset_clear(void) {
  dv_stack_t s;
  const dv_stack_config_t config = u64_config(4u, true);
  ASSERT_OK(dv_stack_init(&s, &config, NULL));

  ASSERT_OK(dv_stack_reserve(&s, 128u));
  ASSERT_EQ_U64(dv_stack_capacity(&s), 128u);

  ASSERT_OK(dv_stack_reserve(&s, 64u)); // never shrinks
  ASSERT_EQ_U64(dv_stack_capacity(&s), 128u);

  for (uint64_t i = 0u; i < 5u; ++i)
    ASSERT_OK(dv_stack_push(&s, &i));

  ASSERT_OK(dv_stack_shrink_to_fit(&s));
  ASSERT_EQ_U64(dv_stack_capacity(&s), 5u);
  ASSERT_EQ_U64(dv_stack_size(&s), 5u);
  ASSERT_EQ_U64(*(const uint64_t *)dv_stack_top(&s), 4u);

  dv_stack_clear(&s);
  ASSERT_EQ_U64(dv_stack_size(&s), 0u);
  ASSERT_EQ_U64(dv_stack_capacity(&s), 5u); // clear keeps the buffer

  ASSERT_OK(dv_stack_reserve(&s, 200u));
  for (uint64_t i = 0u; i < 9u; ++i)
    ASSERT_OK(dv_stack_push(&s, &i));

  ASSERT_OK(dv_stack_reset(&s));
  ASSERT_EQ_U64(dv_stack_size(&s), 0u);
  ASSERT_EQ_U64(dv_stack_capacity(&s), 4u); // back to the configured start

  // Shrinking an empty growable stack releases the buffer outright.
  ASSERT_OK(dv_stack_shrink_to_fit(&s));
  ASSERT_EQ_U64(dv_stack_capacity(&s), 0u);
  ASSERT_PTR_EQ(s.buffer, NULL);

  const uint64_t value = 5u;
  ASSERT_OK(dv_stack_push(&s, &value)); // still usable
  ASSERT_EQ_U64(dv_stack_size(&s), 1u);

  dv_stack_destroy(&s);
  return true;
}

// --- Bulk operations ---------------------------------------------------

static bool test_bulk_push_partial_on_full(void) {
  dv_stack_t s;
  const dv_stack_config_t config = u64_config(TEST_CAPACITY, false);
  ASSERT_OK(dv_stack_init(&s, &config, NULL));

  uint64_t items[12];
  for (uint64_t i = 0u; i < 12u; ++i)
    items[i] = i + 1u;

  ASSERT_EQ_U64(dv_stack_push_bulk(&s, items, 12u), TEST_CAPACITY);
  ASSERT_EQ_U64(dv_stack_size(&s), TEST_CAPACITY);
  ASSERT_EQ_U64(*(const uint64_t *)dv_stack_top(&s), TEST_CAPACITY);
  ASSERT_EQ_U64(dv_stack_push_bulk(&s, items, 1u), 0u);

  dv_stack_destroy(&s);
  return true;
}

static bool test_bulk_pop_is_successive_pops(void) {
  dv_stack_t s;
  const dv_stack_config_t config = u64_config(4u, true);
  ASSERT_OK(dv_stack_init(&s, &config, NULL));

  uint64_t items[6];
  for (uint64_t i = 0u; i < 6u; ++i)
    items[i] = i + 1u;

  ASSERT_EQ_U64(dv_stack_push_bulk(&s, items, 6u), 6u);
  ASSERT_EQ_U64(dv_stack_size(&s), 6u);

  uint64_t out[8];
  memset(out, 0xA5, sizeof(out));

  ASSERT_EQ_U64(dv_stack_pop_bulk(&s, out, 4u), 4u);
  ASSERT_EQ_U64(out[0], 6u);
  ASSERT_EQ_U64(out[1], 5u);
  ASSERT_EQ_U64(out[2], 4u);
  ASSERT_EQ_U64(out[3], 3u);
  ASSERT_EQ_U64(dv_stack_size(&s), 2u);

  // Short reads stop at the bottom of the stack.
  ASSERT_EQ_U64(dv_stack_pop_bulk(&s, out, 8u), 2u);
  ASSERT_EQ_U64(out[0], 2u);
  ASSERT_EQ_U64(out[1], 1u);
  ASSERT_TRUE(dv_stack_is_empty(&s));
  ASSERT_EQ_U64(dv_stack_pop_bulk(&s, out, 8u), 0u);

  // A NULL destination discards.
  ASSERT_EQ_U64(dv_stack_push_bulk(&s, items, 6u), 6u);
  ASSERT_EQ_U64(dv_stack_pop_bulk(&s, NULL, 3u), 3u);
  ASSERT_EQ_U64(dv_stack_size(&s), 3u);
  ASSERT_EQ_U64(*(const uint64_t *)dv_stack_top(&s), 3u);

  dv_stack_destroy(&s);
  return true;
}

static bool test_bulk_zero_and_null_arguments(void) {
  dv_stack_t s;
  const dv_stack_config_t config = u64_config(4u, true);
  ASSERT_OK(dv_stack_init(&s, &config, NULL));

  uint64_t items[2] = {1u, 2u};
  uint64_t out[2] = {0u, 0u};

  ASSERT_EQ_U64(dv_stack_push_bulk(&s, items, 0u), 0u);
  ASSERT_EQ_U64(dv_stack_push_bulk(&s, NULL, 4u), 0u);
  ASSERT_EQ_U64(dv_stack_pop_bulk(&s, out, 0u), 0u);
  ASSERT_EQ_U64(dv_stack_size(&s), 0u);

  dv_stack_destroy(&s);
  return true;
}

static bool test_bulk_push_grows_once(void) {
  tracking_state_t st;
  tracking_init(&st);
  const dv_stack_allocator_t hooks = tracking_hooks(&st, true);

  dv_stack_t s;
  const dv_stack_config_t config = u64_config(0u, true);
  ASSERT_OK(dv_stack_init(&s, &config, &hooks));

  uint64_t items[1000];
  for (uint64_t i = 0u; i < 1000u; ++i)
    items[i] = i;

  // The bulk path sizes the buffer for the whole request up front rather than
  // walking the growth curve one push at a time.
  ASSERT_EQ_U64(dv_stack_push_bulk(&s, items, 1000u), 1000u);
  ASSERT_EQ_U64(st.alloc_calls + st.realloc_calls, 1u);
  ASSERT_TRUE(dv_stack_capacity(&s) >= 1000u);

  for (uint64_t i = 1000u; i-- > 0u;) {
    uint64_t out = 0u;
    ASSERT_OK(dv_stack_pop(&s, &out));
    ASSERT_EQ_U64(out, i);
  }

  dv_stack_destroy(&s);
  ASSERT_EQ_U64(st.live_bytes, 0u);
  return true;
}

// --- Pointer specialization --------------------------------------------

static bool test_pointer_mode(void) {
  dv_stack_t s;
  ASSERT_OK(dv_stack_init_ptr(&s, 4u, true, NULL));
  ASSERT_TRUE(dv_stack_is_ptr_mode(&s));
  ASSERT_EQ_U64(dv_stack_elem_size(&s), sizeof(void *));

  uint64_t objects[6];
  for (uint64_t i = 0u; i < 6u; ++i)
    objects[i] = i + 1u;

  void *out = (void *)0x1;
  ASSERT_STATUS(dv_stack_pop_ptr(&s, &out), DV_STACK_ERR_EMPTY);
  ASSERT_PTR_EQ(out, NULL);
  out = (void *)0x1;
  ASSERT_STATUS(dv_stack_peek_ptr(&s, &out), DV_STACK_ERR_EMPTY);
  ASSERT_PTR_EQ(out, NULL);

  for (size_t i = 0u; i < 6u; ++i)
    ASSERT_OK(dv_stack_push_ptr(&s, &objects[i]));

  ASSERT_EQ_U64(dv_stack_size(&s), 6u);

  ASSERT_OK(dv_stack_peek_ptr(&s, &out));
  ASSERT_PTR_EQ(out, &objects[5]);
  ASSERT_EQ_U64(dv_stack_size(&s), 6u);

  for (size_t i = 6u; i-- > 0u;) {
    ASSERT_OK(dv_stack_pop_ptr(&s, &out));
    ASSERT_PTR_EQ(out, &objects[i]);
  }

  ASSERT_TRUE(dv_stack_is_empty(&s));
  ASSERT_STATUS(dv_stack_push_ptr(NULL, &objects[0]), DV_STACK_ERR_INVALID);
  ASSERT_STATUS(dv_stack_pop_ptr(&s, NULL), DV_STACK_ERR_INVALID);

  // Pointer slots and generic slots are the same storage.
  void *raw = &objects[0];
  ASSERT_OK(dv_stack_push(&s, &raw));
  ASSERT_OK(dv_stack_pop_ptr(&s, &out));
  ASSERT_PTR_EQ(out, &objects[0]);

  dv_stack_destroy(&s);
  return true;
}

static bool test_pointer_mode_detection(void) {
  dv_stack_t generic;
  const dv_stack_config_t narrow = {
      .elem_size = 4u,
      .elem_align = 4u,
      .initial_capacity = 4u,
      .max_capacity = 0u,
      .growable = false,
      .cache_aligned = false,
  };
  ASSERT_OK(dv_stack_init(&generic, &narrow, NULL));
  ASSERT_FALSE(dv_stack_is_ptr_mode(&generic));

#if DV_STACK_ASSERTS_DISABLED
  void *value = &generic;
  void *out = NULL;
  ASSERT_STATUS(dv_stack_push_ptr(&generic, value), DV_STACK_ERR_INVALID);
  ASSERT_STATUS(dv_stack_pop_ptr(&generic, &out), DV_STACK_ERR_INVALID);
  ASSERT_STATUS(dv_stack_peek_ptr(&generic, &out), DV_STACK_ERR_INVALID);
#endif

  dv_stack_destroy(&generic);

  // A generic stack whose stride and alignment happen to match void * is a
  // pointer stack as far as the specialization is concerned.
  dv_stack_t matching;
  const dv_stack_config_t compatible = {
      .elem_size = sizeof(void *),
      .elem_align = alignof(void *),
      .initial_capacity = 2u,
      .max_capacity = 0u,
      .growable = false,
      .cache_aligned = false,
  };
  ASSERT_OK(dv_stack_init(&matching, &compatible, NULL));
  ASSERT_TRUE(dv_stack_is_ptr_mode(&matching));
  dv_stack_destroy(&matching);
  return true;
}

// --- Memory contracts --------------------------------------------------

static bool run_allocator_accounting(bool with_realloc) {
  tracking_state_t st;
  tracking_init(&st);
  const dv_stack_allocator_t hooks = tracking_hooks(&st, with_realloc);

  dv_stack_t s;
  const dv_stack_config_t config = u64_config(2u, true);
  ASSERT_OK(dv_stack_init(&s, &config, &hooks));
  ASSERT_EQ_U64(st.live_bytes, 2u * sizeof(uint64_t));

  for (uint64_t i = 0u; i < 500u; ++i)
    ASSERT_OK(dv_stack_push(&s, &i));

  ASSERT_EQ_U64(st.live_bytes, dv_stack_capacity(&s) * sizeof(uint64_t));
  ASSERT_TRUE(st.align_respected);

  // Live elements survive every reallocation.
  for (uint64_t i = 500u; i-- > 0u;) {
    uint64_t out = 0u;
    ASSERT_OK(dv_stack_pop(&s, &out));
    ASSERT_EQ_U64(out, i);
  }

  ASSERT_OK(dv_stack_reset(&s));
  ASSERT_EQ_U64(st.live_bytes, 2u * sizeof(uint64_t));

  dv_stack_destroy(&s);
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
  const dv_stack_allocator_t hooks = tracking_hooks(&st, true);

  dv_stack_t s;
  const dv_stack_config_t config = u64_config(2u, true);

  st.budget = 0u;
  ASSERT_STATUS(dv_stack_init(&s, &config, &hooks), DV_STACK_ERR_NOMEM);
  ASSERT_EQ_U64(dv_stack_capacity(&s), 0u);
  dv_stack_destroy(&s);

  st.budget = 1u; // enough for the initial buffer, nothing more
  ASSERT_OK(dv_stack_init(&s, &config, &hooks));

  const uint64_t a = 1u;
  const uint64_t b = 2u;
  const uint64_t c = 3u;
  ASSERT_OK(dv_stack_push(&s, &a));
  ASSERT_OK(dv_stack_push(&s, &b));
  ASSERT_STATUS(dv_stack_push(&s, &c), DV_STACK_ERR_NOMEM);

  // A refused growth leaves the stack exactly as it was.
  ASSERT_EQ_U64(dv_stack_size(&s), 2u);
  ASSERT_EQ_U64(dv_stack_capacity(&s), 2u);
  ASSERT_EQ_U64(*(const uint64_t *)dv_stack_top(&s), 2u);
  ASSERT_STATUS(dv_stack_reserve(&s, 64u), DV_STACK_ERR_NOMEM);
  ASSERT_EQ_U64(dv_stack_capacity(&s), 2u);

  // A bulk push falls back to filling the room it already has.
  uint64_t items[4] = {9u, 9u, 9u, 9u};
  ASSERT_EQ_U64(dv_stack_push_bulk(&s, items, 4u), 0u);

  st.budget = SIZE_MAX;
  ASSERT_OK(dv_stack_push(&s, &c));
  ASSERT_EQ_U64(dv_stack_size(&s), 3u);
  ASSERT_EQ_U64(*(const uint64_t *)dv_stack_top(&s), 3u);

  dv_stack_destroy(&s);
  ASSERT_EQ_U64(st.live_bytes, 0u);
  return true;
}

static bool test_byte_size_overflow_is_refused(void) {
  dv_stack_t s;
  const dv_stack_config_t config = {
      .elem_size = SIZE_MAX / 2u,
      .elem_align = 1u,
      .initial_capacity = 0u,
      .max_capacity = 0u,
      .growable = true,
      .cache_aligned = false,
  };

  ASSERT_OK(dv_stack_init(&s, &config, NULL));
  ASSERT_STATUS(dv_stack_reserve(&s, 4u), DV_STACK_ERR_OVERFLOW);
  ASSERT_STATUS(dv_stack_grow_for_push(&s), DV_STACK_ERR_OVERFLOW);
  ASSERT_EQ_U64(dv_stack_capacity(&s), 0u);
  dv_stack_destroy(&s);

  dv_stack_config_t huge = config;
  huge.initial_capacity = 8u;
  ASSERT_STATUS(dv_stack_init(&s, &huge, NULL), DV_STACK_ERR_OVERFLOW);
  ASSERT_EQ_U64(dv_stack_capacity(&s), 0u);
  return true;
}

static bool test_cache_aligned_elements(void) {
  tracking_state_t st;
  tracking_init(&st);
  const dv_stack_allocator_t hooks = tracking_hooks(&st, true);

  dv_stack_t s;
  const dv_stack_config_t config = {
      .elem_size = DV_CACHE_LINE_SIZE,
      .elem_align = DV_CACHE_LINE_SIZE,
      .initial_capacity = 4u,
      .max_capacity = 0u,
      .growable = true,
      .cache_aligned = true,
  };

  ASSERT_OK(dv_stack_init(&s, &config, &hooks));
  ASSERT_EQ_U64(st.last_align, DV_CACHE_LINE_SIZE);

  unsigned char element[DV_CACHE_LINE_SIZE];
  memset(element, 0x5A, sizeof(element));

  for (size_t i = 0u; i < 40u; ++i) {
    element[0] = (unsigned char)i;
    ASSERT_OK(dv_stack_push(&s, element));

    const uintptr_t addr = (uintptr_t)dv_stack_top(&s);
    ASSERT_EQ_U64(addr % DV_CACHE_LINE_SIZE, 0u);
  }

  ASSERT_TRUE(st.align_respected);

  for (size_t i = 40u; i-- > 0u;) {
    unsigned char out[DV_CACHE_LINE_SIZE];
    ASSERT_OK(dv_stack_pop(&s, out));
    ASSERT_EQ_U64(out[0], (unsigned char)i);
    ASSERT_EQ_U64(out[DV_CACHE_LINE_SIZE - 1u], 0x5A);
  }

  dv_stack_destroy(&s);
  ASSERT_EQ_U64(st.live_bytes, 0u);
  return true;
}

// --- Stress ------------------------------------------------------------

// Differential test: every operation is mirrored onto a plain array and the
// two are compared after each step.
static bool test_randomized_against_reference(void) {
  tracking_state_t st;
  tracking_init(&st);
  const dv_stack_allocator_t hooks = tracking_hooks(&st, true);

  dv_stack_t s;
  const dv_stack_config_t config = u64_config(0u, true);
  ASSERT_OK(dv_stack_init(&s, &config, &hooks));

  const size_t reference_capacity = 4096u;
  uint64_t *reference =
      (uint64_t *)malloc(reference_capacity * sizeof(*reference));
  ASSERT_TRUE(reference != NULL);
  size_t reference_top = 0u;

  uint64_t rng = 0x9E3779B97F4A7C15ull;
  bool ok = true;

  for (size_t op = 0u; op < STRESS_OPS && ok; ++op) {
    const uint64_t roll = xorshift64(&rng);

    if ((roll & 3u) != 0u && reference_top < reference_capacity) {
      const uint64_t value = roll >> 3;
      ok = dv_stack_push(&s, &value) == DV_STACK_OK;
      reference[reference_top++] = value;
    } else if (reference_top > 0u) {
      uint64_t out = 0u;
      ok = dv_stack_pop(&s, &out) == DV_STACK_OK;
      ok = ok && out == reference[--reference_top];
    } else {
      uint64_t out = 0u;
      ok = dv_stack_pop(&s, &out) == DV_STACK_ERR_EMPTY;
    }

    ok = ok && dv_stack_size(&s) == reference_top;

    // Periodically make the buffer move under the live elements.
    if ((op & 0xFFFu) == 0xFFFu)
      ok = ok && dv_stack_shrink_to_fit(&s) == DV_STACK_OK;
  }

  free(reference);
  dv_stack_destroy(&s);

  ASSERT_TRUE(ok);
  ASSERT_EQ_U64(st.live_bytes, 0u);
  return true;
}

// --- Thread-safe wrapper -----------------------------------------------

typedef struct {
  dv_stack_mt_t *stack;
  uint64_t sum;
  size_t count;
  size_t thread_id;
} mt_arg_t;

static void *mt_producer(void *arg) {
  mt_arg_t *a = (mt_arg_t *)arg;

  for (size_t i = 0u; i < MT_ITEMS_PER_THREAD; ++i) {
    const uint64_t value =
        ((uint64_t)a->thread_id * (uint64_t)MT_ITEMS_PER_THREAD) + i + 1u;

    while (dv_stack_mt_push(a->stack, &value) != DV_STACK_OK)
      ;

    a->sum += value;
    ++a->count;
  }

  return NULL;
}

static void *mt_consumer(void *arg) {
  mt_arg_t *a = (mt_arg_t *)arg;
  const size_t target = MT_ITEMS_PER_THREAD;

  while (a->count < target) {
    uint64_t out = 0u;
    if (dv_stack_mt_pop(a->stack, &out) != DV_STACK_OK)
      continue;

    a->sum += out;
    ++a->count;
  }

  return NULL;
}

// Every pushed value is popped exactly once: the sums and counts on both sides
// must agree and the stack must drain to empty.
static bool test_mt_wrapper_conserves_elements(void) {
  dv_stack_mt_t stack;
  const dv_stack_config_t config = u64_config(64u, true);
  ASSERT_OK(dv_stack_mt_init(&stack, &config, NULL));

  pthread_t producers[MT_THREADS];
  pthread_t consumers[MT_THREADS];
  mt_arg_t pargs[MT_THREADS];
  mt_arg_t cargs[MT_THREADS];

  for (size_t i = 0u; i < MT_THREADS; ++i) {
    pargs[i] =
        (mt_arg_t){.stack = &stack, .sum = 0u, .count = 0u, .thread_id = i};
    cargs[i] =
        (mt_arg_t){.stack = &stack, .sum = 0u, .count = 0u, .thread_id = i};
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
  size_t produced_count = 0u;
  size_t consumed_count = 0u;

  for (size_t i = 0u; i < MT_THREADS; ++i) {
    produced_sum += pargs[i].sum;
    produced_count += pargs[i].count;
    consumed_sum += cargs[i].sum;
    consumed_count += cargs[i].count;
  }

  const size_t expected = MT_THREADS * MT_ITEMS_PER_THREAD;
  ASSERT_EQ_U64(produced_count, expected);
  ASSERT_EQ_U64(consumed_count, expected);
  ASSERT_EQ_U64(produced_sum, consumed_sum);
  ASSERT_EQ_U64(dv_stack_mt_size(&stack), 0u);
  ASSERT_TRUE(dv_stack_mt_is_empty(&stack));

  dv_stack_mt_destroy(&stack);
  return true;
}

static void mt_drain_to_sum(dv_stack_t *s, void *user_data) {
  uint64_t *total = (uint64_t *)user_data;
  uint64_t value = 0u;

  while (dv_stack_pop(s, &value) == DV_STACK_OK)
    *total += value;
}

static bool test_mt_critical_section(void) {
  dv_stack_mt_t stack;
  ASSERT_OK(dv_stack_mt_init_ptr(&stack, 4u, true, NULL));
  ASSERT_TRUE(dv_stack_is_ptr_mode(&stack.stack));

  uint64_t objects[4] = {1u, 2u, 3u, 4u};
  for (size_t i = 0u; i < 4u; ++i)
    ASSERT_OK(dv_stack_mt_push_ptr(&stack, &objects[i]));

  void *out = NULL;
  ASSERT_OK(dv_stack_mt_pop_ptr(&stack, &out));
  ASSERT_PTR_EQ(out, &objects[3]);
  ASSERT_EQ_U64(dv_stack_mt_size(&stack), 3u);

  dv_stack_mt_clear(&stack);
  ASSERT_EQ_U64(dv_stack_mt_size(&stack), 0u);
  dv_stack_mt_destroy(&stack);

  dv_stack_mt_t generic;
  const dv_stack_config_t config = u64_config(4u, true);
  ASSERT_OK(dv_stack_mt_init(&generic, &config, NULL));
  ASSERT_OK(dv_stack_mt_reserve(&generic, 32u));

  uint64_t items[5] = {1u, 2u, 3u, 4u, 5u};
  ASSERT_EQ_U64(dv_stack_mt_push_bulk(&generic, items, 5u), 5u);

  uint64_t peeked = 0u;
  ASSERT_OK(dv_stack_mt_peek(&generic, &peeked));
  ASSERT_EQ_U64(peeked, 5u);

  uint64_t popped[2] = {0u, 0u};
  ASSERT_EQ_U64(dv_stack_mt_pop_bulk(&generic, popped, 2u), 2u);
  ASSERT_EQ_U64(popped[0], 5u);
  ASSERT_EQ_U64(popped[1], 4u);

  uint64_t total = 0u;
  ASSERT_OK(dv_stack_mt_with(&generic, mt_drain_to_sum, &total));
  ASSERT_EQ_U64(total, 6u); // 1 + 2 + 3
  ASSERT_EQ_U64(dv_stack_mt_size(&generic), 0u);
  ASSERT_STATUS(dv_stack_mt_with(&generic, NULL, NULL), DV_STACK_ERR_INVALID);

  dv_stack_mt_destroy(&generic);
  return true;
}

int main(void) {
  const test_case_t tests[] = {
      {"init rejects bad config", test_init_rejects_bad_config},
      {"empty stack accessors", test_empty_stack_accessors},
      {"status strings", test_status_strings},
      {"push pop lifo order", test_push_pop_lifo_order},
      {"peek top and depth access", test_peek_top_and_depth_access},
      {"fixed capacity boundary", test_fixed_capacity_boundary},
      {"growable growth", test_growable_growth},
      {"max capacity ceiling", test_max_capacity_ceiling},
      {"reserve shrink reset clear", test_reserve_shrink_reset_clear},
      {"bulk push partial on full", test_bulk_push_partial_on_full},
      {"bulk pop is successive pops", test_bulk_pop_is_successive_pops},
      {"bulk zero and null arguments", test_bulk_zero_and_null_arguments},
      {"bulk push grows once", test_bulk_push_grows_once},
      {"pointer mode", test_pointer_mode},
      {"pointer mode detection", test_pointer_mode_detection},
      {"allocator accounting with realloc",
       test_allocator_accounting_with_realloc},
      {"allocator accounting without realloc",
       test_allocator_accounting_without_realloc},
      {"allocation failure is recoverable",
       test_allocation_failure_is_recoverable},
      {"byte size overflow is refused", test_byte_size_overflow_is_refused},
      {"cache aligned elements", test_cache_aligned_elements},
      {"randomized against reference", test_randomized_against_reference},
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
