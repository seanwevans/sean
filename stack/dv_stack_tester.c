// dv_stack_tester.c - soak harness for the stack module.
//
// Each phase drives one behaviour hard enough to shake out mistakes the unit
// tests only sample: LIFO ordering across millions of operations, buffer moves
// under live elements, allocator accounting, and the mutex wrapper under
// contention. Every phase reports independently and the run fails if any one
// of them does.

#include "dv_stack.h"
#include "dv_stack_mt.h"

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef STACK_CAPACITY
#define STACK_CAPACITY 65536u
#endif

#ifndef STACK_ITEMS
#define STACK_ITEMS 4000000u
#endif

#ifndef STACK_CYCLES
#define STACK_CYCLES 64u
#endif

#ifndef STACK_THREADS
#define STACK_THREADS 8u
#endif

#ifndef STACK_SEED
#define STACK_SEED 0x9E3779B97F4A7C15ull
#endif

#ifndef BATCH_SIZE
#define BATCH_SIZE 64u
#endif

// --- Accounting allocator ----------------------------------------------

typedef struct {
  size_t live_bytes;
  size_t peak_bytes;
  size_t alloc_calls;
  size_t realloc_calls;
  size_t free_calls;
  bool align_respected;
} account_t;

static void *account_raw_alloc(size_t size, size_t align) {
  const size_t padded = (size + align - 1u) & ~(align - 1u);
  return aligned_alloc(align, padded);
}

static void *account_alloc(size_t size, size_t align, void *user_data) {
  account_t *a = (account_t *)user_data;

  void *mem = account_raw_alloc(size, align);
  if (!mem)
    return NULL;

  if (((uintptr_t)mem & (uintptr_t)(align - 1u)) != 0u)
    a->align_respected = false;

  ++a->alloc_calls;
  a->live_bytes += size;
  if (a->live_bytes > a->peak_bytes)
    a->peak_bytes = a->live_bytes;

  return mem;
}

static void *account_realloc(void *ptr, size_t old_size, size_t new_size,
                             size_t align, void *user_data) {
  account_t *a = (account_t *)user_data;

  void *mem = account_raw_alloc(new_size, align);
  if (!mem)
    return NULL;

  if (((uintptr_t)mem & (uintptr_t)(align - 1u)) != 0u)
    a->align_respected = false;

  const size_t copy = old_size < new_size ? old_size : new_size;
  if (copy > 0u)
    memcpy(mem, ptr, copy);
  free(ptr);

  ++a->realloc_calls;
  a->live_bytes -= old_size;
  a->live_bytes += new_size;
  if (a->live_bytes > a->peak_bytes)
    a->peak_bytes = a->live_bytes;

  return mem;
}

static void account_free(void *ptr, size_t size, size_t align,
                         void *user_data) {
  account_t *a = (account_t *)user_data;
  (void)align;

  if (!ptr)
    return;

  ++a->free_calls;
  a->live_bytes -= size;
  free(ptr);
}

static dv_stack_allocator_t account_hooks(account_t *a) {
  memset(a, 0, sizeof(*a));
  a->align_respected = true;

  const dv_stack_allocator_t hooks = {
      .alloc = account_alloc,
      .realloc = account_realloc,
      .free = account_free,
      .user_data = a,
  };
  return hooks;
}

// --- Shared helpers ----------------------------------------------------

static uint64_t xorshift64(uint64_t *state) {
  uint64_t x = *state;
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  *state = x;
  return x;
}

static void fail_pthread(int err, const char *what) {
  if (err == 0)
    return;

  fprintf(stderr, "%s failed: %s\n", what, strerror(err));
  exit(EXIT_FAILURE);
}

static void report(const char *phase, bool ok) {
  printf("\t%-28s %s\n", phase, ok ? "ok" : "FAILED");
}

// --- Phase 1: fixed-capacity fill and drain ----------------------------

// Fills a fixed stack to the brim and drains it, cycle after cycle, checking
// LIFO order on every element and that the boundary conditions stay exact.
static bool phase_fixed_fill_drain(void) {
  dv_stack_t s;
  const dv_stack_config_t config = {
      .elem_size = sizeof(uint64_t),
      .elem_align = alignof(uint64_t),
      .initial_capacity = STACK_CAPACITY,
      .max_capacity = 0u,
      .growable = false,
      .cache_aligned = true,
  };

  if (dv_stack_init(&s, &config, NULL) != DV_STACK_OK)
    return false;

  bool ok = true;
  for (size_t cycle = 0u; cycle < STACK_CYCLES && ok; ++cycle) {
    for (uint64_t i = 0u; i < (uint64_t)STACK_CAPACITY && ok; ++i) {
      const uint64_t value = ((uint64_t)cycle << 40) ^ (i + 1u);
      ok = dv_stack_push(&s, &value) == DV_STACK_OK;
    }

    const uint64_t rejected = 0u;
    ok = ok && dv_stack_is_full(&s);
    ok = ok && dv_stack_push(&s, &rejected) == DV_STACK_ERR_FULL;
    ok = ok && dv_stack_size(&s) == STACK_CAPACITY;

    for (uint64_t i = (uint64_t)STACK_CAPACITY; i-- > 0u && ok;) {
      const uint64_t expected = ((uint64_t)cycle << 40) ^ (i + 1u);
      uint64_t out = 0u;
      ok = dv_stack_pop(&s, &out) == DV_STACK_OK && out == expected;
    }

    uint64_t drained = 0u;
    ok = ok && dv_stack_is_empty(&s);
    ok = ok && dv_stack_pop(&s, &drained) == DV_STACK_ERR_EMPTY;
  }

  dv_stack_destroy(&s);
  return ok;
}

// --- Phase 2: growth and shrink under live elements --------------------

// Repeatedly grows past the initial capacity and shrinks back, verifying the
// live prefix survives every buffer move and that all bytes are returned.
static bool phase_grow_shrink_cycles(void) {
  account_t account;
  const dv_stack_allocator_t hooks = account_hooks(&account);

  dv_stack_t s;
  const dv_stack_config_t config = {
      .elem_size = sizeof(uint64_t),
      .elem_align = alignof(uint64_t),
      .initial_capacity = 0u,
      .max_capacity = 0u,
      .growable = true,
      .cache_aligned = false,
  };

  if (dv_stack_init(&s, &config, &hooks) != DV_STACK_OK)
    return false;

  const size_t high_water = 200000u;
  bool ok = true;

  for (size_t cycle = 0u; cycle < 8u && ok; ++cycle) {
    for (uint64_t i = 0u; i < (uint64_t)high_water && ok; ++i) {
      const uint64_t value = ((uint64_t)cycle * (uint64_t)high_water) + i;
      ok = dv_stack_push(&s, &value) == DV_STACK_OK;
    }

    ok = ok && dv_stack_size(&s) == high_water;
    ok = ok && dv_stack_shrink_to_fit(&s) == DV_STACK_OK;
    ok = ok && dv_stack_capacity(&s) == high_water;

    for (uint64_t i = (uint64_t)high_water; i-- > 0u && ok;) {
      const uint64_t expected = ((uint64_t)cycle * (uint64_t)high_water) + i;
      uint64_t out = 0u;
      ok = dv_stack_pop(&s, &out) == DV_STACK_OK && out == expected;
    }

    ok = ok && dv_stack_reset(&s) == DV_STACK_OK;
    ok = ok && dv_stack_capacity(&s) == 0u;
  }

  dv_stack_destroy(&s);

  ok = ok && account.align_respected;
  ok = ok && account.live_bytes == 0u;

  printf("\t  allocs=%zu reallocs=%zu frees=%zu peak_bytes=%zu\n",
         account.alloc_calls, account.realloc_calls, account.free_calls,
         account.peak_bytes);

  return ok;
}

// --- Phase 3: bulk throughput path -------------------------------------

// Drives the batch helpers over the whole item budget and checks the
// successive-pop ordering contract holds at scale.
static bool phase_bulk_roundtrip(void) {
  dv_stack_t s;
  const dv_stack_config_t config = {
      .elem_size = sizeof(uint64_t),
      .elem_align = alignof(uint64_t),
      .initial_capacity = STACK_CAPACITY,
      .max_capacity = STACK_CAPACITY,
      .growable = true,
      .cache_aligned = true,
  };

  if (dv_stack_init(&s, &config, NULL) != DV_STACK_OK)
    return false;

  uint64_t batch[BATCH_SIZE];
  uint64_t drained[BATCH_SIZE];
  uint64_t produced_sum = 0u;
  uint64_t consumed_sum = 0u;
  uint64_t produced_xor = 0u;
  uint64_t consumed_xor = 0u;
  size_t produced = 0u;
  size_t consumed = 0u;
  bool ok = true;

  while (produced < (size_t)STACK_ITEMS && ok) {
    size_t n = (size_t)STACK_ITEMS - produced;
    if (n > BATCH_SIZE)
      n = BATCH_SIZE;

    for (size_t i = 0u; i < n; ++i) {
      const uint64_t value = (uint64_t)(produced + i) + 1u;
      batch[i] = value;
      produced_sum += value;
      produced_xor ^= value;
    }

    const size_t pushed = dv_stack_push_bulk(&s, batch, n);
    ok = pushed == n;
    produced += pushed;

    const size_t popped = dv_stack_pop_bulk(&s, drained, pushed);
    ok = ok && popped == pushed;

    for (size_t i = 0u; i < popped && ok; ++i) {
      // out[0] is the former top, so the batch comes back reversed.
      ok = drained[i] == batch[pushed - 1u - i];
      consumed_sum += drained[i];
      consumed_xor ^= drained[i];
      ++consumed;
    }
  }

  ok = ok && dv_stack_is_empty(&s);
  ok = ok && produced == consumed;
  ok = ok && produced_sum == consumed_sum;
  ok = ok && produced_xor == consumed_xor;

  dv_stack_destroy(&s);

  printf("\t  items=%zu sum=%" PRIu64 " xor=%" PRIu64 "\n", consumed,
         consumed_sum, consumed_xor);

  return ok;
}

// --- Phase 4: pointer specialization over heap objects -----------------

// Every pushed object must come back exactly once; running this phase under
// ASAN turns any ownership mistake into a hard failure.
static bool phase_pointer_objects(void) {
  dv_stack_t s;
  if (dv_stack_init_ptr(&s, 0u, true, NULL) != DV_STACK_OK)
    return false;

  const size_t count = 100000u;
  uint64_t expected_sum = 0u;
  uint64_t observed_sum = 0u;
  bool ok = true;

  for (size_t i = 0u; i < count && ok; ++i) {
    uint64_t *object = (uint64_t *)malloc(sizeof(*object));
    if (!object) {
      ok = false;
      break;
    }

    *object = (uint64_t)i + 1u;
    expected_sum += *object;
    ok = dv_stack_push_ptr(&s, object) == DV_STACK_OK;
    if (!ok)
      free(object);
  }

  ok = ok && dv_stack_size(&s) == count;

  for (size_t i = count; i-- > 0u;) {
    void *out = NULL;
    if (dv_stack_pop_ptr(&s, &out) != DV_STACK_OK) {
      ok = false;
      break;
    }

    uint64_t *object = (uint64_t *)out;
    ok = ok && *object == (uint64_t)i + 1u;
    observed_sum += *object;
    free(object);
  }

  // Drain anything left behind so a failure does not also leak.
  void *leftover = NULL;
  while (dv_stack_pop_ptr(&s, &leftover) == DV_STACK_OK)
    free(leftover);

  dv_stack_destroy(&s);
  return ok && expected_sum == observed_sum;
}

// --- Phase 5: randomized differential ----------------------------------

// Mirrors a random operation mix onto a plain array and compares after every
// step, with periodic shrinks to keep the buffer moving.
static bool phase_random_differential(void) {
  account_t account;
  const dv_stack_allocator_t hooks = account_hooks(&account);

  dv_stack_t s;
  const dv_stack_config_t config = {
      .elem_size = sizeof(uint64_t),
      .elem_align = alignof(uint64_t),
      .initial_capacity = 16u,
      .max_capacity = 0u,
      .growable = true,
      .cache_aligned = false,
  };

  if (dv_stack_init(&s, &config, &hooks) != DV_STACK_OK)
    return false;

  const size_t reference_capacity = 1u << 16;
  uint64_t *reference =
      (uint64_t *)malloc(reference_capacity * sizeof(*reference));
  if (!reference) {
    dv_stack_destroy(&s);
    return false;
  }

  size_t reference_top = 0u;
  size_t pushes = 0u;
  size_t pops = 0u;
  uint64_t rng = STACK_SEED;
  bool ok = true;

  for (size_t op = 0u; op < (size_t)STACK_ITEMS && ok; ++op) {
    const uint64_t roll = xorshift64(&rng);

    if ((roll & 3u) != 0u && reference_top < reference_capacity) {
      const uint64_t value = roll >> 3;
      ok = dv_stack_push(&s, &value) == DV_STACK_OK;
      reference[reference_top++] = value;
      ++pushes;
    } else if (reference_top > 0u) {
      uint64_t out = 0u;
      ok = dv_stack_pop(&s, &out) == DV_STACK_OK;
      ok = ok && out == reference[--reference_top];
      ++pops;
    } else {
      uint64_t out = 0u;
      ok = dv_stack_pop(&s, &out) == DV_STACK_ERR_EMPTY;
    }

    ok = ok && dv_stack_size(&s) == reference_top;

    if ((op & 0xFFFFu) == 0xFFFFu) {
      ok = ok && dv_stack_shrink_to_fit(&s) == DV_STACK_OK;
      ok = ok && dv_stack_capacity(&s) == reference_top;
    }
  }

  // The remaining elements must still unwind in order after all that churn.
  while (ok && reference_top > 0u) {
    uint64_t out = 0u;
    ok = dv_stack_pop(&s, &out) == DV_STACK_OK;
    ok = ok && out == reference[--reference_top];
  }

  free(reference);
  dv_stack_destroy(&s);

  ok = ok && account.live_bytes == 0u;

  printf("\t  pushes=%zu pops=%zu peak_bytes=%zu\n", pushes, pops,
         account.peak_bytes);

  return ok;
}

// --- Phase 6: mutex wrapper under contention ---------------------------

typedef struct {
  dv_stack_mt_t *stack;
  size_t thread_id;
  size_t items;
  uint64_t sum;
  uint64_t xor_value;
  size_t count;
} mt_thread_args_t;

static void *mt_producer(void *arg) {
  mt_thread_args_t *a = (mt_thread_args_t *)arg;

  for (size_t i = 0u; i < a->items; ++i) {
    const uint64_t value =
        ((uint64_t)a->thread_id * (uint64_t)a->items) + i + 1u;
    if (dv_stack_mt_push(a->stack, &value) != DV_STACK_OK)
      return NULL;

    a->sum += value;
    a->xor_value ^= value;
    ++a->count;
  }

  return NULL;
}

static void *mt_consumer(void *arg) {
  mt_thread_args_t *a = (mt_thread_args_t *)arg;

  while (a->count < a->items) {
    uint64_t out = 0u;
    if (dv_stack_mt_pop(a->stack, &out) != DV_STACK_OK)
      continue;

    a->sum += out;
    a->xor_value ^= out;
    ++a->count;
  }

  return NULL;
}

// The wrapper makes no ordering promise across threads, so the invariant under
// test is conservation: the multiset that goes in is the multiset that comes
// out, checked with a sum and an xor.
static bool phase_mt_contention(void) {
  if ((STACK_THREADS % 2u) != 0u || STACK_THREADS < 2u) {
    fprintf(stderr, "STACK_THREADS must be an even number >= 2.\n");
    return false;
  }

  const size_t sides = (size_t)STACK_THREADS / 2u;
  const size_t items = 100000u;

  dv_stack_mt_t stack;
  const dv_stack_config_t config = {
      .elem_size = sizeof(uint64_t),
      .elem_align = alignof(uint64_t),
      .initial_capacity = 1024u,
      .max_capacity = 0u,
      .growable = true,
      .cache_aligned = true,
  };

  if (dv_stack_mt_init(&stack, &config, NULL) != DV_STACK_OK)
    return false;

  pthread_t *threads = (pthread_t *)malloc(2u * sides * sizeof(*threads));
  mt_thread_args_t *args =
      (mt_thread_args_t *)calloc(2u * sides, sizeof(*args));

  if (!threads || !args) {
    free(threads);
    free(args);
    dv_stack_mt_destroy(&stack);
    return false;
  }

  for (size_t i = 0u; i < 2u * sides; ++i) {
    args[i].stack = &stack;
    args[i].thread_id = i % sides;
    args[i].items = items;
  }

  for (size_t i = 0u; i < sides; ++i)
    fail_pthread(pthread_create(&threads[sides + i], NULL, mt_consumer,
                                &args[sides + i]),
                 "pthread_create(consumer)");

  for (size_t i = 0u; i < sides; ++i)
    fail_pthread(pthread_create(&threads[i], NULL, mt_producer, &args[i]),
                 "pthread_create(producer)");

  for (size_t i = 0u; i < 2u * sides; ++i)
    fail_pthread(pthread_join(threads[i], NULL), "pthread_join");

  uint64_t produced_sum = 0u;
  uint64_t consumed_sum = 0u;
  uint64_t produced_xor = 0u;
  uint64_t consumed_xor = 0u;
  size_t produced = 0u;
  size_t consumed = 0u;

  for (size_t i = 0u; i < sides; ++i) {
    produced_sum += args[i].sum;
    produced_xor ^= args[i].xor_value;
    produced += args[i].count;

    consumed_sum += args[sides + i].sum;
    consumed_xor ^= args[sides + i].xor_value;
    consumed += args[sides + i].count;
  }

  const size_t expected = sides * items;
  const bool ok = produced == expected && consumed == expected &&
                  produced_sum == consumed_sum &&
                  produced_xor == consumed_xor &&
                  dv_stack_mt_size(&stack) == 0u;

  printf("\t  threads=%zu items=%zu sum=%" PRIu64 " xor=%" PRIu64 "\n",
         2u * sides, consumed, consumed_sum, consumed_xor);

  free(threads);
  free(args);
  dv_stack_mt_destroy(&stack);
  return ok;
}

// --- Driver ------------------------------------------------------------

typedef struct {
  const char *name;
  bool (*fn)(void);
} phase_t;

int main(void) {
  const phase_t phases[] = {
      {"fixed fill and drain", phase_fixed_fill_drain},
      {"grow and shrink cycles", phase_grow_shrink_cycles},
      {"bulk roundtrip", phase_bulk_roundtrip},
      {"pointer objects", phase_pointer_objects},
      {"random differential", phase_random_differential},
      {"mutex wrapper contention", phase_mt_contention},
  };

  const size_t nphases = sizeof(phases) / sizeof(phases[0]);

  printf("Initializing\n");
  printf("\tStack Capacity:   %zu\n", (size_t)STACK_CAPACITY);
  printf("\tItems:            %zu\n", (size_t)STACK_ITEMS);
  printf("\tCycles:           %zu\n", (size_t)STACK_CYCLES);
  printf("\tBatch Size:       %zu\n", (size_t)BATCH_SIZE);
  printf("\tThreads:          %zu\n", (size_t)STACK_THREADS);
  printf("\tElement Model:    %zu-byte generic, %zu-byte pointer\n",
         sizeof(uint64_t), sizeof(void *));
  printf("\tCache Line:       %zu\n", (size_t)DV_CACHE_LINE_SIZE);

  printf("Running %zu phases...\n", nphases);

  bool all_ok = true;
  for (size_t i = 0u; i < nphases; ++i) {
    printf("[PHASE] %s\n", phases[i].name);
    const bool ok = phases[i].fn();
    report(phases[i].name, ok);
    all_ok = all_ok && ok;
  }

  if (all_ok) {
    puts("GREAT SUCCESS!");
    return EXIT_SUCCESS;
  }

  puts("I AM DIE.");
  return EXIT_FAILURE;
}
