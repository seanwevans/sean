// dv_heap_tester.c - soak harness for the heap module.
//
// Each phase drives one behaviour past the scale the unit tests sample:
// ordering over millions of elements, the O(n) build path against the O(n log
// n) one, a priority queue under a full mix of updates and cancellations, and
// the mutex wrapper under contention. Every phase reports independently and
// the run fails if any one of them does.

#include "dv_heap.h"
#include "dv_heap_mt.h"

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef HEAP_CAPACITY
#define HEAP_CAPACITY 65536u
#endif

#ifndef HEAP_ITEMS
#define HEAP_ITEMS 1000000u
#endif

#ifndef HEAP_CYCLES
#define HEAP_CYCLES 8u
#endif

#ifndef HEAP_THREADS
#define HEAP_THREADS 8u
#endif

#ifndef HEAP_SEED
#define HEAP_SEED 0x9E3779B97F4A7C15ull
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

static dv_heap_allocator_t account_hooks(account_t *a) {
  memset(a, 0, sizeof(*a));
  a->align_respected = true;

  const dv_heap_allocator_t hooks = {
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

static dv_heap_config_t u64_config(dv_heap_order_t order, size_t capacity,
                                   bool growable) {
  const dv_heap_config_t config = {
      .elem_size = sizeof(uint64_t),
      .elem_align = alignof(uint64_t),
      .initial_capacity = capacity,
      .max_capacity = 0u,
      .growable = growable,
      .cache_aligned = true,
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

// --- Phase 1: fill and drain at capacity -------------------------------

// Fills a fixed heap to the brim with unpredictable keys and drains it,
// checking the ordering on every element and the invariant on every cycle.
static bool phase_fill_drain(void) {
  dv_heap_t h;
  const dv_heap_config_t config = u64_config(DV_HEAP_MIN, HEAP_CAPACITY, false);

  if (dv_heap_init(&h, &config, NULL) != DV_HEAP_OK)
    return false;

  uint64_t rng = HEAP_SEED;
  bool ok = true;

  for (size_t cycle = 0u; cycle < HEAP_CYCLES && ok; ++cycle) {
    for (size_t i = 0u; i < HEAP_CAPACITY && ok; ++i) {
      const uint64_t value = xorshift64(&rng);
      ok = dv_heap_push(&h, &value) == DV_HEAP_OK;
    }

    const uint64_t rejected = 0u;
    ok = ok && dv_heap_is_full(&h);
    ok = ok && dv_heap_push(&h, &rejected) == DV_HEAP_ERR_FULL;
    ok = ok && dv_heap_size(&h) == HEAP_CAPACITY;
    ok = ok && dv_heap_is_valid(&h);

    uint64_t previous = 0u;
    for (size_t i = 0u; i < HEAP_CAPACITY && ok; ++i) {
      uint64_t value = 0u;
      ok = dv_heap_pop(&h, &value) == DV_HEAP_OK && value >= previous;
      previous = value;
    }

    uint64_t drained = 0u;
    ok = ok && dv_heap_is_empty(&h);
    ok = ok && dv_heap_pop(&h, &drained) == DV_HEAP_ERR_EMPTY;
  }

  dv_heap_destroy(&h);
  return ok;
}

// --- Phase 2: bottom-up build against repeated pushes ------------------

// Floyd's construction and n successive pushes must produce heaps that drain
// to the same sequence; only the cost of getting there differs.
static bool phase_build_matches_pushes(void) {
  const size_t count = HEAP_ITEMS < 500000u ? HEAP_ITEMS : 500000u;

  uint64_t *items = (uint64_t *)malloc(count * sizeof(*items));
  uint64_t *built = (uint64_t *)malloc(count * sizeof(*built));
  uint64_t *pushed = (uint64_t *)malloc(count * sizeof(*pushed));
  if (!items || !built || !pushed) {
    free(items);
    free(built);
    free(pushed);
    return false;
  }

  uint64_t rng = HEAP_SEED ^ 0xA5A5A5A5u;
  for (size_t i = 0u; i < count; ++i)
    items[i] = xorshift64(&rng) >> 8;

  dv_heap_t h;
  const dv_heap_config_t config = u64_config(DV_HEAP_MIN, 0u, true);
  bool ok = dv_heap_init(&h, &config, NULL) == DV_HEAP_OK;

  ok = ok && dv_heap_build(&h, items, count) == DV_HEAP_OK;
  ok = ok && dv_heap_is_valid(&h);
  ok = ok && dv_heap_pop_bulk(&h, built, count) == count;
  dv_heap_destroy(&h);

  ok = ok && dv_heap_init(&h, &config, NULL) == DV_HEAP_OK;
  for (size_t i = 0u; i < count && ok; ++i)
    ok = dv_heap_push(&h, &items[i]) == DV_HEAP_OK;
  ok = ok && dv_heap_is_valid(&h);
  ok = ok && dv_heap_pop_bulk(&h, pushed, count) == count;
  dv_heap_destroy(&h);

  for (size_t i = 0u; i < count && ok; ++i)
    ok = built[i] == pushed[i];

  for (size_t i = 1u; i < count && ok; ++i)
    ok = built[i] >= built[i - 1u];

  printf("\t  elements=%zu build and push agree\n", count);

  free(items);
  free(built);
  free(pushed);
  return ok;
}

// --- Phase 3: priority queue with updates and cancellation -------------

typedef struct {
  uint64_t key;
  size_t heap_index;
  size_t live_index;
  bool live;
} task_t;

static void task_on_move(void *slot, size_t index, void *user_data) {
  (void)user_data;
  task_t *task = *(task_t **)slot;
  task->heap_index = index;
}

// The full scheduling mix: submit, dispatch, reprioritize, cancel. Every
// dispatch is checked against a linear scan of the live set, so any ordering
// mistake surfaces immediately rather than as a late symptom.
static bool phase_priority_queue_mix(void) {
  account_t account;
  const dv_heap_allocator_t hooks = account_hooks(&account);

  const dv_heap_config_t config = {
      .elem_size = sizeof(void *),
      .elem_align = alignof(void *),
      .initial_capacity = 0u,
      .max_capacity = 0u,
      .growable = true,
      .cache_aligned = false,
      .order = DV_HEAP_MIN,
      .key_type = DV_HEAP_KEY_U64,
      .key_offset = offsetof(task_t, key),
      .key_indirect = true,
      .compare = NULL,
      .compare_user_data = NULL,
      .on_move = task_on_move,
      .move_user_data = NULL,
  };

  dv_heap_t h;
  if (dv_heap_init(&h, &config, &hooks) != DV_HEAP_OK)
    return false;

  // The reference is a linear scan of the live set, so the pool is kept small
  // enough that the O(n) check stays affordable under the sanitizers.
  const size_t pool_size = 512u;
  const size_t ops = HEAP_ITEMS < 200000u ? HEAP_ITEMS : 200000u;
  task_t *pool = (task_t *)calloc(pool_size, sizeof(*pool));
  task_t **live = (task_t **)malloc(pool_size * sizeof(*live));
  task_t **available = (task_t **)malloc(pool_size * sizeof(*available));
  if (!pool || !live || !available) {
    free(pool);
    free(live);
    free(available);
    dv_heap_destroy(&h);
    return false;
  }

  for (size_t i = 0u; i < pool_size; ++i)
    available[i] = &pool[i];

  size_t available_count = pool_size;
  size_t live_count = 0u;
  size_t submitted = 0u;
  size_t dispatched = 0u;
  size_t reprioritized = 0u;
  size_t cancelled = 0u;

  uint64_t rng = HEAP_SEED ^ 0x5A5A5A5Au;
  bool ok = true;

  for (size_t op = 0u; op < ops && ok; ++op) {
    const unsigned choice = (unsigned)(xorshift64(&rng) % 100u);

    if (choice < 45u && available_count > 0u) {
      task_t *task = available[--available_count];
      task->key = xorshift64(&rng) >> 24;
      task->live = true;
      task->live_index = live_count;
      live[live_count++] = task;
      ok = dv_heap_push(&h, &task) == DV_HEAP_OK;
      ++submitted;
    } else if (choice < 78u && live_count > 0u) {
      uint64_t best = live[0]->key;
      for (size_t i = 1u; i < live_count; ++i)
        if (live[i]->key < best)
          best = live[i]->key;

      task_t *task = NULL;
      ok = dv_heap_pop(&h, &task) == DV_HEAP_OK && task != NULL &&
           task->key == best;
      if (ok) {
        const size_t slot = task->live_index;
        live[slot] = live[--live_count];
        live[slot]->live_index = slot;
        task->live = false;
        available[available_count++] = task;
        ++dispatched;
      }
    } else if (choice < 92u && live_count > 0u) {
      task_t *task = live[xorshift64(&rng) % live_count];
      task->key = xorshift64(&rng) >> 24;
      ok = dv_heap_update_at(&h, task->heap_index, &task) == DV_HEAP_OK;
      ++reprioritized;
    } else if (live_count > 0u) {
      task_t *task = live[xorshift64(&rng) % live_count];
      ok = dv_heap_remove_at(&h, task->heap_index, NULL) == DV_HEAP_OK;
      if (ok) {
        const size_t slot = task->live_index;
        live[slot] = live[--live_count];
        live[slot]->live_index = slot;
        task->live = false;
        available[available_count++] = task;
        ++cancelled;
      }
    }

    ok = ok && dv_heap_size(&h) == live_count;

    // Full validation is O(n); sample it rather than pay it every operation.
    if ((op & 0x3FFu) == 0x3FFu)
      ok = ok && dv_heap_is_valid(&h);
  }

  ok = ok && dv_heap_is_valid(&h);

  // Every index the heap reported must still be where it says it is.
  for (size_t i = 0u; i < dv_heap_size(&h) && ok; ++i) {
    task_t *const *slot = (task_t *const *)dv_heap_at(&h, i);
    ok = slot != NULL && (*slot)->heap_index == i;
  }

  uint64_t previous = 0u;
  while (ok && dv_heap_size(&h) > 0u) {
    task_t *task = NULL;
    ok = dv_heap_pop(&h, &task) == DV_HEAP_OK && task->key >= previous;
    if (ok)
      previous = task->key;
  }

  printf("\t  submitted=%zu dispatched=%zu reprioritized=%zu cancelled=%zu\n",
         submitted, dispatched, reprioritized, cancelled);

  free(pool);
  free(live);
  free(available);
  dv_heap_destroy(&h);

  ok = ok && account.live_bytes == 0u;
  ok = ok && account.align_respected;
  return ok;
}

// --- Phase 4: bounded top-k over a stream ------------------------------

// A max-heap of size k holding the k smallest values seen, maintained with
// replace_top, must agree exactly with sorting the whole stream.
static bool phase_top_k_stream(void) {
  const size_t k = 64u;
  const size_t count = HEAP_ITEMS < 200000u ? HEAP_ITEMS : 200000u;

  uint64_t *stream = (uint64_t *)malloc(count * sizeof(*stream));
  if (!stream)
    return false;

  uint64_t rng = HEAP_SEED ^ 0x3C3C3C3Cu;
  for (size_t i = 0u; i < count; ++i)
    stream[i] = xorshift64(&rng) >> 12;

  dv_heap_t h;
  const dv_heap_config_t config = u64_config(DV_HEAP_MAX, k, false);
  bool ok = dv_heap_init(&h, &config, NULL) == DV_HEAP_OK;

  for (size_t i = 0u; i < count && ok; ++i) {
    if (dv_heap_size(&h) < k) {
      ok = dv_heap_push(&h, &stream[i]) == DV_HEAP_OK;
      continue;
    }

    const uint64_t *top = (const uint64_t *)dv_heap_top(&h);
    if (top && stream[i] < *top)
      ok = dv_heap_replace_top(&h, &stream[i], NULL) == DV_HEAP_OK;
  }

  ok = ok && dv_heap_is_valid(&h);
  ok = ok && dv_heap_size(&h) == k;

  // Selecting the k smallest by hand, without the heap.
  uint64_t *smallest = (uint64_t *)malloc(k * sizeof(*smallest));
  if (!smallest) {
    free(stream);
    dv_heap_destroy(&h);
    return false;
  }

  for (size_t i = 0u; i < k; ++i) {
    size_t best = 0u;
    for (size_t j = 1u; j < count; ++j)
      if (stream[j] < stream[best])
        best = j;
    smallest[i] = stream[best];
    stream[best] = UINT64_MAX;
  }

  for (size_t i = k; i-- > 0u && ok;) {
    uint64_t value = 0u;
    ok = dv_heap_pop(&h, &value) == DV_HEAP_OK && value == smallest[i];
  }

  printf("\t  stream=%zu k=%zu\n", count, k);

  free(stream);
  free(smallest);
  dv_heap_destroy(&h);
  return ok;
}

// --- Phase 5: growth and shrink under live elements --------------------

static bool phase_grow_shrink_cycles(void) {
  account_t account;
  const dv_heap_allocator_t hooks = account_hooks(&account);

  dv_heap_t h;
  const dv_heap_config_t config = u64_config(DV_HEAP_MIN, 0u, true);
  if (dv_heap_init(&h, &config, &hooks) != DV_HEAP_OK)
    return false;

  const size_t high_water = 100000u;
  uint64_t rng = HEAP_SEED ^ 0x0F0F0F0Fu;
  bool ok = true;

  for (size_t cycle = 0u; cycle < 4u && ok; ++cycle) {
    for (size_t i = 0u; i < high_water && ok; ++i) {
      const uint64_t value = xorshift64(&rng) >> 8;
      ok = dv_heap_push(&h, &value) == DV_HEAP_OK;
    }

    ok = ok && dv_heap_size(&h) == high_water;
    ok = ok && dv_heap_shrink_to_fit(&h) == DV_HEAP_OK;
    ok = ok && dv_heap_capacity(&h) == high_water;
    ok = ok && dv_heap_is_valid(&h);

    uint64_t previous = 0u;
    for (size_t i = 0u; i < high_water && ok; ++i) {
      uint64_t value = 0u;
      ok = dv_heap_pop(&h, &value) == DV_HEAP_OK && value >= previous;
      previous = value;
    }

    ok = ok && dv_heap_reset(&h) == DV_HEAP_OK;
    ok = ok && dv_heap_capacity(&h) == 0u;
  }

  dv_heap_destroy(&h);

  ok = ok && account.align_respected;
  ok = ok && account.live_bytes == 0u;

  printf("\t  allocs=%zu reallocs=%zu frees=%zu peak_bytes=%zu\n",
         account.alloc_calls, account.realloc_calls, account.free_calls,
         account.peak_bytes);

  return ok;
}

// --- Phase 6: mutex wrapper under contention ---------------------------

typedef struct {
  dv_heap_mt_t *heap;
  size_t thread_id;
  size_t items;
  uint64_t sum;
  uint64_t xor_value;
  size_t count;
} mt_thread_args_t;

static void *mt_producer(void *arg) {
  mt_thread_args_t *a = (mt_thread_args_t *)arg;
  uint64_t rng = HEAP_SEED + (uint64_t)a->thread_id + 1u;

  for (size_t i = 0u; i < a->items; ++i) {
    const uint64_t value = xorshift64(&rng) >> 8;
    if (dv_heap_mt_push(a->heap, &value) != DV_HEAP_OK)
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
    if (dv_heap_mt_pop(a->heap, &out) != DV_HEAP_OK)
      continue;

    a->sum += out;
    a->xor_value ^= out;
    ++a->count;
  }

  return NULL;
}

static bool phase_mt_contention(void) {
  if ((HEAP_THREADS % 2u) != 0u || HEAP_THREADS < 2u) {
    fprintf(stderr, "HEAP_THREADS must be an even number >= 2.\n");
    return false;
  }

  const size_t sides = (size_t)HEAP_THREADS / 2u;
  const size_t items = 50000u;

  dv_heap_mt_t heap;
  const dv_heap_config_t config = u64_config(DV_HEAP_MIN, 1024u, true);
  if (dv_heap_mt_init(&heap, &config, NULL) != DV_HEAP_OK)
    return false;

  pthread_t *threads = (pthread_t *)malloc(2u * sides * sizeof(*threads));
  mt_thread_args_t *args =
      (mt_thread_args_t *)calloc(2u * sides, sizeof(*args));

  if (!threads || !args) {
    free(threads);
    free(args);
    dv_heap_mt_destroy(&heap);
    return false;
  }

  for (size_t i = 0u; i < 2u * sides; ++i) {
    args[i].heap = &heap;
    args[i].thread_id = i;
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
                  produced_xor == consumed_xor && dv_heap_mt_size(&heap) == 0u;

  printf("\t  threads=%zu items=%zu sum=%" PRIu64 " xor=%" PRIu64 "\n",
         2u * sides, consumed, consumed_sum, consumed_xor);

  free(threads);
  free(args);
  dv_heap_mt_destroy(&heap);
  return ok;
}

// --- Driver ------------------------------------------------------------

typedef struct {
  const char *name;
  bool (*fn)(void);
} phase_t;

int main(void) {
  const phase_t phases[] = {
      {"fill and drain", phase_fill_drain},
      {"build matches pushes", phase_build_matches_pushes},
      {"priority queue mix", phase_priority_queue_mix},
      {"top k stream", phase_top_k_stream},
      {"grow and shrink cycles", phase_grow_shrink_cycles},
      {"mutex wrapper contention", phase_mt_contention},
  };

  const size_t nphases = sizeof(phases) / sizeof(phases[0]);

  printf("Initializing\n");
  printf("\tHeap Capacity:    %zu\n", (size_t)HEAP_CAPACITY);
  printf("\tItems:            %zu\n", (size_t)HEAP_ITEMS);
  printf("\tCycles:           %zu\n", (size_t)HEAP_CYCLES);
  printf("\tBatch Size:       %zu\n", (size_t)BATCH_SIZE);
  printf("\tThreads:          %zu\n", (size_t)HEAP_THREADS);
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
